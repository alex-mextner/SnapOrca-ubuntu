#ifndef slic3r_GUI_VramOffloadDialog_hpp_
#define slic3r_GUI_VramOffloadDialog_hpp_

#ifdef __linux__

#include <cstdint>
#include <functional>
#include <vector>

#include <wx/stattext.h>

#include "GUI_Utils.hpp"
#include "Widgets/CheckBox.hpp"
#include "slic3r/Utils/VramOffload.hpp"

namespace Slic3r { namespace GUI {

// Non-modal offer to lend VRAM from GPU daemons while the G-code preview runs (docs/superpowers/specs/2026-11-18-vram-offload-design.md,
// "Orca dialog UX"). The rest of the UI never waits for it: the answer arrives through the callback.
class VramOffloadDialog : public DPIDialog
{
public:
    struct Result
    {
        bool              free_vram{false}; // "Free VRAM", else "Not now" (also the close box)
        std::vector<bool> selected;         // per consumer, in the order given to the dialog
        bool              remember{false};  // "Remember my choice"
    };
    using Callback = std::function<void(const Result&)>;

    VramOffloadDialog(wxWindow* parent, const std::vector<VramOffload::Consumer>& consumers, std::uint64_t free_bytes, std::uint64_t min_free_bytes, Callback on_result);

    // Closes the dialog without an answer (the preview was left while it was open).
    void dismiss();

    // MiB with the unit, e.g. "2224 MiB".
    static wxString format_mib(std::uint64_t bytes);
    // "restore ~0.4 s" from the daemon's last measured restore, else the estimate of the design table for the known
    // daemons, else empty.
    static wxString restore_estimate(const VramOffload::Consumer& consumer);
    // What the user loses while the daemon's GPU weights are away.
    static wxString side_effect(const VramOffload::Consumer& consumer);

private:
    void answer(bool free_vram);
    void on_dpi_changed(const wxRect&) override {}

    Callback                  m_on_result;
    std::vector<CheckBox*>    m_consumer_boxes;
    CheckBox*                 m_remember{nullptr};
    bool                      m_answered{false};
};

}} // namespace Slic3r::GUI

#endif // __linux__

#endif // slic3r_GUI_VramOffloadDialog_hpp_
