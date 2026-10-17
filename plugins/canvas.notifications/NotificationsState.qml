pragma Singleton

import QtQuick
import Quickshell.Services.Notifications

// The notification server and what it leaves behind: the popups on screen,
// the history the bar widget's panel lists, and the silence switch, which
// is kept in the plugin's storage. Another server already holding
// org.freedesktop.Notifications leaves this one without the name: the
// shell runs on, with no popups of its own
// (docs/intents/shell/notifications.md).
Item {
	id: root

	// {id, appName, summary, body, urgency, image, appIcon, actions:[{id,text}],
	//  time} — popups newest first, history newest first.
	property var popups: []
	property var history: []
	property bool silent: false

	property var api: null
	property int popupSeconds: 5
	property bool criticalNeverExpires: true
	property int maxPopups: 4
	property int historyLimit: 50
	// The live Notification objects by id, kept out of the lists: a model
	// holding one dangles as soon as the sender closes it.
	property var live: ({})

	readonly property string storageName: "notifications.json"

	function init(pluginApi) {
		if (api !== null) {
			return
		}
		api = pluginApi
		var settings = api.plugin.settings
		popupSeconds = settings.popupSeconds
		criticalNeverExpires = settings.criticalNeverExpires
		maxPopups = settings.maxPopups
		historyLimit = settings.historyLimit
		var stored = api.storage.read(storageName)
		if (stored != null) {
			silent = stored.silent === true
			history = stored.history != null ? stored.history : []
		}
	}

	function save() {
		if (api === null) {
			return
		}
		api.storage.write(storageName, {silent: silent, history: history})
	}

	function urgencyName(urgency) {
		if (urgency === NotificationUrgency.Critical) {
			return "critical"
		}
		if (urgency === NotificationUrgency.Low) {
			return "low"
		}
		return "normal"
	}

	// How long a popup stays: the sender's own timeout when it set one,
	// else the setting; a critical notification waits for the user.
	function lifetime(entry, expireTimeout) {
		if (entry.urgency === "critical" && criticalNeverExpires) {
			return 0
		}
		if (expireTimeout > 0) {
			return expireTimeout
		}
		return popupSeconds * 1000
	}

	function add(notification) {
		var actions = []
		for (var i = 0; i < notification.actions.length; i++) {
			var action = notification.actions[i]
			actions.push({id: action.identifier, text: action.text})
		}
		var entry = {
			id: String(notification.id),
			appName: notification.appName,
			summary: notification.summary,
			body: notification.body,
			urgency: urgencyName(notification.urgency),
			image: notification.image,
			appIcon: notification.appIcon,
			actions: actions,
			time: Date.now()
		}
		live[entry.id] = notification
		remember(entry)
		if (silent) {
			return
		}
		var next = [entry].concat(popups)
		while (next.length > maxPopups) {
			next.pop()
		}
		popups = next
	}

	function remember(entry) {
		var next = [entry].concat(history)
		while (next.length > historyLimit) {
			next.pop()
		}
		history = next
		save()
	}

	// A popup leaves the screen; the history keeps it.
	function dismiss(id) {
		var next = []
		for (var i = 0; i < popups.length; i++) {
			if (popups[i].id !== id) {
				next.push(popups[i])
			}
		}
		popups = next
	}

	function dismissNewest() {
		if (popups.length > 0) {
			dismiss(popups[0].id)
		}
	}

	function dismissAll() {
		popups = []
	}

	function invoke(id, actionId) {
		var notification = live[id]
		dismiss(id)
		if (notification == null) {
			return
		}
		for (var i = 0; i < notification.actions.length; i++) {
			if (notification.actions[i].identifier === actionId) {
				notification.actions[i].invoke()
				return
			}
		}
	}

	function forget(id) {
		delete live[id]
		dismiss(id)
	}

	function clearHistory() {
		history = []
		save()
	}

	function setSilent(value) {
		silent = value
		if (silent) {
			dismissAll()
		}
		save()
	}
}
