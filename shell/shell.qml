import Quickshell

// The wayland-canvas top bar: one panel per output.
ShellRoot {
	Variants {
		model: Quickshell.screens
		Bar {}
	}
}
