#include "platform/night_light.h"

#include "core/server.h"
#include "core/settings.h"
#include "platform/night_light_ramp.h"
#include "platform/output.h"

namespace {

using ColorTransform = Handle<wlr_color_transform, wlr_color_transform_unref>;

// The DRM backend takes only 3x1D LUTs, so the filter is one.
ColorTransform built_in_filter(int kelvin, std::size_t ramp) {
	std::vector<uint16_t> table = night_light_table(kelvin, ramp);
	return ColorTransform{wlr_color_transform_init_lut_3x1d(
		ramp, table.data(), table.data() + ramp, table.data() + 2 * ramp)};
}

// Whether `output` takes `table`, a client's: it is made for the output's
// ramp, and the backend accepts it.
bool holds_table(wlr_output *output, const wlr_gamma_control_v1 &control,
		wlr_color_transform *table) {
	if (table == nullptr || control.ramp_size != wlr_output_get_gamma_size(output)) {
		return false;
	}
	if (!output->enabled) {
		return true; // a disabled output tests no state: the commit that enables it tells
	}
	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_color_transform(&state, table);
	bool held = wlr_output_test_state(output, &state);
	wlr_output_state_finish(&state);
	return held;
}

} // namespace

// What one output shows: the client's table while it holds one, else the
// built-in filter, else nothing. Lives until its output goes.
struct NightLight::Slot {
	Slot(NightLight &owner, wlr_output *output) : owner(owner), output(output) {
		destroy.connect<&Slot::handle_destroy>(output->events.destroy, this);
	}

	void handle_destroy() {
		destroy_owned(owner.slots_, this);
	}

	NightLight &owner;
	wlr_output *output;
	ColorTransform client;
	std::size_t client_ramp = 0; // the ramp the client's table was made for
	ColorTransform filter; // none while the night light is off
	int filter_k = 0;
	std::size_t filter_ramp = 0;
	Listener destroy;
};

NightLight::NightLight(Server &server) : server_(server) {}

NightLight::~NightLight() {
	finish();
}

void NightLight::init() {
	manager_ = wlr_gamma_control_manager_v1_create(server_.display);
	if (manager_ == nullptr) {
		wlr_log(WLR_ERROR, "failed to create the gamma control manager");
		return;
	}
	set_gamma_.connect<&NightLight::handle_set_gamma>(manager_->events.set_gamma, this);
}

void NightLight::finish() {
	set_gamma_.disconnect();
	slots_.clear();
}

void NightLight::handle_set_gamma(wlr_gamma_control_manager_v1_set_gamma_event *event) {
	wlr_output *output = event->output;
	if (output == nullptr) {
		return;
	}
	// A client's table wins on its output while it holds it; the built-in
	// filter steps aside there and comes back when the client lets go (a
	// null control).
	if (event->control == nullptr) {
		if (Slot *slot = find_slot(output)) {
			slot->client.reset();
		}
	} else {
		ColorTransform table{wlr_gamma_control_v1_get_color_transform(event->control)};
		if (!holds_table(output, *event->control, table.get())) {
			// Its destruction announces the client's leaving to this handler
			// once more, which damages the output.
			wlr_gamma_control_v1_send_failed_and_destroy(event->control);
			return;
		}
		Slot &slot = slot_for(output);
		slot.client = std::move(table);
		slot.client_ramp = event->control->ramp_size;
	}
	// A control also goes with its output, after the Output: only an Output
	// still listed is touched.
	for (const std::unique_ptr<Output> &each : server_.outputs) {
		if (each->wlr == output) {
			each->damage();
			break;
		}
	}
}

NightLight::Slot *NightLight::find_slot(wlr_output *output) {
	for (const std::unique_ptr<Slot> &slot : slots_) {
		if (slot->output == output) {
			return slot.get();
		}
	}
	return nullptr;
}

NightLight::Slot &NightLight::slot_for(wlr_output *output) {
	if (Slot *slot = find_slot(output)) {
		return *slot;
	}
	slots_.push_back(std::make_unique<Slot>(*this, output));
	return *slots_.back();
}

void NightLight::apply_to_output(Output &output, wlr_output_state &state) {
	std::size_t ramp = wlr_output_get_gamma_size(output.wlr);
	if (ramp == 0) {
		return;
	}
	Slot &slot = slot_for(output.wlr);
	if (slot.client != nullptr && slot.client_ramp != ramp) {
		// The output's ramp changed under the client's table: it is told,
		// and its leaving clears the table.
		wlr_gamma_control_v1 *control =
			wlr_gamma_control_manager_v1_get_control(manager_, output.wlr);
		if (control != nullptr) {
			wlr_gamma_control_v1_send_failed_and_destroy(control);
		}
		slot.client.reset();
	}
	const settings::Resolved &conf = server_.conf.resolved;
	int wanted_k = conf.night_light ? conf.night_light_k : 0;
	if (wanted_k != slot.filter_k || ramp != slot.filter_ramp) {
		slot.filter = wanted_k != 0 ? built_in_filter(wanted_k, ramp) : ColorTransform{};
		slot.filter_k = wanted_k;
		slot.filter_ramp = ramp;
	}
	// wlroots drops a transform the output already shows, so handing it the
	// same one every frame commits nothing.
	wlr_output_state_set_color_transform(&state,
		slot.client != nullptr ? slot.client.get() : slot.filter.get());
}

bool NightLight::holds_a_ramp(const Output &output) {
	return wlr_output_get_gamma_size(output.wlr) > 0;
}

void NightLight::settings_changed() {
	server_.outputs.damage_all();
}
