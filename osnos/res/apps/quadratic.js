// quadratic.js — solve a*x^2 + b*x + c = 0
// Written in ES5 (Duktape 2.7 default does not enable let/const or
// arrow functions; using `var` and `function()` keeps the parser
// happy and runs everywhere).
var win = ox.window("Quadratic Solver", 460, 280);

var coef = { a: 1, b: -3, c: 2 };   // default = (x-1)(x-2) → roots 1, 2
var active = "a";
var buf = String(coef.a);
var result = "";

function recompute() {
    var a = coef.a, b = coef.b, c = coef.c;
    if (a === 0) {
        if (b === 0) {
            result = c === 0 ? "infinite solutions" : "no solution";
        } else {
            result = "linear: x = " + (-c / b);
        }
        return;
    }
    var disc = b * b - 4 * a * c;
    if (disc < 0) {
        var re = (-b) / (2 * a);
        var im = Math.sqrt(-disc) / (2 * a);
        result = "x = " + re.toFixed(4) + " +/- " + im.toFixed(4) + "i";
    } else {
        var r1 = (-b + Math.sqrt(disc)) / (2 * a);
        var r2 = (-b - Math.sqrt(disc)) / (2 * a);
        result = "x1 = " + r1.toFixed(4) + "   x2 = " + r2.toFixed(4);
    }
}
recompute();

function field(label, name, x, y) {
    var sel = active === name;
    var bg = sel ? "#3a6ea5" : "#202030";
    var fg = sel ? "#ffffff" : "#cccccc";
    ox.rect(x, y, 120, 28, bg);
    ox.text(x + 8, y + 10, label + " = " + (sel ? buf : String(coef[name])), fg);
}

function paint() {
    ox.clear("#101018");
    ox.text(14, 12, "a x^2 + b x + c = 0", "#ffffff");
    ox.text(14, 28, "press 1/2/3 to select, type to edit, Enter to commit",
            "#888888");
    field("a", "a",  14,  56);
    field("b", "b", 150,  56);
    field("c", "c", 286,  56);
    ox.rect(14, 100, 432, 60, "#1a1a25");
    ox.text(22, 116, "Result:", "#cccccc");
    ox.text(22, 132, result, "#7fff7f");
    ox.text(14, 240, "Backspace: clear   Esc: quit", "#666666");
}
win.onPaint(paint);

function commit() {
    var n = parseFloat(buf);
    if (isNaN(n)) n = 0;
    coef[active] = n;
    recompute();
    buf = String(coef[active]);
}

win.onKey(function (ascii, code) {
    if (ascii === 49) { commit(); active = "a"; buf = String(coef.a); return; }
    if (ascii === 50) { commit(); active = "b"; buf = String(coef.b); return; }
    if (ascii === 51) { commit(); active = "c"; buf = String(coef.c); return; }
    if (ascii === 13 || ascii === 10) { commit(); return; }
    if (ascii === 8 || ascii === 127) { buf = ""; return; }
    var c = String.fromCharCode(ascii);
    if ((c >= "0" && c <= "9") || c === "-" || c === ".") {
        buf = (buf === String(coef[active])) ? c : (buf + c);
    }
});
