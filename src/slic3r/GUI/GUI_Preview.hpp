#ifndef slic3r_GUI_Preview_hpp_
#define slic3r_GUI_Preview_hpp_

#include <wx/panel.h>

#include "libslic3r/Point.hpp"
#include "libslic3r/CustomGCode.hpp"

//BBS: add print base
#include "libslic3r/PrintBase.hpp"

#include <string>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <slic3r/GUI/GCodeViewer.hpp>

class wxGLCanvas;
class wxBoxSizer;
class wxStaticText;
class wxComboBox;
class wxComboCtrl;
class wxCheckBox;

namespace Slic3r {

class DynamicPrintConfig;
class Print;
class BackgroundSlicingProcess;
class Model;

namespace GUI {

class GLCanvas3D;
class GLToolbar;
class Bed3D;
struct Camera;
class Plater;
#ifdef _WIN32
class BitmapComboBox;
#endif

class View3D : public wxPanel
{
    wxGLCanvas* m_canvas_widget;
    GLCanvas3D* m_canvas;

public:
    View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
    virtual ~View3D();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();
    void bed_shape_changed();
    void plates_count_changed();

    void select_view(const std::string& direction);

    //BBS
    void select_curr_plate_all();
    void select_object_from_idx(std::vector<int> &object_idxs);
    void remove_curr_plate_all();

    void select_all();
    void deselect_all();
    void exit_gizmo();
    void delete_selected();
    void center_selected();
    void drop_selected();
    void center_selected_plate(const int plate_idx);
    void mirror_selection(Axis axis);

    bool is_layers_editing_enabled() const;
    bool is_layers_editing_allowed() const;
    void enable_layers_editing(bool enable);

    bool is_dragging() const;
    bool is_reload_delayed() const;

    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    void render();

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
};

class Preview : public wxPanel
{
    wxGLCanvas* m_canvas_widget { nullptr };
    GLCanvas3D* m_canvas { nullptr };
    DynamicPrintConfig* m_config;
    BackgroundSlicingProcess* m_process;
    GCodeProcessorResult* m_gcode_result;

#ifdef __linux__
    // We are getting mysterious crashes on Linux in gtk due to OpenGL context activation GH #1874 #1955.
    // So we are applying a workaround here.
    bool m_volumes_cleanup_required { false };
#endif /* __linux__ */

    // Calling this function object forces Plater::schedule_background_process.
    std::function<void()> m_schedule_background_process;

    unsigned int m_number_extruders { 1 };
    bool m_keep_current_preview_type{ false };

    //bool m_loaded { false };
    //BBS: add logic for preview print
    const Slic3r::PrintBase* m_loaded_print { nullptr };
    //BBS: add only gcode mode
    bool m_only_gcode { false };
    bool m_reload_paint_after_background_process_apply{false};
    // SkipToolpaths causes in effect: while any is set, load_print_as_fff passes skip_toolpaths=true to skip the
    // load_toolpaths GPU vertex buffers.
    unsigned char m_skip_toolpath_causes { 0 };
    // A load skipped the toolpath buffers while the VramOffload cause was set.
    bool m_vram_offload_deferred_load { false };
    bool m_toolpaths_released { false };

public:
    enum class OptionType : unsigned int
    {
        Travel,
        Wipe,
        Retractions,
        Unretractions,
        Seams,
        ToolChanges,
        ColorChanges,
        PausePrints,
        CustomGCodes,
        Shells,
        ToolMarker,
        Legend
    };

    Preview(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process, 
        GCodeProcessorResult* gcode_result, std::function<void()> schedule_background_process = []() {});
    virtual ~Preview();

    //BBS: update gcode_result
    void update_gcode_result(GCodeProcessorResult* gcode_result);

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();

    void bed_shape_changed();
    void select_view(const std::string& direction);
    void set_drop_target(wxDropTarget* target);

    //BBS: add only gcode mode
    void load_print(bool keep_z_range = false, bool only_gcode = false);
    void reload_print(bool keep_volumes = false, bool only_gcode = false);
    void refresh_print();
    //BBS: always load shell at preview
    void load_shells(const Print& print, bool force_previewing = false);
    void reset_shells();

    void msw_rescale();
    void sys_color_changed();

    //BBS: add m_loaded_print logic
    bool is_loaded() const { return (m_loaded_print != nullptr); }
    // The loaded print is forgotten (its toolpath buffers were freed): the next load rebuilds everything.
    void forget_loaded_print()
    {
        m_loaded_print       = nullptr;
        m_toolpaths_released = true;
    }
    // True once after forget_loaded_print(): the toolpaths were freed and have not been rebuilt yet.
    bool take_toolpaths_released()
    {
        const bool released  = m_toolpaths_released;
        m_toolpaths_released = false;
        return released;
    }
    //BBS
    void on_tick_changed(Type type);

    void show_sliders(bool show = true);
    void show_moves_sliders(bool show = true);
    void show_layers_sliders(bool show = true);
    void set_reload_paint_after_background_process_apply(bool flag) { m_reload_paint_after_background_process_apply = flag; }
    bool get_reload_paint_after_background_process_apply() { return m_reload_paint_after_background_process_apply; }
    // Why the preview leaves the toolpath GPU vertex buffers out. Each cause ends on its own event, so ending one
    // must not end the other.
    enum class SkipToolpaths : unsigned char {
        // The user went on with a slice at the low-memory dialog of Plater::reslice(); a new slice or project ends it.
        MemoryWarning = 1 << 0,
        // A canvas lost its GL context (Plater::on_gl_context_lost()); the recovery ends it.
        GLContextLost = 1 << 1,
        // The preview entered while GPU memory is short and the user has not yet decided whether to lend VRAM from other
        // programs (GUI/VramOffloadController); the decision ends it.
        VramOffload = 1 << 2,
    };
    void set_skip_toolpath_preview(SkipToolpaths cause, bool skip)
    {
        if (skip)
            m_skip_toolpath_causes |= static_cast<unsigned char>(cause);
        else
            m_skip_toolpath_causes &= ~static_cast<unsigned char>(cause);
    }
    bool skips_toolpath_preview(SkipToolpaths cause) const { return (m_skip_toolpath_causes & static_cast<unsigned char>(cause)) != 0; }
    // Ends the VramOffload skip. True when a load was left without toolpaths because of it, i.e. the caller has to load
    // the preview again for the toolpaths to reach the GPU.
    bool end_vram_offload_deferral()
    {
        const bool deferred = m_vram_offload_deferred_load;
        m_vram_offload_deferred_load = false;
        set_skip_toolpath_preview(SkipToolpaths::VramOffload, false);
        return deferred;
    }
    GCodeProcessorResult* get_gcode_result() { return m_gcode_result; }

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model);

    void bind_event_handlers();
    void unbind_event_handlers();
    void on_size(wxSizeEvent& evt);
    // Create/Update/Reset double slider on 3dPreview
    void check_layers_slider_values(std::vector<CustomGCode::Item>& ticks_from_model,
        const std::vector<double>& layers_z);

    void update_layers_slider(const std::vector<double>& layers_z, bool keep_z_range = false);    
    void update_layers_slider_mode();
    void update_layers_slider_from_canvas(wxKeyEvent &event);
    //BBS: add only gcode mode
    void load_print_as_fff(bool keep_z_range = false, bool only_gcode = false);
};


class AssembleView : public wxPanel
{
    wxGLCanvas* m_canvas_widget{ nullptr };
    GLCanvas3D* m_canvas{ nullptr };
public:
    AssembleView(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
    ~AssembleView();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();
    void render();

    bool is_reload_delayed() const;
    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    void select_view(const std::string& direction);

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Preview_hpp_
