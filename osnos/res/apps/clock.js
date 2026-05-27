// clock.js — analog + digital clock. Demonstrates ox.time, ox.draw,
// ox.color, and the onTick callback (fires at ~30 Hz).
var W = 360, H = 360;
var win = ox.window("Clock", W, H);

var CX = W / 2;
var CY = H / 2 - 10;
var R  = 130;

function tickAngle(unit, max) {
    return -Math.PI / 2 + (unit / max) * 2 * Math.PI;
}

function drawHand(angle, length, color, thickness) {
    var x2 = CX + Math.cos(angle) * length;
    var y2 = CY + Math.sin(angle) * length;
    // Approximate thickness by drawing parallel lines.
    for (var t = -thickness / 2; t <= thickness / 2; t++) {
        ox.line(CX + t, CY, x2 + t, y2, color);
    }
}

function paint() {
    ox.clear("#1e1e2e");

    // Outer ring.
    ox.circle(CX, CY, R + 4, "#45475a");
    ox.circle(CX, CY, R,     "#cdd6f4");
    ox.circle(CX, CY, R - 8, "#1e1e2e");

    // Hour ticks.
    for (var h = 0; h < 12; h++) {
        var a = tickAngle(h, 12);
        var x1 = CX + Math.cos(a) * (R - 12);
        var y1 = CY + Math.sin(a) * (R - 12);
        var x2 = CX + Math.cos(a) * (R - 4);
        var y2 = CY + Math.sin(a) * (R - 4);
        ox.line(x1, y1, x2, y2, "#cdd6f4");
    }

    var now = new Date();
    var hh = now.getHours() % 12;
    var mm = now.getMinutes();
    var ss = now.getSeconds();

    drawHand(tickAngle(hh + mm/60, 12), R - 50, "#cdd6f4", 4);
    drawHand(tickAngle(mm + ss/60, 60), R - 25, "#cdd6f4", 3);
    drawHand(tickAngle(ss, 60),         R - 15, "#f38ba8", 1);

    ox.circle(CX, CY, 5, "#f38ba8");

    // Digital readout under the clock.
    var z = function (n) { return (n < 10 ? "0" : "") + n; };
    var digital = z(now.getHours()) + ":" + z(mm) + ":" + z(ss);
    ox.text(CX - 32, H - 30, digital, "#f5c2e7");
    ox.text(CX - 50, H - 14, ox.time.date(), "#7f849c");
}

win.onPaint(paint);
win.onTick(function () { /* paint is called automatically after tick */ });
win.onKey(function (a) { if (a === 113) ox.os.exit(0); });
