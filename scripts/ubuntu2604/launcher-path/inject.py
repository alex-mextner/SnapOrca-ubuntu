#!/usr/bin/env python3
"""inject.py WINID ACTION...: keyboard and pointer input through Mutter's RemoteDesktop API.

Actions: KEY[+KEY]... (keysym names, e.g. Control_L+r), click:X,Y (pixels of capture.py's screenshot, the primary monitor's
stream pixels, which is what NotifyPointerMotionAbsolute takes), sleep:SECONDS.
Unlike XTest (which gnome-shell answers with a "Remote Desktop" permission dialog on the user's screen) this needs no approval.

Safety: input is refused unless the X11 window WINID is the active window (_NET_ACTIVE_WINDOW) and, for a click, the point
lies inside that window: the stream point is converted to logical coordinates (origin + point / scale, the scale from
DisplayConfig) and compared with the X11 window geometry; the point checked is the point sent, and the check is repeated right
before the button press. This assumes X11 coordinates are logical pixels, as under mutter's default XWayland scaling. Both are
checked right before each pointer button press and keysym press. Releases (button up, key up) are always sent, even when focus
moved meanwhile: a release that is skipped leaves a stuck button or modifier in whatever window is focused, which is worse than
an unmatched release. The window can still lose focus between the check and the event delivery, so this narrows the risk, it
does not remove it. WINID 0 skips the checks, for shell-level keys only (Super, Escape).
"""
import math
import re
import subprocess
import sys
import time

import gi

gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib

import mutter_dbus
import session_env
from mutter_dbus import RD

BUTTON_LEFT = 0x110


def active_window():
    out = subprocess.run(["xprop", "-root", "_NET_ACTIVE_WINDOW"], capture_output=True, text=True, check=True).stdout
    return int(out.rsplit("#", 1)[1].strip(), 0)


def window_geometry(winid):
    out = subprocess.run(["xwininfo", "-id", str(winid)], capture_output=True, text=True, check=True).stdout
    vals = {}
    for key in ("Absolute upper-left X", "Absolute upper-left Y", "Width", "Height"):
        m = re.search(rf"{key}:\s*(-?\d+)", out)
        if not m:
            sys.exit(f"inject.py: cannot read the geometry of window {winid:#x}")
        vals[key] = int(m.group(1))
    x, y = vals["Absolute upper-left X"], vals["Absolute upper-left Y"]
    return x, y, x + vals["Width"], y + vals["Height"]


def parse_click(arg):
    try:
        x, y = (float(v) for v in arg.split(","))
    except ValueError:
        sys.exit(f"inject.py: bad click:{arg}, want click:X,Y")
    if not (math.isfinite(x) and math.isfinite(y)):
        sys.exit(f"inject.py: bad click:{arg}, not finite")
    return x, y


def parse_sleep(arg):
    try:
        seconds = float(arg)
    except ValueError:
        sys.exit(f"inject.py: bad sleep:{arg}, want sleep:SECONDS")
    if not math.isfinite(seconds) or seconds < 0:
        sys.exit(f"inject.py: bad sleep:{arg}, want a finite non-negative number")
    return seconds


def main(winid, actions):
    bus = mutter_dbus.session_bus()
    monitor_x, monitor_y, monitor_w, monitor_h, factor = mutter_dbus.primary_monitor_geometry(bus)

    def rd(path, iface, method, args=None, sig=None):
        return mutter_dbus.call(bus, RD, path, iface, method, args, sig)

    session = rd(f"{mutter_dbus.ROOT}/RemoteDesktop", RD, "CreateSession", None, "(o)").unpack()[0]
    sid = rd(session, "org.freedesktop.DBus.Properties", "Get", GLib.Variant("(ss)", (RD + ".Session", "SessionId")), "(v)").unpack()[0]
    sc_session = mutter_dbus.create_screencast_session(bus, {"remote-desktop-session-id": GLib.Variant("s", sid)})
    stream = mutter_dbus.record_primary_monitor(bus, sc_session)
    rd(session, RD + ".Session", "Start")
    time.sleep(1)

    def notify(method, params):
        rd(session, RD + ".Session", method, params)

    def require_active():
        if winid:
            current = active_window()
            if current != winid:
                sys.exit(f"inject.py: refused, active window is {current:#x}, not {winid:#x}")

    def require_inside(x, y):
        if winid:
            left, top, right, bottom = window_geometry(winid)
            gx, gy = monitor_x + x / factor, monitor_y + y / factor
            if not (left <= gx < right and top <= gy < bottom):
                sys.exit(f"inject.py: refused, ({x:g},{y:g}) is ({gx:g},{gy:g}) on the desktop, outside the window ({left},{top})-({right},{bottom})")

    def click(arg):
        x, y = parse_click(arg)
        if not (0 <= x < monitor_w and 0 <= y < monitor_h):
            sys.exit(f"inject.py: refused, ({x:g},{y:g}) is outside the primary monitor ({monitor_w}x{monitor_h} px)")
        require_inside(x, y)
        require_active()
        notify("NotifyPointerMotionAbsolute", GLib.Variant("(sdd)", (stream, x, y)))
        time.sleep(0.5)
        require_inside(x, y)
        require_active()
        notify("NotifyPointerButton", GLib.Variant("(ib)", (BUTTON_LEFT, True)))
        try:
            time.sleep(0.1)
            require_active()
        finally:
            notify("NotifyPointerButton", GLib.Variant("(ib)", (BUTTON_LEFT, False)))
        time.sleep(0.1)

    def keys(action):
        keysyms = [Gdk.keyval_from_name(name) for name in action.split("+")]
        if not all(keysyms) or Gdk.KEY_VoidSymbol in keysyms:
            sys.exit(f"inject.py: unknown keysym in {action}")
        down = []
        try:
            for keysym in keysyms:
                require_active()
                notify("NotifyKeyboardKeysym", GLib.Variant("(ub)", (keysym, True)))
                down.append(keysym)
                time.sleep(0.1)
        finally:
            for keysym in reversed(down):
                notify("NotifyKeyboardKeysym", GLib.Variant("(ub)", (keysym, False)))
                time.sleep(0.1)

    handlers = {"click": click, "sleep": lambda arg: time.sleep(parse_sleep(arg))}
    try:
        for action in actions:
            kind, sep, arg = action.partition(":")
            if sep and kind in handlers:
                handlers[kind](arg)
            elif sep:
                sys.exit(f"inject.py: unknown action {action!r}")
            else:
                keys(action)
    finally:
        rd(session, RD + ".Session", "Stop")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    session_env.apply_display_env()
    main(int(sys.argv[1], 0), sys.argv[2:])
