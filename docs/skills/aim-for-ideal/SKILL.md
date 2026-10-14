---
name: aim-for-ideal
description: When a task has both a quick path that resembles what was asked and the proper structure that was actually asked for, build the proper one - and when the proper one is genuinely blocked by something larger, name it and its cost up front instead of silently shipping the shortcut. Use whenever a solution works by a shortcut (scaling each window where a camera was asked for, a private file format where a standard was asked for, a special case where the shared mechanism was asked for), and whenever tempted to defer part of a request as "optional" or "for later".
---

# Aim for the ideal solution, don't silently settle

The same miss shows up in every project: asked for a structure, the
first version is a shortcut that resembles it. In this repo the
decided answers all have a proper form:

- Asked for a zoomed-out overview of the canvas, scaling each window's
  buffer would have resembled it; what was asked for is a camera per
  output onto one plane — zoom is a property of the view, not of each
  window.
- Asked to add launcher items "the standard way", the answer was the
  XDG desktop-entry standard every Linux launcher reads — not a private
  file format of our own.
- Asked for Windows-style title bars and a Start-style launcher, they
  were the task, with the Windows 11 metrics — not optional polish to
  trim.

**The rule.** When the request names a structure or an outcome, build
that. A thing that compiles and resembles the request is not the
request. If the proper solution is blocked by something outside the
immediate change (a larger restructuring, a missing piece, a decision
that is the user's), say so at the start: name the ideal, say what it
takes, and either do it or put the decision in the plan and carry on
with the rest. Do not ship the shortcut and mention the better option
only when asked.

**How established compositors and desktops do it is the default
reference.** For protocol handling, `tinywl` and `wlr_scene` in the
subproject; for look and interaction, Windows 11. Ask what existing
systems do before inventing something smaller.

**Never relabel work as optional.** If the user asked for it, it is in
scope. "Too much work" is not a reason to skip; it is a reason to plan.
What is genuinely the user's call (a taskbar, Xwayland, an
output-configuration protocol) is written into the plan as a decision,
and work continues elsewhere.

**This is not scope creep.** It is not "always do the bigger thing".
It is "do what was asked at the quality it was asked for, and surface a
better option as a real decision rather than a silent default".
