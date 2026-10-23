#pragma once

struct Options {
	const char *startup_cmd = nullptr;
	bool start_bar = true; // autostart the top bar when qs is available
	double scale = 0; // 0: pick per output
};
