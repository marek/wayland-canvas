# Lock screen

> Owns locking the session: what a locked screen shows and blocks, what
> locks it, and the locker that draws it. The protocol's place in the
> supported set is [../platform/protocols.md](../platform/protocols.md); the
> keys are [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)'s
> and the idle timeout is set in [settings.md](settings.md).

## Requirement

A locked session shows the locker and nothing else: no window, no bar, no
canvas — on screen, in a screenshot, in a recording and in a shared screen
alike. Input reaches only the locker. The only way back is the password,
checked through PAM. A locker that dies leaves the session locked.

## Design considerations

- **The compositor owns the locked state, the locker only draws it.**
  ext-session-lock-v1 is the contract, so swaylock or hyprlock locks this
  compositor exactly as the built-in locker does, and the lock survives the
  locker's death — the one property a screen lock cannot afford to lose.
- **The locker is a shell plugin** (`canvas.lock`) on Quickshell's
  `WlSessionLock` and `PamContext`, like every other part of the shell
  ([plugins.md](plugins.md)). A run started with `-B` has no shell, so it
  has no locker: the triggers then ask for a lock that nothing draws, and
  the session stays unlocked.
- **One opaque fill is the floor.** An output the locker has not drawn yet
  — a monitor plugged in while locked, or a locker that died — is filled
  opaque rather than left showing what is behind it.
- **Every trigger is the same action.** The shortcut, logind and the idle
  timeout all run the locker's `lock` action over the control socket
  ([../platform/ipc.md](../platform/ipc.md)), so one locker answers all of
  them and a replacement locker needs nothing else.

## Behavior

### While locked

| Subject | Rule |
|---|---|
| What is drawn | the locker's surface for that output; an output without one is an opaque fill. Nothing else: no window, layer surface, overview, launcher or canvas chrome |
| Captures | a screenshot, a recording and a shared screen take the same path as the screen, so they show the lock; a capture of a single window is refused while locked |
| Keyboard | the locker's surface for the output under the pointer, else any of its surfaces; no canvas shortcut, no `[commands]` chord, no Super tap, and no client behind the lock |
| Pointer and touchpad | motion, buttons and scrolling go to the locker's surface; canvas gestures do nothing and are not forwarded |
| The way out | VT switching (Ctrl+Alt+F1…F12) stays, as everywhere ([../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md)) |
| Unlocking | the locker asks the protocol to unlock after PAM says the password is right; the windows, the focus and the bar come back as they were |

### Locking

| Trigger | Rule |
|---|---|
| The `lock` action | Super+L by default, changed like any shortcut ([../input/config-file.md](../input/config-file.md)) |
| `loginctl lock-session` | the session's logind Lock signal; a login session only — a nested or headless run watches no logind session |
| Suspend and hibernate | the lock is drawn before the machine sleeps: the compositor holds a logind delay inhibitor and releases it once the locker covers every output, or after the wait in `src/core/config.h` if no lock is drawn |
| The idle timeout | `lock-idle` in `canvas.conf`'s `[session]` section, 5 minutes by default, `off` for never; set in the settings window. Any input restarts the countdown, and an idle inhibitor (a video player) stands it down while it lives |

### The locker

| Subject | Rule |
|---|---|
| Surfaces | one per output, covering it, with the clock, the date and a password field |
| The password | checked through PAM against the service the package installs as `sysconfdir/pam.d/wayland-canvas`, which is `/etc/pam.d/wayland-canvas` for both packages; the service name and the directory it is read from are plugin settings, `wayland-canvas` and `/etc/pam.d` unless set, so a test can point them at a scratch service. The directory is always a real one: Quickshell's `PamContext` refuses an empty directory and checks no password at all |
| A wrong password | the field clears and says so; the session stays locked and the attempt can be made again |
| Another locker | swaylock, hyprlock or any other ext-session-lock client locks the session the same way; the plugin's own lock is only the one the shell ships |

## Error states, corner cases

- **A second locker while one is live**: refused — it is told the lock is
  finished and draws nothing.
- **The locker dies while locked**: the session stays locked with every
  output filled opaque, and the next locker to take the protocol draws the
  lock again and can unlock it.
- **An output appears while locked**: it is filled opaque until the locker
  gives it a surface.
- **A lock asked for with no locker in the session**: nothing is locked and
  the ask is logged; the trigger is harmless.
- **PAM unavailable** (no service file): the locker says the password
  cannot be checked and the session stays locked.

## Cross-links

- [plugins.md](plugins.md) — the plugin model the locker lives in.
- [../platform/protocols.md](../platform/protocols.md),
  [../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md),
  [settings.md](settings.md),
  [../workspace/packaging.md](../workspace/packaging.md) — what the package
  installs.
