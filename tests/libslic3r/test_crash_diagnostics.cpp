/**
 * @file  Tests of Slic3r::log_current_exception() and Slic3r::install_crash_log_handlers().
 *
 * The crash tests fork a child that installs the handlers and then really faults, because a signal handler
 * can only be proven by a process dying from it; the parent reads the log file the child wrote.
 *
 * Assumptions: Catch2 installs its own fatal-signal handlers in the test process, so every child resets them to
 * the default action first and the handlers under test see the same starting point as in the application.
 */
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/CrashDiagnostics.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/core/null_deleter.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/trivial.hpp>
#include <boost/make_shared.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Slic3r;

namespace {

class LogCapture
{
    using Sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;

public:
    LogCapture()
    {
        auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
        backend->add_stream(boost::shared_ptr<std::ostream>(&m_stream, boost::null_deleter()));
        backend->auto_flush(true);
        m_sink = boost::make_shared<Sink>(backend);
        m_sink->set_formatter(boost::log::expressions::stream << "[" << boost::log::trivial::severity << "]"
                                                              << boost::log::expressions::smessage);
        boost::log::core::get()->add_sink(m_sink);
    }
    ~LogCapture() { boost::log::core::get()->remove_sink(m_sink); }
    std::string text() const { return m_stream.str(); }

private:
    std::ostringstream m_stream;
    boost::shared_ptr<Sink> m_sink;
};

struct LayerSliceFailure : std::logic_error
{
    using std::logic_error::logic_error;
};

} // namespace

TEST_CASE("log_current_exception logs the type and what() of a std::exception at error level", "[CrashDiagnostics]")
{
    LogCapture capture;
    try {
        throw std::runtime_error("layer 12 has no closed contour");
    } catch (...) {
        log_current_exception("call_process");
    }
    const std::string text = capture.text();
    CHECK(text.find("[error]") != std::string::npos);
    CHECK(text.find("call_process") != std::string::npos);
    CHECK(text.find("std::runtime_error") != std::string::npos);
    CHECK(text.find("layer 12 has no closed contour") != std::string::npos);
}

TEST_CASE("log_current_exception names the dynamic type of a derived exception", "[CrashDiagnostics]")
{
    LogCapture capture;
    try {
        throw Slic3r::SlicingError("object 3 cannot be sliced", 3);
    } catch (...) {
        log_current_exception("call_process");
    }
    try {
        throw LayerSliceFailure("support generation failed");
    } catch (...) {
        log_current_exception("call_process");
    }
    const std::string text = capture.text();
    CHECK(text.find("Slic3r::SlicingError: object 3 cannot be sliced") != std::string::npos);
    CHECK(text.find("LayerSliceFailure: support generation failed") != std::string::npos);
}

TEST_CASE("log_current_exception reports an exception that is not a std::exception", "[CrashDiagnostics]")
{
    LogCapture capture;
    try {
        throw 42;
    } catch (...) {
        log_current_exception("call_process");
    }
    const std::string text = capture.text();
    CHECK(text.find("[error]") != std::string::npos);
    CHECK(text.find("not a std::exception") != std::string::npos);
}

#if defined(__linux__)

namespace {

struct TempDir
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("crash-diag-%%%%-%%%%");
    TempDir() { boost::filesystem::create_directories(path); }
    ~TempDir() { boost::filesystem::remove_all(path); }
};

std::string read_file(const boost::filesystem::path &path)
{
    std::ifstream in(path.string(), std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

size_t count_of(const std::string &text, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size()))
        ++count;
    return count;
}

void reset_fatal_signals_to_default()
{
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT})
        ::signal(sig, SIG_DFL);
}

// The address goes through a volatile pointer variable so the compiler cannot see a constant null-page store
// (-Warray-bounds) and the store is really executed.
void store_to_address(uintptr_t address)
{
    volatile int *volatile target = reinterpret_cast<volatile int *>(address);
    *target = 1;
}

// Runs `body` in a forked child and returns its wait status. A body that returns exits with 99, so a test
// can tell "did not crash" from "died from the expected signal".
int run_in_child(const std::function<void()> &body)
{
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        reset_fatal_signals_to_default();
        // An exception or a failed assertion must not unwind into Catch2 and run the rest of the suite twice.
        try {
            body();
        } catch (...) {
            ::_exit(98);
        }
        ::_exit(99);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    return status;
}

void require_died_from(int status, int sig)
{
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == sig);
}

const char *name_of(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    default:      return "SIGABRT";
    }
}

void check_report_is_complete(const std::string &log)
{
    INFO("log file written by the crashed child:\n" << log);
    CHECK(log.find("[fatal]") != std::string::npos);
    CHECK(log.find("backtrace of the crashing thread") != std::string::npos);
    CHECK(log.find("[0x") != std::string::npos); // at least one frame line from backtrace_symbols_fd
    CHECK(log.find("end of crash report") != std::string::npos);
}

} // namespace

TEST_CASE("fatal signals leave signal, thread and backtrace in the log and keep the default exit status", "[CrashDiagnostics]")
{
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        DYNAMIC_SECTION(name_of(sig))
        {
            TempDir dir;
            const auto log_path = dir.path / "app.log.0";
            const int status = run_in_child([&] {
                ::prctl(PR_SET_NAME, "crash_test_thr");
                install_crash_log_handlers(log_path.string());
                ::raise(sig);
            });
            require_died_from(status, sig);

            const std::string log = read_file(log_path);
            CHECK(log.find(std::string("fatal signal ") + std::to_string(sig) + " (" + name_of(sig) + ")") != std::string::npos);
            CHECK(log.find("\"crash_test_thr\"") != std::string::npos);
            check_report_is_complete(log);
            CHECK(count_of(log, "end of crash report") == 1);
        }
    }
}

TEST_CASE("a hardware fault reports the faulting address", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        store_to_address(0x10);
    });
    require_died_from(status, SIGSEGV);

    const std::string log = read_file(log_path);
    CHECK(log.find("fatal signal 11 (SIGSEGV), fault address 0x10") != std::string::npos);
    check_report_is_complete(log);
}

TEST_CASE("a fault in a library thread names that thread", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        std::thread receiver([] {
            ::prctl(PR_SET_NAME, "MQTTAsync_rcv");
            store_to_address(0x8);
        });
        receiver.join();
    });
    require_died_from(status, SIGSEGV);

    const std::string log = read_file(log_path);
    CHECK(log.find("\"MQTTAsync_rcv\"") != std::string::npos);
    CHECK(log.find("fault address 0x8") != std::string::npos);
    check_report_is_complete(log);
}

TEST_CASE("std::terminate with an active exception reports its type and what() once", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        // Leaving a noexcept function with an exception ends in std::terminate, as an uncaught exception does.
        []() noexcept { std::rethrow_exception(std::make_exception_ptr(std::runtime_error("mesh is corrupt"))); }();
    });
    require_died_from(status, SIGABRT);

    const std::string log = read_file(log_path);
    CHECK(log.find("std::terminate called with an active exception of type") != std::string::npos);
    CHECK(log.find("what(): mesh is corrupt") != std::string::npos);
    check_report_is_complete(log);
    // terminate() ends in abort(): the SIGABRT that follows must not write a second report.
    CHECK(count_of(log, "end of crash report") == 1);
}

TEST_CASE("std::terminate without an active exception is reported", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        std::terminate();
    });
    require_died_from(status, SIGABRT);

    const std::string log = read_file(log_path);
    CHECK(log.find("std::terminate called without an active exception") != std::string::npos);
    check_report_is_complete(log);
}

TEST_CASE("a handler installed earlier still runs after the report", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path    = dir.path / "app.log.0";
    const auto marker_path = (dir.path / "previous-handler-ran").string();
    static int marker_fd   = -1;
    const int status       = run_in_child([&] {
        marker_fd = ::open(marker_path.c_str(), O_WRONLY | O_CREAT, 0644);
        struct sigaction previous = {};
        previous.sa_handler       = [](int) { [[maybe_unused]] auto written = ::write(marker_fd, "ran", 3); };
        ::sigemptyset(&previous.sa_mask);
        ::sigaction(SIGSEGV, &previous, nullptr);

        install_crash_log_handlers(log_path.string());
        ::raise(SIGSEGV);
    });
    // The previous handler returned, so the process still ends with the default action of the signal.
    require_died_from(status, SIGSEGV);

    CHECK(read_file(marker_path) == "ran");
    check_report_is_complete(read_file(log_path));
}

TEST_CASE("a slow previous handler does not get the process SIGKILLed during the default action", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        struct sigaction previous = {};
        // Longer than the backtrace watchdog: a timer that stays armed would end the process with SIGKILL.
        previous.sa_handler = [](int) { ::sleep(7); };
        ::sigemptyset(&previous.sa_mask);
        ::sigaction(SIGSEGV, &previous, nullptr);

        install_crash_log_handlers(log_path.string());
        ::raise(SIGSEGV);
    });
    require_died_from(status, SIGSEGV);
    check_report_is_complete(read_file(log_path));
}

TEST_CASE("two threads faulting together leave one complete report", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        std::atomic<int> arrived{0};
        auto fault = [&arrived] {
            ++arrived;
            while (arrived.load() < 2)
                std::this_thread::yield();
            ::raise(SIGSEGV);
        };
        std::thread other(fault);
        fault();
        other.join();
    });
    require_died_from(status, SIGSEGV);

    const std::string log = read_file(log_path);
    check_report_is_complete(log);
    CHECK(count_of(log, "end of crash report") == 1);
}

TEST_CASE("records of the boost log sink do not overwrite what another descriptor appended to the file", "[CrashDiagnostics]")
{
    TempDir dir;
    const int status = run_in_child([&] {
        set_data_dir(dir.path.string());
        set_log_path_and_level("append.log", 5);
        BOOST_LOG_TRIVIAL(info) << "first record";
        flush_logs();
        const int fd = ::open((dir.path / "log" / "append.log.0").c_str(), O_WRONLY | O_APPEND);
        [[maybe_unused]] const auto written = ::write(fd, "[fatal] appended elsewhere\n", 27);
        ::close(fd);
        BOOST_LOG_TRIVIAL(info) << "second record";
        flush_logs();
        ::_exit(0);
    });
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    const std::string log = read_file(dir.path / "log" / "append.log.0");
    CHECK(log.find("first record") != std::string::npos);
    CHECK(log.find("[fatal] appended elsewhere") != std::string::npos);
    CHECK(log.find("second record") != std::string::npos);
}

TEST_CASE("installing the handlers writes nothing and does not change a normal exit", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        ::_exit(0);
    });
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    CHECK(boost::filesystem::exists(log_path));
    CHECK(read_file(log_path).empty());
}

TEST_CASE("the report starts on its own line when the log ends in the middle of a record", "[CrashDiagnostics]")
{
    TempDir dir;
    const auto log_path = dir.path / "app.log.0";
    {
        std::ofstream partial(log_path.string(), std::ios::binary);
        partial << "[info]\t2026-10-26 18:58:15.479884[Thread 0x1]:apply, has_scarf_joint_seam:false";
    }
    const int status = run_in_child([&] {
        install_crash_log_handlers(log_path.string());
        ::raise(SIGSEGV);
    });
    require_died_from(status, SIGSEGV);

    const std::string log = read_file(log_path);
    INFO("log file written by the crashed child:\n" << log);
    CHECK(log.find("has_scarf_joint_seam:false\n[fatal]\t") != std::string::npos);
    check_report_is_complete(log);
}

TEST_CASE("the report lands in the file of the application's boost log sink, after its records", "[CrashDiagnostics]")
{
    TempDir dir;
    const int status = run_in_child([&] {
        set_data_dir(dir.path.string());
        set_log_path_and_level("session.log", 5);
        BOOST_LOG_TRIVIAL(info) << "slicing started";
        flush_logs();
        install_crash_log_handlers(current_log_file_path());
        ::raise(SIGSEGV);
    });
    require_died_from(status, SIGSEGV);

    const std::string log = read_file(dir.path / "log" / "session.log.0");
    const size_t record   = log.find("slicing started");
    const size_t report   = log.find("fatal signal 11 (SIGSEGV)");
    REQUIRE(record != std::string::npos);
    REQUIRE(report != std::string::npos);
    CHECK(record < report);
    check_report_is_complete(log);
}

#endif // __linux__
