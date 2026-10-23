import QtQuick
import WaylandCanvas.Ui

// The Gestures tab: which swipe preset hops and which opens the overview,
// the pinch, and the direction switches (docs/intents/input/gestures.md).
Column {
	id: root

	required property var model // the SettingsModel

	width: parent.width
	spacing: UiTheme.panelSpacing

	readonly property var gestures: model.settings ? model.settings.gestures : null

	readonly property string swipes: gestures && gestures.swipes ? gestures.swipes : ""

	readonly property bool pinchScale: !gestures || gestures.pinchScale !== false

	// Whether a direction goes `way`; before the settings arrive, its default does.
	function gestureWay(param, way, isDefault) {
		if (!(gestures && gestures[param] !== undefined)) {
			return isDefault
		}
		return gestures[param] === way
	}

	// A switch each, on meaning `onWay`, with a line saying which way it
	// moves now.
	readonly property var directions: [
		{param: "mouseScroll", onWay: "natural", offWay: "traditional", onByDefault: true,
			label: "Natural scrolling with a mouse",
			onLine: "The content moves the way the wheel turns",
			offLine: "The content moves against the wheel"},
		{param: "touchpadScroll", onWay: "natural", offWay: "traditional", onByDefault: true,
			label: "Natural scrolling on the touchpad",
			onLine: "The content follows the fingers",
			offLine: "The content moves against the fingers"},
		{param: "pan", onWay: "natural", offWay: "traditional", onByDefault: true,
			label: "Natural looking away",
			onLine: "Super + scroll moves the content the way you scroll",
			offLine: "Super + scroll moves the view the way you scroll"},
		{param: "overviewSwipe", onWay: "down", offWay: "up", onByDefault: false,
			label: "Open the overview swiping down",
			onLine: "Swiping up goes back in",
			offLine: "Swiping up opens it, swiping down goes back in"},
		{param: "hopSwipe", onWay: "away", offWay: "toward", onByDefault: false,
			label: "Swipes push the desktops along",
			onLine: "A swipe left hops to the desktop on the right",
			offLine: "A swipe left hops to the desktop on the left"}
	]

	PanelSectionHeader {
		text: "Swipes"
	}

	ChoiceRow {
		label: "Three-finger hops"
		description: "3 fingers hop, 4 open the overview"
		chosen: root.swipes === "three-finger-hops"
		note: root.model.fieldFault("swipes")
		onPicked: root.model.setField("swipes", "three-finger-hops")
	}

	ChoiceRow {
		label: "Four-finger hops"
		description: "4 fingers hop, 3 open the overview"
		chosen: root.swipes === "four-finger-hops"
		note: root.model.fieldFault("swipes")
		onPicked: root.model.setField("swipes", "four-finger-hops")
	}

	ChoiceRow {
		label: "Off"
		description: "No 3- or 4-finger compositor gesture"
		chosen: root.swipes === "off"
		note: root.model.fieldFault("swipes")
		onPicked: root.model.setField("swipes", "off")
	}

	PanelSectionHeader {
		text: "Pinch"
	}

	PanelSwitchRow {
		label: "Pinch to scale a desktop"
		detail: "Super + 2 fingers"
		clickable: false
		checked: root.pinchScale
		onToggled: root.model.setField("pinchScale", !root.pinchScale)
	}

	PanelSectionHeader {
		text: "Direction"
	}

	Repeater {
		model: root.directions

		delegate: PanelSwitchRow {
			required property var modelData
			readonly property bool switchedOn: root.gestureWay(modelData.param,
				modelData.onWay, modelData.onByDefault)
			readonly property string fault: root.model.fieldFault(modelData.param)
			readonly property bool failed: fault !== ""

			label: modelData.label
			// A write that failed says why in place of the line.
			detail: failed ? fault
				: switchedOn ? modelData.onLine : modelData.offLine
			marked: failed
			clickable: false
			checked: switchedOn
			onToggled: root.model.setField(modelData.param,
				switchedOn ? modelData.offWay : modelData.onWay)
		}
	}
}
