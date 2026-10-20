// Linked into the allocation steps of the gestures scenario: its malloc
// family stands in for glibc's, and counts the allocations the compositor's
// own code makes - those asked for by C++ (operator new, in libstdc++) and
// by the text and JSON libraries it draws and answers with (glib, pango,
// cairo, pixman, json-glib, harfbuzz, fontconfig, freetype). What wlroots,
// libwayland and the GL driver allocate for their own frame bookkeeping is
// the renderer's, not counted. canvas_allocations() reads the calling
// thread's count. Not built into sanitizer runs, which own malloc.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *ptr, size_t size);
extern void *__libc_memalign(size_t alignment, size_t size);

static __thread unsigned long long allocations;

static const char *const counted_libraries[] = {
	"libstdc++", "libglib-", "libgobject-", "libpango", "libcairo", "libpixman",
	"libjson-glib", "libharfbuzz", "libfontconfig", "libfreetype",
};

// Counts the allocation when the library that asked for it is one of those
// above.
static void count_from(void *caller) {
	Dl_info info;
	if (dladdr(caller, &info) == 0 || info.dli_fname == NULL) {
		return;
	}
	const char *name = strrchr(info.dli_fname, '/');
	name = name != NULL ? name + 1 : info.dli_fname;
	for (size_t i = 0; i < sizeof(counted_libraries) / sizeof(counted_libraries[0]); i++) {
		if (strncmp(name, counted_libraries[i], strlen(counted_libraries[i])) == 0) {
			allocations++;
			return;
		}
	}
}

void *malloc(size_t size) {
	count_from(__builtin_return_address(0));
	return __libc_malloc(size);
}

void *calloc(size_t count, size_t size) {
	count_from(__builtin_return_address(0));
	return __libc_calloc(count, size);
}

void *realloc(void *ptr, size_t size) {
	count_from(__builtin_return_address(0));
	return __libc_realloc(ptr, size);
}

void *memalign(size_t alignment, size_t size) {
	count_from(__builtin_return_address(0));
	return __libc_memalign(alignment, size);
}

void *aligned_alloc(size_t alignment, size_t size) {
	count_from(__builtin_return_address(0));
	return __libc_memalign(alignment, size);
}

int posix_memalign(void **out, size_t alignment, size_t size) {
	count_from(__builtin_return_address(0));
	void *ptr = __libc_memalign(alignment, size);
	if (ptr == NULL) {
		return ENOMEM;
	}
	*out = ptr;
	return 0;
}

unsigned long long canvas_allocations(void) {
	return allocations;
}
