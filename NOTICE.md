# Notice

OSSS is licensed under the MIT license. See [LICENSE](LICENSE) for the terms.
This file records what the project depends on and under what terms, which the
LICENSE file deliberately does not, so that automated license detectors see
nothing in LICENSE but the license itself.

## Third-party components

**There are no vendored components.** OSSS has no vendored third-party source
or binary libraries and no project package manager. It links only libraries
supplied through the operating system or platform SDK:

| Library | Used for |
| --- | --- |
| `d3d11`, `dxgi`, `d3dcompiler` | The capture, flow, fusion, and presentation path |
| `d3d9`, `d3d10`, `d3d12` | The deterministic test-animation source windows only |
| `dwmapi` | Compositor timing and window attributes |
| `windowsapp` | Windows Graphics Capture (the Windows Runtime capture APIs) |
| `comctl32`, `uxtheme` | The launcher's common controls and theming |
| `avrt`, `ole32`, `shell32` | Multimedia thread scheduling, COM, and shell paths |
| `Cocoa`, `CoreGraphics` | macOS window presentation and window capture |
| `X11`, `Xext`, optional `Xrandr` | Linux window enumeration, capture, presentation, input shaping, and refresh-rate discovery |

On macOS the portable target links the system Cocoa and CoreGraphics frameworks;
on Linux it links the system X11 and Xext libraries when the X11 backend is
enabled. Those are platform SDK libraries, not vendored third-party source or
binary dependencies. This zero-vendored-dependency property is deliberate. It
is most of why the codebase can be read end to end, and it is the reason a fresh
clone needs only the platform's normal C++/desktop development packages.

## Adding a dependency

Do not add one without opening an issue first — see
[CONTRIBUTING.md](CONTRIBUTING.md). If a dependency is ever adopted, record it
in the table above with its license, and confirm that license permits
redistribution under MIT before merging.

A machine-learning interpolation backend is a special case: it needs a license
audit of the model **and** its weights, which are frequently licensed
separately from, and more restrictively than, the code that runs them. See
[docs/ROADMAP.md](docs/ROADMAP.md).

## Contributions

Contributions are accepted under the MIT license that covers the rest of the
project. There is no CLA. By opening a pull request you agree your contribution
is licensed under those terms and that you have the right to submit it.
