#!/usr/bin/env python3
"""The systemd user environment (what gnome-shell hands to launcher-started apps), shared by the launcher-path scripts.

CLI for launcher-path.sh: `session_env.py config-dir` / `cache-dir` print where Snapmaker Orca keeps its config (the
Snapmaker_Orca directory under XDG_CONFIG_HOME, default ~/.config) and cache (XDG_CACHE_HOME, default ~/.cache) in that environment.
"""
import os
import subprocess
import sys

SIMPLE_ESCAPES = {"a": "\a", "b": "\b", "e": "\x1b", "E": "\x1b", "f": "\f", "n": "\n", "r": "\r", "t": "\t", "v": "\v",
                  "\\": "\\", "'": "'", '"': '"', "?": "?", "$": "$", "`": "`"}


def _unescape_ansi_c(body):
    """Decode the inside of $'...': C escapes, \\xHH and \\NNN as raw bytes, \\uHHHH/\\UHHHHHHHH as characters."""
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\" or i + 1 >= len(body):
            out += c.encode("utf-8", "surrogateescape")
            i += 1
            continue
        n = body[i + 1]
        if n in SIMPLE_ESCAPES:
            out += SIMPLE_ESCAPES[n].encode()
            i += 2
        elif n == "x" and body[i + 2:i + 4].strip("0123456789abcdefABCDEF") == "" and body[i + 2:i + 3]:
            digits = body[i + 2:i + 4]
            out.append(int(digits, 16))
            i += 2 + len(digits)
        elif n in "01234567":
            digits = body[i + 1:i + 4]
            j = 0
            while j < len(digits) and digits[j] in "01234567":
                j += 1
            out.append(int(digits[:j], 8) & 0xFF)
            i += 1 + j
        elif n in "uU":
            width = 4 if n == "u" else 8
            digits = body[i + 2:i + 2 + width]
            if digits and digits.strip("0123456789abcdefABCDEF") == "":
                out += chr(int(digits, 16)).encode("utf-8", "surrogateescape")
                i += 2 + len(digits)
            else:
                out += b"\\" + n.encode()
                i += 2
        else:
            out += b"\\" + n.encode("utf-8", "surrogateescape")
            i += 2
    return out.decode("utf-8", "surrogateescape")


def _decode(value):
    """Undo the shell quoting `systemctl show-environment` applies: $'...', '...' or "..." (one word)."""
    if value.startswith("$'") and value.endswith("'") and len(value) >= 3:
        return _unescape_ansi_c(value[2:-1])
    if len(value) >= 2 and value[0] == value[-1] == "'":
        return value[1:-1]
    if len(value) >= 2 and value[0] == value[-1] == '"':
        return "".join(_unescape_double(value[1:-1]))
    return value


def _unescape_double(body):
    i = 0
    while i < len(body):
        if body[i] == "\\" and i + 1 < len(body) and body[i + 1] in '$`"\\':
            yield body[i + 1]
            i += 2
        else:
            yield body[i]
            i += 1


def parse(stdout):
    env = {}
    for line in stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep:
            env[key] = _decode(value)
    return env


def read():
    shown = subprocess.run(["systemctl", "--user", "show-environment"], capture_output=True, text=True, check=True).stdout
    return parse(shown)


def apply_display_env(env=None):
    """Point this process at the session's X display, overriding values a caller's shell may carry over from elsewhere."""
    env = read() if env is None else env
    os.environ["DISPLAY"] = env.get("DISPLAY") or os.environ.get("DISPLAY") or ":0"
    if env.get("XAUTHORITY"):
        os.environ["XAUTHORITY"] = env["XAUTHORITY"]
    else:
        os.environ.pop("XAUTHORITY", None)


def config_dir(env):
    return os.path.join(env.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "Snapmaker_Orca")


def cache_dir(env):
    return env.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")


if __name__ == "__main__":
    commands = {"config-dir": config_dir, "cache-dir": cache_dir}
    if len(sys.argv) != 2 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    print(commands[sys.argv[1]](read()))
