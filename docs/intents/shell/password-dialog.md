# Password dialog

> Owns what an application asking for administrator rights gets: the polkit
> agent the session registers and the dialog it shows. The plugin model is
> [plugins.md](plugins.md); the lock screen's own password field is
> [lock.md](lock.md)'s.

## Requirement

An application that asks for a privileged action — a package manager, a
system setting, `pkexec` — is answered by a dialog in this session: it says
what is being asked and who is being asked, takes the password, and tells
polkit the answer. While it is open it holds the keyboard, so no part of
the password reaches a window behind it.

## Design considerations

- **The agent is a shell plugin** (`canvas.polkit`) on Quickshell's
  `PolkitAgent`: the compositor knows nothing of polkit, and a session that
  already has an agent (another desktop on another VT, a GNOME session)
  keeps it.
- **The dialog takes the keyboard exclusively**, the one shell surface that
  does — a password typed into a window behind it would be a leak, and a
  notification popup must not be able to steal it back
  ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)).
- **A refused registration is not a failure of the shell.** The agent says
  so once and everything else of the shell keeps running.

## Behavior

| Subject | Rule |
|---|---|
| When it opens | polkit asks this session to authenticate an action |
| What it shows | the action's own message, the identity the password is for, a masked field with polkit's prompt, and what polkit last said (an error in the attention colour) |
| Which output | the output the dialog's surface is placed on; the screen behind it is dimmed |
| Authenticate | the typed answer goes to polkit; the dialog stays for its verdict |
| A wrong password | polkit says so on the dialog and asks again; the field is cleared |
| Cancel | the Cancel button, Escape, or a click on the dimmed screen: polkit is told the request was cancelled and the dialog goes |
| A session that already has an agent | registration is refused, the shell logs it once and runs on with no dialog of its own |

## Error states, corner cases

- **No polkitd on the bus** (a run on a bus of its own): no agent
  registers; nothing asks for a password.
- **The requesting application goes away** while the dialog is open: polkit
  cancels the request and the dialog closes with it.
- **Proving it without a polkitd**: the plugin carries a stand-in flow that
  answers exactly what polkit's does, so a scenario drives the dialog's
  states — asked, refused, authenticated, cancelled — without ever
  registering an agent with the machine's own polkitd.

## Cross-links

- [plugins.md](plugins.md), [lock.md](lock.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md).
