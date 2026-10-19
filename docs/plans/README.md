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
| [2026-10-14-1550-infinite-canvas-compositor.md](2026-10-14-1550-infinite-canvas-compositor.md) | The compositor: canvas, cameras, desktops, overview, gestures, Windows-style title bars, window management, layer shell, scale, scenarios; on-screen runs owed; undecided extensions | 32 | 5 |
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
| [2026-10-16-0944-wifi-and-claude-panels.md](2026-10-16-0944-wifi-and-claude-panels.md) | The network panel after the other shell's: live figures, band, DNS provider, known and other networks; an AI usage panel for every family used, Claude and GPT first, with limits, tokens by day and by model | 16 | 1 |
| [2026-10-16-1003-normal-view-hops.md](2026-10-16-1003-normal-view-hops.md) | A 3-finger swipe in the normal view hops like the overview's to the nearest desktop that way, and stays put with none; the free pan with momentum goes | 7 | 1 |
| [2026-10-16-1029-tiling-slices.md](2026-10-16-1029-tiling-slices.md) | Tiled desktops hold windows in slices — the whole, halves, quarters — moved and resized by Super+arrows like Windows' snap keys; growth pushes windows into free space; new windows take the largest free slice | 5 | 1 |
| [2026-10-16-1404-screen-capture-and-sharing.md](2026-10-16-1404-screen-capture-and-sharing.md) | macOS-style shots and recordings (Super+Shift+3/4/5, Print keys, thumbnail, clipboard) and screen and window sharing through the portal | 11 | 1 |
| [2026-10-16-1458-trackpad-clicks-and-overview-launch.md](2026-10-16-1458-trackpad-clicks-and-overview-launch.md) | Two-finger press is a right click; in the overview a right click makes or removes a desktop; an app launched from the overview opens in its target | 4 | 2 |
| [2026-10-16-1929-answering-questions.md](2026-10-16-1929-answering-questions.md) | A question from the user is answered, never acted on: the skill, and an omp extension pinning each answer above the input until it is dismissed | 5 | 1 |
| [2026-10-16-2055-settings-and-config-file.md](2026-10-16-2055-settings-and-config-file.md) | A settings window from the overview toolbar's gear and `canvas.conf` written on first run: every shortcut recorded by pressing it, swipe presets, the pinch switch, the toolbar on any edge, bar widgets on and off | 16 | 1 |
| [2026-10-17-0722-login-session-gaps.md](2026-10-17-0722-login-session-gaps.md) | What a wayland-canvas login lacks next to Omarchy's Hyprland session: the lock screen, notifications, the password dialog, X11 apps, the night light, the session services; the install script's root re-run | 34 | 4 |
| [2026-10-17-1108-note-editing-in-place.md](2026-10-17-1108-note-editing-in-place.md) | A note opened for editing is edited on the note itself: one card at every zoom and pan, no overlay copy beside it | 8 | 0 |
| [2026-10-17-1109-note-text-and-size.md](2026-10-17-1109-note-text-and-size.md) | Notes: bold, italic, underline, size and font per selected span, kept with the note; a card resized by dragging a corner | 1 | 0 |
| [2026-10-17-1113-overlay-drawing.md](2026-10-17-1113-overlay-drawing.md) | Drawing over the canvas: sheets and groups in a panel, depth above, below or between windows by drag, pen, shapes, text and photos, hidden or removed whole; apps unaffected | 0 | 49 |
| [2026-10-17-1335-app-bar-and-plugin-settings.md](2026-10-17-1335-app-bar-and-plugin-settings.md) | An app bar in the overview showing every open app and jumping to its window, an invert switch for the pointer and swipes, and settings pages built from what each plugin declares | 27 | 0 |
| [2026-10-17-2025-capture-with-a-panel-open.md](2026-10-17-2025-capture-with-a-panel-open.md) | A capture chord while a shell panel holds the keyboard: proven for a bar panel, owed for the settings window in the overview | 1 | 2 |
| [2026-10-18-1130-notes-end-to-end.md](2026-10-18-1130-notes-end-to-end.md) | Sticky notes usable on screen: placed and following the plane on every output, typed into, grown, dragged by the strip, sized by the corner, recoloured and deleted from the strip, font per note, size, bold, italic and underline per span, one HTML file per note | 20 | 1 |
| [2026-10-18-1436-super-held-pan.md](2026-10-18-1436-super-held-pan.md) | The normal view pans only while Super is held, by a drag or a 2-finger scroll, as a look away; a double Super tap eases back to the view it left; moving desktops stays the overview's | 11 | 1 |
| [2026-10-18-1736-lock-password-check.md](2026-10-18-1736-lock-password-check.md) | The lock screen checks the password with nothing set: the shipped PAM directory; the frozen clock, a shell reload while locked, and a shell taking over an abandoned lock | 2 | 4 |
| [2026-10-18-1906-x11-input-follows-the-canvas.md](2026-10-18-1906-x11-input-follows-the-canvas.md) | X11 windows take the pointer where they are drawn: X's stack and positions follow the canvas through one offset that follows the pointer; Krita's dialogs, zoomed-out desktops | 9 | 2 |
| [2026-10-18-1909-recording-stop.md](2026-10-18-1909-recording-stop.md) | Stopping a recording finishes a playable file whatever was started meanwhile: no descriptor of the compositor's reaches a process it starts | 3 | 1 |
| [2026-10-18-1930-clipboard-shortcuts.md](2026-10-18-1930-clipboard-shortcuts.md) | Super+C and Super+V copy and paste in every app by sending Ctrl+Insert and Shift+Insert, rebindable in the settings | 3 | 1 |
| [2026-10-18-1930-note-at-cursor.md](2026-10-18-1930-note-at-cursor.md) | A new sticky note opens centred on the cursor, kept on its screen and clear of the toolbar and bars | 3 | 1 |
| [2026-10-18-1930-super-n-makes-desktops.md](2026-10-18-1930-super-n-makes-desktops.md) | Super+N makes a missing desktop beside desktop 1: sides then corners clockwise from the top, slot N-1 first, the next free slot, rings outwards, never overlapping | 3 | 1 |
| [2026-10-18-2043-dialogs-and-desktop-focus.md](2026-10-18-2043-dialogs-and-desktop-focus.md) | A dialog floats centred over its window and never joins a tiled board; going to a desktop gives the keyboard to the window last used there | 6 | 1 |
| [2026-10-19-0813-fullscreen-desktop-of-its-own.md](2026-10-19-0813-fullscreen-desktop-of-its-own.md) | A window going fullscreen over others gets a desktop of its own beside its desktop while it lasts, and comes back however it leaves; alone, it goes fullscreen in place | 4 | 1 |
| [2026-10-19-0853-tag-builds-pass.md](2026-10-19-0853-tag-builds-pass.md) | A v* tag builds both packages on GitHub: the Arch job installs the makedepends read from the PKGBUILD, and the version names the next tag, 1.0.2 | 4 | 1 |
| [2026-10-19-0925-version-from-the-tag.md](2026-10-19-0925-version-from-the-tag.md) | The version is the git tag: tools/version.sh for meson, the tarball and both packages; tag.rN.gHASH between tags; ci-check-tag.sh gone | 5 | 0 |
| [2026-10-19-1009-the-flake-builds.md](2026-10-19-1009-the-flake-builds.md) | nix build and nix develop work again: every library meson asks for, installs under the prefix, the display-free suites in the check phase | 5 | 0 |
