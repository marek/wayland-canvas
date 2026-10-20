# Screen capture

> Owns the shots-and-recordings UI the compositor draws itself: the key
> chords, the region/window/output picker, the capture toolbar, the
> after-shot thumbnail, and the recorder's life cycle. The offscreen
> captures underneath are
> [../render/rendering.md](../render/rendering.md)'s; the protocols and
> portal sharing are [../platform/protocols.md](../platform/protocols.md)'s;
> the `capture.pick` method is [../platform/ipc.md](../platform/ipc.md)'s.

## Requirement

macOS-style screenshots and recordings from the keyboard: a shot of the
whole screen, of a dragged region or of one window, each saved, copied
to the clipboard and shown as a thumbnail; recordings of a screen, a
region or a window, stopped from the bar. Adding Ctrl to a shot chord
copies instead of saving.

## Behavior

### Keys

| Keys | Action |
|---|---|
| Super+Shift+3 | Shot of the whole output |
| Super+Shift+4 | The picker: drag a region, click a window, Space windows-only, Esc cancel |
| Super+Shift+5 | The capture toolbar (shots and recordings, options) |
| Ctrl on any shot chord | Copy to the clipboard instead of saving: no file, no thumbnail |
| Print | The same picker as Super+Shift+4 |
| Shift+Print | Shot of the whole output |
| Alt+Print | Stop a running recording, else the toolbar with the recording choices accented |

The chords work with the picker or the toolbar up — the same chord
closes what it opened. The digit chords are matched through the key's
unshifted symbol too (Super+Shift+3 is `#` on a US layout); see the
matching rule in
[../input/keyboard-and-pointer.md](../input/keyboard-and-pointer.md).
Nothing here needs the recorder: shots never depend on it.

### The picker

Region mode dims the screen, shows a crosshair, and a drag frames the
region with its size labelled beside it. A click instead of a drag
picks the window under the pointer, its frame and all; a click on the
empty canvas picks nothing and the picker stays. Space toggles between
region and window picking; in window mode the hovered window's frame is
highlighted and the empty canvas is no pick. Esc cancels.

The portal's chooser may ask for `any`: window picking, but a click on
the empty canvas picks that output (its own outline shown while
hovered), because a browser offers windows and monitors in one list.
Region picking stays on its own output; window, output and `any`
picking follow the pointer onto every output.

A picked window is captured alone — title bar, border and content,
nothing overlapping it — at the output's scale whatever the canvas
zoom. A window *recording* keeps the rectangle the window's frame had
at the moment it was picked, whatever the window does after.

### The toolbar

Super+Shift+5 opens the toolbar at the bottom of the output: six modes
— shot / shot window / shot region, then record screen / record window
/ record region — with the options row above: destination (saved to
the `Screenshots` folder of the XDG pictures dir, or the clipboard),
timer (none / 5 s / 10 s) and whether the pointer is drawn into the
capture. A mode click closes the toolbar and runs; a window or region
mode opens the picker first. Esc closes. Options persist between
opens. Alt+Print with nothing recording opens it with the recording
modes accented.

### After a shot

A saved shot lands in the XDG pictures dir's `Screenshots` folder as
`Screenshot <date> at <time>.png` (a same-second shot gets a suffix,
never an overwrite), is copied to the clipboard as `image/png`, and is
shown as a bottom-right thumbnail for a few seconds: a click opens the
file, a swipe dismisses it. The thumbnail never lands in a capture
taken under it — offscreen captures exclude the compositor's own UI.

### Recording

Recordings are the compositor's own doing: each frame is an offscreen
capture of the chosen screen, window or region (the same render path a
shot takes, the compositor's own UI excluded), piped as raw frames to
`ffmpeg` (a dependency of both packages) for H.264 encoding — so nested runs
and the TTY both work, no external capture protocol involved. Files land
in the XDG videos dir as `Screen Recording <date> at <time>.mp4`. The
bar's recording dot (a shell plugin) appears while one runs: red, a
click stops it, like Alt+Print. A stop ends the recorder on the end of
its input, so the file is left finished and playable and the dot is
gone, whatever was started while the recording ran. An encoder that falls
behind loses frames; the compositor never waits for it, so input and
drawing keep their pace while a recording runs. A recording that cannot
start says so with a hint; shots are unaffected. The timer, when set,
counts down on a hint first; Esc cancels it.
