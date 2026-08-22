# Environment at capture

Captured: 2026-08-15

| Item | Value |
| --- | --- |
| OS | Windows 11 Pro 10.0.26200 |
| CPU | AMD Ryzen 7 7800X3D |
| GPU (used) | NVIDIA GeForce RTX 5090, driver 32.0.16.1074 |
| Primary display | 3840x2160 physical at 239 Hz (reported as `CurrentRefreshRate` 239); 2560x1440 logical, i.e. 150 % DPI scaling |
| Other displays/adapters present | AMD Radeon iGPU; a second display 3440x1440 at 174 Hz; several virtual display drivers (IddSampleDriver, SudoMaker, Sunshine, vorpX, Virtual Desktop) — none used by the test window, which opens at (32,32) on the primary display |
| Session | Console session, active (not RDP) |
| Overlay and injector software running | MSI Afterburner, RivaTuner Statistics Server (and its installer), NVIDIA GeForce Overlay. RTSS injects an on-screen display into Direct3D processes; no OSD was visible in any recorded frame, but its presence is noted because it could have affected them. Ordinary desktop applications were also open. |
| Desktop state | In interactive use throughout, not a clean benchmark bench. Two batches were obscured by other windows (Discord) and discarded. |

## Timing facts learned while setting up

- `osss.exe` compiles its HLSL at startup. On this machine the overlay window
  appears **10–15 s** after launch (13.1 s and 12.2 s in the recorded legs;
  17.7 s under desktop load), and `osss.exe --self-test`, which compiles the
  same shaders, takes 13–15 s. A burst scheduled before that scores the bare
  source, and `osss.exe` then dies with "The requested capture target is not a
  valid window" when the animation closes before it reaches `capture.Start`.
  Hence `--burst-at-ms 26000` in the recorded configuration.
- One BitBlt of the 960x540 client area costs **4.5–11 ms** here (per-row
  `capture_cost_ms`), against the default 10.3 ms burst interval at 60 FPS. The
  recorded bursts are close to capture-bound: the 20-capture span was 198–249 ms
  against a nominal 196 ms.
- Native captures trail the animation clock by 4–29 ms (DWM composition plus
  GDI capture); through OSSS the same figure is 31–57 ms. osss.exe's own stats
  put queue delay at ~24.7 ms and capture-to-present at ~26–31 ms, so the
  difference is OSSS's presentation latency, not the harness's.
- `osss.exe --list-windows` output stops at the first window whose title has a
  non-ASCII character (a typographic apostrophe in a browser tab title, in this
  run): `std::wcout` enters a failed state and prints nothing further.
  `--title` matching is unaffected (it is in-memory), so this is cosmetic, but
  it made the target window look missing during setup. Not fixed in this
  package.

## PowerShell notes for anyone re-running

- Both executables are console programs. Launch them with
  `ProcessStartInfo.CreateNoWindow = true` (as `run-burst-baseline.ps1` does),
  not `Start-Process`: a visible terminal lands over the test window and is
  captured, and `-WindowStyle Hidden` hides the animation's own window too
  because the STARTUPINFO show state overrides the first `ShowWindow` call.
- Passing `$null` to a P/Invoke `string` parameter from PowerShell yields `""`,
  not null; use `[NullString]::Value`. `FindWindowW("OSSS.FrameOutput", $null)`
  otherwise looks for a window with an empty title and returns 0 while OSSS is
  plainly running. The harness's own C++ `FrameOutputIsVisibleOver` is fine.
