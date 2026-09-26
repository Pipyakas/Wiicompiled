# WiiCompiled — ingame-playable push: session notes

Goal (task #7, in_progress; tasks #1–#6 complete): reach an ingame playable build.
Pivot: desktop-first. Branch: `usa-windows` → `origin/usa-windows` (tip `065243c`).
Target exe: `build/desktop-usa/WiiCompiled.exe` (~79 MB), built from `C:/mkw-ws/runtime`
+ `C:/mkw-ws/generated` shards (`MKW_TRANSLATED_SHARD_MANIFEST=C:/mkw-ws/generated/build_shards/shards.cmake`).
Config: `%LOCALAPPDATA%/WiiCompiled/Config.toml`, `dvd_root=C:\code\Wiicompiled\Assets-USA\DATA` (USA `RMCE01`).
Logs: `%LOCALAPPDATA%/WiiCompiled/Logs/base_*/console.log`.

Upstream: `https://github.com/patchzyy/Wiicompiled` (tracked locally as `upstream-main`,
tip `6458ec6`). Local diverged ~57 ahead / 55 behind; no rebase attempted (too invasive mid-debug).

## Status: NOT PLAYABLE — fatal #6/#7 DVD hangs cleared; fatal #8 TRK missing-target patched pending retest

### Fatal #7 (after #6): `func_80165990` int3
- Same pattern: real `b .` at `0x801659FC` / `0x80165A1C` → empty self-goto → clang deleted → `int3` at `func_80165990+0x67C`.
- Call chain: `__start` → `OS::Init` → `DVD::InquiryAsync` → `stateReady` → `func_80161574` → `func_8016189C` → `func_80165990`.
- Fix: force success edges `loc_801659FC→loc_80165A00` and `loc_80165A1C→loc_80165A20` (DAED cover check bypass). Batch-patched **43** DVD/OS-range `func_8016*`/`func_801A*` empty self-loops the same way (`// Unblock USA boot: was empty self-loop …`).
- **emit-build-shards cache gotcha:** metadata `sourceBundlePath` pointed at `base_translation_sources.bin` (430MB); emit preferred the stale bundle over edited `functions/*.cpp`. Must set `"sourceBundlePath": null` in `base_translation_output.json` (and refresh per-function `size`/`sha256`) before re-emit, or edits never land in shards. Bundle still on disk for reference.

### Fatal #8 (after #7): missing target `0x80020684`
- `InvokeIndirectCpu: target 0x80020684 not translated` from `OS::IsTitleInstalled` body at `func_801AE4A4` walking `_ctors` table at `0x80244D40` (`bctrl` via CTR).
- USA DOL real prologues in TRK range that PAL MAP does **not** label: `0x80020684`, `0x800206C0`, `0x800207F0` (MAP stops `TRKTargetSupportRequest` at `0x80020638` which is mid-function on USA).
- Fix: HLE stubs `TRKTargetSupportRequest_HLE_80020684` / `…800206C0` / `…800207F0` in `runtime/src/hle/trk.cpp` + `REGISTER_NATIVE_FUNCTION` (USA). Re-emit + exclusive ninja after native reg change.

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

## Boot fatals fixed this slice (USA desktop)

- Fatal #1–#4: EXIInit USA `0x80168F00` + EXI alias block; AI MMIO poll stub
  `AIHwPollStub_801A12B8`; MEM soft-MMIO `0xCC004000-0xCC004FFF`
  (`IsMemMmioAddress` + `ProtectRange`); PCH rebuild after `memory_access.h` edit.
- Fatal #5: `InvokeIndirectCpu: target 0x800149A0 not translated`.
  DOL: `0x80014990` is prior-function epilogue (lwz/mflr/addi/blr); real
  `atof` prologue is `0x800149A0`. Data tables store `80 01 49 A0` at
  `0x8026C9F0/CA40/CA90`. MAP+recomp.yml corrected; HLE
  `Atof_HLE_800149A0` registered in `runtime/src/hle/c_stdio.cpp`.
- Fatal #6: structured exception `0x80000003` at `func_801668C4+0x677` (`int3` after call `func_801A2530`); guest CTR/LR showed DVD path; host stack captured after moving `DumpHostStackTrace`/`WriteFatalLogImpl` BEFORE `ShowRuntimeFatalPopup` in `runtime/src/main.cpp` `ReportFatalSehAndExit`.
  - Guest DOL truth: `0x80166958` and `0x80166978` are real `b .` (`0x48000000`). Translator emitted empty `goto` self-loops; clang deleted them as unreachable → `int3` fall-through.
  - Root cause of taking hang path: USA DI cover context at `0x8033F160` unseeded (`r6+12=0` not `0xFEEBDAED`), `cmplwi` vs `0xDAED` fails; also busy flag path. PAL MAP placeholder / merged Low* functions; native override `0x80166964` bypassed because entry is mid-merge `func_801668C4` via `InvokeDirectCpu` from `func_8016189C`.
  - Fixes: (1) `main.cpp` crash dump order; (2) `func_801668C4.cpp` volatile self-spin at `0x80166958`/`0x80166978` + forced gotos to `loc_8016695C` / `loc_8016697C` / `loc_80166A0C` success edges; (3) `CxxLinearCodeGenerator.EmitGotoUnlessFallthrough` emits side-effecting spin for self-jumps + `SelfLoopCodeGenTests.cs`.
  - `emit-build-shards` MUST re-run after editing `generated-usa/functions/*.cpp` because bodies are `#line`-included into `build_shards/base_common/shard_*.cpp`.
- USA DOL is non-standard 7-text/11-data layout (`DolFile.cs` constants);
  entry `0x800060A4`, T0 `0x80004000+0x2460`, T1 `0x800072C0+0x23DA80`.
- PowerShell: never cast `0x80…` literals to `[uint32]` (Int32 overflow);
  use `[uint32]::Parse(hex, HexNumber)` or byte-wise BE assemble.

## TODO

1. Re-run emit-build-shards + exclusive ninja after native reg changes; update NOTES after each slice.
2. DvdThread start / disc-error chain (prior notes) once boot clears remaining missing-target fatals.
3. True USA fix blocked on: real USA MAP (placeholder is a PAL copy) + ~582 HLE addresses to re-derive.

## Environment notes

- Build: `C:/mkw-toolkit/tc` (cmake, ninja, llvm-mingw), `MKW_CPPWINRT_INCLUDE_DIR` =
  `C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/cppwinrt`.
  Rebuild: `ninja WiiCompiled` in `build/desktop-usa`; sync edited runtime files to `C:/mkw-ws/runtime` first
  (plain copies, not junctions). `C:/mkw-ws/generated` (15283 fns) ≠ repo `generated` (29637 fns).
- Android parked: phone `d1cadee8` (sdm845), scrcpy at `C:\Program Files (Portable)\scrcpy\scrcpy.exe`
  — run `-s d1cadee8 -S --power-off-on-close --window-title Wiicompiled --no-audio` when needed.
- Tooling / PowerShell caveats: PowerShell cannot take python heredocs; write `.py` to `%TEMP%` and run it; DOL parse needs BE32; never cast `0x80…` to `[uint32]` literals in params.
- Standing: commit regularly + push, `git fetch origin`, check upstream for updates.
