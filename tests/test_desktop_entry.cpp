// Unit tests for src/apps/desktop_entry.cpp. Self-contained: no framework, links
// only desktop_entry.cpp. Run via `meson test` (test 'desktop-entry').

#include <cstdio>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include "apps/desktop_entry.h"

namespace {

int failures = 0;

void check(bool cond, std::source_location loc = std::source_location::current()) {
	if (!cond) {
		failures++;
		std::printf("FAIL %s:%u: %s\n", loc.file_name(), loc.line(),
			loc.function_name());
	}
}

void check_str(std::string_view got, std::string_view want,
	std::source_location loc = std::source_location::current()) {
	if (got != want) {
		failures++;
		std::printf("FAIL %s:%u: \"%s\", expected \"%s\"\n", loc.file_name(),
			loc.line(), std::string(got).c_str(), std::string(want).c_str());
	}
}

/* ---- basic parse ---- */

void test_parse_basic() {
	constexpr std::string_view contents =
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Firefox\n"
		"GenericName=Web Browser\n"
		"Keywords=web;internet;\n"
		"Exec=firefox %u\n"
		"TryExec=firefox\n"
		"Icon=firefox\n"
		"Terminal=false\n"
		"\n"
		"# trailing comment\n";

	std::optional<App> app = App::parse(contents, "firefox.desktop", "");
	check(bool(app));
	check_str(app->id, "firefox.desktop");
	check_str(app->name, "Firefox");
	check_str(app->generic, "Web Browser");
	check_str(app->keywords, "web;internet;");
	check_str(app->exec, "firefox");
	check_str(app->try_exec, "firefox");
	check_str(app->icon, "firefox");
	check(!app->terminal);

	/* no id argument: id stays empty, everything else parses */
	app = App::parse(contents, "", "C");
	check(app->id.empty());
	check_str(app->name, "Firefox");
}

/* ---- localization ---- */

void test_localized() {
	constexpr std::string_view contents =
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Files\n"
		"Name[de]=Dateien\n"
		"Name[de_DE]=Dateiverwaltung\n"
		"GenericName=File Manager\n"
		"GenericName[de]=Dateimanager\n"
		"Exec=nautilus\n";

	std::optional<App> app = App::parse(contents, "n.desktop", "de_DE.UTF-8");
	check(bool(app));
	check_str(app->name, "Dateiverwaltung");
	check_str(app->generic, "Dateimanager");

	app = App::parse(contents, "n.desktop", "de_DE");
	check_str(app->name, "Dateiverwaltung");

	/* de_DE@euro: the modifier key is missing, de_DE wins */
	app = App::parse(contents, "n.desktop", "de_DE@euro");
	check_str(app->name, "Dateiverwaltung");

	app = App::parse(contents, "n.desktop", "de_DE.UTF-8@euro");
	check_str(app->name, "Dateiverwaltung");

	/* plain de: falls back to Name[de] */
	app = App::parse(contents, "n.desktop", "de.UTF-8");
	check_str(app->name, "Dateien");
	check_str(app->generic, "Dateimanager");

	/* other country: lang fallback, not country */
	app = App::parse(contents, "n.desktop", "de_AT.UTF-8");
	check_str(app->name, "Dateien");

	/* unlocalized */
	app = App::parse(contents, "n.desktop", "C");
	check_str(app->name, "Files");
	check_str(app->generic, "File Manager");
	app = App::parse(contents, "n.desktop", "POSIX");
	check_str(app->name, "Files");
	app = App::parse(contents, "n.desktop", "");
	check_str(app->name, "Files");
}

/* ---- entries that must not be listed ---- */

void test_rejected() {
	constexpr std::string_view nodisplay =
		"[Desktop Entry]\nType=Application\nName=X\nExec=x\nNoDisplay=true\n";
	check(!App::parse(nodisplay, "x.desktop", ""));

	constexpr std::string_view hidden =
		"[Desktop Entry]\nType=Application\nName=X\nExec=x\nHidden=true\n";
	check(!App::parse(hidden, "x.desktop", ""));

	constexpr std::string_view link =
		"[Desktop Entry]\nType=Link\nName=X\nURL=https://example.org\n";
	check(!App::parse(link, "x.desktop", ""));

	constexpr std::string_view no_name =
		"[Desktop Entry]\nType=Application\nExec=x\n";
	check(!App::parse(no_name, "x.desktop", ""));

	constexpr std::string_view no_exec =
		"[Desktop Entry]\nType=Application\nName=X\n";
	check(!App::parse(no_exec, "x.desktop", ""));

	constexpr std::string_view no_type =
		"[Desktop Entry]\nName=X\nExec=x\n";
	check(!App::parse(no_type, "x.desktop", ""));

	/* boolean matching is case-insensitive, other values are not */
	constexpr std::string_view nodisplay_upper =
		"[Desktop Entry]\nType=Application\nName=X\nExec=x\nNoDisplay=TRUE\n";
	check(!App::parse(nodisplay_upper, "x.desktop", ""));

	constexpr std::string_view type_space =
		"[Desktop Entry]\nType=application\nName=X\nExec=x\n";
	check(!App::parse(type_space, "x.desktop", ""));
}

/* ---- [Desktop Action] groups are ignored ---- */

void test_actions_ignored() {
	constexpr std::string_view contents =
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=App\n"
		"Exec=app\n"
		"\n"
		"[Desktop Action new-window]\n"
		"Name=New Window\n"
		"Name[de]=Neues Fenster\n"
		"Exec=app --new-window\n"
		"NoDisplay=true\n"
		"Hidden=true\n";

	std::optional<App> app = App::parse(contents, "app.desktop", "");
	check(bool(app));
	check_str(app->name, "App");
	check_str(app->exec, "app");
	check(!app->terminal);

	/* localized action names must not leak into the entry either */
	app = App::parse(contents, "app.desktop", "de_DE.UTF-8");
	check_str(app->name, "App");

	/* no [Desktop Entry] group at all */
	constexpr std::string_view action_only =
		"[Desktop Action new-window]\nName=New Window\nExec=app\n";
	check(!App::parse(action_only, "app.desktop", ""));
}

/* ---- value escapes ---- */

void test_escapes() {
	constexpr std::string_view contents =
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Foo\\sBar\n"
		"GenericName=line\\nbreak\n"
		"Keywords=tab\\there\n"
		"Exec=echo\\sdone\n"
		"Icon=icon\\tname\n";

	std::optional<App> app = App::parse(contents, "e.desktop", "");
	check(bool(app));
	check_str(app->name, "Foo Bar");
	check_str(app->generic, "line\nbreak");
	check_str(app->keywords, "tab\there");
	check_str(app->exec, "echo done");
	check_str(app->icon, "icon\tname");
}

/* ---- Exec cleaning ---- */

void test_exec_clean() {
	check_str(clean_exec("firefox %u"), "firefox");
	check_str(clean_exec("app \"%u\""), "app");
	check_str(clean_exec("foo %% bar"), "foo % bar");
	check_str(clean_exec("app %i %c"), "app");
	check_str(clean_exec("eog  %F   extra"), "eog extra");
	check_str(clean_exec("code %f%F"), "code");
	check_str(clean_exec("myapp %d %D %n %N %v %m %k"), "myapp");
	check_str(clean_exec("  spaced %u  "), "spaced");
	check_str(clean_exec("run \"%U\" now"), "run now");
	check_str(clean_exec("plain command"), "plain command");
	check_str(clean_exec("keep \"a%fb\" arg"), "keep \"ab\" arg");
}

/* ---- match ranking ---- */

void test_match() {
	const App app{
		.id = "x.desktop",
		.name = "Firefox Web Browser",
		.generic = "Navigator",
		.keywords = "web;internet;",
		.exec = "/usr/bin/firefox",
	};

	check(app.match("fir") == 400); /* name prefix */
	check(app.match("FIREFOX") == 400); /* case-insensitive */
	check(app.match("web") == 300); /* word prefix */
	check(app.match("bro") == 300); /* word prefix */
	check(app.match("fox") == 200); /* name substring */
	check(app.match("nav") == 100); /* generic name */
	check(app.match("internet") == 100); /* keywords */
	check(app.match("zzz") == -1); /* no match */
	check(app.match("") == 0); /* empty query */

	/* exec's first word basename only: 50 */
	const App tool{
		.id = "t.desktop",
		.name = "Zed",
		.generic = "Editor",
		.keywords = "code;",
		.exec = "/opt/zed/zedbin",
	};
	check(tool.match("bin") == 50);
	check(tool.match("zedbin") == 50);
	check(tool.match("opt") == -1); /* path prefix, not basename */
	check(tool.match("zed") == 400); /* name beats exec */

	/* word boundaries: space, '-', '_', '.' */
	const App named{
		.id = "n.desktop",
		.name = "My-App_here.now",
		.exec = "myapp",
	};
	check(named.match("app") == 300);
	check(named.match("here") == 300);
	check(named.match("now") == 300);
	check(named.match("my") == 400);
	check(named.match("App") == 300); /* case-insensitive */

	/* empty optional fields are safe */
	const App solo{.id = "s.desktop", .name = "Solo"};
	check(solo.match("s") == 400);
	check(solo.match("olo") == 200);
	check(solo.match("z") == -1);
	check(solo.match("") == 0);

	/* ranking order: prefix beats word beats substring beats generic */
	const App rank{
		.id = "r.desktop",
		.name = "web Web",
		.generic = "web",
		.exec = "web",
	};
	check(rank.match("web") == 400);
	const App rank2{
		.id = "r.desktop",
		.name = "a webweb",
		.generic = "web",
		.exec = "web",
	};
	check(rank2.match("web") == 300); /* word, not substring */
}

} // namespace

int main() {
	test_parse_basic();
	test_localized();
	test_rejected();
	test_actions_ignored();
	test_escapes();
	test_exec_clean();
	test_match();

	if (failures == 0) {
		std::printf("all desktop-entry tests passed\n");
	} else {
		std::printf("%d desktop-entry test failure(s)\n", failures);
	}
	return failures != 0;
}
