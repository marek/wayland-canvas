#include "platform/image_capture.h"

#include <algorithm>
#include <cstring>
#include <drm_fourcc.h>

#include "core/server.h"
#include "platform/output.h"
#include "render/capture.h"
#include "windows/view.h"

// Window capture sources: ext-image-copy-capture over a view's offscreen
// render. Every frame request is answered — a fresh render, never a hang.

struct ImageCapture::ToplevelSource {
	wlr_ext_image_capture_source_v1 base{}; // first: the protocol object

	View *view = nullptr;
	ImageCapture *owner = nullptr;
	// The buffer the next frame is rendered into; it owns its reference.
	Handle<wlr_buffer, wlr_buffer_drop> buffer{};
	timespec when{}; // the stamp given to the last pushed frame

	Listener commit_, unmap_, destroy_;

	bool started = false;
	// The last render failed (no buffer of that size, or the pass): its
	// frame is still announced, and copying it fails the client's frame.
	bool failed = false;

	~ToplevelSource() {
		wlr_ext_image_capture_source_v1_finish(&base); // frees the formats too
	}

	void handle_commit();

	void handle_unmap() { detach(); }
	void handle_destroy() { detach(); }
	void detach() { destroy_owned(owner->sources_, this); }
};
// The source's owner: our ToplevelSource keeps the protocol object as its
// first member (what wl_container_of would compute, without its offsetof).
static ImageCapture::ToplevelSource *source_of(wlr_ext_image_capture_source_v1 *source) {
	return reinterpret_cast<ImageCapture::ToplevelSource *>(source);
}

// The formats a client may copy into; malloc'd because
// wlr_ext_image_capture_source_v1_finish frees the array.
static uint32_t *source_formats() {
	static const uint32_t formats[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};
	auto *copy = static_cast<uint32_t *>(std::malloc(sizeof formats));
	if (copy != nullptr) {
		std::memcpy(copy, formats, sizeof formats);
	}
	return copy;
}

// Renders the window at its told scale and pushes the frame to the sessions.
// The one entry point that produces frames: called for a frame request, and
// again whenever the window commits while a session is started. A frame that
// cannot be rendered is still announced, so every request is answered.
static void source_emit_frame(ImageCapture::ToplevelSource *s) {
	if (!s->view->mapped) {
		return;
	}
	Server &server = s->view->server;
	float scale = s->view->told_scale > 0 ? static_cast<float>(s->view->told_scale) : 1.0f;
	// The device-pixel frame, rounded exactly as the render draws it.
	wlr_box size = view_device_size(*s->view, scale);
	int width = std::max(1, size.width);
	int height = std::max(1, size.height);
	if (s->base.width != static_cast<uint32_t>(width)
			|| s->base.height != static_cast<uint32_t>(height)) {
		s->base.width = width;
		s->base.height = height;
		wl_signal_emit_mutable(&s->base.events.constraints_update, nullptr);
	}
	if (s->buffer == nullptr || s->buffer->width != width || s->buffer->height != height) {
		s->buffer.reset(make_capture_buffer(server, width, height));
	}
	s->failed = !render_view_offscreen(*s->view, scale, s->buffer.get());
	// A whole new buffer: full damage, every time.
	pixman_region32_t damage{};
	pixman_region32_init_rect(&damage, 0, 0, width, height);
	wlr_ext_image_capture_source_v1_frame_event event{.damage = &damage};
	clock_gettime(CLOCK_MONOTONIC, &s->when);
	wl_signal_emit_mutable(&s->base.events.frame, &event);
	pixman_region32_fini(&damage);
}

static void source_start(wlr_ext_image_capture_source_v1 *source, bool with_cursors) {
	auto *s = source_of(source);
	s->started = true;
}

static void source_stop(wlr_ext_image_capture_source_v1 *source) {
	auto *s = source_of(source);
	s->started = false;
}

static void source_request_frame(wlr_ext_image_capture_source_v1 *source, bool schedule_frame) {
	// Answered now, not on some next commit: a frame request is always
	// answered.
	auto *s = source_of(source);
	source_emit_frame(s);
}

static void source_copy_frame(wlr_ext_image_capture_source_v1 *source,
		wlr_ext_image_copy_capture_frame_v1 *frame,
		wlr_ext_image_capture_source_v1_frame_event *event) {
	auto *s = source_of(source);
	if (s->failed) {
		wlr_ext_image_copy_capture_frame_v1_fail(frame,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return;
	}
	// A client buffer that does not fit fails the frame inside copy_buffer,
	// which destroys it: ready only follows a copy that worked.
	if (!wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, s->buffer.get(),
			s->view->server.renderer)) {
		return;
	}
	wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &s->when);
}

static const wlr_ext_image_capture_source_v1_interface SOURCE_IMPL = {
	.start = source_start,
	.stop = source_stop,
	.request_frame = source_request_frame,
	.copy_frame = source_copy_frame,
};

void ImageCapture::ToplevelSource::handle_commit() {
	if (started) {
		source_emit_frame(this);
	}
}

ImageCapture::ImageCapture(Server &server)
	: server_(server) {
}

ImageCapture::~ImageCapture() {
	finish();
}

void ImageCapture::init() {
	wl_display *display = server_.display;
	list = wlr_ext_foreign_toplevel_list_v1_create(display, 1);
	copy_mgr_ = wlr_ext_image_copy_capture_manager_v1_create(display, 1);
	// The output sources are wlroots' own: fed by the outputs' committed
	// frames, which the renderer already sends (a damaged commit makes
	// wlroots' output source ask for a frame).
	output_mgr_ = wlr_ext_output_image_capture_source_manager_v1_create(display, 1);
	toplevel_mgr_ = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(display, 1);
	new_request_.connect<&ImageCapture::handle_new_request>(
		toplevel_mgr_->events.new_request, this);
}

void ImageCapture::finish() {
	new_request_.disconnect();
	sources_.clear(); // each finish()es its protocol object first
}

void ImageCapture::handle_new_request(
		wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request) {
	View *view = static_cast<View *>(request->toplevel_handle->data);
	// The handle is destroyed on unmap, so wlroots already answers requests
	// for a gone window with a dead source; here the window is mapped.
	if (view == nullptr || !view->mapped) {
		return;
	}
	auto source = std::make_unique<ToplevelSource>();
	source->view = view;
	source->owner = this;
	// init() zeroes the protocol object, so the formats and the size —
	// the first constraints a session sees — are set after it.
	wlr_ext_image_capture_source_v1_init(&source->base, &SOURCE_IMPL);
	source->base.shm_formats = source_formats();
	source->base.shm_formats_len = source->base.shm_formats != nullptr ? 2 : 0;
	float scale = view->told_scale > 0 ? static_cast<float>(view->told_scale) : 1.0f;
	wlr_box size = view_device_size(*view, scale);
	source->base.width = std::max(1, size.width);
	source->base.height = std::max(1, size.height);
	ToplevelSource *s = source.get();
	s->commit_.connect<&ToplevelSource::handle_commit>(
		view->wlr->base->surface->events.commit, s);
	s->unmap_.connect<&ToplevelSource::handle_unmap>(
		view->wlr->base->surface->events.unmap, s);
	s->destroy_.connect<&ToplevelSource::handle_destroy>(view->wlr->events.destroy, s);
	sources_.push_back(std::move(source));
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, &s->base);
}
