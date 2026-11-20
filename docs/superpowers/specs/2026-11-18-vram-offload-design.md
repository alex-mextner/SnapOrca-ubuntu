# VRAM offload for Orca preview and idle unload for GPU daemons

Status: draft for owner review · 2026-11-18 · host: ubuntu-pc (RTX 4060 Laptop 8 GiB, driver 595.99.02, PCIe 4.0 x8)

## Problem

Two resident daemons hold about 4.8 GiB of the 8 GiB GPU while they sit idle almost all day:

| Daemon | Model | VRAM now (nvidia-smi) | Weights |
|---|---|---|---|
| `wyoming-whisper.service` | faster-whisper large-v3-turbo, CTranslate2 fp16 | 2224 MiB | 1.51 GiB (`model.bin`) |
| `task-classify-server.service` | laya (ModernBERT-large) fp32 + bf16 autocast | 2722 MiB | 1.569 GiB fp32 (measured) |

That leaves about 1.95 GiB free. The Orca G-code preview then runs short of VRAM, and whisper itself fails: since 2026-11-17 21:00 the journal shows 28 `CUDA failed with error out of memory` lines, and aisis-failover restarted whisper at 00:46 and 01:34 on 11-18.

## Goals

1. When the Orca preview needs VRAM, Orca offers to offload the GPU consumers that support it, and restores them when the user leaves preview, when Orca exits, and after an Orca crash.
2. Each daemon unloads its model after a long idle period and reloads it on the next real request.
3. Each daemon reports how long its offload and its restore take, so the cost of loading before the first answer is known.

## Non-goals

- Stopping, restarting or reconfiguring units from Orca. The processes stay alive, so `Restart=`, `Wants=` and client autospawn never fire.
- `cuda-checkpoint` and the cgroup freezer.
- Releasing the CUDA context itself. About 0.35–0.5 GiB per process stays allocated (estimate). Freeing it requires the process to exit; see the "Classifier coordination" section.
- Offloading arbitrary GPU processes. Only daemons that expose the control socket are offered.
- Moving laya permanently to bf16. That is a separate quick win (weights go from 1.569 to 0.785 GiB, measured) and gets its own ticket.

## Components

### 1. Control protocol (shared by both daemons)

- **Socket:** `$XDG_RUNTIME_DIR/vram-offload/<name>.sock`. The directory is mode 0700 and the socket 0600. `<name>` is `wyoming-whisper` or `task-classify`. Messages are newline-delimited JSON, one request per connection.
- **`{"op":"status"}`** returns:
  ```
  {"ok":true,"name":…,"pid":…,"state":"loaded|unloaded|pinned|loading",
   "weights_bytes":…,"idle_s":…,"leases":[{"id","holder_pid","holder_start"}],
   "last_offload_s":…,"last_restore_s":…,"last_first_answer_s":…}
  ```
- **`{"op":"offload","lease":"<uuid>","holder_pid":P,"holder_start":T}`** adds a pin lease. Control ops are serialized. The daemon first persists the lease and the epoch's pre-lease state atomically (crash-safe), then waits up to 10 s for in-flight requests to drain, moves the weights off the GPU and frees the allocator cache. On drain timeout or unload failure it atomically removes that lease again (and clears the epoch if it was the only lease), so the model state and lease set are exactly as before. Reply: `{"ok":true,"seconds":…,"freed_bytes":…}`, or `{"ok":false,"error":"busy|timeout"}`.
- **`{"op":"restore","lease":"<uuid>"}`** removes that lease. When no live lease is left, the daemon returns to the state it had before the first lease: it records `loaded_before_lease` when the first lease arrives (an `offload` on an already idle-unloaded daemon only adds the lease and moves nothing), and reloads to the GPU on release only if that flag is true. An idle-unloaded daemon stays unloaded after Orca releases it and reloads on its next real request. It is idempotent: an unknown lease returns ok. A CUDA OOM during restore returns `{"ok":false,"error":"oom"}`; the daemon stays in `unloaded` and reloads on the next request.
- **Leases:** pinning is reference-counted by lease. A lease whose holder (pid + `/proc/<pid>/stat` start time, to survive PID reuse) is gone is dropped when the daemon checks it: every 30 s and on every request. Pruning the last lease performs the same final-lease transition as `restore` (reload if `loaded_before_lease`, otherwise stay unloaded). This is a backstop only; the restorer (component 5) restores proactively.
- **Persistence:** leases go to `$XDG_RUNTIME_DIR/vram-offload/<name>.leases.json` together with `loaded_before_lease` for the current lease epoch. A daemon that is restarted while a live lease exists starts without the GPU copy (lazy load), so a restart cannot take the VRAM back in the middle of a preview, and on the final release it still knows whether to reload.
- **States:**
  - `unloaded` (idle): the next real request reloads the model to the GPU first.
  - `pinned`: never reloads to the GPU. The classifier answers on the CPU. Whisper answers with a Wyoming `Error` event, code `gpu-lent`.
- **Never used for offload:** the classifier's existing socket op `shutdown`. Under `Restart=always` it just causes a reload.

### 2. Daemon-side idle unload

- An idle timer counts only real requests. Defaults: `WHISPER_IDLE_UNLOAD_S=900` and `TASK_CLASSIFY_IDLE_UNLOAD_S=900`; `0` disables it.
- **Whisper:** calls `unload_model(to_cpu=False)`, which drops the host copy too. The reload reads `model.bin` again, which takes 0.50 s from NVMe cold (measured). That is no slower than paging a 1.5 GiB host copy back from swap: 27 GiB of the 32 GiB swap is in use. The Orca offload uses `to_cpu=True` instead, because that offload is short and its restore needs to be fast.
- **Classifier:** `model.to("cpu")` followed by `torch.cuda.empty_cache()`. The host copy stays, so a pinned classifier can serve from it on the CPU (3.1 s per predict, measured) and an idle restore is a single `.to("cuda")`.
- Every reload logs `restore_s` and `first_answer_s`, and `status` reports them. That provides the "load before first answer" figures in production.

### 3. NVML probe in Orca

- Loads with `dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_NODELETE)` and calls `nvmlInit_v2`, `nvmlDeviceGetHandleByIndex_v2(0)`, `nvmlDeviceGetMemoryInfo` and `nvmlDeviceGetComputeRunningProcesses_v3`, then always `nvmlShutdown`. It never calls `dlclose`: cuda-checkpoint issue #58 is a SIGSEGV after NVML was `dlclose`d without a shutdown.
- **Consumer list:** the `pid` from each socket's `status` is joined with the NVML per-pid `usedGpuMemory`. A GPU process without a socket is not offered.
- **No NVIDIA library, or NVML error:** the feature is off. Orca behaves as it does today.

### 4. Orca dialog UX

- **Trigger:** switching to Preview (`EVT_GLVIEWTOOLBAR_PREVIEW` → `Plater::priv::select_view_3D("Preview")`) while NVML reports free memory below `vram_offload_min_free_mb` (default 1536) and at least one consumer is offloadable.
- **Dialog:** a non-modal wxDialog with one checkbox per consumer, showing name, VRAM, and restore estimate from `last_restore_s` or the table below. It also states each consumer's side effect: "voice recognition pauses" for whisper, "classification runs on the CPU (~3 s)" for the classifier.
- **Buttons:** "Free VRAM", "Not now", and a "Remember" checkbox. These map to the AppConfig key `vram_offload_policy` = `ask|always|never`. The Preview tab opens immediately, but the toolpath GPU upload is deferred (the existing skip-toolpaths path) while the decision is pending: "Free VRAM" offloads the selected daemons and then loads the toolpaths; "Not now" loads them at once. The rest of the UI never waits.
- **Restore triggers:**
  - Leaving Preview (`set_current_panel`, or `select_view_3D("3D")`) starts a cancellable 60 s grace timer. Re-entering Preview cancels it and keeps the leases; only a continuous 60 s outside Preview releases them.
  - `GUI_App::OnExit`.
  - The restorer (component 5) after a crash.
- **Notification:** when an offload or restore finishes, a `NotificationManager` line shows the freed amount and time, or the error.

### 5. Crash-safe restorer

- **State file:** `$XDG_RUNTIME_DIR/snapmaker-orca/vram-offload-<pid>.json`:
  ```
  {orca_pid, orca_start, items:[{name, socket, lease}]}
  ```
  Orca writes it atomically (tmp + rename) **before** it sends `offload`. It removes items after a successful `restore`.
- **Launch:** on the first offload, Orca starts the restorer:
  ```
  systemd-run --user --collect --unit=orca-vram-restore-<pid> \
      --property=MemoryMax=64M <orca-binary> --vram-restore-watch <state-file>
  ```
  That helper mode is handled at the top of `main()`, before wx and GL init. It reuses the same C++ protocol client, so only one implementation exists.
- **Behaviour:**
  - The restorer blocks on `pidfd_open(orca_pid)`. It checks `orca_start` first; a mismatch counts as "already dead".
  - When Orca exits, it sends `restore` for every item left in the state file, deletes the file and exits.
  - It lives in the user manager, outside Orca's scope, so it survives an Orca crash or SIGKILL.
- **Clean exit:** Orca restores and empties the file itself. The restorer then finds nothing to do.
- **No restorer:** if `systemd-run` fails, Orca does not offer offload. The daemons' lease liveness check (component 1) is the second line of defence.

### 6. Classifier coordination (hyper-task / task-cli)

- **Idle policy:** keep `TASK_CLASSIFY_SERVER_IDLE_SECONDS=0` (no idle exit) and keep `Restart=always`. Idle unload replaces idle exit.
  - An exit-based design would need `Restart=on-failure`. It would also have to handle the `Wants=task-classify-server.service` units (`guardian-observation-control`, `tg-laya-clvm-forward`), `pm-chatgpt-audit` `ExecStartPre=systemctl --user start`, and client autospawn of a detached server outside systemd. All of these restart a stopped server.
  - This design never stops the process, so none of those units need changes.
- **Socket must keep answering:** the classify socket stays bound in every state (pinned requests are served on the CPU). Clients therefore never see a missing socket and never autospawn a second loader during a preview.
- **Autospawned server:** a detached server outside systemd runs the same code. It registers the same control socket name, and the existing `flock` keeps it a singleton.
- **Readiness check:** `laya-readiness-check` (every :00/:30) must keep using the CPU-pinned multilingual model or `capabilities`, so that it never triggers a GPU reload. `pm-chatgpt-audit` requests are real work and do reload.

### 7. Whisper wrapper and aisis probe

- **Wrapper:** `wyoming_whisper_offload.py`, in aisis `services/yandex-station/code/`.
  - It subclasses `wyoming_faster_whisper.models.ModelLoader` and runs the upstream `main()`. The CT2 handle is `transcriber.model.model`, a `ctranslate2.models.Whisper`.
  - Its `transcribe` runs under an asyncio lock plus an in-flight counter. It calls `load_model()` when the model is not loaded (and it is not pinned), and records the reload and first-answer times.
  - The control server runs in the same event loop. `95-pc.conf` `ExecStart` switches from `-m wyoming_faster_whisper` to the wrapper, with the same arguments.
  - At import it checks the version (wyoming-faster-whisper 3.1.0 / ctranslate2 4.7.1) and the hook points. On a mismatch it logs an error and runs the upstream `main()` without a control socket, so Orca simply sees no consumer.
- **CT2 facts the wrapper must respect** (`python/cpp/replica_pool.h`):
  - `unload_model` silently does nothing while a batch is active or the model lock is held. The wrapper therefore re-checks `model_is_loaded` and retries.
  - Using an unloaded model raises `"The model for this translator was unloaded"`.
  - `unload_model` calls the CUB allocator's `clear_cache()`.
  - `load_model(keep_cache=False)` drops the host copy.
- **Gateway `/healthz/asr`** (`stt_wyoming.py`):
  - It asks the control socket `status` first. If the model is `unloaded` or `pinned`, it returns 202 `{"skipped": "<state>"}` (no verdict), so the 60 s probe no longer reloads the model every minute.
  - The probe's `Transcribe` carries `context={"aisis_probe": true}`. The wrapper does not count such requests as activity, so the probe cannot hold off idle unload.
  - A `gpu-lent` `Error` event maps to 202. Without this it would be a `RECOGNIZER_BROKEN` 503 and aisis-failover would restart whisper during preview.

## Data flow

```
Preview entered → NVML free < threshold → status on each socket → dialog
  → write state file (intent) → systemd-run restorer (once) → offload(lease) per item
  → update state file → notification
Preview left (+60 s) / OnExit → restore(lease) per item → daemon returns to its pre-lease state (reload only if it was loaded) → clear state file
Orca crash → restorer pidfd fires → restore(lease) per remaining item → delete file
Daemon idle 900 s → unload → next real request → reload (logged) → answer
```

## Failure handling

| Failure | Behaviour |
|---|---|
| Offload times out (request in flight) | `{"error":"timeout"}`; the dialog shows it and the item stays loaded |
| Restore OOM (GPU still full) | The daemon stays `unloaded` and reloads on the next request; Orca shows the notification |
| Orca SIGKILL / crash | The restorer restores; if the restorer is gone too, the daemon drops the dead lease within 30 s and returns to its pre-lease state (reload if it was loaded) |
| Daemon restarted while pinned | Reads the leases file; with a live holder it starts with no GPU copy |
| Daemon restarted, lease lost | Normal preload. Orca's restore of an unknown lease is ok (idempotent) |
| Whisper request while pinned | `Error` `gpu-lent`; the gateway probe treats it as 202; Home Assistant shows an STT error until restore |
| Prompt timer `try-restart` during pin | Covered by persisted leases (lazy start) |
| NVML / socket / systemd-run missing | No offer; Orca works as today |
| Two Orca instances | Separate leases; the daemon restores to the GPU only when the last lease is gone |

## Measured timings and estimates

**Measured** on 2026-11-18. CPU-only scratch processes run at `nice 19`, with `CUDA_VISIBLE_DEVICES=` and the GPU untouched, 3 runs each. The whisper cold-start row comes from the journal. Values are median (min–max).

| Operation | Result |
|---|---|
| whisper `model.bin` 1.51 GiB read, cold (page cache evicted for that file only) | 0.50 s (0.50–0.50), 3.0 GiB/s |
| whisper `model.bin` read, page cache | 0.10 s (0.10–0.12) |
| laya `model.safetensors` 0.78 GiB read, cold / page cache | 0.26 s (0.26–0.27) / 0.05 s (0.05–0.06) |
| whisper service start → `Ready` (16 starts since 11-01, journal) | 2.69 s (1.92–12.15); ~10–12 s at boot; ~0.8 s of a warm start is HF Hub round trips |
| whisper first request after start (earlier journal analysis) | +0.45 s |
| classifier `import torch` | 0.94 s (0.91–1.60) |
| classifier `_load_laya` English, CPU, fp32 | 2.22 s (2.17–2.57) |
| classifier predict on CPU (2 threads, as the unit runs), first / warm | 3.12 s (3.10–3.21) / 3.13 s (3.12–3.32) |
| laya weights + buffers fp32 → bf16 (`model.to(torch.bfloat16)`, CPU, 0.34 s) | 1.569 GiB → 0.785 GiB |
| VRAM before (nvidia-smi now) | whisper 2224 MiB, classifier 2722 MiB, free 1957–1977 / 8188 MiB |

**Estimates.** No GPU scratch copies were loaded, because they would have OOMed the live daemons. These figures are replaced in rollout step 3.

| Operation | Estimate | Basis |
|---|---|---|
| whisper `unload_model(to_cpu=True)` | 0.2–0.5 s; frees ≈ 1.5 GiB + CUB cache | D2H at PCIe 4.0 x8, pageable |
| whisper `load_model()` from host copy | 0.2–0.6 s (+0.5–5 s if swapped out) | H2D 1.51 GiB |
| whisper reload after idle unload (from disk) | 0.8–1.2 s | 0.50 s read + H2D/init |
| classifier `.to("cpu")` + `empty_cache` | 0.2–0.5 s; frees ≈ 1.57 GiB + ~0.7 GiB autocast reserve | |
| classifier `.to("cuda")` fp32 / bf16 | 0.2–0.5 s / 0.1–0.3 s | |
| classifier cold process start on GPU | 4–5 s | 0.94 + 2.22 measured + CUDA init + H2D |
| classifier VRAM with bf16 weights | 1.2–1.4 GiB total | 0.785 GiB measured + context |
| residual VRAM after offload (CUDA context, cuBLAS/cuDNN) | 0.35–0.5 GiB per process | not measured |

## Test plan

- **task-cli (pytest, CPU):**
  - Offload, restore and pin state machine on a real small `torch.nn.Module`, with the device injected.
  - Lease reference counting and dead-holder pruning (a fake `/proc` start time); with the restorer absent, pruning the last lease reloads a prior-loaded model and leaves a prior-idle-unloaded one unloaded.
  - Pre-lease state: idle-unloaded → offload(lease) → restore(lease) leaves the model unloaded; loaded → offload → restore reloads it.
  - Lease persistence and lazy start after a restart, including restart-while-pinned for both prior-loaded (reloads on release) and prior-idle-unloaded (stays unloaded) epochs.
  - The idle timer with an injected clock.
  - In-flight drain and timeout: a busy/timeout offload leaves both the model state and the persisted lease set unchanged.
  - CPU serving while pinned.
  - The `capabilities` and legacy `predict` paths still return byte-identical replies.
- **aisis (pytest):**
  - The wrapper against a fake CT2 object with the real semantics: `unload_model` no-ops while busy, and using an unloaded model raises.
  - Probe context does not count as activity.
  - `/healthz/asr` returns 202 for `unloaded`, for `pinned` and for `gpu-lent`, and 503 still fires on a genuinely broken answer (existing `test_failover.py` cases stay).
- **snap-orca (Catch2):**
  - Decision gate: while the offload decision is pending, no toolpath GPU upload happens; "Free VRAM" uploads after the offload replies, "Not now" uploads immediately.
  - Grace timer: leave → re-enter at 59 s does not restore; a later continuous 60 s absence does.
  - State file round trip and atomic rewrite.
  - PID-reuse detection.
  - Protocol client against a fake socket server: ok, timeout, oom, unknown lease.
  - NVML loader: disabled cleanly when the library is missing.
  - Restorer helper mode: a fake holder process killed with SIGKILL → `restore` received.
- **Laptop integration (rollout step 3):** run `status`/`offload`/`restore` through each daemon's CLI. Record nvidia-smi per-pid before and after, plus the in-process timings, 3 cycles each. Crash test: a dummy holder (not Orca) takes a lease and is killed with SIGKILL. Orca's GUI flow is checked by the owner via `scripts/ubuntu2604/launcher-path/launcher-path.sh`.

## Rollout order and per-repo changes

1. **aisis gateway first:** `stt_wyoming.py` `/healthz/asr` handles `status`, the probe context and `gpu-lent` → 202, and treats a missing socket as today. It has to ship before the wrapper, or a `gpu-lent` answer would trigger failover restarts.
2. **Daemon idle unload:**
   - **task-cli** (`git.hyperide.ai/ultrabricks/task-cli`, installed as `hyper-task`): the control socket in `classify_server.py` (added to the `select` list), offload/restore/pin in the model registry, the idle-unload env var, and `task classify-server status|offload|restore`. Set the env var in `~/.config/systemd/user/task-classify-server.service` and its README section.
   - **aisis:** `wyoming_whisper_offload.py` with its idle unload and a `ctl` subcommand, plus the `95-pc.conf` `ExecStart` switch, in `services/yandex-station/systemd/`. The wyoming package itself stays unpatched; this is a wrapper.
3. **Measure in-process on the laptop.** Offload, then restore, timed inside each daemon (no second copy), 3 cycles, with nvidia-smi before and after. Replace the estimates table here.
4. **snap-orca:**
   - `src/slic3r/Utils/VramOffload.{hpp,cpp}`: NVML probe, protocol client, state file, restorer launch.
   - `src/slic3r/GUI/VramOffloadDialog.{hpp,cpp}`.
   - Hooks in `Plater` (preview enter and leave) and `GUI_App::OnExit`.
   - The `--vram-restore-watch` mode in `main()`.
   - AppConfig keys `vram_offload_policy` and `vram_offload_min_free_mb`.
   - Everything is guarded by `#ifdef __linux__`.
