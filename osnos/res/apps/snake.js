// snake.js — classic Snake. Arrows steer; q quits. ES5 syntax
// because Duktape 2.7 default doesn't enable let/const/arrows.
var W = 480, H = 320;
var CELL = 16;
var COLS = (W / CELL) | 0;
var ROWS = (H / CELL) | 0;
var win = ox.window("Snake", W, H);

var snake = [ {x: 5, y: 5}, {x: 4, y: 5}, {x: 3, y: 5} ];
var dir = { x: 1, y: 0 };
var pendingDir = dir;
var food = { x: 12, y: 7 };
var score = 0;
var dead = false;
var tickCount = 0;
var TICKS_PER_STEP = 4;

function placeFood() {
    while (true) {
        food.x = (Math.random() * COLS) | 0;
        food.y = (Math.random() * ROWS) | 0;
        var onSnake = false;
        for (var i = 0; i < snake.length; i++) {
            if (snake[i].x === food.x && snake[i].y === food.y) {
                onSnake = true; break;
            }
        }
        if (!onSnake) return;
    }
}

function step() {
    if (dead) return;
    if (pendingDir.x !== -dir.x || pendingDir.y !== -dir.y) {
        dir = pendingDir;
    }
    var head = snake[0];
    var nx = head.x + dir.x;
    var ny = head.y + dir.y;
    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) { dead = true; return; }
    for (var i = 0; i < snake.length; i++) {
        if (snake[i].x === nx && snake[i].y === ny) { dead = true; return; }
    }
    snake.unshift({ x: nx, y: ny });
    if (nx === food.x && ny === food.y) {
        score += 1;
        placeFood();
    } else {
        snake.pop();
    }
}

function paint() {
    ox.clear("#0a0a14");
    for (var i = 0; i < COLS; i++) ox.rect(i * CELL, 0, 1, H, "#181830");
    for (var j = 0; j < ROWS; j++) ox.rect(0, j * CELL, W, 1, "#181830");
    ox.rect(food.x * CELL + 2, food.y * CELL + 2, CELL - 4, CELL - 4, "#ff4040");
    for (var k = 0; k < snake.length; k++) {
        var s = snake[k];
        var c = (k === 0) ? "#80ff80" : "#40c060";
        ox.rect(s.x * CELL + 1, s.y * CELL + 1, CELL - 2, CELL - 2, c);
    }
    ox.text(8, 4, "Score: " + score, "#ffffff");
    if (dead) {
        ox.rect(W / 2 - 90, H / 2 - 20, 180, 40, "#400000");
        ox.text(W / 2 - 60, H / 2 - 4, "GAME OVER - q quit", "#ffffff");
    }
}
win.onPaint(paint);

win.onKey(function (ascii, code) {
    if (ascii === 113 || ascii === 81) { dead = true; return; }
    if (code === 103)      pendingDir = { x:  0, y: -1 };
    else if (code === 108) pendingDir = { x:  0, y:  1 };
    else if (code === 105) pendingDir = { x: -1, y:  0 };
    else if (code === 106) pendingDir = { x:  1, y:  0 };
});

win.onTick(function () {
    tickCount += 1;
    if (tickCount >= TICKS_PER_STEP) {
        tickCount = 0;
        step();
    }
});

placeFood();
