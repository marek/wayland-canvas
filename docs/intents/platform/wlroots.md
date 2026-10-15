# wlroots

> Owns the platform rule: the wlroots version, the subproject, and how
> its objects and requests are handled.

## Requirement

wlroots 0.20 is the platform and the reference for compositor
behaviour: how a Wayland compositor answers clients, seats, inputs and
outputs. It is the reference for behaviour and features, not for code
structure — the canvas and its renderer are this project's own
([../render/rendering.md](../render/rendering.md)).

## Behavior

| Subject | Rule |
|---|---|
| Version | wlroots 0.20 (accepted: ≥ 0.20.0, < 0.21.0); the wrap pins 0.20.2 |
| Linking | the system `wlroots-0.20` when installed, else the meson subproject linked statically ([../workspace/build-and-run.md](../workspace/build-and-run.md)) |
| Reference reading | tinywl, in the subproject, is the reference compositor; the wlr_scene sources are the reference for surface and output handling even though the renderer does not use wlr_scene |
| Listener balance | every signal an object handles is a `Listener` member of that object ([cpp-standards.md](cpp-standards.md)); a listener disconnects when its owner is destroyed, and every server-level listener is gone at shutdown — wlroots asserts that an object being destroyed has no listeners |
| request_* handlers | a request handler starts the action through wlroots and returns; it never re-emits the request signal, which would recurse into the handler. The pointer-drag request is the canonical case: call the seat's start-drag entry point, not the signal |
| xdg lifecycle | the initial commit is answered with a configure; maximize, fullscreen and minimize requests are answered with a scheduled configure once the surface is initialized ([../windows/window-management.md](../windows/window-management.md)) |

## Error states, corner cases

- A listener wired outside a `Listener` member (`src/core/util.h`) is not a
  leak but a crash: the assert fires on destroy. Count listeners in
  both directions when wiring a new object by hand against the wlroots
  C API.
- A request answered by a request loops: a new request handler is
  checked to end in a real action.

## Cross-links

- [cpp-standards.md](cpp-standards.md) — the `Listener` and ownership
  rules the C++ side carries.
- [protocols.md](protocols.md), [outputs.md](outputs.md),
  [../render/rendering.md](../render/rendering.md),
  [../workspace/repos.md](../workspace/repos.md).
