// lab.js — interactive playground of ALL ox.* modules. Each module
// has a panel; click it to run a self-test and see the result.
var W = 800, H = 560;
var win = ox.window("oxjs Lab — module sampler", W, H);

var TESTS = [
    {
        name: "ox.fs",
        run: function () {
            ox.fs.writeFile("/home/.oxlab.txt", "hello @ " + ox.time.date());
            var got = ox.fs.readFile("/home/.oxlab.txt");
            var ents = ox.fs.listDir("/home");
            return "wrote " + got.length + " bytes; /home has " +
                   ents.length + " entries; cwd=" + ox.fs.cwd();
        }
    },
    {
        name: "ox.os",
        run: function () {
            return "pid=" + ox.os.getpid() +
                   " host=" + ox.os.hostname() +
                   " PATH=" + (ox.os.getenv("PATH") || "(unset)").substring(0, 40);
        }
    },
    {
        name: "ox.http",
        run: function () {
            var r = ox.http.get("http://httpbin.org/get");
            return r ? ("HTTP " + r.status + " body=" + r.body.length + "B")
                     : "fetch failed";
        }
    },
    {
        name: "ox.net",
        run: function () {
            var s = ox.net.tcpConnect("10.0.2.2", 80);
            if (s < 0) return "connect failed (no listener?)";
            ox.net.send(s, "GET / HTTP/1.0\r\n\r\n");
            var d = ox.net.recv(s, 1024);
            ox.net.close(s);
            return d ? ("got " + d.length + " bytes") : "no data";
        }
    },
    {
        name: "ox.color",
        run: function () {
            return ox.color.rgb(0, 255, 128) + " / " +
                   ox.color.hex(255, 64, 32).toString(16);
        }
    },
    {
        name: "ox.sys",
        run: function () {
            var info = ox.sys.meminfo() || {};
            return "uptime=" + ox.sys.uptime() + "s mem=" +
                   info.raw.split("\n")[0];
        }
    },
    {
        name: "ox.time",
        run: function () {
            return ox.time.date() + " (now=" + ox.time.now().toFixed(0) + ")";
        }
    },
    {
        name: "ox.clipboard",
        run: function () {
            ox.clipboard.set("hello from oxjs lab " + ox.time.date());
            return "set/get roundtrip: '" + ox.clipboard.get().substring(0, 40) + "'";
        }
    },
    {
        name: "ox.log",
        run: function () {
            ox.log.info("lab ran log.info");
            ox.log.warn("lab ran log.warn");
            ox.log.error("lab ran log.error");
            return "wrote 3 lines to /dev/ttyS0";
        }
    },
    {
        name: "ox.syscall",
        run: function () {
            var pid = ox.syscall(ox.syscall.GETPID);
            return "raw syscall(getpid) = " + pid;
        }
    },
    {
        name: "ox.sqlite",
        run: function () {
            var rows = ox.sqlite.query("/home/demo.db",
                "SELECT COUNT(*) FROM books");
            return rows && rows[0] ? ("books table has " + rows[0] + " rows")
                                   : "query failed";
        }
    },
    {
        name: "ox.ui.prompt",
        run: function () {
            var v = ox.ui.prompt("Test", "Enter anything (Enter=ok, Esc=cancel)");
            return v ? ("you typed: " + v) : "(cancelled)";
        }
    }
];

var results = new Array(TESTS.length);
for (var i = 0; i < TESTS.length; i++) results[i] = "";

var COLS = 2;
function rectFor(i) {
    var col = i % COLS;
    var row = (i / COLS) | 0;
    return { x: 10 + col * (W / COLS - 5),
             y: 36 + row * 70,
             w: W / COLS - 15,
             h: 60 };
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "oxjs Lab — click any panel to run that module's self-test",
            "#f5c2e7");

    for (var i = 0; i < TESTS.length; i++) {
        var r = rectFor(i);
        ox.rect(r.x, r.y, r.w, r.h, "#313244");
        ox.frame(r.x, r.y, r.w, r.h, "#45475a", 1);
        ox.text(r.x + 8, r.y + 6, TESTS[i].name, "#89b4fa");
        ox.text(r.x + 8, r.y + 26, results[i] || "(click to run)", "#cdd6f4");
    }
}

win.onPaint(paint);
win.onClick(function (x, y) {
    for (var i = 0; i < TESTS.length; i++) {
        var r = rectFor(i);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            try {
                results[i] = TESTS[i].run();
            } catch (e) {
                results[i] = "ERROR: " + e.message;
            }
            return;
        }
    }
});
win.onKey(function (a) { if (a === 113) ox.os.exit(0); });
