// colors.js — palette + HSV grid demo. Click to copy color hex to clipboard.
var W = 640, H = 460;
var win = ox.window("Color Picker (JS)", W, H);

var selected = null;
var hexBuf = "";

function paint() {
    ox.clear("#1e1e2e");
    ox.rect(0, 0, W, 28, "#313244");
    ox.text(10, 8, "Click any color - it goes to clipboard - q quit",
            "#f5c2e7");

    var CW = 24, CH = 24;
    var COLS = 24, ROWS = 16;
    for (var r = 0; r < ROWS; r++) {
        for (var c = 0; c < COLS; c++) {
            var R = (c * 255 / (COLS - 1)) | 0;
            var G = (r * 255 / (ROWS - 1)) | 0;
            var B = ((R + G) / 2) | 0;
            ox.rect(8 + c * CW, 40 + r * CH, CW - 1, CH - 1,
                    ox.color.rgb(R, G, B));
        }
    }

    if (selected) {
        ox.rect(8, 40 + ROWS * CH + 8, 60, 40, selected);
        ox.text(80, 40 + ROWS * CH + 16, "selected: " + selected, "#cdd6f4");
        ox.text(80, 40 + ROWS * CH + 30,
                "(also copied to clipboard)", "#7f849c");
    }
}

win.onPaint(paint);
win.onClick(function (x, y) {
    var CW = 24, CH = 24, COLS = 24, ROWS = 16;
    var col = ((x - 8) / CW) | 0;
    var row = ((y - 40) / CH) | 0;
    if (row >= 0 && row < ROWS && col >= 0 && col < COLS) {
        var R = (col * 255 / (COLS - 1)) | 0;
        var G = (row * 255 / (ROWS - 1)) | 0;
        var B = ((R + G) / 2) | 0;
        selected = ox.color.rgb(R, G, B);
        ox.clipboard.set(selected);
    }
});
win.onKey(function (a) { if (a === 113) ox.os.exit(0); });
