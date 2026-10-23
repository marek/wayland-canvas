# C++ standards

> Owns what the source looks like and what builds it. The `code-style`
> skill is the short form for day-to-day use.

## Requirement

The compositor is C++26 built by LLVM's clang. The language carries what
a compositor gets wrong most often: every wlroots listener, texture and
handle belongs to an object that releases it, every compositor object has
exactly one owner, states of different kinds cannot be mixed up, and code
sits on the object it acts on.

## Design considerations

- **The newest standard the toolchain implements.** A C++26 feature is
  used once clang and the standard library it builds against both have
  it; contracts are the one C++26 feature clang 22 lacks.
- **LLVM builds everything**, the wlroots subproject included. clangd
  reads `build/compile_commands.json`.
- **wlroots stays C.** Its structs and functions are used as they are;
  a C++ type exists only where it owns a lifetime.
- **wlroots destroys objects on its own schedule** and aborts when one
  still has listeners. Ownership is therefore single and explicit, and a
  listener removes itself.
- **Per-frame paths run 60 to 240 times a second** (rendering, pointer
  motion, gesture updates): they do not allocate.

## Behavior

### Toolchain

| Subject | Rule |
|---|---|
| Compiler | `clang++`; `clang` for the wlroots subproject. `toolchain/llvm.ini` selects them; `meson.build` stops with the command to use when another compiler is configured |
| Standard | `-std=c++26`; no compiler extensions in our code |
| Standard library | The one clang finds: libstdc++ here, libc++ where installed |
| Linker | The system linker; `lld` once installed, set in `toolchain/llvm.ini` |
| Warnings | `-Wall -Wextra -Wpedantic` clean; library headers are system headers, so the warnings cover our code |
| Exceptions, RTTI | Off (`-fno-exceptions -fno-rtti`). Errors are values; an allocation failure aborts |

### What C++ carries

| Need | Form |
|---|---|
| A wlroots signal | A `Listener` member of the object that handles it. `connect<&Class::handler>(signal, this)` names a member function taking nothing or the signal's data pointer; destroying, reconnecting or `disconnect()` removes it |
| A C object with a destroy function (texture, cairo surface and context, pango layout, `JsonParser`, event source) | `Handle<T, destroy>`, a `std::unique_ptr` with that function as its deleter, in the wlroots-free `src/core/handle.h`; `TexturePtr` and friends in `src/core/util.h`, `JsonParserPtr` and friends in `src/core/json.h`. Text a C library returns is released with its own free (`g_free` for glib), never `std::free` |
| A file read, an atomic write, a directory made; text quoted for `/bin/sh` or JSON; a key-file read | `fs::` in `src/core/fs.h` and `text::` in `src/core/text.h`, both free of wlroots and shared with the usage helper. A text file is written to a temporary file and renamed, and the write checks `fwrite` and `fclose` before the rename |
| A compositor object (output, view, popup, layer surface, keyboard, desktop) | Owned by exactly one `std::vector<std::unique_ptr<T>>` of its owner. Its wlroots destroy handler ends with `destroy_owned(owner, this)` |
| A reference that does not own | A raw pointer or reference for the length of a call. Kept longer: a name resolved when it is used (`DesktopId` through `Desktops::find`, `nullptr` once the desktop is gone). Outputs and views have no names: the one subsystem holding such a pointer is told when they go (`forget`, `output_gone`); no other code writes its fields |
| A state, kind or mode | `enum class`, enumerators `snake_case`. Plain integers only for masks and values shared with a C API (`wlr_edges`, layer indices). A kind that brings its own behaviour and its own wlroots object is a subclass instead (Classes, below) |
| A state machine whose states carry different data | A `std::variant` of one struct per state (`Gestures`, `PointerGrab`), each holding only what that state needs; `std::visit` or `std::get_if`. No shared fields plus a kind enum |
| A result that may be absent | `std::optional`; out-parameters are not used |
| Several results | A small struct returned by value (`ViewHit`, `TabLayout`) |
| A sequence passed in | `std::span` |
| Text | `std::string` owned, `std::string_view` read; `const char *` where it goes straight to a C API that needs the terminator |
| A list built per frame | `std::inplace_vector`, or a member vector reused with `clear()` |
| A callback into our code | `std::function_ref` parameter. From C (iterators, timers): a captureless lambda or static member, `void *` cast back with `static_cast` |
| A constant | `constexpr`. Tunables live in `src/core/config.h`, namespace `config` |

### Classes

| Kind | Shape |
|---|---|
| A value (`Rect`, `Camera`, `Point`) | `struct`, public fields with default member initialisers, `constexpr` members, defaulted `==` |
| An object with a lifetime (`Server`, `Output`, `View`, `LayerSurface`, `Keyboard`) | `class`, neither copyable nor movable. Listeners, wlroots handles and caches are private; state other classes read or legitimately set is public, changed through the member function that keeps an invariant where one exists (`move`, `set_minimized`) |
| One per compositor (canvas, input, gestures, bindings, hint, launcher, painter, decorations, ipc, ipc_events) | A class held by value in `Server`; nothing is global, so a test builds a whole `Server` in one process |
| Fallible setup | `bool init()` after construction; a constructor never fails |
| Teardown order | `~Server` releases wlroots resources explicitly, in reverse order of creation; each subsystem's `finish()` releases its own before the display goes |
| Inheritance | Only where something is polymorphic; not for sharing code |
| A kind fixed at creation whose behaviour differs (a window's shell) | An abstract base with one `final` subclass per kind: `View`, and `XdgView` and `X11View`. The kind's wlroots object is private to its subclass, so code holding the base cannot reach one kind's object on the other kind. RTTI is off and there is no `dynamic_cast`; the one subsystem that exists for a single kind (the X window manager) reaches its windows through a virtual on the base (`View::as_x11()`) |

A class is one concept of the compositor: the canvas and its desktops, a
screen's camera and its overview, gestures, pointer grabs, bindings. Other
classes ask it through queries and change it through its members. A class
spans files only for a separate concern of the same object (`Output`'s
layer arrangement in `platform/layer.cpp`).

### Layout

Sources sit in one folder per area of the compositor; `src/main.cpp` alone
stays at the root.

| Folder | Holds |
|---|---|
| `src/core/` | `Server`, the wlroots wrapper, ownership helpers, tunables, canvas geometry |
| `src/canvas/` | The canvas and its desktops; a screen's camera and overview, and the overview's chrome (the tabs, the shelf, the help line: their layout, labels and drawing) |
| `src/windows/` | Views, popups and their title bars |
| `src/input/` | Devices and the seat, gestures, pointer grabs, key bindings |
| `src/render/` | The painter, the drawing primitives of a render pass, the hint and text drawing |
| `src/platform/` | Outputs, layer surfaces, the control socket |
| `src/apps/` | The launcher, installed applications, icons, desktop entries |
| `src/shell/` | Helper programs the shell's plugins run as child processes (the agents panel's usage collector) |

Input and rendering meet only at the drawing types: `src/input/` includes
nothing from `src/render/` but `RenderCtx`, which a UI layer draws with. What
the overview draws and what a press on it hits read one layout, the overview
chrome's.

A new file goes in the folder of the concept it belongs to. Includes are
written from `src/` (`#include "canvas/desktop.h"`), so a moved file changes
its includers' lines and nothing else. An area that outgrows one folder
splits into subfolders, never more than two levels below `src/`.

### Code

| Subject | Rule |
|---|---|
| Files | `.cpp` sources, `.h` headers with `#pragma once`; a header per class or close group |
| wlroots | Only through `src/core/wlr.h`, the `extern "C"` wrapper; other C libraries directly |
| File-local code | An unnamed namespace |
| Null, casts | `nullptr`; named casts only |
| Macros | None of our own; constants are `constexpr`, helpers `constexpr` functions or templates |
| `auto` | When the type is on the same line, or for iterators and lambdas |
| Initialisation | Designated initialisers in declaration order for C structs; default member initialisers on our types; never `memset` |
| Loops | Range-for, `<algorithm>` and `<ranges>`. `wl_list_for_each` only on lists wlroots owns |
| Errors | `bool`, `std::optional`, `std::expected`; reported with `wlr_log`, the log wlroots writes to |

### Names

| Thing | Form |
|---|---|
| Types, concepts | `PascalCase` |
| Functions, variables, members, enumerators, namespaces, files | `snake_case` |
| Private data members | `snake_case_` |
| The wlroots object a class wraps | `wlr` |
| Tunables | `config::snake_case` |

### Hot paths

Rendering, pointer motion, axis and gesture updates allocate nothing,
format no text and create no texture unless what the texture shows
changed; they call no `std::function`.

### Comments

The `code-style` rules hold: sparse one-liners stating behaviour and the
non-obvious reason, never history.

## Error states, corner cases

- **A destroy handler destroys `this`** when it calls `destroy_owned`:
  that call is its last statement.
- **A loop whose body destroys elements of the container it walks**
  (an output unplug destroying its layer surfaces) walks a copy of the
  pointers.
- **A wlroots object that dies before ours** (a decoration, a drag
  icon): our listeners on it disconnect in its destroy handler.
- **wlroots headers that are not C++** (`[static N]` parameters, a field
  named `namespace`) are handled once in `src/core/wlr.h` and never patched.
- **The wlroots subproject and other bundled code** keep their own style.

## Cross-links

- [wlroots.md](wlroots.md) - the listener-balance rule `Listener`
  enforces.
- [../workspace/build-and-run.md](../workspace/build-and-run.md) - the
  toolchain and the build.
- The `code-style` skill.
