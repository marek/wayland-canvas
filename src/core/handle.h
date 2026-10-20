#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

template <auto Destroy>
struct Deleter {
	template <typename T>
	void operator()(T *object) const { Destroy(object); }
};

// A C object released by its destroy function.
template <typename T, auto Destroy>
using Handle = std::unique_ptr<T, Deleter<Destroy>>;

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
