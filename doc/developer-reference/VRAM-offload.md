# VRAM offload for the G-code preview (Linux)

Heavy GPU daemons (`wyoming-whisper`, `task-classify-server`) can move their models off the GPU for the time the G-code
preview needs the memory. Orca asks them over a control socket; it never stops or restarts a unit. Design and protocol:
`docs/superpowers/specs/2026-11-18-vram-offload-design.md` (branch `docs/vram-offload-spec`).

## What happens

1. Switching to the Preview tab while NVML reports less free VRAM than `vram_offload_min_free_mb` and at least one
   daemon with a socket in `$XDG_RUNTIME_DIR/vram-offload/` holds VRAM: a non-modal dialog lists the daemons with their
   VRAM, restore time and side effect. The toolpath GPU upload waits for the answer; the rest of the UI does not.
2. "Free VRAM" sends `offload` with a lease per daemon. "Not now" loads the toolpaths at once. "Remember my choice" sets
   `vram_offload_policy` to `always` or `never`.
3. Leaving the Preview tab starts a 60 s grace timer; coming back cancels it and keeps the leases. After 60 s outside
   the preview, and at Orca exit, Orca sends `restore` for every lease.
4. Crash safety: before the first `offload`, Orca writes `$XDG_RUNTIME_DIR/snapmaker-orca/vram-offload-<pid>.json` and
   starts `systemd-run --user --unit=orca-vram-restore-<pid> --property=MemoryMax=64M <orca> --vram-restore-watch <file>`.
   That helper mode (handled at the top of `main()`) waits for Orca's pid + start time to disappear and restores what is
   left in the file. The daemons also drop a lease whose holder is gone within 30 s.

## Settings (Preferences > General, Linux only)

| Key | Values | Default |
|---|---|---|
| `vram_offload_policy` | `ask`, `always`, `never` | `ask` |
| `vram_offload_min_free_mb` | MiB | `1536` |

`never` switches the feature off completely: no NVML call, no socket access. Without `libnvidia-ml.so.1`, without
`systemd-run`, or without any daemon socket the feature is off as well and Orca behaves as before.

## Checking by hand

```
ls $XDG_RUNTIME_DIR/vram-offload/ $XDG_RUNTIME_DIR/snapmaker-orca/
systemctl --user status 'orca-vram-restore-*'
printf '{"op":"status"}\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/vram-offload/wyoming-whisper.sock
```

Logic and tests live in `src/slic3r/Utils/VramOffload.*` (no wx or GL dependency) and
`tests/slic3rutils/VramOffload_test.cpp`; the dialog and the preview hooks are `src/slic3r/GUI/VramOffload*.{hpp,cpp}`.
