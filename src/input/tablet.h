#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "core/util.h"

class Input;
class Server;

// The pen (docs/intents/input/touch-and-pen.md). Over a client that speaks
// tablet-v2 it reaches the client as a tablet tool: in and out of range,
// motion, pressure, tilt, the tip and the buttons. Over anything else — a
// client without tablet support, the compositor's UI, the canvas — it is
// the mouse: the pointer follows it, the tip is the left button and the
// barrel button the right.
class Tablets {
public:
	Tablets(Server &server, Input &input);
	~Tablets();
	Tablets(const Tablets &) = delete;
	Tablets &operator=(const Tablets &) = delete;

	void init(); // the tablet-v2 global and the cursor's tablet events
	void finish(); // before the cursor and the seat go
	void new_device(wlr_input_device *device);
	// Each tablet to the output it acts on, again: outputs came or went.
	void map_devices();
	// Every tool leaves what it is on, the tip lifting first: the client it
	// is in range of, or the mouse's button.
	void end_all(uint32_t time);

private:
	// A tablet and its tablet-v2 twin, gone with the device.
	class Device {
	public:
		Device(Tablets &tablets, wlr_input_device *device);
		Device(const Device &) = delete;
		Device &operator=(const Device &) = delete;
		wlr_input_device *const wlr;
		wlr_tablet_v2_tablet *v2 = nullptr;

	private:
		void handle_destroy();
		Tablets &tablets_;
		Listener destroy_;
	};

	// A tool that came in range, its tablet-v2 twin and what it is doing,
	// gone with the tool.
	class Tool {
	public:
		Tool(Tablets &tablets, wlr_tablet_tool *wlr);
		Tool(const Tool &) = delete;
		Tool &operator=(const Tool &) = delete;
		wlr_tablet_tool *const wlr;
		wlr_tablet_v2_tablet_tool *v2 = nullptr;
		// The tip is down as the mouse's left button.
		bool tip_as_button = false;
		// Where the tool last was on the client it is in range of: the
		// origin its motion is measured from while the tip holds it there.
		double sx0 = 0, sy0 = 0, lx0 = 0, ly0 = 0, scale = 1;
		double tilt_x = 0, tilt_y = 0;

	private:
		void handle_destroy();
		void handle_set_cursor(wlr_tablet_v2_event_cursor *event);
		Tablets &tablets_;
		Listener destroy_, set_cursor_;
	};

	void handle_axis(wlr_tablet_tool_axis_event *event);
	void handle_proximity(wlr_tablet_tool_proximity_event *event);
	void handle_tip(wlr_tablet_tool_tip_event *event);
	void handle_button(wlr_tablet_tool_button_event *event);
	void device_gone(Device *device);
	void tool_gone(Tool *tool);
	Device *device_for(wlr_tablet *tablet);
	Tool *tool_for(wlr_tablet_tool *tool);
	// The tool where the cursor now is: to the tablet-v2 client under it,
	// else as the mouse.
	void move(Tool &tool, Device &device, uint32_t time);
	// The tool leaves what it is on, the tip lifting first.
	void leave(Tool &tool, uint32_t time);
	void send_axes(Tool &tool, const wlr_tablet_tool_axis_event &event);

	Server &server_;
	Input &input_;
	wlr_tablet_manager_v2 *manager_ = nullptr;
	std::vector<std::unique_ptr<Device>> devices_;
	std::vector<std::unique_ptr<Tool>> tools_;
	Listener axis_, proximity_, tip_, button_;
};
