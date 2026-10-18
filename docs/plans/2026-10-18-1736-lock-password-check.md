# The lock screen checks the password with its shipped settings

Date: 2026-10-18 17:36 EDT

## Context

With nothing set by the user, the lock screen checks the typed password
against the PAM service the package installs, `/etc/pam.d/wayland-canvas`,
and the right password unlocks. The locker takes the service name and the
directory from its plugin settings so a test can point both at a scratch
service. The shipped directory was an empty string, which Quickshell's
`PamContext` refuses as no directory: no password could be checked, and a
locked session could only be ended. The scenarios set both settings, and the
shipped directory cannot be exercised there without the machine's own login
stack, which tests never touch.

The contract is [../intents/shell/lock.md](../intents/shell/lock.md). The work
grew out of the on-screen lock box of
[2026-10-17-0722-login-session-gaps.md](2026-10-17-0722-login-session-gaps.md).

## Tasks

- [x] The locker's shipped PAM directory is `/etc/pam.d`, where both packages install its service; the intent names it — proven on the SDDM login by the user unlocking with that directory set
- [x] Both package builds stop unless the lock screen's shipped PAM settings name a service file the package installs (`packaging/check-lock-pam.sh`): it fails on the empty directory and passes on `/etc/pam.d`; `tools/package.sh` green for both formats
- [ ] The fixed package installed on the SDDM login, the `pamConfigDirectory` override taken out of `~/.config/wayland-canvas/shell.json`, and a lock unlocked with the real password (the user)
- [ ] The lock screen's clock keeps time: it shows the time the shell started, because its timer runs on `WlSessionLock.locked`, whose change Quickshell 0.3.1 announces on unlock only
- [ ] A shell reload while locked (`shell.json` edited, a plugin file changed, the package reinstalled) destroys the lock object after `locked`; wlroots ends the shell for that protocol error and nothing draws a locker again (the user to decide: the host holds reloads until unlock, or the lock is carried across a reload)
- [ ] A shell started while the lock is abandoned does not take it over: the compositor ignores lock requests while locked, so only `wayland-canvas msg action.run` with `canvas.lock/lock` brings a locker back (the user to decide: the compositor asks a shell that connects to an abandoned lock to lock, or a lock request while abandoned runs the action again)
