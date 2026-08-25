# Security Policy

## Supported versions

OSSS is pre-1.0 and experimental. The latest GitHub release and current `main`
branch are supported; there are no maintained release branches or backports to
older tags.

## Reporting a vulnerability

Please report privately rather than opening a public issue:

**[Open a private security advisory][advisory]** on this repository.

Include what you can of: the affected file or subsystem, what an attacker gains,
the conditions needed (a specific target window, a hostile capture source, a
crafted profile file, a particular GPU or driver), and a reproduction. A crash
with a stack trace is a perfectly good report.

You should get an acknowledgement within about a week. This is a hobby project
maintained in spare time, so please do not expect a same-day response, and
there is no bug-bounty program.

## What is in scope

OSSS runs entirely as an unprivileged local process. The realistic attack
surface is:

- **Parsing untrusted input.** `src/app_profile.cpp` reads
  `%LOCALAPPDATA%\OSSS\profiles.txt`, `src/ui_mask.cpp` parses mask rectangles
  from the command line, and `src/shader_cache.cpp` loads compiled bytecode
  from a cache directory. A malformed or hostile file causing memory
  corruption is a real bug.
- **Captured frame data.** The pipeline consumes pixels from another
  application's window. Anything in the capture or interpolation path that can
  be made to read or write out of bounds by the *contents* or *dimensions* of a
  captured frame is in scope.
- **Writing outside expected locations.** The dump paths
  (`--dump`, `--dump-sequence`, test captures) and the shader cache write files;
  a path-traversal or arbitrary-overwrite bug in any of them is in scope.

## What is not in scope

- **The overlay is not a security boundary.** OSSS presents a click-through,
  topmost window over another application. It is not sandboxing, hiding, or
  protecting anything, and it is not designed to resist a hostile target
  process — which, being a normal process on the same desktop at the same
  privilege level, could interfere with it in a dozen ways regardless.
- **Anti-cheat and DRM interactions.** OSSS uses the operating system's desktop
  capture API and injects no code into the target, but some games and DRM
  systems will still object to a topmost overlay or to being captured. That is
  a compatibility matter, not a vulnerability, and OSSS will not accept
  changes whose purpose is to evade such detection.
- **Bugs requiring administrator privileges or physical access** to exploit.
- **Vulnerabilities in an operating system, GPU driver, or platform SDK.** Report
  those to the relevant vendor. If OSSS can *trigger* one from untrusted input,
  that part is in scope here.
- **Crashes in the test and benchmark harnesses** driven by deliberately
  malformed developer flags. Those are ordinary bugs; open a normal issue.

## Disclosure

Coordinated disclosure is preferred: give the maintainers a reasonable chance
to ship a fix before publishing. Given the project's size, 90 days is more than
reasonable and less is often fine. You will be credited in the advisory and the
release notes unless you would rather not be.

[advisory]: https://github.com/Lenovive/OSSS/security/advisories/new
