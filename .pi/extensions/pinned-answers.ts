// Keeps the answer to a question from the user pinned above the input until they dismiss it (the answering-questions skill).
import type { ExtensionAPI, ExtensionContext } from "@earendil-works/pi-coding-agent";
import { Text } from "@earendil-works/pi-tui";
import { Type, type Static } from "typebox";
import { Value } from "typebox/value";

interface Pin {
	id: string;
	question: string;
	answer: string;
}

const PinSchema = Type.Object({ id: Type.String(), question: Type.String(), answer: Type.String() });
const DismissedSchema = Type.Object({ ids: Type.Array(Type.String()) });

const WIDGET = "pinned-answers";
const DISMISSED = "wayland-canvas.pinned-answers.dismissed";
const REMINDER = "wayland-canvas.pinned-answers.reminder";
// the widget shows ten entries at most: the pins, the overflow line and the hint
const MAX_SHOWN = 8;
const RULE =
	"If this message asks a question - how far the work has got, why something was done, why something else was not done instead, what something means - it gets an answer, never a change (skill://answering-questions): answer it with the `answer` tool before anything else, change nothing because of it, then carry on with work that was already under way; an idle session stops after the answer.";

export default function pinnedAnswers(pi: ExtensionAPI): void {
	const AnswerParams = Type.Object({
		question: Type.String({ description: "The user's question in a few words" }),
		answer: Type.String({ description: "Plain text: the conclusion in the first line, a few lines in all" }),
	});
	let pins: Pin[] = [];

	function show(ctx: ExtensionContext): void {
		if (pins.length === 0) {
			ctx.ui.setWidget(WIDGET, undefined);
			return;
		}
		const { theme } = ctx.ui;
		const hidden = Math.max(0, pins.length - MAX_SHOWN);
		const lines = pins
			.slice(hidden)
			.map((pin, i) => `${theme.fg("accent", theme.bold(`${hidden + i + 1}. ${pin.question}`))}\n${pin.answer}`);
		if (hidden > 0) lines.unshift(theme.fg("muted", `Earlier pinned answers not shown: ${hidden}`));
		const hint = pins.length === 1 ? "/dismiss or Alt+X closes it" : "/dismiss or Alt+X closes all · /dismiss 2 closes the second";
		lines.push(theme.fg("dim", hint));
		ctx.ui.setWidget(WIDGET, lines);
	}

	function dismiss(ids: string[], ctx: ExtensionContext): void {
		if (ids.length === 0) {
			ctx.ui.notify("No answers are pinned", "info");
			return;
		}
		pi.appendEntry(DISMISSED, { ids });
		pins = pins.filter(pin => !ids.includes(pin.id));
		show(ctx);
	}

	// the pins are the answer results on the current branch, less the dismissed ones
	function restore(_event: unknown, ctx: ExtensionContext): void {
		const live = new Map<string, Pin>();
		for (const entry of ctx.sessionManager.getBranch()) {
			if (entry.type === "custom" && entry.customType === DISMISSED) {
				if (Value.Check(DismissedSchema, entry.data)) {
					for (const id of (entry.data as Static<typeof DismissedSchema>).ids) live.delete(id);
				}
				continue;
			}
			if (entry.type !== "message" || entry.message.role !== "toolResult") continue;
			if (entry.message.toolName !== "answer" || entry.message.isError) continue;
			if (Value.Check(PinSchema, entry.message.details)) live.set(entry.message.details.id, entry.message.details);
		}
		pins = [...live.values()];
		show(ctx);
	}

	pi.on("session_start", restore);
	pi.on("session_tree", restore);

	pi.on("before_agent_start", (_event, ctx) => {
		return { message: { customType: REMINDER, content: RULE, display: false } };
	});

	pi.registerTool({
		name: "answer",
		label: "Answer",
		description:
			"Pin the answer to a question from the user above the input, where it stays while work goes on until the user dismisses it. Use it for every question the user asks - how far the work has got, why something was done or not done, what something means - before anything else. A question is never a reason to edit, revert or start anything.",
		parameters: AnswerParams,
		annotations: { readOnlyHint: true, destructiveHint: false, openWorldHint: false },
		async execute(toolCallId, params, _signal, _onUpdate, ctx) {
			const pin: Pin = { id: toolCallId, question: params.question.trim(), answer: params.answer.trim() };
			pins.push(pin);
			show(ctx);
			const text = `Pinned above the input as answer ${pins.length} until the user dismisses it; the reply does not repeat it.`;
			return { content: [{ type: "text", text }], details: pin };
		},
		// the transcript keeps the question and the whole answer; the result text is meant for the model
		renderCall(args, theme) {
			return new Text(`${theme.fg("toolTitle", theme.bold("Answer"))} ${theme.fg("accent", args.question ?? "")}`, 0, 0);
		},
		renderResult(result, _options, theme) {
			if (Value.Check(PinSchema, result.details)) return new Text(result.details.answer, 0, 0);
			const text = result.content.map(part => (part.type === "text" ? part.text : "")).join("");
			return new Text(theme.fg("error", text), 0, 0);
		},
	});

	pi.registerCommand("dismiss", {
		description: "Close pinned answers: all of them, or the numbered ones (/dismiss 2)",
		handler: async (args, ctx) => {
			const numbers = args.split(/[\s,]+/).filter(Boolean);
			if (numbers.length === 0) return dismiss(pins.map(pin => pin.id), ctx);
			const picked = numbers.map(n => pins[Number(n) - 1]);
			if (picked.some(pin => pin === undefined)) {
				ctx.ui.notify(`Pinned answers are numbered 1 to ${pins.length}`, "warning");
				return;
			}
			dismiss(picked.map(pin => pin.id), ctx);
		},
	});

	pi.registerShortcut("alt+x", {
		description: "Close all pinned answers",
		handler: ctx => dismiss(pins.map(pin => pin.id), ctx),
	});
}
