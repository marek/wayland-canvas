# Trackpad clicks and launches from the overview

Date: 2026-10-16 14:58 EDT

## Context

On the touchpad a right click never came through: pressing the pad with two fingers stayed a left click, because
only a press in the bottom-right corner counted as a right one. The touchpad now works the way the user's previous
desktop set it: two fingers pressed anywhere are a right click, three a middle click. In the overview a right click
is how desktops are made and unmade on a trackpad — on the empty canvas it makes a desktop there, on a desktop (its
tab or its interior) it removes that desktop, whose windows stay on the canvas.

An app launched from the overview opens in the overview's target — the desktop or the free spot its outline marks —
not on the desktop the overview was opened from. That holds even when the overview closes, or its target moves,
before the window appears: a slow app opens where it was launched for. A window the launch cannot be traced to (one
handed to an instance already running, one started over D-Bus) is placed by the usual rules.

## Clicks

- [ ] Two fingers pressed anywhere on the pad are a right click, three a middle click
- [ ] In the overview a right click on a desktop (tab or interior) removes it, on the empty canvas makes one there

## Launches

- [ ] A window launched from the overview opens in the target the overview had at the launch, whatever the overview does before it appears

## Verification

- [ ] Scenarios: a right click removing a desktop and making one; a launch from the overview whose window appears after the overview closed, opening in the launch's target
- [ ] Intents, README and `-h` updated; warning-free build, every suite green
- [ ] On screen: two-finger clicks on the touchpad, in the overview and in apps, and a launch from the overview (the user)
