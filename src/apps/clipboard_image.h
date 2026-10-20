#pragma once

#include <cstdint>
#include <vector>

class Server;

// A shot's PNG on the seat's selection, offered as image/png until the
// selection is replaced. The bytes are pumped to the reading client without
// ever blocking the loop.
class ClipboardImage {
public:
	explicit ClipboardImage(Server &server);
	~ClipboardImage();
	ClipboardImage(const ClipboardImage &) = delete;
	ClipboardImage &operator=(const ClipboardImage &) = delete;

	// Takes the selection, with `png` as its image/png.
	void copy(std::vector<std::uint8_t> png);
	// Gives up the selection before the display goes.
	void finish();

private:
	struct Source;

	void gone(); // the seat destroyed our source

	Server &server_;

	// The source, null while the selection is not ours.
	Source *source_ = nullptr;
};
