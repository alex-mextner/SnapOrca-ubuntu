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
    // The canvas has a window to bind to. A canvas that is not realized yet cannot be bound, which says nothing about
    // the context: such a bind leaves the current context alone and is not a loss.
    bool drawable{false};
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

// The shared context is current: the last bind_gl_context() that reached a drawable succeeded. A failed bind leaves
// no context current, and GL calls made then are dropped by the driver.
bool gl_context_bound();

// glDeleteBuffers(), glDeleteVertexArrays() and glDeleteTextures() that keep the names for the next successful
// bind_gl_context() while no context is current: a delete issued then is dropped and the GPU memory stays taken until
// the context is destroyed. Volumes, shells and toolpaths are released while a canvas is lost (every bind fails until
// graphics memory is free again), so their GL objects go through these. The kept names are locked because a GLModel
// can be dropped on a worker thread.
void delete_gl_buffers(int count, const unsigned int* names);
void delete_gl_vertex_arrays(int count, const unsigned int* names);
void delete_gl_textures(int count, const unsigned int* names);

}} // namespace Slic3r::GUI

#endif // slic3r_GLBindTrap_hpp_
