// Everything before the first step: the machine's environment saved, the private buses, the stand-ins
// and the scratch plugin world the shell host loads from.
#include "scenarios/bar/world.h"

namespace bar {

void remember_machine_environment() {
	// the machine's environment, saved before the run's isolation replaces it: the
	// theme phase reads the real color-scheme through a child with it restored
	for (SavedEnv &saved : saved_env) {
		const char *value = std::getenv(saved.name);
		saved.set = value != nullptr && *value != '\0';
		if (saved.set) {
			saved.value = value;
		}
	}
}

bool bring_up(std::string_view run_name) {
	shot_dir = harness::scratch();
	// The stand-ins log and keep their state in the run's own folder.
	setenv("CANVAS_BAR_DIR", shot_dir.c_str(), 1);
	// One folder per run, so runs of different phases side by side keep
	// their buses apart; a hash keeps the socket paths short.
	bus_dir = std::format("{}/bb-{:08x}", harness::tmp_root(),
		std::hash<std::string_view>{}(run_name) & 0xffffffffU);
	setenv("GSETTINGS_BACKEND", "keyfile", 1);
	// The terminal action runs `exec ${TERMINAL:-foot}`: pinned to foot, the
	// settings phase's terminal check never depends on the user's own pick.
	setenv("TERMINAL", "foot", 1);
	// Isolation first: the bar may only ever start on the private buses
	// with their stand-ins — when either cannot come up, no bar runs at
	// all, and the machine's buses are never touched. The stand-ins and
	// every later child (the bar above all) inherit the addresses.
	if (!start_private_bus(private_system, "system", "y")
			|| !start_private_bus(private_session, "session", "n")) {
		std::println("FAILED: the private buses did not start; the bar did not run");
		return false;
	}
	// Python children must never drop bytecode into the source tree
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
	setenv("DBUS_SYSTEM_BUS_ADDRESS", private_system.address.c_str(), 1);
	setenv("DBUS_SESSION_BUS_ADDRESS", private_session.address.c_str(), 1);
	if (!start_standins()) {
		std::println("FAILED: the stand-in services did not start; the bar did not run");
		return false;
	}
	if (!start_curl_standin()) {
		std::println("FAILED: the curl stand-in did not start; the bar did not run");
		return false;
	}
	if (!start_tool_standins()) {
		std::println("FAILED: the tool stand-ins did not start; the bar did not run");
		return false;
	}
	if (!start_pipewire_standin()) {
		std::println("FAILED: the PipeWire stand-in did not start; the bar did not run");
		return false;
	}
	// the plugin world the shell host loads from, in this run's scratch XDG dirs; a
	// broken manifest is there from the start and the shell must skip it
	std::string xdg = shot_dir + "/xdg";
	// A previous run's leftovers (a dropped-in plugin, a written config)
	// would leak into this one: the scratch world starts empty.
	std::error_code ec;
	std::filesystem::remove_all(xdg, ec);
	std::string plugins_dir = xdg + "/data/wayland-canvas/plugins";
	mkdir_p(xdg + "/config/wayland-canvas");
	mkdir_p(plugins_dir + "/bad.plugin");
	mkdir_p(xdg + "/state");
	write_file(plugins_dir + "/bad.plugin/plugin.json", "{ not json\n");
	write_file(plugins_dir + "/bad.plugin/Bad.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\nBarButton {\n"
		"\ticon: true\n\ttext: \"\\uf00d\"\n}\n");
	// the toolbar phase's scratch plugin: an action button, a panel button and a menu
	// button, whose action appends its args to the plugin's storage, and two invalid
	// buttons the host drops
	std::string scratch_plugin = plugins_dir + "/canvas.scratch";
	mkdir_p(scratch_plugin);
	write_file(scratch_plugin + "/plugin.json",
		"{\n\t\"id\": \"canvas.scratch\",\n\t\"name\": \"Scratch\",\n"
		"\t\"version\": \"1.0.0\",\n\t\"apiVersion\": 1,\n"
		"\t\"description\": \"The toolbar scenario's plugin: an action, a panel and a menu button\",\n"
		"\t\"contributes\": {\n"
		"\t\t\"toolbarButtons\": [\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"act\",\n\t\t\t\t\"icon\": \"A\",\n"
		"\t\t\t\t\"label\": \"Act\",\n\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\"args\": {\"kind\": \"button\"},\n\t\t\t\t\"order\": 0\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"panel\",\n\t\t\t\t\"icon\": \"P\",\n"
		"\t\t\t\t\"label\": \"Panel\",\n\t\t\t\t\"panel\": \"scratch\",\n"
		"\t\t\t\t\"order\": 1\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"menu\",\n\t\t\t\t\"icon\": \"M\",\n"
		"\t\t\t\t\"label\": \"Menu\",\n\t\t\t\t\"menu\": [\n"
		"\t\t\t\t\t{\n\t\t\t\t\t\t\"label\": \"One\",\n\t\t\t\t\t\t\"icon\": \"1\",\n"
		"\t\t\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\t\t\"args\": {\"kind\": \"menu-one\"}\n\t\t\t\t\t},\n"
		"\t\t\t\t\t{\n\t\t\t\t\t\t\"label\": \"Yellow\",\n\t\t\t\t\t\t\"color\": \"#e0af68\",\n"
		"\t\t\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\t\t\"args\": {\"kind\": \"menu-yellow\"}\n\t\t\t\t\t}\n"
		"\t\t\t\t],\n\t\t\t\t\"order\": 2\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"both\",\n\t\t\t\t\"icon\": \"B\",\n"
		"\t\t\t\t\"label\": \"Both\",\n\t\t\t\t\"action\": \"mark\",\n"
		"\t\t\t\t\"panel\": \"scratch\",\n\t\t\t\t\"order\": 3\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"none\",\n\t\t\t\t\"icon\": \"N\",\n"
		"\t\t\t\t\"label\": \"None\",\n\t\t\t\t\"order\": 4\n\t\t\t}\n"
		"\t\t],\n"
		"\t\t\"panels\": [\n\t\t\t{\n\t\t\t\t\"id\": \"scratch\",\n"
		"\t\t\t\t\"entry\": \"ScratchPanel.qml\"\n\t\t\t}\n\t\t],\n"
		"\t\t\"actions\": [\n\t\t\t{\n\t\t\t\t\"id\": \"mark\",\n"
		"\t\t\t\t\"entry\": \"Mark.qml\"\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"open\",\n"
		"\t\t\t\t\"entry\": \"Open.qml\"\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"write\",\n"
		"\t\t\t\t\"entry\": \"Write.qml\"\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"wait\",\n"
		"\t\t\t\t\"entry\": \"Wait.qml\"\n\t\t\t}\n\t\t]\n\t}\n}\n");
	write_file(scratch_plugin + "/Mark.qml",
		"import QtQuick\n// The scratch action: appends its args to the plugin's\n"
		"// storage, so the scenario reads back that it ran and what with.\n"
		"Item {\n\tproperty var api: null\n"
	"\tfunction run(args) {\n"
		"\t\tvar marks = api.storage.read(\"marks\")\n"
		"\t\tif (marks == null) {\n\t\t\tmarks = []\n\t\t}\n"
		"\t\tmarks.push(args == null ? {} : args)\n"
		"\t\tapi.storage.write(\"marks\", marks)\n\t}\n}\n");
	write_file(scratch_plugin + "/Write.qml",
		"import QtQuick\n// The scratch write action: one of the shell's own\n"
		"// writes through api.shell - an edge for the toolbar, or a widget\n"
		"// hidden or shown - so the scenario can make them with a panel open.\n"
		"Item {\n\tproperty var api: null\n"
		"\tfunction run(args) {\n"
		"\t\tif (args.edge !== undefined) {\n"
		"\t\t\tapi.shell.setToolbarPosition(args.edge)\n"
		"\t\t} else {\n"
		"\t\t\tapi.shell.setBarHidden(args.hide, args.hidden)\n"
		"\t\t}\n\t}\n}\n");
	write_file(scratch_plugin + "/Wait.qml",
		"import QtQuick\n// The scratch wait action: three calls to the compositor - one it\n"
		"// answers, one it refuses, one it answers only when a chord is\n"
		"// pressed - each answer written to storage as it comes, so the\n"
		"// scenario reads back what the callbacks were given. The host\n"
		"// destroys an action's item once run() returns: the callbacks keep\n"
		"// to what run() captured, never to the item.\n"
		"Item {\n\tproperty var api: null\n"
		"\tfunction run(args) {\n"
		"\t\tvar answers = []\n"
		"\t\tvar compositor = api.compositor\n"
		"\t\tvar storage = api.storage\n"
		"\t\tvar ask = function(call, params) {\n"
		"\t\t\tcompositor.call(call, params, function(result, error) {\n"
		"\t\t\t\tanswers.push({call: call, result: result, error: error})\n"
		"\t\t\t\tstorage.write(\"answers\", answers)\n"
		"\t\t\t})\n\t\t}\n"
		"\t\task(\"subscribe\")\n"
		"\t\task(\"capture.stop\")\n"
		"\t\task(\"shortcut.capture\", {scope: \"global\"})\n"
		"\t}\n}\n");
	write_file(scratch_plugin + "/ScratchPanel.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\n"
		"// The scratch panel: two labelled rows, so the scenario finds the\n"
		"// card where it should be on the screenshot.\n"
		"PanelCard {\n\tpanelWidth: 200\n"
		"\tPanelRow { glyph: \"1\"; label: \"Scratch one\"; clickable: false }\n"
		"\tPanelRow { glyph: \"2\"; label: \"Scratch two\"; clickable: false }\n}\n");
	write_file(scratch_plugin + "/Open.qml",
		"import QtQuick\n// The scratch open action: opens a panel by its global name\n"
		"// through api.panels, with no widget or button to open it from, so the\n"
		"// scenario can see the panel read its state from its plugin.\n"
		"Item {\n\tproperty var api: null\n"
		"\tfunction run(args) {\n"
		"\t\tapi.panels.open(args.panel, null)\n"
		"\t}\n}\n");
	// A plugin whose second panel cannot be made (a required property
	// nobody gives): the host skips it whole, so its first panel, which
	// made fine, is not there to open.
	std::string halfway_plugin = plugins_dir + "/canvas.halfway";
	mkdir_p(halfway_plugin);
	write_file(halfway_plugin + "/plugin.json",
		"{\n\t\"id\": \"canvas.halfway\",\n\t\"name\": \"Halfway\",\n"
		"\t\"version\": \"1.0.0\",\n\t\"apiVersion\": 1,\n"
		"\t\"contributes\": {\n\t\t\"panels\": [\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"one\",\n\t\t\t\t\"entry\": \"One.qml\"\n\t\t\t},\n"
		"\t\t\t{\n\t\t\t\t\"id\": \"two\",\n\t\t\t\t\"entry\": \"Two.qml\"\n\t\t\t}\n"
		"\t\t]\n\t}\n}\n");
	write_file(halfway_plugin + "/One.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\n"
		"PanelCard {\n\tpanelWidth: 200\n"
		"\tPanelRow { glyph: \"1\"; label: \"Halfway one\"; clickable: false }\n}\n");
	write_file(halfway_plugin + "/Two.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\n"
		"PanelCard {\n\trequired property string nobodyGivesThis\n}\n");

	setenv("XDG_CONFIG_HOME", (xdg + "/config").c_str(), 1);
	setenv("XDG_DATA_HOME", (xdg + "/data").c_str(), 1);
	setenv("XDG_STATE_HOME", (xdg + "/state").c_str(), 1);
	// The scratch config the theme switch writes theme.mode into (and the
	// theme phase reads back and rewrites to prove the reload keeps it).
	shell_config = xdg + "/config/wayland-canvas/shell.json";
	// The compositor's own config, written on its first run into the
	// same scratch: the settings phase reads what its writes put there.
	canvas_conf = xdg + "/config/wayland-canvas/canvas.conf";
	// the user plugin the plugins phase drops in while the shell runs: a bar widget past
	// the logo, staged outside the plugin root until then
	user_plugin = plugins_dir + "/canvas.hello";
	staged_plugin = xdg + "/staged";
	mkdir_p(staged_plugin);
	write_file(staged_plugin + "/plugin.json",
		"{\n\t\"id\": \"canvas.hello\",\n\t\"name\": \"Hello\",\n"
		"\t\"version\": \"1.0.0\",\n\t\"apiVersion\": 1,\n"
		"\t\"description\": \"A user plugin: a bar widget that says so\",\n"
		"\t\"contributes\": {\n\t\t\"barWidgets\": [\n\t\t\t{\n"
		"\t\t\t\t\"id\": \"hello\",\n\t\t\t\t\"entry\": \"Hello.qml\",\n"
		"\t\t\t\t\"section\": \"left\",\n\t\t\t\t\"order\": 1\n\t\t\t}\n\t\t]\n\t}\n}\n");
	write_file(staged_plugin + "/Hello.qml",
		"import QtQuick\nimport WaylandCanvas.Ui\nBarButton {\n"
		"\ticon: true\n\ttext: \"\\uf120\"\n}\n");

	// the agents panel's world: a scratch HOME for the collector, a fake codex app-server
	// first on the PATH, the usage endpoint at a loopback stand-in; the fixtures land in
	// the agents phase, until then the panel is empty and its widget hidden
	scratch_home = shot_dir + "/home";
	mkdir_p(scratch_home);
	// A run that died inside the agents phase leaves its fixture world
	// behind, and a stale agents widget would throw off every run count
	// after it: each run starts from the quiet world, its stand-ins' logs
	// empty, so the proofs at the end read only this run's traffic.
	remove_usage_fixtures();
	for (const char *log :
			{"usage-requests.log", "codex-standin.log", "curl-standin.log"}) {
		write_file(std::format("{}/{}", shot_dir, log), "");
	}
	unsetenv("CLAUDE_CONFIG_DIR");
	unsetenv("CODEX_HOME");
	setenv("HOME", scratch_home.c_str(), 1);
	std::string tool_bin = shot_dir + "/tool-bin";
	mkdir_p(tool_bin);
	write_file(tool_bin + "/codex",
		"#!/usr/bin/env python3\n"
		"# Generated by tests/scenarios/bar/bring_up.cpp: the codex app-server\n"
		"# stand-in. It answers the JSON-RPC handshake and the account\n"
		"# reads the usage collector makes; account/rateLimits/read is\n"
		"# refused — the signed-out answer — so the GPT tab shows its\n"
		"# sign-in card. Every method is logged.\n"
		"import json, os, sys\n"
		"log = os.path.join(os.environ['CANVAS_BAR_DIR'], 'codex-standin.log')\n"
		"def reply(obj):\n"
		"    sys.stdout.write(json.dumps(obj) + '\\n')\n"
		"    sys.stdout.flush()\n"
		"for line in sys.stdin:\n"
		"    line = line.strip()\n"
		"    if not line:\n"
		"        continue\n"
		"    with open(log, 'a') as out:\n"
		"        out.write(line + '\\n')\n"
		"    try:\n"
		"        req = json.loads(line)\n"
		"    except ValueError:\n"
		"        continue\n"
		"    if 'id' not in req:\n"
		"        continue\n"
		"    if req.get('method') == 'initialize':\n"
		"        reply({'id': req['id'], 'result': {'info':\n"
		"            {'name': 'codex', 'version': 'standin'}}})\n"
		"    elif req.get('method') == 'account/read':\n"
		"        reply({'id': req['id'], 'result':\n"
		"            {'account': {'planType': 'pro'}}})\n"
		"    elif req.get('method') == 'account/rateLimits/read':\n"
		"        reply({'id': req['id'], 'error': {'message': 'Not signed in'}})\n"
		"    else:\n"
		"        reply({'id': req['id'], 'result': {}})\n");
	chmod((tool_bin + "/codex").c_str(), 0755);
	{
		const char *path = std::getenv("PATH");
		setenv("PATH", std::format("{}:{}", tool_bin,
			path != nullptr ? path : "").c_str(), 1);
	}
	write_usage_payload("37.0", "68.4");
	const char *py = std::getenv("CANVAS_BAR_STANDIN_PY");
	const char *standins = std::getenv("CANVAS_BAR_STANDINS");
	std::string agent_script = standins != nullptr && *standins != '\0'
		? (std::filesystem::path(standins).parent_path()
			/ "bar-standins/agent-usage.py").string() : std::string{};
	if (py == nullptr || *py == '\0' || agent_script.empty()) {
		std::println("FAILED: the usage stand-in environment is missing; "
			"the bar did not run");
		return false;
	}
	const char *const standin_argv[] = {py, agent_script.c_str(),
		static_cast<const char *>(nullptr)};
	Spawned standin_child = spawn(standin_argv);
	std::string ready = standin_child.pid > 0
		? ready_line(standin_child, 30) : std::string{};
	if (!ready.starts_with("READY ")) {
		std::println("FAILED: the usage endpoint stand-in did not become "
			"ready; the bar did not run");
		reap(standin_child.pid);
		return false;
	}
	usage_standin_pid = standin_child.pid;
	close(standin_child.out);
	setenv("WAYLAND_CANVAS_CLAUDE_USAGE_URL",
		std::format("http://127.0.0.1:{}/usage",
			ready.substr(6)).c_str(), 1);

	return true;
}

void watch_bar_surfaces() {
	bar_surface_counter.notify = [](wl_listener *, void *data) {
		const auto *made = static_cast<wlr_layer_surface_v1 *>(data);
		if (made->namespace_ != nullptr && std::strcmp(made->namespace_, bar_namespace) == 0) {
			bar_surfaces_made++;
		}
	};
	wl_list_insert(&server->layer_shell.wlr()->events.new_surface.listener_list,
		&bar_surface_counter.link);
}

void tear_down() {
	wl_list_remove(&bar_surface_counter.link);
}
} // namespace bar
