#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "core/util.h"

class Input;
class Server;

// The touchscreens (docs/intents/input/touch-and-pen.md). A finger on a
// client's surface reaches that client as touch and stays with the surface
// until it lifts. On the compositor's own UI one finger is the pointer's
// left button, and held still a right click. Two fingers on the canvas move
// the view as Super does on the touchpad; three or four run the touchpad's
// swipes, by the same preset.
class Touch {
public:
	Touch(Server &server, Input &input);
	~Touch();
	Touch(const Touch &) = delete;
	Touch &operator=(const Touch &) = delete;

	void init(); // listens to the cursor's touch events
	void finish(); // before the cursor and the seat go
	void new_device(wlr_input_device *device);
	// Each touchscreen to the output it is built into, again: outputs came
	// or went.
	void map_devices();
	bool connected() const { return !devices_.empty(); }
	// Every contact ends: what reached clients is cancelled, a press is
	// released, a gesture ends cancelled. Fingers still down are ignored
	// until they lift.
	void end_all(uint32_t time);

private:
	// A touchscreen, gone with its device.
	class Device {
	public:
		Device(Touch &touch, wlr_input_device *device);
		Device(const Device &) = delete;
		Device &operator=(const Device &) = delete;
		wlr_input_device *const wlr;

	private:
		void handle_destroy();
		Touch &touch_;
		Listener destroy_;
	};

	// What one finger is: a client's, the pointer's on the compositor's UI,
	// one of a gesture moving the view, or nothing's (it landed where
	// nothing takes it, or its gesture ended before it lifted).
	enum class Role { client, pointer, view, ignored };
	struct Contact {
		int32_t id = 0;
		Role role = Role::ignored;
		double sx = 0, sy = 0, scale = 1; // a client's: where on its surface, its px per layout px
		double lx0 = 0, ly0 = 0; // where it landed, layout px
		double lx = 0, ly = 0; // where it is
	};
	// The finger that is the pointer: waiting to be told a tap, a drag or
	// a hold; pressed, dragging; or spent by a hold, which right-clicked.
	enum class Press { none, waiting, pressed, held };
	// Two fingers on the canvas: not yet told a look away from a pinch,
	// looking away, or pinching.
	enum class Two { none, undecided, looking, pinching };

	void handle_down(wlr_touch_down_event *event);
	void handle_up(wlr_touch_up_event *event);
	void handle_motion(wlr_touch_motion_event *event);
	void handle_cancel(wlr_touch_cancel_event *event);
	void handle_frame();
	void device_gone(Device *device);

	Contact *find(int32_t id);
	// Whether a landing finger joins a gesture: a swipe going on, the third
	// or fourth finger, or the second on the canvas beside the first.
	bool join_gesture(Contact &contact, std::size_t fingers, uint32_t time);
	void place(wlr_touch *touch, double x, double y, double &lx, double &ly) const;
	// Three fingers, or four: the touchpad's swipe, and what reached
	// clients is cancelled for them.
	void begin_swipe(uint32_t time, uint32_t fingers);
	void update_swipe(uint32_t time);
	void update_two(uint32_t time);
	// The fingers moving the view moved since the last frame: the gesture
	// reads them together, once all of them have.
	void apply_moves();
	void cancel_clients();
	void end_press(uint32_t time);
	void end_two(uint32_t time, bool cancelled);
	void arm_hold();
	static int handle_hold_timer(void *data);
	void held();
	// The centroid of the contacts that move the view.
	bool centroid(double &cx, double &cy) const;

	Server &server_;
	Input &input_;
	std::vector<std::unique_ptr<Device>> devices_;
	std::vector<Contact> contacts_;
	Press press_ = Press::none;
	EventSourcePtr hold_timer_;
	Two two_ = Two::none;
	double two_cx0_ = 0, two_cy0_ = 0, two_cx_ = 0, two_cy_ = 0, two_dist0_ = 0;
	bool swiping_ = false;
	uint32_t swipe_fingers_ = 0;
	double swipe_cx_ = 0, swipe_cy_ = 0, swipe_travel_ = 0;
	// Fingers moving the view moved since the last frame, the latest at moved_time_.
	bool moved_ = false;
	uint32_t moved_time_ = 0;
	Listener down_, up_, motion_, cancel_, frame_;
};
