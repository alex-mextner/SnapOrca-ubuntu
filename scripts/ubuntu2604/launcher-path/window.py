#!/usr/bin/env python3
"""window.py UNIT [SECONDS]: print the X11 id of the main window of the Snapmaker Orca that runs in the systemd user unit UNIT.

Ownership is decided by the HOST pid of the X client connection, which the X server reads from the socket (XRes
XResQueryClientIds, PID mask); a client cannot choose it, and it is unique across PID namespaces, unlike the _NET_WM_PID
property (set by the client, and a namespace-local number inside the sandbox). A window counts only when that pid is in the
unit's cgroup, so the user's own (unsandboxed) Orca, or another sandbox, is never returned. Of the unit's Snapmaker_Orca-class
windows the largest is the main one (the others are GL helper windows; while only the splash screen exists, that is returned,
and input is refused once the main window takes the focus, so call `window` again). Exits 1, printing nothing, when there is no
such window within SECONDS (default 0: one look; otherwise it polls once a second in this process).
"""
import ctypes
import os
import re
import subprocess
import sys
import time

import session_env

CLASS_LINE = re.compile(r'^\s*(0x[0-9a-fA-F]+) .*"Snapmaker_Orca"\)\s+(\d+)x(\d+)')
XRES_CLIENT_ID_PID_MASK = 2


class ClientIdSpec(ctypes.Structure):
    _fields_ = [("client", ctypes.c_ulong), ("mask", ctypes.c_uint)]


class ClientIdValue(ctypes.Structure):
    _fields_ = [("spec", ClientIdSpec), ("length", ctypes.c_long), ("value", ctypes.c_void_p)]


def cgroup_pids(unit):
    group = subprocess.run(
        ["systemctl", "--user", "show", "-p", "ControlGroup", "--value", unit], capture_output=True, text=True, check=True
    ).stdout.strip()
    pids = set()
    if not group:
        return pids
    for directory, _dirs, files in os.walk("/sys/fs/cgroup" + group):
        if "cgroup.procs" in files:
            with open(os.path.join(directory, "cgroup.procs")) as procs:
                pids.update(int(p) for p in procs.read().split())
    return pids


class XClientPids:
    def __init__(self):
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xres = ctypes.CDLL("libXRes.so.1")
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.xres.XResQueryClientIds.argtypes = [
            ctypes.c_void_p, ctypes.c_long, ctypes.POINTER(ClientIdSpec), ctypes.POINTER(ctypes.c_long),
            ctypes.POINTER(ctypes.POINTER(ClientIdValue)),
        ]
        self.xres.XResQueryClientIds.restype = ctypes.c_int
        self.xres.XResGetClientPid.argtypes = [ctypes.POINTER(ClientIdValue)]
        self.xres.XResGetClientPid.restype = ctypes.c_int
        self.xres.XResClientIdsDestroy.argtypes = [ctypes.c_long, ctypes.POINTER(ClientIdValue)]
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            sys.exit("window.py: cannot open the X display")

    def pid_of(self, winid):
        spec = ClientIdSpec(winid, XRES_CLIENT_ID_PID_MASK)
        count = ctypes.c_long(0)
        values = ctypes.POINTER(ClientIdValue)()
        if self.xres.XResQueryClientIds(self.display, 1, ctypes.byref(spec), ctypes.byref(count), ctypes.byref(values)) != 0:
            return None
        try:
            for i in range(count.value):
                pid = self.xres.XResGetClientPid(ctypes.byref(values[i]))
                if pid > 0:
                    return pid
            return None
        finally:
            self.xres.XResClientIdsDestroy(count, values)


def find(unit, clients):
    pids = cgroup_pids(unit)
    if not pids:
        return None
    tree = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True, text=True).stdout
    best = None
    for line in tree.splitlines():
        m = CLASS_LINE.match(line)
        if not m:
            continue
        area = int(m.group(2)) * int(m.group(3))
        if (best is None or area > best[0]) and clients.pid_of(int(m.group(1), 16)) in pids:
            best = (area, m.group(1))
    return best[1] if best else None


def main(unit, seconds):
    session_env.apply_display_env()
    clients = XClientPids()
    deadline = time.monotonic() + seconds
    while True:
        found = find(unit, clients)
        if found:
            print(found)
            return 0
        if time.monotonic() >= deadline:
            return 1
        time.sleep(1)


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], float(sys.argv[2]) if len(sys.argv) == 3 else 0))
