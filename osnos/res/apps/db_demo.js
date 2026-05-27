// db_demo.js — SQL query browser. Reads /home/demo.db, shows the
// pre-seeded books table. Press 'r' to refresh, 'q' to quit.
var W = 720, H = 500;
var win = ox.window("SQL Demo (/home/demo.db)", W, H);

var rows = [];
var status = "";

function reload() {
    status = "running query...";
    rows = ox.sqlite.query("/home/demo.db",
        "SELECT id, title, author, year FROM books ORDER BY year DESC");
    if (!rows || rows.length === 0) {
        status = "no rows (db missing? run /bin/sqlite3 /home/demo.db .tables)";
    } else {
        status = rows.length + " rows";
    }
}

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "Books table - r reload - q quit - " + status, "#f5c2e7");

    // Header row.
    ox.rect(0, 30, W, 16, "#45475a");
    ox.text(10,  34, "id",     "#a6e3a1");
    ox.text(50,  34, "title",  "#a6e3a1");
    ox.text(360, 34, "author", "#a6e3a1");
    ox.text(560, 34, "year",   "#a6e3a1");

    var y = 50;
    for (var i = 0; i < rows.length && y < H - 12; i++) {
        var cols = rows[i].split("\t");
        ox.text(10,  y, cols[0] || "",        "#cdd6f4");
        ox.text(50,  y, (cols[1] || "").substring(0, 38), "#f5c2e7");
        ox.text(360, y, (cols[2] || "").substring(0, 24), "#cdd6f4");
        ox.text(560, y, cols[3] || "",        "#94e2d5");
        y += 14;
    }
}

win.onPaint(paint);
win.onKey(function (a) {
    if (a === 113) ox.os.exit(0);
    if (a === 114) reload();
});

reload();
