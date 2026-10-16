# wayland-canvas

## What wayland-canvas is

A Wayland compositor (wlroots 0.20, C++26) built on one idea: an
infinite canvas combined with a wide array of virtual desktops.
Windows live anywhere on a single unbounded plane; every display shows
that plane through its own camera; a 3-finger swipe moves the canvas
anywhere. Virtual desktops are screen-sized dashed frames on the same
plane, positioned over the windows however you like — moving a frame
never moves the windows under it. Around that core: Windows-11-style
title bars with minimize, maximize/restore and close, and a
Start-style launcher on a tap of the Windows key.

Touchpad gestures lead; the keyboard and the pointer reach the same
actions.

## The experience

| Area | What you get |
|---|---|
| Canvas | One unbounded plane; every output has its own camera onto it; a 3-finger swipe hops to the closest desktop in that direction, or one screen that way where none lies |
| Desktops | Screen-sized dashed frames; a window belongs to the desktop its frame centre lies in; frames are created, abandoned, combined and tiled from the overview |
| Overview | A 4-finger swipe up zooms out over everything; a target marks the landing spot; a 3-finger swipe hops its target as it hops the camera in the normal view |
| Free areas | Anywhere that is not a desktop is still a place to work: pan there, hop there, zoom in there |
| Windows | Server-side Windows 11 title bars; new windows open centred on the screen you are looking at; restoring a window brings you to it |
| Opening apps | A tap of Super, or Super+Space, opens a Start-style launcher over the standard XDG desktop entries; adding an item is a `.desktop` file in `~/.local/share/applications` |
| Displays | Scale 2 on dense panels by the dpi rule, 1 elsewhere; crisp at 1:1; runs from a spare TTY or nested in another compositor |

## Principles

1. **Free areas are as easy as desktops.** The canvas is the thing;
   desktops are marks on it. Anything you can do at a desktop you can
   do in the empty canvas beside it.
   ([canvas/infinite-canvas.md](canvas/infinite-canvas.md))
2. **One source of truth for a window's place: its canvas position.**
   Desktop membership, the maximize area and what an output shows are
   derived from it; moving a desktop frame never moves windows.
   ([canvas/desktops.md](canvas/desktops.md),
   [windows/window-management.md](windows/window-management.md))
3. **Clients never see the zoom.** The canvas zoom is
   compositor-internal; a client deals in logical pixels and its
   output's scale, so panning and zooming never make clients re-render.
   ([render/rendering.md](render/rendering.md),
   [render/hidpi.md](render/hidpi.md))
4. **Compositor shortcuts never steal application shortcuts.** Every
   canvas shortcut includes Super; Ctrl+arrows, Ctrl+D, Alt+arrows and
   Ctrl+F4 keep working inside apps.
   ([input/keyboard-and-pointer.md](input/keyboard-and-pointer.md))
5. **Gestures lead; keys and pointer complete.** Every overview action
   has a gesture, a key and a pointer path.
   ([input/gestures.md](input/gestures.md),
   [canvas/overview.md](canvas/overview.md))
6. **Native feel at 2x.** Text, title bars and the launcher are drawn
   at the output scale; at 1:1 the camera sits on the pixel grid so
   windows are never blurry. ([render/hidpi.md](render/hidpi.md))
7. **Standard where a standard exists.** Windows 11 for the look of a
   window, XDG desktop entries for the app list, wlroots for the
   platform, GNOME's dpi rule for the scale.
   ([windows/title-bars.md](windows/title-bars.md),
   [apps/desktop-entries.md](apps/desktop-entries.md),
   [platform/wlroots.md](platform/wlroots.md))
8. **Proven, not presumed.** Behaviour is proven on the headless
   backend and on screen before it is called done.
   ([workspace/build-and-run.md](workspace/build-and-run.md))
9. **One repo, nothing public without the user.**
   ([workspace/repos.md](workspace/repos.md))

## How the corpus is organised

By area: the workspace
([workspace/](workspace/)), the canvas
([canvas/](canvas/)), input ([input/](input/)), windows
([windows/](windows/)), apps ([apps/](apps/)), rendering
([render/](render/)), the platform ([platform/](platform/)), and bug
case studies ([bugs/](bugs/)) — the one place history is told.
[README.md](README.md) is the index and fixes the reading paths;
[glossary.md](glossary.md) fixes the words. Plans are volatile
checklists in [../plans/](../plans/README.md); what a plan decides is
written here, not there.
