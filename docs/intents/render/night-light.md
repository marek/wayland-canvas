# Night light

> Owns warming the screen: the switch, the temperature, where the filter is
> applied, and the gamma-control protocol a client uses instead. The
> renderer it rides on is [rendering.md](rendering.md); the file the choice
> lives in is [../input/config-file.md](../input/config-file.md).

## Requirement

The screen can be warmed by a colour temperature, kept across restarts and
turned on or off from the brightness panel, the settings window or a key.
What the filter changes is what the display shows — not what a screenshot,
a recording or a shared screen carries: it rides the output's own gamma
ramp. An output with no ramp of its own (a nested or headless run) shows
no night light, and says so to a client that asks for its gamma.

## Design considerations

- **The compositor owns it, not a client.** Warming the screen is a
  property of the output, so it holds while no client is running and
  survives a shell reload.
- **The gamma-control protocol is supported all the same**, so wlsunset and
  gammastep work — with their own schedules, which the built-in filter has
  none of. A client holding an output's gamma owns that output: the
  built-in filter steps aside there and comes back when the client lets go.
- **The ramp is the mechanism.** The filter is a colour transform
  committed with the output's state, which is what keeps every capture in
  true colours. Drawing the same warming into the frames is not a
  substitute: a per-channel multiply is not something the renderer's
  passes offer, and an overlay drawn at some alpha lightens as much as it
  warms.
- **Out of scope**: a sunset schedule of our own and Hyprland's CTM
  protocol. A client that wants a schedule brings one through
  gamma-control.

## Behavior

| Subject | Rule |
|---|---|
| The switch | `night-light` in `canvas.conf`'s `[session]` section, off by default |
| The temperature | `night-light-temperature`, 1000 K to 6500 K, 4000 K by default; 6500 K is daylight and changes nothing |
| Where it is set | the brightness panel's night-light row, the settings window's Session page, and the `night-light` action (Super+Ctrl+N), which writes the file like any other change |
| Over the IPC | `settings.get` reports `session.nightLight` and `session.nightLightTemperature`; `settings.set` changes either ([../platform/ipc.md](../platform/ipc.md)) |
| On an output with a gamma ramp | the transform is committed with the output's state: the display is warm and every capture stays in true colours |
| On an output without one | the setting is kept and nothing is warmed; the frames are what they were |
| A gamma-control client | its table replaces whatever the compositor was showing on that output; the built-in filter applies again as soon as the client is gone. An output with no ramp tells the client its request failed |

## Error states, corner cases

- **A table the output refuses**: the client is told its request failed and
  the output keeps what it had.
- **A temperature outside the range**: refused with the range in the error;
  the file keeps its line, which is then one of the lines the config
  reports as ignored ([../input/config-file.md](../input/config-file.md)).
- **An output hotplugged while the filter is on**: it comes up with the
  filter, like every other.

## Cross-links

- [rendering.md](rendering.md), [../platform/outputs.md](../platform/outputs.md),
  [../platform/protocols.md](../platform/protocols.md),
  [../shell/bar.md](../shell/bar.md),
  [../shell/settings.md](../shell/settings.md),
  [../input/config-file.md](../input/config-file.md).
