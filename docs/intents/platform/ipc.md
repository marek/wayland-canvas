# Control IPC

> Owns the compositor's control socket: the path, the JSON-lines protocol,
> the methods and events, the state it reports, canvas items, and the `msg`
> client. The shell's API over it is [../shell/plugins.md](../shell/plugins.md).

## Requirement

Shell plugins and scripts drive the compositor and follow its state without
speaking a Wayland protocol: the bar's logo opens the launcher, the overview
toolbar shows while a screen is in the overview, sticky notes sit on the
canvas, and the light/dark switch reaches the title bars.

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
display socket exists and before any client is spawned.

### Messages

| Message | Shape |
|---|---|
| Request | `{"id": 7, "method": "overview.toggle", "params": {"output": "DP-1"}}`: `id` a number or string, echoed back; `params` optional |
| Response | `{"id": 7, "result": …}` or `{"id": 7, "error": "…"}` |
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
| `theme.set` | `mode`: `light` or `dark` | `true`; on the real session (the DRM backend) the applications' preference follows — [../shell/theme.md](../shell/theme.md) |
| `item.set` | `id`; `color` (`#rrggbb`); `text`; `width`, `height` (canvas px); `x`, `y` (canvas, the top-left) | The item as placed: `{"id", "x", "y", "width", "height"}` |
| `item.remove` | `id` | `true`; an error for an unknown id |

`item.set` creates the item or updates the one with that id. Without `x`
and `y`, a new item is centred on what the output under the cursor shows
(the overview's target there, when that output is in the overview); an
update keeps its place. Ids are the plugin's own strings, named
`<plugin id>/<item id>` by the shell.

### State

| Part | Each entry |
|---|---|
| `outputs` | `name`, `x`, `y`, `width`, `height` (layout px), `scale`, `overview` (true while the output shows the overview) |
| `desktops` | `id`, `number`, `x`, `y`, `width`, `height` (canvas px) |
| `items` | as last set, with the place the user dragged it to |

### Events

| Event | Params | When |
|---|---|---|
| `overview` | `output`, `active` | An output enters or leaves the overview |
| `outputs` | `outputs`, as in the state | An output appears, goes, or changes size or scale |
| `desktops` | `desktops`, as in the state | A desktop is created, moved, combined or abandoned |
| `theme` | `mode` | The mode changes, by any client; the shell adopts it — [../shell/theme.md](../shell/theme.md) |
| `item.moved` | `id`, `x`, `y` | The user dropped a dragged item |
| `item.activated` | `id`, `output`, and `box`: the item on that output in output-local logical px | The user double-clicked the item |

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
`$WAYLAND_CANVAS_SOCKET`, sends one request, prints the result and exits
0, or prints the error to stderr and exits 1 (also on a transport failure).
`wayland-canvas msg subscribe` prints each event as one line until
interrupted.

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
