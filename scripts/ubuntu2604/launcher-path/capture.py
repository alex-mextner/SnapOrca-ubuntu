#!/usr/bin/env python3
"""capture.py OUT.png: screenshot of the composited desktop (what the user sees), through Mutter's ScreenCast + PipeWire.

GetImage of one X window (what xwd/ImageMagick/GDK read) is not the same thing: under XWayland the compositor, not the X
server, puts the pixels on screen. gnome-shell's own Screenshot D-Bus method refuses unprivileged callers, ScreenCast does not.
Needs: python3-gi, gst-launch-1.0 with pipewiresrc and pngenc.
"""
import glob
import os
import subprocess
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

import mutter_dbus
from mutter_dbus import SC


GRACE_AFTER_FIRST_FRAME = 1.5


def frame_index(path):
    """multifilesink counter of OUT.N.png; anything that is not a number sorts first."""
    middle = path.rsplit(".", 2)[1]
    return int(middle) if middle.isdigit() else -1


def frames_of(out):
    return sorted((p for p in glob.glob(glob.escape(out) + ".*.png") if frame_index(p) >= 0), key=frame_index)


def main(out):
    bus = mutter_dbus.session_bus()
    session = mutter_dbus.create_screencast_session(bus, {})
    stream = mutter_dbus.record_primary_monitor(bus, session)
    for stale in frames_of(out):
        os.unlink(stale)
    node = {}
    loop = GLib.MainLoop()

    def on_signal(_conn, _sender, _path, _iface, name, params, *_):
        if name == "PipeWireStreamAdded":
            node["id"] = params.unpack()[0]
            loop.quit()

    bus.signal_subscribe(SC, SC + ".Stream", "PipeWireStreamAdded", stream, None, Gio.DBusSignalFlags.NONE, on_signal)
    mutter_dbus.call(bus, SC, session, SC + ".Session", "Start", None, "()")
    GLib.timeout_add_seconds(10, loop.quit)
    loop.run()
    try:
        if "id" not in node:
            sys.exit("capture.py: no PipeWire node")
        # A static screen delivers one buffer only, so ask for two but stop shortly after the first frame was written
        # instead of waiting out the second one.
        gst = subprocess.Popen(
            ["gst-launch-1.0", "-q", "pipewiresrc", f"path={node['id']}", "num-buffers=2", "!", "videoconvert", "!", "pngenc", "!",
             "multifilesink", f"location={out}.%d.png"]
        )
        deadline = time.monotonic() + 20
        first_frame_at = None
        while gst.poll() is None and time.monotonic() < deadline:
            if first_frame_at is None and frames_of(out):
                first_frame_at = time.monotonic()
            if first_frame_at is not None and time.monotonic() - first_frame_at > GRACE_AFTER_FIRST_FRAME:
                break
            time.sleep(0.1)
        if gst.poll() is None:
            gst.terminate()
            gst.wait()
    finally:
        mutter_dbus.call(bus, SC, session, SC + ".Session", "Stop", None, "()")
    frames = frames_of(out)
    if not frames:
        sys.exit("capture.py: no frame received")
    os.replace(frames[-1], out)
    for extra in frames[:-1]:
        os.unlink(extra)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
