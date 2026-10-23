#pragma once

// Fake input devices. Each is announced on the headless backend's new_input,
// the path libinput's devices take, so what a scenario sends goes through
// the compositor's own device handling. Coordinates are output-local logical
// pixels on the headless backend's one 1280 x 720 output.

#include <cstdint>
#include <linux/input-event-codes.h>
#include <optional>
#include <span>
#include "core/geometry.h"
#include "core/wlr.h"

namespace harness {

inline constexpr int screen_w = 1280, screen_h = 720;

// Attaches the device; run() takes it away again.
void attach_keyboard();
void attach_pointer();
void attach_touchscreen();
void attach_tablet();

wlr_touch &touchscreen();

// The time stamped on fake events, in ms: each event moves it on by 8 ms and
// the driver by a second per step.
uint32_t event_time();
void advance_clock(uint32_t ms);

// Takes the devices away: called by run().
void detach_devices();

// ------------------------------------------------------------ keyboard

void key(uint32_t code, bool pressed);
void tap(uint32_t code);
// Holds `modifiers` and taps `code`, then lets go in reverse.
void chord(uint32_t code, std::span<const uint32_t> modifiers = {});

namespace mods {
inline constexpr uint32_t shift[] = {KEY_LEFTSHIFT};
inline constexpr uint32_t super[] = {KEY_LEFTMETA};
inline constexpr uint32_t super_shift[] = {KEY_LEFTMETA, KEY_LEFTSHIFT};
inline constexpr uint32_t super_ctrl[] = {KEY_LEFTMETA, KEY_LEFTCTRL};
} // namespace mods

// ------------------------------------------------------------- pointer

void move_to(double x, double y);
void button(uint32_t code, bool pressed);
void click(double x, double y, uint32_t code = BTN_LEFT);
// Presses at (x0, y0), moves to (x1, y1) in ten steps and lets go.
void drag(double x0, double y0, double x1, double y1);
// A touchpad swipe through the entry points libinput's events reach.
void swipe(int fingers, double dx, double dy);

// --------------------------------------------------------- touchscreen

void finger_down(int32_t id, double x, double y);
void touch_frame();
// Fingers moving together share one frame, as libinput sends them.
void finger_move(int32_t id, double x, double y, bool frame = true);
void finger_up(int32_t id);
// Fingers starting at `from`, moved together by (dx, dy), then lifted.
void fingers_drag(std::span<const Point> from, double dx, double dy, int steps = 10);

// ----------------------------------------------------------------- pen

void pen_proximity(double x, double y, bool in);
void pen_move(double x, double y, std::optional<double> pressure = std::nullopt,
	std::optional<Point> tilt = std::nullopt);
void pen_tip(double x, double y, bool down);
void pen_button(uint32_t code, bool pressed);

} // namespace harness
