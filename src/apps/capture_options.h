#pragma once

// Where a shot goes and how it is taken; the toolbar owns one set of these
// as its visible options, the key chords pass one-shot sets.
struct CaptureOptions {
	bool clipboard = false; // copy as image/png only: no file, no thumbnail
	int timer_s = 0; // countdown before the capture or recording starts
	bool show_pointer = false; // the cursor composited into shots
};
