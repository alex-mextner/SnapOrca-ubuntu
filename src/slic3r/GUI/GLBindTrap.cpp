#include "GLBindTrap.hpp"

#include <atomic>

#include <wx/glcanvas.h>

#if defined(__WXGTK3__)
#include <gtk/gtk.h>
#endif

// X11 headers define macros (Status, Bool, None, ...) that collide with the rest of the GUI sources;
// this unit keeps them away from GLCanvas3D.cpp.
#if defined(__WXGTK3__) && defined(GDK_WINDOWING_X11)
#include <gdk/gdkx.h>
#include <GL/glx.h>
#define SLIC3R_GL_BIND_TRAP 1
#endif

namespace Slic3r { namespace GUI {

#ifdef SLIC3R_GL_BIND_TRAP
namespace {

// The bind in progress. Only the GUI thread binds GL contexts, but Xlib's error handler is process-wide and runs for
// errors of every X connection of the process (GStreamer's ximagesink opens its own on another thread), so the state is
// static and atomic instead of living on the binding thread's stack. Non-null s_trap_display marks a bind in progress.
std::atomic<Display*> s_trap_display{nullptr};
// Serial of the first request the bind sends: older requests are not ours.
std::atomic<unsigned long> s_trap_first_serial{0};
std::atomic<int>           s_trap_error{0};
// The handler ours replaced (GDK's): it gets every error that does not belong to a bind.
std::atomic<XErrorHandler> s_next_handler{nullptr};

int bind_error_handler(Display* xdisplay, XErrorEvent* event)
{
    // NVIDIA's GLX reports a failed glXMakeCurrent (BadAlloc, request 149 minor 5) with an error event of serial 0,
    // generated in the client library rather than read from the server, so the serial cannot be matched to a
    // request and anything with serial 0 that arrives during the bind belongs to it.
    if (xdisplay == s_trap_display.load() && (event->serial == 0 || event->serial >= s_trap_first_serial.load())) {
        int none = 0;
        s_trap_error.compare_exchange_strong(none, event->error_code);
        return 0;
    }
    // Errors of earlier requests and of other connections go where they would have gone without us.
    XErrorHandler next = s_next_handler.load();
    return next != nullptr ? next(xdisplay, event) : 0;
}

// Ours stays installed instead of being swapped in and out around each bind: a component that saves and restores the
// handler on another thread (ximagesink does) could interleave with the swap, leave ours installed with no bind to
// serve, or save ours as its predecessor. Each bind puts ours back in front in case something replaced it meanwhile.
// Only the handler found at the first install (GDK's) is kept as the next one: one found later is another component's
// temporary handler (ximagesink's swallows every error), which must not take over GDK's errors for good. A bind
// whose error such a temporary handler swallows reports a plain failure, and the next bind is trapped again.
void install_bind_error_handler()
{
    XErrorHandler previous = XSetErrorHandler(bind_error_handler);
    XErrorHandler none     = nullptr;
    if (previous != bind_error_handler)
        s_next_handler.compare_exchange_strong(none, previous);
}

} // namespace
#endif

GLBindResult bind_gl_context(wxGLCanvas& canvas, const wxGLContext& context)
{
    GLBindResult result;

#ifdef SLIC3R_GL_BIND_TRAP
    // Orca forces GDK_BACKEND=x11 (Snapmaker_Orca.cpp), but a Wayland display has no X errors to trap.
    GtkWidget*  widget  = canvas.GetHandle();
    GdkDisplay* display = widget != nullptr ? gtk_widget_get_display(widget) : nullptr;
    if (display != nullptr && GDK_IS_X11_DISPLAY(display)) {
        Display* xdisplay = GDK_DISPLAY_XDISPLAY(display);
        // Not gdk_x11_display_error_trap_push(): GDK matches an error to its traps by serial, and the serial 0 of
        // NVIDIA's GLX error lies outside every trap, so GDK's default handler printed the error and exited anyway.
        install_bind_error_handler();
        s_trap_error        = 0;
        s_trap_first_serial = NextRequest(xdisplay);
        s_trap_display      = xdisplay;
        // NVIDIA's GLX corrupts its state when a bind fails while the context is current on another drawable: the
        // next glXMakeCurrent or GL call segfaults in libGLX_nvidia or libnvidia-glcore (reproduced with a 4x MSAA
        // window under exhausted VRAM). From the released state the same bind fails cleanly and succeeds again once
        // memory is free, so the context is released first.
        if (glXGetCurrentContext() != nullptr)
            glXMakeCurrent(xdisplay, None, nullptr);
        result.bound = canvas.SetCurrent(context);
        // An error the server sends for a request of the bind arrives asynchronously: collect it while trapped.
        XSync(xdisplay, False);
        s_trap_display = nullptr;

        result.x_error = s_trap_error.load();
        if (result.x_error != 0) {
            // The bind may report success while the server rejected one of its requests: the drawable is then not usable.
            result.bound         = false;
            result.out_of_memory = result.x_error == BadAlloc;
            char text[128]       = {};
            XGetErrorText(xdisplay, result.x_error, text, sizeof(text) - 1);
            result.x_error_text = text;
        }
        return result;
    }
#endif

    result.bound = canvas.SetCurrent(context);
    return result;
}

}} // namespace Slic3r::GUI
