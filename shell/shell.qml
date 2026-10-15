import Quickshell

// The wayland-canvas shell: a host of plugins. It draws nothing itself —
// the bar, every widget and every panel belongs to a plugin the host loads
// from the built-in, user and configured plugin directories
// (docs/intents/shell/plugins.md).
ShellRoot {
	Host {
		id: host
	}
}
