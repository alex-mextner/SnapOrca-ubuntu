#include "VramOffloadController.hpp"

#ifdef __linux__
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "VramOffloadDialog.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr std::uint64_t MIB = 1024ULL * 1024ULL;
// Releases of leases a daemon did not confirm are retried this many times after each offload; then the restorer has them.
constexpr int MAX_RELEASE_ATTEMPTS = 3;

VramOffload::Policy configured_policy()
{
    return VramOffload::policy_from_string(wxGetApp().app_config->get("vram_offload_policy"));
}

std::uint64_t configured_min_free_bytes()
{
    const std::string value = wxGetApp().app_config->get("vram_offload_min_free_mb");
    const long        mb    = value.empty() ? 1536 : std::strtol(value.c_str(), nullptr, 10);
    return mb > 0 ? static_cast<std::uint64_t>(mb) * MIB : 0;
}

std::string failure_text(const VramOffload::Reply& reply)
{
    switch (reply.failure) {
    case VramOffload::Failure::Busy: return _u8L("it is busy with a request");
    case VramOffload::Failure::Timeout: return _u8L("it did not finish in time");
    case VramOffload::Failure::Unreachable: return _u8L("it did not answer");
    default: return reply.error.empty() ? _u8L("unknown error") : reply.error;
    }
}

} // namespace

// The only way a worker thread reaches the UI thread. The controller closes it in its destructor, on the UI thread and
// under the same lock that post() holds while it queues, so a worker either queues before the close (and the queued call
// finds the mailbox closed) or sees it closed and never touches the wx application: the application outlives the
// controller, and the controller cannot finish dying while a worker is inside post().
struct VramOffloadController::Mailbox
{
    std::mutex        mutex;
    std::atomic_bool  closed{false};

    void post(const std::shared_ptr<Mailbox>& self, std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed)
            return;
        wxGetApp().CallAfter([self, fn = std::move(fn)] {
            if (!self->closed)
                fn();
        });
    }
    void close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
    }
};

struct VramOffloadController::ProbeShared
{
    std::mutex                 mutex;
    std::condition_variable    ready;
    std::optional<ProbeResult> result;
    bool                       handled{false}; // the UI thread has taken the result
};

VramOffloadController::VramOffloadController(Plater& plater)
    : m_plater(plater), m_grace_timer(this), m_mailbox(std::make_shared<Mailbox>())
{
    Bind(wxEVT_TIMER, &VramOffloadController::on_grace_timer, this);
}

VramOffloadController::~VramOffloadController()
{
    m_mailbox->close();
    m_grace_timer.Stop();
    if (m_dialog != nullptr)
        m_dialog->dismiss();
}

void VramOffloadController::on_preview_entered()
{
    // Entering cancels a running grace period: the leases are kept.
    m_grace_timer.Stop();

    const VramOffload::Policy policy = configured_policy();
    const bool                active = policy != VramOffload::Policy::Never && VramOffload::has_control_sockets(VramOffload::socket_dir()) &&
                        VramOffload::restorer_available();
    switch (m_flow.enter_preview(active)) {
    case VramOffload::PreviewFlow::OnEnter::Probe: {
        auto shared = start_probe(policy, configured_min_free_bytes());
        // A probe that answers at once (typically: enough VRAM, or no NVIDIA library) settles the matter before the
        // toolpath upload ever waits. The wait is short, so the page switch is not held up.
        {
            std::unique_lock<std::mutex> lock(shared->mutex);
            shared->ready.wait_for(lock, std::chrono::milliseconds(150), [&] { return shared->result.has_value(); });
            if (shared->result && !needs_offer(*shared->result)) {
                shared->handled = true;
                lock.unlock();
                BOOST_LOG_TRIVIAL(info) << "VRAM offload probe: free " << (shared->result->free_bytes / MIB) << " MiB, nothing to offer";
                m_flow.no_offer();
                break;
            }
        }
        // Slow probe or an offer to make: the toolpath GPU upload waits for the decision. Set before the preview loads
        // (Plater::priv::set_current_panel loads it right after this call).
        m_plater.set_toolpath_upload_deferred(true);
        break;
    }
    case VramOffload::PreviewFlow::OnEnter::AwaitRelease:
        // The leases are on their way back: the upload waits for that, then on_release_done() probes again.
        m_await_release = true;
        m_plater.set_toolpath_upload_deferred(true);
        break;
    case VramOffload::PreviewFlow::OnEnter::KeepLeases:
    case VramOffload::PreviewFlow::OnEnter::Nothing: break;
    }
    // No decision pending for this entry (nothing to offer, leases kept, feature off): a deferral left by an earlier
    // entry, whose own cleanup was skipped as stale, must not outlive it, or the toolpaths would never reach the GPU.
    if (!m_flow.upload_deferred())
        end_deferral();
    // Once the preview is shown (set_current_panel is still running): freed toolpaths that entering does not reload.
    CallAfter([this] { m_plater.rebuild_released_preview(); });
}

void VramOffloadController::on_preview_left()
{
    m_flow.leave_preview(VramOffload::PreviewFlow::Clock::now());
    if (m_dialog != nullptr) {
        // Nobody looks at the preview any more: the offer lapses like "Not now".
        m_dialog->dismiss();
        m_dialog = nullptr;
        m_flow.declined();
        // set_current_panel() hides the preview only after this call: ending the deferral now would load the toolpaths
        // onto the GPU while the preview is still shown. It ends right after, unless the preview was entered again by
        // then: that entry has its own deferral, which this stale call must not clear.
        const auto epoch = m_flow.epoch();
        CallAfter([this, epoch] {
            if (epoch == m_flow.epoch())
                end_deferral();
        });
    }
    arm_grace_timer();
}

std::shared_ptr<VramOffloadController::ProbeShared> VramOffloadController::start_probe(VramOffload::Policy policy, std::uint64_t min_free_bytes)
{
    auto mailbox = m_mailbox;
    auto shared  = std::make_shared<ProbeShared>();
    const unsigned int self_pid = static_cast<unsigned int>(::getpid());
    std::thread([this, mailbox, shared, policy, min_free_bytes, self_pid] {
        ProbeResult result;
        result.min_free_bytes = min_free_bytes;
        std::string why;
        if (const auto gpu = VramOffload::probe_gpu("libnvidia-ml.so.1", &why, self_pid)) {
            result.gpu_known  = true;
            result.free_bytes = gpu->free_bytes;
            if (gpu->free_bytes < min_free_bytes)
                result.consumers = VramOffload::find_consumers(VramOffload::socket_dir(), *gpu, std::chrono::milliseconds(2000));
        } else {
            BOOST_LOG_TRIVIAL(info) << "VRAM offload: " << why << ", the feature stays off";
        }
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->result = std::move(result);
        }
        shared->ready.notify_all();
        mailbox->post(mailbox, [this, shared, policy] {
            ProbeResult taken;
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                if (shared->handled)
                    return; // on_preview_entered() already settled it
                shared->handled = true;
                taken           = *shared->result;
            }
            on_probe_done(taken, policy);
        });

        // A crashed Orca whose restorer died too (e.g. killed by its memory limit) left leases behind.
        static std::atomic_flag orphans_pruned = ATOMIC_FLAG_INIT;
        if (!orphans_pruned.test_and_set())
            VramOffload::restore_orphans(VramOffload::state_dir());
    }).detach();
    return shared;
}

void VramOffloadController::on_probe_done(const ProbeResult& probe, VramOffload::Policy policy)
{
    BOOST_LOG_TRIVIAL(info) << "VRAM offload probe: free " << (probe.free_bytes / MIB) << " MiB, threshold " << (probe.min_free_bytes / MIB)
                            << " MiB, consumers " << probe.consumers.size();
    // Enough VRAM, no NVIDIA library, nothing that can lend, or the preview was left meanwhile: no offer.
    // Policy changed to never meanwhile (Preferences): the result is dropped, nothing is unloaded.
    const VramOffload::Policy current = configured_policy();
    if (!probe.gpu_known || probe.consumers.empty() || !m_flow.in_preview() || current == VramOffload::Policy::Never) {
        m_flow.no_offer();
        end_deferral();
        return;
    }
    if (current == VramOffload::Policy::AlwaysLend)
        start_offload(probe.consumers);
    else
        show_dialog(probe);
}

void VramOffloadController::show_dialog(const ProbeResult& probe)
{
    auto consumers = probe.consumers;
    // Called only while the controller lives: its destructor dismisses the dialog without an answer.
    m_dialog = new VramOffloadDialog(wxGetApp().mainframe, consumers, probe.free_bytes, probe.min_free_bytes,
                                     [this, consumers](const VramOffloadDialog::Result& result) {
                                         m_dialog = nullptr;
                                         // Switched off in Preferences while the dialog was open: nothing is unloaded.
                                         if (configured_policy() == VramOffload::Policy::Never) {
                                             m_flow.declined();
                                             end_deferral();
                                             return;
                                         }
                                         if (result.remember) {
                                             wxGetApp().app_config->set("vram_offload_policy", result.free_vram ? "always" : "never");
                                             wxGetApp().app_config->save();
                                         }
                                         std::vector<VramOffload::Consumer> selected;
                                         for (size_t i = 0; i < consumers.size() && i < result.selected.size(); ++i)
                                             if (result.selected[i])
                                                 selected.push_back(consumers[i]);
                                         if (!result.free_vram || selected.empty()) {
                                             m_flow.declined();
                                             end_deferral();
                                             return;
                                         }
                                         start_offload(std::move(selected));
                                     });
    m_dialog->Show();
}

void VramOffloadController::start_offload(std::vector<VramOffload::Consumer> consumers)
{
    m_flow.offload_started();
    auto mailbox     = m_mailbox;
    auto coordinator = wxGetApp().vram_offload_coordinator();
    std::thread([this, mailbox, coordinator, consumers = std::move(consumers)] {
        auto results = coordinator->offload(consumers);
        mailbox->post(mailbox, [this, results = std::move(results)] { on_offload_done(results); });
    }).detach();
}

void VramOffloadController::on_offload_done(const std::vector<VramOffload::OffloadResult>& results)
{
    bool leased = false;
    for (const VramOffload::OffloadResult& result : results) {
        if (result.leased) {
            leased = true;
            const std::uint64_t freed = result.reply.freed_bytes != 0 ? result.reply.freed_bytes : result.consumer.vram_bytes;
            notify(false, (boost::format(_u8L("Freed %1% MiB of GPU memory from %2% in %3$.1f s. It returns when you leave the preview.")) %
                           (freed / MIB) % result.consumer.name % result.reply.seconds)
                              .str());
        } else if (result.uncertain) {
            notify(true, (boost::format(_u8L("Could not free GPU memory from %1%: %2%. It did not confirm the cancellation either, so Orca keeps trying to give the request back.")) %
                          result.consumer.name % failure_text(result.reply))
                             .str());
        } else {
            notify(true, (boost::format(_u8L("Could not free GPU memory from %1%: %2%.")) % result.consumer.name % failure_text(result.reply)).str());
        }
    }
    m_release_attempts = 0;
    // A lease whose rollback was not confirmed is held all the same: it has to be given back too.
    const bool holds = leased || wxGetApp().vram_offload_coordinator()->held_count() != 0;
    m_flow.offload_finished(holds, VramOffload::PreviewFlow::Clock::now());
    end_deferral();
    // The preview was left while the offload ran: the grace period starts now.
    arm_grace_timer();
}

void VramOffloadController::arm_grace_timer()
{
    const auto deadline = m_flow.restore_deadline();
    if (!deadline)
        return;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - VramOffload::PreviewFlow::Clock::now()).count();
    m_grace_timer.StartOnce(static_cast<int>(std::max<long long>(left, 0)) + 20);
}

void VramOffloadController::on_grace_timer(wxTimerEvent&)
{
    if (m_release_running)
        return;
    if (m_flow.restore_due(VramOffload::PreviewFlow::Clock::now()))
        start_release();
    else
        arm_grace_timer(); // fired early: the deadline moved
}

void VramOffloadController::start_release()
{
    m_release_running = true;
    m_flow.release_started();
    // The hidden preview still holds its toolpath buffers, which are what the borrowed VRAM went to: the models would
    // reload into a full GPU. They go first and are rebuilt when the preview is entered again.
    if (!m_plater.release_preview_gpu_buffers())
        BOOST_LOG_TRIVIAL(warning) << "VRAM offload: the preview's GPU buffers could not be released before the restore";
    auto mailbox     = m_mailbox;
    auto coordinator = wxGetApp().vram_offload_coordinator();
    std::thread([this, mailbox, coordinator] {
        auto results = coordinator->release_all();
        mailbox->post(mailbox, [this, results = std::move(results)] { on_release_done(results); });
    }).detach();
}

void VramOffloadController::on_release_done(const std::vector<VramOffload::ReleaseResult>& results)
{
    m_release_running = false;
    for (const VramOffload::ReleaseResult& result : results) {
        if (!result.released)
            notify(true, (boost::format(_u8L("%1% did not confirm the return of its GPU memory (%2%); it is retried.")) % result.item.name %
                          failure_text(result.reply))
                             .str());
        else if (result.reply.failure == VramOffload::Failure::Oom)
            notify(true, (boost::format(_u8L("%1% could not reload to the GPU (out of memory); it reloads on its next request.")) % result.item.name).str());
        else
            notify(false, (boost::format(_u8L("GPU memory returned to %1% in %2$.1f s.")) % result.item.name % result.reply.seconds).str());
    }
    const bool all_released = wxGetApp().vram_offload_coordinator()->held_count() == 0;
    const bool any_released = std::any_of(results.begin(), results.end(), [](const VramOffload::ReleaseResult& r) { return r.released; });
    m_flow.release_finished(all_released);

    if (m_await_release) {
        // The preview was entered while the leases were on their way back: the models that came back are on the GPU
        // again, so the VRAM is judged afresh, also after a partial release (the daemons still held stay as they are).
        // The toolpath upload keeps waiting meanwhile (the flow is still deciding).
        m_await_release = false;
        if ((all_released || any_released) && m_flow.in_preview() && configured_policy() != VramOffload::Policy::Never) {
            start_probe(configured_policy(), configured_min_free_bytes());
            return;
        }
        m_flow.no_offer();
        end_deferral();
    }
    // Unconfirmed leases stay; try again after another grace period, a few times (then the restorer has them).
    if (!all_released && !m_flow.in_preview() && ++m_release_attempts < MAX_RELEASE_ATTEMPTS) {
        m_flow.leave_preview(VramOffload::PreviewFlow::Clock::now());
        arm_grace_timer();
    }
}

void VramOffloadController::end_deferral()
{
    m_plater.set_toolpath_upload_deferred(false);
}

void VramOffloadController::notify(bool warning, const std::string& text)
{
    BOOST_LOG_TRIVIAL(info) << "VRAM offload: " << text;
    if (NotificationManager* notifications = m_plater.get_notification_manager())
        notifications->push_notification(NotificationType::CustomNotification,
                                         warning ? NotificationManager::NotificationLevel::WarningNotificationLevel :
                                                   NotificationManager::NotificationLevel::RegularNotificationLevel,
                                         text);
}

}} // namespace Slic3r::GUI

#endif // __linux__
