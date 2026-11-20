#include "VramOffloadDialog.hpp"

#ifdef __linux__

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

wxString VramOffloadDialog::format_mib(std::uint64_t bytes)
{
    return wxString::Format(_L("%llu MiB"), static_cast<unsigned long long>((bytes + (512ULL << 10)) >> 20));
}

wxString VramOffloadDialog::restore_estimate(const VramOffload::Consumer& consumer)
{
    if (consumer.last_restore_s)
        return wxString::Format(_L("restore ~%.1f s"), *consumer.last_restore_s);
    // Estimates of the design table until the daemons have measured a restore themselves.
    if (consumer.name == "wyoming-whisper")
        return _L("restore ~0.2-0.6 s");
    if (consumer.name == "task-classify")
        return _L("restore ~0.2-0.5 s");
    return wxString();
}

wxString VramOffloadDialog::side_effect(const VramOffload::Consumer& consumer)
{
    if (consumer.name == "wyoming-whisper")
        return _L("voice recognition pauses");
    if (consumer.name == "task-classify")
        return _L("classification runs on the CPU (~3 s)");
    return _L("it may pause or slow down meanwhile");
}

VramOffloadDialog::VramOffloadDialog(wxWindow* parent, const std::vector<VramOffload::Consumer>& consumers, std::uint64_t free_bytes,
                                     std::uint64_t min_free_bytes, Callback on_result)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Free GPU memory for the preview?"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_on_result(std::move(on_result))
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY,
                                   wxString::Format(_L("Only %s of GPU memory are free and the G-code preview needs at least %s. "
                                                       "These programs can lend theirs while you look at the preview; they get it back shortly after you leave it."),
                                                    format_mib(free_bytes), format_mib(min_free_bytes)));
    intro->Wrap(FromDIP(440));
    sizer->Add(intro, 0, wxEXPAND | wxALL, FromDIP(12));

    for (const VramOffload::Consumer& consumer : consumers) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* box = new ::CheckBox(this);
        box->SetValue(true);
        m_consumer_boxes.push_back(box);
        row->Add(box, 0, wxALIGN_TOP | wxTOP | wxRIGHT, FromDIP(4));

        wxString headline = wxString::FromUTF8(consumer.name) + ": " + format_mib(consumer.vram_bytes);
        const wxString estimate = restore_estimate(consumer);
        if (!estimate.empty())
            headline += ", " + estimate;
        auto* text = new wxBoxSizer(wxVERTICAL);
        text->Add(new wxStaticText(this, wxID_ANY, headline), 0, wxEXPAND);
        auto* effect = new wxStaticText(this, wxID_ANY, side_effect(consumer));
        effect->SetFont(Label::Body_12);
        effect->SetForegroundColour(wxColour(107, 107, 107));
        text->Add(effect, 0, wxEXPAND);
        row->Add(text, 1, wxEXPAND);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    }

    auto* remember_row = new wxBoxSizer(wxHORIZONTAL);
    m_remember = new ::CheckBox(this);
    remember_row->Add(m_remember, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    remember_row->Add(new wxStaticText(this, wxID_ANY, _L("Remember my choice (change it in Preferences)")), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(remember_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    auto* buttons = new DialogButtons(this, {"OK", "Cancel"});
    buttons->GetOK()->SetLabel(_L("Free VRAM"));
    buttons->GetCANCEL()->SetLabel(_L("Not now"));
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { answer(true); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { answer(false); });
    sizer->Add(buttons, 0, wxEXPAND);

    // The close box and Escape mean "Not now".
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { answer(false); });

    SetSizer(sizer);
    Layout();
    sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void VramOffloadDialog::answer(bool free_vram)
{
    if (m_answered)
        return;
    m_answered = true;
    Result result;
    result.free_vram = free_vram;
    for (CheckBox* box : m_consumer_boxes)
        result.selected.push_back(box->GetValue());
    result.remember = m_remember != nullptr && m_remember->GetValue();
    Hide();
    // The callback may drop its pointer to the dialog; destroy it only after it ran.
    if (m_on_result)
        m_on_result(result);
    Destroy();
}

void VramOffloadDialog::dismiss()
{
    if (m_answered)
        return;
    m_answered = true;
    Hide();
    Destroy();
}

}} // namespace Slic3r::GUI

#endif // __linux__
