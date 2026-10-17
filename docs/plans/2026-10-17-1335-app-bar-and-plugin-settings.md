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

- [ ] Intent: the choice in `[gestures]` of
      [input/config-file.md](../intents/input/config-file.md), the
      direction rule in
      [input/gestures.md](../intents/input/gestures.md), the natural
      scrolling row of
      [input/keyboard-and-pointer.md](../intents/input/keyboard-and-pointer.md),
      the settings key and `settings.set` parameter in
      [platform/ipc.md](../intents/platform/ipc.md), and the Gestures
      page of [shell/settings.md](../intents/shell/settings.md).
- [ ] Compositor: the setting read, reloaded and applied — to every
      pointer and touchpad device's scroll direction, and to the
      direction a swipe hop and an overview pan take; the compile-time
      `natural_scroll` becomes the default the file states.
- [ ] Unit test: a `canvas.conf` with each value resolves to the
      expected direction, an unknown value is an error line.
- [ ] Settings window: the switch on the Gestures page, written
      through `settings.set` and following the `settings` event.
- [ ] Scenario: the switch flipped over IPC reverses a scroll's effect
      on the camera.

### Settings a plugin registers

- [ ] Intent: the manifest's `settings` declaration gains per-key
      metadata (type, label, description, group, choices, range) in
      [shell/plugins.md](../intents/shell/plugins.md), and
      [shell/settings.md](../intents/shell/settings.md) states how the
      window builds a page from it and what it writes.
- [ ] Shell: the settings window renders a page per plugin that
      declares settings, from the schema alone, writing through
      `api.plugin.setSetting`; a key with no metadata is left out.
- [ ] The toolbar's edge and the bar's hidden widgets keep their pages
      and their meaning while the new mechanism lands beside them.
- [ ] Scenario: a plugin's declared switch and choice appear in
      settings, and a change lands in `shell.json`.

### Windows the shell can see and go to

- [ ] Intent: the open windows in the state and their event, and the
      method that goes to one, in
      [platform/ipc.md](../intents/platform/ipc.md); the go-to-a-window
      path in
      [windows/window-management.md](../intents/windows/window-management.md)
      (it is the restore path the overview's shelf already takes).
- [ ] Compositor: `windows` in `state` (session id, app id, title,
      desktop or none, minimized, focused) and the `windows` event on
      every change.
- [ ] Compositor: `window.focus` — focus the window and move the view
      to it: to its desktop at that desktop's scale when it has one,
      else a view centred on the window; from the overview it leaves
      the overview the way a shelf click does.
- [ ] Unit test: the view a window on a desktop, on a free spot, and
      minimized each resolves to.

### The app bar

- [ ] Intent: `docs/intents/shell/app-bar.md` — the surface, its
      edges, the shared edge and separator, the entries and their
      order, what a click does, and the off switch; cross-linked from
      [shell/toolbar.md](../intents/shell/toolbar.md),
      [shell/plugins.md](../intents/shell/plugins.md),
      [canvas/overview.md](../intents/canvas/overview.md) and the
      intents index.
- [ ] Shell: the `canvas.apps` plugin — a surface per output, shown
      while that output is in the overview, entries from the
      compositor's window list with the app's icon and name.
- [ ] Shell: the shared edge — with `appbar.position` equal to
      `toolbar.position`, one column or row holds the toolbar's
      buttons, a separator, then the apps.
- [ ] Shell: a click goes to the window through `window.focus`.
- [ ] Settings: the app bar's edge and its on/off switch, through the
      plugin-declared settings mechanism above.
- [ ] Scenario: two foot windows on two desktops — the app bar lists
      both, a click on each lands the view on its desktop, and a window
      on no desktop lands centred.
- [ ] Scenario: the app bar off shows nothing; on the toolbar's edge
      both show with the apps last.
- [ ] `tools/check.sh` green, the plans index and the intents index
      current.
