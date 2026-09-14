# WiiCompiled — ingame-playable push: session notes

Goal (task #7, in_progress; tasks #1–#6 complete): reach an ingame playable build.
Pivot: desktop-first. Branch: `usa-windows` → `origin/usa-windows` (tip `065243c`).
Target exe: `build/desktop-usa/WiiCompiled.exe` (~79 MB), built from `C:/mkw-ws/runtime`
+ `C:/mkw-ws/generated` shards (`MKW_TRANSLATED_SHARD_MANIFEST=C:/mkw-ws/generated/build_shards/shards.cmake`).
Config: `%LOCALAPPDATA%/WiiCompiled/Config.toml`, `dvd_root=C:\code\Wiicompiled\Assets-USA\DATA` (USA `RMCE01`).
Logs: `%LOCALAPPDATA%/WiiCompiled/Logs/base_*/console.log`.

Upstream: `https://github.com/patchzyy/Wiicompiled` (tracked locally as `upstream-main`,
tip `6458ec6`). Local diverged ~57 ahead / 55 behind; no rebase attempted (too invasive mid-debug).

## Status: NOT PLAYABLE — black screen, guest alive but parked in disc-error branch

Live signal (pid 40600, USA DATA):
- `presented≈+150–300/5s`, `viadv≈vipost≈viret` advancing, PC cycles
  `0x8016FC38` (GX::CopyDisp HLE) / `0x8020FF98` (AsyncDisplay::beginRender) / `0x801AA9B8` (OSSleepThread).
- `chain[run=1 rk=0 sm=0 cc=0 sc=0 sd=0 se=1 sk=0 de≈dh≈presented]`, `gates[g106=1 g81=1]`,
  `dvd[A=1 B=0 C=0]`, `dvd[dth=0 dst=1 ret=0xffffffff dcs=1 dec=0 drd=2 dse=0]`.
- `dco[]` (slice 9): `obj=0x8042e930 vt=0x80270bc0 run=0x80008D18 os=0x90112660
  osst=1(READY) susp=0 ospc=0x8024373c oslr=0x801aa0f0 w72=0x5 w80=0x1010000` —
  DvdThread object/vtable/OSThread valid, parked at `EGG::Thread::Run` trampoline,
  never reaches its first `GetDriveStatus` poll.

## Root cause traced so far (translated bodies read)

- `RKSystem::Run (0x8000951C)`: per-frame `+81` gate → `0x80008E74 DiscInfo_printError`
  branch, never the scene-continue path.
- `DvdThread_main (0x80008D18)`: `GetDriveStatus → +72/+80/+81` gates → `VIWaitForRetrace` loop.
- `GetDriveStatus (0x80162B50)` / `GetCommandBlockStatus (0x80162A88)` /
  `cbForStateError (0x8015EE70)` / `stateReady (0x80161614)` decision trees mapped.
- Region mismatch proven: PAL translation (PAL SDA/MAP/HLE) on USA `RMCE01` DATA →
  `SimpleAddressData fail`, `Matching Area: 2`, empty `eu/` strap dir (shimmed with `us/` copy).

## USA slices committed (all in `usa-windows`, pushed)

1. `system_bridge.cpp` + `os_init.cpp`: disc-code lowmem + NTSC TV format follow disc.
2. `sc.cpp`: Area/GameRegion/ProductCode (`LU` vs `LEH`) follow disc region byte.
3. `dvd.cpp`: `CurrentDiscGameCode()` reads `DATA/sys/boot.bin` directly.
4. `vi.cpp`/`hle_stubs.h`/`settings_overlay.cpp`: retire boot cover on first XFB, unguarded counters.
5. Desktop watchdog thread (5s): PC + presented/retraces + chain + gates.
6. Watchdog Run gates (`g104/105/106/107/108/g81`) + DVD gate words (`A/B/C`).
7. DVD seed-once (reseed-every-call forced the `cbForStateError` path).
8. DVD liveness in watchdog (`dth/dst/ret/dcs/dec/drd/dse`).
9. `dco[]` probe: DiscCheckThread object state in watchdog.
Also: merged upstream `c2289e4` (`os_sleep.cpp`).

## TODO

1. DvdThread start: why `dth=0` with READY/susp=0 fiber `T` — trace `EGG::Thread::__ct` →
   `OSCreateThread` → `OSResumeThread` → `FiberProc` EGG-start deferral (`vtable+12`); fiber likely never switched in.
2. Cause vs symptom: whether `g81=1` disc-error branch is caused by the dead DvdThread or vice versa;
   capture `GetDriveStatus` return distribution (`ret` stuck `0xffffffff`).
3. Audit Run/calc direct-call targets for HLE/translated/missing status.
4. True USA fix blocked on: real USA MAP (placeholder is a PAL copy — breaks boundaries,
   e.g. HID at `0x8012E598` vs PAL `0x8012E638`) + no .NET SDK to rebuild translator +
   ~582 HLE addresses to re-derive.

## Environment notes

- Build: `C:/mkw-toolkit/tc` (cmake, ninja, llvm-mingw), `MKW_CPPWINRT_INCLUDE_DIR` =
  `C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt`.
  Rebuild: `ninja WiiCompiled` in `build/desktop-usa`; sync edited runtime files to `C:/mkw-ws/runtime` first
  (plain copies, not junctions). `C:/mkw-ws/generated` (15283 fns) ≠ repo `generated` (29637 fns).
- Android parked: phone `d1cadee8` (sdm845), scrcpy at `C:\Program Files (Portable)\scrcpy\scrcpy.exe`
  — run `-s d1cadee8 -S --power-off-on-close --window-title Wiicompiled --no-audio` when needed.
- Standing: commit regularly + push, `git fetch origin`, check upstream for updates.
