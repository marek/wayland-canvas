# Answering questions

Date: 2026-10-16 19:29 EDT

## Context

A question from the user — how far the work has got, why something was done, why something else was not done
instead — is answered, never acted on: nothing is changed, reverted or started because of it. The answer leads with
the conclusion, takes a few lines, and stays in view until the user dismisses it, while work that was already under
way carries on. The rule is a skill, so every agent working here follows it. In omp a project extension keeps each
answer pinned above the input until it is dismissed; in other harnesses the answer ends the turn.

## Work

- [x] The `answering-questions` skill: what counts as a question, what an answer may and may not change, its shape, where it is kept in view
- [x] The omp extension `.omp/extensions/pinned-answers.ts`: the `answer` tool pins a question and its answer above the input; `/dismiss` (all, or by number) and Alt+X remove them; pins survive a restart and follow `/tree`; the rule is repeated to the model with every prompt
- [x] `AGENTS.md` skills table, `repos.md` (the extension is committed), the autopilot skill and the Claude reminder hook point at the skill

## Verification

- [x] An RPC run of omp in the repo: the extension loads, `answer` sends the pin, `/dismiss` removes it, the reminder reaches the model
- [x] A TUI run: a question asked while work is under way is answered by a pin with nothing changed, the pin stays while the work goes on, Alt+X and `/dismiss` clear it
- [ ] The transcript view of an answer (question as its title, the whole answer below) seen on screen (the user)
