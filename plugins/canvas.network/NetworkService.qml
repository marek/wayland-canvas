import Quickshell.Io
import QtQuick
import "nmcli.js" as Nm

// What the Quickshell backend does not know about the connection in use, read
// and written through NetworkManager's CLI and the optional tools: address,
// gateway and profile, the Wi-Fi band, the DNS provider, the live figures, the
// share QR, the speed test and the enterprise join. Reads, samples and the
// scan run only while `live` is set; the share and speed answers drop with it.
Item {
	id: root

	property var device: null
	property var wifi: null
	property var network: null

	// Set by the panel while it is open.
	property bool live: false

	readonly property string iface: device != null ? device.name : ""

	property string profile: "" // the active connection's name
	property string ipAddress: ""
	property string gateway: ""
	property string bandCurrent: "" // the band the radio is on
	property string bandSelected: "auto" // the pinned choice, "auto" when none
	property var bandAvailable: []
	property string dnsProvider: Nm.automaticDns

	// A change in flight shows the state that was asked for until the read
	// lands, so the controls answer the click instead of lagging a poll.
	property string pendingBand: ""
	property string pendingDns: ""
	readonly property string bandEffective: pendingBand !== ""
		? pendingBand : bandSelected
	readonly property string dnsEffective: pendingDns !== "" ? pendingDns : dnsProvider

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

	property bool hasQrencode: false
	property string speedTool: "" // "speedtest" or "speedtest-cli", "" without one
	property var qrRows: [] // the encoded matrix, one "01" string a row
	property var speedResult: null
	property bool speedRunning: false
	readonly property bool sharing: qrRows.length > 0

	// Networks the system does not know are listed only while the device
	// scans: scan while the panel is open, never in the background.
	Binding {
		target: root.wifi
		property: "scannerEnabled"
		value: root.live
		when: root.wifi != null
	}

	onLiveChanged: if (!live) {
		// A closed panel keeps no samples: the next open starts clean, so
		// no rate is manufactured from a minutes-old counter.
		prevRx = -1
		prevTx = -1
		prevSample = 0
		pingSamples = []
		qrRows = []
		speedResult = null
	}

	function closeShare() {
		qrRows = []
	}

	function closeSpeed() {
		speedResult = null
	}

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
		running: root.live
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
		running: root.live
		onTriggered: root.pollConnection()
	}

	Process {
		id: status
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var lines = Nm.terseLines(text)
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
				if (root.network == null) {
					return
				}
				var bands = Nm.bandsOf(text, root.network.name)
				root.bandCurrent = bands.current
				root.bandAvailable = bands.available
			}
		}
	}

	Process {
		id: bandSel
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				root.bandSelected = Nm.bandFromNm(Nm.terseUnescape(String(text)).trim())
				root.pendingBand = ""
			}
		}
	}

	Process {
		id: dns
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var lines = Nm.terseLines(text)
				var ignore = (lines.length > 1 ? lines[1].trim() : "no")
					+ (lines.length > 3 ? lines[3].trim() : "no")
				root.dnsProvider = Nm.classifyDns(
					lines.length > 0 ? lines[0].trim() : "",
					lines.length > 2 ? lines[2].trim() : "", ignore)
				root.pendingDns = ""
			}
		}
	}

	// Locking the band reassociates; the panel stays open so the figures
	// report the reconnect as it happens.
	function setBand(band) {
		if (profile === "" || action.running) {
			return
		}
		pendingBand = band
		action.command = ["sh", "-c",
			'nmcli connection modify "$1" 802-11-wireless.band "$2" && nmcli connection up "$1"',
			"band", profile, Nm.nmBand(band)]
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

	// `servers` is what the user typed, read only for the typed provider.
	function setDns(provider, servers) {
		var request = Nm.dnsRequest(provider, servers)
		if (profile === "" || action.running || request === null) {
			return
		}
		pendingDns = provider
		action.command = ["sh", "-c",
			'nmcli connection modify "$1" ipv4.ignore-auto-dns "$2" ipv4.dns "$3"'
			+ ' ipv6.ignore-auto-dns "$4" ipv6.dns "$5" && nmcli device reapply "$6"',
			"dns", profile, request.ignoreAuto, request.v4, request.ignoreAuto,
			request.v6, iface]
		action.running = true
	}

	Process {
		id: action
		stdout: StdioCollector { waitForEnd: true }
		onExited: function(exitCode) {
			if (root.pendingBand !== "" && exitCode !== 0) {
				// A refused or reverted pin leaves the pills showing what is
				// actually in force, not what was asked.
				root.pendingBand = ""
			}
			root.pollConnection()
		}
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

	function joinEnterprise(ssid, identity, passphrase) {
		eap.secret = passphrase
		eap.command = ["sh", "-c", eapScript, "eap", ssid, identity]
		eap.running = true
	}

	Process {
		id: eap
		property string secret: ""
		stdinEnabled: true
		onStarted: {
			write(secret + "\n")
			secret = ""
		}
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

	Process {
		id: psk
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				var secret = Nm.terseUnescape(String(text).replace(/\n$/, ""))
				if (secret === "" || root.network == null) {
					return
				}
				qr.payload = Nm.wifiPayload(root.network.name, secret)
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
				var rows = Nm.qrRows(text)
				if (rows.length > 0) {
					root.qrRows = rows
				}
			}
		}
	}

	Process {
		id: speed
		stdout: StdioCollector {
			waitForEnd: true
			onStreamFinished: {
				root.speedRunning = false
				root.speedResult = Nm.speedResult(text)
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
				root.hasQrencode = answer.indexOf("QR") >= 0
				if (answer.indexOf("SPEEDTEST\n") >= 0) {
					root.speedTool = "speedtest"
				} else if (answer.indexOf("SPEEDTEST_CLI") >= 0) {
					root.speedTool = "speedtest-cli"
				}
			}
		}
	}
}
