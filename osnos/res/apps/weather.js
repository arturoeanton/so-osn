// weather.js — HTTP fetch + JSON parse demo. Hits httpbin.org/get and
// shows the parsed origin + headers. Click "Reload" to refetch.
var W = 600, H = 400;
var win = ox.window("HTTP Demo", W, H);

var url = "http://httpbin.org/get";
var status = "click Reload to fetch";
var body  = "";
var origin = "";
var dataLines = [];

function refetch() {
    status = "fetching " + url + "...";
    paint();
    var r = ox.http.get(url);
    if (!r) { status = "fetch failed"; return; }
    status = "HTTP " + r.status + " (" + r.body.length + " bytes)";
    body = r.body;
    try {
        var j = JSON.parse(r.body);
        origin = j.origin || "(no origin field)";
        dataLines = [];
        if (j.headers) {
            for (var k in j.headers) dataLines.push(k + ": " + j.headers[k]);
        }
    } catch (e) {
        origin = "json parse failed: " + e.message;
    }
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, url, "#f5c2e7");

    // Reload button
    ox.rect(W - 80, 4, 70, 20, "#89b4fa");
    ox.text(W - 60, 8, "Reload", "#1e1e2e");

    ox.text(10, 40, status, "#a6e3a1");
    ox.text(10, 60, "Origin: " + origin, "#94e2d5");
    ox.text(10, 76, "Headers seen:", "#94e2d5");

    var y = 96;
    for (var i = 0; i < dataLines.length && y < H - 16; i++) {
        ox.text(10, y, dataLines[i], "#cdd6f4");
        y += 12;
    }
}

win.onPaint(paint);
win.onClick(function (x, y) {
    if (x >= W - 80 && x < W - 10 && y >= 4 && y < 24) refetch();
});
win.onKey(function (a) {
    if (a === 113) ox.os.exit(0);
    if (a === 13 || a === 10) refetch();   // Enter
});
