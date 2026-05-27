// notes.js — sticky-notes manager backed by /home/.oxnotes.json.
// New: ox.ui.prompt() to add. Click a note to delete it.
var W = 700, H = 500;
var win = ox.window("Notes", W, H);

var NOTES_PATH = "/home/.oxnotes.json";
var notes = [];

function load() {
    var raw = ox.fs.readFile(NOTES_PATH);
    if (!raw) { notes = []; return; }
    try { notes = JSON.parse(raw); } catch (e) { notes = []; }
    if (!notes || !notes.length) notes = [];
}
function save() {
    ox.fs.writeFile(NOTES_PATH, JSON.stringify(notes));
}

function addNote() {
    var text = ox.ui.prompt("New Note", "Type your note then press Enter");
    if (text && text.length > 0) {
        notes.push({ t: text, at: ox.time.date() });
        save();
    }
}

var COLORS = ["#fef3c7", "#fee2e2", "#dbeafe", "#dcfce7", "#f3e8ff"];

function noteRect(i) {
    var col = i % 3;
    var row = (i / 3) | 0;
    return { x: 10 + col * 230, y: 40 + row * 110, w: 220, h: 100 };
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "Notes — click + to add - click note to delete - q quit",
            "#f5c2e7");
    // Add button
    ox.rect(W - 40, 4, 30, 20, "#a6e3a1");
    ox.text(W - 30, 8, "+", "#1e1e2e");

    for (var i = 0; i < notes.length; i++) {
        var r = noteRect(i);
        if (r.y + r.h > H) break;
        ox.rect(r.x, r.y, r.w, r.h, COLORS[i % COLORS.length]);
        ox.frame(r.x, r.y, r.w, r.h, "#313244", 1);
        var line = notes[i].t;
        // Wrap text at ~25 chars
        var y = r.y + 10;
        while (line.length > 0 && y < r.y + r.h - 14) {
            var seg = line.substring(0, 25);
            ox.text(r.x + 8, y, seg, "#1e1e2e");
            line = line.substring(25);
            y += 12;
        }
        ox.text(r.x + 8, r.y + r.h - 12, notes[i].at, "#5b5b6e");
    }
}

win.onPaint(paint);
win.onClick(function (x, y) {
    // Add?
    if (x >= W - 40 && x < W - 10 && y >= 4 && y < 24) { addNote(); return; }
    // Delete a note?
    for (var i = 0; i < notes.length; i++) {
        var r = noteRect(i);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            notes.splice(i, 1);
            save();
            return;
        }
    }
});
win.onKey(function (a) {
    if (a === 113) ox.os.exit(0);
    if (a === 43)  addNote();   // +
});

load();
