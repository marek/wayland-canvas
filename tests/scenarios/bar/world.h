#pragma once

// What the bar scenario's phases share: the scenario's state, its stand-in services and the
// helpers that read the screen, the bar's surfaces and the notes. A phase (one file each) is a list
// of named steps; bring_up() is everything every phase needs before its first step.
#include <cairo.h>
#include <json-glib/json-glib.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <format>
#include <functional>
#include <linux/input-event-codes.h>
#include <optional>
#include <poll.h>
#include <print>
#include <ranges>
#include <regex>
#include <source_location>
#include <span>
#include <string>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "apps/capture_toolbar_layout.h"
#include "core/config.h"
#include "platform/output.h"
#include "core/server.h"
#include "core/process.h"
#include "core/theme.h"
#include "core/wlr.h"
#include "canvas/overview_chrome.h"
#include "harness/check.h"
#include "harness/devices.h"
#include "harness/files.h"
#include "harness/image.h"
#include "harness/layout.h"
#include "harness/run.h"
#include "harness/steps.h"

namespace bar {

using harness::act_when;
using harness::after;
using harness::again;
using harness::bar_height;
using harness::bar_mid_y;
using harness::button;
using harness::check;
using harness::check_near;
using harness::click;
using harness::chord;
using harness::finish;
using harness::go_to;
using harness::key;
using harness::mkdir_p;
using harness::move_to;
using harness::polls;
using harness::server;
using harness::shot;
using harness::tap;
using harness::type_text;
using harness::wait_for;
using harness::wheel;
using harness::write_file;
using harness::Result;
using harness::Wait;
using harness::screen_w;
using harness::screen_h;
namespace mods = harness::mods;

extern std::string shot_dir;

extern std::string user_plugin; // the canvas.hello plugin the plugins phase drops in

extern std::string staged_plugin; // where its files wait until the drop-in

extern pid_t bar_pid; // the shell client, for the idle spawn watch

inline constexpr double widen_px = 20; // how much the edited widget must grow

extern double theme_x; // the theme switch's centre: the right-most bar run

extern double logo_x; // the logo's centre: the left-most bar run

extern std::size_t bar_run_count; // the runs on the bar once bring-up has it drawn

extern double power_x; // the power indicator's centre: the theme phase's panel

extern std::string shell_config; // the scratch shell.json the theme switch writes

// The plugin's settings defaults, kept in sync with
// plugins/canvas.notes/plugin.json (the swatch colours the test creates
// and looks for on screen).
inline constexpr const char *note_yellow = "#ffd866";

// A note as its file holds it (docs/intents/shell/notes.md): the
// rectangle, the colour, font and size, and the text's HTML.
struct NoteEntry {
	std::string id, color, text, font;
	long long x = 0, y = 0, width = 0, height = 0, size = 0;
};

extern NoteEntry note; // the note under test

// The window the app bar's clicked entry stands for, kept between the
// click and the check that the view landed on it.
extern View *app_bar_target;

extern wlr_box app_bar_entry; // its entry's square, for the flyout's rows

extern std::vector<pid_t> msg_children; // `wayland-canvas msg` runs, swept when done

extern pid_t usage_standin_pid;

extern double agents_x; // the widget's run centre, once it shows

extern double bt_x; // the bluetooth indicator's run centre, to find it

extern int agents_runs_before; // the collector's runs before it did

extern std::string scratch_home;

extern std::string canvas_conf; // the scratch canvas.conf the compositor owns

// Bar surfaces the shell has made, counted as wlroots announces each: a
// reload makes a new one. The count at the last watch_remember_bar().
extern int bar_surfaces_made;

extern int bar_surfaces_remembered;

extern wl_listener bar_surface_counter;

extern double settings_gear_y; // the gear button's band centre, on its edge

extern int settings_card_x;

struct PrivateBus {
	pid_t pid = -1;
	std::string address; // what DBUS_*_BUS_ADDRESS becomes for the bar
};

extern PrivateBus private_session; // the world's own buses

extern PrivateBus private_system;

extern std::string bus_dir; // where their sockets live: a socket path is capped near 108 bytes

// The panel widths shell/Panel.qml commits, one distinct width per panel so
// a panel can be told apart by its surface. Keep in sync with the panels.
inline constexpr int network_panel = 340;

inline constexpr int weather_panel = 320;

inline constexpr int bluetooth_panel = 300;

inline constexpr int power_panel = 260;

inline constexpr int brightness_panel = 240;

inline constexpr int clock_panel = 220;

inline constexpr int agents_panel = 360;

inline constexpr const char *bar_namespace = "wayland-canvas-bar";

// The gap the shared strip leaves between the toolbar's buttons and the
// apps, the separator drawn in it (shell/ui/WaylandCanvas/Ui/StripLayout.qml).
inline constexpr int strip_separator = 9;

// The shell's own colours the theme phase samples (the UI kit's palette;
// the compositor's half comes from config.h's palettes). Keep in sync with
// shell/ui/WaylandCanvas/Ui/UiTheme.qml.
inline constexpr uint32_t shell_bg_dark = 0x1a1b26ff;

inline constexpr uint32_t shell_bg_light = 0xe1e2e7ff;

// The toolbar strip the plugin commits (docs/intents/shell/toolbar.md): 44 px square
// buttons, 8 px from the right edge, centred vertically; the flyout and a panel it opens
// sit a further 8 px left, 200 px wide.
inline constexpr int toolbar_button = 44;

inline constexpr int toolbar_margin = 8;

inline constexpr int strip_left = screen_w - toolbar_margin - toolbar_button;

inline constexpr int beside_gap = toolbar_margin;

inline constexpr int scratch_card = 200;

enum CloseMode { close_outside, close_escape, close_icon };

struct Indicator {
	const char *name;
	int width;
	double x = 0; // centre of the bar run that opens it
	int button = BTN_LEFT;
	CloseMode close_mode = close_outside;
	bool connect_flow = false; // the Wi-Fi flow, ending in Connect/Disconnect
	bool bt_flow = false; // the paired/found device actions
	bool profile_flow = false; // the power profile switch
	bool wx_flow = false; // the forecast rows read against the fixture
};

extern std::vector<Indicator> indicators;

extern std::size_t current;

extern wlr_box popup_box; // the open panel's card, from the walk's phase 1

extern wlr_box field_box; // where the Wi-Fi password field ended up

extern double clock_x;

extern double network_x; // where the walk found the clock, network

extern std::size_t wifi_base; // the network panel's settled row count, baseline

extern int wifi_last_top; // its last row's top: a prompt under a row above

// Ends the run at a step that cannot go on, because a position it needs
// cannot be read: a FAIL line says why.
harness::Result end_run(std::string_view why, std::source_location where = std::source_location::current());

// Reads a small text file whole (the msg clients' captured output).
std::string read_file(const std::string &path);

// Replaces a file the way an editor's atomic save does: the new bytes in
// a sibling, renamed over the old name. The old inode goes away.
void replace_file(const std::string &path, std::string_view text);

// A forked child that keeps running: its stdout on a pipe, its death tied
// to the scenario process so nothing outlives a killed run.
struct Spawned {
	pid_t pid = -1;
	int out = -1; // its stdout, or -1 once drained
};

Spawned spawn(const char *const argv[]);

// The environment this run replaces, saved at start: a child that restores
// it reads — and only reads — the machine's own settings over its own bus.
struct SavedEnv {
	const char *name;
	bool set = false;
	std::string value;
};

inline SavedEnv saved_env[] = {
	{"DBUS_SESSION_BUS_ADDRESS"},
	{"DBUS_SYSTEM_BUS_ADDRESS"},
	{"XDG_CONFIG_HOME"},
	{"GSETTINGS_BACKEND"},
};

// An outside client's theme.set on the compositor's control socket, from a
// double-forked child (the compositor runs in this process, so nothing
// here may block its loop) — the way a user's script or keybinding would
// switch the compositor alone, for the shell to adopt.
void outside_theme_set(const char *mode);

// A dbus-daemon of our own on a private socket under tmp/: everything on
// it is allowed, it exists for this run only, and no machine service is
// reachable through it.
bool start_private_bus(PrivateBus &bus, const char *type, const char *name);

// The dbusmock stand-ins on the private buses (tests/scenarios/
// bar-standins.py). READY means their four system-bus names are up; the
// tray item registers with the bar's own watcher once the bar appears.
bool start_standins();

// Waits for a child up to two seconds, then kills it.
void reap(pid_t pid);

// The curl stand-in: the weather widget's wttr.in request is answered from a captured
// response (tests/scenarios/bar-standins/wttr-j1.json), anything else goes to the real
// curl, and every call is logged under the run's scratch.
bool start_curl_standin();

inline constexpr const char *wifi_ssid = "HomeNet";

inline constexpr const char *wifi_ip = "192.168.8.42";

inline constexpr const char *wifi_gateway = "192.168.8.1";

inline constexpr const char *wifi_psk = "correcthorse";

// The fixture world's connection state, as the nmcli stand-in reads it:
// the walk's disconnect takes it offline and the known join brings it
// back, so the panel's reads always agree with the mock's device state.
// The stand-in's key() keeps the last line, so appending is enough.
void fixture_online(bool on);

// The tool stand-ins: wrappers first on the bar's PATH answer nmcli, ping, qrencode,
// speedtest and brightnessctl from the fixture world, keeping their changes in state
// files so reads follow writes, pass wl-copy through, and log every call.
bool start_tool_standins();

inline constexpr double standin_sink_start = 0.5;

// The stand-in sink as `pw-dump` shows it: its object id and the volume of
// its first channel in the widget's scale. False when the daemon does not
// answer or the sink is not there yet.
bool pw_sink(int &id, double &volume);

bool start_pipewire_standin();

// The network panel's tool log, one line a call: what the panel asked of
// the fixture world's tools, and the proof it asked nothing of the real
// one.
std::string standin_log();

int standin_calls(std::string_view text, std::string_view needle);

// The network panel's row centres in card coordinates (the card's top is 0), as
// plugins/canvas.network/NetworkPanel.qml lays out its connected shape: 10 px padding,
// 6 spacing, 1 px separators. Keep in sync with the panel.
struct WifiRows {
	double hero;
	double figures[4];
	double band;
	double pills;
	double dns;
	double dns_pills;
	double dns_field;
	double known;
	double home;
	double oldcafe;
	double other;
	double cafe;
	double corp;
	double library;
};

WifiRows wifi_rows(bool pinned, bool editing);

// The same panel, disconnected: the head, then the networks straight away
// (the connected row is 32 px now — no detail without a connection).
struct WifiOffRows {
	double hero;
	double known;
	double home;
	double oldcafe;
	double other;
	double cafe;
	double corp;
	double library;
};

WifiOffRows wifi_off_rows();

// Everything the scenario started, in reverse order: the launcher (which
// terminates the dbusmock mocks with it), then the buses.
void stop_isolation();

// One variable of a live process's environment, as /proc tells it — the
// isolation proof straight from the bar's own process.
std::string proc_env_value(pid_t pid, const char *key);

// `events` wheel events of `units` over the bar at x, turned away from the
// user: the direction that raises a volume.
void wheel_over_bar(double x, int units, int events = 1);

// A click on the empty canvas below every panel's card: the panel closes.
void click_outside_cards(double x = 60.0);

// The centre of the bar run that opens the named indicator's panel, 0
// before the walk found it.
double indicator_x(const char *name);

// The stand-in backlight's percentage, and how many times the widget set it.
int backlight_percent();

// The scratch plugin's storage file, read back as text: the proof an
// action ran is what it wrote (api.storage writes JSON under the
// scratch state dir).
std::string read_storage(const char *name);

// The mapped layer surface with this namespace: the shell client's
// surfaces are told apart by name (the bar, each panel, the toolbar).
LayerSurface *named_layer(const char *ns);

LayerSurface *bar_layer();

wl_client *bar_client();

// The bar client's mapped panel layer surface with this name (each panel
// names its surface; the shell and this suite agree on the names).
LayerSurface *panel_layer(const char *name);

// The overview toolbar surface, shown only while its output is in the
// overview.
LayerSurface *toolbar_layer();

// Mapped panels of any name: the one-at-a-time rule counts them.
int panel_layers();

// The card a panel draws, on screen: centred under the indicator and
// clamped exactly the way Panel.qml clamps it.
wlr_box card_box(double icon_x, int width);

// The lit text bands inside a card's column, top to bottom: one per row,
// header or field of the panel's content, so positions are read off the
// screenshot instead of assumed from the layout.
struct Band {
	int top, bottom;
	int lit = 0; // lit pixels: a row of text has hundreds, a dim header
		     // fragment dozens — the row filter below tells them apart
	double centre() const { return (top + bottom - 1) / 2.0; }
};

// The lit row bands inside a rectangle of the screen, top to bottom: one
// per line of glyphs or text, so positions are read off the screenshot
// instead of assumed from the layout. Clamped to the image on every side.
std::vector<Band> bands_in(cairo_surface_t *s, const wlr_box &rect);

// The lit text bands inside a card's column, top to bottom: one per row,
// header or field of the panel's content.
std::vector<Band> text_bands(cairo_surface_t *s, const wlr_box &card);

// Only the rows of a named shot: dim section headers render as thin
// half-lit fragments that shift raw band indices, so the action flows
// click by row, counted from the bottom (the last row is stable).
std::vector<Band> panel_rows(const char *name);

// The rows of the card itself, so what the desktop shows below the card is
// not counted and the same card reads the same wherever it hangs: the
// text bands down to the first pixel under the card's left padding that
// is not the card's own fill.
std::vector<Band> card_rows(cairo_surface_t *s, const wlr_box &card);

// panel_rows of the shot, but only the card's own: the rows with at least
// 60 lit pixels inside popup_box. The last of them is the last row of the
// view shown, whatever its gap to the row above.
std::vector<Band> panel_card_rows(const char *name);

// Shots `name` on each call and says whether two consecutive shots showed the same shape:
// call it on successive ticks and click only on yes, since under load the bar renders
// late and a click aimed at a stale shot lands wrong or toggles a row twice.
bool settle_advance(const char *name);

inline constexpr const char *NM_NAME = "org.freedesktop.NetworkManager";

inline constexpr const char *NM_MANAGER = "/org/freedesktop/NetworkManager";

inline constexpr const char *NM_DEVICE = "/org/freedesktop/NetworkManager/Devices/wifi0";

// All of a child's stdout, or empty; the child is reaped either way.
std::string child_output(Spawned &child, int timeout_s);

// busctl against the private system bus only — the address is always
// explicit, so nothing can ever reach the machine's bus.
std::string busctl(const char *verb, const char *name, const char *path, const char *interface, const char *member);

// True when the mock object at `path` recorded a call to `method` — with
// `arg` in its arguments when `arg` is not empty.
bool mock_called(const char *name, const char *path, const char *method, const char *arg = nullptr);

// A property of a stand-in object, as busctl prints it.
std::string mock_property(const char *name, const char *path, const char *interface, const char *property);

cairo_surface_t *load_png(const char *name);

struct Run {
	double x0, x1;
	double centre() const { return (x0 + x1) / 2; }
};

// The lit column runs of the bar strip: each run is one widget's glyphs on
// the bar's own flat background. Gaps of 8 px or less stay inside a widget
// (letter spacing); widget boundaries are wider.
std::vector<Run> bar_runs(cairo_surface_t *s, const wlr_box &bar);

// Bright pixels (text-coloured) inside a rectangle of the screen: password
// dots, or a cursor sliver when nothing was typed.
int bright_count(cairo_surface_t *s, const wlr_box &rect);

// One screenshot pixel as 0xRRGGBB (grim's ARGB32: B, G, R, A in memory).
uint32_t pixel_at(cairo_surface_t *s, int x, int y);

// True when the pixel is the palette colour: the bar, panel cards, title
// bars and the target tab are flat fills of one palette entry, so sampling
// one clear pixel tells the mode. The palette carries 0xRRGGBBAA with the
// alpha in the low byte; a screenshot pixel has no alpha, so the alpha is
// shifted out before the channels are compared.
bool pixel_is(cairo_surface_t *s, int x, int y, uint32_t want);

// A shot's one pixel against a palette colour.
bool shot_pixel_is(const char *name, int x, int y, uint32_t want);

// A shot that has to show something. Each call is a poll: the first takes
// `name`, each later one reads the shot that landed since and, until `holds`
// accepts it, takes a fresh one. Nothing once it holds, or once the wait's
// budget is spent (said on stderr): `name` is then the last shot that
// landed, and the checks that read it report what did not come true.
std::optional<Result> shot_holds(const char *name,
	const std::function<bool(cairo_surface_t *)> &holds, Wait wait = {.poll_ms = 400, .budget_ms = 15000},
	std::source_location where = std::source_location::current());

// What shot_holds waits for when it is one pixel: it is the palette colour.
std::function<bool(cairo_surface_t *)> pixel_shows(int x, int y, uint32_t want);

// What shot_holds waits for when it is the bar's own background: the bar,
// wherever its surface is when the shot is read, is the palette colour.
std::function<bool(cairo_surface_t *)> bar_shows(uint32_t want);

// What shot_holds waits for when it is the bar with all the runs it had when
// bring-up read it: a shot taken before a widget's source answered has fewer.
std::function<bool(cairo_surface_t *)> bar_drawn();

// The mapped view with this title, as the input scenario finds its windows.
View *titled(const char *title);

// Runs `wayland-canvas msg <method> [<params>]` through /bin/sh, with
// stdout, stderr and the exit code captured under `name` (.out, .err,
// .code); a stale capture from an earlier run is unlinked first. The
// compositor runs in this process, so the child is never waited on here.
void run_msg(std::string_view method, std::string_view params, const char *name);

bool msg_done(const char *name);

int msg_code(const char *name);

// Nothing the scenario started outlives it: a msg child still running at
// the end is killed and reaped.
void stop_children();

// One JSON object member as a string, or "" when it is absent.
std::string json_string(JsonObject *object, const char *name);

// The notes plugin's own surfaces on the plane: one per note, named by
// the plugin and its note's id (docs/intents/platform/ipc.md).
inline constexpr std::string_view note_surface_prefix = "wayland-canvas-canvas-canvas.notes/";

bool is_note_surface(const LayerSurface &l);

// The mapped surface of the note `id`; any note's when `id` is empty.
LayerSurface *note_surface(std::string_view id = {});

// The note's rectangle on the plane, as the compositor placed it.
wlr_box note_box(std::string_view id);

// The first output the headless backend gives.
Output *out();

// A canvas point of the note `id`, `dx`, `dy` canvas px from its top-left
// corner (negative: from its right or bottom edge), in output-local
// logical pixels: taken through the camera the way the renderer draws it.
Point note_local(std::string_view id, double dx, double dy);

// The plugin's strip (plugins/canvas.notes/NoteSurface.qml): 26 px along
// the card's top, the dots 8 px wide from 6 px in, 4 px apart.
inline constexpr int note_strip = 26;

// The pointer from (x0, y0) to (x1, y1) in ten steps, the button as it is.
void travel(double x0, double y0, double x1, double y1);

// The left section's right edge, by ink: glyph pixels stand off the
// background in either mode — bright on the dark bar, dark on the light
// one — so the edge reads the same before and after a switch. The
// re-enabled plugin's label sits past the logo, pushing the edge right.
double bar_ink_edge(cairo_surface_t *s, const wlr_box &bar, bool light);

// True once the overview is where the test wants it and the camera has
// stopped easing: positions read off the screen are stable then.
bool overview_settled(bool active);

// The panel's palette colours as pixel_is takes them: 0xRRGGBBAA, the
// alpha shifted out before the channels are compared.
inline constexpr uint32_t accent_dark = 0x7aa2f7ff;   // UiTheme.accent, dark

inline constexpr uint32_t attention_dark = 0xf7768eff; // UiTheme.attention, dark

// The stand-in's READY line ("READY <port>"), or empty on a timeout.
std::string ready_line(Spawned &child, int timeout_s);

// The local date `offset` days from today: the collector buckets at local
// midnight, so the fixtures speak local dates.
std::string day_iso(int offset);

// The usage endpoint's answer, at whatever shares the phase wants.
void write_usage_payload(const char *session, const char *weekly);

// One omp session line: the shape the pi/omp tools write, which the
// collector counts by provider family.
std::string usage_line(const std::string &id, const std::string &date, const char *clock, const char *provider, const char *model, long long input, long long output);

// The fixture world: every family the panel shows — Claude's subscription
// (exact `anthropic`), GPT, Fireworks, a custom Anthropic gateway — plus
// the lines that must not count, the CLI's saved sign-in, and a Codex
// session file. Claude's day figures deliberately equal Omarchy's own
// record for the days the reference screenshot shows.
void drop_usage_fixtures();

// The collector's runs so far: each one starts the fake codex app-server,
// whose log holds one initialize call per run.
int codex_runs();

// The whole fixture world removed: the panel's own quiet-world check.
void remove_usage_fixtures();

// Rewrites the scratch shell.json as it stands: the shell sees an edit
// and reloads, leaving everything the config carries in place.
void reload_shell();

// Pixels of one colour (within the palette tolerance) inside a rect.
int colour_count(cairo_surface_t *s, const wlr_box &rect, uint32_t want);

extern double dot_x;

bool shot_landed(const char *name);

// The settings plugin's metrics: its own SettingsPanel.qml on the UI
// kit's fixed panel metrics. Keep in sync with the panel and
// UiTheme.qml — the phase aims its clicks by these numbers and
// cross-checks each against the screenshot's bands before clicking.
inline constexpr int settings_panel = 488;

inline constexpr int settings_pad = 10;     // the kit's panelPadding

inline constexpr int settings_gap = 6;      // the kit's panelSpacing

inline constexpr int settings_header = 18;  // a section header

inline constexpr int settings_row = 32;     // a plain row, and the tabs row

inline constexpr int settings_detail = 14;  // a detail line

inline constexpr int settings_choice = settings_row + settings_detail; // a choice row

inline constexpr int settings_footer = 2 * settings_detail + 2; // the two file paths

inline constexpr int settings_switch_w = 38; // PanelSwitch's width

// Shortcuts, Gestures, Session, Toolbar, Bar, then one tab per plugin that
// declares settings (docs/intents/shell/plugins.md): canvas.notifications is
// the built-in one that does.
inline constexpr int settings_tabs = 6;

// The kit lays the tabs in one row of equal shares (PanelTabs.qml); the
// card is wide enough that no label has to elide.
inline constexpr int settings_tab_cols = settings_tabs;

inline constexpr int settings_tab_w = (settings_panel - 2 * settings_pad) / settings_tab_cols;

inline constexpr int settings_tabs_h = settings_row;

// One Bar-tab section's height: its header, its rows (a detailed row is
// the taller), and the gaps between them.
int settings_section_height(int plain, int detailed);

// Where PanelCard puts the settings card's left edge, beside the toolbar
// strip on the right edge or the left one.
int settings_card_left(bool left_edge);

// The settings card's height with `column` of tab content in it: the
// padding, the tabs, the content, the separator and the footer.
int settings_card_height(int column);

int settings_card_top(int height, double level);

// The band whose centre is nearest `y`, or end() when there is none.
std::vector<Band>::const_iterator settings_band_at(const std::vector<Band> &bands, double y);

// Tab `i`'s centre in output coordinates: equal shares, at most four to a
// row, spread over as many rows as that takes (PanelTabs.qml).
std::pair<double, double> settings_tab_point(int i, int card_y);

// The tab row's chosen tab, read off the shot: the one tab whose fill is
// the accent's tint while every other tab shows the panel's own colour.
// -1 when the row does not show exactly one chosen tab.
int settings_chosen_tab(cairo_surface_t *s, int card_y);

// A PanelSwitch whose row is centred on `y`: on, its track is the accent
// at the end the knob left; off, the track's grey at that end.
bool settings_switch_is(cairo_surface_t *s, double y, bool on);

// The label columns of a settings row: everything left of the switches and
// dots, so a band scan sees the text and not the controls.
inline constexpr int settings_label_w = settings_panel - 2 * settings_pad - 48;

// True while the bar surface remembered last is still the bar: a reload
// makes a new one, and the count of bar surfaces made only grows, where
// an address or a protocol id can come back for the new surface.
bool settings_shell_kept();

// The shell reloaded since watch_remember_bar(), its bar up again.
bool shell_reloaded();

// The weather state fetches once as it starts (the first widget or panel
// to need it), so the curl stand-in's calls for wttr.in count the times
// the weather was started.
int weather_fetch_count();

extern int weather_fetches; // weather_fetch_count() before a host write

// notify-send on the private session bus, where the shell's own server is
// the only one listening. A sender's own timeout of a minute keeps a card
// up for the shot, and an urgent one carries the attention border that
// popup_cards counts.
void notify_send(const char *summary);

// The names the config's bar.hidden lists.
std::vector<std::string> shell_bar_hidden();

// The scratch shell.json as it stands with the hello plugin switched on
// or off, every other byte kept: the edit the watcher steps save.
std::string with_hello(bool on);

// Remembers the bar surface that is up now, so the next edit's reload is
// told by it being replaced.
void watch_remember_bar();

// The scratch shell.json with the hello plugin switched off and nothing
// else in it: where the plugins phase leaves the config.
extern const std::string_view hello_off_config;

// A step that leaves the scratch world as the plugins phase ends it: the
// user plugin in the data dir with its labelled widget, switched off in
// shell.json. Does nothing when the plugins phase already did; else it
// installs the plugin and waits for the reload that follows. The phases
// that edit the config around the plugin start with it, so each runs alone.
harness::Step hello_plugin_step(std::string_view name);

bool set_toolbar_position(const char *edge);

// The toolbar.position the scratch shell.json names now.
std::string toolbar_position_in_config();

bool set_app_bar_position(const char *edge);

bool set_app_bar_enabled(bool on);

// The strip's settled box, from the surface's placed one: the surface
// is the strip plus the pocket it slides into — overhanging the screen
// edge by a button square on the left, right and bottom (the pocket
// past it), and past the screen's top on the top edge (the row sits
// below the bar) — so the strip's near edge is the surface's own on
// the right and bottom, the pocket's far one on the left and top.
wlr_box toolbar_strip_box(const wlr_box &geo);

// Whether the layer surface holds a strip on `edge`: a 44 px column or row of whole squares
// on the edge's margin (the top row below the bar). The old surface stays mapped at the old
// edge until a reload, so waits loop on its geo, not on its mapping.
bool strip_on_edge(LayerSurface *layer, const char *edge);

bool toolbar_on_edge(const char *edge);

// The app bar surface, shown with the overview on the edge its config
// names (the left by default): one entry per open app
// (docs/intents/shell/app-bar.md).
LayerSurface *app_bar_layer();

// The app bar's own strip, from its placed surface: like the toolbar's, the
// surface is the strip plus the pocket it slides into.
wlr_box app_bar_strip_box(const wlr_box &geo);

// The app bar's flyout surface: an app's windows, unfolded beside its entry
// (docs/intents/shell/app-bar.md).
LayerSurface *app_bar_flyout_layer();

// How many windows the app `view` belongs to holds: an app with more than
// one unfolds them instead of jumping straight there.
int app_bar_windows_of(const View *view);

// Where the flyout's row for `view` sits, from the entry's square: the card
// hangs beside the strip, one margin away, its rows from its top padding
// down. Keep in sync with AppBar.qml.
std::pair<double, double> app_bar_flyout_row(const wlr_box &entry, const View *view);

// The strip is centred along its edge, from the placed box: the pair is,
// where the app bar shares the edge — the toolbar first, then the gap and
// the apps.
bool toolbar_centred(const wlr_box &geo);

// The strip is on `edge` and centred along it: where a move ends once the
// strip has slid in and the apps beside it are counted.
bool toolbar_settled_on(const char *edge);

// Steps that make the state a phase starts from when no phase before it
// has: each does nothing when the state already holds, so the whole run
// and a phase alone take the same steps.
//
// A mapped window titled "themed", a foot that sleeps.
harness::Step themed_window_step(std::string_view name);
// At least `count` mapped windows of the first window's app.
harness::Step app_windows_step(std::string_view name, int count);
// The first mapped toplevel, nullptr when none is.
View *first_mapped_toplevel();
// The Wi-Fi stand-in disconnected, as the walk leaves it: the network
// panel lists the five networks and no connection's detail.
harness::Step wifi_offline_step(std::string_view name);
// The overview open and settled, with the shell's toolbar mapped.
harness::Step overview_open_step(std::string_view name);
// The settings gear's centre along the toolbar's column, from the strip's
// placed box; 0 while no toolbar is mapped.
double toolbar_gear_centre();
// A second headless output joined, and its bar up beside the first's.
harness::Step second_output_step(std::string_view name);
// The mapped bars, one per output.
int bar_layer_count();
// Adds a headless output to the backend; false when there is no headless
// backend to add it to.
bool add_headless_output();

// The phases, in the order they run.
std::vector<harness::Step> bringup_steps();
std::vector<harness::Step> walk_steps();
std::vector<harness::Step> plugins_steps();
std::vector<harness::Step> overview_steps();
std::vector<harness::Step> toolbar_steps();
std::vector<harness::Step> theme_steps();
std::vector<harness::Step> watcher_steps();
std::vector<harness::Step> nightlight_steps();
std::vector<harness::Step> wheel_steps();
std::vector<harness::Step> notes_steps();
std::vector<harness::Step> network_steps();
std::vector<harness::Step> agents_steps();
std::vector<harness::Step> recording_steps();
std::vector<harness::Step> edges_steps();
std::vector<harness::Step> barwrites_steps();
std::vector<harness::Step> socket_steps();
std::vector<harness::Step> settings_steps();
std::vector<harness::Step> appbar_steps();
std::vector<harness::Step> plugin_settings_steps();
std::vector<harness::Step> appbar_hover_steps();
std::vector<harness::Step> screenshots_steps();
std::vector<harness::Step> shared_steps();
std::vector<harness::Step> handover_steps();

// Called before the harness starts; what the bring-up and the run share.
void remember_machine_environment();
// `run_name` is the run's scratch name; the run's buses get a folder of their own.
bool bring_up(std::string_view run_name);
void tear_down();
void watch_bar_surfaces();

} // namespace bar
