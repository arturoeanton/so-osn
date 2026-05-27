// sysinfo.js — Live system monitor. Reads /sys/ via ox.sys + ox.fs,
// renders memory bar, uptime, task list. Refreshes every 30 ticks.
var W = 640, H = 480;
var win = ox.window("System Info", W, H);

var refresh = 0;
var meminfoRaw = "";
var tasksRaw   = "";
var uptimeStr  = "";

function readAll() {
    meminfoRaw = ox.fs.readFile("/sys/meminfo")  || "";
    tasksRaw   = ox.fs.readFile("/sys/tasks")    || "";
    uptimeStr  = ox.fs.readFile("/sys/uptime")   || "";
}

function parseKV(raw) {
    var lines = raw.split("\n");
    var kv = {};
    for (var i = 0; i < lines.length; i++) {
        var ln = lines[i];
        var sp = ln.indexOf(":");
        if (sp > 0) {
            var k = ln.substring(0, sp).trim();
            var v = ln.substring(sp + 1).trim();
            kv[k] = v;
        }
    }
    return kv;
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "OSnOS — System Info — q quit", "#f5c2e7");

    // Memory section
    ox.text(10, 40, "Memory:", "#a6e3a1");
    var mi = parseKV(meminfoRaw);
    var total = parseInt(mi["MemTotal"]) || 0;
    var free  = parseInt(mi["MemFree"])  || 0;
    var used  = total - free;
    var pct   = total > 0 ? (used / total) : 0;
    var barW  = 380;
    ox.rect(80, 40, barW, 14, "#45475a");
    ox.rect(80, 40, (barW * pct) | 0, 14,
            pct > 0.8 ? "#f38ba8" : (pct > 0.5 ? "#f9e2af" : "#a6e3a1"));
    ox.frame(80, 40, barW, 14, "#cdd6f4", 1);
    ox.text(80 + barW + 10, 42,
            used + " / " + total + " kB (" + (pct * 100 | 0) + "%)",
            "#cdd6f4");

    // Uptime
    ox.text(10, 70, "Uptime: " + uptimeStr.replace("\n", ""), "#94e2d5");
    ox.text(10, 86, "Date:   " + ox.time.date(), "#94e2d5");

    // Tasks
    ox.text(10, 110, "Tasks:", "#a6e3a1");
    var lines = tasksRaw.split("\n");
    var y = 130;
    var lineH = 12;
    var max = ((H - y - 8) / lineH) | 0;
    for (var i = 0; i < lines.length && i < max; i++) {
        ox.text(10, y, lines[i], "#cdd6f4");
        y += lineH;
    }
}

win.onPaint(paint);
win.onTick(function () {
    refresh++;
    if (refresh >= 30) {       // ~1 s at 30 Hz
        refresh = 0;
        readAll();
    }
});
win.onKey(function (a) { if (a === 113) ox.os.exit(0); });

readAll();
