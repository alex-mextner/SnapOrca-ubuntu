// See CrashDiagnostics.hpp for what this file is for and which limits the crash report has.
#include "CrashDiagnostics.hpp"
#include "Utils.hpp"

#include <exception>
#include <typeinfo>

#include <boost/core/demangle.hpp>
#include <boost/log/trivial.hpp>

#if defined(__linux__)
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <signal.h>
#include <cstddef>
#include <cstring>
#include <ctime>

#include <cxxabi.h>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Slic3r {

void log_current_exception(const char *where) noexcept
{
    try {
        try {
            throw;
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << where << ": caught " << boost::core::demangle(typeid(e).name()) << ": " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << where << ": caught an exception that is not a std::exception";
        }
        flush_logs();
    } catch (...) {
        // Logging must not turn a handled slicing error into a second failure.
    }
}

#if defined(__linux__)

namespace {

constexpr int    k_fatal_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
constexpr size_t k_fatal_signal_count = sizeof(k_fatal_signals) / sizeof(k_fatal_signals[0]);
constexpr int    k_max_backtrace_frames = 64;
constexpr unsigned k_backtrace_watchdog_seconds = 5;

// Everything below runs in a signal handler or in the terminate handler of a possibly corrupted process:
// plain statics, no allocation, no locks, no boost::log.
int                     g_log_fd = -1;
struct sigaction        g_previous_actions[k_fatal_signal_count];
std::terminate_handler  g_previous_terminate = nullptr;
std::atomic_flag        g_report_written = ATOMIC_FLAG_INIT;
// Thread id of the thread that claimed the report; 0 until then.
std::atomic<long>       g_report_owner{0};
// Set once the winning thread's report is complete; losing threads wait for it before the default action ends the process.
std::atomic<bool>       g_report_finished{false};
// Ends the process if the backtrace blocks; created by install_crash_log_handlers(), armed by the handler.
timer_t                 g_watchdog;
bool                    g_watchdog_ready = false;
// Lets the handler run when the fault is a stack overflow of the thread that installed it.
alignas(16) char        g_alternate_stack[64 * 1024];

// Fixed-size text builder written to the log with a single write(), so a report line is never interleaved.
class ReportBuffer
{
public:
    void append(const char *text)
    {
        while (*text != '\0' && m_length < sizeof(m_data))
            m_data[m_length++] = *text++;
    }
    void append_char_array(const char *text, size_t max_length)
    {
        for (size_t i = 0; i < max_length && text[i] != '\0' && m_length < sizeof(m_data); ++i)
            m_data[m_length++] = text[i];
    }
    // Terminates the line even when a long what() filled the buffer: the last character is sacrificed.
    void end_line()
    {
        if (m_length == sizeof(m_data))
            --m_length;
        m_data[m_length++] = '\n';
    }
    void append_unsigned(unsigned long long value, size_t min_digits = 1)
    {
        char digits[24];
        size_t count = 0;
        do {
            digits[count++] = char('0' + value % 10);
            value /= 10;
        } while (value != 0);
        for (size_t pad = count; pad < min_digits; ++pad)
            append("0");
        while (count > 0 && m_length < sizeof(m_data))
            m_data[m_length++] = digits[--count];
    }
    void append_signed(long long value)
    {
        if (value < 0) {
            append("-");
            append_unsigned(0ULL - static_cast<unsigned long long>(value));
        } else {
            append_unsigned(static_cast<unsigned long long>(value));
        }
    }
    void append_hex(unsigned long long value)
    {
        append("0x");
        char digits[16];
        size_t count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 0xf];
            value >>= 4;
        } while (value != 0);
        while (count > 0 && m_length < sizeof(m_data))
            m_data[m_length++] = digits[--count];
    }
    void write_to(int fd) const
    {
        const char *cursor = m_data;
        size_t remaining = m_length;
        while (remaining > 0) {
            const ssize_t written = ::write(fd, cursor, remaining);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                return;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
    }

private:
    char   m_data[2048];
    size_t m_length = 0;
};

const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    case SIGABRT: return "SIGABRT";
    default:      return "unknown signal";
    }
}

bool signal_carries_fault_address(int sig)
{
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGFPE || sig == SIGILL;
}

// Appends "YYYY-MM-DD HH:MM:SS UTC" from the calendar algorithm of days_from_civil, which needs no libc time zone
// state (localtime() takes locks and is not async-signal-safe).
void append_utc_timestamp(ReportBuffer &out, long long epoch_seconds)
{
    const long long days = epoch_seconds >= 0 ? epoch_seconds / 86400 : -((-epoch_seconds + 86399) / 86400);
    const long long seconds_of_day = epoch_seconds - days * 86400;
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long day_of_era = z - era * 146097;
    const long long year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const long long day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const long long month_index = (5 * day_of_year + 2) / 153;
    const long long day = day_of_year - (153 * month_index + 2) / 5 + 1;
    const long long month = month_index < 10 ? month_index + 3 : month_index - 9;
    const long long year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);

    out.append_unsigned(static_cast<unsigned long long>(year), 4);
    out.append("-");
    out.append_unsigned(static_cast<unsigned long long>(month), 2);
    out.append("-");
    out.append_unsigned(static_cast<unsigned long long>(day), 2);
    out.append(" ");
    out.append_unsigned(static_cast<unsigned long long>(seconds_of_day / 3600), 2);
    out.append(":");
    out.append_unsigned(static_cast<unsigned long long>(seconds_of_day / 60 % 60), 2);
    out.append(":");
    out.append_unsigned(static_cast<unsigned long long>(seconds_of_day % 60), 2);
    out.append(" UTC");
}

// Starts a line shaped like the boost sink's records: "[fatal]<TAB>time[Thread tid "name"]:". It begins with a newline
// because boost flushes its buffer at block boundaries and the file can end in the middle of a record.
void begin_report_line(ReportBuffer &out)
{
    char thread_name[17] = {};
    ::prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(thread_name), 0, 0, 0);
    struct timespec now = {};
    ::clock_gettime(CLOCK_REALTIME, &now);

    out.append("\n[fatal]\t");
    append_utc_timestamp(out, static_cast<long long>(now.tv_sec));
    out.append("[Thread tid ");
    out.append_signed(static_cast<long long>(::syscall(SYS_gettid)));
    out.append(" \"");
    out.append_char_array(thread_name, sizeof(thread_name) - 1);
    out.append("\"]:");
}

void set_watchdog(time_t seconds)
{
    if (!g_watchdog_ready)
        return;
    struct itimerspec grace;
    std::memset(&grace, 0, sizeof(grace));
    grace.it_value.tv_sec = seconds; // zero disarms
    ::timer_settime(g_watchdog, 0, &grace, nullptr);
}

void write_backtrace_and_sync()
{
    ReportBuffer header;
    header.append("[fatal]\tbacktrace of the crashing thread, innermost frame first (the first frames belong to the crash handler):\n");
    header.write_to(g_log_fd);

    // backtrace() is not formally async-signal-safe: its unwinder takes the dynamic loader lock, which the faulting
    // thread may hold (a crash inside dlopen or a lazy symbol binding). The signal line above is already in the log;
    // if the unwinder blocks, the private timer created at installation kills the process after the grace period
    // instead of leaving a hung window that the user has to close by hand. It does not touch SIGALRM, alarm() or
    // any timer of the application.
    set_watchdog(k_backtrace_watchdog_seconds);
    void *frames[k_max_backtrace_frames];
    const int frame_count = ::backtrace(frames, k_max_backtrace_frames);
    ::backtrace_symbols_fd(frames, frame_count, g_log_fd);
    // The watchdog guards the unwinder only: a core dump of a large process can take longer than the grace period,
    // and SIGKILL would truncate it and change the exit status.
    set_watchdog(0);

    ReportBuffer footer;
    footer.append("[fatal]\tend of crash report; the process now terminates with the default action\n");
    footer.write_to(g_log_fd);
    ::fsync(g_log_fd);
    g_report_finished.store(true);
}

// True for the one thread that reports this process's fatal event. Only the first event is reported: a terminate()
// ends in abort(), and several threads can fault together.
bool claim_report()
{
    if (g_report_written.test_and_set())
        return false;
    g_report_owner.store(::syscall(SYS_gettid));
    return true;
}

// A thread that lost the race for the report must not take the default action while the winner is still writing:
// the default action ends every thread of the process. Bounded, in case the winner died inside its handler. The
// winner itself never waits: a fault inside its own report (a damaged heap under backtrace()) nests on its thread.
void wait_for_report()
{
    if (g_report_owner.load() == ::syscall(SYS_gettid))
        return;
    struct timespec pause = {0, 5 * 1000 * 1000};
    for (unsigned waited_ms = 0; !g_report_finished.load() && waited_ms < (k_backtrace_watchdog_seconds + 1) * 1000; waited_ms += 5)
        ::nanosleep(&pause, nullptr);
}

void fatal_signal_handler(int sig, siginfo_t *info, void *context)
{
    const int saved_errno = errno;
    const bool raised_by_kernel = info != nullptr && info->si_code > 0;

    if (!claim_report()) {
        wait_for_report();
    } else {
        ReportBuffer line;
        begin_report_line(line);
        line.append("fatal signal ");
        line.append_signed(sig);
        line.append(" (");
        line.append(signal_name(sig));
        line.append(")");
        if (info == nullptr) {
            line.append(", no signal information");
        } else if (raised_by_kernel && signal_carries_fault_address(sig)) {
            line.append(", fault address ");
            line.append_hex(reinterpret_cast<unsigned long long>(info->si_addr));
        } else {
            line.append(", no fault address (raised by pid ");
            line.append_signed(info->si_pid);
            line.append(")");
        }
        line.end_line();
        line.write_to(g_log_fd);
        write_backtrace_and_sync();
    }

    size_t index = 0;
    while (index < k_fatal_signal_count && k_fatal_signals[index] != sig)
        ++index;

    // From here on the process dies from this signal with the default action, after the handler that was
    // installed before ours, if any, has seen it.
    struct sigaction default_action;
    std::memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    ::sigemptyset(&default_action.sa_mask);
    ::sigaction(sig, &default_action, nullptr);
    sigset_t unblock;
    ::sigemptyset(&unblock);
    ::sigaddset(&unblock, sig);
    ::pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

    if (index < k_fatal_signal_count) {
        const struct sigaction &previous = g_previous_actions[index];
        // sa_handler and sa_sigaction share storage; the kernel keeps SA_SIGINFO on a saved SIG_DFL or SIG_IGN.
        if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            if (previous.sa_flags & SA_SIGINFO)
                previous.sa_sigaction(sig, info, context);
            else
                previous.sa_handler(sig);
        }
    }

    errno = saved_errno;
    // A hardware fault re-executes the faulting instruction when the handler returns and dies with the real
    // fault information; a signal sent by abort() or kill() has to be delivered again.
    if (!raised_by_kernel)
        ::raise(sig);
}

[[noreturn]] void terminate_handler()
{
    if (claim_report()) {
        ReportBuffer line;
        begin_report_line(line);
        line.append("std::terminate called");
        if (abi::__cxa_current_exception_type() != nullptr) {
            // The mangled type name is kept as is: demangling allocates, and terminate() may run on a damaged heap.
            try {
                throw;
            } catch (const std::exception &e) {
                line.append(" with an active exception of type ");
                line.append(typeid(e).name());
                line.append(", what(): ");
                line.append(e.what());
            } catch (...) {
                line.append(" with an active exception that is not a std::exception");
            }
        } else {
            line.append(" without an active exception");
        }
        line.end_line();
        line.write_to(g_log_fd);
        write_backtrace_and_sync();
    }
    if (g_previous_terminate != nullptr)
        g_previous_terminate();
    std::abort();
}

} // namespace

void install_crash_log_handlers(const std::string &log_file_path)
{
    // Set only once the handlers are in place, so a failed open() (descriptors exhausted at startup) can be retried.
    static std::atomic<bool> installed{false};
    if (installed.load())
        return;

    g_log_fd = ::open(log_file_path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (g_log_fd < 0) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": cannot open " << log_file_path << " for crash reports, errno " << errno;
        return;
    }

    // backtrace() loads libgcc lazily on its first call, which allocates: do it now, not inside the handler.
    void *warm_up[1];
    ::backtrace(warm_up, 1);

    // The watchdog of the backtrace (see write_backtrace_and_sync): a private timer whose expiry kills the process,
    // created here because timer_create() is not for a signal handler; the handler only arms it.
    struct sigevent expiry;
    std::memset(&expiry, 0, sizeof(expiry));
    expiry.sigev_notify = SIGEV_SIGNAL;
    expiry.sigev_signo  = SIGKILL;
    g_watchdog_ready    = ::timer_create(CLOCK_MONOTONIC, &expiry, &g_watchdog) == 0;
    if (!g_watchdog_ready)
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": no watchdog for the crash backtrace, errno " << errno;

    stack_t current_stack;
    if (::sigaltstack(nullptr, &current_stack) == 0 && (current_stack.ss_flags & SS_DISABLE) != 0) {
        stack_t stack;
        std::memset(&stack, 0, sizeof(stack));
        stack.ss_sp = g_alternate_stack;
        stack.ss_size = sizeof(g_alternate_stack);
        if (::sigaltstack(&stack, nullptr) != 0)
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": no alternate signal stack, a stack overflow will not be reported, errno " << errno;
    }

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = &fatal_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    // Blocked while the handler runs, so a fault inside the report path (a damaged heap under backtrace()) is ended by
    // the kernel at once with its default action instead of nesting.
    ::sigemptyset(&action.sa_mask);
    for (int fatal : k_fatal_signals)
        ::sigaddset(&action.sa_mask, fatal);
    for (size_t i = 0; i < k_fatal_signal_count; ++i) {
        if (::sigaction(k_fatal_signals[i], &action, &g_previous_actions[i]) != 0) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": no crash report for " << signal_name(k_fatal_signals[i]) << ", errno " << errno;
            // Nothing to chain to and nothing was replaced: the default action stays.
            std::memset(&g_previous_actions[i], 0, sizeof(g_previous_actions[i]));
            g_previous_actions[i].sa_handler = SIG_DFL;
        }
    }

    g_previous_terminate = std::set_terminate(&terminate_handler);
    installed.store(true);
}

#else // __linux__

void install_crash_log_handlers(const std::string &)
{
}

#endif // __linux__

} // namespace Slic3r
