// calc.js — calculator. Click digits / operators; '=' to evaluate.
// Also accepts keyboard input. Internal: just JS eval() of the buffer.
var W = 300, H = 360;
var win = ox.window("Calculator (JS)", W, H);

var BTNS = [
    ["7", "8", "9", "/"],
    ["4", "5", "6", "*"],
    ["1", "2", "3", "-"],
    ["0", ".", "C", "+"],
    ["(", ")", "=", "←"]
];
var buf = "";
var result = "0";

function btnRect(r, c) {
    return { x: 4 + c * 73, y: 80 + r * 50, w: 70, h: 46 };
}

function clickBtn(r, c) {
    var v = BTNS[r][c];
    if (v === "=") {
        try { result = "" + eval(buf); } catch (e) { result = "error"; }
    } else if (v === "C") { buf = ""; result = "0"; }
    else if (v === "←") { buf = buf.substring(0, buf.length - 1); }
    else { buf += v; }
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "Calc - q quit", "#f5c2e7");

    // Display
    ox.rect(8, 36, W - 16, 32, "#11111b");
    ox.frame(8, 36, W - 16, 32, "#45475a", 1);
    var disp = buf || result;
    ox.text(W - 16 - disp.length * 8, 50, disp, "#a6e3a1");

    for (var r = 0; r < BTNS.length; r++) {
        for (var c = 0; c < BTNS[r].length; c++) {
            var b = btnRect(r, c);
            var v = BTNS[r][c];
            var col = (v >= "0" && v <= "9") || v === "." ? "#45475a"
                    : v === "=" ? "#a6e3a1"
                    : v === "C" ? "#f38ba8"
                    : "#89b4fa";
            var fg = (v === "=" || v === "C") ? "#1e1e2e" : "#cdd6f4";
            ox.rect(b.x, b.y, b.w, b.h, col);
            ox.text(b.x + b.w / 2 - 4, b.y + b.h / 2 - 4, v, fg);
        }
    }
}
win.onPaint(paint);
win.onClick(function (x, y) {
    for (var r = 0; r < BTNS.length; r++) {
        for (var c = 0; c < BTNS[r].length; c++) {
            var b = btnRect(r, c);
            if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
                clickBtn(r, c);
                return;
            }
        }
    }
});
win.onKey(function (a) {
    if (a === 113) ox.os.exit(0);
    if (a === 13 || a === 10 || a === 61) {     // Enter or '='
        try { result = "" + eval(buf); } catch (e) { result = "error"; }
        return;
    }
    if (a === 8 || a === 127) { buf = buf.substring(0, buf.length - 1); return; }
    if (a === 99 || a === 67) { buf = ""; result = "0"; return; }  // c/C
    if ((a >= 48 && a <= 57) || a === 46 ||
        a === 43 || a === 45 || a === 42 || a === 47 ||
        a === 40 || a === 41 || a === 32) buf += String.fromCharCode(a);
});
