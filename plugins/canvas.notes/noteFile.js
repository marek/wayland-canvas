.pragma library

// A note's file format (docs/intents/shell/notes.md): plain HTML, one page
// per note, and the two conversions its text goes through — Qt's rich text
// down to that HTML, and the HTML back up into a TextArea. Nothing here
// touches a note object or the theme, so the file can be proven on its own.

// Qt's rich text, as a TextArea holds it, down to plain HTML: the
// paragraphs, line breaks, the b, i and u spans and a span's size, and
// nothing of fonts or colours — the font is the note's, not a span's.
function cleanHtml(rich) {
	var body = rich
	var start = rich.indexOf("<body")
	if (start >= 0) {
		var end = rich.lastIndexOf("</body>")
		body = rich.substring(rich.indexOf(">", start) + 1, end >= 0 ? end : rich.length)
	}
	body = body.replace(/<span style="([^"]*)">([\s\S]*?)<\/span>/g,
		function(all, style, inner) {
			var open = "", close = ""
			// Written as px; pasted text may come in pt.
			var size = /font-size:\s*(\d+(?:\.\d+)?)(px|pt)/.exec(style)
			if (size !== null) {
				var px = Math.round(Number(size[1]) * (size[2] === "pt" ? 4 / 3 : 1))
				open += "<x-size " + px + ">"
				close = "</x-size>"
			}
			var weight = /font-weight:\s*(\d+)/.exec(style)
			if (weight !== null && Number(weight[1]) >= 600) {
				open += "<b>"
				close = "</b>" + close
			}
			if (/font-style:\s*italic/.test(style)) {
				open += "<i>"
				close = "</i>" + close
			}
			if (/text-decoration:[^;"]*underline/.test(style)) {
				open += "<u>"
				close = "</u>" + close
			}
			return open + inner + close
		})
	// Every tag but these goes, its text kept; the size marks above
	// become the plain span they stand for.
	body = body.replace(/<(\/?)([a-zA-Z][a-zA-Z0-9-]*)([^>]*)>/g,
		function(all, slash, tag, rest) {
			tag = tag.toLowerCase()
			if (tag === "br") {
				return "<br>"
			}
			if (tag === "p" || tag === "b" || tag === "i" || tag === "u") {
				return "<" + slash + tag + ">"
			}
			if (tag === "x-size") {
				return slash !== "" ? "</span>"
					: "<span style=\"font-size: " + rest.trim() + "px\">"
			}
			return ""
		})
	return body.replace(/^\s+|\s+$/g, "")
}

// Plain HTML back into a TextArea: paragraphs without Qt's own margins,
// spaces kept, an empty paragraph as one empty line.
function richHtml(html) {
	var marked = html.replace(/<p><br><\/p>/g,
		"<p style=\"-qt-paragraph-type:empty\"><br /></p>")
	return "<html><head><style>p { margin-top: 0px; margin-bottom: 0px; "
		+ "white-space: pre-wrap; }</style></head><body>" + marked + "</body></html>"
}

function cssString(text) {
	return "\"" + String(text).replace(/\\/g, "\\\\").replace(/"/g, "\\\"") + "\""
}

// The whole file: the text, the colour, the font and the size of the
// text that names none as the page's style, so a browser shows the
// note as the card does, and the note's canvas rectangle in a meta
// element. `note` holds x, y, width, height, color, font, size and html.
function encode(note, ink) {
	return "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
		+ "<meta name=\"canvas-note\" content=\"x=" + note.x + " y=" + note.y
		+ " width=" + note.width + " height=" + note.height + "\">\n"
		+ "<style>\nbody { background: " + note.color + "; color: " + ink
		+ "; font-family: " + cssString(note.font) + "; font-size: " + note.size
		+ "px; }\np { margin-top: 0; margin-bottom: 0; white-space: pre-wrap; }\n"
		+ "</style>\n</head>\n<body>\n" + note.html + "\n</body>\n</html>\n"
}

// A file back into the fields of a note; what it leaves out takes
// `defaults` (the same fields), so a page written by hand is a note too.
function parse(text, defaults) {
	var place = {}
	var meta = /<meta\s+name="canvas-note"\s+content="([^"]*)"/i.exec(text)
	if (meta !== null) {
		meta[1].replace(/(\w+)=(-?\d+)/g, function(all, key, value) {
			place[key] = Number(value)
		})
	}
	var style = /<style[^>]*>([\s\S]*?)<\/style>/i.exec(text)
	var rule = style !== null ? /body\s*\{([^}]*)\}/.exec(style[1]) : null
	var css = rule !== null ? rule[1] : ""
	var background = /background(?:-color)?:\s*(#[0-9a-fA-F]{3,8})/.exec(css)
	var family = /font-family:\s*"((?:[^"\\]|\\.)*)"/.exec(css)
	var px = /font-size:\s*(\d+)px/.exec(css)
	var body = /<body[^>]*>([\s\S]*?)<\/body>/i.exec(text)
	return {
		color: background !== null ? background[1] : defaults.color,
		html: body !== null ? body[1].replace(/^\s+|\s+$/g, "") : "",
		x: place.x !== undefined ? place.x : defaults.x,
		y: place.y !== undefined ? place.y : defaults.y,
		width: place.width > 0 ? place.width : defaults.width,
		height: place.height > 0 ? place.height : defaults.height,
		font: family !== null ? family[1].replace(/\\(.)/g, "$1") : defaults.font,
		size: px !== null ? Number(px[1]) : defaults.size,
	}
}
