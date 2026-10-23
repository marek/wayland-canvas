import QtQuick
import WaylandCanvas.Ui
import "."

// The clock panel: a month calendar with today marked. ‹ and › step the
// month; it is a read-out, not a picker.
PanelCard {
	id: root

	panelWidth: 220

	readonly property date today: ClockState.now

	property int viewYear: today.getFullYear()
	property int viewMonth: today.getMonth()

	readonly property bool viewingToday: viewYear === today.getFullYear()
		&& viewMonth === today.getMonth()

	// The day-of-week column of the month's first day, Monday-first.
	readonly property int firstOffset: (new Date(viewYear, viewMonth, 1).getDay() + 6) % 7

	readonly property int cellWidth: Math.floor(
		(panelWidth - 2 * UiTheme.panelPadding) / 7)
	readonly property string monthName: Qt.formatDateTime(
		new Date(viewYear, viewMonth, 1), "MMMM yyyy")

	function dayAt(index) {
		return new Date(viewYear, viewMonth, 1 - firstOffset + index)
	}

	function isToday(date) {
		return viewingToday && date.getFullYear() === today.getFullYear()
			&& date.getMonth() === today.getMonth()
			&& date.getDate() === today.getDate()
	}

	function stepMonth(by) {
		var month = new Date(viewYear, viewMonth + by, 1)
		viewYear = month.getFullYear()
		viewMonth = month.getMonth()
	}

	// Month stepper: ‹ name ›
	Item {
		width: parent.width
		height: UiTheme.rowHeight

		PanelButton {
			kind: "flat"
			anchors.left: parent.left
			anchors.verticalCenter: parent.verticalCenter
			text: "‹"
			onClicked: root.stepMonth(-1)
		}

		PanelText {
			anchors.centerIn: parent
			text: root.monthName
		}

		PanelButton {
			kind: "flat"
			anchors.right: parent.right
			anchors.verticalCenter: parent.verticalCenter
			text: "›"
			onClicked: root.stepMonth(1)
		}
	}

	// Weekday initials, Monday-first.
	Row {
		Repeater {
			model: ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]

			PanelText {
				required property var modelData
				width: root.cellWidth
				horizontalAlignment: Text.AlignHCenter
				dim: true
				text: modelData
			}
		}
	}

	// Six week rows cover every month.
	Grid {
		columns: 7

		Repeater {
			model: 42

			Item {
				id: cell
				required property int index
				readonly property date date: root.dayAt(index)
				width: root.cellWidth
				height: UiTheme.rowHeight

				Rectangle {
					visible: root.isToday(cell.date)
					anchors.centerIn: parent
					width: 20
					height: 20
					radius: 10
					color: UiTheme.accent
				}

				PanelText {
					anchors.centerIn: parent
					text: cell.date.getDate()
					color: {
						if (root.isToday(cell.date)) {
							return UiTheme.onAccent
						}
						return cell.date.getMonth() === root.viewMonth
							? UiTheme.text
							: UiTheme.muted
					}
				}
			}
		}
	}
}
