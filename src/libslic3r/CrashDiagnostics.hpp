/**
 * @file  Makes a failed slice or a crashed process explainable from the application log.
 *
 * Accessed via: BackgroundSlicingProcess::call_process() calls log_current_exception() when slicing throws;
 *               GUI_App::on_init_inner() calls install_crash_log_handlers() right after the log file is configured.
 *
 * Assumptions: the crash report goes to the first file of the boost log sink (see current_log_file_path()) through
 *              its own descriptor, because boost::log cannot be entered from a signal handler. Lines boost still
 *              holds in its stream buffer at the moment of the crash are therefore lost (the buffer is not flushed
 *              per record, to keep logging cheap); the report itself is written with write() and fsync()ed.
 *              A log file rotated at 100 MB leaves the report in the first file of the run.
 *              The sink is opened in append mode (utils.cpp) so that a record it writes after the report never
 *              overwrites it, and a thread that loses the race for the report waits until the winner has finished it.
 *              Limits: a previous handler that recovers from a hardware fault and returns is not supported (the
 *              report is already written and the default action restored when it runs), and the old-log cleanup of
 *              another instance of the application can unlink the file of a running one, with its report.
 *              Everything in the handler is async-signal-safe except glibc's backtrace(), whose unwinder can take the
 *              dynamic loader lock; a private 5 s timer ends the process if it blocks there
 *              (it is disarmed before the default action, so a slow core dump is never cut short).
 *
 * Past bugs: v2.4.0-linux.4 aborted with "double free or corruption", threw during slicing and segfaulted in a
 *            library thread, and each time the log said nothing (https://github.com/alex-mextner/SnapOrca-ubuntu/issues/22).
 */
#pragma once

#include <string>

namespace Slic3r {

// Logs the type and what() of the exception being handled at error level, then flushes the log so the
// message survives a crash that follows. Must be called from inside a catch block. Never throws.
void log_current_exception(const char *where) noexcept;

// Linux only (no-op elsewhere): on SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT and std::terminate append the signal,
// the faulting address, the crashing thread and its backtrace to `log_file_path`, then let the default action
// proceed so the exit status (core dump / abort) is unchanged. A handler installed earlier is called after the
// report. Only the first call has an effect. The alternate signal stack (so that a stack overflow can still be reported)
// is set up for the calling thread only; an overflow on another thread is not covered.
void install_crash_log_handlers(const std::string &log_file_path);

} // namespace Slic3r
