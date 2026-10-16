# Plans

Every plan for wayland-canvas is a file in this folder named `YYYY-MM-DD-HHMM-<slug>.md` (local time the plan
first appeared), with a `Date:` line under its title and a checkbox list that is ticked and committed as each task
finishes. The `plan-checklists` skill has the rules; `tools/check-plans.sh` checks them and
`tools/check-plans.sh --fix` refreshes the counts below.

A plan is volatile: tasks, boxes, decisions waiting for the user. What a plan settles about the compositor is
written into [../intents/](../intents/README.md), and the plan links to it.

"What is left" is the Open column. Boxes marked for the user, waiting or owed on screen need a person; the rest can
be worked.

| Plan | Covers | Done | Open |
|---|---|---|---|
| [2026-10-14-1550-infinite-canvas-compositor.md](2026-10-14-1550-infinite-canvas-compositor.md) | The compositor: canvas, cameras, desktops, overview, gestures, Windows-style title bars, window management, layer shell, scale, scenarios; on-screen runs owed; undecided extensions | 28 | 9 |
| [2026-10-14-1715-app-launcher.md](2026-10-14-1715-app-launcher.md) | The Start-style launcher, desktop entries, icons, custom entries the standard way; the taskbar decision | 10 | 2 |
| [2026-10-14-1802-triple-helix-adoption.md](2026-10-14-1802-triple-helix-adoption.md) | Intents, tests, plans, skills, roles and hooks adopted from `MilitaryForcesWorkspace`, generic to whoever works | 12 | 1 |
| [2026-10-14-1803-code-review-findings.md](2026-10-14-1803-code-review-findings.md) | The reviewer's four bugs and two checks; scenarios still owed for drag-and-drop, output unplug and mixed scales | 6 | 3 |
| [2026-10-14-1941-cpp-and-scripting.md](2026-10-14-1941-cpp-and-scripting.md) | C++26 on LLVM under rules of our own: toolchain, wlroots wrapper, classes, the conversion; scripting the top bar and layout engine waits on the user's decisions | 12 | 3 |
| [2026-10-14-2140-top-bar.md](2026-10-14-2140-top-bar.md) | The top bar: a Quickshell client per output, its widgets and click actions; the control IPC and the msg client; autostart and the off flag | 7 | 0 |
| [2026-10-14-2151-domain-classes.md](2026-10-14-2151-domain-classes.md) | Classes per concept: desktops named by ID, camera and overview per output, gestures and pointer grabs as variant state machines, bindings | 10 | 0 |
| [2026-10-14-2150-packaging.md](2026-10-14-2150-packaging.md) | The .pkg.tar.zst and .deb build targets, the session file, the default-session tool and its rules per display manager, the installer flow, removal guarantees | 14 | 0 |
| [2026-10-15-0719-bar-panels.md](2026-10-15-0719-bar-panels.md) | A panel under every bar indicator: Wi-Fi, Bluetooth, audio, brightness, power, weather, calendar, tray menus | 15 | 0 |
| [2026-10-15-1156-source-layout.md](2026-10-15-1156-source-layout.md) | `src/` in one folder per area, includes written from `src/`, the placement rule for new files | 6 | 0 |
| [2026-10-15-1244-plugin-framework.md](2026-10-15-1244-plugin-framework.md) | The shell as a plugin host, the bar and panels as plugins, the overview toolbar, sticky notes, the light/dark switch, the JSON control socket with canvas items | 22 | 0 |
| [2026-10-15-1251-desktop-scale-and-tiling.md](2026-10-15-1251-desktop-scale-and-tiling.md) | A scale per desktop set with Super + 2-finger pinch, windows usable and sharp at it; tiling as a switch per desktop that keeps new windows tiled | 14 | 0 |
| [2026-10-15-2056-desktop-scale-as-scaling.md](2026-10-15-2056-desktop-scale-as-scaling.md) | A desktop's scale scales its picture: windows keep their size and layout, clients are never told; Super+0 and `0` in the overview reset to 100 %; the bar-collapse bug | 16 | 1 |
| [2026-10-16-0944-wifi-and-claude-panels.md](2026-10-16-0944-wifi-and-claude-panels.md) | The network panel after the other shell's: live figures, band, DNS provider, known and other networks; an AI usage panel for every family used, Claude and GPT first, with limits, tokens by day and by model | 0 | 17 |
| [2026-10-16-1003-normal-view-hops.md](2026-10-16-1003-normal-view-hops.md) | A 3-finger swipe in the normal view hops like the overview's to the nearest desktop that way, and stays put with none; the free pan with momentum goes | 7 | 1 |
| [2026-10-16-1029-tiling-slices.md](2026-10-16-1029-tiling-slices.md) | Tiled desktops hold windows in slices — the whole, halves, quarters — moved and resized by Super+arrows like Windows' snap keys; growth pushes windows into free space; new windows take the largest free slice | 5 | 1 |
| [2026-10-16-1404-screen-capture-and-sharing.md](2026-10-16-1404-screen-capture-and-sharing.md) | macOS-style shots and recordings (Super+Shift+3/4/5, Print keys, thumbnail, clipboard) and screen and window sharing through the portal | 0 | 11 |
