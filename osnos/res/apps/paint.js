// paint.js — Drawing app. Click+drag to paint. Number keys 1-7 change
// color; 'c' clears; 's' saves snapshot to /home/paint.txt; ESC exits.
var W = 700, H = 500;
var win = ox.window("Paint", W, H);

var TOOLBAR_H = 28;
var palette = [
    "#f38ba8", "#f9e2af", "#a6e3a1", "#94e2d5",
    "#89b4fa", "#cba6f7", "#cdd6f4"
];
var curColor = palette[0];
var brush = 4;
var dragging = false;
var lastX = -1, lastY = -1;
var status = "click to draw - 1-7 color - c clear - + - brush - s save";

function paintToolbar() {
    ox.rect(0, 0, W, TOOLBAR_H, "#313244");
    var px = 8;
    for (var i = 0; i < palette.length; i++) {
        ox.rect(px, 4, 20, 20, palette[i]);
        if (palette[i] === curColor) ox.frame(px - 2, 2, 24, 24, "#ffffff", 1);
        px += 26;
    }
    ox.text(px + 10, 8, "brush:" + brush, "#cdd6f4");
    ox.text(px + 100, 8, status, "#7f849c");
}

function clearCanvas() {
    ox.rect(0, TOOLBAR_H, W, H - TOOLBAR_H, "#1e1e2e");
}

win.onPaint(function () {
    paintToolbar();
});

// We draw on the fly inside onMouse rather than waiting for the next
// onPaint — so strokes are visible immediately.
win.onMouse(function (ev) {
    if (ev.y < TOOLBAR_H) {
        if (ev.kind === 1 && (ev.buttons & 1)) {
            var i = ((ev.x - 8) / 26) | 0;
            if (i >= 0 && i < palette.length) {
                curColor = palette[i];
                paintToolbar();
            }
        }
        return;
    }
    if (ev.kind === 1 && (ev.buttons & 1)) {
        dragging = true;
        ox.circle(ev.x, ev.y, brush, curColor);
        lastX = ev.x; lastY = ev.y;
    } else if (ev.kind === 2) { // MOVE
        if (dragging && (ev.buttons & 1)) {
            ox.line(lastX, lastY, ev.x, ev.y, curColor);
            ox.circle(ev.x, ev.y, brush, curColor);
            lastX = ev.x; lastY = ev.y;
        }
    } else if (ev.kind === 0 || !(ev.buttons & 1)) {
        dragging = false;
    }
});

win.onKey(function (ascii, code) {
    if (ascii === 0x1b || ascii === 113) ox.os.exit(0);
    if (ascii === 99) { clearCanvas(); paintToolbar(); }   // c
    if (ascii === 43 && brush < 30) { brush++; paintToolbar(); }   // +
    if (ascii === 45 && brush > 1)  { brush--; paintToolbar(); }   // -
    if (ascii >= 49 && ascii <= 49 + palette.length - 1) {
        curColor = palette[ascii - 49];
        paintToolbar();
    }
    if (ascii === 115) {           // s = save snapshot info
        var ts = ox.time.date();
        ox.fs.appendFile("/home/paint.log",
            ts + " painted in " + curColor + " brush=" + brush + "\n");
        status = "saved log to /home/paint.log";
        paintToolbar();
    }
});

clearCanvas();
paintToolbar();
