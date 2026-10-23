// The private buses and the stand-in services on them: NetworkManager, BlueZ, UPower, PipeWire, curl
// and the tools, and what the mocks recorded.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

pid_t standins_pid = -1;

// The child's first stdout line, or empty when it ends without one.
std::string first_line(Spawned &child) {
	std::string line;
	char c = 0;
	while (read(child.out, &c, 1) == 1 && c != '\n') {
		line += c;
	}
	close(child.out);
	child.out = -1;
	return line;
}

// True when the child prints the line within the timeout; false when it
// exits or stalls first.
bool prints_line(Spawned &child, std::string_view want, int timeout_s) {
	std::string buffer;
	char chunk[256];
	for (int tick = 0; tick < timeout_s * 2; tick++) { // ticks of 500 ms
		pollfd p = {.fd = child.out, .events = POLLIN, .revents = 0};
		if (poll(&p, 1, 500) > 0 && (p.revents & (POLLIN | POLLHUP)) != 0) {
			ssize_t n = read(child.out, chunk, sizeof chunk);
			if (n <= 0) {
				break;
			}
			buffer.append(chunk, static_cast<std::size_t>(n));
			if (buffer.find(want) != std::string::npos) {
				return true;
			}
		}
		if (waitpid(child.pid, nullptr, WNOHANG) == child.pid) {
			break;
		}
	}
	return false;
}

// The PipeWire stand-in: a daemon of the run's own holds one null sink as the default, at
// half volume, so a wheel turn can only move that. Everything here speaks the widget's
// scale (Quickshell's `volume`, the cube root of PipeWire's per-channel linear volume); the
// cube is taken where the daemon is written to.
pid_t pipewire_pid = -1;
constexpr const char *standin_sink = "standin-sink";

void pause_ms(int ms) {
	timespec ts = {.tv_sec = 0, .tv_nsec = ms * 1'000'000L};
	nanosleep(&ts, nullptr);
}

JsonObject *object_member(JsonObject *parent, const char *name) {
	return parent != nullptr && json_object_has_member(parent, name)
			&& JSON_NODE_HOLDS_OBJECT(json_object_get_member(parent, name))
		? json_object_get_object_member(parent, name) : nullptr;
}

} // namespace

PrivateBus private_session; // the world's own buses

PrivateBus private_system;

std::string bus_dir; // where their sockets live, kept short (see below)

std::vector<Indicator> indicators;

std::size_t current = 0;

wlr_box popup_box{}; // the open panel's card, from the walk's phase 1

wlr_box field_box{}; // where the Wi-Fi password field ended up

double clock_x = 0;

double network_x = 0; // where the walk found the clock, network

std::size_t wifi_base = 0; // the network panel's settled row count, baseline

int wifi_last_top = 0; // its last row's top: a prompt under a row above

std::string read_file(const std::string &path) {
	FILE *f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		return {};
	}
	std::string out;
	char buf[4096];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
		out.pop_back();
	}
	return out;
}

void replace_file(const std::string &path, std::string_view text) {
	std::string sibling = path + ".saving";
	write_file(sibling, text);
	if (rename(sibling.c_str(), path.c_str()) != 0) {
		std::fprintf(stderr, "cannot rename %s: %s\n", sibling.c_str(),
			std::strerror(errno));
	}
}

Spawned spawn(const char *const argv[]) {
	int fds[2];
	if (pipe(fds) != 0) {
		return {};
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		close(fds[1]);
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, nullptr);
		prctl(PR_SET_PDEATHSIG, SIGKILL); // a killed scenario must not leak buses
		execvp(argv[0], const_cast<char *const *>(argv));
		_exit(127);
	}
	close(fds[1]);
	return pid > 0 ? Spawned{pid, fds[0]} : Spawned{};
}

void outside_theme_set(const char *mode) {
	const char *path = std::getenv("WAYLAND_CANVAS_SOCKET");
	if (path == nullptr || *path == '\0') {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		if (fork() == 0) {
			alarm(5); // never outlive the run on a stalled socket
			int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
			sockaddr_un addr{};
			addr.sun_family = AF_UNIX;
			if (fd >= 0 && strlen(path) < sizeof(addr.sun_path)) {
				strcpy(addr.sun_path, path);
				if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
					std::string line = std::format(
						"{{\"id\": 1, \"method\": \"theme.set\", "
						"\"params\": {{\"mode\": \"{}\"}}}}\n", mode);
					(void)write(fd, line.data(), line.size());
					// The response, read out so the compositor's write is
					// never blocked; then this end goes quiet.
					char chunk[256];
					ssize_t got = 0;
					while ((got = read(fd, chunk, sizeof chunk)) > 0) {
						if (memchr(chunk, '\n', static_cast<std::size_t>(got)) != nullptr) {
							break;
						}
					}
				}
			}
			if (fd >= 0) {
				close(fd);
			}
		}
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0); // the middle child, at once: no zombie
}

bool start_private_bus(PrivateBus &bus, const char *type, const char *name) {
	// A unix socket path is capped near 108 bytes, which a scratch folder
	// under a worktree can spend on its own: the buses sit in the
	// shortest path under the run's scratch root.
	std::string dir = bus_dir;
	mkdir_p(dir);
	std::string socket = std::format("{}/{}", dir, name);
	std::string conf = socket + ".conf";
	unlink(socket.c_str()); // a stale socket from a killed run
	std::string xml = std::format(
		"<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
		" \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
		"<busconfig>\n"
		"  <type>{}</type>\n"
		"  <listen>unix:path={}</listen>\n"
		"  <auth>EXTERNAL</auth>\n"
		"  <policy context=\"default\">\n"
		"    <allow own=\"*\"/>\n"
		"    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
		"    <allow eavesdrop=\"true\"/>\n"
		"  </policy>\n"
		"</busconfig>\n",
		type, socket);
	FILE *f = std::fopen(conf.c_str(), "w");
	if (f == nullptr) {
		std::fprintf(stderr, "cannot write %s\n", conf.c_str());
		return false;
	}
	std::fwrite(xml.data(), 1, xml.size(), f);
	std::fclose(f);
	const char *const argv[] = {"dbus-daemon", "--config-file", conf.c_str(), "--print-address=1",
		static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return false;
	}
	// The printed address only says the daemon is serving; the address the
	// bar and the stand-ins get is the plain listen form — dbus-python
	// never finishes connecting to the guid-decorated one.
	std::string printed = first_line(child);
	if (!printed.starts_with("unix:")) {
		std::fprintf(stderr, "the private %s bus did not start: %s\n", type,
			printed.c_str());
		kill(child.pid, SIGKILL);
		waitpid(child.pid, nullptr, 0);
		return false;
	}
	bus.pid = child.pid;
	bus.address = "unix:path=" + socket;
	return true;
}

bool start_standins() {
	const char *py = std::getenv("CANVAS_BAR_STANDIN_PY");
	const char *script = std::getenv("CANVAS_BAR_STANDINS");
	if (py == nullptr || *py == '\0' || script == nullptr || *script == '\0') {
		std::fprintf(stderr, "CANVAS_BAR_STANDIN_PY or CANVAS_BAR_STANDINS is unset: "
			"the stand-in environment is missing\n");
		return false;
	}
	const char *const argv[] = {py, script, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return false;
	}
	if (!prints_line(child, "READY\n", 30)) {
		std::fprintf(stderr, "the stand-in services did not become ready\n");
		kill(child.pid, SIGTERM);
		waitpid(child.pid, nullptr, 0);
		return false;
	}
	close(child.out);
	child.out = -1;
	standins_pid = child.pid;
	return true;
}

void reap(pid_t pid) {
	if (pid <= 0) {
		return;
	}
	for (int i = 0; i < 200; i++) {
		if (waitpid(pid, nullptr, WNOHANG) == pid) {
			return;
		}
		timespec ts = {.tv_sec = 0, .tv_nsec = 10'000'000};
		nanosleep(&ts, nullptr);
	}
	kill(pid, SIGKILL);
	waitpid(pid, nullptr, 0);
}

bool start_curl_standin() {
	const char *script = std::getenv("CANVAS_BAR_STANDINS");
	if (script == nullptr || *script == '\0') {
		std::fprintf(stderr, "CANVAS_BAR_STANDINS is unset: the curl stand-in "
			"cannot find its fixture\n");
		return false;
	}
	std::string fixture = (std::filesystem::path(script).parent_path()
		/ "bar-standins/wttr-j1.json").string();
	if (!std::filesystem::exists(fixture)) {
		std::fprintf(stderr, "the curl stand-in is missing its fixture: %s\n",
			fixture.c_str());
		return false;
	}
	const char *const which[] = {"sh", "-c", "command -v curl", nullptr};
	Spawned child = spawn(which);
	std::string real = first_line(child);
	reap(child.pid);
	if (real.empty()) {
		std::fprintf(stderr, "the curl stand-in found no real curl to pass "
			"other URLs to\n");
		return false;
	}
	std::string dir = shot_dir + "/curl-bin";
	mkdir_p(dir);
	std::string wrapper = dir + "/curl";
	std::string sh = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's curl\n"
		"# stand-in. wttr.in — the weather widget's fetch — is answered at\n"
		"# once from the canned response captured once; anything else goes\n"
		"# to the real curl. Every invocation is logged under the run's\n"
		"# scratch, so a run can prove it never asked the network for\n"
		"# weather.\n"
		"log=${CANVAS_BAR_DIR}/curl-standin.log\n"
		"printf '%s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"for arg in \"$@\"; do\n"
		"\tcase \"$arg\" in\n"
		"\t*wttr.in*)\n"
		"\t\tcat " + fixture + "\n"
		"\t\texit 0\n"
		"\t\t;;\n"
		"\tesac\n"
		"done\n"
		"exec " + real + " \"$@\"\n";
	write_file(wrapper, sh);
	chmod(wrapper.c_str(), 0755);
	const char *path = std::getenv("PATH");
	setenv("PATH", std::format("{}:{}", dir, path != nullptr ? path : "").c_str(), 1);
	return true;
}

void fixture_online(bool on) {
	FILE *f = std::fopen((shot_dir + "/nmcli-state").c_str(), "a");
	if (f == nullptr) {
		return;
	}
	std::fprintf(f, "offline=%d\n", on ? 0 : 1);
	std::fclose(f);
}

bool start_tool_standins() {
	std::string dir = shot_dir + "/tool-bin";
	mkdir_p(dir);
	// a stale state file or log from a killed run would leak into this one
	unlink((shot_dir + "/nmcli-state").c_str());
	unlink((shot_dir + "/tool-standin.log").c_str());
	const char *const which[] = {"sh", "-c", "command -v wl-copy", nullptr};
	Spawned child = spawn(which);
	std::string real = first_line(child);
	reap(child.pid);
	if (real.empty()) {
		std::fprintf(stderr, "the tool stand-ins found no real wl-copy to "
			"pass the copy through to\n");
		return false;
	}
	std::string nmcli = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's nmcli\n"
		"# stand-in. It answers the network panel's queries from the\n"
		"# fixture world, records the changes the panel makes in a state\n"
		"# file so the panel's reads follow its writes, and logs every\n"
		"# call. The passphrase in an enterprise join arrives on stdin and\n"
		"# is proven received, never logged.\n"
		"log=${CANVAS_BAR_DIR}/tool-standin.log\n"
		"state=${CANVAS_BAR_DIR}/nmcli-state\n"
		"printf 'nmcli %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"key(){ grep \"$1\" \"$state\" 2>/dev/null | tail -n 1 | cut -d= -f2-; }\n"
		"# nmcli's terse mode writes the colon and the backslash of a\n"
		"# value behind a backslash\n"
		"esc(){ printf '%s' \"$1\" | sed 's/[\\\\:]/\\\\&/g'; }\n"
		"case \"$*\" in\n"
		"*\"device show\"*)\n"
		"\t# the scenario marks the world offline once the walk's click\n"
		"\t# disconnects the fixture: the reads agree with the mock then; a\n"
		"\t# roam names the network the machine moved to instead\n"
		"\tif [ \"$(key '^offline=')\" = 1 ]; then\n"
		"\t\tprintf '\\n\\n\\n'\n"
		"\telif [ -n \"$(key '^roam_ssid=')\" ]; then\n"
		"\t\tprintf '%s\\n" + std::string(wifi_ip) + "/24\\n" + std::string(wifi_gateway)
		+ "\\n' \"$(esc \"$(key '^roam_ssid=')\")\"\n"
		"\telse\n"
		"\t\tprintf '" + std::string(wifi_ssid) + "\\n" + std::string(wifi_ip)
		+ "/24\\n" + std::string(wifi_gateway) + "\\n'\n"
		"\tfi\n"
		"\t;;\n"
		"*\"dev wifi list\"*)\n"
		"\thome='*'\n"
		"\tif [ \"$(key '^offline=')\" = 1 ] || [ -n \"$(key '^roam_ssid=')\" ]; then\n"
		"\t\thome=\n"
		"\tfi\n"
		"\tprintf '%s:2437:" + std::string(wifi_ssid) + "\\n:5180:" + std::string(wifi_ssid)
		+ "\\n:6175:" + std::string(wifi_ssid) + "\\n' \"$home\"\n"
		"\tif [ -n \"$(key '^roam_ssid=')\" ]; then\n"
		"\t\tprintf '*:%s:%s\\n' \"$(key '^roam_freq=')\" \"$(esc \"$(key '^roam_ssid=')\")\"\n"
		"\tfi\n"
		"\tprintf ':2437:CafeGuest\\n:5240:CorpNet\\n:2462:LibraryFree\\n:2462:OldCafe\\n'\n"
		"\t;;\n"
		"*\"802-11-wireless.band connection show\"*)\n"
		"\tkey '^band='\n"
		"\t;;\n"
		"*\"802-11-wireless-security.psk connection show\"*)\n"
		"\tif [ -n \"$(key '^roam_ssid=')\" ]; then\n"
		"\t\tprintf '%s\\n' \"$(esc \"$(key '^roam_psk=')\")\"\n"
		"\telse\n"
		"\t\tprintf '" + std::string(wifi_psk) + "\\n'\n"
		"\tfi\n"
		"\t;;\n"
		"*\"connection modify\"*)\n"
		"\tprev=\n"
		"\tfor arg in \"$@\"; do\n"
		"\t\tcase \"$prev\" in\n"
		"\t\t802-11-wireless.band) printf 'band=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv4.dns) printf 'dns4=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv4.ignore-auto-dns) printf 'ignore4=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv6.dns) printf 'dns6=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tipv6.ignore-auto-dns) printf 'ignore6=%s\\n' \"$arg\" >>\"$state\" ;;\n"
		"\t\tesac\n"
		"\t\tprev=$arg\n"
		"\tdone\n"
		"\t;;\n"
		"*\"ipv4.dns\"*)\n"
		"\tprintf '%s\\n%s\\n%s\\n%s\\n' \"$(key '^dns4=')\" \"$(key '^ignore4=' || :)\""
		" \"$(key '^dns6=')\" \"$(key '^ignore6=' || :)\"\n"
		"\t;;\n"
		"*\"connection edit\"*)\n"
		"\twhile IFS= read -r line; do\n"
		"\t\tcase \"$line\" in\n"
		"\t\t\"set 802-1x.password \"*) printf 'nmcli-eap-secret-set\\n' >>\"$log\" ;;\n"
		"\t\tesac\n"
		"\tdone\n"
		"\t;;\n"
		"esac\n"
		"exit 0\n";
	write_file(dir + "/nmcli", nmcli);
	std::string ping = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's ping\n"
		"# stand-in. Every probe of the fixture gateway answers 12.3 ms, so\n"
		"# the panel's ping and packet-loss figures are deterministic.\n"
		"log=${CANVAS_BAR_DIR}/tool-standin.log\n"
		"printf 'ping %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"printf 'PING " + std::string(wifi_gateway) + " 56 data bytes\\n'\n"
		"printf '64 bytes from " + std::string(wifi_gateway)
		+ ": icmp_seq=0 ttl=64 time=12.3 ms\\n'\n"
		"printf -- '--- ping statistics ---\\n'\n"
		"printf -- '1 packets transmitted, 1 packets received, 0%% packet loss\\n'\n"
		"exit 0\n";
	write_file(dir + "/ping", ping);
	std::string qrencode = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's\n"
		"# qrencode stand-in. Like the real tool it encodes its last\n"
		"# argument that is not an option, and reads its stdin to the\n"
		"# end when there is none. The argv is logged as one line, the\n"
		"# text read from stdin as another, so a run can prove which of\n"
		"# the two carried a payload. A real encoded matrix of the\n"
		"# fixture's payload answers — the drawn code has to scan, not\n"
		"# just show ink. The matrix is the ASCII type that the panel\n"
		"# itself parses: two characters a module, quiet zone included,\n"
		"# so the run's reader is the panel's own.\n"
		"log=${CANVAS_BAR_DIR}/tool-standin.log\n"
		"printf 'qrencode %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"text=\n"
		"skip=\n"
		"for arg in \"$@\"; do\n"
		"\tif [ -n \"$skip\" ]; then skip=; continue; fi\n"
		"\tcase \"$arg\" in\n"
		"\t-t|-m) skip=1 ;;\n"
		"\t-*) ;;\n"
		"\t*) text=$arg ;;\n"
		"\tesac\n"
		"done\n"
		"if [ -z \"$text\" ]; then\n"
		"\tprintf 'qrencode-stdin %s\\n' \"$(cat)\" >>\"$log\" 2>/dev/null\n"
		"fi\n"
		"# the matrix's lines carry their light modules as trailing\n"
		"# spaces, which nothing downstream may rely on: awk pads every\n"
		"# line back out to the matrix's full width, 37 modules of two\n"
		"# characters\n"
		"awk '{printf \"%-74s\\n\", $0}' <<'QR'\n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"        ##############    ##    ######  ##    ####  ##############        \n"
		"        ##          ##  ##    ############  ##      ##          ##        \n"
		"        ##  ######  ##  ############    ##  ######  ##  ######  ##        \n"
		"        ##  ######  ##    ######  ########  ####    ##  ######  ##        \n"
		"        ##  ######  ##  ##        ######        ##  ##  ######  ##        \n"
		"        ##          ##  ####  ##  ####  ######      ##          ##        \n"
		"        ##############  ##  ##  ##  ##  ##  ##  ##  ##############        \n"
		"                        ####    ##  ##  ########                          \n"
		"        ####  ##    ####    ##              ##      ######  ####          \n"
		"          ########          ####    ##    ##  ##  ##  ####      ##        \n"
		"        ##########  ##    ####        ######  ##  ##    ####  ####        \n"
		"          ##  ####    ##            ##      ####  ######    ##  ##        \n"
		"        ##  ##    ######        ##        ####    ##      ##              \n"
		"        ##      ##        ########    ##  ##  ##      ##########          \n"
		"        ##################  ##  ##      ##    ########      ######        \n"
		"            ##        ##  ##    ######  ####      ######    ##            \n"
		"                ######  ##  ####  ####  ##    ##########  ##  ##          \n"
		"            ##  ##    ####  ##              ####            ######        \n"
		"        ##  ##      ######  ##    ##      ##      ########  ##  ##        \n"
		"            ####      ######  ##  ##      ##  ######      ##              \n"
		"        ##  ####    ##    ########  ##  ##  ##  ##########  ##            \n"
		"                        ####  ######      ####  ##      ##  ##            \n"
		"        ##############  ##  ##########  ####  ####  ##  ##    ##          \n"
		"        ##          ##        ######    ##  ##  ##      ##  ##            \n"
		"        ##  ######  ##        ##    ##          ##########  ######        \n"
		"        ##  ######  ##  ##  ####  ####    ##    ##  ##  ##    ####        \n"
		"        ##  ######  ##        ##  ########    ##  ######  ##    ##        \n"
		"        ##          ##  ####  ##        ##    ####      ##########        \n"
		"        ##############  ######  ####        ##          ##                \n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"                                                                          \n"
		"QR\n"
		"exit 0\n";
	write_file(dir + "/qrencode", qrencode);
	std::string speedtest = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's\n"
		"# speedtest stand-in, answering Ookla's JSON shape.\n"
		"log=${CANVAS_BAR_DIR}/tool-standin.log\n"
		"printf 'speedtest %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"printf '{\"ping\":{\"latency\":9.5},\"download\":"
		"{\"bandwidth\":12345678},\"upload\":{\"bandwidth\":2345678}}\\n'\n"
		"exit 0\n";
	write_file(dir + "/speedtest", speedtest);
	std::string wlcopy = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's\n"
		"# wl-copy stand-in: the copy passes through to the real one under\n"
		"# the scenario's display, and the call is logged so a run can\n"
		"# prove which figure was copied.\n"
		"log=${CANVAS_BAR_DIR}/tool-standin.log\n"
		"printf 'wl-copy\\n' >>\"$log\" 2>/dev/null\n"
		"exec " + real + " \"$@\"\n";
	write_file(dir + "/wl-copy", wlcopy);
	write_file(shot_dir + "/brightness-state", "50\n");
	std::string brightnessctl = "#!/bin/sh\n"
		"# Generated by tests/scenarios/bar/standins.cpp: the bar scenario's\n"
		"# brightnessctl stand-in. One backlight at a percentage kept in a\n"
		"# state file: `set N%` stores it, and every call prints the machine\n"
		"# line (name,class,current,max,percent) the widget parses. Every\n"
		"# call is logged.\n"
		"log=${CANVAS_BAR_DIR}/tool-standin.log\n"
		"state=${CANVAS_BAR_DIR}/brightness-state\n"
		"printf 'brightnessctl %s\\n' \"$*\" >>\"$log\" 2>/dev/null\n"
		"for arg in \"$@\"; do\n"
		"\tcase \"$arg\" in\n"
		"\t*%) printf '%s\\n' \"${arg%\\%}\" >\"$state\" ;;\n"
		"\tesac\n"
		"done\n"
		"pct=$(cat \"$state\" 2>/dev/null || printf 50)\n"
		"printf 'standin,backlight,%s,100,%s%%\\n' \"$pct\" \"$pct\"\n"
		"exit 0\n";
	write_file(dir + "/brightnessctl", brightnessctl);
	for (const char *name : {"nmcli", "ping", "qrencode", "speedtest", "wl-copy", "brightnessctl"}) {
		chmod((dir + "/" + name).c_str(), 0755);
	}
	const char *path = std::getenv("PATH");
	setenv("PATH", std::format("{}:{}", dir, path != nullptr ? path : "").c_str(), 1);
	return true;
}

bool pw_sink(int &id, double &volume) {
	std::string dump = shot_dir + "/pw-dump.json";
	if (std::system(std::format("timeout 5 pw-dump >'{}' 2>/dev/null", dump).c_str()) != 0) {
		return false;
	}
	JsonParser *parser = json_parser_new();
	JsonNode *root = json_parser_load_from_file(parser, dump.c_str(), nullptr)
		? json_parser_get_root(parser) : nullptr;
	bool found = false;
	JsonArray *objects = root != nullptr && JSON_NODE_HOLDS_ARRAY(root)
		? json_node_get_array(root) : nullptr;
	for (guint i = 0; objects != nullptr && i < json_array_get_length(objects) && !found; i++) {
		JsonObject *object = json_array_get_object_element(objects, i);
		JsonObject *info = object_member(object, "info");
		JsonObject *props = object_member(info, "props");
		if (props == nullptr || !json_object_has_member(props, "node.name")
				|| std::string_view{json_object_get_string_member(props, "node.name")}
					!= standin_sink) {
			continue;
		}
		JsonObject *params = object_member(info, "params");
		JsonArray *list = params != nullptr && json_object_has_member(params, "Props")
			? json_object_get_array_member(params, "Props") : nullptr;
		JsonObject *first = list != nullptr && json_array_get_length(list) > 0
			? json_array_get_object_element(list, 0) : nullptr;
		JsonArray *volumes = first != nullptr && json_object_has_member(first, "channelVolumes")
			? json_object_get_array_member(first, "channelVolumes") : nullptr;
		if (volumes != nullptr && json_array_get_length(volumes) > 0) {
			id = static_cast<int>(json_object_get_int_member(object, "id"));
			volume = std::cbrt(json_array_get_double_element(volumes, 0));
			found = true;
		}
	}
	g_object_unref(parser);
	return found;
}

bool start_pipewire_standin() {
	// Beside the buses: its socket path is capped near 108 bytes too.
	std::string dir = bus_dir + "/pw";
	std::error_code ec;
	std::filesystem::remove_all(dir, ec); // a stale socket from a killed run
	mkdir_p(dir);
	write_file(dir + "/pipewire.conf",
		"context.properties = {\n\tcore.daemon = true\n\tcore.name = pipewire-0\n}\n"
		"context.spa-libs = {\n\taudio.convert.* = audioconvert/libspa-audioconvert\n"
		"\taudio.adapt = audioconvert/libspa-audioconvert\n"
		"\tsupport.* = support/libspa-support\n}\n"
		"context.modules = [\n\t{ name = libpipewire-module-protocol-native }\n"
		"\t{ name = libpipewire-module-metadata }\n"
		"\t{ name = libpipewire-module-access args = { } }\n"
		"\t{ name = libpipewire-module-spa-node-factory }\n"
		"\t{ name = libpipewire-module-client-node }\n"
		"\t{ name = libpipewire-module-adapter }\n"
		"\t{ name = libpipewire-module-link-factory }\n]\n"
		"context.objects = [\n\t{ factory = metadata args = { metadata.name = default } }\n"
		"\t{ factory = spa-node-factory args = { factory.name = support.node.driver\n"
		"\t\tnode.name = Dummy-Driver node.group = pipewire.dummy\n"
		"\t\tpriority.driver = 20000 } }\n"
		"\t{ factory = adapter args = { factory.name = support.null-audio-sink\n"
		"\t\tnode.name = " + std::string{standin_sink} + "\n"
		"\t\tnode.description = \"Stand-in speakers\" media.class = Audio/Sink\n"
		"\t\taudio.position = [ FL FR ] object.linger = true } }\n]\n");
	setenv("PIPEWIRE_RUNTIME_DIR", dir.c_str(), 1);
	unsetenv("PIPEWIRE_REMOTE");
	unsetenv("PIPEWIRE_CORE");
	std::string launch = std::format(
		"exec pipewire -c '{0}/pipewire.conf' >'{0}/pipewire.log' 2>&1", dir);
	const char *const argv[] = {"sh", "-c", launch.c_str(), static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return false;
	}
	close(child.out);
	pipewire_pid = child.pid;
	// Once the daemon has made the sink: it becomes the default, at half volume.
	for (int tries = 0; tries < 50; tries++) {
		int id = 0;
		double volume = 0;
		if (pw_sink(id, volume)
				&& std::system(std::format(
					"timeout 5 pw-metadata -n default 0 default.audio.sink "
					"'{{\"name\":\"{0}\"}}' Spa:String:JSON >/dev/null 2>&1 && "
					"timeout 5 pw-cli set-param {1} Props "
					"'{{ channelVolumes: [ {2}, {2} ] }}' >/dev/null 2>&1",
					standin_sink, id, std::pow(standin_sink_start, 3)).c_str()) == 0) {
			return true;
		}
		pause_ms(100);
	}
	std::fprintf(stderr, "the PipeWire stand-in did not come up (see %s/pipewire.log)\n",
		dir.c_str());
	return false;
}

std::string standin_log() {
	return read_file(shot_dir + "/tool-standin.log");
}

int standin_calls(std::string_view text, std::string_view needle) {
	int count = 0;
	std::size_t at = 0;
	while ((at = text.find(needle, at)) != std::string_view::npos) {
		count++;
		at += needle.size();
	}
	return count;
}

WifiRows wifi_rows(bool pinned, bool editing) {
	WifiRows r{};
	double y = 10;
	r.hero = y + 22;
	y += 44 + 6;
	for (int i = 0; i < 4; i++) {
		r.figures[i] = y + 8;
		y += 16 + 6;
	}
	y += 1 + 6;
	r.band = y + 9;
	y += 18 + 6;
	if (pinned) {
		r.pills = y + 12;
		y += 24 + 6;
	}
	y += 1 + 6;
	r.dns = y + 9;
	y += 18 + 6;
	r.dns_pills = y + 12;
	y += 24 + 6;
	if (editing) {
		r.dns_field = y + 15;
		y += 30 + 6;
	}
	y += 1 + 6;
	r.known = y + 9;
	y += 18 + 6;
	r.home = y + 23;
	y += 46 + 6;
	r.oldcafe = y + 16;
	y += 32 + 6;
	r.other = y + 9;
	y += 18 + 6;
	r.cafe = y + 16;
	y += 32 + 6;
	r.corp = y + 16;
	y += 32 + 6;
	r.library = y + 16;
	return r;
}

WifiOffRows wifi_off_rows() {
	WifiOffRows r{};
	double y = 10;
	r.hero = y + 22;
	y += 44 + 6 + 1 + 6; // the hero, then the networks separator
	r.known = y + 9;
	y += 18 + 6;
	r.home = y + 16;
	y += 32 + 6;
	r.oldcafe = y + 16;
	y += 32 + 6;
	r.other = y + 9;
	y += 18 + 6;
	r.cafe = y + 16;
	y += 32 + 6;
	r.corp = y + 16;
	y += 32 + 6;
	r.library = y + 16;
	return r;
}

void stop_isolation() {
	if (pipewire_pid > 0) {
		kill(pipewire_pid, SIGTERM);
	}
	reap(pipewire_pid);
	reap(usage_standin_pid);
	reap(private_system.pid);
	reap(private_session.pid);
}

std::string proc_env_value(pid_t pid, const char *key) {
	if (pid <= 0) {
		return {};
	}
	FILE *f = std::fopen(std::format("/proc/{}/environ", pid).c_str(), "r");
	if (f == nullptr) {
		return {};
	}
	std::string blob;
	char buf[4096];
	std::size_t n = 0;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) { // environ may exceed 8 KiB
		blob.append(buf, n);
	}
	std::fclose(f);
	std::string prefix = std::format("{}=", key);
	std::size_t start = 0;
	while (start < blob.size()) {
		std::size_t end = blob.find('\0', start);
		if (end == std::string::npos) {
			end = blob.size();
		}
		if (blob.compare(start, prefix.size(), prefix) == 0) {
			return blob.substr(start + prefix.size(), end - start - prefix.size());
		}
		start = end + 1;
	}
	return {};
}

std::string child_output(Spawned &child, int timeout_s) {
	std::string out;
	char buf[4096];
	std::time_t deadline = std::time(nullptr) + timeout_s;
	while (std::time(nullptr) < deadline) {
		pollfd p{.fd = child.out, .events = POLLIN};
		if (poll(&p, 1, 200) <= 0 || (p.revents & (POLLIN | POLLHUP)) == 0) {
			continue;
		}
		ssize_t n = read(child.out, buf, sizeof buf);
		if (n <= 0) {
			break; // EOF: the child is done writing
		}
		out.append(buf, n);
	}
	kill(child.pid, SIGKILL); // a no-op on an already-exited child
	waitpid(child.pid, nullptr, 0);
	close(child.out);
	return out;
}

std::string busctl(const char *verb, const char *name, const char *path,
	const char *interface, const char *member) {
	const char *const argv[] = {"busctl", "--address", private_system.address.c_str(),
		verb, name, path, interface, member, static_cast<const char *>(nullptr)};
	Spawned child = spawn(argv);
	if (child.pid <= 0) {
		return {};
	}
	return child_output(child, 10);
}

bool mock_called(const char *name, const char *path, const char *method, const char *arg) {
	std::string calls = busctl("call", name, path, "org.freedesktop.DBus.Mock",
		"GetCalls");
	if (calls.find(std::format("\"{}\"", method)) == std::string::npos) {
		return false;
	}
	return arg == nullptr || calls.find(arg) != std::string::npos;
}

std::string mock_property(const char *name, const char *path, const char *interface,
	const char *property) {
	return busctl("get-property", name, path, interface, property);
}

} // namespace bar
