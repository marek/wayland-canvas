#include "windows/foreign_toplevel.h"

#include <cstring>
#include "core/server.h"
#include "platform/output.h"
#include "windows/view.h"

namespace {

// Whether a handle holding `held` needs telling `wanted`: wlroots resends a
// title or an app id however often it is set.
bool differs(const char *held, const char *wanted) {
	return wanted != nullptr && (held == nullptr || std::strcmp(held, wanted) != 0);
}

} // namespace

ForeignToplevel::ForeignToplevel(View &view)
	: view_(view) {}

ForeignToplevel::~ForeignToplevel() = default;

void ForeignToplevel::open() {
	wlr_foreign_toplevel_handle_v1 *handle =
		wlr_foreign_toplevel_handle_v1_create(view_.server.foreign_mgr);
	if (handle != nullptr) {
		handle_.reset(handle);
		handle->data = &view_;
		activate_.connect<&ForeignToplevel::handle_activate>(
			handle->events.request_activate, this);
		maximize_.connect<&ForeignToplevel::handle_maximize>(
			handle->events.request_maximize, this);
		minimize_.connect<&ForeignToplevel::handle_minimize>(
			handle->events.request_minimize, this);
		fullscreen_.connect<&ForeignToplevel::handle_fullscreen>(
			handle->events.request_fullscreen, this);
		close_.connect<&ForeignToplevel::handle_close>(
			handle->events.request_close, this);
	}
	// The ext handle names the window for capture clients (image_capture.cpp
	// reads the view back through ->data).
	if (view_.server.image_capture.list != nullptr) {
		wlr_ext_foreign_toplevel_handle_v1_state state{
			.title = view_.shell_title(), .app_id = view_.shell_app_id()};
		wlr_ext_foreign_toplevel_handle_v1 *ext =
			wlr_ext_foreign_toplevel_handle_v1_create(view_.server.image_capture.list, &state);
		if (ext != nullptr) {
			ext_handle_.reset(ext);
			ext->data = &view_;
		}
	}
	publish();
}

void ForeignToplevel::close() {
	activate_.disconnect();
	maximize_.disconnect();
	minimize_.disconnect();
	fullscreen_.disconnect();
	close_.disconnect();
	ext_handle_.reset();
	handle_.reset();
}

void ForeignToplevel::publish() {
	const char *title = view_.shell_title();
	const char *app_id = view_.shell_app_id();
	if (handle_ != nullptr) {
		wlr_foreign_toplevel_handle_v1 *handle = handle_.get();
		if (differs(handle->title, title)) {
			wlr_foreign_toplevel_handle_v1_set_title(handle, title);
		}
		if (differs(handle->app_id, app_id)) {
			wlr_foreign_toplevel_handle_v1_set_app_id(handle, app_id);
		}
		wlr_foreign_toplevel_handle_v1_set_maximized(handle,
			view_.mode == WindowMode::maximized);
		wlr_foreign_toplevel_handle_v1_set_minimized(handle, view_.minimized);
		wlr_foreign_toplevel_handle_v1_set_fullscreen(handle,
			view_.mode == WindowMode::fullscreen);
		wlr_foreign_toplevel_handle_v1_set_activated(handle,
			view_.server.focused_view == &view_);
	}
	if (ext_handle_ != nullptr) {
		wlr_ext_foreign_toplevel_handle_v1_state state{.title = title, .app_id = app_id};
		wlr_ext_foreign_toplevel_handle_v1_update_state(ext_handle_.get(), &state);
	}
}

void ForeignToplevel::output_enter(Output &output) {
	if (handle_ != nullptr) {
		wlr_foreign_toplevel_handle_v1_output_enter(handle_.get(), output.wlr);
	}
}

void ForeignToplevel::output_leave(Output &output) {
	if (handle_ != nullptr) {
		wlr_foreign_toplevel_handle_v1_output_leave(handle_.get(), output.wlr);
	}
}

const char *ForeignToplevel::identifier() const {
	return ext_handle_ != nullptr ? ext_handle_->identifier : nullptr;
}

void ForeignToplevel::handle_activate() {
	view_.activate();
}

void ForeignToplevel::handle_maximize(
		const wlr_foreign_toplevel_handle_v1_maximized_event *event) {
	view_.set_maximized(event->maximized);
}

void ForeignToplevel::handle_minimize(
		const wlr_foreign_toplevel_handle_v1_minimized_event *event) {
	view_.set_minimized(event->minimized);
}

void ForeignToplevel::handle_fullscreen(
		const wlr_foreign_toplevel_handle_v1_fullscreen_event *event) {
	view_.set_fullscreen(event->fullscreen);
}

void ForeignToplevel::handle_close() {
	view_.close();
}
