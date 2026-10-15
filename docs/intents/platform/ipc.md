# Control IPC

> Owns the compositor's control socket: the path, the line protocol, the
> commands, and the `msg` client. The bar that uses it is in
> [../apps/bar.md](../apps/bar.md).

## Requirement

Shell clients and scripts can drive compositor actions without speaking a
Wayland protocol: the top bar's logo button opens the launcher, the power
menu ends the session.

## Design considerations

- **A text protocol on a stream socket**: one newline-terminated command
  line in, one newline-terminated response line out. Any client language,
  and a shell one-liner, can speak it; no client keeps protocol state.
- **Commands reuse the keybinding actions** (`launcher`, `overview`), so
  the keyboard and the bar cannot drift apart.

## Behavior

The compositor listens on
`$XDG_RUNTIME_DIR/wayland-canvas-<wayland display>.sock` (the prefix is a
constant in `src/config.h`) and exports the path to every spawned client as
`$WAYLAND_CANVAS_SOCKET`. The socket is created once the Wayland display
socket exists and before any client is spawned.

| Command | Effect | Response |
|---|---|---|
| `launcher` | toggle the launcher on the output under the cursor | `ok` |
| `overview` | toggle the overview on the output under the cursor | `ok`, or `error: no output under the cursor` |
| `quit` | end the compositor session | `ok` |

Unknown commands answer `error: unknown command '…'`. A client may send
several commands on one connection or reconnect per command; the client
closes the connection.

`wayland-canvas msg <command>` connects to `$WAYLAND_CANVAS_SOCKET`, sends
the command, prints the response, and exits non-zero on a transport failure
or an `error` response.

## Error states, corner cases

- No `$XDG_RUNTIME_DIR`: the socket is not created (logged) and the
  compositor runs on without control IPC.
- A socket file left by a crashed run is unlinked before binding.
- A connection that ends mid-command is dropped with its partial line.
- The socket file is removed when the compositor exits.

## Cross-links

- [../apps/bar.md](../apps/bar.md) — the client that uses it.
- [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md) —
  the bindings whose actions the commands share.
