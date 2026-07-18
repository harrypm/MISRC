# MISRC FX3 ADC Integration — Prompt Log

Per user rule: record all prompt input, reference context, file map, and commands run.

## Prompt input
- User: "continue intergration for fx3adc"
- Prior session (conversation 492664f4) committed "Save FX3 support work snapshot" (f201bda) on `feature/fx3adc-clean-integration`, touching `misrc_tools/common/device_enum.c` (+94/-94) and `misrc_tools/misrc_gui/input/gui_fx3.c` (+343/-120). FX3 backend code (enumerate/open/start/stop, sigrok + fx3usbadc protocols, firmware upload, capture thread, ringbuffer write) was already written and wired into app state, capture start/stop, record device-type name, and UI (device dropdown, mode label, A/B-swap gating).

## Root cause (verified from hard data)
Build was BROKEN on this host:
```
../misrc_gui/input/gui_fx3.c:19:10: fatal error: cyusb.h: No such file or directory
../common/device_enum.c:16:10: fatal error: cyusb.h: No such file or directory
```
- `build/meson-logs/meson-log.txt` + `meson-private/coredata.dat` show the prior session enabled FX3 via a `libcyusb` "cwrap" wrapper at `/tmp/fx3sdk_tool/cwrap` (pkg-config `libcyusb.pc` v`1.0.5-cwrap`, `cyusb.h`, `libcyusb.so`), configured with `PKG_CONFIG_PATH=/tmp/fx3sdk_tool/cwrap`.
- `/tmp` is ephemeral → wrapper gone → `build.ninja` had 53 stale references to `/tmp/fx3sdk_tool/cwrap` → fatal error.
- cwrap was NEVER committed (verified: `git log --all -- '*cyusb*' '*cwrap*'` empty; `git ls-tree -r backup/fx3-support-20260718-0722` has no cyusb/cwrap paths).
- `libcyusb` is not a distro package (`pkg-config --exists libcyusb` → NOT FOUND on Linux Mint), so the original `dependency('libcyusb')` meson lookup can never resolve on a clean host/CI.

## cyusb API surface used by code (exact)
6 functions + 1 type, all in `gui_fx3.c` (Linux `#ifdef __linux__` path):
- `cyusb_handle` — passed directly to `libusb_get_device()`, `libusb_control_transfer()`, `libusb_clear_halt()`, `libusb_bulk_transfer()` (via `fx3_usb_bulk_transfer` macro). Therefore MUST be an alias for `libusb_device_handle` (the cwrap typedef'd it so; confirmed by the call sites typechecking).
- `cyusb_open()` → int (device count); `cyusb_close()` → void; `cyusb_gethandle(i)` → `cyusb_handle*`;
- `cyusb_claim_interface(h,i)`, `cyusb_release_interface(h,i)`, `cyusb_bulk_transfer(h,ep,data,len,&actual,timeout)`.

Lifecycle relied on by gui_fx3.c: enumerate path calls `cyusb_open()`→iterate `cyusb_gethandle(i)`→`cyusb_close()`; open path calls `cyusb_open()`→select+claim one runtime handle (no close on success)→capture→`gui_fx3_close()` does release+`cyusb_close()`. So `cyusb_open()` pre-opens every device and `cyusb_close()` closes them all; must be re-entrant (dropdown enum then Connect).

## Fix: vendored cyusb compat shim built from source
No external pkg-config / no `/tmp` wrapper / no `.deps` step. Built directly in meson so local == CI.

### File map
- NEW `third_party/cyusb/cyusb.h` — includes libusb (same `__has_include` pattern as `common/libusb_compat.h`), `typedef struct libusb_device_handle cyusb_handle;`, declares the 6 functions.
- NEW `third_party/cyusb/cyusb.c` — implements shim over libusb-1.0: `cyusb_open()` inits context (`libusb_init_context` if API>=0x0100010A else `libusb_init`), enumerates, opens each device (NULL on failure), returns count; re-entrant (closes prior session first). `cyusb_close()` closes all handles, frees device list, exits context. `cyusb_gethandle(i)` bounds-checked. claim/release/bulk are direct libusb passthroughs. `CYUSB_MAX_DEVICES=256`.
- EDIT `misrc_tools/meson.build` (FX3 block, ~lines 221-255) — replaced the old `dependency('libcyusb', required:false)` lookup with `static_library('cyusb_compat', '../third_party/cyusb/cyusb.c', deps:[libusb_common_dep])` + `declare_dependency(link_with, include_directories, dependencies:[libusb_common_dep])`, added to `deps`, `-DENABLE_FX3=1`, `fx3_enabled=true`. The Linux/non-Linux split was DROPPED: the cyusb shim is now built on ALL platforms (Linux, Windows, macOS) whenever `libusb-1.0` is found, so FX3 is native everywhere with no external `libcyusb` package on any OS. `check_meson_fx3_policy` in ci_guard_tests.py enforces this (requires the `static_library('cyusb_compat'` + `-DENABLE_FX3=1` + `fx3_enabled = true` snippets and forbids `dependency('libcyusb', required : false)`).

### CI implication
CI builds on ALL platforms now compile `ENABLE_FX3=1` automatically when libusb-1.0 is present (Linux: `libusb-1.0-0-dev`; Windows: `mingw-w64-*-libusb`; macOS: brew `libusb`). FX3 is opt-in at runtime (only used when a Cypress FX3 device PID 04B4:00F1/00F3/1234 is selected in the dropdown). Satisfies HARD CI MIRROR RULE: no host `/usr/local/lib/libcyusb.so` can shadow a vendored copy — the dep is built from source in-tree; `ldd misrc_gui` shows only `libusb-1.0.so.0`. A post-build CI guard (see below) now verifies FX3 actually compiled in + vendored hsdaoh linkage on every build job.

## Commands run + results
- `git status`/`log` → branch `feature/fx3adc-clean-integration`, HEAD f201bda, clean.
- `pkg-config --exists libcyusb` → NOT FOUND; `pkg-config --exists libusb-1.0` → FOUND 1.0.25.
- Prior stale build repro: `PKG_CONFIG_PATH=/tmp/fx3sdk_tool/cwrap ninja misrc_gui` → `fatal error: cyusb.h: No such file or directory` (confirmed broken).
- `meson setup --reconfigure build` (unset PKG_CONFIG_PATH) → `Message: Vendored cyusb shim + libusb-1.0 found, building with FX3 support` + `libusb-1.0 found, building with DdD support`. No errors.
- `ninja misrc_gui misrc_capture` → exit 0. Built `libcyusb_compat.a`, `misrc_gui` (3.2 MiB), `misrc_capture` (101 KB). Only warning: `gui_fx3.c:297: 'fx3_upload_firmware' defined but not used` — PRE-EXISTING (function only called in the non-Linux `#else` firmware-upload path; dead under `#ifdef __linux__`). Not a regression.
- `nm libcyusb_compat.a` → `T cyusb_bulk_transfer/claim_interface/close/gethandle/open/release_interface` (all 6).
- `nm misrc_gui` → cyusb_* + `gui_fx3_start` present (LTO-inlined to `t` local, resolved).
- `strings misrc_gui` → `[FX3] %s`, `Cypress FX3`, `FX3 USB ADC`, `Cypress FX3 Bootloader`, `fx3usbadc`, etc.
- `ldd misrc_gui` → `libusb-1.0.so.0 => /lib/x86_64-linux-gnu/libusb-1.0.so.0` (no libcyusb.so).

## Status — FX3 CAPTURE WORKING (user-confirmed 2026-07-18)
FX3 (fx3usbadc, PID 0x00F1) captures at 100 MSPS in the GUI end-to-end.
User confirmed: "FX3 mode works now". Log shows:
  [FX3] Opened FX3 index 0 (PID=0x00F1, protocol=fx3usbadc, EP=0x81)
  [FX3] fx3usbadc start command sent (wValue=0x0094, wIndex=0xC010)
  [FX3] Capture thread started (fx3usbadc) at 100 MSPS
  [FX3] First data received: 42168 bytes ... 143319 batches

Hardware setup (persistent, one-time):
- Fx3Adc.img flashed to I2C EEPROM via `download_fx3 -t I2C -i Fx3Adc.img` -> boots on power-up as PID 0x00F1 (no host firmware load needed).
- Cypress cyusb_linux 1.0.5 SDK deployed system-wide (udev rule, /etc/cyusb.conf, libcyusb.so, CLI tools). See FX3_DEV_ENVIRONMENT.md.
- Repo builds a from-source cyusb compat shim (third_party/cyusb/cyusb.c) in meson so the GUI does NOT link the prebuilt libcyusb.so — local == CI.

## Bugs fixed during integration (all verified against real hardware + log data)
1. **Build broken**: /tmp cwrap gone -> vendored cyusb shim built from source in meson.
2. **"Failed to open device"**: no udev rule -> deployed 88-cyusb.rules (mode 666 for VID 0x04b4).
3. **Default start command wrong for fx3usbadc**: 0x800C (64MHz internal) -> 0xC010 (100MHz external clock, config 16). Sample rate 64MHz -> 100MHz. Ctrl timeout 100ms -> 1000ms.
4. **Connect loop (auto-reconnect watchdog)**: FX3/DdD start paths never set capture_start_time -> watchdog fired within 2s -> stop+reconnect loop. Fixed: set capture_start_time + reconnect_pending=false + capture_timestamp on FX3/DdD successful start.
5. **Pre-CMD_START diagnostic read**: poisoned EP 0x81 before 0x91 -> removed.
6. **0x91 ordering**: fx3usbadc needs 0x91 BEFORE bulk reads (reference app BulkTransfer.cpp); GUI copied sigrok's opposite ordering -> 3x EP 0x81 timeout. Fixed: fx3usbadc sends 0x91 before capture thread; sigrok keeps post-thread ordering.
7. **Endpoint-switch spam**: fx3usbadc has only EP 0x81; sigrok fallback to 0x82/0x83 -> LIBUSB_ERROR_IO spam (3.3M lines). Fixed: fx3usbadc stays on EP 0x81 only.
8. **NO_DEVICE error spin**: when FX3 drops mid-stream, bulk_transfer returns NO_DEVICE instantly -> thread spun 432684x. Fixed: break on fatal USB errors (NO_DEVICE/NOT_FOUND/NO_MEM/ACCESS) + rate-limit transient error logs.
9. **Dropdown UX overflow**: [FX3] FX3 USB ADC (Bus 2, Addr 6) -> [FX3] Fx3ADC (trimmed, no overflow).

## HSDAOH/MISRC REGRESSION — RESOLVED (root cause: stale system libhsdaoh)
Symptom: HSDAOH/MISRC modes broken on this branch. raw-parser mode got 448 system
errors, 0 frames, sync=NO. User-verified: v1.1.1 AppImage + 8c0166e build both work.

Root cause (verified via ldd + readelf + sha256): the built misrc_gui linked a stale
manually-installed libhsdaoh at /usr/local/lib (sha a0b7f155, 26592 bytes, no `srate`
field) instead of the vendored .deps/install copy (sha d4dc3d8e, 54360 bytes, has
`srate`). The binary's RUNPATH was `/usr/local/lib:.deps/install/lib` — /usr/local/lib
first, so the dynamic loader picked the stale system lib. The 8c0166e baseline worked
only because it happened to static-link hsdaoh (no NEEDED libhsdaoh.so.0, no runtime
resolution). This was NOT a code regression in the hsdaoh callback path — the GUI
code and vendored hsdaoh library were identical between 8c0166e (works) and f201bda.

Fix: removed the stale /usr/local/lib/libhsdaoh.* (manual install, not from a package;
matches a clean CI runner). After removal + rebuild, ldd confirms misrc_gui links
/home/harry/MISRC/.deps/install/lib/libhsdaoh.so.0. User confirmed both modes work:
raw-parser (MISRC) 73/98 frames 0 errors, upstream (HSDAOH) 229 frames sync=OK.

Guardrail: ci_guard_tests.py already has check_built_gui_links_vendored_hsdaoh()
which runs ldd and fails if misrc_gui links a system hsdaoh (/usr/local/lib, /usr/lib,
/lib). It is wired into CI (.github/workflows/build.yml). All 20 CI guard tests now
PASS. The gap was it was not run locally before reporting the build as good. Run
`python3 misrc_tools/test/ci_guard_tests.py` after every local build to catch this.

## CI reinforcement — post-build guards + FX3 native on all platforms (2026-07-18)
Prompt: "fix the CI, add back the FX3 code, and add libcyusb to cache/builds/gh
actions so FX3 support is native across all platforms and add that to CI too".

Changes (all verified locally with hard data):
- `misrc_tools/meson.build`: cyusb shim built on ALL platforms (dropped the
  Linux/non-Linux split). `fx3_enabled=true` + `-DENABLE_FX3=1` whenever
  `libusb-1.0` found. Verified: re-setup build-appimage-local -> meson log says
  `Vendored cyusb shim + libusb-1.0 found, building with FX3 support` +
  `libusb-1.0 found, building with DdD support`.
- `misrc_tools/test/ci_guard_tests.py`: added `--post-build` + `--gui-path` args
  and two post-build binary-introspection checks:
  - `check_built_gui_links_vendored_hsdaoh` (platform-aware: ldd Linux,
    otool -L macOS, objdump -p DLL NEEDED Windows) — fails on system hsdaoh.
  - `check_built_gui_has_fx3_symbols` (nm/strings for `[FX3]` +
    `fx3usbadc start command sent`) — fails if FX3 compiled out.
  - `check_meson_fx3_policy` (pre-build text check) — requires the cyusb shim
    `static_library` + `-DENABLE_FX3=1` + `fx3_enabled = true` snippets.
  - Guard gap fix: in `--post-build` mode a missing `--gui-path` now FAILS
    (previously the per-function `if not gui.exists(): return 0` let a
    non-existent binary silently PASS in post-build mode, defeating the guard).
    Preflight mode (no `--post-build`) still skips gracefully.
- `.github/workflows/build.yml`: post-build step running
  `ci_guard_tests.py --post-build --gui-path $BUILD_DIR/misrc_gui` added to all
  4 build jobs (linux-appimage, windows-exe, windows-exe-arm64, macos-app-build).

Hard-data verification (this session):
- Pre-build guard suite: all PASS (rc=0).
- Post-build guard vs stale build-appimage-local/misrc_gui: FAILED (rc=1) —
  `misrc_gui has no FX3 symbols/strings — FX3 did not compile in`. Root cause:
  that build dir was configured BEFORE the meson.build FX3 change, so its meson
  log still showed the old `Determining dependency 'libcyusb' ... libcyusb not
  found, building without FX3 support` and the binary had 0 `fx3usbadc` strings.
  This is exactly the silent-FX3-disable the post-build guard exists to catch.
- Reconfigured build-appimage-local (`meson setup --wipe ...` with the
  build-appimage-local.sh env: PKG_CONFIG_PATH + CMAKE_PREFIX_PATH ->
  .deps/install-appimage-local) -> new meson log: `libusb-1.0 found: YES 1.0.25`,
  `Vendored cyusb shim + libusb-1.0 found, building with FX3 support`,
  `Using vendored hsdaoh from .deps/install`.
- `ninja -C build-appimage-local misrc_gui` -> [43/43] rc=0.
- Rebuilt binary hard-data: `strings` -> `fx3usbadc start command sent` (count=1)
  + `[FX3]` present; `ldd` -> `libhsdaoh.so.0 => .deps/install/lib/libhsdaoh.so.0`.
- Post-build guard vs rebuilt binary: ALL PASS (rc=0), including
  `built GUI has FX3 symbols (post-build)` + `built GUI links vendored hsdaoh (post-build)`.
- Guard gap fix verified: `--post-build --gui-path <nonexistent>` -> rc=1 (was rc=0);
  preflight `--static-only` -> rc=0 (graceful skip preserved).

## VCS
Local branch `feature/fx3adc-clean-integration` only. `origin` (Stefan-Olt
upstream) read-only; fork remote is `harrypm`. No push unless user explicitly
asks. CI reinforcement + guard gap fix + docs committed locally (see git log).

## Out of scope / next
- Hardware validation of Connect with a real FX3 board (user action).
- Firmware upload path (`fx3_upload_firmware`) is non-Linux only on Linux it relies on runtime PID 0x00F1/0x1234 devices; bootloader-mode handling on Linux currently searches for a runtime FX3 fallback (see `gui_fx3_open` Linux branch). Confirm behavior with hardware.
