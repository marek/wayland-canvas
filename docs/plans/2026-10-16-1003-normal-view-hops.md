# Hops in the normal view

Date: 2026-10-16 10:03 EDT

## Context

A 3-finger swipe in the normal view hops to the nearest desktop in the direction the fingers move, easing onto it
at its scale. Where no desktop lies that way the view stays where it is and a hint says so: after trying the first
version, which moved one screen into empty canvas there, the user found that still lost their place. The free pan
with momentum that the swipe used to do is gone for the same reason. Free areas are reached from the overview, by
a pointer drag or a 2-finger scroll over empty canvas, or with Super+Alt+arrows. The rules are in
[input/gestures.md](../intents/input/gestures.md) and [canvas/infinite-canvas.md](../intents/canvas/infinite-canvas.md).

## Hops

- [x] A 3-finger swipe in the normal view, or a sloppy 3-finger pinch, hops to the nearest desktop in the swipe's direction at its scale; one hop per swipe
- [x] With no desktop that way the view stays and a hint says so: the normal view never hops into empty canvas (Super+Ctrl+Arrows the same)
- [x] A swipe made while a hop still eases starts from where that hop lands, so quick swipes walk desktop by desktop (Super+Ctrl+Arrows too)
- [x] The momentum, the landing snap and their constants go with the free 3-finger pan

## Verification

- [x] The gestures scenario hops onto the desktop on either side, one screen into a free area and back, keeps a long swipe to one hop, walks two desktops with two quick swipes; the earlier phases that panned now hop
- [x] The input scenario's device swipes hop
- [x] Intents, README and `-h` state the hop; warning-free build, every suite green
- [ ] On screen: 3-finger swipes on the touchpad in the normal view, nested and on a TTY (the user)
