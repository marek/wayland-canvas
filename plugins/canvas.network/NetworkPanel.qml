import Quickshell
import Quickshell.Networking
import QtQuick
import WaylandCanvas.Ui
import "format.js" as Format
import "."

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
// What nmcli and the tools tell is NetworkState.service's; the band, DNS,
// share and speed-test views are their own files.
PanelCard {
	id: root

	panelWidth: 340

	readonly property var service: NetworkState.service
	readonly property var device: NetworkState.device
	readonly property var wifi: NetworkState.wifi
	readonly property var net: NetworkState.connectedNetwork
	readonly property bool connected: device != null && device.connected
	readonly property bool wifiOn: Networking.wifiEnabled
	readonly property int strength: NetworkState.strength

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

	Binding {
		target: root.service
		property: "live"
		value: root.open
	}

	onOpenChanged: if (!open) {
		closePrompt()
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
			service.joinEnterprise(promptSsid, promptIdentity, promptPassword)
		} else {
			network.connectWithPsk(promptPassword)
		}
		closePrompt()
	}

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

	function forget(network) {
		if (!network.connected) {
			network.forget()
		}
	}

	// A PSK network shares as a WIFI: QR payload; an enterprise one has no
	// password to encode, an open one none to fetch.
	readonly property bool canShare: wifi != null && net != null && net.connected
		&& secured(net) && !enterprise(net) && service.hasQrencode
	readonly property bool canSpeedTest: service.iface !== "" && service.speedTool !== ""

	// The panel in its normal shape; the QR and speed-test answers each
	// replace it with their own rows while they are shown.
	Item {
		id: normalView
		width: parent.width
		height: normal.implicitHeight
		visible: !root.service.sharing && root.service.speedResult == null

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
					return connected && net != null ? NetworkState.wifiGlyph(strength) : "󰤮"
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
					return root.service.bandCurrent !== ""
						? "Connected · " + root.service.bandCurrent + " GHz" : "Connected"
				}

				HeroButton {
					visible: root.canShare
					glyph: "󰐲"
					onClicked: root.service.shareQr()
				}

				HeroButton {
					visible: root.canSpeedTest
					glyph: "󰓅"
					onClicked: root.service.runSpeedTest()
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
					value: Format.formatPing(root.service.hasPing, root.service.pingLatency)
				}
				Figure {
					share: 0.45
					label: "Packet Loss"
					value: Format.formatLoss(root.service.hasPing, root.service.packetLoss)
				}
				Figure {
					share: 0.55
					label: "Receiving"
					value: Format.formatRate(root.service.hasStats, root.service.downRate)
				}
				Figure {
					share: 0.45
					label: "Sending"
					value: Format.formatRate(root.service.hasStats, root.service.upRate)
				}
				Figure {
					share: 0.55
					label: "Downloaded"
					value: root.service.hasStats ? Format.formatBytes(root.service.rxTotal)
						: "--"
				}
				Figure {
					share: 0.45
					label: "Uploaded"
					value: root.service.hasStats ? Format.formatBytes(root.service.txTotal)
						: "--"
				}
				Figure {
					share: 0.55
					label: "IP Address"
					value: root.service.ipAddress !== "" ? root.service.ipAddress : "--"
					copyable: root.service.ipAddress !== ""
				}
				Figure {
					share: 0.45
					label: "Gateway"
					value: root.service.gateway !== "" ? root.service.gateway : "--"
					copyable: root.service.gateway !== ""
				}
			}

			BandSection {
				visible: root.wifi != null && root.connected
				width: parent.width
			}

			DnsSection {
				visible: root.service.profile !== ""
				width: parent.width
				open: root.open
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
						glyph: NetworkState.wifiGlyph(NetworkState.percent(network))
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

	ShareView {
		visible: root.service.sharing
		width: parent.width
	}

	SpeedView {
		visible: root.service.speedResult != null
		width: parent.width
	}

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
