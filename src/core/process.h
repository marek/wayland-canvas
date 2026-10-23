#pragma once

#include <sys/types.h>

// Milliseconds on the monotonic clock.
double now_ms();

// Runs `cmd` through /bin/sh -c, double-forked so it never becomes a zombie.
// Returns the shell's pid (the command's own when the shell execs it), or -1.
pid_t spawn(const char *cmd);

// The compositor ignores SIGPIPE for itself, so a write to a peer that has
// gone fails with EPIPE. What spawn() starts gets the disposition the
// compositor found back, since an ignored signal stays ignored across exec; a
// second Server in the same process would find the first one's ignore
// instead, so only the first call records it.
void ignore_sigpipe();
