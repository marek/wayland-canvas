# Control IPC

> Owns the compositor's control socket: the path, the JSON-lines protocol,
> the methods and events, the state it reports, the surfaces a plugin
> places on the canvas, and the `msg`
> client. The shell's API over it is [../shell/plugins.md](../shell/plugins.md).

## Requirement

Shell plugins and scripts drive the compositor and follow its state without
speaking a Wayland protocol: the bar's logo opens the launcher, the overview
toolbar shows while a screen is in the overview, sticky notes sit on the
canvas, the light/dark switch reaches the title bars, and the settings
window reads and changes the shortcuts and gestures.

## Design considerations

- **JSON lines on a stream socket**: one JSON object per line each way. Any
  language, and a shell one-liner, can speak it. A request names a method
  and gets exactly one response carrying its id; a connection that
  subscribed also receives events, unprompted.
- **Methods reuse the keybinding actions** (launcher, overview), so the
  keyboard and the shell cannot drift apart.
- **State is asked for once, changes are pushed**: a client reads the whole
  state, then follows events; nothing polls.
- **The compositor draws nothing for a plugin.** A plugin that wants
  something on the plane — a sticky note, a drawing — draws it in a
  surface of its own and asks for it to be placed at a canvas rectangle;
  the compositor places it, moves it with the plane and sends it input,
  and knows nothing of what is in it.
- **A drag of a plugin's surface is the compositor's**, as an interactive
  move or resize of a window is (xdg-shell's `move` and `resize`): the
  plugin hands over the press, and the compositor moves or sizes the
  surface with its own pointer until the release. A plugin that moved its
  surface from its own pointer events would move the very coordinates
  those events are measured in, and its drag would chase itself.
- **A client can never stall the compositor**: a subscriber that stops
  reading is dropped rather than waited for.

## Behavior

The compositor listens on
`$XDG_RUNTIME_DIR/wayland-canvas-<wayland display>.sock` (the prefix is a
constant in `src/core/config.h`) and exports the path to every spawned
client as `$WAYLAND_CANVAS_SOCKET`. The socket is created once the Wayland
display socket exists and before any client is spawned. A process the
compositor did not spawn — a service the session bus starts, the portal's
chooser among them — finds it by the same rule from `$XDG_RUNTIME_DIR` and
the `$WAYLAND_DISPLAY` the session hands over (an absolute
`$WAYLAND_DISPLAY` names the socket's directory itself).

### Messages

| Message | Shape |
|---|---|
| Request | `{"id": 7, "method": "overview.toggle", "params": {"output": "DP-1"}}`: `id` a number or string, echoed back; `params` optional |
| Response | `{"id": 7, "result": …}` or `{"id": 7, "error": "…"}` — `capture.pick`'s and `shortcut.capture`'s only when the user has finished |
| Event | `{"event": "overview", "params": {…}}`, only on a connection that subscribed |

A client may send several requests on one connection or reconnect per
request; responses come in request order.

### Methods

| Method | Params | Result |
|---|---|---|
| `state` | — | `{"outputs": […], "cursor": …, "desktops": […], "windows": […], "theme": "dark", "recording": null}` |
| `subscribe` | — | `true`; from then on every event goes to this connection |
| `launcher.toggle` | `output` (optional; default the output under the cursor) | `true` |
| `overview.toggle` | `output` (optional, as above) | `true`; an error without an output |
| `quit` | — | `true`, then the session ends |
| `theme.set` | `mode`: `light` or `dark` | `true`; on the real session (a seat session) the applications' preference follows — [../shell/theme.md](../shell/theme.md) |
| `surface.place` | `surface` (the layer surface's name); `x`, `y`, `width`, `height` (canvas px) | `true`: that surface is drawn on the plane at the rectangle, pans and zooms with it, and takes input there (below). A name no surface carries yet is remembered for when one does; an error names a rectangle with no area |
| `surface.unplace` | `surface` | `true`; the surface goes back to being a screen-space layer surface |
| `surface.move` | `surface` | Answered at the release: the rectangle the surface was dropped at, `{"x", "y", "width", "height"}` (canvas px). Asked while a button pressed on that surface is held — the press the plugin is answering — it hands the drag to the compositor, which moves the surface with the pointer until the button is released. An error when no button is held on the surface, it is not placed, or the pointer is already in a drag |
| `surface.resize` | `surface`; `edges` (a list of `left`, `right`, `top`, `bottom`); `minWidth`, `minHeight` (optional, canvas px) | As `surface.move`, the compositor sizing the surface by those edges instead, never below the minimum: the client is told each size as the pointer goes, and the answer is the rectangle at the release |
| `window.focus` | `window` (a session id from the window list) | `true`: the window is focused and the view moves to it — to its desktop at that desktop's scale, else to a view centred on the window; a screen in the overview leaves it as a shelf click does, and a minimized window is restored first ([../windows/window-management.md](../windows/window-management.md)). An error for an unknown id |
| `action.run` | `name` (a plugin action's global name); `args` (an object) | `true`; an `action` event with the name and args goes to every subscriber, and the host runs the named action |
| `capture.pick` | `what`: `region`, `window`, `output` or `any`; `output` (optional, as above) | The pick, when the user finishes it: a region `{"output", "x", "y", "w", "h"}` (output-local logical), a window `{"window" (session id), "identifier" (ext-foreign-toplevel)}`, an output `{"output"}` — [../apps/capture.md](../apps/capture.md) |
| `capture.stop` | — | `true`; an error (`not recording`) with nothing running |
| `settings.get` | — | The settings (below) |
| `settings.set` | Exactly one of: `scope` (`global` or `overview`), `action` and `keys` (chords; `[]` for none, `null` for the defaults); `commands` (a list of `{"keys", "command"}` replacing the file's commands); `swipes` (a preset); `pinchScale` (a boolean); `mouseScroll`, `touchpadScroll` or `pan` (`natural` or `traditional`); `overviewSwipe` (`up` or `down`); `hopSwipe` (`toward` or `away`); `lockIdleMinutes` (0 for never); `nightLight` (a boolean); `nightLightTemperature` (kelvin) | The settings, with `moved`: the chords taken from other actions or commands, each `{"scope", "action", "keys"}`. Errors name the reason: an unknown scope, action, preset or direction, a chord that does not parse or is not allowed there, the file unwritable |
| `shortcut.capture` | `scope`: `global` (the default) or `overview` | `{"keys": "Super+Shift+K"}` once the user presses a chord — `Super` for a tap, `Super Super` for a double tap — `{"keys": null}` when cancelled |
| `shortcut.cancel` | — | `true`; a capture under way answers `{"keys": null}` |

`action.run` is the compositor's way into the shell: it only delivers the
event (the action itself belongs to a plugin), so a script or a
keybinding can run a shell action without speaking to the shell directly —
`wayland-canvas msg action.run '{"name":"canvas.notes/create",
"args":{"color":"#ffd8f66"}}'`.

`capture.pick`, `shortcut.capture`, `surface.move` and `surface.resize`
are the methods whose answers wait for the user. `capture.pick` opens
the compositor's own picker ([../apps/capture.md](../apps/capture.md))
and the response goes out
when the pick completes; a cancel answers `cancelled`, and a second
pick while one runs answers `a capture pick is already running`. The
chooser of xdg-desktop-portal-wlr is this method through the msg
client (below). `shortcut.capture` takes every key until a chord is
pressed, Escape alone cancels, and a second capture cancels the first
([../shell/settings.md](../shell/settings.md)); it reports what was
pressed, and `settings.set` decides whether that chord is allowed. The
two drags answer when the button is released, with the rectangle the
surface ended at.

### Settings

`settings.get`, `settings.set` and the `settings` event carry the effective
settings of [../input/config-file.md](../input/config-file.md):

| Key | Holds |
|---|---|
| `path` | The file |
| `errors` | Each ignored line: `{"line", "text"}` |
| `shortcuts` | Every action, in the bindings table's order, the overview's last: `scope`, `action` (its name), `label`, `group`, `keys` and `defaults` (chords as the file writes them) |
| `commands` | `{"keys", "command"}` each |
| `gestures` | `swipes` (the preset), `pinchScale`, `mouseScroll`, `touchpadScroll` and `pan` (`natural` or `traditional`), `overviewSwipe` (`up` or `down`), `hopSwipe` (`toward` or `away`) ([../input/gestures.md](../input/gestures.md)) |
| `session` | `lockIdleMinutes` (0 for never), `nightLight`, `nightLightTemperature` |


### State

| Part | Each entry |
|---|---|
| `outputs` | `name`, `x`, `y`, `width`, `height` (layout px), `scale`, `view` (the canvas rectangle the output looks at: `x`, `y`, `width`, `height` in canvas px), `usable` (the canvas rectangle of that view the bars and the overview's toolbar and app bar leave free, in canvas px), `overview` (true while the output shows the overview) |
| `cursor` | `x`, `y` (canvas px), `output` (the output the cursor is on); a read for the moment of the answer — no event carries it, so a client that needs it asks `state` |
| `desktops` | `id`, `number`, `x`, `y`, `width`, `height` (canvas px) |
| `windows` | `id` (the session id), `appId`, `title`, `desktop` (its desktop's id, or null on free canvas), `minimized`, `focused`, in the order the windows opened. Where a window is on the canvas is the compositor's to act on, not a client's to read ([../shell/app-bar.md](../shell/app-bar.md)) |
| `recording` | `null`, or `{"path": …}` while a recording runs |

### Events

| Event | Params | When |
|---|---|---|
| `overview` | `output`, `active` | An output enters or leaves the overview |
| `outputs` | `outputs`, as in the state | An output appears, goes, or changes size or scale |
| `desktops` | `desktops`, as in the state | A desktop is created, moved, combined or abandoned |
| `windows` | `windows`, as in the state | A window opens, closes, is moved, retitled, minimized, restored or focused |
| `theme` | `mode` | The mode changes, by any client; the shell adopts it — [../shell/theme.md](../shell/theme.md) |
| `action` | `name`, `args` | A client ran `action.run`; the host runs the named plugin action |
| `capture.recording` | `active`, `path` | A recording started (the file it writes) or stopped |
| `settings` | The settings, as `settings.get` returns them | The effective settings change: the file saved by hand or deleted, or a `settings.set` |

### Surfaces on the canvas

A plugin that wants something of its own on the plane draws it in a
surface of its own and asks the compositor where to put it: not a place on
a screen, but a rectangle in canvas coordinates. From then on the surface
is part of the plane — it pans, zooms and scales with everything else,
shows in the overview, and is hit-tested where it is drawn — and the
plugin draws every pixel of it, its text, its caret and its selection
included. The compositor adds no drawing of its own and knows nothing
about what the plugin put there.

| Subject | Rule |
|---|---|
| Asking | the plugin opens a layer surface named `wayland-canvas-canvas-<plugin id>/<surface id>` — the name is set as the surface is created, since a layer surface keeps the one it was made with — and calls `surface.place` with that name and a canvas rectangle |
| Before it is placed | a surface with that name is never given a place on a screen: until the plugin says where on the plane it goes it is not drawn at all, so a surface meant for the canvas can never appear centred on the screen instead |
| Remembered | the compositor keeps the rectangle under the name: a placement asked for before the surface exists, or again after the shell reloads, applies when a surface of that name maps |
| Drawing | the surface is drawn above windows in canvas space by every output whose view it is in, whichever output it was made on; the client is told the rectangle's size in logical pixels, renders at the scale of the outputs it is on, and is drawn at the size it last committed, so a size in flight is never stretched |
| Input | clicks, scrolls and drags inside it go to the surface, in the normal view and the overview alike, on every output; the keyboard follows the same rule as any shell surface asking for it on demand ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) |
| Moving | the plugin moves or sizes it by placing it again; a drag of it, the plugin hands to the compositor with `surface.move` or `surface.resize` from the press, as a window's client asks for an interactive move. A drag the plugin does not claim pans the canvas, as the empty plane does |
| An output going | the surface stays on the plane, taken over by an output that is left; it closes only with the last output |
| Going | the surface unmapping or the plugin's `surface.unplace` takes it off the plane |
| Never | a canvas surface takes no part in window management: no placement, no tiling, no desktop membership, no minimise |

### The `msg` client

`wayland-canvas msg <method> [<params as JSON>]` connects to
`$WAYLAND_CANVAS_SOCKET`, else to the socket `$WAYLAND_DISPLAY` names, sends
one request, prints the result and exits
0, or prints the error to stderr and exits 1 (also on a transport failure).
`wayland-canvas msg subscribe` prints each event as one line until
interrupted. `wayland-canvas msg capture.pick`, with the chooser list
on stdin (`Monitor: <name> <description>` / `Window: <title>
(<identifier>)`, as xdg-desktop-portal-wlr prints it), runs the
picker and prints the chosen list line back byte-exactly; silence
means the user cancelled. That command is the portal's `chooser_cmd`.

## Error states, corner cases

- No `$XDG_RUNTIME_DIR`: the socket is not created (logged) and the
  compositor runs on without control IPC.
- A socket file left by a crashed run is unlinked before binding.
- A line that is not a JSON object, or has no `method`: an error response
  with `id` null (or the id, when one was readable); the connection stays.
- An unknown method or bad params: an error response naming them.
- A line longer than 64 KiB: an error response, then the connection is
  dropped.
- A subscriber whose unread events pass 1 MiB is dropped.
- A connection that ends mid-line is dropped with its partial line.
- The socket file is removed when the compositor exits.
- `surface.move` or `surface.resize` asked once the button is up (a quick
  click): an error, and nothing moves. The surface or the pointer's
  output going mid-drag ends the drag with an error answer; a client that
  goes mid-drag is answered nothing, and the surface stays where the
  release left it.

## Cross-links

- [../shell/plugins.md](../shell/plugins.md) — the shell's API over this
  socket.
- [../shell/notes.md](../shell/notes.md) — sticky notes, drawn by the plugin in surfaces on the plane.
- [../shell/app-bar.md](../shell/app-bar.md) — the window list's and
  `window.focus`'s one user.
- [../shell/theme.md](../shell/theme.md) — the mode the compositor follows.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md) —
  the bindings whose actions the methods share.
