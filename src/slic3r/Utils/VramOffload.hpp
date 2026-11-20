#ifndef slic3r_Utils_VramOffload_hpp_
#define slic3r_Utils_VramOffload_hpp_

// Temporary VRAM offload for the G-code preview (docs/superpowers/specs/2026-11-18-vram-offload-design.md).
//
// Heavy GPU daemons (wyoming-whisper, task-classify-server) expose a control socket
// ($XDG_RUNTIME_DIR/vram-offload/<name>.sock). When the preview is short of VRAM, Orca asks them to move their weights
// off the GPU under a lease and gives the lease back when the preview is left. The processes are never stopped.
//
// This file has no wx and no GL dependency, so the logic builds and is tested without the GUI.

#ifdef __linux__

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace VramOffload {

// ---------------------------------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------------------------------

// $XDG_RUNTIME_DIR, or an empty path when it is unset (the feature is then off).
std::filesystem::path runtime_dir();
// <runtime>/vram-offload: the daemons' control sockets.
std::filesystem::path socket_dir();
// <runtime>/snapmaker-orca: Orca's restorer state files.
std::filesystem::path state_dir();
std::filesystem::path state_file_path(const std::filesystem::path& dir, pid_t orca_pid);

// ---------------------------------------------------------------------------------------------------------------------
// Process identity: pid + start time (field 22 of /proc/<pid>/stat), which survives pid reuse.
// ---------------------------------------------------------------------------------------------------------------------

// Field 22 (clock ticks since boot) of a /proc/<pid>/stat line; the command name may contain spaces and parentheses.
std::optional<unsigned long long> parse_stat_start_ticks(const std::string& stat_line);
std::optional<unsigned long long> process_start_ticks(pid_t pid);
// True when `pid` runs and its start time is `start_ticks` (a reused pid has another one).
bool process_alive(pid_t pid, unsigned long long start_ticks);

// ---------------------------------------------------------------------------------------------------------------------
// Control protocol client: newline-delimited JSON, one request per connection.
// ---------------------------------------------------------------------------------------------------------------------

enum class Failure {
    Ok,          // no failure (X11 defines `None`, which the GUI translation units see)
    Busy,        // the daemon could not drain its requests; it rolled the lease back
    Timeout,     // the daemon timed out unloading; it rolled the lease back
    Oom,         // restore: CUDA out of memory; the lease is gone, the daemon stays unloaded
    Unreachable, // no connection, no reply in time, or the connection broke
    Protocol,    // a reply that is not valid for the request
    Other        // {"ok":false,"error":<something else>}
};

struct Reply
{
    bool          ok{false};
    Failure       failure{Failure::Ok};
    std::string   error;          // the daemon's error text, or what went wrong on the way
    double        seconds{0.};    // offload / restore duration the daemon measured
    std::uint64_t freed_bytes{0}; // offload only
};

struct LeaseInfo
{
    std::string        id;
    long long          holder_pid{0};
    unsigned long long holder_start{0};
};

struct DaemonStatus
{
    std::string                name;
    long long                  pid{0};
    std::string                state; // loaded | unloaded | pinned | loading
    std::uint64_t              weights_bytes{0};
    double                     idle_s{0.};
    std::vector<LeaseInfo>     leases;
    std::optional<double>      last_offload_s;
    std::optional<double>      last_restore_s;
    std::optional<double>      last_first_answer_s;
};

struct Timeouts
{
    std::chrono::milliseconds status{2000};
    // The daemon waits up to 10 s for its requests to drain, then moves the weights.
    std::chrono::milliseconds offload{30000};
    std::chrono::milliseconds restore{15000};
};

// Pure parsing of the daemons' replies (exposed for the fake-server tests).
Reply  parse_reply(const std::string& line);
std::optional<DaemonStatus> parse_status(const std::string& line);

std::optional<DaemonStatus> request_status(const std::filesystem::path& socket, std::chrono::milliseconds timeout, std::string* error = nullptr);
Reply request_offload(const std::filesystem::path& socket, const std::string& lease, pid_t holder_pid, unsigned long long holder_start,
                      std::chrono::milliseconds timeout);
Reply request_restore(const std::filesystem::path& socket, const std::string& lease, std::chrono::milliseconds timeout);

// ---------------------------------------------------------------------------------------------------------------------
// NVML probe
// ---------------------------------------------------------------------------------------------------------------------

struct GpuProcess
{
    unsigned int                pid{0};
    std::optional<std::uint64_t> used_bytes; // unknown when NVML reports "not available" (containers)
};

struct GpuSnapshot
{
    std::uint64_t           total_bytes{0};
    std::uint64_t           free_bytes{0};
    std::vector<GpuProcess> processes;
};

// dlopen(library, RTLD_NOW | RTLD_NODELETE) + nvmlInit_v2 ... nvmlShutdown. Never dlclose (cuda-checkpoint issue #58).
// nullopt, with the reason in `why`, when the library or any symbol is missing or NVML reports an error: the feature
// is then off and Orca behaves as without it.
std::optional<GpuSnapshot> probe_gpu(const char* library = "libnvidia-ml.so.1", std::string* why = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Consumers: a daemon with a control socket that holds VRAM
// ---------------------------------------------------------------------------------------------------------------------

struct Consumer
{
    std::string           name;
    std::filesystem::path socket;
    long long             pid{0};
    std::uint64_t         vram_bytes{0}; // NVML per-pid usage, else the daemon's weights_bytes
    std::optional<double> last_restore_s;
};

// Sockets whose status reports state "loaded" and whose pid NVML lists as a GPU process. A GPU process without a socket
// is not offered; a socket that does not answer is skipped.
std::vector<Consumer> find_consumers(const std::filesystem::path& dir, const GpuSnapshot& gpu, std::chrono::milliseconds status_timeout);
// True when at least one socket exists: the cheap test whether the feature can do anything at all.
bool has_control_sockets(const std::filesystem::path& dir);

// ---------------------------------------------------------------------------------------------------------------------
// Restorer state file
// ---------------------------------------------------------------------------------------------------------------------

struct RestoreItem
{
    std::string           name;
    std::filesystem::path socket;
    std::string           lease;
    bool operator==(const RestoreItem& other) const { return name == other.name && socket == other.socket && lease == other.lease; }
};

struct RestoreState
{
    long long                orca_pid{0};
    unsigned long long       orca_start{0};
    std::vector<RestoreItem> items;
};

// tmp file + rename, mode 0600.
bool write_state_file(const std::filesystem::path& file, const RestoreState& state, std::string* error = nullptr);
std::optional<RestoreState> read_state_file(const std::filesystem::path& file);

// ---------------------------------------------------------------------------------------------------------------------
// Lease holder (Orca side)
// ---------------------------------------------------------------------------------------------------------------------

struct OffloadResult
{
    Consumer consumer;
    Reply    reply;
    bool     leased{false};    // the daemon holds a lease for this consumer now
    bool     uncertain{false}; // the offload failed and so did the rollback: the lease stays held (see Coordinator::offload)
};

struct ReleaseResult
{
    RestoreItem item;
    Reply       reply;
    bool        released{false}; // the daemon no longer holds the lease (ok, or the lease is gone after an OOM reload)
};

class Coordinator
{
public:
    struct Config
    {
        pid_t              pid{0};
        unsigned long long start{0};
        std::filesystem::path state_file;
        // Starts the restorer when it does not run yet; false when it cannot. Without a restorer nothing is offloaded.
        std::function<bool(const std::filesystem::path& state_file)> ensure_restorer;
        Timeouts timeouts;
    };

    explicit Coordinator(Config config);

    // The state file is written before `offload` is sent and rewritten after each reply. A consumer that does not
    // confirm (busy, timeout, no reply) is rolled back with a `restore` of its lease and dropped. When even the
    // `restore` gets no answer the daemon may still hold the lease, so it stays held (OffloadResult::uncertain) and
    // is released later like the others.
    std::vector<OffloadResult> offload(const std::vector<Consumer>& consumers);

    // `restore` for every held lease. A lease the daemon cannot confirm (unreachable) stays held and stays in the
    // state file, for the next release or the restorer.
    std::vector<ReleaseResult> release_all();

    // Final release at Orca exit with a short per-item timeout (a daemon that does not answer in time keeps its lease in
    // the state file, for the restorer): release_all(), then the state file is removed when nothing is left in it. An
    // offload or release that is still running keeps the lock; after `lock_wait` the exit goes on without it and the
    // restorer finishes the job.
    std::vector<ReleaseResult> close(std::chrono::milliseconds per_item = std::chrono::milliseconds(3000),
                                     std::chrono::milliseconds lock_wait = std::chrono::milliseconds(2000));

    std::size_t held_count() const;
    bool        holds_leases() const { return held_count() != 0; }

private:
    bool persist_locked(std::string* error);

    std::vector<ReleaseResult> release_with(std::chrono::milliseconds per_item);

    std::timed_mutex         m_op_mutex; // one control operation at a time; the daemon side serializes them too
    mutable std::mutex       m_mutex;    // guards m_held and the state file; never held across a socket call
    Config                   m_config;
    std::vector<RestoreItem> m_held;
};

// Random version-4 UUID text.
std::string new_lease_id();

// The coordinator of this Orca process: its pid and start time, the state file under state_dir() and the systemd-run
// restorer. Constructing it does not touch the daemons.
std::shared_ptr<Coordinator> make_default_coordinator();

// ---------------------------------------------------------------------------------------------------------------------
// Restorer (Orca binary in --vram-restore-watch mode)
// ---------------------------------------------------------------------------------------------------------------------

// Sends `restore` for every item of `state`; items the daemon confirms (or whose lease is gone) are removed from it.
// Returns the items that are still unconfirmed.
std::vector<RestoreItem> restore_items(const RestoreState& state, const Timeouts& timeouts, int attempts = 3,
                                       std::chrono::milliseconds retry_delay = std::chrono::milliseconds(1000));

// Blocks until the process `orca_pid` with start time `orca_start` is gone (immediately when it is not that process).
void wait_for_process_exit(pid_t orca_pid, unsigned long long orca_start);

// The --vram-restore-watch entry: wait for the holder in `state_file` to exit, send `restore` for every item the file
// holds at that moment, delete the file. Returns the process exit code (0 when every restore was confirmed).
int run_restore_watch(const std::filesystem::path& state_file);

// Restores and deletes the state files of Orca instances that are gone (crash with the restorer lost too, e.g. OOM
// killed). Returns the number of files processed.
int restore_orphans(const std::filesystem::path& dir);

// Starts `systemd-run --user --collect --unit=orca-vram-restore-<pid> --property=MemoryMax=64M <exe> --vram-restore-watch
// <state file>`, or does nothing when the unit already runs. False when systemd-run is missing or fails.
bool ensure_restorer_unit(pid_t orca_pid, const std::filesystem::path& state_file);
// systemd-run on PATH and a user runtime dir: the precondition for offering offload at all.
bool restorer_available();

// ---------------------------------------------------------------------------------------------------------------------
// Preview flow: when the toolpath upload waits, and when the leases go back
// ---------------------------------------------------------------------------------------------------------------------

// `Always` is an X11 macro that the GUI translation units see, hence AlwaysLend.
enum class Policy { Ask, AlwaysLend, Never };
Policy      policy_from_string(const std::string& value); // anything unknown is Ask
const char* policy_to_string(Policy policy);

class PreviewFlow
{
public:
    using Clock = std::chrono::steady_clock;

    explicit PreviewFlow(std::chrono::seconds grace = std::chrono::seconds(60)) : m_grace(grace) {}

    enum class OnEnter {
        Nothing,      // inert: the feature is off
        KeepLeases,   // leases are held; the grace timer was cancelled
        Probe,        // decide whether VRAM is short; the toolpath upload waits for the outcome
        AwaitRelease  // the leases are being restored right now: the upload waits for that, then the caller probes again
    };
    // `feature_active`: policy is not never and the platform supports it.
    OnEnter enter_preview(bool feature_active);
    // Starts the grace timer when leases are held.
    void leave_preview(Clock::time_point now);

    // True while the toolpath GPU upload has to wait: the probe runs, the dialog is open or the offload is in flight.
    bool upload_deferred() const { return m_phase != Phase::Idle; }
    // The probe found nothing to offer (VRAM is enough, no consumer): upload at once. Returns true when it ends a deferral.
    bool no_offer();
    // The dialog or the policy leads to offloading `n > 0` consumers.
    void offload_started();
    // The offload replied: upload now. `leased` is whether any daemon holds a lease. When the preview was left in the
    // meantime, the grace period starts now.
    bool offload_finished(bool leased, Clock::time_point now);
    // "Not now": upload at once.
    bool declined();

    // The grace period ran out and the leases are on their way back (a worker is sending `restore`).
    void release_started() { m_releasing = true; }
    // The release ended: `all_released` when no lease is left. Otherwise the unconfirmed ones stay held.
    void release_finished(bool all_released)
    {
        m_releasing = false;
        if (all_released)
            m_leased = false;
        m_left_at.reset();
    }
    bool releasing() const { return m_releasing; }

    // True once the preview has been left continuously for the whole grace period while leases are held.
    bool restore_due(Clock::time_point now) const;
    std::optional<Clock::time_point> restore_deadline() const;
    bool in_preview() const { return m_in_preview; }
    bool leased() const { return m_leased; }

private:
    enum class Phase { Idle, Probing, Offloading };

    std::chrono::seconds                m_grace;
    Phase                               m_phase{Phase::Idle};
    bool                                m_in_preview{false};
    bool                                m_leased{false};
    bool                                m_releasing{false};
    std::optional<Clock::time_point>    m_left_at;
};

}} // namespace Slic3r::VramOffload

#endif // __linux__

#endif // slic3r_Utils_VramOffload_hpp_
