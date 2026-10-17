# An app bar, an invert switch, and settings every plugin registers

Date: 2026-10-17 13:35 EDT

## Context

Three gaps in what the shell offers the user.

The pointer and the touchpad have one direction, fixed at compile time;
a user who wants the other one has no way to ask for it. The settings
window's Gestures page is where that choice belongs, beside the swipe
preset and the pinch switch.

There is no way to see what is open. The canvas spreads windows over
desktops and free spots, so a window out of view is reachable only by
the overview or by remembering where it was put. The answer is an app
bar in the overview, built like the top bar — a surface of its own, one
per output, holding entries the way the bar holds widgets — showing
every open app and nothing else: it never launches anything. Its
default edge is the overview's left, opposite the toolbar's default
right; when the user puts both on one edge they share it, the toolbar's
buttons first and the apps last, parted by a small separator. Turning
it off is one of its choices. Clicking an entry goes to its window: to
the desktop the window sits on, or, for a window on no desktop, to a
view centred on it.

Behind both: the settings window knows each page by hand today, so
every new choice means new code in it. A plugin should instead declare
its configurable parameters in its manifest, and the settings window
should render them — so the app bar's edge and switch, and anything a
later plugin adds, arrive in settings without the settings window being
touched.

## Decisions for the user

- [ ] **Does the invert switch change what clients see, or only canvas
      movement?** Inverting libinput's natural scrolling changes
      scrolling inside apps too (one system-wide direction, which is
      what "invert the mouse" usually means); inverting only the
      compositor's panning and hops leaves apps as they are. (the user
      to decide — the plan assumes system-wide, apps included)
- [ ] **One entry per app, or per window?** A dock groups windows under
      one app icon with a mark for how many; a taskbar lists windows.
      (the user to decide — the plan assumes one entry per app, its
      windows reachable by repeated clicks, newest focused first)
- [ ] **Does the app bar show in the normal view too, or only in the
      overview?** (the user to decide — the plan assumes the overview
      only, as the toolbar)

## Tasks

### The invert switch

- [x] Intent: the choice in `[gestures]` of
      [input/config-file.md](../intents/input/config-file.md), the
      direction rule in
      [input/gestures.md](../intents/input/gestures.md), the natural
      scrolling row of
      [input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md),
      the settings key and `settings.set` parameter in
      [platform/ipc.md](../intents/platform/ipc.md), and the Gestures
      page of [shell/settings.md](../intents/shell/settings.md).
- [x] Compositor: the setting read, reloaded and applied — to every
      pointer and touchpad device's scroll direction, and to the
      direction a swipe hop and an overview pan take; the compile-time
      `natural_scroll` becomes the default the file states.
- [x] Unit test: a `canvas.conf` with each value resolves to the
      expected direction, an unknown value is an error line.
- [x] Settings window: the switch on the Gestures page, written
      through `settings.set` and following the `settings` event (the bar
      scenario sees the row and its default).
- [x] Scenario: with invert on, a 3-finger swipe right hops left
      (gestures). The pointer's own scroll direction is libinput's, with
      no headless device to prove it on (owed on screen — the user).

### Settings a plugin registers

- [x] Intent: the manifest's `settings` declaration gains per-key
      metadata (type, label, description, group, choices, range) in
      [shell/plugins.md](../intents/shell/plugins.md), and
      [shell/settings.md](../intents/shell/settings.md) states how the
      window builds a page from it and what it writes.
- [x] Shell: the settings window renders the declarations from the
      schema alone, writing through `api.plugin.setSetting`; a key with no
      metadata is left out. One Plugins tab holds every plugin's groups —
      a tab per plugin would grow the row past the card's width.
- [x] The toolbar's edge and the bar's hidden widgets keep their pages
      and their meaning while the new mechanism lands beside them.
- [x] Scenario: canvas.notifications' declared switch shows its row and
      its default, and the click lands in `shell.json` under
      `plugins.canvas.notifications.settings`. (A declared choice and a
      number are drawn by the same row component, unchecked on screen.)

### Windows the shell can see and go to

- [x] Intent: the open windows in the state and their event, and the
      method that goes to one, in
      [platform/ipc.md](../intents/platform/ipc.md); the go-to-a-window
      path in
      [windows/window-management.md](../intents/windows/window-management.md)
      (it is the restore path the overview's shelf already takes).
- [x] Compositor: `windows` in `state` (session id, app id, title,
      desktop or none, minimized, focused) and the `windows` event on
      every change.
- [x] Compositor: `window.focus` — focus the window and move the view
      to it: to its desktop at that desktop's scale when it has one,
      else a view centred on the window; from the overview it leaves
      the overview the way a shelf click does.
- [x] The ipc scenario in place of a unit test: the view goes to the
      window from off-screen and an unknown id errors (a window on a free
      spot and a minimized one are owed).

### The app bar

- [x] Intent: `docs/intents/shell/app-bar.md` — the surface, its
      edges, the shared edge and separator, the entries and their
      order, what a click does, and the off switch; cross-linked from
      [shell/toolbar.md](../intents/shell/toolbar.md),
      [shell/plugins.md](../intents/shell/plugins.md),
      [canvas/overview.md](../intents/canvas/overview.md) and the
      intents index.
- [x] Shell: the `canvas.apps` plugin — a surface per output, shown
      while that output is in the overview, entries from the
      compositor's window list with the app's icon and name.
- [x] Shell: the shared edge — with `appbar.position` equal to
      `toolbar.position`, one column or row holds the toolbar's
      buttons, a separator, then the apps.
- [x] Shell: a click goes to the window through `window.focus`.
- [x] Settings: the app bar's edge and its on/off switch, on the Toolbar
      page beside the toolbar's edge — what two surfaces must agree on is
      the shell config's own, not one plugin's.
- [x] Scenario: the bar suite — the app bar lists every open app, and a
      click lands the view on that window, focused.
- [ ] Scenario, owed: two desktops at once, and a window on no desktop
      landing on a centred view.
- [x] Shell: the app's icon and name from its desktop entry, the first
      letter standing in where it names none (the icon itself is owed on
      screen — the suite's scratch data home holds no desktop entries).
- [x] Shell: an app's windows unfold beside its entry, a row each, the
      focused one marked; the hover label fixed (entries are keyed, not
      compared by identity) and both proven on the default left edge.
- [x] Scenario: the app bar off shows nothing; on the toolbar's edge
      both show with the apps last.
- [x] `tools/check.sh` green (unit, packaging, scenarios), the plans
      index and the intents index current.
