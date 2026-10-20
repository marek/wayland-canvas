// What the scenarios' test clients share: the report line on stdout, an
// anonymous shared-memory file, and a solid-colour wl_shm buffer.
// Needs _GNU_SOURCE (memfd_create): each client defines it first.
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

// One line of the client's report, flushed so the scenario reading the pipe
// sees it at once.
__attribute__((format(printf, 1, 2)))
static inline void say(const char *format, ...) {
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}

// An anonymous shared-memory file of `size` bytes, mapped read-write at
// *mapped. Returns its descriptor, or -1.
static inline int shm_file(size_t size, void **mapped) {
	int fd = memfd_create("test-client", MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return -1;
	}
	*mapped = data;
	return fd;
}

// One ARGB8888 buffer of width x height filled with `colour`, or NULL.
static inline struct wl_buffer *solid_buffer(struct wl_shm *shm, int width,
		int height, uint32_t colour) {
	size_t stride = (size_t)width * 4;
	size_t size = stride * (size_t)height;
	void *mapped;
	int fd = shm_file(size, &mapped);
	if (fd < 0) {
		return NULL;
	}
	uint32_t *pixels = mapped;
	for (size_t i = 0; i < size / 4; i++) {
		pixels[i] = colour;
	}
	munmap(mapped, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		(int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static inline void release_buffer(void *data, struct wl_buffer *buffer) {
	wl_buffer_destroy(buffer);
}

// A solid_buffer attached to `surface` and committed; the buffer goes when
// the compositor releases it. Returns 0, or -1 when no buffer could be made.
static inline int draw_solid(struct wl_shm *shm, struct wl_surface *surface,
		int width, int height, uint32_t colour) {
	static const struct wl_buffer_listener listener = {
		.release = release_buffer,
	};
	struct wl_buffer *buffer = solid_buffer(shm, width, height, colour);
	if (buffer == NULL) {
		return -1;
	}
	wl_buffer_add_listener(buffer, &listener, NULL);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	return 0;
}
