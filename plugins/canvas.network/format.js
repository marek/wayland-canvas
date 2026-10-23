.pragma library

// The figures the network panel shows as text. Each reads "--" where there
// is nothing to show yet, so a late answer never changes a row's shape.

function formatBytes(bytes) {
	var n = Number(bytes)
	if (!isFinite(n) || n < 0) {
		return "--"
	}
	if (n < 1024) {
		return Math.round(n) + " B"
	}
	if (n < 1024 * 1024) {
		return (n / 1024).toFixed(1) + " KB"
	}
	if (n < 1024 * 1024 * 1024) {
		return (n / (1024 * 1024)).toFixed(1) + " MB"
	}
	return (n / (1024 * 1024 * 1024)).toFixed(2) + " GB"
}

function formatRate(hasStats, rate) {
	return hasStats ? formatBytes(rate) + "/s" : "--"
}

function formatMs(ms) {
	return ms.toFixed(ms > 0 && ms < 10 ? 1 : 0) + " ms"
}

// A negative latency is a probe window with no reply in it.
function formatPing(hasPing, latency) {
	if (!hasPing) {
		return "--"
	}
	if (latency < 0) {
		return "Timeout"
	}
	return formatMs(latency)
}

function formatLoss(hasPing, loss) {
	return hasPing ? loss + "%" : "--"
}

// Bits per second.
function formatSpeed(value) {
	var mbit = value / 1e6
	if (!isFinite(mbit)) {
		return "--"
	}
	return (mbit >= 100 ? Math.round(mbit) : mbit.toFixed(1)) + " Mbit/s"
}
