/* Unit tests for src/desktop_entry.c. Self-contained: no framework, links
 * only desktop_entry.c. Run via `meson test` (test 'desktop-entry'). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop_entry.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

#define CHECK_STR(got, want) do { \
	const char *g_ = (got), *w_ = (want); \
	if (!g_ || strcmp(g_, w_) != 0) { \
		failures++; \
		printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", \
			__FILE__, __LINE__, #got, g_ ? g_ : "(null)", w_); \
	} \
} while (0)

static void check_zeroed(const struct cv_app *app)
{
	CHECK(app->id == NULL);
	CHECK(app->name == NULL);
	CHECK(app->generic == NULL);
	CHECK(app->keywords == NULL);
	CHECK(app->exec == NULL);
	CHECK(app->icon == NULL);
	CHECK(app->try_exec == NULL);
	CHECK(!app->terminal);
}

/* ---- basic parse ---- */

static void test_parse_basic(void)
{
	static const char contents[] =
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

	struct cv_app app;
	CHECK(desktop_entry_parse(contents, "firefox.desktop", NULL, &app));
	CHECK_STR(app.id, "firefox.desktop");
	CHECK_STR(app.name, "Firefox");
	CHECK_STR(app.generic, "Web Browser");
	CHECK_STR(app.keywords, "web;internet;");
	CHECK_STR(app.exec, "firefox");
	CHECK_STR(app.try_exec, "firefox");
	CHECK_STR(app.icon, "firefox");
	CHECK(!app.terminal);
	desktop_entry_free(&app);
	check_zeroed(&app);

	/* no id argument: id stays NULL, everything else parses */
	CHECK(desktop_entry_parse(contents, NULL, "C", &app));
	CHECK(app.id == NULL);
	CHECK_STR(app.name, "Firefox");
	desktop_entry_free(&app);
}

/* ---- localization ---- */

static void test_localized(void)
{
	static const char contents[] =
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Files\n"
		"Name[de]=Dateien\n"
		"Name[de_DE]=Dateiverwaltung\n"
		"GenericName=File Manager\n"
		"GenericName[de]=Dateimanager\n"
		"Exec=nautilus\n";

	struct cv_app app;

	CHECK(desktop_entry_parse(contents, "n.desktop", "de_DE.UTF-8", &app));
	CHECK_STR(app.name, "Dateiverwaltung");
	CHECK_STR(app.generic, "Dateimanager");
	desktop_entry_free(&app);

	CHECK(desktop_entry_parse(contents, "n.desktop", "de_DE", &app));
	CHECK_STR(app.name, "Dateiverwaltung");
	desktop_entry_free(&app);

	/* de_DE@euro: the modifier key is missing, de_DE wins */
	CHECK(desktop_entry_parse(contents, "n.desktop", "de_DE@euro", &app));
	CHECK_STR(app.name, "Dateiverwaltung");
	desktop_entry_free(&app);

	CHECK(desktop_entry_parse(contents, "n.desktop", "de_DE.UTF-8@euro", &app));
	CHECK_STR(app.name, "Dateiverwaltung");
	desktop_entry_free(&app);

	/* plain de: falls back to Name[de] */
	CHECK(desktop_entry_parse(contents, "n.desktop", "de.UTF-8", &app));
	CHECK_STR(app.name, "Dateien");
	CHECK_STR(app.generic, "Dateimanager");
	desktop_entry_free(&app);

	/* other country: lang fallback, not country */
	CHECK(desktop_entry_parse(contents, "n.desktop", "de_AT.UTF-8", &app));
	CHECK_STR(app.name, "Dateien");
	desktop_entry_free(&app);

	/* unlocalized */
	CHECK(desktop_entry_parse(contents, "n.desktop", "C", &app));
	CHECK_STR(app.name, "Files");
	CHECK_STR(app.generic, "File Manager");
	desktop_entry_free(&app);
	CHECK(desktop_entry_parse(contents, "n.desktop", "POSIX", &app));
	CHECK_STR(app.name, "Files");
	desktop_entry_free(&app);
	CHECK(desktop_entry_parse(contents, "n.desktop", NULL, &app));
	CHECK_STR(app.name, "Files");
	desktop_entry_free(&app);
}

/* ---- entries that must not be listed ---- */

static void test_rejected(void)
{
	struct cv_app app;

	static const char nodisplay[] =
		"[Desktop Entry]\nType=Application\nName=X\nExec=x\nNoDisplay=true\n";
	CHECK(!desktop_entry_parse(nodisplay, "x.desktop", NULL, &app));
	check_zeroed(&app);

	static const char hidden[] =
		"[Desktop Entry]\nType=Application\nName=X\nExec=x\nHidden=true\n";
	CHECK(!desktop_entry_parse(hidden, "x.desktop", NULL, &app));
	check_zeroed(&app);

	static const char link[] =
		"[Desktop Entry]\nType=Link\nName=X\nURL=https://example.org\n";
	CHECK(!desktop_entry_parse(link, "x.desktop", NULL, &app));
	check_zeroed(&app);

	static const char no_name[] =
		"[Desktop Entry]\nType=Application\nExec=x\n";
	CHECK(!desktop_entry_parse(no_name, "x.desktop", NULL, &app));
	check_zeroed(&app);

	static const char no_exec[] =
		"[Desktop Entry]\nType=Application\nName=X\n";
	CHECK(!desktop_entry_parse(no_exec, "x.desktop", NULL, &app));
	check_zeroed(&app);

	static const char no_type[] =
		"[Desktop Entry]\nName=X\nExec=x\n";
	CHECK(!desktop_entry_parse(no_type, "x.desktop", NULL, &app));
	check_zeroed(&app);

	/* boolean matching is case-insensitive, other values are not */
	static const char nodisplay_upper[] =
		"[Desktop Entry]\nType=Application\nName=X\nExec=x\nNoDisplay=TRUE\n";
	CHECK(!desktop_entry_parse(nodisplay_upper, "x.desktop", NULL, &app));

	static const char type_space[] =
		"[Desktop Entry]\nType=application\nName=X\nExec=x\n";
	CHECK(!desktop_entry_parse(type_space, "x.desktop", NULL, &app));
}

/* ---- [Desktop Action] groups are ignored ---- */

static void test_actions_ignored(void)
{
	static const char contents[] =
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

	struct cv_app app;
	CHECK(desktop_entry_parse(contents, "app.desktop", NULL, &app));
	CHECK_STR(app.name, "App");
	CHECK_STR(app.exec, "app");
	CHECK(!app.terminal);
	desktop_entry_free(&app);

	/* localized action names must not leak into the entry either */
	CHECK(desktop_entry_parse(contents, "app.desktop", "de_DE.UTF-8", &app));
	CHECK_STR(app.name, "App");
	desktop_entry_free(&app);

	/* no [Desktop Entry] group at all */
	static const char action_only[] =
		"[Desktop Action new-window]\nName=New Window\nExec=app\n";
	CHECK(!desktop_entry_parse(action_only, "app.desktop", NULL, &app));
	check_zeroed(&app);
}

/* ---- value escapes ---- */

static void test_escapes(void)
{
	static const char contents[] =
		"[Desktop Entry]\n"
		"Type=Application\n"
		"Name=Foo\\sBar\n"
		"GenericName=line\\nbreak\n"
		"Keywords=tab\\there\n"
		"Exec=echo\\sdone\n"
		"Icon=icon\\tname\n";

	struct cv_app app;
	CHECK(desktop_entry_parse(contents, "e.desktop", NULL, &app));
	CHECK_STR(app.name, "Foo Bar");
	CHECK_STR(app.generic, "line\nbreak");
	CHECK_STR(app.keywords, "tab\there");
	CHECK_STR(app.exec, "echo done");
	CHECK_STR(app.icon, "icon\tname");
	desktop_entry_free(&app);
}

/* ---- Exec cleaning ---- */

static void test_exec_clean(void)
{
	char *s;

	s = desktop_exec_clean("firefox %u");
	CHECK_STR(s, "firefox");
	free(s);

	s = desktop_exec_clean("app \"%u\"");
	CHECK_STR(s, "app");
	free(s);

	s = desktop_exec_clean("foo %% bar");
	CHECK_STR(s, "foo % bar");
	free(s);

	s = desktop_exec_clean("app %i %c");
	CHECK_STR(s, "app");
	free(s);

	s = desktop_exec_clean("eog  %F   extra");
	CHECK_STR(s, "eog extra");
	free(s);

	s = desktop_exec_clean("code %f%F");
	CHECK_STR(s, "code");
	free(s);

	s = desktop_exec_clean("myapp %d %D %n %N %v %m %k");
	CHECK_STR(s, "myapp");
	free(s);

	s = desktop_exec_clean("  spaced %u  ");
	CHECK_STR(s, "spaced");
	free(s);

	s = desktop_exec_clean("run \"%U\" now");
	CHECK_STR(s, "run now");
	free(s);

	s = desktop_exec_clean("plain command");
	CHECK_STR(s, "plain command");
	free(s);

	s = desktop_exec_clean("keep \"a%fb\" arg");
	CHECK_STR(s, "keep \"ab\" arg");
	free(s);
}

/* ---- match ranking ---- */

static void test_match(void)
{
	struct cv_app app = {
		.id = "x.desktop",
		.name = "Firefox Web Browser",
		.generic = "Navigator",
		.keywords = "web;internet;",
		.exec = "/usr/bin/firefox",
	};

	CHECK(desktop_entry_match(&app, "fir") == 400); /* name prefix */
	CHECK(desktop_entry_match(&app, "FIREFOX") == 400); /* case-insensitive */
	CHECK(desktop_entry_match(&app, "web") == 300); /* word prefix */
	CHECK(desktop_entry_match(&app, "bro") == 300); /* word prefix */
	CHECK(desktop_entry_match(&app, "fox") == 200); /* name substring */
	CHECK(desktop_entry_match(&app, "nav") == 100); /* generic name */
	CHECK(desktop_entry_match(&app, "internet") == 100); /* keywords */
	CHECK(desktop_entry_match(&app, "zzz") == -1); /* no match */
	CHECK(desktop_entry_match(&app, "") == 0); /* empty query */
	CHECK(desktop_entry_match(&app, NULL) == 0);

	/* exec's first word basename only: 50 */
	struct cv_app tool = {
		.id = "t.desktop",
		.name = "Zed",
		.generic = "Editor",
		.keywords = "code;",
		.exec = "/opt/zed/zedbin",
	};
	CHECK(desktop_entry_match(&tool, "bin") == 50);
	CHECK(desktop_entry_match(&tool, "zedbin") == 50);
	CHECK(desktop_entry_match(&tool, "opt") == -1); /* path prefix, not basename */
	CHECK(desktop_entry_match(&tool, "zed") == 400); /* name beats exec */

	/* word boundaries: space, '-', '_', '.' */
	struct cv_app named = {
		.id = "n.desktop",
		.name = "My-App_here.now",
		.exec = "myapp",
	};
	CHECK(desktop_entry_match(&named, "app") == 300);
	CHECK(desktop_entry_match(&named, "here") == 300);
	CHECK(desktop_entry_match(&named, "now") == 300);
	CHECK(desktop_entry_match(&named, "my") == 400);
	CHECK(desktop_entry_match(&named, "App") == 300); /* case-insensitive */

	/* NULL optional fields are safe */
	struct cv_app solo = {.id = "s.desktop", .name = "Solo"};
	CHECK(desktop_entry_match(&solo, "s") == 400);
	CHECK(desktop_entry_match(&solo, "olo") == 200);
	CHECK(desktop_entry_match(&solo, "z") == -1);
	CHECK(desktop_entry_match(&solo, "") == 0);
	CHECK(desktop_entry_match(NULL, "") == 0);
	CHECK(desktop_entry_match(NULL, "x") == -1);

	/* ranking order: prefix beats word beats substring beats generic */
	struct cv_app rank = {
		.id = "r.desktop",
		.name = "web Web",
		.generic = "web",
		.exec = "web",
	};
	CHECK(desktop_entry_match(&rank, "web") == 400);
	struct cv_app rank2 = {
		.id = "r.desktop",
		.name = "a webweb",
		.generic = "web",
		.exec = "web",
	};
	CHECK(desktop_entry_match(&rank2, "web") == 300); /* word, not substring */
}

int main(void)
{
	test_parse_basic();
	test_localized();
	test_rejected();
	test_actions_ignored();
	test_escapes();
	test_exec_clean();
	test_match();

	if (failures == 0) {
		printf("all desktop-entry tests passed\n");
	} else {
		printf("%d desktop-entry test failure(s)\n", failures);
	}
	return failures != 0;
}
