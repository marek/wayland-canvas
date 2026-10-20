// Unit tests for the join of the settings registry to the actions behind it
// (src/input/bindings.cpp): every registry action has a runner, and the
// table an action's chords are put in follows from its scope and reach, never
// from its group label. Run via `meson test` (test 'bindings').

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "core/settings.h"
#include "input/bindings.h"

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
	if (!condition) {
		++failures;
		std::printf("FAIL %s\n", what.c_str());
	}
}

std::string named(const settings::ActionDef &def) {
	return std::string{def.scope == settings::Scope::global ? "[shortcuts] " : "[overview] "}
		+ std::string{def.name};
}

const settings::ActionDef &action(std::string_view name,
		settings::Scope scope = settings::Scope::global) {
	for (const settings::ActionDef &def : settings::registry()) {
		if (def.scope == scope && def.name == name) {
			return def;
		}
	}
	std::printf("FAIL the registry has no action '%s'\n", std::string{name}.c_str());
	std::exit(1);
}

// A key bound to a row with no runner would reach the client instead, so a
// row added to the registry without its runner fails here.
void every_action_has_a_runner() {
	for (const settings::ActionDef &def : settings::registry()) {
		check(Bindings::has_runner(def), named(def) + " has a runner");
	}
	settings::ActionDef orphan{settings::Scope::global, "no-such-action",
		"Nothing", "Nowhere", {}};
	check(!Bindings::has_runner(orphan), "an action nothing runs has no runner");
	orphan.scope = settings::Scope::overview;
	orphan.name = "go";
	check(Bindings::has_runner(orphan), "the overview's go has one");
	orphan.name = "launcher";
	check(!Bindings::has_runner(orphan),
		"a runner belongs to its own scope: the launcher is not an overview action");
}

// The capture actions are tried before all else, the launcher and the
// terminal reach the overview, the overview's own actions go in its table
// and every other action in the global one.
void tables_follow_scope_and_reach() {
	using Table = Bindings::Table;
	for (std::string_view name : {"shot-screen", "shot-screen-copy", "capture-pick",
			"capture-pick-copy", "capture-toolbar", "capture-toolbar-copy",
			"record-stop", "record-stop-copy"}) {
		const settings::ActionDef &def = action(name);
		check(def.reach == settings::Reach::everywhere, std::string{name} + " reaches everywhere");
		check(Bindings::table_of(def) == Table::everywhere,
			std::string{name} + " is tried before all else");
	}
	for (std::string_view name : {"launcher", "terminal"}) {
		const settings::ActionDef &def = action(name);
		check(def.reach == settings::Reach::overview, std::string{name} + " reaches the overview");
		check(Bindings::table_of(def) == Table::global, std::string{name} + " is a global action");
	}
	check(Bindings::table_of(action("close")) == Table::global, "close is a global action");
	check(Bindings::table_of(action("go", settings::Scope::overview)) == Table::overview,
		"the overview's go is in the overview table");
	check(Bindings::table_of(action("hop-left", settings::Scope::overview)) == Table::overview,
		"the overview's hop-left is in the overview table");
	for (const settings::ActionDef &def : settings::registry()) {
		check(def.scope == settings::Scope::global || def.reach == settings::Reach::scope,
			named(def) + " is an overview action with a reach of its own");
	}
}

// The group is how the settings window names a section: renaming it changes
// the label and nothing about where the chords apply or what they run.
void renaming_the_group_changes_only_the_label() {
	for (const settings::ActionDef &def : settings::registry()) {
		settings::ActionDef renamed = def;
		renamed.group = "Renamed";
		check(renamed.group != def.group, named(def) + " has another label");
		check(Bindings::table_of(renamed) == Bindings::table_of(def),
			named(def) + " lands in the same table under another group");
		check(Bindings::has_runner(renamed) == Bindings::has_runner(def),
			named(def) + " keeps its runner under another group");
		check(renamed.reach == def.reach, named(def) + " keeps its reach under another group");
	}
}

} // namespace

int main() {
	every_action_has_a_runner();
	tables_follow_scope_and_reach();
	renaming_the_group_changes_only_the_label();

	if (failures == 0) {
		std::printf("ALL PASSED\n");
		return 0;
	}
	std::printf("%d failure(s)\n", failures);
	return 1;
}
