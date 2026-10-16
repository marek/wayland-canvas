# Glossary

Terms as this project uses them. Where a word has a general meaning
and a narrower one here, the narrower one is given.

| Term | Meaning here |
|---|---|
| **wayland-canvas** | This compositor: an infinite canvas plus virtual desktops ([overview.md](overview.md)) |
| **Canvas** | The single unbounded plane on which windows live at canvas coordinates ([canvas/infinite-canvas.md](canvas/infinite-canvas.md)) |
| **Canvas unit** | One logical pixel at zoom 1; the measure of canvas coordinates, independent of any output's scale |
| **Camera** | Per-output state — a centre on the canvas plus a zoom — saying what an output is looking at ([canvas/infinite-canvas.md](canvas/infinite-canvas.md)) |
| **Viewport** | The screen-sized rectangle of canvas an output currently shows |
| **1:1** | Normal mode: zoom exactly 1, camera on the pixel grid, clients receive input |
| **Zoomed-out** | Any view at a zoom below 1, from the first dip of a 4-finger swipe to the far end of the overview |
| **Overview** | The zoomed-out, compositor-owned view of the canvas where desktops are arranged ([canvas/overview.md](canvas/overview.md)) |
| **Target** | The desktop or free spot the overview will zoom into, drawn as an accent outline ([canvas/overview.md](canvas/overview.md)) |
| **Free spot, free area** | A screen-sized spot on the canvas that is not a desktop; first-class — you can pan, land and work there |
| **Desktop** | A screen-sized dashed frame on the canvas, named "Desktop N" by id ([canvas/desktops.md](canvas/desktops.md)) |
| **Frame** | A desktop's dashed rectangle. A *window frame* is a window's content plus its title bar and border — a different thing ([windows/title-bars.md](windows/title-bars.md)) |
| **Tab** | A desktop's label in the overview, carrying its id and the Tile and Abandon buttons; dragging it moves the frame ([canvas/overview.md](canvas/overview.md)) |
| **Hop** | The 3-finger-swipe move, the same in the normal view and the overview: to the closest desktop in the swipe's direction, or one screen that way when no desktop lies there; one hop per swipe ([canvas/infinite-canvas.md](canvas/infinite-canvas.md), [canvas/overview.md](canvas/overview.md)) |
| **Abandon** | Remove a desktop; its windows stay on the canvas ([canvas/desktops.md](canvas/desktops.md)) |
| **Combine** | Move another desktop's windows into this one, keeping their relative layout, and remove that desktop ([canvas/desktops.md](canvas/desktops.md)) |
| **Tile** | Grid-tile the windows whose centre lies in a desktop or target spot, inside the usable area, with gaps ([canvas/desktops.md](canvas/desktops.md)) |
| **Shelf** | The strip along the bottom of the overview holding minimized windows; clicking one restores it and goes there ([canvas/overview.md](canvas/overview.md)) |
| **Hint** | A transient message the overview shows briefly, such as "Tiled 2 windows" |
| **SSD** | Server-side decorations: the compositor draws the title bar and border ([windows/title-bars.md](windows/title-bars.md)) |
| **CSD** | Client-side decorations: the client draws its own |
| **Title bar** | The SSD bar: title text on the left, caption buttons on the right ([windows/title-bars.md](windows/title-bars.md)) |
| **Caption buttons** | Minimize, maximize/restore, close, in the title bar |
| **Launcher** | The Start-style app panel opened by a tap of Super or Super+Space ([apps/launcher.md](apps/launcher.md)) |
| **Desktop entry** | An XDG `.desktop` file; the standard unit of the app list ([apps/desktop-entries.md](apps/desktop-entries.md)) |
| **Layer surface** | A layer-shell client surface: a bar, launcher, wallpaper or notification ([platform/protocols.md](platform/protocols.md)) |
| **Exclusive zone** | The screen area a layer surface reserves; the rest of the screen is the usable area |
| **Usable area** | A screen minus its layer surfaces' exclusive zones; maximized and tiled windows and new windows keep inside it ([windows/window-management.md](windows/window-management.md)) |
| **Output** | A display, with its own camera, scale and mode ([platform/outputs.md](platform/outputs.md)) |
| **Layout** | The arrangement of outputs in global coordinates; pointer position is measured in them ([platform/outputs.md](platform/outputs.md)) |
| **Headless backend** | The wlroots backend with no display hardware: renders in memory; the scenario venue ([workspace/build-and-run.md](workspace/build-and-run.md)) |
| **Scenario** | A headless test: the real compositor on the headless backend with real clients, driven through fake devices, judged from screenshots and logs |
| **Nested** | Running wayland-canvas as a client of another compositor ([workspace/build-and-run.md](workspace/build-and-run.md)) |
| **TTY** | A Linux virtual terminal; running from a spare TTY gives the compositor the real display |
| **wlroots** | The Wayland compositor library this is built on, and the reference project ([platform/wlroots.md](platform/wlroots.md)) |
| **Intent, plan, skill, role** | The contract; a volatile checklist; how to work; who is working ([README.md](README.md), `AGENTS.md`) |
| **Strand** | One of intent, tests, implementation |
