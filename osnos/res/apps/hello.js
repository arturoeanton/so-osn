// hello.js — minimal Ox app. Shows window, draws colored "hello".
var win = ox.window("Hello", 400, 200);

win.onPaint(function () {
    ox.clear("#1e1e2e");
    ox.text(20, 30, "Hello from oxjs!",       "#f5c2e7");
    ox.text(20, 50, "Click anywhere.",         "#cdd6f4");
    ox.text(20, 70, "Press 'q' to quit.",      "#cdd6f4");
    ox.text(20, 100, "host: " + ox.os.hostname(), "#a6e3a1");
    ox.text(20, 116, "pid:  " + ox.os.getpid(),   "#a6e3a1");
    ox.text(20, 132, "time: " + ox.time.date(),   "#a6e3a1");
});

win.onClick(function (x, y) {
    ox.rect(x - 5, y - 5, 10, 10, "#f9e2af");
});

win.onKey(function (ascii) {
    if (ascii === 113 || ascii === 81) ox.os.exit(0);
});
