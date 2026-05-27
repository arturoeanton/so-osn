// fs_explorer.js — directory browser. Click a name to enter; ".." to go up.
var W = 700, H = 500;
var win = ox.window("Files (JS)", W, H);

var cwd = "/home";
var entries = [];

function refresh() {
    entries = [];
    if (cwd !== "/") entries.push("..");
    var list = ox.fs.listDir(cwd);
    if (list) for (var i = 0; i < list.length; i++) entries.push(list[i]);
}

function joinPath(a, b) {
    if (a === "/") return "/" + b;
    return a + "/" + b;
}

function rowRect(i) { return { x: 0, y: 36 + i * 16, w: W, h: 16 }; }

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "FS: " + cwd, "#f5c2e7");
    ox.text(W - 100, 8, "q quit", "#7f849c");
    ox.rect(0, 28, W, 8, "#181825");

    for (var i = 0; i < entries.length; i++) {
        var r = rowRect(i);
        if (r.y >= H) break;
        var p = joinPath(cwd, entries[i]);
        var st = entries[i] === ".." ? { isdir: true } : ox.fs.stat(p);
        var isdir = st ? st.isdir : false;
        ox.text(20, r.y + 2, (isdir ? "[d] " : "    ") + entries[i],
                isdir ? "#89b4fa" : "#cdd6f4");
        if (st && !isdir) ox.text(400, r.y + 2, st.size + " B", "#7f849c");
    }
}

win.onPaint(paint);
win.onClick(function (x, y) {
    for (var i = 0; i < entries.length; i++) {
        var r = rowRect(i);
        if (y < r.y || y >= r.y + r.h) continue;
        var name = entries[i];
        if (name === "..") {
            // go up
            var slash = cwd.lastIndexOf("/");
            cwd = slash <= 0 ? "/" : cwd.substring(0, slash);
        } else {
            var target = joinPath(cwd, name);
            var st = ox.fs.stat(target);
            if (st && st.isdir) cwd = target;
            else {
                // Open the file in a msgbox preview (text mode).
                var content = ox.fs.readFile(target);
                ox.ui.msgbox(target,
                    content ? content.substring(0, 600) : "(cannot read)");
            }
        }
        refresh();
        return;
    }
});
win.onKey(function (a) { if (a === 113) ox.os.exit(0); });

refresh();
