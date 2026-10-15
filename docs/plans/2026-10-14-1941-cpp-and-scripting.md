# C++ and scripting

Date: 2026-10-14 19:41 EDT

## Context

The compositor in C++ under rules written for it, built by LLVM, and a path to a scriptable top bar and layout engine.
A scriptable top bar is a separate layer-shell client fed by protocols (foreign-toplevel, and ext-workspace for
desktops); a scriptable layout engine is an interpreter embedded in the compositor. meson stays the build system:
wlroots is a meson subproject built from source here.

## Decisions for the user

- [x] Language: C++26 on LLVM under rules of our own (decided by the user)
- [ ] Scripting language: Lua, JavaScript, or both (the user to decide)
- [x] The top bar as a separate layer-shell client, or drawn and scripted inside the compositor (decided: a separate Quickshell client — [2026-10-14-2140-top-bar.md](2026-10-14-2140-top-bar.md))

## Conversion

The classes change every cross-module call, so the modules convert in one step, in parallel slices against
headers written first, rather than one module at a time.

- [x] `platform/cpp-standards.md`: toolchain, what C++ carries, classes, code, names, hot paths
- [x] LLVM toolchain: `toolchain/llvm.ini`; `meson.build` refuses other compilers; `tools/check.sh` configures through the native file; wlroots built by clang
- [x] `src/core/wlr.h`: wlroots inside `extern "C"` with the two header hazards handled
- [x] `Listener`, `Handle` and `destroy_owned` in `src/core/util.h`
- [x] Class headers for the server, outputs, views, popups, decorations, layer surfaces, desktops, input, gestures, painter, hint, launcher, apps, icons and desktop entries, compiling together
- [x] Every module and test converted to the headers
- [x] The docs name the C++ code: code-style skill, build-and-run, wlroots, the intent index, README
- [x] Nix flake on `clangStdenv`: `nix build` compiles warning-free and passes the unit suite in its check phase
- [x] No behaviour change: a warning-free build at `-Wpedantic`, the unit and scenario suites pass with every check still run, a headless run with foot clients and a clean SIGTERM shutdown
- [x] Committed: the rules and docs, the conversion, the plans

## Scripting, once decided

- [ ] ext-workspace-v1 so an external bar can show and switch desktops (waiting on the bar decision)
- [ ] The embedded interpreter and the API a layout script sees (windows, desktops, outputs, the canvas), written into intents first (waiting on the language decision)
