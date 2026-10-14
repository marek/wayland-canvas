#ifndef CV_DESKTOP_ENTRY_H
#define CV_DESKTOP_ENTRY_H

#include <stdbool.h>

/* Pure XDG desktop-entry handling (no wlroots), unit-tested in
 * tests/test_desktop_entry.c. */

struct cv_app {
	char *id; /* desktop file id, e.g. "org.gnome.Nautilus.desktop" */
	char *name; /* Name, localized for `lang` when the file has it */
	char *generic; /* GenericName or NULL */
	char *keywords; /* Keywords (';'-separated as in the file) or NULL */
	char *exec; /* Exec with field codes removed, for /bin/sh -c */
	char *icon; /* Icon: theme name or absolute path, or NULL */
	char *try_exec; /* TryExec or NULL; apps.c drops entries whose binary is missing */
	bool terminal; /* Terminal=true */
};

/*
 * Parses the [Desktop Entry] group of a .desktop file. `lang` is the locale
 * (e.g. "de_DE.UTF-8", "de"; NULL/"C" for none): Name[de_DE]/Name[de] win over
 * Name. Returns false, leaving *app zeroed, for entries that must not be
 * listed: Type other than Application, NoDisplay=true, Hidden=true, or a
 * missing Name or Exec.
 */
bool desktop_entry_parse(const char *contents, const char *id, const char *lang,
	struct cv_app *app);

/* Exec value with %f %F %u %U %d %D %n %N %i %c %k %v %m removed and "%%"
 * turned into "%". Caller frees. */
char *desktop_exec_clean(const char *exec);

/*
 * How well `query` (case-insensitive, may be empty) matches: -1 no match,
 * otherwise higher is better. Empty query matches everything equally.
 * Ranking: name prefix > word prefix in name > substring of name >
 * generic name / keywords / exec match.
 */
int desktop_entry_match(const struct cv_app *app, const char *query);

void desktop_entry_free(struct cv_app *app);

#endif
