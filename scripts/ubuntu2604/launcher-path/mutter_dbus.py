"""Mutter D-Bus plumbing shared by capture.py (ScreenCast) and inject.py (RemoteDesktop + ScreenCast)."""
import sys
import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

RD = "org.gnome.Mutter.RemoteDesktop"
SC = "org.gnome.Mutter.ScreenCast"
DC = "org.gnome.Mutter.DisplayConfig"
ROOT = "/org/gnome/Mutter"


def session_bus():
    return Gio.bus_get_sync(Gio.BusType.SESSION, None)


def call(bus, name, path, iface, method, args=None, sig=None):
    try:
        return bus.call_sync(name, path, iface, method, args, GLib.VariantType(sig) if sig else None, 0, 10000, None)
    except GLib.Error as e:
        # Mutter refuses every session while it inhibits remote access (for example "Session creation inhibited").
        sys.exit(f"{method} on {name} failed: {e.message}")


def create_screencast_session(bus, properties):
    return call(bus, SC, f"{ROOT}/ScreenCast", SC, "CreateSession", GLib.Variant("(a{sv})", (properties,)), "(o)").unpack()[0]


def record_primary_monitor(bus, session):
    """The stream of the primary monitor; its pointer coordinates are relative to that monitor's top-left corner."""
    return call(bus, SC, session, SC + ".Session", "RecordMonitor", GLib.Variant("(sa{sv})", ("", {})), "(o)").unpack()[0]


def primary_monitor_geometry(bus):
    """(x, y, stream_width, stream_height, factor) of the primary monitor.

    x, y: the monitor's origin in the logical (X11/layout) space. stream_*: the size in stream pixels, i.e. the pixels of the
    current mode, which is what capture.py's PNG and NotifyPointerMotionAbsolute use. factor: stream pixels per logical
    pixel (the monitor scale when the layout mode is logical, 1 when it is physical), so a stream point (sx, sy) is the
    logical point (x + sx / factor, y + sy / factor).
    """
    state = call(bus, DC, f"{ROOT}/DisplayConfig", DC, "GetCurrentState", None, None).unpack()
    monitors, logical, properties = state[1], state[2], state[3]
    logical_layout = properties.get("layout-mode", 1) == 1
    for x, y, scale, transform, primary, members, _props in logical:
        if not primary:
            continue
        connector = members[0][0]
        for spec, modes, _mprops in monitors:
            if spec[0] != connector:
                continue
            for _id, width, height, _refresh, _preferred, _scales, mode_props in modes:
                if mode_props.get("is-current"):
                    if transform in (1, 3, 5, 7):
                        width, height = height, width
                    return x, y, width, height, scale if logical_layout else 1.0
    raise RuntimeError("no primary monitor with a current mode in DisplayConfig")
