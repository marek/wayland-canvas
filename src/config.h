#ifndef CV_CONFIG_H
#define CV_CONFIG_H

/*
 * Compile-time configuration (dwl-style): edit and rebuild. Keybindings live
 * in input.c next to the actions they call.
 */

/* Server-side window decorations, logical pixels (Windows 11 metrics). */
#define TITLEBAR_HEIGHT 32
#define BUTTON_WIDTH 46
#define BORDER_WIDTH 1
#define RESIZE_MARGIN 8 /* invisible resize grab area outside the frame */
#define TITLE_FONT "Sans 10"
#define TITLE_PADDING 12
#define UI_FONT "Sans 10"
#define UI_FONT_BOLD "Sans Bold 10"

/* Colours, 0xRRGGBBAA. */
#define COLOR_CANVAS 0x14161bff
#define COLOR_GRID 0xffffff0c
#define COLOR_TITLE_ACTIVE 0x202020ff
#define COLOR_TITLE_INACTIVE 0x2b2b2bff
#define COLOR_TEXT_ACTIVE 0xffffffff
#define COLOR_TEXT_INACTIVE 0x8c8c8cff
#define COLOR_BUTTON_HOVER 0xffffff1a
#define COLOR_BUTTON_PRESS 0xffffff0f
#define COLOR_CLOSE_HOVER 0xc42b1cff
#define COLOR_CLOSE_PRESS 0xa52618ff
#define COLOR_BORDER_ACTIVE 0x0078d4ff
#define COLOR_BORDER_INACTIVE 0x3c3c3cff
#define COLOR_FRAME 0xc8ccd4d0 /* virtual desktop dashed outline */
#define COLOR_ACCENT 0x4c9bffff /* overview target */
#define COLOR_COMBINE 0x3ddc84ff
#define COLOR_ABANDON 0xff6b5bff
#define COLOR_DIM 0x000000a0 /* over wallpapers while zoomed out */
#define COLOR_PANEL 0x101216e0 /* overview labels, hints, shelf */
#define COLOR_PANEL_TEXT 0xe8eaedff

/* Panning. Touchpad units are libinput's (roughly pointer pixels). */
#define PAN_SPEED 1.0 /* canvas px per unit at zoom 1 */
#define NATURAL_PAN 1 /* 1: canvas follows the fingers like a map */
#define INERTIA_TAU_MS 325.0 /* momentum decay time constant */
#define INERTIA_MIN_SPEED 0.25 /* px/ms to keep coasting after release */
#define SNAP_RADIUS 0.10 /* landing this close (fraction of screen) to a desktop snaps onto it */
#define SCROLL_PAN_SPEED 1.5 /* 2-finger scroll over empty canvas */

/* Overview (zoomed-out view). */
#define OVERVIEW_SWIPE_DISTANCE 260.0 /* 4-finger travel for a full zoom transition */
#define OVERVIEW_COMMIT 0.25 /* progress past which a released zoom gesture completes */
#define OVERVIEW_MAX_ZOOM 0.30
#define OVERVIEW_MIN_ZOOM 0.02
#define OVERVIEW_MARGIN 80 /* logical px kept around the content */
#define HOP_THRESHOLD 36.0 /* 3-finger travel that triggers a jump to the next desktop */
#define HOP_CONE_DEG 60.0 /* how far off the swipe direction a desktop may lie */
#define ABANDON_DISTANCE 200.0 /* 4-finger sideways travel that abandons a desktop */
#define PINCH_TILE_SCALE 1.30 /* 3-finger spread past this tiles */
#define PINCH_COMBINE_SCALE 0.72 /* 3-finger pinch below this combines */
#define COMBINE_OVERLAP 0.5 /* dropping a frame this far onto another combines them */

#define ANIM_MS 280.0
#define TILE_GAP 8
#define DRAG_THRESHOLD 6.0 /* px before a press becomes a drag */
#define DOUBLE_CLICK_MS 400

/* Input devices. */
#define REPEAT_RATE 25
#define REPEAT_DELAY 600
#define TAP_TO_CLICK 1
#define NATURAL_SCROLL 1
#define DISABLE_WHILE_TYPING 1
#define CURSOR_SIZE 24

/* Commands run through /bin/sh -c. */
#define TERMINAL_CMD "exec ${TERMINAL:-foot}"

/* App launcher: tap the logo key (or Super+Space). */
#define LAUNCHER_WIDTH 600
#define LAUNCHER_SEARCH_HEIGHT 56
#define LAUNCHER_ROW_HEIGHT 44
#define LAUNCHER_MAX_ROWS 10
#define LAUNCHER_ICON 28
#define LAUNCHER_FONT "Sans 11"
#define LAUNCHER_SEARCH_FONT "Sans 13"
#define COLOR_LAUNCHER 0x1f1f1ff5
#define COLOR_LAUNCHER_SEARCH 0x2b2b2bff
#define COLOR_LAUNCHER_SELECTED 0x0078d4ff
#define COLOR_LAUNCHER_HOVER 0xffffff14
#define COLOR_LAUNCHER_TEXT 0xffffffff
#define COLOR_LAUNCHER_DIM_TEXT 0x9a9a9aff
#define COLOR_LAUNCHER_BACKDROP 0x00000060

#endif
