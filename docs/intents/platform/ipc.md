# Control IPC

> Owns the compositor's control socket: the path, the JSON-lines protocol,
> the methods and events, the state it reports, canvas items, and the `msg`
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
- **The compositor draws canvas items, plugins own them.** A sticky note is
  a coloured card with text on the canvas: its plugin creates, updates,
  removes and keeps it; the compositor places it, draws it, lets the user
  drag it, and reports what the user did.
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
| `state` | — | `{"outputs": […], "desktops": […], "items": […], "theme": "dark"}` |
| `subscribe` | — | `true`; from then on every event goes to this connection |
| `launcher.toggle` | `output` (optional; default the output under the cursor) | `true` |
| `overview.toggle` | `output` (optional, as above) | `true`; an error without an output |
| `quit` | — | `true`, then the session ends |
| `theme.set` | `mode`: `light` or `dark` | `true`; on the real session (a seat session) the applications' preference follows — [../shell/theme.md](../shell/theme.md) |
| `item.set` | `id`; `color` (`#rrggbb`); `text`; `width`, `height` (canvas px); `x`, `y` (canvas, the top-left) | The item as placed: `{"id", "x", "y", "width", "height"}` |
| `item.remove` | `id` | `true`; an error for an unknown id |
| `item.activate` | `id` | `true`; the compositor reports the item exactly as a double-click does — the `item.activated` event with its output and output-local box — so a plugin can open an editor over an item it just placed. An error for an unknown id, or when the item is on no output's screen |
| `action.run` | `name` (a plugin action's global name); `args` (an object) | `true`; an `action` event with the name and args goes to every subscriber, and the host runs the named action |
| `capture.pick` | `what`: `region`, `window`, `output` or `any`; `output` (optional, as above) | The pick, when the user finishes it: a region `{"output", "x", "y", "w", "h"}` (output-local logical), a window `{"window" (session id), "identifier" (ext-foreign-toplevel)}`, an output `{"output"}` — [../apps/capture.md](../apps/capture.md) |
| `capture.stop` | — | `true`; an error (`not recording`) with nothing running |
| `settings.get` | — | The settings (below) |
| `settings.set` | Exactly one of: `scope` (`global` or `overview`), `action` and `keys` (chords; `[]` for none, `null` for the defaults); `commands` (a list of `{"keys", "command"}` replacing the file's commands); `swipes` (a preset); `pinchScale` (a boolean); `lockIdleMinutes` (0 for never); `nightLight` (a boolean); `nightLightTemperature` (kelvin) | The settings, with `moved`: the chords taken from other actions or commands, each `{"scope", "action", "keys"}`. Errors name the reason: an unknown scope, action or preset, a chord that does not parse or is not allowed there, the file unwritable |
| `shortcut.capture` | `scope`: `global` (the default) or `overview` | `{"keys": "Super+Shift+K"}` once the user presses a chord, `{"keys": null}` when cancelled |
| `shortcut.cancel` | — | `true`; a capture under way answers `{"keys": null}` |

`item.set` creates the item or updates the one with that id. Without `x`
and `y`, a new item is centred on what the output under the cursor shows
(the overview's target there, when that output is in the overview); an
update keeps its place. Ids are the plugin's own strings, named
`<plugin id>/<item id>` by the shell.

`action.run` is the compositor's way into the shell: it only delivers the
event (the action itself belongs to a plugin), so a script or a
keybinding can run a shell action without speaking to the shell directly —
`wayland-canvas msg action.run '{"name":"canvas.notes/create",
"args":{"color":"#ffd8f66"}}'`.

`capture.pick` and `shortcut.capture` are the methods whose answers wait
for the user. `capture.pick` opens the compositor's own picker
([../apps/capture.md](../apps/capture.md)) and the response goes out
when the pick completes; a cancel answers `cancelled`, and a second
pick while one runs answers `a capture pick is already running`. The
chooser of xdg-desktop-portal-wlr is this method through the msg
client (below). `shortcut.capture` takes every key until a chord is
pressed, Escape alone cancels, and a second capture cancels the first
([../shell/settings.md](../shell/settings.md)); it reports what was
pressed, and `settings.set` decides whether that chord is allowed.

### Settings

`settings.get`, `settings.set` and the `settings` event carry the effective
settings of [../input/config-file.md](../input/config-file.md):

| Key | Holds |
|---|---|
| `path` | The file |
| `errors` | Each ignored line: `{"line", "text"}` |
| `shortcuts` | Every action, in the bindings table's order, the overview's last: `scope`, `action` (its name), `label`, `group`, `keys` and `defaults` (chords as the file writes them) |
| `commands` | `{"keys", "command"}` each |
| `gestures` | `swipes` (the preset), `pinchScale` |
| `session` | `lockIdleMinutes` (0 for never), `nightLight`, `nightLightTemperature` |


### State

| Part | Each entry |
|---|---|
| `outputs` | `name`, `x`, `y`, `width`, `height` (layout px), `scale`, `overview` (true while the output shows the overview) |
| `desktops` | `id`, `number`, `x`, `y`, `width`, `height` (canvas px) |
| `items` | as last set, with the place the user dragged it to |
| `recording` | `null`, or `{"path": …}` while a recording runs |

### Events

| Event | Params | When |
|---|---|---|
| `overview` | `output`, `active` | An output enters or leaves the overview |
| `outputs` | `outputs`, as in the state | An output appears, goes, or changes size or scale |
| `desktops` | `desktops`, as in the state | A desktop is created, moved, combined or abandoned |
| `theme` | `mode` | The mode changes, by any client; the shell adopts it — [../shell/theme.md](../shell/theme.md) |
| `item.moved` | `id`, `x`, `y` | The user dropped a dragged item |
| `item.activated` | `id`, `output`, and `box`: the item on that output in output-local logical px | The user double-clicked the item, or a client ran `item.activate` |
| `action` | `name`, `args` | A client ran `action.run`; the host runs the named plugin action |
| `capture.recording` | `active`, `path` | A recording started (the file it writes) or stopped |
| `settings` | The settings, as `settings.get` returns them | The effective settings change: the file saved by hand or deleted, or a `settings.set` |

### Canvas items

An item is drawn above windows in canvas space, so it pans and zooms with
the canvas and shows in the overview: a card of its colour, a slightly
darker strip along its top, its text in dark ink wrapped to its width. A
drag that starts on an item, in the normal view or the overview, moves it
and ends with `item.moved`; a double-click reports `item.activated`. Items
never take part in window management (placement, tiling, desktop
membership).

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

## Cross-links

- [../shell/plugins.md](../shell/plugins.md) — the shell's API over this
  socket.
- [../shell/notes.md](../shell/notes.md) — the canvas items' one user.
- [../shell/theme.md](../shell/theme.md) — the mode the compositor follows.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md) —
  the bindings whose actions the methods share.
