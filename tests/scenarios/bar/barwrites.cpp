// The writes a plugin makes into the shell's config with a panel open.
#include "scenarios/bar/world.h"

namespace bar {

namespace {

// What a host write that changes no widget leaves as it was: the clock's
// panel open, the shell not reloaded, and no widget made again.
void check_widgets_kept(std::string_view write) {
	check(panel_layer("clock") != nullptr,
		std::format("the clock's panel stayed open through {}", write));
	check(weather_fetch_count() == weather_fetches, std::format("{} made no "
		"widget again (the weather fetched {} times, {} before)", write,
		weather_fetch_count(), weather_fetches));
	check(settings_shell_kept(), std::format("{} reloaded nothing", write));
}

} // namespace

std::vector<harness::Step> barwrites_steps() {
	return {
		{"barwrites_shell_has_reloaded", [] {
			if (auto wait = wait_for(shell_reloaded(), {.poll_ms = 200, .budget_ms = 20000})) {
				return *wait;
			}
			check(shell_reloaded(), "the shell reloaded when the app bar went back to its edge");
			return after(100);
		}},
		{"barwrites_clock_panel_opened", [] {
			if (clock_x <= 0) {
				return end_run("the walk found no clock to open the panel from");
			}
			watch_remember_bar();
			click(clock_x, bar_mid_y());
			return after(500);
		}},
		{"barwrites_panel_is_up", [] {
			if (auto wait = wait_for(panel_layer("clock") != nullptr, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layer("clock") != nullptr,
				"the clock's panel is open before the host's writes");
			weather_fetches = weather_fetch_count();
			check(weather_fetches >= 1, std::format("the weather widget fetched "
				"when it was made ({} fetches)", weather_fetches));
			run_msg("action.run",
				R"({"name":"canvas.scratch/write","args":{"edge":"left"}})",
				"write-edge-left");
			return after(600);
		}},
		{"barwrites_on_the_left_edge_in", [] {
			if (auto wait = wait_for(toolbar_position_in_config() == "left", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(toolbar_position_in_config() == "left",
				"the scratch action moved toolbar.position to left");
			check_widgets_kept("moving the toolbar to the left edge");
			run_msg("action.run",
				R"({"name":"canvas.scratch/write","args":{"hide":"canvas.agents/agents","hidden":true}})",
				"write-hide");
			return after(600);
		}},
		{"barwrites_agents_widget_in_bar_hidden", [] {
			if (auto wait = wait_for(shell_bar_hidden().size() == 1, {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			std::vector<std::string> hidden = shell_bar_hidden();
			check(hidden.size() == 1 && hidden[0] == "canvas.agents/agents",
				"the scratch action wrote the agents widget into bar.hidden");
			check_widgets_kept("hiding the agents widget");
			run_msg("action.run",
				R"({"name":"canvas.scratch/write","args":{"hide":"canvas.agents/agents","hidden":false}})",
				"write-show");
			return after(600);
		}},
		{"barwrites_bar_hidden_empty_again", [] {
			if (auto wait = wait_for(!(!shell_bar_hidden().empty()), {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(shell_bar_hidden().empty(), "showing the agents widget emptied bar.hidden");
			check_widgets_kept("showing the agents widget");
			run_msg("action.run",
				R"({"name":"canvas.scratch/write","args":{"edge":"right"}})",
				"write-edge-right");
			return after(600);
		}},
		{"barwrites_toolbar_back", [] {
			if (auto wait = wait_for(toolbar_position_in_config() == "right", {.poll_ms = 300, .budget_ms = 15000})) {
				return *wait;
			}
			check(toolbar_position_in_config() == "right",
				"the scratch action moved toolbar.position back to right");
			check_widgets_kept("moving the toolbar back to the right edge");
			click_outside_cards(); // corner of the output, outside every card
			return after(400);
		}},
		{"barwrites_world_left_as_the_settings", [] {
			if (auto wait = wait_for(panel_layers() <= 0, {.poll_ms = 100, .budget_ms = 15000})) {
				return *wait;
			}
			check(panel_layers() == 0, "the clock's panel closed on a click outside");
			return after(100);
		}},
	};
}

} // namespace bar
