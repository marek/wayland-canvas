# Notifications

> Owns what an application's notification does: the popup, its actions, how
> long it stays, the history behind it and the silence switch. The plugin
> model it lives in is [plugins.md](plugins.md); the keys are
> [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)'s.

## Requirement

An application that sends a desktop notification gets a popup on screen,
with its text, its icon or image and its buttons; clicking a button reaches
the sender. Nothing a notification does takes the keyboard away from what
the user is typing in. What has been shown is kept in a history the bar
reaches, and popups can be silenced without losing them.

## Design considerations

- **The shell is the notification server**, through Quickshell's
  `NotificationServer` in the `canvas.notifications` plugin: the
  compositor speaks no notification protocol, so another daemon (mako,
  dunst) can serve the session instead by taking the bus name first.
- **A popup is an overlay layer surface that never asks for the
  keyboard**, so typing is never interrupted; only the card takes clicks,
  the rest of the output stays the windows' and the canvas's.
- **The history is the plugin's own state**, written to its storage
  directory ([plugins.md](plugins.md)), so a shell reload or a restart
  keeps what arrived and the silence switch with it.

## Behavior

| Subject | Rule |
|---|---|
| Place | a column down the top-right of every output, below the bar, newest at the top |
| Stacking | at most the setting's number of popups; an older one leaves the screen as a newer arrives, and the history keeps it |
| Expiry | the sender's own timeout when it set one, else the setting (5 s by default); a critical notification stays until it is dismissed or acted on |
| Content | the sender's name, the summary, the body (markup honoured), its image or icon, and a button per action |
| An action | clicking it invokes it on the sender and the popup goes |
| A click on the card | dismisses that popup; the history keeps it |
| Keyboard | never: a popup is not focusable, and no key of the compositor's reaches it |
| Dismiss keys | `notifications-dismiss` (Super+N) takes the newest popup off the screen, `notifications-dismiss-all` (Super+Shift+N) all of them; both leave the history alone |
| The bar widget | a bell in the right section with the history's count; a click opens the history panel |
| The history panel | the silence switch, then what arrived, newest first, each with its sender and body, a critical one marked; one row clears the lot |
| Silence | popups are not shown while it is on, and what arrives still enters the history; the switch is kept across restarts and the bell shows it |

## Error states, corner cases

- **Another server already holds `org.freedesktop.Notifications`** (mako,
  dunst, another session's shell): this one does not get the name, the
  shell runs on without popups of its own, and the other server shows
  them.
- **A notification closed by its sender** while its popup is up: the popup
  goes with it; the history keeps what it said.
- **An image the sender did not keep**: the popup shows the text alone.
- **No bar plugin loaded**: popups still show; the history is reachable
  only through the plugin's actions.

## Cross-links

- [plugins.md](plugins.md), [bar.md](bar.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [../platform/ipc.md](../platform/ipc.md) — how a shortcut reaches a
  plugin action.
