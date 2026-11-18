#ifndef slic3r_GLBindTrap_hpp_
#define slic3r_GLBindTrap_hpp_

#include <string>

class wxGLCanvas;
class wxGLContext;

namespace Slic3r { namespace GUI {

// Outcome of making the shared GL context current on a canvas.
struct GLBindResult
{
    // The context is current on the canvas' drawable.
    bool bound{false};
    // X protocol error raised while binding, 0 for none. Always 0 where no error trap exists
    // (not GTK3 on X11), there the bind reports failure only through `bound`.
    int x_error{0};
    // X BadAlloc: the X server or the GPU driver could not allocate memory for the bind.
    bool out_of_memory{false};
    // XGetErrorText() of x_error, for the log and the dialog.
    std::string x_error_text;
};

// wxGLCanvas::SetCurrent() is glXMakeCurrent() underneath. When it fails with an X error (BadAlloc when the
// GPU driver cannot back the canvas window), GDK's default handler prints "received an X Window System error"
// and ends the process (_gdk_x11_display_error_event in gdk/x11/gdkdisplay-x11.c), losing the open project.
// This puts its own Xlib error handler in front of GDK's and claims the errors raised during the bind, so the
// caller gets them instead; an XSync at the end of the bind collects errors the server sends asynchronously.
// Every other error goes on to GDK's handler. GUI thread only.
GLBindResult bind_gl_context(wxGLCanvas& canvas, const wxGLContext& context);

}} // namespace Slic3r::GUI

#endif // slic3r_GLBindTrap_hpp_
