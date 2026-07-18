# FX3 Development Environment (Cypress cyusb_linux 1.0.5 SDK)

This documents the persistent Cypress FX3 dev setup on this host, so FX3
capture development and hardware testing work without the ephemeral
`/tmp/fx3sdk_tool/cwrap` hack the earlier session used (which broke the build
after reboot — see `misrc_fx3_integration_prompt.md`).

## SDK location
`/home/harry/cyusb_linux_1.0.5/` (extracted from `/home/harry/Downloads/fx3.tar`,
inner archive `cyusb_linux_1.0.5.tar.gz`). User-owned and persistent.

Contents:
- `bin/cyusb_linux` — Cypress Control Center GUI (NOT usable: needs Qt4, unavailable on this Mint).
- `lib/libcyusb.so.1` (+ `libcyusb.so` symlink) — the real Cypress cyusb 1.0.5 library over libusb-1.0.
- `include/cyusb.h` — the real Cypress header. API verified bit-faithful to the repo's from-source shim (`third_party/cyusb/cyusb.h`): both `typedef struct libusb_device_handle cyusb_handle;` and the same 6 function signatures.
- `src/` — CLI tools built from source (no Qt4 needed): `download_fx3`, `08_cybulk`, `09_cyusb_performance`, `01_getdesc`, `03_getconfig`, `04_kerneldriver`, `05_claiminterface`, `06_setalternate`, `00_fwload`, `cyusbd`, `config_parser`, plus our `fx3_streamtest.cpp`.
- `fx3_images/` — Cypress FX3 firmware images (`cyfxbulksrcsink.img`, `cyfxbulklpautoenum.img`, etc.).
- `deploy.sh` / `uninstall.sh` — system install/remove (run with sudo).

## System deploy (run once)
`sudo bash /home/harry/cyusb_linux_1.0.5/deploy.sh` installs:
- `/etc/udev/rules.d/88-cyusb.rules` — grants mode 666 to all VID 0x04b4 devices (fixes "failed to open device" as non-root).
- `/etc/cyusb.conf` — VPD list (Cypress tools refuse to run without it).
- `/usr/local/lib/libcyusb.so.1` (+ symlink) + `ldconfig`.
- `/usr/local/bin/cy_renumerate.sh` — hotplug helper.
- `/etc/profile.d/cyusb.sh` — exports `CYUSB_ROOT=/home/harry/cyusb_linux_1.0.5` (source it or re-login).
- Symlinks the CLI tools onto `/usr/local/bin` (on PATH).

Remove with `sudo bash /home/harry/cyusb_linux_1.0.5/uninstall.sh`.

## CLI tools for FX3 testing (no Qt4 needed)
- `download_fx3 -t RAM -i <img>` — load firmware to RAM (non-destructive; does NOT touch SPI flash). Targets: RAM / I2C / SPI.
- `fx3_streamtest [seconds]` — our tool: reads bulk IN EP 0x81 for N seconds, reports packets/bytes/MB/s + first 16 bytes. Use to prove the FX3 streams.
- `08_cybulk -t <secs>` — Cypress bulk loopback test (hardcoded EP 0x86/0x02; use only if the loaded image matches those endpoints).
- `09_cyusb_performance` — Cypress performance benchmark.
- `01_getdesc` / `03_getconfig` — read device/config descriptors.

## Proven streaming (hard data, 2026-07-18)
Device was in factory DFU mode (PID 0x00F3, iProduct="WestBridge"). Loaded
`cyfxbulksrcsink.img` to RAM via `download_fx3 -t RAM`; device re-enumerated as
PID 0x00F1 (FX3 Streamer, EP 0x81 IN / 0x01 OUT, 1024-byte packets). `fx3_streamtest 3`:
```
FX3 opened: VID=04B4 PID=00F1
Claimed interface 0. Reading EP 0x81 for 3 s...
Received 74744 packets, 76537856 bytes in 3.000 s -> 25.51 MB/s
First 16 bytes of last packet: AA AA AA AA AA AA AA AA AA AA AA AA AA AA AA AA
```
=> FX3 hardware + USB streaming path works end-to-end as user harry (no root).

## Important: device boots into DFU mode
`lsusb` shows PID 0x00F3 (factory bootloader), iManufacturer="Cypress",
iProduct="WestBridge" — no runtime firmware is running on power-up. This is why
the MISRC GUI said "Failed to open device": `gui_fx3_open` on Linux looks for a
runtime FX3 (PID 0x00F1/0x1234) and only finds the DFU device. To capture real
ADC data the FX3 must be running its ADC firmware (e.g. the MISRC
`Internal_clock_16bit/cypress-fx3.fw` from `MISRC-cypress-fx3_firmware.zip`,
which enumerates as PID 0x00F1 fx3usbadc). Either:
1. Load ADC firmware to RAM each session: `download_fx3 -t RAM -i <adc.img>` (non-destructive), or
2. Flash it to SPI flash so it boots on power-up: `download_fx3 -t SPI -i <adc.img>` (persistent), or
3. Check the PMODE/boot jumper if flash was already written but the device still enters DFU.

## Repo build vs this SDK
The MISRC repo does NOT link the prebuilt `libcyusb.so`. It builds a from-source
compat shim (`third_party/cyusb/cyusb.c`) over libusb-1.0 in meson on ALL
platforms (Linux, Windows, macOS) whenever `libusb-1.0` is present, so local and
CI builds are identical and need no external pkg-config anywhere. FX3 is native
on every platform: no Linux-only `libcyusb` package, no system-lib shadowing.
This SDK is for standalone hardware testing/udev/firmware, not for building the
GUI. See `misrc_fx3_integration_prompt.md` for the build details.

## CI post-build guard (catches silent FX3-disable + vendored-dep shadowing)
`misrc_tools/test/ci_guard_tests.py --post-build --gui-path <built misrc_gui>` is
run AFTER the build in all 4 CI jobs (linux-appimage, windows-exe,
windows-exe-arm64, macos-app-build). It binary-introspects the real misrc_gui:
- `built GUI links vendored hsdaoh (post-build)` — ldd/otool/objdump asserts the
  binary links the vendored `.deps/install` hsdaoh, not a stale system lib
  (platform-aware: ldd on Linux, otool -L on macOS, objdump -p DLL NEEDED on Windows).
- `built GUI has FX3 symbols (post-build)` — nm/strings asserts `[FX3]` +
  `fx3usbadc start command sent` are present, i.e. `gui_fx3.c` compiled in
  (`-DENABLE_FX3=1`). Catches a silent FX3-disable where libusb-1.0 was missing.
In `--post-build` mode a missing `--gui-path` FAILS (does not silently skip), so a
broken build step can never pass CI without validating linkage + FX3. Preflight
mode (no `--post-build`) still skips gracefully when no local build exists.
Run locally: `python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-appimage-local/misrc_gui`.
