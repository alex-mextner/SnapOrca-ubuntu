#!/usr/bin/env python3
"""activate.py WINID: ask the window manager (mutter's XWM) to activate an X11 window, as a pager would.

A window the sandbox opened while the user works in another application stays behind it; this brings it forward so
capture.py shows it and inject.py's active-window check passes.
"""
import ctypes
import sys

import session_env

CLIENT_MESSAGE = 33
SUBSTRUCTURE_NOTIFY_REDIRECT = (1 << 19) | (1 << 20)


class ClientMessage(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
        ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong), ("format", ctypes.c_int), ("data", ctypes.c_long * 5),
    ]


class Event(ctypes.Union):
    _fields_ = [("xclient", ClientMessage), ("pad", ctypes.c_long * 24)]


def main(winid):
    session_env.apply_display_env()
    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XDefaultRootWindow.restype = ctypes.c_ulong
    x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x11.XSendEvent.restype = ctypes.c_int
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    if not display:
        sys.exit("activate.py: cannot open the X display")
    event = Event()
    event.xclient.type = CLIENT_MESSAGE
    event.xclient.window = winid
    event.xclient.message_type = x11.XInternAtom(display, b"_NET_ACTIVE_WINDOW", 0)
    event.xclient.format = 32
    event.xclient.data[0] = 2  # source indication: pager
    if not x11.XSendEvent(display, x11.XDefaultRootWindow(display), 0, SUBSTRUCTURE_NOTIFY_REDIRECT, ctypes.byref(event)):
        sys.exit("activate.py: XSendEvent failed")
    x11.XFlush(display)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(int(sys.argv[1], 0))
