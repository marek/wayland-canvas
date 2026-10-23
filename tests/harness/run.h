#pragma once

// A scenario's life: begin() prepares its scratch space and environment,
// run() starts the real compositor on the headless backend, drives the
// scenario's steps (steps.h) on the compositor's own event loop and tears
// everything down. main is
//
//     if (!harness::begin("name", {"WAYLAND_CANVAS_BIN"})) return 2;
//     ... environment the scenario needs ...
//     return harness::run(options, steps);

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/server.h"
#include "harness/steps.h"

class Output;
class View;

namespace harness {

// The compositor under test: set while run() drives the steps, empty
// otherwise.
extern std::optional<Server> server;

// Makes the scenario's scratch directory, points the compositor at the
// headless backend and its config at <scratch>/xdg-config. False, explained
// on stderr, when a variable of `required_env` (the paths meson passes to
// the scenarios) is not set.
bool begin(std::string_view scenario, std::initializer_list<const char *> required_env);

// Points the compositor at the headless backend: what begin() does besides
// the scratch space, for a run that makes its own.
void use_headless_backend();

// Points the environment variable `name` at <scratch>/<relative>, created,
// and returns that path.
std::string scratch_env(const char *name, std::string_view relative);

// Puts `dir` first on $PATH: the stand-ins a scenario wants found before the
// machine's own programs.
void put_first_on_path(const std::string &dir);

// Starts the compositor with `options`, runs `steps` until one finishes the run or the
// compositor stops, and returns 0 when every check passed and the run reached its end, 1 when
// not, 2 when the compositor did not start. `teardown` runs once the steps are over, with the
// compositor still up.
int run(const Options &options, std::vector<Step> steps, std::function<void()> teardown = {});

// Adds a headless output of `width` x `height` physical pixels, the way a
// monitor is plugged in; null when the backend is not headless.
wlr_output *add_output(int width, int height);

// The first output.
Output &output();

// True once the output's camera has stopped easing and the desktop outlines
// it fades in while panning are gone: what is read off the screen is stable
// then.
bool settled(const Output &output);
// ... on every output.
bool settled();

// The first view that is mapped.
View *first_mapped_view();

} // namespace harness
