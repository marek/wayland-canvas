.pragma library

// What NetworkManager's CLI and the optional tools print, and the words the
// panel asks them in: terse output, bands, DNS providers, the Wi-Fi QR
// payload and the matrix and speed answers read back. Nothing here touches
// a process or an item, so the service and the views share one reading of
// each.

// nmcli's terse output escapes ":" and "\" with a backslash in every
// value: a value is read by undoing the escapes, a line by cutting it
// at the colons that are not escaped.
function terseUnescape(value) {
	return String(value).replace(/\\(.)/g, "$1")
}

function terseLines(text) {
	return String(text).split("\n").map(function(line) {
		return terseUnescape(line)
	})
}

function terseFields(line) {
	var fields = []
	var field = ""
	for (var i = 0; i < line.length; i++) {
		var ch = line.charAt(i)
		if (ch === "\\" && i + 1 < line.length) {
			field += line.charAt(++i)
		} else if (ch === ":") {
			fields.push(field)
			field = ""
		} else {
			field += ch
		}
	}
	fields.push(field)
	return fields
}

function bandForFreq(mhz) {
	if (mhz >= 2400 && mhz < 2500) {
		return "2.4"
	}
	if (mhz >= 4900 && mhz < 5925) {
		return "5"
	}
	if (mhz >= 5925 && mhz < 7125) {
		return "6"
	}
	return ""
}

function bandOrder(a, b) {
	var order = ["2.4", "5", "6"]
	return order.indexOf(a) - order.indexOf(b)
}

// NetworkManager names the bands "a", "bg" and — since 1.44 — "6GHz".
function nmBand(band) {
	if (band === "2.4") {
		return "bg"
	}
	if (band === "5") {
		return "a"
	}
	if (band === "6") {
		return "6GHz"
	}
	return ""
}

function bandFromNm(value) {
	if (value === "bg") {
		return "2.4"
	}
	if (value === "a") {
		return "5"
	}
	if (value === "6GHz") {
		return "6"
	}
	return "auto"
}

// The bands one network is heard on and the one the radio is on, from
// `nmcli -t -f IN-USE,FREQ,SSID dev wifi list`. The band in use is always
// among the options: a weak radio gets missed by plenty of scans.
function bandsOf(text, ssid) {
	var lines = String(text).split("\n")
	var current = ""
	var available = ({})
	for (var i = 0; i < lines.length; i++) {
		if (lines[i] === "") {
			continue
		}
		var fields = terseFields(lines[i])
		var band = bandForFreq(parseInt(fields[1], 10))
		var name = fields.length > 2 ? fields[2] : ""
		if (band === "" || name !== ssid) {
			continue
		}
		available[band] = true
		if (fields[0] === "*") {
			current = band
		}
	}
	if (current !== "") {
		available[current] = true
	}
	return {current: current, available: Object.keys(available).sort(bandOrder)}
}

// The DNS providers, in the order the panel lists them. Automatic keeps the
// connection's own servers; a provider with `servers` sets them for both
// families, and is told apart by one address of each (`marks`); the typed
// one takes the servers the user writes, and is what any other setting
// reads as.
var dnsProviders = [
	{name: "DHCP", automatic: true},
	{
		name: "Cloudflare",
		servers: {
			v4: "1.1.1.1 1.0.0.1",
			v6: "2606:4700:4700::1111 2606:4700:4700::1001"
		},
		marks: ["1.1.1.1", "2606:4700"]
	},
	{
		name: "Google",
		servers: {
			v4: "8.8.8.8 8.8.4.4",
			v6: "2001:4860:4860::8888 2001:4860:4860::8844"
		},
		marks: ["8.8.8.8", "2001:4860"]
	},
	{name: "Custom", typed: true}
]

var automaticDns = dnsProviders.find(function(provider) {
	return provider.automatic
}).name

function dnsServersOf(provider, typed) {
	if (provider.servers) {
		return provider.servers
	}
	var servers = {v4: "", v6: ""}
	if (!provider.typed) {
		return servers
	}
	var parts = String(typed).split(/[\s,]+/)
	for (var i = 0; i < parts.length; i++) {
		if (parts[i] === "") {
			continue
		}
		var family = parts[i].indexOf(":") >= 0 ? "v6" : "v4"
		servers[family] += (servers[family] !== "" ? " " : "") + parts[i]
	}
	return servers
}

// What `nmcli connection modify` is given to put a provider in force, null
// for a name the list does not hold: the servers per family, and whether the
// connection's own are ignored.
function dnsRequest(name, typed) {
	var provider = dnsProviders.find(function(entry) {
		return entry.name === name
	})
	if (provider === undefined) {
		return null
	}
	var servers = dnsServersOf(provider, typed)
	return {
		v4: servers.v4,
		v6: servers.v6,
		ignoreAuto: provider.automatic ? "no" : "yes"
	}
}

// The provider a connection's DNS settings amount to.
function classifyDns(v4, v6, ignore) {
	var all = v4 + " " + v6
	if (ignore.indexOf("yes") < 0 || all.trim() === "") {
		return automaticDns
	}
	var known = dnsProviders.find(function(provider) {
		return provider.marks !== undefined && provider.marks.some(function(mark) {
			return all.indexOf(mark) >= 0
		})
	})
	return known !== undefined ? known.name : dnsProviders.find(function(provider) {
		return provider.typed
	}).name
}

// The WIFI: payload escapes \ ; , : and " in every field.
function wifiField(value) {
	return String(value).replace(/[\\;,:"]/g, "\\$&")
}

function wifiPayload(ssid, secret) {
	return "WIFI:T:WPA;S:" + wifiField(ssid) + ";P:" + wifiField(secret) + ";;"
}

// qrencode's ASCII type prints two characters a module: "##" dark, "  "
// light, with its quiet zone. Only the final newline may go: the quiet
// zone's all-space rows are modules too, and a whole-string trim would eat
// them and skew the matrix. Rows of "0" and "1", one string a row; empty
// unless the matrix is square.
function qrRows(text) {
	var lines = String(text).replace(/\r/g, "").split("\n")
	while (lines.length > 0 && lines[lines.length - 1] === "") {
		lines.pop()
	}
	var rows = []
	for (var i = 0; i < lines.length; i++) {
		var row = ""
		for (var j = 0; j + 1 < lines[i].length; j += 2) {
			row += lines[i].charAt(j) === "#" ? "1" : "0"
		}
		if (row !== "") {
			rows.push(row)
		}
	}
	return rows.length > 0 && rows.length === rows[0].length ? rows : []
}

// A speed tool's JSON as bits per second and milliseconds; null when it is
// not JSON. Ookla reports bytes per second, speedtest-cli bits.
function speedResult(text) {
	try {
		var json = JSON.parse(text)
		var down = json.download != null
			? (json.download.bandwidth != null
				? json.download.bandwidth * 8 : json.download) : NaN
		var up = json.upload != null
			? (json.upload.bandwidth != null
				? json.upload.bandwidth * 8 : json.upload) : NaN
		var latency = json.ping != null
			? (json.ping.latency != null ? json.ping.latency : json.ping) : NaN
		return {down: down, up: up, ping: latency}
	} catch (e) {
		return null
	}
}
