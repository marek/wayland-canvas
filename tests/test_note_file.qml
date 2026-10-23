import QtQuick
import QtTest
import "../plugins/canvas.notes/noteFile.js" as NoteFile

// plugins/canvas.notes/noteFile.js: a note's plain HTML through Qt's rich
// text and back, and the file written around it. Run by qmltestrunner
// (meson test -C build note-file).
TestCase {
	id: root
	name: "NoteFile"

	Component {
		id: editorComponent

		TextEdit {
			textFormat: TextEdit.RichText
			font.family: "Serif"
			font.pixelSize: 14
			color: "#123456"
		}
	}

	readonly property var defaults: ({
		color: "#a6e3a1",
		x: 0,
		y: 0,
		width: 220,
		height: 220,
		font: "Default Font",
		size: 13,
	})

	function note(overrides) {
		return Object.assign({
			color: "#ffd866",
			x: -12,
			y: 34,
			width: 260,
			height: 180,
			font: "Inter",
			size: 14,
			html: "<p>hello</p>",
		}, overrides)
	}

	// Every field of a note, and no other.
	function sameNote(actual, expected) {
		var fields = ["color", "x", "y", "width", "height", "font", "size", "html"]
		compare(Object.keys(actual).sort().join(), fields.slice().sort().join())
		for (var i = 0; i < fields.length; i++) {
			compare(actual[fields[i]], expected[fields[i]], fields[i])
		}
	}

	// What a TextArea holds for `html` as the plugin loads it, written back
	// as the plugin saves it. Qt puts a line break between paragraphs.
	function throughEditor(html) {
		var edit = createTemporaryObject(editorComponent, root)
		edit.text = NoteFile.richHtml(html)
		return NoteFile.cleanHtml(edit.text)
	}

	function withoutBreaks(html) {
		return html.replace(/\n/g, "")
	}

	function test_text_survives_the_editor_data() {
		return [
			{tag: "plain", html: "<p>plain</p>"},
			{tag: "mixed spans",
				html: "<p>one <b>bold</b> <i>it</i> <u>un</u> "
					+ "<span style=\"font-size: 20px\">big</span></p>"
					+ "<p><b><i>both</i></b> end</p>"},
			{tag: "every attribute on one span",
				html: "<p>a <span style=\"font-size: 20px\"><b><i><u>mix</u></i></b></span> z</p>"},
			{tag: "neighbouring spans", html: "<p><b>a</b><i>b</i><b>c</b></p>"},
			{tag: "empty paragraphs", html: "<p>a</p><p><br></p><p><br></p><p>b<br>c</p>"},
			{tag: "empty paragraphs at both ends", html: "<p><br></p><p>x</p><p><br></p>"},
			{tag: "line breaks", html: "<p>x<br><br>y</p>"},
			{tag: "spaces", html: "<p>  two  spaces</p>"},
			{tag: "entities", html: "<p>a &lt; b &amp; c</p>"},
		]
	}

	function test_text_survives_the_editor(data) {
		compare(withoutBreaks(throughEditor(data.html)), data.html)
	}

	// Loading the saved text again must not change it: a text that grew a
	// line per reload would show here.
	function test_text_is_stable_over_reloads_data() {
		return test_text_survives_the_editor_data()
	}

	function test_text_is_stable_over_reloads(data) {
		var once = throughEditor(data.html)
		compare(throughEditor(once), once)
		compare(throughEditor(throughEditor(once)), once)
	}

	function test_no_font_family_or_colour_is_kept() {
		var edit = createTemporaryObject(editorComponent, root)
		edit.text = "<p><span style=\"font-family:'Courier New'; color:#ff0000; "
			+ "background-color:#00ff00; font-size:20px; font-weight:700\">x</span> y</p>"
		// the editor does hold them, so the cleaning is what drops them
		verify(edit.text.indexOf("Courier New") >= 0)
		verify(edit.text.indexOf("#ff0000") >= 0)
		var clean = NoteFile.cleanHtml(edit.text)
		compare(clean, "<p><span style=\"font-size: 20px\"><b>x</b></span> y</p>")
		verify(clean.indexOf("font-family") < 0)
		verify(clean.indexOf("color") < 0)
	}

	// An empty paragraph is a paragraph with nothing in it: one holding a
	// line break is a second line, and the text grows by it at each load.
	function test_an_empty_paragraph_is_one_empty_line() {
		var edit = createTemporaryObject(editorComponent, root)
		edit.text = NoteFile.richHtml("<p>a</p><p><br></p><p>b<br>c</p>")
		compare(edit.getText(0, edit.length).split("\u2029").join("|"), "a||b\u2028c")
	}

	function test_sizes_in_points_become_pixels() {
		compare(NoteFile.cleanHtml("<p><span style=\" font-size:15pt;\">x</span></p>"),
			"<p><span style=\"font-size: 20px\">x</span></p>")
		compare(NoteFile.cleanHtml("<p><span style=\" font-size:11.5px;\">x</span></p>"),
			"<p><span style=\"font-size: 12px\">x</span></p>")
	}

	function test_bold_starts_at_weight_600() {
		compare(NoteFile.cleanHtml("<p><span style=\" font-weight:600;\">x</span></p>"),
			"<p><b>x</b></p>")
		compare(NoteFile.cleanHtml("<p><span style=\" font-weight:500;\">x</span></p>"),
			"<p>x</p>")
	}

	function test_tags_that_are_not_kept_go_with_their_text_kept() {
		compare(NoteFile.cleanHtml("<h1>big</h1><p><a href=\"x\">link</a> <font color=\"red\">red</font></p>"),
			"big<p>link red</p>")
	}

	function test_the_file_is_plain_html() {
		var file = NoteFile.encode(note({html: "<p>one <b>two</b></p>"}), "#1f2430")
		verify(file.indexOf("<!DOCTYPE html>") === 0)
		verify(file.indexOf("<meta name=\"canvas-note\" content=\"x=-12 y=34 width=260 height=180\">") > 0)
		verify(file.indexOf("body { background: #ffd866; color: #1f2430; "
			+ "font-family: \"Inter\"; font-size: 14px; }") > 0)
		verify(file.indexOf("<body>\n<p>one <b>two</b></p>\n</body>") > 0)
	}

	function test_a_file_parses_back_to_its_note_data() {
		return [
			{tag: "plain", note: note({})},
			{tag: "mixed text", note: note({
				html: "<p>a <span style=\"font-size: 20px\"><b><i>x</i></b></span></p>\n<p><br></p>\n<p>z</p>"})},
			{tag: "a font with quotes and a backslash", note: note({font: "Say \"hi\" \\ there"})},
			{tag: "an empty note", note: note({html: ""})},
			{tag: "a short colour", note: note({color: "#fa0"})},
		]
	}

	function test_a_file_parses_back_to_its_note(data) {
		sameNote(NoteFile.parse(NoteFile.encode(data.note, "#1f2430"), defaults), data.note)
	}

	function test_what_a_page_leaves_out_takes_the_defaults() {
		sameNote(NoteFile.parse("<html><body><p>by hand</p></body></html>", defaults),
			Object.assign({}, defaults, {html: "<p>by hand</p>"}))
		sameNote(NoteFile.parse("", defaults), Object.assign({}, defaults, {html: ""}))
	}

	function test_a_rectangle_without_a_size_takes_the_default_size() {
		var page = "<html><head><meta name=\"canvas-note\" content=\"x=5 y=-6 width=0 height=-3\">"
			+ "</head><body></body></html>"
		var parsed = NoteFile.parse(page, defaults)
		compare(parsed.x, 5)
		compare(parsed.y, -6)
		compare(parsed.width, defaults.width)
		compare(parsed.height, defaults.height)
	}

	function test_a_page_styled_by_hand_is_read() {
		var page = "<html><head><style>body { background-color: #abc; "
			+ "font-family: \"Serif\"; font-size: 17px }</style></head><body>x</body></html>"
		var parsed = NoteFile.parse(page, defaults)
		compare(parsed.color, "#abc")
		compare(parsed.font, "Serif")
		compare(parsed.size, 17)
		compare(parsed.html, "x")
	}
}
