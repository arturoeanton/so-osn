// bench.js — perf benchmarks. Pi via Leibniz + sort + hash. Reports
// MOps/sec via ox.time.now() and prints to console + window.
var W = 600, H = 360;
var win = ox.window("Bench (JS)", W, H);

var results = [];
var running = false;

function bench(name, fn) {
    var t0 = ox.time.now();
    var iters = fn();
    var dt = ox.time.now() - t0;
    var mops = (iters / 1e6) / (dt / 1000);
    var line = name + ": " + iters + " ops in " + dt.toFixed(1)
        + " ms (" + mops.toFixed(2) + " Mops/s)";
    results.push(line);
    console.log(line);
}

function runAll() {
    if (running) return;
    running = true;
    results = [];
    results.push("starting benchmarks...");

    bench("pi-leibniz", function () {
        var sum = 0;
        var n = 1000000;
        for (var i = 0; i < n; i++) sum += (i & 1 ? -1 : 1) / (2 * i + 1);
        return n;
    });

    bench("array-sort", function () {
        var n = 5000;
        var a = new Array(n);
        for (var i = 0; i < n; i++) a[i] = (Math.random() * 1e6) | 0;
        a.sort(function (x, y) { return x - y; });
        return n;
    });

    bench("hash-djb2", function () {
        var n = 200000;
        var h = 5381;
        for (var i = 0; i < n; i++) h = ((h << 5) + h + i) | 0;
        return n;
    });

    bench("json-rt", function () {
        var n = 5000;
        for (var i = 0; i < n; i++) {
            var s = JSON.stringify({ a: i, b: "x" + i, c: [1, 2, 3] });
            JSON.parse(s);
        }
        return n;
    });

    bench("syscall-getpid", function () {
        var n = 100000;
        var pid = 0;
        for (var i = 0; i < n; i++) pid = ox.syscall(ox.syscall.GETPID);
        return n;
    });

    results.push("done. q to quit");
    running = false;
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "Bench - Enter run - q quit - " + ox.os.hostname(),
            "#f5c2e7");

    var y = 50;
    for (var i = 0; i < results.length && y < H - 12; i++) {
        ox.text(10, y, results[i], "#a6e3a1");
        y += 14;
    }
}

win.onPaint(paint);
win.onKey(function (a) {
    if (a === 113) ox.os.exit(0);
    if (a === 13 || a === 10 || a === 32) runAll();
});

// auto-run once on startup
runAll();
