---
name: fresh-deploy
description: wayland-canvas has no installed base to stay compatible with - protocol usage, config.h layout, code structure and file names may change freely, with no compatibility shims. But removing the old implementation of something is never licence to remove the capability it provided. Use before deleting a file, function, constant or feature, before writing a compatibility path or alias, and when restructuring code that carries features.
---

# Assume a fresh deployment

Nothing running today constrains what can change. There are no clients
in the field depending on our behaviour beyond the fixed Wayland
protocols we implement, no saved state in an old format, no installed
config. Default to the clean shape of the code.

## Removing old code must never remove what it did

This is the load-bearing rule.

The built-in launcher replaced an external-launcher fallback; the
capability — opening apps — carried over, and the fallback went only
once its replacement covered it. Nobody ever decides to remove a
capability; what removes them is replacing the code that carried them
with code that does not.

"No backward compatibility" is about not keeping duplicate old
implementations beside their replacement. It is never licence to drop a
capability because carrying it over is more work. Before deleting or
replacing anything, list what it does beyond the obvious path — every
gesture, every protocol interaction, every corner case — and confirm
each still works on what replaces it. If re-homing one of them is real
work, that is a reason to plan it, not to drop it.

Two checks that apply here often:

- **Commented-out or disabled code may have been broken, not
  unwanted.** Restoring a feature means validating that its logic makes
  sense, not pasting it back.
- **Restructuring changes no behaviour.** A file split, a rename or a
  helper move lands with `tools/check.sh` passing and the same pixels
  on screen.

## What "remove, don't deprecate" means here

- **No aliases for old conventions.** A renamed constant or function
  leaves no old name behind; `src/core/config.h` has one name per tunable.
- **No compatibility paths in protocol usage.** The decoration
  negotiation has one policy; there is no legacy mode beside it.
- **The wlroots subproject is fetched source, not ours.** It is never
  edited, restyled or pinned back for our convenience; if it lacks
  something, our code works around it on our side of the boundary.
- **Rename freely** when a name is wrong, without leaving the old one.

## The exception: what a person has made

- **A user's desktop entries** in `~/.local/share/applications` and
  their icon theme choices. The launcher keeps reading the standard
  locations and the standard fields; a change to how entries are
  treated keeps the user's overrides meaningful (theirs wins,
  `NoDisplay=true` hides) or resets to sensible defaults on purpose,
  never silently breaks them.
- **A contributor's uncommitted files** in any working tree.

## What does not count as data

Build folders, `tmp/`, the fetched subproject, logs, captures,
screenshots. Change their shape freely and rebuild.

## When in doubt

If the change touches something a person made or configured, treat it
as data. If it only touches code or something rebuilt from source —
make the change and remove what it replaces, once the replacement does
everything the old thing did.
