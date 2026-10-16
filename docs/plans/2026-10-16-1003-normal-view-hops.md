# Hops in the normal view

Date: 2026-10-16 10:03 EDT

## Context

A 3-finger swipe in the normal view moves the way it does in the overview: it hops to the nearest desktop in the
direction the fingers move, easing onto it at its scale, and where no desktop lies that way it moves one screen
that way. The free pan with momentum that the swipe used to do is gone: it left the view stranded between desktops
and made it easy to lose one's place. A free area is still one swipe away wherever no desktop lies, and a pointer
drag or a 2-finger scroll over empty canvas still pans freely. The rules are in
[input/gestures.md](../intents/input/gestures.md) and [canvas/infinite-canvas.md](../intents/canvas/infinite-canvas.md).

## Hops

- [ ] A 3-finger swipe in the normal view, or a sloppy 3-finger pinch, hops as in the overview: the nearest desktop in the swipe's direction at its scale; none that way, one screen that way at the view's zoom; one hop per swipe
- [ ] A swipe made while a hop still eases starts from where that hop lands, so quick swipes walk desktop by desktop (Super+Ctrl+Arrows too)
- [ ] The momentum, the landing snap and their constants go with the free 3-finger pan

## Verification

- [ ] The gestures scenario hops onto the desktop on either side, one screen into a free area and back, keeps a long swipe to one hop, walks two desktops with two quick swipes; the earlier phases that panned now hop
- [ ] The input scenario's device swipes hop
- [ ] Intents, README and `-h` state the hop; warning-free build, every suite green
- [ ] On screen: 3-finger swipes on the touchpad in the normal view, nested and on a TTY (the user)
