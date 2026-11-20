// Tests of the VRAM offload logic (src/slic3r/Utils/VramOffload.*). They need no GUI: the daemons are played by a
// fake server on a Unix socket in a temp dir, the holder by a forked child process.
#include <catch2/catch_test_macros.hpp>

#ifdef __linux__

#include <poll.h>
#include <cstdio>
#include <memory>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/VramOffload.hpp"

namespace fs = std::filesystem;
using namespace Slic3r::VramOffload;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

struct TempDir
{
    fs::path path;
    TempDir()
    {
        char pattern[] = "/tmp/vramoffXXXXXX";
        REQUIRE(::mkdtemp(pattern) != nullptr);
        path = pattern;
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A daemon's control socket: one request line per connection, the reply the handler returns. An empty reply closes the
// connection without answering.
class FakeDaemon
{
public:
    using Handler = std::function<std::string(const json& request)>;

    FakeDaemon(const fs::path& socket, Handler handler) : m_socket(socket), m_handler(std::move(handler))
    {
        m_listen = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(m_listen >= 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket.c_str());
        REQUIRE(::bind(m_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(m_listen, 8) == 0);
        REQUIRE(::pipe(m_stop) == 0);
        m_thread = std::thread([this] { serve(); });
    }
    ~FakeDaemon()
    {
        char byte = 1;
        (void) !::write(m_stop[1], &byte, 1);
        m_thread.join();
        ::close(m_listen);
        ::close(m_stop[0]);
        ::close(m_stop[1]);
    }

    std::vector<json> requests() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_requests;
    }
    std::vector<json> requests_of(const std::string& op) const
    {
        std::vector<json> out;
        for (const json& request : requests())
            if (request.value("op", "") == op)
                out.push_back(request);
        return out;
    }

private:
    void serve()
    {
        for (;;) {
            pollfd fds[2] = {{m_listen, POLLIN, 0}, {m_stop[0], POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0 && errno == EINTR)
                continue;
            if (fds[1].revents != 0)
                return;
            const int client = ::accept(m_listen, nullptr, nullptr);
            if (client < 0)
                continue;
            std::string line;
            char        c;
            while (::read(client, &c, 1) == 1 && c != '\n')
                line.push_back(c);
            const json request = json::parse(line, nullptr, false);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_requests.push_back(request);
            }
            const std::string reply = m_handler(request);
            if (!reply.empty()) {
                const std::string out = reply + "\n";
                (void) !::send(client, out.data(), out.size(), MSG_NOSIGNAL);
            }
            ::close(client);
        }
    }

    fs::path                  m_socket;
    Handler                   m_handler;
    int                       m_listen{-1};
    int                       m_stop[2]{-1, -1};
    std::thread               m_thread;
    mutable std::mutex        m_mutex;
    std::vector<json>         m_requests;
};

std::string ok_reply() { return R"({"ok":true,"seconds":0.25,"freed_bytes":1610612736})"; }
std::string err_reply(const std::string& error) { return json{{"ok", false}, {"error", error}}.dump(); }

std::string status_reply(const std::string& name, long long pid, const std::string& state)
{
    return json{{"ok", true},
                {"name", name},
                {"pid", pid},
                {"state", state},
                {"weights_bytes", 1610612736},
                {"idle_s", 12.5},
                {"leases", json::array()},
                {"last_restore_s", 0.4}}
        .dump();
}

Coordinator::Config coordinator_config(const fs::path& state_file, std::function<bool(const fs::path&)> ensure_restorer)
{
    Coordinator::Config config;
    config.pid             = ::getpid();
    config.start           = process_start_ticks(::getpid()).value_or(0);
    config.state_file      = state_file;
    config.ensure_restorer = std::move(ensure_restorer);
    config.timeouts.status  = 500ms;
    config.timeouts.offload = 2000ms;
    config.timeouts.restore = 2000ms;
    return config;
}

Consumer consumer_of(const std::string& name, const fs::path& socket)
{
    Consumer consumer;
    consumer.name       = name;
    consumer.socket     = socket;
    consumer.vram_bytes = 1;
    return consumer;
}

} // namespace

TEST_CASE("Process start time parsing survives odd command names", "[VramOffload]")
{
    // "(a) b) c (d)" holds spaces and parentheses: the fields begin after the LAST ')'.
    const std::string line = "4242 ((a) b) c (d)) S 1 4242 4242 0 -1 4194560 100 0 0 0 1 2 0 0 20 0 1 0 987654 1000 20 18446744073709551615";
    CHECK(parse_stat_start_ticks(line) == 987654ULL);
    CHECK_FALSE(parse_stat_start_ticks("garbage").has_value());
    CHECK_FALSE(parse_stat_start_ticks("1 (x) S 2 3").has_value());
}

TEST_CASE("A reused pid with another start time is not the holder", "[VramOffload]")
{
    const auto start = process_start_ticks(::getpid());
    REQUIRE(start.has_value());
    CHECK(process_alive(::getpid(), *start));
    CHECK_FALSE(process_alive(::getpid(), *start + 1));
    CHECK_FALSE(process_alive(0x7ffffff0, *start));
}

TEST_CASE("Replies map to failures", "[VramOffload]")
{
    CHECK(parse_reply(ok_reply()).ok);
    CHECK(parse_reply(ok_reply()).freed_bytes == 1610612736ULL);
    CHECK(parse_reply(err_reply("busy")).failure == Failure::Busy);
    CHECK(parse_reply(err_reply("timeout")).failure == Failure::Timeout);
    CHECK(parse_reply(err_reply("oom")).failure == Failure::Oom);
    CHECK(parse_reply(err_reply("whatever")).failure == Failure::Other);
    CHECK(parse_reply("not json").failure == Failure::Protocol);
    CHECK(parse_reply(R"({"seconds":1})").failure == Failure::Protocol);
}

TEST_CASE("Protocol client against a fake daemon", "[VramOffload]")
{
    TempDir  dir;
    const fs::path socket = dir.path / "d.sock";

    SECTION("status, offload and restore round trip")
    {
        FakeDaemon daemon(socket, [](const json& request) {
            const std::string op = request.value("op", "");
            if (op == "status")
                return status_reply("wyoming-whisper", 4321, "loaded");
            return ok_reply();
        });
        const auto status = request_status(socket, 1s);
        REQUIRE(status.has_value());
        CHECK(status->name == "wyoming-whisper");
        CHECK(status->pid == 4321);
        CHECK(status->state == "loaded");
        CHECK(status->last_restore_s == 0.4);
        CHECK_FALSE(status->last_offload_s.has_value());

        const Reply offload = request_offload(socket, "lease-1", 77, 12345, 1s);
        CHECK(offload.ok);
        const auto sent = daemon.requests_of("offload");
        REQUIRE(sent.size() == 1);
        CHECK(sent[0]["lease"] == "lease-1");
        CHECK(sent[0]["holder_pid"] == 77);
        CHECK(sent[0]["holder_start"] == 12345);

        CHECK(request_restore(socket, "lease-1", 1s).ok);
        CHECK(daemon.requests_of("restore").size() == 1);
    }
    SECTION("busy, timeout and oom keep their kind")
    {
        FakeDaemon daemon(socket, [](const json& request) {
            const std::string lease = request.value("lease", "");
            return err_reply(lease == "b" ? "busy" : lease == "t" ? "timeout" : "oom");
        });
        CHECK(request_offload(socket, "b", 1, 1, 1s).failure == Failure::Busy);
        CHECK(request_offload(socket, "t", 1, 1, 1s).failure == Failure::Timeout);
        CHECK(request_restore(socket, "o", 1s).failure == Failure::Oom);
    }
    SECTION("an unknown lease is ok")
    {
        FakeDaemon daemon(socket, [](const json&) { return std::string(R"({"ok":true,"seconds":0})"); });
        CHECK(request_restore(socket, "never-issued", 1s).ok);
    }
    SECTION("a missing socket and a silent daemon are unreachable")
    {
        CHECK(request_restore(dir.path / "absent.sock", "x", 200ms).failure == Failure::Unreachable);
        FakeDaemon daemon(socket, [](const json&) {
            std::this_thread::sleep_for(600ms);
            return std::string();
        });
        const auto started = std::chrono::steady_clock::now();
        CHECK(request_restore(socket, "x", 150ms).failure == Failure::Unreachable);
        CHECK(std::chrono::steady_clock::now() - started < 500ms);
    }
}

TEST_CASE("State file round trip with an atomic rewrite", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = state_file_path(dir.path / "snapmaker-orca", 4242);
    CHECK(file.filename() == "vram-offload-4242.json");

    RestoreState state;
    state.orca_pid   = 4242;
    state.orca_start = 987654;
    state.items      = {{"wyoming-whisper", "/run/user/1000/vram-offload/wyoming-whisper.sock", "lease-a"},
                        {"task-classify", "/run/user/1000/vram-offload/task-classify.sock", "lease-b"}};
    REQUIRE(write_state_file(file, state));

    const auto read = read_state_file(file);
    REQUIRE(read.has_value());
    CHECK(read->orca_pid == 4242);
    CHECK(read->orca_start == 987654ULL);
    CHECK(read->items == state.items);

    struct stat info{};
    REQUIRE(::stat(file.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    CHECK((fs::status(file.parent_path()).permissions() & fs::perms::others_all) == fs::perms::none);

    state.items.pop_back();
    REQUIRE(write_state_file(file, state));
    CHECK(read_state_file(file)->items.size() == 1);
    CHECK_FALSE(fs::exists(file.string() + ".tmp"));

    std::ofstream(file) << "{ truncated";
    CHECK_FALSE(read_state_file(file).has_value());
}

TEST_CASE("Consumers are the loaded daemons that NVML lists, heaviest first", "[VramOffload]")
{
    TempDir dir;
    FakeDaemon whisper(dir.path / "wyoming-whisper.sock", [](const json&) { return status_reply("wyoming-whisper", 100, "loaded"); });
    FakeDaemon classifier(dir.path / "task-classify.sock", [](const json&) { return status_reply("task-classify", 200, "loaded"); });
    FakeDaemon idle(dir.path / "idle.sock", [](const json&) { return status_reply("idle", 300, "unloaded"); });
    FakeDaemon pinned(dir.path / "pinned.sock", [](const json&) { return status_reply("pinned", 400, "pinned"); });
    FakeDaemon off_gpu(dir.path / "cpu.sock", [](const json&) { return status_reply("cpu", 500, "loaded"); });

    GpuSnapshot gpu;
    gpu.total_bytes = 8ULL << 30;
    gpu.free_bytes  = 1ULL << 30;
    gpu.processes   = {{100, 2224ULL << 20}, {200, 2722ULL << 20}, {300, 10ULL << 20}, {400, 10ULL << 20}, {999, 4ULL << 30}};

    CHECK(has_control_sockets(dir.path));
    const auto consumers = find_consumers(dir.path, gpu, 500ms);
    REQUIRE(consumers.size() == 2); // 999 has no socket; idle/pinned hold nothing to give; cpu.sock's pid is not on the GPU
    CHECK(consumers[0].name == "task-classify");
    CHECK(consumers[0].vram_bytes == 2722ULL << 20);
    CHECK(consumers[1].name == "wyoming-whisper");
    CHECK(consumers[1].last_restore_s == 0.4);

    CHECK_FALSE(has_control_sockets(dir.path / "nothing"));
}

TEST_CASE("NVML probe is off cleanly when the library is missing", "[VramOffload]")
{
    std::string why;
    CHECK_FALSE(probe_gpu("libvram-offload-no-such-nvml.so.9", &why).has_value());
    CHECK_FALSE(why.empty());
}

TEST_CASE("The GPU Orca renders on is picked among several", "[VramOffload]")
{
    GpuSnapshot a;
    a.index         = 0;
    a.free_bytes    = 6ULL << 30;
    a.graphics_pids = {111};
    GpuSnapshot b;
    b.index         = 1;
    b.free_bytes    = 1ULL << 30;
    b.graphics_pids = {222, 4242};
    b.processes     = {{100, 2ULL << 30}};

    CHECK_FALSE(choose_render_gpu({}, 4242).has_value());
    // Two GPUs: the one listing this process as a graphics client, not index 0.
    CHECK(choose_render_gpu({a, b}, 4242) == std::optional<std::size_t>(1));
    CHECK(choose_render_gpu({b, a}, 111) == std::optional<std::size_t>(1));
    // Several GPUs and none lists it: no guess, nothing is offered.
    CHECK_FALSE(choose_render_gpu({a, b}, 9999).has_value());
    CHECK_FALSE(choose_render_gpu({a, b}, 0).has_value());
    // One GPU: it is the one, even when the render path is not listed (Zink and the like).
    CHECK(choose_render_gpu({a}, 9999) == std::optional<std::size_t>(0));
    // A compute entry counts as well, when no graphics list names the process.
    GpuSnapshot c = a;
    c.processes   = {{4242, 1ULL << 20}};
    CHECK(choose_render_gpu({a, c}, 4242) == std::optional<std::size_t>(1));

    // Graphics beats compute: GPU 0 holds a compute context of this pid, GPU 1 the graphics one that draws.
    GpuSnapshot compute_only = a;
    compute_only.processes   = {{4242, 1ULL << 20}};
    CHECK(choose_render_gpu({compute_only, b}, 4242) == std::optional<std::size_t>(1));

    // Two graphics matches, or two compute matches: ambiguous, enumeration order must not decide.
    GpuSnapshot b2 = b;
    b2.index       = 2;
    CHECK_FALSE(choose_render_gpu({a, b, b2}, 4242).has_value());
    GpuSnapshot c2 = c;
    c2.index       = 3;
    CHECK_FALSE(choose_render_gpu({a, c, c2}, 4242).has_value());
}

TEST_CASE("A restorer counts only once its marker exists", "[VramOffload]")
{
    TempDir  dir;
    const fs::path ready = dir.path / "vram-offload-1.json.ready";
    // An active helper without a marker (stuck in its start, or unable to write it) is not ready.
    const auto started = std::chrono::steady_clock::now();
    CHECK_FALSE(wait_for_restorer_ready(ready, 100ms));
    CHECK(std::chrono::steady_clock::now() - started >= 100ms);
    // A marker that shows up while waiting ends the wait.
    std::thread writer([&] {
        std::this_thread::sleep_for(80ms);
        std::ofstream(ready) << "1\n";
    });
    CHECK(wait_for_restorer_ready(ready, 2000ms));
    writer.join();
    CHECK(wait_for_restorer_ready(ready, 0ms));
}

TEST_CASE("A stale callback is told apart by the preview entry count", "[VramOffload][PreviewFlow]")
{
    PreviewFlow flow;
    const auto  t0 = PreviewFlow::Clock::time_point{};
    flow.enter_preview(true);
    flow.leave_preview(t0);
    const auto queued_at_leave = flow.epoch();
    CHECK(queued_at_leave == flow.epoch()); // nothing happened yet: the callback is current
    flow.enter_preview(true);               // entered again before the callback ran
    CHECK(queued_at_leave != flow.epoch());
}

TEST_CASE("Offload writes the lease down before it asks the daemon", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file   = dir.path / "state" / "vram-offload-1.json";
    const fs::path socket = dir.path / "w.sock";

    std::optional<RestoreState> seen_at_offload;
    FakeDaemon daemon(socket, [&](const json& request) {
        if (request.value("op", "") == "offload")
            seen_at_offload = read_state_file(file);
        return ok_reply();
    });
    bool restorer_started_before_offload = false;
    Coordinator coordinator(coordinator_config(file, [&](const fs::path& state_file) {
        restorer_started_before_offload = fs::exists(state_file) && daemon.requests_of("offload").empty();
        return true;
    }));

    const auto results = coordinator.offload({consumer_of("wyoming-whisper", socket)});
    REQUIRE(results.size() == 1);
    CHECK(results[0].leased);
    CHECK(restorer_started_before_offload);
    REQUIRE(seen_at_offload.has_value());
    REQUIRE(seen_at_offload->items.size() == 1);
    CHECK(seen_at_offload->items[0].lease == daemon.requests_of("offload")[0]["lease"].get<std::string>());
    CHECK(seen_at_offload->orca_pid == ::getpid());
    CHECK(coordinator.held_count() == 1);
    CHECK(read_state_file(file)->items.size() == 1);
}

TEST_CASE("A daemon that cannot offload is rolled back", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "state" / "vram-offload-1.json";

    for (const char* error : {"busy", "timeout"}) {
        DYNAMIC_SECTION("daemon answers " << error)
        {
            FakeDaemon daemon(dir.path / "w.sock", [&](const json& request) {
                return request.value("op", "") == "offload" ? err_reply(error) : ok_reply();
            });
            Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));
            const auto results = coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock")});
            REQUIRE(results.size() == 1);
            CHECK_FALSE(results[0].leased);
            CHECK(results[0].reply.failure == (std::string(error) == "busy" ? Failure::Busy : Failure::Timeout));
            // The lease is released again, for a reply the daemon could have sent after we gave up.
            const auto offered  = daemon.requests_of("offload");
            const auto restored = daemon.requests_of("restore");
            REQUIRE(offered.size() == 1);
            REQUIRE(restored.size() == 1);
            CHECK(restored[0]["lease"] == offered[0]["lease"]);
            CHECK(coordinator.held_count() == 0);
            CHECK(read_state_file(file)->items.empty());
        }
    }

    SECTION("no reply at all")
    {
        FakeDaemon daemon(dir.path / "w.sock", [](const json& request) {
            return request.value("op", "") == "offload" ? std::string() : ok_reply();
        });
        Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));
        const auto results = coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock")});
        CHECK_FALSE(results[0].leased);
        CHECK(results[0].reply.failure == Failure::Unreachable);
        CHECK(daemon.requests_of("restore").size() == 1);
        CHECK(coordinator.held_count() == 0);
    }

    SECTION("one consumer fails, the other keeps its lease")
    {
        FakeDaemon whisper(dir.path / "w.sock", [](const json& request) { return request.value("op", "") == "offload" ? err_reply("busy") : ok_reply(); });
        FakeDaemon classifier(dir.path / "c.sock", [](const json&) { return ok_reply(); });
        Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));
        const auto results = coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock"), consumer_of("task-classify", dir.path / "c.sock")});
        REQUIRE(results.size() == 2);
        CHECK_FALSE(results[0].leased);
        CHECK(results[1].leased);
        const auto state = read_state_file(file);
        REQUIRE(state->items.size() == 1);
        CHECK(state->items[0].name == "task-classify");
    }
}

TEST_CASE("Without a restorer nothing is offloaded", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "state" / "vram-offload-1.json";
    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });
    Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return false; }));
    const auto results = coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock")});
    CHECK_FALSE(results[0].leased);
    CHECK(daemon.requests().empty());
    CHECK(coordinator.held_count() == 0);
    CHECK(read_state_file(file)->items.empty());
}

TEST_CASE("Release restores every lease and keeps what the daemon did not confirm", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "state" / "vram-offload-1.json";

    FakeDaemon whisper(dir.path / "w.sock", [](const json& request) { return request.value("op", "") == "restore" ? err_reply("oom") : ok_reply(); });
    auto classifier = std::make_unique<FakeDaemon>(dir.path / "c.sock", [](const json&) { return ok_reply(); });

    Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));
    REQUIRE(coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock"), consumer_of("task-classify", dir.path / "c.sock")}).size() == 2);
    REQUIRE(coordinator.held_count() == 2);

    // The classifier goes away before the release.
    classifier.reset();
    fs::remove(dir.path / "c.sock");

    const auto released = coordinator.release_all();
    REQUIRE(released.size() == 2);
    // An OOM on reload leaves the lease gone (the daemon stays unloaded): it counts as released.
    CHECK(released[0].released);
    CHECK(released[0].reply.failure == Failure::Oom);
    CHECK_FALSE(released[1].released);
    CHECK(released[1].reply.failure == Failure::Unreachable);
    CHECK(coordinator.held_count() == 1);
    CHECK(read_state_file(file)->items.size() == 1);

    // Exit with the daemon still away: the lease stays in the file for the restorer.
    coordinator.close(100ms);
    CHECK(fs::exists(file));

    // The classifier comes back; the restorer's retry confirms it.
    FakeDaemon back(dir.path / "c.sock", [](const json&) { return ok_reply(); });
    CHECK(restore_items(*read_state_file(file), Timeouts{}, 1, 0ms).empty());
    CHECK(back.requests_of("restore").size() == 1);

}

TEST_CASE("A clean close empties and removes the state file", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "state" / "vram-offload-1.json";
    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });
    Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));
    REQUIRE(coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock")})[0].leased);
    coordinator.close();
    CHECK(coordinator.held_count() == 0);
    CHECK_FALSE(fs::exists(file));
    CHECK(daemon.requests_of("restore").size() == 1);
}

TEST_CASE("Restorer: a SIGKILLed holder gets its leases restored", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "vram-offload-holder.json";

    const pid_t holder = ::fork();
    REQUIRE(holder >= 0);
    if (holder == 0) {
        for (;;)
            ::pause();
    }
    const auto start = process_start_ticks(holder);
    REQUIRE(start.has_value());

    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });
    RestoreState state;
    state.orca_pid   = holder;
    state.orca_start = *start;
    state.items      = {{"wyoming-whisper", dir.path / "w.sock", "lease-crash"}};
    REQUIRE(write_state_file(file, state));

    std::atomic<int> code{-1};
    std::thread      watcher([&] { code = run_restore_watch(file); });

    std::this_thread::sleep_for(150ms);
    CHECK(daemon.requests_of("restore").empty()); // the holder lives: nothing is restored while it runs
    CHECK(code == -1);
    // The readiness handshake ensure_restorer_unit() waits for: the helper has read the file and waits for the holder.
    CHECK(fs::exists(restorer_ready_file(file)));

    ::kill(holder, SIGKILL);
    int status = 0;
    ::waitpid(holder, &status, 0);
    watcher.join();

    CHECK(code == 0);
    const auto restored = daemon.requests_of("restore");
    REQUIRE(restored.size() == 1);
    CHECK(restored[0]["lease"] == "lease-crash");
    CHECK_FALSE(fs::exists(file));
    CHECK_FALSE(fs::exists(restorer_ready_file(file))); // the marker goes with the helper
}

TEST_CASE("Restorer: a holder pid with another start time counts as already dead", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "vram-offload-reused.json";
    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });

    RestoreState state;
    state.orca_pid   = ::getpid(); // alive, but not the process that took the lease
    state.orca_start = process_start_ticks(::getpid()).value_or(0) + 12345;
    state.items      = {{"wyoming-whisper", dir.path / "w.sock", "lease-stale"}};
    REQUIRE(write_state_file(file, state));

    CHECK(run_restore_watch(file) == 0);
    CHECK(daemon.requests_of("restore").size() == 1);
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("Restorer: an emptied state file means nothing to do", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "vram-offload-clean.json";
    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });

    RestoreState state;
    state.orca_pid   = ::getpid();
    state.orca_start = process_start_ticks(::getpid()).value_or(0) + 1; // "dead" at once
    REQUIRE(write_state_file(file, state));
    CHECK(run_restore_watch(file) == 0);
    CHECK(daemon.requests().empty());
    CHECK_FALSE(fs::exists(file));
    CHECK(run_restore_watch(dir.path / "missing.json") == 0);
}

TEST_CASE("Dead holders' state files are pruned, live ones are left alone", "[VramOffload]")
{
    TempDir  dir;
    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });

    RestoreState dead;
    dead.orca_pid   = ::getpid();
    dead.orca_start = process_start_ticks(::getpid()).value_or(0) + 1;
    dead.items      = {{"wyoming-whisper", dir.path / "w.sock", "lease-dead"}};
    REQUIRE(write_state_file(dir.path / "vram-offload-1.json", dead));

    RestoreState alive = dead;
    alive.orca_start   = process_start_ticks(::getpid()).value_or(0);
    alive.items        = {{"wyoming-whisper", dir.path / "w.sock", "lease-alive"}};
    REQUIRE(write_state_file(dir.path / "vram-offload-2.json", alive));

    CHECK(restore_orphans(dir.path) == 1);
    const auto restored = daemon.requests_of("restore");
    REQUIRE(restored.size() == 1);
    CHECK(restored[0]["lease"] == "lease-dead");
    CHECK_FALSE(fs::exists(dir.path / "vram-offload-1.json"));
    CHECK(fs::exists(dir.path / "vram-offload-2.json"));
}

TEST_CASE("A dead holder's lease the daemon did not confirm stays recorded for the next sweep", "[VramOffload]")
{
    TempDir  dir;
    RestoreState dead;
    dead.orca_pid   = ::getpid();
    dead.orca_start = process_start_ticks(::getpid()).value_or(0) + 1;
    dead.items      = {{"wyoming-whisper", dir.path / "w.sock", "lease-later"}};
    const fs::path file = dir.path / "vram-offload-1.json";
    REQUIRE(write_state_file(file, dead));

    // The daemon is not there: nothing is confirmed, the record stays.
    CHECK(restore_orphans(dir.path) == 1);
    REQUIRE(read_state_file(file).has_value());
    CHECK(read_state_file(file)->items.size() == 1);

    // It comes back: the next sweep restores the lease and removes the record.
    FakeDaemon daemon(dir.path / "w.sock", [](const json&) { return ok_reply(); });
    CHECK(restore_orphans(dir.path) == 1);
    CHECK(daemon.requests_of("restore").size() == 1);
    CHECK_FALSE(fs::exists(file));
}

TEST_CASE("Lease ids are version 4 UUIDs and distinct", "[VramOffload]")
{
    const std::string a = new_lease_id();
    const std::string b = new_lease_id();
    CHECK(a != b);
    REQUIRE(a.size() == 36);
    CHECK(a[8] == '-');
    CHECK(a[14] == '4');
}

TEST_CASE("Policy strings", "[VramOffload]")
{
    CHECK(policy_from_string("always") == Policy::AlwaysLend);
    CHECK(policy_from_string("never") == Policy::Never);
    CHECK(policy_from_string("ask") == Policy::Ask);
    CHECK(policy_from_string("") == Policy::Ask);
    CHECK(policy_from_string("garbage") == Policy::Ask);
    CHECK(std::string(policy_to_string(Policy::Never)) == "never");
}

TEST_CASE("The toolpath upload waits for the offload decision", "[VramOffload][PreviewFlow]")
{
    using Flow = PreviewFlow;
    const auto t0 = Flow::Clock::time_point{};

    SECTION("a dialog that answers Not now uploads at once")
    {
        Flow flow;
        CHECK_FALSE(flow.upload_deferred());
        CHECK(flow.enter_preview(true) == Flow::OnEnter::Probe);
        CHECK(flow.upload_deferred()); // the probe and the dialog are pending
        CHECK(flow.declined());
        CHECK_FALSE(flow.upload_deferred());
        CHECK_FALSE(flow.leased());
    }
    SECTION("Free VRAM uploads only after the offload replied")
    {
        Flow flow;
        REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
        flow.offload_started();
        CHECK(flow.upload_deferred());
        CHECK(flow.offload_finished(true, t0));
        CHECK_FALSE(flow.upload_deferred());
        CHECK(flow.leased());
    }
    SECTION("a failed offload uploads too and holds nothing")
    {
        Flow flow;
        REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
        flow.offload_started();
        CHECK(flow.offload_finished(false, t0));
        CHECK_FALSE(flow.upload_deferred());
        CHECK_FALSE(flow.leased());
    }
    SECTION("nothing to offer uploads at once")
    {
        Flow flow;
        REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
        CHECK(flow.no_offer());
        CHECK_FALSE(flow.upload_deferred());
    }
    SECTION("an inactive feature never defers")
    {
        Flow flow;
        CHECK(flow.enter_preview(false) == Flow::OnEnter::Nothing);
        CHECK_FALSE(flow.upload_deferred());
    }
    SECTION("re-entering while the decision is pending does not start a second probe")
    {
        Flow flow;
        REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
        flow.leave_preview(t0);
        CHECK(flow.enter_preview(true) == Flow::OnEnter::Nothing);
        CHECK(flow.upload_deferred());
    }
}

TEST_CASE("The grace timer restores only after a continuous 60 s outside the preview", "[VramOffload][PreviewFlow]")
{
    using Flow = PreviewFlow;
    const auto t0 = Flow::Clock::time_point{} + 1000s;

    Flow flow(60s);
    REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
    flow.offload_started();
    REQUIRE(flow.offload_finished(true, t0));
    CHECK_FALSE(flow.restore_due(t0 + 3600s)); // in the preview: never

    flow.leave_preview(t0);
    CHECK(flow.restore_deadline() == t0 + 60s);
    CHECK_FALSE(flow.restore_due(t0 + 59s));

    // Back at 59 s: the timer is cancelled and the leases are kept without a new probe.
    CHECK(flow.enter_preview(true) == Flow::OnEnter::KeepLeases);
    CHECK_FALSE(flow.upload_deferred());
    CHECK_FALSE(flow.restore_deadline().has_value());
    CHECK_FALSE(flow.restore_due(t0 + 61s));

    // Out again: the 60 s count from the new departure, not the old one.
    const auto t1 = t0 + 59s;
    flow.leave_preview(t1);
    CHECK_FALSE(flow.restore_due(t1 + 59s));
    CHECK(flow.restore_due(t1 + 60s));

    flow.release_started();
    flow.release_finished(true);
    CHECK_FALSE(flow.restore_due(t1 + 600s));
    // After the restore a new entry probes again.
    CHECK(flow.enter_preview(true) == Flow::OnEnter::Probe);
}

TEST_CASE("Re-entering the preview while the leases are restored waits for the restore", "[VramOffload][PreviewFlow]")
{
    using Flow = PreviewFlow;
    const auto t0 = Flow::Clock::time_point{} + 1000s;

    Flow flow(60s);
    REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
    flow.offload_started();
    REQUIRE(flow.offload_finished(true, t0));
    flow.leave_preview(t0);
    flow.release_started();

    // The models are coming back onto the GPU: no keep-the-leases shortcut, the upload waits, the VRAM is judged again.
    CHECK(flow.enter_preview(true) == Flow::OnEnter::AwaitRelease);
    CHECK(flow.upload_deferred());
    flow.release_finished(true);
    CHECK_FALSE(flow.leased());
    CHECK(flow.no_offer());
    CHECK_FALSE(flow.upload_deferred());
}

TEST_CASE("A release that is not confirmed keeps the leases held", "[VramOffload][PreviewFlow]")
{
    using Flow = PreviewFlow;
    const auto t0 = Flow::Clock::time_point{} + 1000s;

    Flow flow(60s);
    REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
    flow.offload_started();
    flow.offload_finished(true, t0);
    flow.leave_preview(t0);
    flow.release_started();
    flow.release_finished(false);
    CHECK(flow.leased());
    CHECK(flow.enter_preview(true) == Flow::OnEnter::KeepLeases);
}

TEST_CASE("A lease whose rollback is not confirmed stays held", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "state" / "vram-offload-1.json";
    std::atomic<bool> healthy{false};
    FakeDaemon daemon(dir.path / "w.sock", [&](const json&) { return healthy ? ok_reply() : std::string(); });
    Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));

    const auto results = coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock")});
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].leased);
    CHECK(results[0].uncertain);
    CHECK(coordinator.held_count() == 1);
    CHECK(read_state_file(file)->items.size() == 1);

    // The daemon answers again: the next release gives the lease back.
    healthy = true;
    const auto released = coordinator.release_all();
    REQUIRE(released.size() == 1);
    CHECK(released[0].released);
    CHECK(coordinator.held_count() == 0);
}

TEST_CASE("Closing does not wait for an offload that is still running", "[VramOffload]")
{
    TempDir  dir;
    const fs::path file = dir.path / "state" / "vram-offload-1.json";
    FakeDaemon daemon(dir.path / "w.sock", [](const json& request) {
        if (request.value("op", "") == "offload")
            std::this_thread::sleep_for(1200ms);
        return ok_reply();
    });
    Coordinator coordinator(coordinator_config(file, [](const fs::path&) { return true; }));

    std::thread offloading([&] { coordinator.offload({consumer_of("wyoming-whisper", dir.path / "w.sock")}); });
    while (daemon.requests_of("offload").empty())
        std::this_thread::sleep_for(10ms);

    const auto started = std::chrono::steady_clock::now();
    const auto results = coordinator.close(200ms, 150ms);
    CHECK(std::chrono::steady_clock::now() - started < 800ms);
    CHECK(results.empty());
    // The lease is in the state file for the restorer.
    REQUIRE(read_state_file(file).has_value());
    CHECK(read_state_file(file)->items.size() == 1);
    offloading.join();
}

TEST_CASE("An offload that ends after the preview was left starts the grace period then", "[VramOffload][PreviewFlow]")
{
    using Flow = PreviewFlow;
    const auto t0 = Flow::Clock::time_point{} + 1000s;

    Flow flow(60s);
    REQUIRE(flow.enter_preview(true) == Flow::OnEnter::Probe);
    flow.offload_started();
    flow.leave_preview(t0);
    CHECK(flow.offload_finished(true, t0 + 12s));
    CHECK(flow.restore_deadline() == t0 + 72s);
}

#else

TEST_CASE("VRAM offload is Linux only", "[VramOffload]")
{
    SUCCEED();
}

#endif // __linux__
