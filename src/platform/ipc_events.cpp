#include "platform/ipc_events.h"

#include <memory>

#include "core/changes.h"
#include "core/json.h"
#include "core/server.h"
#include "core/text.h"
#include "platform/ipc_server.h"
#include "platform/output.h"

IpcEvents::IpcEvents(Server &server, IpcServer &ipc) : server_(server), ipc_(ipc) {
}

void IpcEvents::flush() {
	Changes &changes = server_.changes;
	bool outputs = changes.take(Change::outputs);
	bool desktops = changes.take(Change::desktops);
	bool overview = changes.take(Change::overview);
	bool windows = changes.take(Change::windows);
	bool theme = changes.take(Change::theme);
	bool settings = changes.take(Change::settings);
	bool recording = changes.take(Change::recording);
	if (!ipc_.has_subscriber()) {
		for (const std::unique_ptr<Output> &output : server_.outputs) {
			output->overview.take_changed();
		}
		windows_told_ = false;
		return;
	}
	if (outputs) {
		ipc_.push_event("outputs", object_text([&](JsonBuilder *builder) {
			add_outputs_array(builder, server_);
		}));
	}
	if (desktops) {
		ipc_.push_event("desktops", object_text([&](JsonBuilder *builder) {
			add_desktops_array(builder, server_);
		}));
	}
	if (overview) {
		flush_overview();
	}
	// A desktop created, moved, combined or abandoned changes which desktop
	// the windows report.
	if (windows || desktops) {
		flush_windows();
	}
	if (theme) {
		std::string mode = std::string{theme_name(server_.theme.mode())};
		ipc_.push_event("theme", object_text([&](JsonBuilder *builder) {
			json_builder_set_member_name(builder, "mode");
			json_builder_add_string_value(builder, mode.c_str());
		}));
	}
	if (settings) {
		ipc_.push_event("settings", object_text([&](JsonBuilder *builder) {
			add_settings(builder, server_);
		}));
	}
	if (recording) {
		flush_recording();
	}
}

void IpcEvents::flush_windows() {
	collect_windows(server_, window_rows_);
	if (windows_told_ && window_rows_ == told_rows_) {
		return;
	}
	windows_told_ = true;
	told_rows_.swap(window_rows_);
	ipc_.push_event("windows", object_text([&](JsonBuilder *builder) {
		add_windows_array(builder, told_rows_);
	}));
}

void IpcEvents::flush_overview() {
	for (const std::unique_ptr<Output> &output : server_.outputs) {
		if (!output->overview.take_changed()) {
			continue;
		}
		std::string name = output->wlr->name;
		bool active = output->overview.active();
		ipc_.push_event("overview", object_text([&](JsonBuilder *builder) {
			json_builder_set_member_name(builder, "output");
			json_builder_add_string_value(builder, name.c_str());
			json_builder_set_member_name(builder, "active");
			json_builder_add_boolean_value(builder, active);
		}));
	}
}

void IpcEvents::flush_recording() {
	bool active = server_.capture_ui.recording();
	if (active) {
		recording_path_ = server_.capture_ui.recording_path();
	}
	ipc_.push_event("capture.recording", object_text([&](JsonBuilder *builder) {
		json_builder_set_member_name(builder, "active");
		json_builder_add_boolean_value(builder, active);
		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, recording_path_.c_str());
	}));
	if (!active) {
		recording_path_.clear();
	}
}

void IpcEvents::emit_action(std::string_view name, std::string_view args_json) {
	// Spliced by hand: a builder cannot hold a bare object as a value.
	std::string params = "{\"name\":";
	params += text::json_quote(name);
	params += ",\"args\":";
	params += args_json;
	params += "}";
	ipc_.push_event("action", params);
}
