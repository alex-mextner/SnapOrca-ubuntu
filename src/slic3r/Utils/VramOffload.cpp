#include "VramOffload.hpp"

#ifdef __linux__

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

extern char** environ;

namespace Slic3r { namespace VramOffload {

namespace fs = std::filesystem;
using json   = nlohmann::json;
using Clock  = std::chrono::steady_clock;

// ---------------------------------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------------------------------

fs::path runtime_dir()
{
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return (dir != nullptr && dir[0] == '/') ? fs::path(dir) : fs::path();
}

fs::path socket_dir()
{
    const fs::path runtime = runtime_dir();
    return runtime.empty() ? fs::path() : runtime / "vram-offload";
}

fs::path state_dir()
{
    const fs::path runtime = runtime_dir();
    return runtime.empty() ? fs::path() : runtime / "snapmaker-orca";
}

fs::path state_file_path(const fs::path& dir, pid_t orca_pid)
{
    return dir / ("vram-offload-" + std::to_string(orca_pid) + ".json");
}

// ---------------------------------------------------------------------------------------------------------------------
// Process identity
// ---------------------------------------------------------------------------------------------------------------------

std::optional<unsigned long long> parse_stat_start_ticks(const std::string& stat_line)
{
    // "pid (comm) state ppid ...": comm may hold spaces and parentheses, so the fields start after the last ')'.
    const std::size_t close = stat_line.rfind(')');
    if (close == std::string::npos)
        return std::nullopt;
    std::istringstream rest(stat_line.substr(close + 1));
    std::string        field;
    // Field 3 (state) is the first one after the command, so field 22 (starttime) is the 20th.
    for (int index = 3; index <= 22; ++index) {
        if (!(rest >> field))
            return std::nullopt;
        if (index == 22) {
            char*              end   = nullptr;
            errno                    = 0;
            const unsigned long long value = std::strtoull(field.c_str(), &end, 10);
            if (errno != 0 || end == field.c_str() || *end != '\0')
                return std::nullopt;
            return value;
        }
    }
    return std::nullopt;
}

std::optional<unsigned long long> process_start_ticks(pid_t pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat)
        return std::nullopt;
    std::string line;
    std::getline(stat, line);
    return parse_stat_start_ticks(line);
}

bool process_alive(pid_t pid, unsigned long long start_ticks)
{
    const auto current = process_start_ticks(pid);
    return current.has_value() && *current == start_ticks;
}

// ---------------------------------------------------------------------------------------------------------------------
// Control protocol client
// ---------------------------------------------------------------------------------------------------------------------

namespace {

int ms_until(Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(std::min<long long>(left, 1000 * 60)) : 0;
}

// Waits for `events` on `fd`. 1 when ready, 0 on timeout, -1 on error.
int wait_fd(int fd, short events, Clock::time_point deadline)
{
    for (;;) {
        pollfd p{fd, events, 0};
        const int rc = ::poll(&p, 1, ms_until(deadline));
        if (rc > 0)
            return 1;
        if (rc == 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

// One request line out, one reply line in. nullopt with `error` set when the exchange did not complete.
std::optional<std::string> exchange(const fs::path& socket, const std::string& request, std::chrono::milliseconds timeout, std::string& error)
{
    const auto deadline = Clock::now() + timeout;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = socket.string();
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        error = "invalid socket path";
        return std::nullopt;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return std::nullopt;
    }
    struct Closer
    {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};

    // A non-blocking connect to a Unix socket either succeeds at once or fails with EAGAIN while the listen backlog is full.
    for (;;) {
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        error = std::string("connect: ") + std::strerror(errno);
        return std::nullopt;
    }

    std::string line = request;
    line.push_back('\n');
    std::size_t sent = 0;
    while (sent < line.size()) {
        const ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_fd(fd, POLLOUT, deadline) == 1)
                continue;
            error = "timed out sending the request";
            return std::nullopt;
        }
        error = std::string("send: ") + std::strerror(errno);
        return std::nullopt;
    }

    std::string reply;
    for (;;) {
        char          buffer[4096];
        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            reply.append(buffer, static_cast<std::size_t>(n));
            const std::size_t eol = reply.find('\n');
            if (eol != std::string::npos)
                return reply.substr(0, eol);
            if (reply.size() > (1u << 20)) {
                error = "reply too long";
                return std::nullopt;
            }
            continue;
        }
        if (n == 0) {
            // The daemon closed without a trailing newline: a complete line all the same.
            if (!reply.empty())
                return reply;
            error = "connection closed without a reply";
            return std::nullopt;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            const int ready = wait_fd(fd, POLLIN, deadline);
            if (ready == 1)
                continue;
            error = ready == 0 ? "no reply in time" : std::string("poll: ") + std::strerror(errno);
            return std::nullopt;
        }
        error = std::string("recv: ") + std::strerror(errno);
        return std::nullopt;
    }
}

Reply transport_failure(const std::string& error)
{
    Reply reply;
    reply.failure = Failure::Unreachable;
    reply.error   = error;
    return reply;
}

template<class T> T number_or(const json& object, const char* key, T fallback)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<T>() : fallback;
}

std::string string_or(const json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

} // namespace

Reply parse_reply(const std::string& line)
{
    const json value = json::parse(line, nullptr, false);
    Reply      reply;
    if (!value.is_object() || !value.contains("ok") || !value["ok"].is_boolean()) {
        reply.failure = Failure::Protocol;
        reply.error   = "malformed reply";
        return reply;
    }
    reply.ok = value["ok"].get<bool>();
    if (reply.ok) {
        reply.seconds     = number_or<double>(value, "seconds", 0.);
        reply.freed_bytes = number_or<std::uint64_t>(value, "freed_bytes", 0);
        return reply;
    }
    reply.error = string_or(value, "error");
    if (reply.error == "busy")
        reply.failure = Failure::Busy;
    else if (reply.error == "timeout")
        reply.failure = Failure::Timeout;
    else if (reply.error == "oom")
        reply.failure = Failure::Oom;
    else
        reply.failure = Failure::Other;
    return reply;
}

std::optional<DaemonStatus> parse_status(const std::string& line)
{
    const json value = json::parse(line, nullptr, false);
    if (!value.is_object() || !value.contains("ok") || !value["ok"].is_boolean() || !value["ok"].get<bool>())
        return std::nullopt;
    DaemonStatus status;
    status.name          = string_or(value, "name");
    status.pid           = number_or<long long>(value, "pid", 0);
    status.state         = string_or(value, "state");
    status.weights_bytes = number_or<std::uint64_t>(value, "weights_bytes", 0);
    status.idle_s        = number_or<double>(value, "idle_s", 0.);
    if (status.pid <= 0 || status.state.empty())
        return std::nullopt;
    const auto leases = value.find("leases");
    if (leases != value.end() && leases->is_array()) {
        for (const json& entry : *leases) {
            if (!entry.is_object())
                continue;
            LeaseInfo lease;
            lease.id           = string_or(entry, "id");
            lease.holder_pid   = number_or<long long>(entry, "holder_pid", 0);
            lease.holder_start = number_or<unsigned long long>(entry, "holder_start", 0);
            status.leases.push_back(std::move(lease));
        }
    }
    const std::pair<const char*, std::optional<double>*> optional_numbers[] = {{"last_offload_s", &status.last_offload_s},
                                                                             {"last_restore_s", &status.last_restore_s},
                                                                             {"last_first_answer_s", &status.last_first_answer_s}};
    for (const auto& [key, target] : optional_numbers) {
        const auto it = value.find(key);
        if (it != value.end() && it->is_number())
            *target = it->get<double>();
    }
    return status;
}

std::optional<DaemonStatus> request_status(const fs::path& socket, std::chrono::milliseconds timeout, std::string* error)
{
    std::string why;
    const auto  line = exchange(socket, json{{"op", "status"}}.dump(), timeout, why);
    if (!line) {
        if (error != nullptr)
            *error = why;
        return std::nullopt;
    }
    auto status = parse_status(*line);
    if (!status && error != nullptr)
        *error = "malformed status reply";
    return status;
}

Reply request_offload(const fs::path& socket, const std::string& lease, pid_t holder_pid, unsigned long long holder_start,
                      std::chrono::milliseconds timeout)
{
    std::string why;
    const auto  line = exchange(socket,
                                json{{"op", "offload"}, {"lease", lease}, {"holder_pid", static_cast<long long>(holder_pid)}, {"holder_start", holder_start}}.dump(),
                                timeout, why);
    return line ? parse_reply(*line) : transport_failure(why);
}

Reply request_restore(const fs::path& socket, const std::string& lease, std::chrono::milliseconds timeout)
{
    std::string why;
    const auto  line = exchange(socket, json{{"op", "restore"}, {"lease", lease}}.dump(), timeout, why);
    return line ? parse_reply(*line) : transport_failure(why);
}

// ---------------------------------------------------------------------------------------------------------------------
// NVML probe
// ---------------------------------------------------------------------------------------------------------------------

namespace {

// The NVML ABI used here (nvml.h): the structs are laid out by hand so that no NVIDIA header is needed to build.
using NvmlReturn = int;
using NvmlDevice = void*;
struct NvmlMemory
{
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};
struct NvmlProcessInfo
{
    unsigned int       pid;
    unsigned long long usedGpuMemory;
    unsigned int       gpuInstanceId;
    unsigned int       computeInstanceId;
};
constexpr NvmlReturn        NVML_SUCCESS                 = 0;
constexpr NvmlReturn        NVML_ERROR_INSUFFICIENT_SIZE = 7;
constexpr unsigned long long NVML_VALUE_NOT_AVAILABLE    = ~0ULL;

template<class Fn> bool load_symbol(void* library, const char* name, Fn& out)
{
    out = reinterpret_cast<Fn>(::dlsym(library, name));
    return out != nullptr;
}

} // namespace

std::optional<std::size_t> choose_render_gpu(const std::vector<GpuSnapshot>& gpus, unsigned int self_pid)
{
    if (gpus.empty())
        return std::nullopt;
    std::vector<std::size_t> graphics_matches, compute_matches;
    if (self_pid != 0) {
        for (std::size_t i = 0; i < gpus.size(); ++i) {
            const GpuSnapshot& gpu = gpus[i];
            if (std::find(gpu.graphics_pids.begin(), gpu.graphics_pids.end(), self_pid) != gpu.graphics_pids.end())
                graphics_matches.push_back(i);
            if (std::any_of(gpu.processes.begin(), gpu.processes.end(), [&](const GpuProcess& p) { return p.pid == self_pid; }))
                compute_matches.push_back(i);
        }
    }
    // A graphics context is the render path itself; a compute context only a fallback. At either tier exactly one GPU may
    // match: with several, the order of enumeration would decide and the wrong GPU's free memory would be judged.
    if (!graphics_matches.empty())
        return graphics_matches.size() == 1 ? std::optional<std::size_t>(graphics_matches.front()) : std::nullopt;
    if (!compute_matches.empty())
        return compute_matches.size() == 1 ? std::optional<std::size_t>(compute_matches.front()) : std::nullopt;
    if (gpus.size() == 1)
        return 0;
    return std::nullopt;
}

std::optional<GpuSnapshot> probe_gpu(const char* library_name, std::string* why, unsigned int self_pid)
{
    auto fail = [why](const std::string& reason) -> std::optional<GpuSnapshot> {
        if (why != nullptr)
            *why = reason;
        return std::nullopt;
    };

    // RTLD_NODELETE and no dlclose: closing NVML after init crashes in cuda-checkpoint issue #58.
    void* library = ::dlopen(library_name, RTLD_NOW | RTLD_NODELETE);
    if (library == nullptr)
        return fail(std::string("cannot load ") + library_name);

    using ProcessList = NvmlReturn (*)(NvmlDevice, unsigned int*, NvmlProcessInfo*);
    NvmlReturn (*init)()                                     = nullptr;
    NvmlReturn (*shutdown)()                                 = nullptr;
    NvmlReturn (*device_count)(unsigned int*)                = nullptr;
    NvmlReturn (*handle_by_index)(unsigned int, NvmlDevice*) = nullptr;
    NvmlReturn (*memory_info)(NvmlDevice, NvmlMemory*)       = nullptr;
    ProcessList compute_processes                            = nullptr;
    ProcessList graphics_processes                           = nullptr;
    if (!load_symbol(library, "nvmlInit_v2", init) || !load_symbol(library, "nvmlShutdown", shutdown) ||
        !load_symbol(library, "nvmlDeviceGetCount_v2", device_count) ||
        !load_symbol(library, "nvmlDeviceGetHandleByIndex_v2", handle_by_index) ||
        !load_symbol(library, "nvmlDeviceGetMemoryInfo", memory_info) ||
        !load_symbol(library, "nvmlDeviceGetComputeRunningProcesses_v3", compute_processes))
        return fail(std::string(library_name) + " lacks a required symbol");
    // Optional (NVML of driver 510+): without it a single GPU is still identified, several are not.
    load_symbol(library, "nvmlDeviceGetGraphicsRunningProcesses_v3", graphics_processes);

    if (init() != NVML_SUCCESS)
        return fail("nvmlInit_v2 failed");

    // The process list can grow between the size query and the fetch: retry with the size NVML reports.
    const auto list_processes = [](ProcessList fn, NvmlDevice device, std::vector<NvmlProcessInfo>& infos) {
        unsigned int count = 0;
        infos.clear();
        for (int attempt = 0; attempt < 4; ++attempt) {
            const NvmlReturn rc = fn(device, &count, infos.empty() ? nullptr : infos.data());
            if (rc == NVML_SUCCESS) {
                infos.resize(count);
                return true;
            }
            if (rc != NVML_ERROR_INSUFFICIENT_SIZE)
                break;
            infos.assign(static_cast<std::size_t>(count) + 8, NvmlProcessInfo{});
            count = static_cast<unsigned int>(infos.size());
        }
        return false;
    };

    std::vector<GpuSnapshot> gpus;
    std::string              reason = "NVML query failed";
    unsigned int             count  = 0;
    if (device_count(&count) == NVML_SUCCESS) {
        bool ok = true;
        for (unsigned int index = 0; index < count && ok; ++index) {
            NvmlDevice                   device = nullptr;
            NvmlMemory                   memory{};
            std::vector<NvmlProcessInfo> infos;
            if (handle_by_index(index, &device) != NVML_SUCCESS || memory_info(device, &memory) != NVML_SUCCESS) {
                ok = false;
                break;
            }
            GpuSnapshot gpu;
            gpu.index       = index;
            gpu.total_bytes = memory.total;
            gpu.free_bytes  = memory.free;
            if (!list_processes(compute_processes, device, infos)) {
                reason = "nvmlDeviceGetComputeRunningProcesses_v3 failed";
                ok     = false;
                break;
            }
            for (const NvmlProcessInfo& info : infos) {
                GpuProcess process;
                process.pid = info.pid;
                if (info.usedGpuMemory != NVML_VALUE_NOT_AVAILABLE)
                    process.used_bytes = info.usedGpuMemory;
                gpu.processes.push_back(process);
            }
            // A failing graphics list only means this GPU cannot be matched by it.
            if (graphics_processes != nullptr && list_processes(graphics_processes, device, infos))
                for (const NvmlProcessInfo& info : infos)
                    gpu.graphics_pids.push_back(info.pid);
            gpus.push_back(std::move(gpu));
        }
        if (!ok)
            gpus.clear();
    }
    shutdown();

    if (gpus.empty())
        return fail(reason);
    const auto chosen = choose_render_gpu(gpus, self_pid);
    if (!chosen)
        return fail("several GPUs and none lists this process as a graphics client: cannot tell which one draws the preview");
    return gpus[*chosen];
}

// ---------------------------------------------------------------------------------------------------------------------
// Consumers
// ---------------------------------------------------------------------------------------------------------------------

bool has_control_sockets(const fs::path& dir)
{
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return false;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".sock")
            return true;
    return false;
}

std::vector<Consumer> find_consumers(const fs::path& dir, const GpuSnapshot& gpu, std::chrono::milliseconds status_timeout)
{
    std::vector<Consumer> consumers;
    std::error_code       ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return consumers;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path socket = it->path();
        if (socket.extension() != ".sock")
            continue;
        const auto status = request_status(socket, status_timeout);
        // A daemon that is not loaded (idle-unloaded, or pinned by another holder) has nothing to give back.
        if (!status || status->state != "loaded")
            continue;
        const auto process = std::find_if(gpu.processes.begin(), gpu.processes.end(),
                                          [&](const GpuProcess& p) { return static_cast<long long>(p.pid) == status->pid; });
        if (process == gpu.processes.end())
            continue;
        Consumer consumer;
        consumer.name           = status->name.empty() ? socket.stem().string() : status->name;
        consumer.socket         = socket;
        consumer.pid            = status->pid;
        consumer.vram_bytes     = process->used_bytes.value_or(status->weights_bytes);
        consumer.last_restore_s = status->last_restore_s;
        consumers.push_back(std::move(consumer));
    }
    std::sort(consumers.begin(), consumers.end(), [](const Consumer& a, const Consumer& b) {
        return a.vram_bytes != b.vram_bytes ? a.vram_bytes > b.vram_bytes : a.name < b.name;
    });
    return consumers;
}

// ---------------------------------------------------------------------------------------------------------------------
// Restorer state file
// ---------------------------------------------------------------------------------------------------------------------

bool write_state_file(const fs::path& file, const RestoreState& state, std::string* error)
{
    auto fail = [error](const std::string& why) {
        if (error != nullptr)
            *error = why;
        return false;
    };

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec)
        return fail("cannot create " + file.parent_path().string() + ": " + ec.message());
    ::chmod(file.parent_path().c_str(), 0700);

    json items = json::array();
    for (const RestoreItem& item : state.items)
        items.push_back({{"name", item.name}, {"socket", item.socket.string()}, {"lease", item.lease}});
    const std::string text = json{{"orca_pid", state.orca_pid}, {"orca_start", state.orca_start}, {"items", items}}.dump();

    const fs::path tmp = file.string() + ".tmp";
    const int      fd  = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return fail("cannot open " + tmp.string() + ": " + std::strerror(errno));
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            const std::string why = std::strerror(errno);
            ::close(fd);
            fs::remove(tmp, ec);
            return fail("cannot write " + tmp.string() + ": " + why);
        }
    }
    ::fsync(fd);
    ::close(fd);
    if (::rename(tmp.c_str(), file.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        fs::remove(tmp, ec);
        return fail("cannot rename " + tmp.string() + ": " + why);
    }
    return true;
}

std::optional<RestoreState> read_state_file(const fs::path& file)
{
    std::ifstream stream(file);
    if (!stream)
        return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const json        value = json::parse(text, nullptr, false);
    if (!value.is_object() || !value.contains("orca_pid") || !value["orca_pid"].is_number() || !value.contains("orca_start") ||
        !value["orca_start"].is_number() || !value.contains("items") || !value["items"].is_array())
        return std::nullopt;
    RestoreState state;
    state.orca_pid   = value["orca_pid"].get<long long>();
    state.orca_start = value["orca_start"].get<unsigned long long>();
    for (const json& entry : value["items"]) {
        if (!entry.is_object())
            continue;
        RestoreItem item;
        item.name   = string_or(entry, "name");
        item.socket = string_or(entry, "socket");
        item.lease  = string_or(entry, "lease");
        if (!item.socket.empty() && !item.lease.empty())
            state.items.push_back(std::move(item));
    }
    return state;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lease holder
// ---------------------------------------------------------------------------------------------------------------------

std::shared_ptr<Coordinator> make_default_coordinator()
{
    const pid_t pid = ::getpid();
    Coordinator::Config config;
    config.pid             = pid;
    config.start           = process_start_ticks(pid).value_or(0);
    config.state_file      = state_file_path(state_dir(), pid);
    config.ensure_restorer = [pid](const fs::path& state_file) { return ensure_restorer_unit(pid, state_file); };
    return std::make_shared<Coordinator>(std::move(config));
}

std::string new_lease_id()
{
    std::random_device                        random;
    std::array<unsigned char, 16>             bytes{};
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = random();
        for (std::size_t k = 0; k < 4; ++k)
            bytes[i + k] = static_cast<unsigned char>(word >> (8 * k));
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    char text[37];
    std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2],
                  bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                  bytes[14], bytes[15]);
    return text;
}

namespace {

// The daemon no longer holds the lease: it confirmed, or an OOM while reloading left the lease gone (the daemon stays
// unloaded and reloads on its next request).
bool lease_is_gone(const Reply& reply) { return reply.ok || reply.failure == Failure::Oom; }

} // namespace

Coordinator::Coordinator(Config config) : m_config(std::move(config)) {}

std::size_t Coordinator::held_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_held.size();
}

bool Coordinator::persist_locked(std::string* error)
{
    RestoreState state;
    state.orca_pid   = m_config.pid;
    state.orca_start = m_config.start;
    state.items      = m_held;
    return write_state_file(m_config.state_file, state, error);
}

std::vector<OffloadResult> Coordinator::offload(const std::vector<Consumer>& consumers)
{
    std::lock_guard<std::timed_mutex> op(m_op_mutex);

    std::vector<OffloadResult> results(consumers.size());
    for (std::size_t i = 0; i < consumers.size(); ++i)
        results[i].consumer = consumers[i];
    if (consumers.empty())
        return results;

    auto fail_all = [&](const std::string& why) {
        for (OffloadResult& result : results) {
            result.reply.failure = Failure::Other;
            result.reply.error   = why;
        }
        return results;
    };

    // 1. The intent goes to the state file before any `offload` is sent: whatever happens next, the restorer knows
    //    every lease the daemons may hold.
    std::vector<RestoreItem> intent;
    for (const Consumer& consumer : consumers)
        intent.push_back({consumer.name, consumer.socket, new_lease_id()});
    const auto drop_intent = [&] {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const RestoreItem& item : intent)
            m_held.erase(std::remove(m_held.begin(), m_held.end(), item), m_held.end());
        std::string ignored;
        persist_locked(&ignored);
    };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_held.insert(m_held.end(), intent.begin(), intent.end());
        std::string why;
        if (!persist_locked(&why)) {
            for (const RestoreItem& item : intent)
                m_held.erase(std::remove(m_held.begin(), m_held.end(), item), m_held.end());
            return fail_all(why);
        }
    }

    // 2. The restorer must run before the first lease exists.
    if (!m_config.ensure_restorer || !m_config.ensure_restorer(m_config.state_file)) {
        drop_intent();
        return fail_all("the crash-safe restorer could not be started");
    }

    // 3. One `offload` per consumer. A daemon that does not confirm rolled itself back; a `restore` of the lease covers
    //    a reply that was lost on the way (restore of an unknown lease is ok). Only a confirmed restore ends the lease:
    //    when even that gets no answer the daemon may still hold it, so it stays held (and in the state file) for the
    //    next release and for the restorer.
    for (std::size_t i = 0; i < consumers.size(); ++i) {
        const Consumer&    consumer = consumers[i];
        const RestoreItem& item     = intent[i];
        Reply reply = request_offload(consumer.socket, item.lease, m_config.pid, m_config.start, m_config.timeouts.offload);
        if (reply.ok) {
            results[i].leased = true;
        } else if (lease_is_gone(request_restore(consumer.socket, item.lease, m_config.timeouts.restore))) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_held.erase(std::remove(m_held.begin(), m_held.end(), item), m_held.end());
        } else {
            results[i].uncertain = true;
        }
        results[i].reply = std::move(reply);
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string ignored;
        persist_locked(&ignored);
    }
    return results;
}

std::vector<ReleaseResult> Coordinator::release_with(std::chrono::milliseconds per_item)
{
    std::vector<RestoreItem> held;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        held = m_held;
    }
    std::vector<ReleaseResult> results;
    for (const RestoreItem& item : held) {
        ReleaseResult result;
        result.item     = item;
        result.reply    = request_restore(item.socket, item.lease, per_item);
        result.released = lease_is_gone(result.reply);
        if (result.released) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_held.erase(std::remove(m_held.begin(), m_held.end(), item), m_held.end());
            std::string ignored;
            persist_locked(&ignored);
        }
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<ReleaseResult> Coordinator::release_all()
{
    std::lock_guard<std::timed_mutex> op(m_op_mutex);
    return release_with(m_config.timeouts.restore);
}

std::vector<ReleaseResult> Coordinator::close(std::chrono::milliseconds per_item, std::chrono::milliseconds lock_wait)
{
    // An offload or release that is still waiting for a daemon holds the lock for up to 30 s: do not hold the exit up
    // for it. Its lease is already in the state file, which the restorer works through after this process is gone.
    std::unique_lock<std::timed_mutex> op(m_op_mutex, std::defer_lock);
    if (!op.try_lock_for(lock_wait))
        return {};
    std::vector<ReleaseResult>  results = release_with(per_item);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_held.empty()) {
        std::error_code ec;
        fs::remove(m_config.state_file, ec);
    }
    return results;
}

// ---------------------------------------------------------------------------------------------------------------------
// Restorer
// ---------------------------------------------------------------------------------------------------------------------

std::vector<RestoreItem> restore_items(const RestoreState& state, const Timeouts& timeouts, int attempts, std::chrono::milliseconds retry_delay)
{
    std::vector<RestoreItem> left = state.items;
    for (int attempt = 0; attempt < attempts && !left.empty(); ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(retry_delay);
        std::vector<RestoreItem> unconfirmed;
        for (const RestoreItem& item : left)
            if (!lease_is_gone(request_restore(item.socket, item.lease, timeouts.restore)))
                unconfirmed.push_back(item);
        left = std::move(unconfirmed);
    }
    return left;
}

void wait_for_process_exit(pid_t orca_pid, unsigned long long orca_start)
{
    if (!process_alive(orca_pid, orca_start))
        return;
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, orca_pid, 0));
    if (pidfd >= 0) {
        // The pid may have been reused between the check and the open: the fd would then follow a stranger.
        if (process_alive(orca_pid, orca_start)) {
            for (;;) {
                pollfd p{pidfd, POLLIN, 0};
                if (::poll(&p, 1, -1) > 0 || errno != EINTR)
                    break;
            }
        }
        ::close(pidfd);
        return;
    }
    // No pidfd (kernel older than 5.3, or a seccomp filter): poll the start time.
    while (process_alive(orca_pid, orca_start))
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

int run_restore_watch(const fs::path& state_file)
{
    const auto initial = read_state_file(state_file);
    if (!initial)
        return 0;
    std::error_code ec;
    const fs::path  ready = restorer_ready_file(state_file);
    // Handshake with ensure_restorer_unit(): the state file is read, the wait for the holder begins.
    {
        std::ofstream marker(ready);
        marker << ::getpid() << '\n';
    }
    wait_for_process_exit(static_cast<pid_t>(initial->orca_pid), initial->orca_start);

    // Orca rewrote the file while it ran: what it holds now is what is still leased.
    const auto current = read_state_file(state_file);
    if (current && !current->items.empty()) {
        RestoreState left = *current;
        left.items        = restore_items(*current, Timeouts{});
        if (!left.items.empty()) {
            // Unconfirmed leases stay on disk: the next Orca start restores them (restore_orphans()); the daemons drop
            // a dead holder's lease on their own within 30 s as well.
            write_state_file(state_file, left);
            fs::remove(ready, ec);
            return 1;
        }
    }
    fs::remove(state_file, ec);
    fs::remove(ready, ec);
    return 0;
}

int restore_orphans(const fs::path& dir)
{
    int             processed = 0;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return 0;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("vram-offload-", 0) == 0 && it->path().extension() == ".json")
            files.push_back(it->path());
    }
    for (const fs::path& file : files) {
        const auto state = read_state_file(file);
        if (!state)
            continue;
        if (process_alive(static_cast<pid_t>(state->orca_pid), state->orca_start))
            continue;
        // A lease the daemons did not confirm stays in the file: the next Orca start tries again.
        RestoreState left = *state;
        left.items        = restore_items(*state, Timeouts{});
        if (left.items.empty())
            fs::remove(file, ec);
        else
            write_state_file(file, left);
        ++processed;
    }
    return processed;
}

namespace {

// Runs a command with its output discarded. -1 when it cannot be started or does not finish in `timeout`.
int run_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout)
{
    std::vector<char*> argv;
    for (const std::string& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    // The commands are the system's systemd-run and systemctl: Orca's bundled libraries, which the AppImage's AppRun puts on
    // LD_LIBRARY_PATH, must not be picked up by them (a mismatching libsystemd makes systemd-run die on a symbol lookup).
    std::vector<char*> envp;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
        if (std::strncmp(*entry, "LD_LIBRARY_PATH=", 16) != 0)
            envp.push_back(*entry);
    envp.push_back(nullptr);
    pid_t      child = 0;
    const int  rc    = ::posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0)
        return -1;

    const auto deadline = Clock::now() + timeout;
    for (;;) {
        int       status = 0;
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (done < 0 && errno != EINTR)
            return -1;
        if (Clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool on_path(const char* program)
{
    const char* path = std::getenv("PATH");
    if (path == nullptr)
        return false;
    std::istringstream stream(path);
    std::string        dir;
    while (std::getline(stream, dir, ':')) {
        if (dir.empty())
            continue;
        const std::string candidate = dir + "/" + program;
        if (::access(candidate.c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

} // namespace

bool restorer_available()
{
    return !runtime_dir().empty() && on_path("systemd-run") && on_path("systemctl");
}

bool ensure_restorer_unit(pid_t orca_pid, const fs::path& state_file)
{
    const std::string unit  = "orca-vram-restore-" + std::to_string(orca_pid);
    const fs::path    ready = restorer_ready_file(state_file);
    // Already running (a second launch under the same unit name would fail): it still has to have reported ready; one
    // that is stuck in its start or could not write the marker does not count.
    if (run_command({"systemctl", "--user", "is-active", "--quiet", unit + ".service"}, std::chrono::seconds(5)) == 0)
        return wait_for_restorer_ready(ready, std::chrono::seconds(5));

    std::error_code ec;
    const fs::path  exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty())
        return false;

    // A stale marker of an earlier watcher would pass for this one.
    fs::remove(ready, ec);

    // Type=exec: systemd-run returns only once the helper has actually been exec'd (a missing or unreadable binary fails
    // here instead of leaving offloaded models without a restorer).
    // MemoryMax: the watcher is the whole Orca binary (statically linked, ~80 MB resident, mostly file-backed pages), so
    // 256M leaves a wide margin; the limit only stops a runaway helper.
    std::vector<std::string> args{"systemd-run", "--user", "--collect", "--quiet", "--unit=" + unit, "--property=Type=exec", "--property=MemoryMax=256M"};
    // The user manager's environment is not Orca's: the AppImage's AppRun puts its bundled libraries on LD_LIBRARY_PATH.
    if (const char* libraries = std::getenv("LD_LIBRARY_PATH"); libraries != nullptr && libraries[0] != '\0')
        args.push_back(std::string("--setenv=LD_LIBRARY_PATH=") + libraries);
    args.push_back(exe.string());
    args.push_back("--vram-restore-watch");
    args.push_back(state_file.string());
    if (run_command(args, std::chrono::seconds(10)) != 0)
        return false;
    return wait_for_restorer_ready(ready, std::chrono::seconds(5));
}

bool wait_for_restorer_ready(const fs::path& ready, std::chrono::milliseconds timeout)
{
    // The helper has started, read the state file and is waiting for Orca. One that died on a missing library after
    // exec, or on a bad file, or that cannot write its marker, never reports ready; no offload is made without it.
    std::error_code ec;
    const auto      deadline = Clock::now() + timeout;
    while (!fs::exists(ready, ec)) {
        if (Clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

fs::path restorer_ready_file(const fs::path& state_file)
{
    return state_file.string() + ".ready";
}

// ---------------------------------------------------------------------------------------------------------------------
// Preview flow
// ---------------------------------------------------------------------------------------------------------------------

Policy policy_from_string(const std::string& value)
{
    if (value == "always")
        return Policy::AlwaysLend;
    if (value == "never")
        return Policy::Never;
    return Policy::Ask;
}

const char* policy_to_string(Policy policy)
{
    switch (policy) {
    case Policy::AlwaysLend: return "always";
    case Policy::Never: return "never";
    case Policy::Ask: break;
    }
    return "ask";
}

PreviewFlow::OnEnter PreviewFlow::enter_preview(bool feature_active)
{
    ++m_epoch;
    m_in_preview = true;
    m_left_at.reset();
    // The leases are being restored right now: that brings the models back onto the GPU, so the toolpath upload waits for
    // it and the VRAM is judged again afterwards.
    if (m_leased && m_releasing && feature_active && m_phase == Phase::Idle) {
        m_phase = Phase::Probing;
        return OnEnter::AwaitRelease;
    }
    if (m_leased)
        return OnEnter::KeepLeases;
    if (m_phase != Phase::Idle || !feature_active)
        return OnEnter::Nothing;
    m_phase = Phase::Probing;
    return OnEnter::Probe;
}

void PreviewFlow::leave_preview(Clock::time_point now)
{
    m_in_preview = false;
    if (m_leased)
        m_left_at = now;
}

bool PreviewFlow::no_offer()
{
    const bool was_deferred = m_phase != Phase::Idle;
    m_phase                 = Phase::Idle;
    return was_deferred;
}

void PreviewFlow::offload_started()
{
    m_phase = Phase::Offloading;
}

bool PreviewFlow::offload_finished(bool leased, Clock::time_point now)
{
    const bool was_deferred = m_phase != Phase::Idle;
    m_phase                 = Phase::Idle;
    m_leased                = m_leased || leased;
    if (m_leased && !m_in_preview)
        m_left_at = now;
    return was_deferred;
}

bool PreviewFlow::declined()
{
    return no_offer();
}

std::optional<PreviewFlow::Clock::time_point> PreviewFlow::restore_deadline() const
{
    if (!m_leased || m_in_preview || !m_left_at)
        return std::nullopt;
    return *m_left_at + m_grace;
}

bool PreviewFlow::restore_due(Clock::time_point now) const
{
    const auto deadline = restore_deadline();
    return deadline.has_value() && now >= *deadline;
}

}} // namespace Slic3r::VramOffload

#endif // __linux__
