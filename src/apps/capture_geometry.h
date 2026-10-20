#pragma once

#include "core/wlr.h"

class Output;
class View;

// The view's frame as an output-local logical box, whatever the zoom.
wlr_box frame_on_output(const Output &output, const View &view);
