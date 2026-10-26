#!/usr/bin/env python3
"""Run a command inside a USB/HID-less bubblewrap sandbox with the environment GNOME gives a launched app.

    sandbox.py --appimage FILE [--net] [--setenv K=V]... RUNDIR MOUNT -- CMD...

RUNDIR/config and RUNDIR/cache are bound over the Snapmaker_Orca config directory and the cache directory of the session
environment (XDG_CONFIG_HOME/XDG_CACHE_HOME, default ~/.config/Snapmaker_Orca and ~/.cache; a copy of the real config, so the
real one is never written); the rest of $HOME is read-only. MOUNT is the FUSE mount point of the AppImage, mounted by the
caller outside the sandbox (nothing of the app runs there). The environment is the systemd user environment, which is
what gnome-shell hands to launcher-started apps, plus the variables the AppImage runtime sets (APPIMAGE, APPDIR, ARGV0, OWD).

Why the sandbox: Snapmaker Orca's USB/HID enumeration has hung the development laptop (AGENTS.md). /sys/bus/usb,
/sys/class/hidraw, /sys/class/usbmisc, /sys/bus/hid and /run/udev are empty tmpfs inside; /dev has only the GPU nodes.
"""
import argparse
import os
import sys

import session_env

def main():
    ap = argparse.ArgumentParser(allow_abbrev=False)
    ap.add_argument("rundir")
    ap.add_argument("mount")
    ap.add_argument("--net", action="store_true", help="share the network namespace (printer connection, cloud pages)")
    ap.add_argument("--appimage", required=True, help="path the launcher runs (APPIMAGE/ARGV0 of the runtime)")
    ap.add_argument("--setenv", action="append", default=[], metavar="K=V")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    if not cmd:
        sys.exit("sandbox.py: no command")

    home = os.path.expanduser("~")
    uid = os.getuid()

    env = session_env.read()
    env.update(
        {
            "GIO_LAUNCHED_DESKTOP_FILE": f"{home}/.local/share/applications/snapmaker-orca.desktop",
            "APPIMAGE": args.appimage,
            "APPDIR": args.mount,
            "ARGV0": args.appimage,
            "OWD": home,
        }
    )
    for item in args.setenv:
        key, sep, value = item.partition("=")
        if not sep or not key:
            sys.exit(f"sandbox.py: --setenv needs K=V, got {item!r}")
        if key in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
            sys.exit(f"sandbox.py: --setenv {key} is not supported (the config copy and the project path come from the session's value)")
        env[key] = value

    config_target = session_env.config_dir(env)
    cache_target = session_env.cache_dir(env)
    # bwrap cannot create a mount point inside the read-only root, so a missing one (first run, no cache yet) is made
    # on the host: an empty directory, nothing else of the real home is written.
    os.makedirs(config_target, exist_ok=True)
    os.makedirs(cache_target, exist_ok=True)

    bwrap = ["bwrap", "--unshare-all"]
    if args.net:
        bwrap.append("--share-net")
    bwrap += ["--clearenv", "--ro-bind", "/", "/", "--dev", "/dev", "--dev-bind", "/dev/dri", "/dev/dri"]
    for node in ("nvidia0", "nvidiactl", "nvidia-uvm", "nvidia-uvm-tools", "nvidia-modeset"):
        if os.path.exists(f"/dev/{node}"):
            bwrap += ["--dev-bind", f"/dev/{node}", f"/dev/{node}"]
    for empty in ("/sys/bus/usb", "/sys/class/hidraw", "/sys/class/usbmisc", "/sys/bus/hid", "/run/udev"):
        bwrap += ["--tmpfs", empty]
    bwrap += [
        "--tmpfs", "/tmp",
        "--bind", "/tmp/.X11-unix", "/tmp/.X11-unix",
        "--bind", f"/run/user/{uid}", f"/run/user/{uid}",
        "--bind", args.mount, args.mount,
        "--bind", f"{args.rundir}/config", config_target,
        "--bind", f"{args.rundir}/cache", cache_target,
        "--chdir", home,
    ]
    for key, value in env.items():
        bwrap += ["--setenv", key, value]
    os.execvp("bwrap", bwrap + cmd)


if __name__ == "__main__":
    main()
