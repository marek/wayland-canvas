import Quickshell
import Quickshell.Networking
import Quickshell.Io
import QtQuick
import WaylandCanvas.Ui

// The network panel: the connection in use in the head — its name, state
// and band, share-as-QR and speed-test buttons where the system has the
// tools (a button whose tool is missing is hidden), and the Wi-Fi switch;
// the live figures, sampled only while the panel is open; the Wi-Fi band
// with an Automatic switch (off, a lock to 2.4, 5 or 6 GHz for the
// connection); the DNS provider applied to the active connection; and the
// networks — known ones first, the connected one on top, then what the
// scan finds while the panel is open, never in the background. A row click
// joins: an open or known network connects at once, a secured unknown one
// asks for a password — an enterprise one for an identity and passphrase
// too — and is never tried without them; the connected row disconnects.
PanelCard {
	id: root

	panelWidth: 340
	panelName: "network"

	// ---- what the service backends already know ----

	readonly property var device: model ? model.device : null
	readonly property var wifi: model ? model.wifi : null
	readonly property var net: model ? model.connectedNetwork : null
	readonly property bool connected: device != null && device.connected
	readonly property bool wifiOn: Networking.wifiEnabled
	readonly property string iface: device != null ? device.name : ""
	readonly property int strength: model ? model.strength : 0

	// ---- what NetworkManager's CLI fills in ----

	property string profile: "" // the active connection's name
	property string ipAddress: ""
	property string gateway: ""
	property string bandCurrent: "" // the band the radio is on
	property string bandSelected: "auto" // the pinned choice, "auto" when none
	property var bandAvailable: []
	property string dnsProvider: "DHCP"
	property bool dnsEditing: false
	property string dnsServers: ""

	// A change in flight shows the state that was asked for until the read
	// lands, so the controls answer the click instead of lagging a poll.
	property string pendingBand: ""
	property string pendingDns: ""
	readonly property string bandEffective: pendingBand !== ""
		? pendingBand : bandSelected
	readonly property string dnsEffective: pendingDns !== "" ? pendingDns : dnsProvider

	// ---- live figures ----

	property real rxTotal: -1
	property real txTotal: -1
	property real downRate: 0
	property real upRate: 0
	property real prevRx: -1
	property real prevTx: -1
	property real prevSample: 0
	property var pingSamples: [] // ms per probe, null when it timed out
	readonly property int pingWindow: 8
	readonly property int pingAverage: 5
	readonly property bool hasStats: rxTotal >= 0
	readonly property bool hasPing: pingSamples.length > 0
	readonly property real pingLatency: {
		var total = 0, count = 0
		var from = Math.max(0, pingSamples.length - pingAverage)
		for (var i = from; i < pingSamples.length; i++) {
			if (pingSamples[i] != null) {
				total += pingSamples[i]
				count++
			}
		}
		return count > 0 ? total / count : -1
	}
	readonly property int packetLoss: {
		if (pingSamples.length === 0) {
			return 0
		}
		var lost = 0
		for (var i = 0; i < pingSamples.length; i++) {
			if (pingSamples[i] == null) {
				lost++
			}
		}
		return Math.round(lost * 100 / pingSamples.length)
	}

	// ---- the head's optional tools ----

	property bool hasQrencode: false
	property string speedTool: "" // "speedtest" or "speedtest-cli", "" without one
	property var qrRows: [] // the encoded matrix, one "01" string a row
	property bool qrActive: false
	property var speedResult: null
	property bool speedRunning: false

	// ---- joining ----

	property bool promptOpen: false
	property string promptSsid: ""
	property string promptPassword: ""
	property string promptIdentity: ""

	// Networks the scan lists: connected first, then known, then by signal.
	readonly property var rows: {
		var list = wifi != null && wifi.networks ? wifi.networks.values.slice() : []
		list.sort(function(a, b) {
			if (a.connected !== b.connected) {
				return a.connected ? -1 : 1
			}
			if (a.known !== b.known) {
				return a.known ? -1 : 1
			}
			return b.signalStrength - a.signalStrength
		})
		return list
	}

	// Networks the system does not know are listed only while the device
	// scans: scan while the panel is open, never in the background.
	Binding {
		target: root.wifi
		property: "scannerEnabled"
		value: root.open
		when: root.wifi != null
	}

	onOpenChanged: if (!open) {
		// A closed panel keeps no samples: the next open starts clean, so
		// no rate is manufactured from a minutes-old counter.
		prevRx = -1
		prevTx = -1
		prevSample = 0
		pingSamples = []
		closePrompt()
		dnsEditing = false
		qrRows = []
		qrActive = false
		speedResult = null
	}

	// OWE (enhanced open) encrypts without credentials, so it asks for
	// nothing; anything else secured does, and unknown security stays on
	// the credentialed side as the conservative fallback.
	function secured(network) {
		return network.security !== WifiSecurityType.Open
			&& network.security !== WifiSecurityType.Owe
	}

	function enterprise(network) {
		return network.security === WifiSecurityType.Wpa2Eap
			|| network.security === WifiSecurityType.WpaEap
	}

	function stateName(network) {
		if (network.connected) {
			return "Connected"
		}
		if (network.state === ConnectionState.Connecting) {
			return "Connecting…"
		}
		return ""
	}

	function networkForSsid(ssid) {
		var networks = wifi != null && wifi.networks ? wifi.networks.values : []
		for (var i = 0; i < networks.length; i++) {
			if (networks[i].name === ssid) {
				return networks[i]
			}
		}
		return null
	}

	function rowClicked(network) {
		if (network.connected) {
			network.disconnect()
			return
		}
		if (secured(network) && !network.known) {
			openPrompt(network)
			return
		}
		network.connect()
	}

	function openPrompt(network) {
		if (promptSsid !== network.name) {
			promptPassword = ""
			promptIdentity = ""
		}
		promptSsid = network.name
		promptOpen = true
	}

	function closePrompt() {
		promptOpen = false
		promptSsid = ""
		promptPassword = ""
		promptIdentity = ""
	}

	function submitJoin() {
		var network = networkForSsid(promptSsid)
		if (network == null || promptPassword === ""
				|| (enterprise(network) && promptIdentity === "")) {
			return
		}
		if (enterprise(network)) {
			eap.secret = promptPassword
			eap.command = ["sh", "-c", eapScript, "eap", promptSsid, promptIdentity]
			eap.running = true
		} else {
			network.connectWithPsk(promptPassword)
		}
		closePrompt()
	}

	// The 802.1X profile is created and activated with nmcli; the passphrase
	// travels on stdin because argv is world-readable in /proc.
	readonly property string eapScript:
		"u=$(uuidgen); IFS= read -r pw;" +
		' nmcli connection add type wifi con-name "$1" ssid "$1" connection.uuid "$u"' +
		' wifi-sec.key-mgmt wpa-eap 802-1x.eap peap 802-1x.phase2-auth mschapv2' +
		' 802-1x.identity "$2" >/dev/null' +
		" && printf 'set 802-1x.password %s\\nsave\\nquit\\n' \"$pw\"" +
		' | nmcli connection edit uuid "$u" >/dev/null' +
		' && nmcli connection up uuid "$u" >/dev/null' +
		' || { nmcli connection delete uuid "$u" >/dev/null 2>&1; false; }'

	Process {
		id: eap
		property string secret: ""
		stdinEnabled: true
		onStarted: {
			write(secret + "\n")
			secret = ""
		}
	}

	// ---- the head ----

	function copy(value) {
		if (value === "") {
			return
		}
		// Detached, like the reference shell: a managed child's forked
		// wl-copy daemon would stay in the shell's own process tree and
		// never come to serve its selection.
		Quickshell.execDetached(["sh", "-c",
			'printf %s "$1" | wl-copy', "copy", value])
	}

	// A PSK network shares as a WIFI: QR payload; an enterprise one has no
	// password to encode, an open one none to fetch.
	readonly property bool canShare: wifi != null && net != null && net.connected
		&& secured(net) && !enterprise(net) && hasQrencode
	readonly property bool canSpeedTest: iface !== "" && speedTool !== ""

	// The WIFI: payload escapes \ ; , : and " in every field.
	function wifiField(value) {
		return String(value).replace(/[\\;,:"]/g, "\\$&")
	}

	function shareQr() {
		if (profile === "") {
			return
		}
		psk.command = ["nmcli", "-s", "-g", "802-11-wireless-security.psk",
			"connection", "show", profile]
		psk.running = true
	}

	function runSpeedTest() {
		if (speedRunning) {
			return
		}
		speedRunning = true
		speedResult = null
		speed.command = speedTool === "speedtest"
			? ["speedtest", "--accept-license", "--accept-gdpr", "-f", "json"]
			: ["speedtest-cli", "--json"]
		speed.running = true
	}

	// ---- the reads, while the panel is open ----

	function pollStatus() {
		if (iface === "" || status.running) {
			return
		}
		status.command = ["nmcli", "-g", "GENERAL.CONNECTION,IP4.ADDRESS,IP4.GATEWAY",
			"device", "show", iface]
		status.running = true
	}

	function pollCounters() {
		if (iface === "" || counters.running) {
			return
		}
		counters.command = ["sh", "-c",
			'cat "/sys/class/net/$1/statistics/rx_bytes" "/sys/class/net/$1/statistics/tx_bytes" 2>/dev/null',
			"counters", iface]
		counters.running = true
	}

	function pollPing() {
		if (gateway === "" || ping.running) {
			return
		}
		ping.command = ["ping", "-n", "-c", "1", "-W", "1", gateway]
		ping.running = true
	}

	// The band and DNS readings follow the connection's profile, so they run
	// once the status read has named it (and again after every change).
	function pollConnection() {
		if (profile === "") {
			return
		}
		if (!wifiList.running && wifi != null) {
			wifiList.command = ["nmcli", "-t", "-f", "IN-USE,FREQ,SSID",
				"dev", "wifi", "list", "ifname", iface, "--rescan", "no"]
			wifiList.running = true
		}
		if (!bandSel.running) {
			bandSel.command = ["nmcli", "-g", "802-11-wireless.band",
				"connection", "show", profile]
			bandSel.running = true
		}
		if (!dns.running) {
			dns.command = ["nmcli", "-g",
				"ipv4.dns,ipv4.ignore-auto-dns,ipv6.dns,ipv6.ignore-auto-dns",
				"connection", "show", profile]
			dns.running = true
		}
	}

	Timer {
		interval: 1500
		repeat: true
		running: root.open
		triggeredOnStart: true
		onTriggered: {
			root.pollStatus()
			root.pollCounters()
		}
	}

	// The wifi list shells out per row; band availability only moves when a
	// scan turns up a new BSSID, so this one walks slower.
	Timer {
		interval: 4000
		repeat: true
		running: root.open
		onTriggered: root.pollConnection()
	}

	Process {
		id: status
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var lines = root.terseLines(text)
				root.profile = lines.length > 0 ? lines[0].trim() : ""
				root.ipAddress = lines.length > 1 ? lines[1].split("/")[0].trim() : ""
				root.gateway = lines.length > 2 ? lines[2].trim() : ""
				root.pollPing()
				if (root.profile !== "") {
					root.pollConnection()
				}
			}
		}
	}

	Process {
		id: counters
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var lines = String(text).split("\n")
				var rx = parseFloat(lines[0])
				var tx = parseFloat(lines.length > 1 ? lines[1] : "")
				if (!isFinite(rx) || !isFinite(tx)) {
					root.rxTotal = -1
					root.txTotal = -1
					return
				}
				var now = Date.now() / 1000
				var dt = now - root.prevSample
				if (root.prevSample > 0 && dt > 0
						&& rx >= root.prevRx && tx >= root.prevTx) {
					root.downRate = Math.max(0, (rx - root.prevRx) / dt)
					root.upRate = Math.max(0, (tx - root.prevTx) / dt)
				} else {
					// a fresh window or a counter reset: no rate from it
					root.downRate = 0
					root.upRate = 0
				}
				root.prevRx = rx
				root.prevTx = tx
				root.prevSample = now
				root.rxTotal = rx
				root.txTotal = tx
			}
		}
	}

	Process {
		id: ping
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var match = String(text).match(/time[=<]\s*([0-9.]+)/)
				var value = match != null ? parseFloat(match[1]) : NaN
				var samples = root.pingSamples.slice()
				samples.push(isFinite(value) && value >= 0 ? value : null)
				while (samples.length > root.pingWindow) {
					samples.shift()
				}
				root.pingSamples = samples
			}
		}
	}

	Process {
		id: wifiList
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				if (root.net == null) {
					return
				}
				var ssid = root.net.name
				var lines = String(text).split("\n")
				var current = ""
				var avail = ({})
				for (var i = 0; i < lines.length; i++) {
					var line = lines[i]
					if (line === "") {
						continue
					}
					var fields = root.terseFields(line)
					var inUse = fields[0] === "*"
					var band = root.bandForFreq(parseInt(fields[1], 10))
					var name = fields.length > 2 ? fields[2] : ""
					if (band === "" || name !== ssid) {
						continue
					}
					avail[band] = true
					if (inUse) {
						current = band
					}
				}
				// The band in use is always among the options: a weak radio
				// gets missed by plenty of scans.
				if (current !== "") {
					avail[current] = true
				}
				root.bandCurrent = current
				root.bandAvailable = Object.keys(avail).sort(root.bandOrder)
			}
		}
	}

	Process {
		id: bandSel
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				root.bandSelected = root.bandFromNm(root.terseUnescape(String(text)).trim())
				root.pendingBand = ""
			}
		}
	}

	Process {
		id: dns
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var lines = root.terseLines(text)
				var ignore = (lines.length > 1 ? lines[1].trim() : "no")
					+ (lines.length > 3 ? lines[3].trim() : "no")
				root.dnsProvider = root.classifyDns(
					lines.length > 0 ? lines[0].trim() : "",
					lines.length > 2 ? lines[2].trim() : "", ignore)
				root.pendingDns = ""
			}
		}
	}

	// ---- the writes ----

	// Locking the band reassociates; the panel stays open so the figures
	// report the reconnect as it happens.
	function setBand(band) {
		if (profile === "" || action.running) {
			return
		}
		pendingBand = band
		action.command = ["sh", "-c",
			'nmcli connection modify "$1" 802-11-wireless.band "$2" && nmcli connection up "$1"',
			"band", profile, nmBand(band)]
		action.running = true
	}

	// Switching Automatic off has to commit to something, so it pins the
	// band the radio already landed on.
	function toggleAuto() {
		if (bandEffective !== "auto") {
			setBand("auto")
		} else if (bandCurrent !== "") {
			setBand(bandCurrent)
		}
	}

	function setDns(provider, servers) {
		if (profile === "" || action.running) {
			return
		}
		var v4 = ""
		var v6 = ""
		if (provider === "Cloudflare") {
			v4 = "1.1.1.1 1.0.0.1"
			v6 = "2606:4700:4700::1111 2606:4700:4700::1001"
		} else if (provider === "Google") {
			v4 = "8.8.8.8 8.8.4.4"
			v6 = "2001:4860:4860::8888 2001:4860:4860::8844"
		} else if (provider !== "DHCP") {
			// Custom: the typed servers, split by family.
			var parts = String(servers).split(/[\s,]+/)
			for (var i = 0; i < parts.length; i++) {
				if (parts[i] === "") {
					continue
				}
				if (parts[i].indexOf(":") >= 0) {
					v6 += (v6 !== "" ? " " : "") + parts[i]
				} else {
					v4 += (v4 !== "" ? " " : "") + parts[i]
				}
			}
		}
		var ignore = provider === "DHCP" ? "no" : "yes"
		pendingDns = provider
		action.command = ["sh", "-c",
			'nmcli connection modify "$1" ipv4.ignore-auto-dns "$2" ipv4.dns "$3"'
			+ ' ipv6.ignore-auto-dns "$4" ipv6.dns "$5" && nmcli device reapply "$6"',
			"dns", profile, ignore, v4, ignore, v6, iface]
		action.running = true
	}

	function dnsClicked(provider) {
		if (provider === "Custom") {
			dnsEditing = !dnsEditing
			return
		}
		setDns(provider, "")
	}

	function submitDns() {
		if (dnsServers.trim() === "") {
			return
		}
		setDns("Custom", dnsServers)
		dnsEditing = false
	}

	function forget(network) {
		if (!network.connected) {
			network.forget()
		}
	}

	Process {
		id: action
		stdout: StdioCollector { waitForEnd: true }
		onExited: function(exitCode) {
			if (pendingBand !== "" && exitCode !== 0) {
				// A refused or reverted pin leaves the pills showing what is
				// actually in force, not what was asked.
				pendingBand = ""
			}
			pollConnection()
		}
	}

	Process {
		id: psk
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var secret = root.terseUnescape(String(text).replace(/\n$/, ""))
				if (secret === "" || root.net == null) {
					return
				}
				qr.payload = "WIFI:T:WPA;S:" + root.wifiField(root.net.name)
					+ ";P:" + root.wifiField(secret) + ";;"
				// ASCII with its quiet zone: the matrix is read back as
				// rows, then drawn as modules — block text renders with
				// the font's own spacing and stops scanning
				qr.command = ["qrencode", "-t", "ASCII", "-m", "4"]
				qr.stdinEnabled = true
				qr.running = true
			}
		}
	}

	// The payload carries the password, so it goes in on stdin and never
	// on the command line; qrencode reads to the end of its stdin, which
	// closes behind the write.
	Process {
		id: qr
		property string payload: ""
		onStarted: {
			write(payload)
			payload = ""
			stdinEnabled = false
		}
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				// the ASCII type prints two characters a module: "##"
				// dark, "  " light. Only the final newline may go: the
				// quiet zone's all-space rows are modules too, and a
				// whole-string trim would eat them and skew the matrix
				var lines = String(text).replace(/\r/g, "").split("\n")
				while (lines.length > 0 && lines[lines.length - 1] === "") {
					lines.pop()
				}
				var rows = []
				for (var i = 0; i < lines.length; i++) {
					var line = lines[i]
					var row = ""
					for (var j = 0; j + 1 < line.length; j += 2) {
						row += line.charAt(j) === "#" ? "1" : "0"
					}
					if (row !== "") {
						rows.push(row)
					}
				}
				if (rows.length > 0 && rows.length === rows[0].length) {
					qrRows = rows
					qrActive = true
				}
			}
		}
	}


	Process {
		id: speed
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				speedRunning = false
				try {
					var json = JSON.parse(text)
					// Ookla reports bytes per second, speedtest-cli bits.
					var down = json.download != null
						? (json.download.bandwidth != null
							? json.download.bandwidth * 8 : json.download) : NaN
					var up = json.upload != null
						? (json.upload.bandwidth != null
							? json.upload.bandwidth * 8 : json.upload) : NaN
					var latency = json.ping != null
						? (json.ping.latency != null ? json.ping.latency : json.ping) : NaN
					speedResult = {down: down, up: up, ping: latency}
				} catch (e) {
					speedResult = null
				}
			}
		}
	}

	// The tool probes run once at load: presence does not change while the
	// shell runs, and a button whose tool is missing is hidden.
	Component.onCompleted: {
		probe.command = ["sh", "-c",
			"command -v qrencode >/dev/null && echo QR;" +
			" command -v speedtest >/dev/null && echo SPEEDTEST;" +
			" command -v speedtest-cli >/dev/null && echo SPEEDTEST_CLI;" +
			" true"] // the last probe's failure must not fail the process
		probe.running = true
	}

	Process {
		id: probe
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var answer = String(text) // not "var text": a local of
				// that name would shadow the collector's own property
				hasQrencode = answer.indexOf("QR") >= 0
				if (answer.indexOf("SPEEDTEST\n") >= 0) {
					speedTool = "speedtest"
				} else if (answer.indexOf("SPEEDTEST_CLI") >= 0) {
					speedTool = "speedtest-cli"
				}
			}
		}
	}

	// ---- formatting ----

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

	function classifyDns(v4, v6, ignore) {
		var all = v4 + " " + v6
		if (ignore.indexOf("yes") < 0 || all.trim() === "") {
			return "DHCP"
		}
		if (all.indexOf("1.1.1.1") >= 0 || all.indexOf("2606:4700") >= 0) {
			return "Cloudflare"
		}
		if (all.indexOf("8.8.8.8") >= 0 || all.indexOf("2001:4860") >= 0) {
			return "Google"
		}
		return "Custom"
	}

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

	function formatRate(rate) {
		return hasStats ? formatBytes(rate) + "/s" : "--"
	}

	function formatPing() {
		if (!hasPing) {
			return "--"
		}
		if (pingLatency < 0) {
			return "Timeout"
		}
		return pingLatency.toFixed(pingLatency > 0 && pingLatency < 10 ? 1 : 0) + " ms"
	}

	function formatLoss() {
		return hasPing ? packetLoss + "%" : "--"
	}

	function formatSpeed(value) {
		var mbit = value / 1e6
		if (!isFinite(mbit)) {
			return "--"
		}
		return (mbit >= 100 ? Math.round(mbit) : mbit.toFixed(1)) + " Mbit/s"
	}

	// ---- the content ----

	// The panel in its normal shape; the QR and speed-test answers each
	// replace it with their own rows while they are shown.
	Item {
		id: normalView
		width: parent.width
		height: normal.implicitHeight
		visible: !qrActive && speedResult == null

		Column {
			id: normal
			width: parent.width
			spacing: UiTheme.panelSpacing

			PanelHero {
				width: parent.width
				glyph: {
					if (wifi == null) {
						return device != null && device.type === DeviceType.Ethernet
							? "󰈀" : "󰛳"
					}
					if (!wifiOn) {
						return "󰤭"
					}
					return connected && net != null ? model.wifiGlyph(strength) : "󰤮"
				}
				title: {
					if (wifi == null) {
						return device != null && device.type === DeviceType.Ethernet
							? "Ethernet" : "No network"
					}
					if (!wifiOn) {
						return "Wi-Fi off"
					}
					if (connected && net != null) {
						return net.name !== "" ? net.name : "Hidden network"
					}
					return "Wi-Fi"
				}
				meta: {
					if (wifi == null) {
						return connected ? "Connected" : "Not connected"
					}
					if (!wifiOn) {
						return "Radio off"
					}
					if (!connected) {
						return "Not connected"
					}
					return bandCurrent !== ""
						? "Connected · " + bandCurrent + " GHz" : "Connected"
				}

				HeroButton {
					visible: root.canShare
					glyph: "󰐲"
					onClicked: root.shareQr()
				}

				HeroButton {
					visible: root.canSpeedTest
					glyph: "󰓅"
					onClicked: root.runSpeedTest()
				}

				PanelSwitch {
					visible: root.wifi != null
					checked: root.wifiOn
					onToggled: Networking.wifiEnabled = !Networking.wifiEnabled
				}
			}

			// The live figures, two columns; every row stays mounted whether
			// or not its sample has landed, so a late answer never reflows.
			// The left column takes the wider share: the IP Address pair
			// would not fit an equal half of this panel's width.
			Grid {
				visible: root.connected
				width: parent.width
				columns: 2
				spacing: 6

				Figure {
					share: 0.55
					label: "Ping"
					value: root.formatPing()
				}
				Figure {
					share: 0.45
					label: "Packet Loss"
					value: root.formatLoss()
				}
				Figure {
					share: 0.55
					label: "Receiving"
					value: root.formatRate(root.downRate)
				}
				Figure {
					share: 0.45
					label: "Sending"
					value: root.formatRate(root.upRate)
				}
				Figure {
					share: 0.55
					label: "Downloaded"
					value: root.hasStats ? root.formatBytes(root.rxTotal)
						: "--"
				}
				Figure {
					share: 0.45
					label: "Uploaded"
					value: root.hasStats ? root.formatBytes(root.txTotal)
						: "--"
				}
				Figure {
					share: 0.55
					label: "IP Address"
					value: root.ipAddress !== "" ? root.ipAddress : "--"
					copyable: root.ipAddress !== ""
				}
				Figure {
					share: 0.45
					label: "Gateway"
					value: root.gateway !== "" ? root.gateway : "--"
					copyable: root.gateway !== ""
				}
			}

			PanelSeparator {
				visible: root.wifi != null && root.connected
			}

			// The band in use, with Automatic on the header line; pinned,
			// the pills below offer the lock.
			Item {
				visible: root.wifi != null && root.connected
				width: parent.width
				height: pills.visible ? 48 : 18

				PanelSectionHeader {
					id: bandHeader
					text: bandEffective === "auto" && root.bandCurrent !== ""
						? "Wi-Fi band: " + root.bandCurrent + " GHz" : "Wi-Fi band"
				}

				Row {
					anchors.right: parent.right
					anchors.verticalCenter: bandHeader.verticalCenter
					spacing: 6

					PanelSectionHeader {
						// inside the row: its own width, not the row's
						// (which would loop the layout)
						width: implicitWidth
						text: "Automatic"
					}

					PanelSwitch {
						checked: root.bandEffective === "auto"
						onToggled: root.toggleAuto()
					}
				}

				Row {
					id: pills
					visible: root.bandEffective !== "auto" && root.bandAvailable.length > 1
					y: 24
					width: parent.width
					spacing: 6

					Repeater {
						model: root.bandAvailable

						Pill {
							id: bandPill
							required property string modelData
							width: (parent.width - 2 * pills.spacing) / 3
							text: modelData + " GHz"
							active: root.bandCurrent === modelData
							chosen: root.bandEffective === modelData
							onClicked: root.setBand(modelData)
						}
					}
				}
			}

			PanelSeparator {
				visible: root.profile !== ""
			}

			Item {
				visible: root.profile !== ""
				width: parent.width
				height: dnsField.visible ? 84 : 48

				PanelSectionHeader {
					id: dnsHeader
					text: "DNS provider"
				}

				Row {
					id: dnsPills
					y: 24
					width: parent.width
					spacing: 6

					Pill {
						width: (parent.width - 3 * dnsPills.spacing) / 4
						text: "DHCP"
						active: root.dnsEffective === "DHCP"
						onClicked: root.dnsClicked("DHCP")
					}

					Pill {
						width: (parent.width - 3 * dnsPills.spacing) / 4
						text: "Cloudflare"
						active: root.dnsEffective === "Cloudflare"
						onClicked: root.dnsClicked("Cloudflare")
					}

					Pill {
						width: (parent.width - 3 * dnsPills.spacing) / 4
						text: "Google"
						active: root.dnsEffective === "Google"
						onClicked: root.dnsClicked("Google")
					}

					Pill {
						width: (parent.width - 3 * dnsPills.spacing) / 4
						text: "Custom"
						active: root.dnsEffective === "Custom" && !root.dnsEditing
						chosen: root.dnsEditing
						onClicked: root.dnsClicked("Custom")
					}
				}

				PanelField {
					id: dnsField
					visible: root.dnsEditing
					y: 54
					width: parent.width
					placeholderText: "DNS servers"
					onTextChanged: if (root.dnsEditing) root.dnsServers = text
					onAccepted: root.submitDns()
					TapHandler { onTapped: dnsField.forceActiveFocus() }
					onVisibleChanged: if (!visible && root.open) {
						normalView.forceActiveFocus()
					}
				}
			}

			PanelSeparator {
				visible: root.wifi != null && root.wifiOn && root.rows.length > 0
			}

			PanelText {
				visible: root.wifi != null && !root.wifiOn
				dim: true
				width: parent.width
				height: UiTheme.sectionHeight
				text: "Wi-Fi is off"
			}

			PanelText {
				visible: root.wifi != null && root.wifiOn && root.rows.length === 0
				dim: true
				width: parent.width
				height: UiTheme.sectionHeight
				text: "None found"
			}

			Repeater {
				model: root.wifi != null && root.wifiOn ? root.rows : []

				Column {
					id: entry
					width: parent.width
					spacing: UiTheme.panelSpacing

					required property var modelData
					required property int index
					readonly property var network: modelData
					readonly property bool promptMine: root.promptOpen
						&& root.promptSsid === (network != null ? network.name : "\x01")
					readonly property bool firstKnown: network != null && network.known
						&& (index === 0 || !root.rows[index - 1].known)
					readonly property bool firstOther: network != null && !network.known
						&& (index === 0 || root.rows[index - 1].known)

					PanelSectionHeader {
						visible: entry.firstKnown
						text: "Known networks"
					}

					PanelSectionHeader {
						visible: entry.firstOther
						text: "Other networks"
					}

					PanelRow {
						width: parent.width
						onClicked: root.rowClicked(entry.network)
						glyph: model.wifiGlyph(model.percent(network))
						label: network.name !== "" ? network.name
							: "Hidden network"
						detail: root.stateName(network)
						selected: network.connected

						// The right edge: a lock while credentials are
						// needed, Forget for known networks not in use.
						Row {
							anchors.right: parent.right
							anchors.rightMargin: 8
							anchors.verticalCenter: parent.verticalCenter
							spacing: 6

							Text {
								visible: root.secured(entry.network)
								text: "󰌾"
								color: UiTheme.muted
								font {
									family: UiTheme.fontFamily
									pixelSize: UiTheme.fontIcon
								}
								renderType: Text.NativeRendering
							}

							Item {
								visible: entry.network.known && !entry.network.connected
								width: 14
								height: 14

								Text {
									anchors.centerIn: parent
									text: "󰅙"
									color: UiTheme.attention
									font {
										family: UiTheme.fontFamily
										pixelSize: UiTheme.fontIcon
									}
									renderType: Text.NativeRendering
								}

								MouseArea {
									anchors.fill: parent
									hoverEnabled: true
									cursorShape: Qt.PointingHandCursor
									onClicked: root.forget(entry.network)
								}
							}
						}
					}

					// The credentials prompt, inline under its row: a
					// password, or an identity and passphrase on an
					// enterprise network — Enter submits, nothing connects
					// before it is filled.
					Column {
						visible: entry.promptMine
						width: parent.width
						spacing: 4

						PanelField {
							id: identity
							visible: root.enterprise(entry.network)
							width: parent.width
							height: 30
							placeholderText: "Identity"
							text: root.promptIdentity
							onTextChanged: if (entry.promptMine
									&& text !== root.promptIdentity) {
								root.promptIdentity = text
							}
							onAccepted: passphrase.forceActiveFocus()
							TapHandler {
								onTapped: identity.forceActiveFocus()
							}
							onVisibleChanged: {
								if (visible) {
									Qt.callLater(forceActiveFocus)
								} else if (root.open) {
									normalView.forceActiveFocus()
								}
							}
						}

						PanelField {
							id: passphrase
							width: parent.width
							height: 30
							password: true
							placeholderText: root.enterprise(entry.network)
								? "Passphrase" : "Password"
							text: root.promptPassword
							onTextChanged: if (entry.promptMine
									&& text !== root.promptPassword) {
								root.promptPassword = text
							}
							onAccepted: root.submitJoin()
							TapHandler {
								onTapped: passphrase.forceActiveFocus()
							}
							onVisibleChanged: {
								if (visible
										&& !root.enterprise(entry.network)) {
									Qt.callLater(forceActiveFocus)
								} else if (!visible && root.open) {
									// the panel keeps the keys: Escape still
									// closes after a prompt folded away
									normalView.forceActiveFocus()
								}
							}
						}
						}
				}
			}
		}
	}

	// The share view: the QR of the network in use, the payload fetched
	// through NetworkManager's secrets and drawn module by module on a
	// light card — dark on light with its quiet zone, the way scanners
	// read it, and never as text: fonts put their own spacing between
	// block characters and the code stops scanning.
	Column {
		visible: root.qrActive
		width: parent.width
		spacing: UiTheme.panelSpacing

		PanelSectionHeader {
			text: root.net != null && root.net.name !== ""
				? "Share " + root.net.name : "Share network"
		}

		Rectangle {
			id: qrCard
			readonly property int size: root.qrRows.length
			readonly property int module: size > 0
				? Math.max(4, Math.floor(240 / size)) : 4
			anchors.horizontalCenter: parent.horizontalCenter
			width: size * module
			height: width
			radius: 4
			color: "#ffffff"

			Grid {
				anchors.centerIn: parent
				columns: qrCard.size

				Repeater {
					model: qrCard.size * qrCard.size

					Rectangle {
						required property int index
						width: qrCard.module
						height: qrCard.module
						color: root.qrRows[Math.floor(index / qrCard.size)]
							.charAt(index % qrCard.size) === "1"
							? "#111111" : "transparent"
					}
				}
			}
		}

		PanelRow {
			width: parent.width
			label: "Back"
			onClicked: root.qrActive = false
		}
	}

	// The speed-test view: the tool's answer as rows, until Back.
	Column {
		visible: root.speedResult != null
		width: parent.width
		spacing: UiTheme.panelSpacing

		PanelSectionHeader {
			text: "Speed test"
		}

		PanelRow {
			width: parent.width
			clickable: false
			label: "Download"
			detail: root.speedResult != null ? root.formatSpeed(root.speedResult.down) : ""
		}

		PanelRow {
			width: parent.width
			clickable: false
			label: "Upload"
			detail: root.speedResult != null ? root.formatSpeed(root.speedResult.up) : ""
		}

		PanelRow {
			width: parent.width
			clickable: false
			label: "Ping"
			detail: root.speedResult != null && isFinite(root.speedResult.ping)
				? root.speedResult.ping.toFixed(
					root.speedResult.ping > 0 && root.speedResult.ping < 10 ? 1 : 0) + " ms" : ""
		}

		PanelRow {
			width: parent.width
			label: "Back"
			onClicked: root.speedResult = null
		}
	}

	// ---- the panel's own small pieces ----

	// One label-and-value figure, right-aligned in its half of a row;
	// the value copies itself when it holds something worth copying.
	component Figure: Item {
		id: fig
		property string label: ""
		property string value: ""
		property bool copyable: false
		// the row's halves are equal, except where a pair needs more room
		// than its share: the last row's IP and its label together are
		// wider than half, so that figure takes a little more of the row
		property real share: 0.5
		width: (parent.width - 6) * share
		height: 16

		Row {
			anchors.verticalCenter: parent.verticalCenter
			spacing: 4

			PanelText {
				id: figureLabel
				dim: true
				text: fig.label
				// its own width: a fixed one truncates the longer labels
				width: implicitWidth
			}

			Item {
				width: fig.width - figureLabel.width - 4
				height: fig.height

				PanelText {
					anchors.right: parent.right
					anchors.verticalCenter: parent.verticalCenter
					width: parent.width
					horizontalAlignment: Text.AlignRight
					elide: Text.ElideRight
					text: fig.value
				}

				MouseArea {
					anchors.fill: parent
					enabled: fig.copyable
					hoverEnabled: enabled
					cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
					onClicked: root.copy(fig.value)
				}
			}
		}
	}


	// One pill of the band or DNS rows: the one in force carries the tint,
	// the pinned choice the accent border and weight.
	component Pill: Item {
		id: pill
		property string text: ""
		property bool active: false
		property bool chosen: false
		signal clicked()
		height: 24

		Rectangle {
			anchors.fill: parent
			radius: 4
			border.width: 1
			border.color: pill.chosen ? UiTheme.accent : UiTheme.border
			color: pill.active ? UiTheme.selected : "transparent"
		}

		PanelText {
			anchors.centerIn: parent
			text: pill.text
			font.bold: pill.chosen
		}

		MouseArea {
			anchors.fill: parent
			hoverEnabled: true
			cursorShape: Qt.PointingHandCursor
			onClicked: pill.clicked()
		}
	}

	// A head control: a bordered glyph square.
	component HeroButton: Item {
		id: btn
		property string glyph: ""
		signal clicked()
		width: 26
		height: 26

		Rectangle {
			anchors.fill: parent
			radius: 4
			border.width: 1
			border.color: UiTheme.border
			color: mouse.containsMouse ? UiTheme.hover : "transparent"
		}

		Text {
			anchors.centerIn: parent
			text: btn.glyph
			textFormat: Text.PlainText
			renderType: Text.NativeRendering
			color: UiTheme.text
			font {
				family: UiTheme.fontFamily
				pixelSize: UiTheme.fontIcon
			}
		}

		MouseArea {
			id: mouse
			anchors.fill: parent
			hoverEnabled: true
			cursorShape: Qt.PointingHandCursor
			onClicked: btn.clicked()
		}
	}
}
