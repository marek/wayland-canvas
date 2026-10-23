import QtQuick
import WaylandCanvas.Ui
import "."

// The Shortcuts tab: the file's complaints, every action's row grouped as
// settings.get lists them, and the commands. The list scrolls: the rows
// together are taller than an output.
Flickable {
	id: root

	required property var model // the SettingsModel
	required property real maxHeight // the most the list may take

	width: parent.width
	implicitHeight: Math.min(contentHeight, maxHeight)
	height: implicitHeight
	contentWidth: width
	contentHeight: column.implicitHeight
	clip: true

	// The groups in the order settings.get lists them (overview last), each
	// with its rows.
	function groups() {
		var out = []
		var byName = ({})
		var shortcuts = model.settings && model.settings.shortcuts ? model.settings.shortcuts : []
		for (var i = 0; i < shortcuts.length; i++) {
			var entry = shortcuts[i]
			if (byName[entry.group] == null) {
				byName[entry.group] = {name: entry.group, rows: []}
				out.push(byName[entry.group])
			}
			byName[entry.group].rows.push(entry)
		}
		return out
	}

	// The tab's first lines: the file's parse errors, then the socket's
	// own fault — one list, so a quiet file shows nothing.
	function errorLines() {
		var out = []
		if (model.settingsError !== "") {
			out.push({line: 0, text: model.settingsError})
		}
		var errors = model.settings && model.settings.errors ? model.settings.errors : []
		for (var i = 0; i < errors.length; i++) {
			out.push(errors[i])
		}
		return out
	}

	Column {
		id: column
		width: root.width
		spacing: UiTheme.panelSpacing

		Repeater {
			model: root.errorLines()

			PanelText {
				required property var modelData
				height: UiTheme.detailHeight
				verticalAlignment: Text.AlignVCenter
				width: parent.width
				elide: Text.ElideRight
				color: UiTheme.attention
				text: modelData.line > 0
					? "canvas.conf line " + modelData.line + ": " + modelData.text
					: modelData.text
			}
		}

		Repeater {
			model: root.groups()

			Column {
				id: group
				required property var modelData
				width: parent.width
				spacing: UiTheme.panelSpacing

				PanelSectionHeader {
					text: group.modelData.name
				}

				Repeater {
					model: group.modelData.rows

					ShortcutRow {
						id: actionRow
						required property var modelData
						entry: modelData
						recording: root.model.recording != null
							&& root.model.recording.scope === modelData.scope
							&& root.model.recording.action === modelData.action
						note: root.model.rowNote(modelData)
						fault: root.model.rowFault(modelData)
						onRecord: root.model.capture(modelData.scope, modelData.action,
							function(keys, fault) {
								if (fault !== "") {
									root.model.fail({scope: modelData.scope,
										action: modelData.action}, fault)
									return
								}
								if (keys != null) {
									root.model.setKeys(modelData.scope, modelData.action, [keys])
								}
							})
						onReset: root.model.setKeys(modelData.scope, modelData.action, null)
						onClear: root.model.setKeys(modelData.scope, modelData.action, [])
					}
				}
			}
		}

		PanelSectionHeader {
			text: "Commands"
		}

		Repeater {
			model: root.model.commands()

			CommandRow {
				id: commandRow
				required property var modelData
				required property int index
				command: modelData
				draft: root.model.draft != null && index === root.model.commandCount()
				recording: root.model.recording != null
					&& root.model.recording.action === "command" + index
				fault: root.model.faultRow != null && root.model.faultRow.scope === "commands"
					? root.model.faultText : ""
				onRecord: function(text) {
					var at = commandRow.index
					root.model.holdDraftText(at, text)
					root.model.capture("global", "command" + at, function(keys, fault) {
						if (fault !== "") {
							root.model.fail({scope: "commands"}, fault)
							return
						}
						if (keys != null) {
							root.model.commandKeys(at, keys)
						}
					})
				}
				onSave: function(text) { root.model.commandText(commandRow.index, text) }
				onRemove: root.model.removeCommand(commandRow.index)
			}
		}

		PanelRow {
			glyph: "+"
			label: "Add a command"
			onClicked: root.model.draft = {keys: "", command: ""}
		}
	}
}
