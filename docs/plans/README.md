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
| [2026-10-14-1802-triple-helix-adoption.md](2026-10-14-1802-triple-helix-adoption.md) | Intents, tests, plans, skills, roles and hooks adopted from `MilitaryForcesWorkspace`, generic to whoever works | 10 | 3 |
| [2026-10-14-1803-code-review-findings.md](2026-10-14-1803-code-review-findings.md) | The reviewer's four bugs and two checks; scenarios still owed for drag-and-drop, output unplug and mixed scales | 6 | 3 |
| [2026-10-14-1941-cpp-and-scripting.md](2026-10-14-1941-cpp-and-scripting.md) | C++26 on LLVM under rules of our own: toolchain, wlroots wrapper, classes, the conversion; scripting the top bar and layout engine waits on the user's decisions | 0 | 11 |
