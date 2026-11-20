#ifndef slic3r_GUI_VramOffloadController_hpp_
#define slic3r_GUI_VramOffloadController_hpp_

#ifdef __linux__

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <wx/event.h>
#include <wx/timer.h>

#include "slic3r/Utils/VramOffload.hpp"

namespace Slic3r { namespace GUI {

class Plater;
class VramOffloadDialog;

// Drives the temporary VRAM offload around the G-code preview (docs/superpowers/specs/2026-11-18-vram-offload-design.md).
//
//   entering the preview  -> NVML probe on a worker thread; when free VRAM is below `vram_offload_min_free_mb` and a
//                            daemon can lend its weights: the dialog (policy ask) or the offload at once (policy always)
//   toolpath GPU upload   -> deferred (Plater::set_toolpath_upload_deferred) from entering until the decision is settled
//   leaving the preview   -> cancellable 60 s grace timer, then `restore` for every lease; re-entering cancels it
//   Orca exit             -> GUI_App::OnExit closes the coordinator; the crash-safe restorer covers a crash
//
// Every socket call runs on a worker thread; results come back through CallAfter, so the UI never waits for a daemon.
class VramOffloadController : public wxEvtHandler
{
public:
    explicit VramOffloadController(Plater& plater);
    ~VramOffloadController() override;

    // Called by Plater::priv::set_current_panel on the transitions into and out of the preview.
    void on_preview_entered();
    void on_preview_left();

private:
    struct ProbeResult
    {
        bool                              gpu_known{false};
        std::uint64_t                     free_bytes{0};
        std::uint64_t                     min_free_bytes{0};
        std::vector<VramOffload::Consumer> consumers;
    };

    struct ProbeShared; // worker thread -> UI thread hand-over of the probe result
    static bool needs_offer(const ProbeResult& probe) { return probe.gpu_known && !probe.consumers.empty(); }

    std::shared_ptr<ProbeShared> start_probe(VramOffload::Policy policy, std::uint64_t min_free_bytes);
    void on_probe_done(const ProbeResult& probe, VramOffload::Policy policy);
    void show_dialog(const ProbeResult& probe);
    void start_offload(std::vector<VramOffload::Consumer> consumers);
    void on_offload_done(const std::vector<VramOffload::OffloadResult>& results);
    void start_release();
    void on_release_done(const std::vector<VramOffload::ReleaseResult>& results);
    void on_grace_timer(wxTimerEvent& event);
    void arm_grace_timer();
    // The decision is settled: the toolpaths may reach the GPU.
    void end_deferral();
    void notify(bool warning, const std::string& text);

    Plater&                      m_plater;
    VramOffload::PreviewFlow     m_flow;
    wxTimer                      m_grace_timer;
    VramOffloadDialog*           m_dialog{nullptr};
    bool                         m_release_running{false};
    // The preview was entered while the leases were being restored: probe again when that is done.
    bool                         m_await_release{false};
    int                          m_release_attempts{0};
    // Cleared by the destructor: results of worker threads that arrive afterwards are dropped.
    std::shared_ptr<std::atomic_bool> m_alive{std::make_shared<std::atomic_bool>(true)};
};

}} // namespace Slic3r::GUI

#endif // __linux__

#endif // slic3r_GUI_VramOffloadController_hpp_
