// Keeps the answer to a question from the user pinned above the input until they dismiss it (the answering-questions skill).
import type { ExtensionAPI, ExtensionContext } from "@oh-my-pi/pi-coding-agent";
import { Text } from "@oh-my-pi/pi-tui";

interface Pin {
	id: string;
	question: string;
	answer: string;
}

const WIDGET = "pinned-answers";
const DISMISSED = "wayland-canvas.pinned-answers.dismissed";
const REMINDER = "wayland-canvas.pinned-answers.reminder";
// the widget shows ten entries at most: the pins, the overflow line and the hint
const MAX_SHOWN = 8;
const RULE =
	"If this message asks a question - how far the work has got, why something was done, why something else was not done instead, what something means - it gets an answer, never a change (skill://answering-questions): answer it with the `answer` tool before anything else, change nothing because of it, then carry on with work that was already under way; an idle session stops after the answer.";

export default function pinnedAnswers(pi: ExtensionAPI): void {
	const z = pi.zod;
	const pinShape = z.object({ id: z.string(), question: z.string(), answer: z.string() });
	const dismissedShape = z.object({ ids: z.array(z.string()) });
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
				const dismissed = dismissedShape.safeParse(entry.data);
				if (dismissed.success) for (const id of dismissed.data.ids) live.delete(id);
				continue;
			}
			if (entry.type !== "message" || entry.message.role !== "toolResult") continue;
			if (entry.message.toolName !== "answer" || entry.message.isError) continue;
			const pin = pinShape.safeParse(entry.message.details);
			if (pin.success) live.set(pin.data.id, pin.data);
		}
		pins = [...live.values()];
		show(ctx);
	}

	pi.on("session_start", restore);
	pi.on("session_switch", restore);
	pi.on("session_branch", restore);
	pi.on("session_tree", restore);

	pi.on("before_agent_start", (_event, ctx) => {
		if (ctx.agent.kind !== "main") return;
		return { message: { customType: REMINDER, content: RULE, display: false } };
	});

	pi.registerTool({
		name: "answer",
		label: "Answer",
		description:
			"Pin the answer to a question from the user above the input, where it stays while work goes on until the user dismisses it. Use it for every question the user asks - how far the work has got, why something was done or not done, what something means - before anything else. A question is never a reason to edit, revert or start anything.",
		parameters: z.object({
			question: z.string().describe("The user's question in a few words"),
			answer: z.string().describe("Plain text: the conclusion in the first line, a few lines in all"),
		}),
		loadMode: "essential",
		approval: "read",
		async execute(toolCallId, params, _signal, _onUpdate, ctx) {
			const pin = { id: toolCallId, question: params.question.trim(), answer: params.answer.trim() };
			pins.push(pin);
			show(ctx);
			const text = `Pinned above the input as answer ${pins.length} until the user dismisses it; the reply does not repeat it.`;
			return { content: [{ type: "text", text }], details: pin };
		},
		// the transcript keeps the question and the whole answer; the result text is meant for the model
		renderCall(args, _options, theme) {
			return new Text(`${theme.fg("toolTitle", theme.bold("Answer"))} ${theme.fg("accent", args.question ?? "")}`, 0, 0);
		},
		renderResult(result, _options, theme) {
			const pin = pinShape.safeParse(result.details);
			if (pin.success) return new Text(pin.data.answer, 0, 0);
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
