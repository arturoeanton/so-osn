-- osnos sqlite demo DB — populated at build time.
-- Try things like:
--   sqlite3 /home/demo.db "SELECT * FROM books"
--   sqlite3 /home/demo.db "SELECT title, year FROM books WHERE year > 1980 ORDER BY year"
--   sqlite3 /home/demo.db "SELECT author, COUNT(*) AS n FROM books GROUP BY author ORDER BY n DESC"
--   sqlite3 /home/demo.db ".schema"
--   sqlite3 /home/demo.db ".tables"

CREATE TABLE books (
    id     INTEGER PRIMARY KEY,
    title  TEXT NOT NULL,
    author TEXT NOT NULL,
    year   INTEGER,
    pages  INTEGER
);

CREATE TABLE users (
    id    INTEGER PRIMARY KEY,
    name  TEXT NOT NULL,
    email TEXT UNIQUE,
    role  TEXT DEFAULT 'reader'
);

CREATE TABLE checkouts (
    id       INTEGER PRIMARY KEY,
    user_id  INTEGER REFERENCES users(id),
    book_id  INTEGER REFERENCES books(id),
    out_date TEXT,
    in_date  TEXT
);

INSERT INTO books(title, author, year, pages) VALUES
    ('The C Programming Language',           'Kernighan & Ritchie', 1978, 272),
    ('UNIX Network Programming',             'Stevens',              1990, 1024),
    ('Operating Systems: Three Easy Pieces', 'Arpaci-Dusseau',       2014,  714),
    ('Modern Operating Systems',             'Tanenbaum',            1992,  944),
    ('The Design of the UNIX OS',            'Bach',                 1986,  471),
    ('Linux Kernel Development',             'Love',                 2010,  440),
    ('Compilers: Principles, Tech, Tools',   'Aho & Ullman',         1986, 1009),
    ('SICP',                                 'Abelson & Sussman',    1985,  657),
    ('The Mythical Man-Month',               'Brooks',               1975,  322),
    ('Programming Pearls',                   'Bentley',              1986,  256),
    ('The Pragmatic Programmer',             'Hunt & Thomas',        1999,  320),
    ('Code Complete',                        'McConnell',            2004,  960),
    ('Refactoring',                          'Fowler',               1999,  431),
    ('Design Patterns',                      'Gang of Four',         1994,  395),
    ('Clean Code',                           'Martin',               2008,  464);

INSERT INTO users(name, email, role) VALUES
    ('Arturo',  'arturoeanton@gmail.com', 'admin'),
    ('Alice',   'alice@example.com',      'reader'),
    ('Bob',     'bob@example.com',        'reader'),
    ('Carol',   'carol@example.com',      'librarian');

INSERT INTO checkouts(user_id, book_id, out_date, in_date) VALUES
    (1, 1,  '2026-01-15', '2026-02-15'),
    (1, 3,  '2026-02-01', NULL),
    (2, 5,  '2026-01-20', '2026-01-25'),
    (3, 11, '2026-03-01', NULL),
    (4, 7,  '2026-02-10', '2026-03-10'),
    (4, 13, '2026-03-15', NULL);

CREATE VIEW v_open_checkouts AS
    SELECT u.name AS user, b.title AS book, c.out_date
    FROM checkouts c
    JOIN users u ON u.id = c.user_id
    JOIN books b ON b.id = c.book_id
    WHERE c.in_date IS NULL;

CREATE INDEX idx_books_year   ON books(year);
CREATE INDEX idx_books_author ON books(author);
