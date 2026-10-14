# C++ and scripting

Date: 2026-10-14 19:41 EDT

## Context

The compositor in C++ under rules written for it, built by LLVM, and a path to a scriptable top bar and layout engine.
A scriptable top bar is a separate layer-shell client fed by protocols (foreign-toplevel, and ext-workspace for
desktops); a scriptable layout engine is an interpreter embedded in the compositor. meson stays the build system:
wlroots is a meson subproject built from source here.

## Decisions for the user

- [ ] Convert the compositor to C++23 under rules like `MilitaryForcesWorkspace`'s (the user to decide: go ahead now)
- [ ] Scripting language: Lua, JavaScript, or both (the user to decide)
- [ ] The top bar as a separate layer-shell client, or drawn and scripted inside the compositor (the user to decide)

## Conversion, once decided

- [ ] `platform/cpp-standards.md`: C++23, features used and avoided, classes and naming, comments, third-party code, adapted from `MilitaryForcesWorkspace`'s `engine/cpp-standards.md`; the `code-style` skill's C++ section (waiting on the conversion decision)
- [ ] One C++ header that includes wlroots inside `extern "C"` with the two header hazards handled (waiting on the conversion decision)
- [ ] A listener type that is removed with its owner, so no wlroots object is destroyed with a listener left (waiting on the conversion decision)
- [ ] Build as C++23 (meson `cpp`), the pure geometry and desktop-entry code first, unit tests green (waiting on the conversion decision)
- [ ] Server, outputs, views, popups, layer surfaces, desktops, launcher, gestures and input as classes, one module per step, `tools/check.sh` green after each (waiting on the conversion decision)
- [ ] No behaviour change: every scenario passes unchanged at the end (waiting on the conversion decision)

## Scripting, once decided

- [ ] ext-workspace-v1 so an external bar can show and switch desktops (waiting on the bar decision)
- [ ] The embedded interpreter and the API a layout script sees (windows, desktops, outputs, the canvas), written into intents first (waiting on the language decision)
