#pragma once

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>
#include <cairo.h>
#include "core/wlr.h"

namespace listener_detail {

template <typename>
struct Handler;

template <typename C>
struct Handler<void (C::*)()> {
	using Owner = C;
	using Data = void;
};

template <typename C, typename T>
struct Handler<void (C::*)(T *)> {
	using Owner = C;
	using Data = T;
};

} // namespace listener_detail

// Calls a member function of its owner when a wl_signal fires. Destroying, reconnecting
// or disconnecting removes it from the signal, so no wlroots object dies with it attached.
class Listener {
public:
	Listener() = default;
	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;
	~Listener() { disconnect(); }

	// Handler: `void (Owner::*)()` or `void (Owner::*)(T *)`, T the signal's data type.
	template <auto Handler, typename Owner>
	void connect(wl_signal &signal, Owner *owner) {
		using Traits = listener_detail::Handler<decltype(Handler)>;
		static_assert(std::is_base_of_v<typename Traits::Owner, Owner>,
			"the handler must be a member function of the owner");
		disconnect();
		owner_ = owner;
		call_ = [](void *o, void *data) {
			auto *self = static_cast<Owner *>(o);
			if constexpr (std::is_void_v<typename Traits::Data>) {
				(self->*Handler)();
			} else {
				(self->*Handler)(static_cast<typename Traits::Data *>(data));
			}
		};
		listener_.notify = notify;
		wl_signal_add(&signal, &listener_);
	}

	void disconnect() {
		if (connected()) {
			wl_list_remove(&listener_.link);
			listener_.link = {};
		}
	}

	bool connected() const { return listener_.link.next != nullptr; }

private:
	// First member of a standard-layout class: a wl_listener * is a Listener *.
	wl_listener listener_{};
	void *owner_ = nullptr;
	void (*call_)(void *owner, void *data) = nullptr;

	static void notify(wl_listener *listener, void *data) {
		auto *self = reinterpret_cast<Listener *>(listener);
		self->call_(self->owner_, data);
	}
};

static_assert(std::is_standard_layout_v<Listener>);

template <auto Destroy>
struct Deleter {
	template <typename T>
	void operator()(T *object) const { Destroy(object); }
};

// A C object released by its destroy function.
template <typename T, auto Destroy>
using Handle = std::unique_ptr<T, Deleter<Destroy>>;

using TexturePtr = Handle<wlr_texture, wlr_texture_destroy>;
using CairoSurfacePtr = Handle<cairo_surface_t, cairo_surface_destroy>;
using CairoPtr = Handle<cairo_t, cairo_destroy>;
using EventSourcePtr = Handle<wl_event_source, wl_event_source_remove>;

// Takes `target` out of the container that owns it, then destroys it: the container is
// consistent again before the destructor runs. Nothing may touch `target` afterwards.
template <typename T>
void destroy_owned(std::vector<std::unique_ptr<T>> &owner, const T *target) {
	auto it = std::ranges::find_if(owner, [target](const std::unique_ptr<T> &p) {
		return p.get() == target;
	});
	if (it == owner.end()) {
		return;
	}
	std::unique_ptr<T> doomed = std::move(*it);
	owner.erase(it);
}
