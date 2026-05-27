# NetSurf → osnos Porting Plan

Status: **groundwork only — nothing compiles yet**. This document is the
contract between the vendor tree (`vendor/netsurf/`) and a future
`/bin/oxnetsurf` ELF. The build is gated by `OXNETSURF_ENABLED` in the
top-level `GNUmakefile`; default is `0`, so a normal `make` is
unaffected by anything below.

## 1. Why NetSurf

We already have `oxbrowser` (Lynx-tier: tag stripper + naive HTTP via
BearSSL). The next step toward "real" web rendering wants:

- HTML5 tokenizer + tree construction (not regex). NetSurf's
  **libhubbub** is a small standalone implementation.
- A proper DOM (`Element`, `Node`, `Text`, attributes, traversal).
  NetSurf's **libdom** is built on top of libhubbub.
- A CSS engine that can do at least the visual-formatting basics
  (block/inline boxes, fonts, colors). NetSurf's **libcss** is a small,
  self-contained engine that consumes a stylesheet + a DOM and yields a
  computed-style tree.
- A laid-out box model + paint pipeline. NetSurf's core does this in
  `desktop/` + `content/handlers/html/` against the DOM+CSS computed
  styles.
- A framebuffer-only frontend, which NetSurf already supports
  (`frontends/framebuffer/`). It draws into a raw pixel buffer, which
  is a natural fit for our Ox surface.

NetSurf is licensed GPLv2 — fine for a hobby OS. Total source after
vendoring (see §3) is ~80 MB on disk, ~120K LOC excluding generated JS
bindings.

## 2. Why not WebKit / Blink / Servo / NetSurf-via-libcurl

- **WebKit/Blink**: hundreds of thousands of LOC each, C++17, hundreds
  of external deps (skia, ICU, V8). Multi-day port even with musl.
- **Servo**: Rust, no toolchain in osnos.
- **NetSurf-via-libcurl**: works on Linux but pulls in libcurl, which
  pulls openssl/zlib/c-ares/libidn2/nghttp2 etc. We'd be vendoring
  half of Debian.

NetSurf's FB frontend is also the **only** mainstream browser engine
known to work standalone (no X11, no Wayland, just `/dev/fb0`). That
is exactly what Ox exposes.

## 3. Vendored library inventory

All tarballs live on `https://download.netsurf-browser.org/libs/releases/`
and `https://download.netsurf-browser.org/netsurf/releases/source-full/`.
Versions are the latest as of the porting date.

| Library          | Version | Files | LOC (.c)   | Disk    | Required?  |
| ---------------- | ------- | ----- | ---------- | ------- | ---------- |
| libwapcaplet     | 0.4.3   |   1   |    292     | 144 KB  | **yes**    |
| libparserutils   | 0.2.5   |  15   |  5,358     | 692 KB  | **yes**    |
| libnsutils       | 0.1.1   |   3   |    599     |  84 KB  | **yes**    |
| libhubbub        | 0.3.8   |  30   | 11,774     | 9.7 MB  | **yes**    |
| libdom           | 0.4.2   |  94   | 33,238     |  16 MB  | **yes**    |
| libcss           | 0.9.2   | 184   | 39,393     | 4.1 MB  | **yes**    |
| libnslog         | bundled |   2   |    596     | 236 KB  | **yes**    |
| libnspsl         | 0.1.7   |   1   |    208     | 2.8 MB  | optional   |
| libnsbmp         | 0.1.7   |   1   |  1,388     | 8.2 MB  | **no v1**  |
| libnsgif         | 1.0.0   |   2   |  2,689     | 2.1 MB  | **no v1**  |
| libnsfb          | 0.2.2   |  21   |  8,773     | 640 KB  | replaced   |
| libsvgtiny       | 0.1.8   |   3   |  3,178     |  16 MB  | **no v1**  |
| netsurf-core     | 3.11    | ~250  | ~220,000   |  21 MB  | partial    |
| **Total**        |         |       |            | **81 MB** |          |

Notes:
- Most of the per-lib disk weight is in `docs/` and test fixtures (e.g.
  libdom's testcases ≈ 14 MB of WPT-style HTML). The actual .c+.h
  payload is ~5 MB across the core seven libs.
- `netsurf-core/content/handlers/javascript` is 103K LOC — mostly
  *generated* DOM bindings (from `nsgenbind`). We exclude all of it
  for v1 since we have no JS engine. `nsgenbind` is the host-side
  tool that produces those binders; we won't run it.
- `libnsfb` is the NetSurf framebuffer backend that targets a real
  Linux `/dev/fb0`, SDL, xcb, wayland, or VNC. **We will not use it.**
  Instead we substitute Ox surfaces in the FB frontend's `bitmap.c` +
  `framebuffer.c` shims (see §5, Stage 3).

### Avoidable for v1 (text-only HTML)

- `libnsbmp` (BMP/ICO loader): no image rendering at v1
- `libnsgif` (GIF loader): same
- `libsvgtiny` (SVG): same
- `libnspsl` (Public Suffix List): only for cookie scope. We have no
  cookie store at v1.
- `frontends/framebuffer/font_freetype.c`: skip; use Ox's 8×8 builtin
  font via `ox_draw_text` (we'll route NetSurf's `font_internal.c`
  glyph table through `ox_font_glyph` instead).
- `frontends/framebuffer/local_history.c` (history HUD widget):
  defer.
- `content/handlers/image/*`, `content/handlers/javascript/*`,
  `content/handlers/css/css_internal.c` decoder for non-text MIME:
  skip.

### Minimum viable subset (Stage 5)

For an HTML+CSS text-only browser at v1:

```
libwapcaplet  (string interning — every lib depends on it)
  ↓
libparserutils (charset detection + iconv glue + tokenizer scaffolding)
  ↓
libhubbub     (HTML5 parser)
  ↓
libdom        (DOM tree built from hubbub tokens)
  ↓
libcss        (stylesheet parser + selector engine + computed-style cascade)
  ↓
libnslog      (logging — required by libcss/libdom, trivial to satisfy)
  ↓
netsurf-core/utils      (~14K LOC: messages, url, talloc, hashtables, etc.)
  ↓
netsurf-core/content    (~5K LOC core, NOT handlers/*)
netsurf-core/content/handlers/html        (~29K LOC — laying out the box tree)
netsurf-core/content/handlers/css         (~6K LOC — stylesheet binding)
netsurf-core/content/handlers/text/text_plain.c (~1.7K LOC — text/plain fallback)
  ↓
netsurf-core/desktop    (~29K LOC — browser window, render tree, options)
  ↓
oxnetsurf glue (~2K LOC, written by us — replaces frontends/framebuffer)
```

Hard pruning targets within `netsurf-core`:
- `content/fetchers/*`: replace all of curl/file/data/about with our own
  fetcher (Stage 5 uses oxbrowser's BearSSL helpers).
- `content/handlers/javascript/*`: drop entirely.
- `content/handlers/image/*`: drop entirely (HTML `<img>` shows the
  alt text as a span).
- `desktop/save_complete.c`, `desktop/save_text.c`: defer.
- `desktop/print.c`: drop.
- `desktop/searchweb.c`: defer (search-as-you-type box).

Estimated active LOC for v1 (excluding test fixtures, generated code,
unused frontends, JS bindings): **~95K LOC**.

## 4. Porting blockers — what mini-libc lacks

`oxbrowser` builds against mini-libc (libosnos_c.a). NetSurf will NOT.
Survey of every external symbol referenced by the seven core libs:

| Symbol               | mini-libc | musl | Notes                          |
| -------------------- | --------- | ---- | ------------------------------ |
| `iconv`/`iconv_open` | NO        | YES  | libparserutils/src/input/filter.c hard dep |
| `regex.h` (POSIX)    | NO        | YES  | used by netsurf-core/utils + libcss attribute selectors |
| `scandir`            | NO        | YES  | netsurf-core/utils/file.c     |
| `getopt_long`        | partial   | YES  | netsurf-core/main.c (we replace main anyway) |
| `setjmp`/`longjmp`   | YES       | YES  | libdom uses both for parser recovery |
| `fnmatch`            | NO        | YES  | netsurf-core/utils                |
| `glob.h`             | NO        | YES  | netsurf-core/utils/filepath.c     |
| `pthread`            | stub      | YES  | libcss + libdom use mutexes — but only if HUBBUB_USE_PTHREADS is defined. Build without it. |
| `<locale.h>` real    | stub      | YES  | libcss number parsing trips on it |
| `<time.h>` strftime  | partial   | YES  | desktop/cookies                   |
| `<wchar.h>` real     | partial   | YES  | libparserutils utf-16 path        |
| `mmap` for files     | YES (anon only) | YES (full) | libdom's data backing |
| stdio `%n$` and `%a` | NO        | YES  | libcss parser error formatter     |
| `iswupper`/wctype    | NO        | YES  | libhubbub case-folding fallback   |
| `flockfile`          | NO        | YES  | netsurf-core/utils/log.c          |
| dynamic load (`dlopen`) | NO     | partial | libdom optional plugin loader — leave undefined |

**Verdict**: must build against musl (the FASE 13.0 path,
`USER_ELF_MUSL_SRCS`). The closest in-tree analog is the
`/bin/sqlite3` port — it links musl static and weighs ~6 MB. Expect
`/bin/oxnetsurf` to land around 4–8 MB.

### Build-system blockers

NetSurf libs ship a shared `buildsystem/` (their own Makefile macro
package). It expects:

- `pkg-config` for dep discovery
- `gperf` for libcss perfect hashes (already-generated `*.c` ships in
  the tarball, so we can skip running gperf)
- `nsgenbind` host tool for JS bindings — **skip; we drop JS**
- `re2c`-generated `*.c` for libcss tokenizer — ships pre-generated
- `lemon` parser for libcss — ships pre-generated
- `flex/bison`/yacc-generated `*.c` — none of the core seven need it

Practical approach: **do NOT use NetSurf's `buildsystem/Makefile`.**
Write our own per-lib `.a` rule in `GNUmakefile` (mirroring how we
handled BearSSL: a `find … -name '*.c'` glob, per-file override CFLAGS,
ar rcs into `lib<name>.a`). The pre-generated lookup tables ship in
the tarball under `src/`, so a flat compile of every .c works.

## 5. Staged plan

Each stage is a self-contained PR-worth of work. Each stage builds and
boots — i.e. `oxnetsurf` is functional (if limited) at every stage.

### Stage 1 — parse HTML to in-memory DOM, no rendering

Goal: `oxnetsurf file:///home/index.html` opens the file, runs it
through hubbub → libdom, dumps the resulting tree to stdout (or to an
Ox window as plain text). Proves the parser stack builds and links.

Tasks:
1. Compile libwapcaplet (1 .c) → libwapcaplet.a.
2. Compile libparserutils → libparserutils.a. Stub out iconv calls
   for now (`#define HAVE_ICONV 0` patch; UTF-8 only path).
3. Compile libhubbub → libhubbub.a. Disable pthread cb hooks.
4. Compile libnslog → libnslog.a. Single-process logger, no nl
   filter expressions.
5. Compile libdom (without expat fallback) → libdom.a. Only the
   hubbub binding, not the xml-parser binding.
6. `elfs/gui/oxnetsurf.c`: load file via stdio, feed bytes to
   `dom_hubbub_parser_parse_chunk`, traverse the DOM with
   `dom_node_get_first_child` etc., print `<tag attr="..">text` lines.

Acceptance: `dom_tree` matches `xmllint --html --xpath '//*'` for a
~10 KB sample HTML.

Estimated effort: 3–5 sessions. Biggest unknown is libparserutils
without iconv — may need to vendor a `iconv_stub.c` that only does
identity copy for UTF-8 and ASCII.

### Stage 2 — apply CSS, build computed-style tree (still no paint)

Goal: parse `<style>` blocks + linked `<link rel=stylesheet>` (loaded
synchronously from same dir), run libcss selectors, emit per-node
computed styles. No box layout yet — just `css_select_results` per
element.

Tasks:
1. Compile libcss → libcss.a. Pre-generated tokenizer (`parserutils_*`
   tables) ships in tarball; no re2c needed at build time.
2. Wire `libcss` to `libdom` via `netsurf-core/content/handlers/css/`
   adapter (~6K LOC).
3. Ship a minimal user-agent default stylesheet (50 lines) as a
   built-in C string array (no `messages` file machinery yet).
4. Resolve `<link href>` relative URLs in oxnetsurf's
   file-fetcher (no HTTP yet).

Acceptance: `oxnetsurf testcase.html` prints `color: rgb(...)` etc.
matching what Firefox shows in DevTools' "computed" panel for a
hand-picked subset (color, font-size, display, margin).

### Stage 3 — layout + paint to Ox surface

Goal: a recognizable rendering of `example.com`-tier HTML in an Ox
window. Block + inline boxes, basic font fallback, link underlines,
no images, no tables, no floats beyond left-aligned text.

Tasks:
1. Compile `netsurf-core/desktop/` + `netsurf-core/content/` +
   `content/handlers/html/` + `content/handlers/text/text_plain.c`.
2. Implement a `bitmap_t` plot table (NetSurf's `struct plotter_table`)
   pointing at Ox primitives:
   - `plot.rectangle` → `ox_draw_rect`
   - `plot.line` → two-pixel-tall `ox_draw_rect`
   - `plot.text` → `ox_draw_text` (or `ox_text_draw` if TTF on disk)
   - `plot.path` (paths/curves) → stub (return NSERROR_OK)
   - `plot.disc`/`arc` → stub (rectangle approximation)
   - `plot.bitmap` → no-op for v1 (no image support)
3. Replace `frontends/framebuffer/gui.c` with a slim `ox_gui.c` that
   drives one window per browser_window and pumps `ox_poll_event`.
4. Re-route `frontends/framebuffer/schedule.c` to `nanosleep`-based
   scheduling against the libc clock.
5. Pick an in-window scroll model: Ox window is non-scrollable, so
   simulate by an offset variable; mouse-wheel = scroll viewport.

Acceptance: `oxnetsurf https://example.com/` (via stub fetcher pointing
at a captured copy on disk) renders the heading + paragraph + the link
"More information..." in the correct visual location, with link
underlined.

Risk: NetSurf's plot API has ~15 entry points and three of them
(`bitmap`, `path`, `clip`) are non-trivial. Stage 3 is the longest.

### Stage 4 — input handling: links + scroll

Goal: clicking a link causes `oxnetsurf` to fetch the target and
re-render. Keyboard `PgUp`/`PgDn`/arrows scroll.

Tasks:
1. `ox_event_t` → `desktop/browser.c::browser_window_mouse_action()`
   translation. Map `OX_MOUSE_DOWN`/`UP` to NetSurf
   `BROWSER_MOUSE_PRESS_1` / `CLICK_1` / `DRAG_1`.
2. Same for keyboard: `OX_KEY_PGUP` → `NS_KEY_PAGE_UP`, etc. NetSurf
   has its own keycode enum in `netsurf/keypress.h`.
3. URL bar: reuse our `oxbrowser.c`'s URL-editor widget verbatim
   (extract to a shared `oxurl_widget.c` in `elfs/gui/`). Enter
   triggers `browser_window_navigate(url)`.
4. Back/forward stack: NetSurf's `desktop/browser_history.c` already
   does this — wire its `browser_window_history_back/forward`.

Acceptance: starting at a hand-coded `index.html` with two links,
clicking each one loads the corresponding subpage; back button
returns to the index.

### Stage 5 — wire real HTTP/HTTPS via BearSSL

Goal: type `https://example.com/` in the URL bar and it loads.

Tasks:
1. Lift the BearSSL helpers from `elfs/gui/oxbrowser.c` into a shared
   `elfs/gui/oxbrowser_net.c` + `.h` (raw GET, Host header, response
   buffering).
2. Replace NetSurf's `content/fetchers/curl.c` with `fetchers/oxnet.c`
   that implements the `fetcher_operation_table` against the
   oxbrowser helpers. The table is small (~10 entries: `initialise`,
   `acceptable`, `setup`, `start`, `abort`, `free`, `finalise`,
   `set_certs`, `poll`, `set_method`, `set_postdata`).
3. Use `libnsutils/url.c` for URL parsing (already vendored).
4. Stub `fetchers/file.c` against `fopen`/`fread` for `file://` URLs.
5. Skip `fetchers/about.c`, `fetchers/data.c` for v1.

Acceptance: `oxnetsurf https://example.com/` renders the real page
from the network; `oxnetsurf https://news.ycombinator.com/` renders a
recognizable HN frontpage (no images, no JS, but tile list + titles +
hyperlinks). Wikipedia article pages should be readable end-to-end.

### Stage 6+ (out of scope for groundwork)

- Image rendering: pull in libnsbmp + libnsgif + (eventually) libnspng
- Forms: input boxes + submit (POST encoding done by NetSurf already)
- Cookies: libnspsl + libnscookie (the latter is part of netsurf-core)
- Tabbed UI: multiple `browser_window` against one Ox window
- TTF rendering via Ox's existing FreeType-style backend

## 6. Build wiring (gated)

The Makefile section in the repo's `GNUmakefile` defines:

```
OXNETSURF_ENABLED ?= 0
NETSURF_DIR := vendor/netsurf
```

and per-lib `.a` archive rules, but they're inside an `ifeq
($(OXNETSURF_ENABLED),1) … endif` block. The default build does
not even try to descend into `vendor/netsurf/`.

To attempt the build (will fail until Stage 1 lands):

```
make OXNETSURF_ENABLED=1 build/elfs/gui/oxnetsurf.elf
```

The stub at `elfs/gui/oxnetsurf.c` compiles unconditionally as part of
`USER_ELF_MUSL_SRCS` — it just opens an Ox window with a "port pending"
message. The lib archives are only built when the gate flips.

## 7. Risk register

| Risk                                         | Likelihood | Mitigation                                 |
| -------------------------------------------- | ---------- | ------------------------------------------ |
| libdom expects expat — too painful to drop   | Low        | Stage 1 build flag disables xml-parser binding; hubbub-only is supported |
| libparserutils iconv usage spreads           | Low        | Confined to one .c file (`filter.c`); stub for UTF-8 path is ~50 LOC |
| Pre-generated tables in libcss are stale     | Very low   | They're checked into release tarballs; we don't run re2c/gperf |
| Plot API mismatch between NetSurf and Ox     | High       | Stage 3 is gated on this; stub unimplemented entries to NSERROR_OK |
| `/bin/oxnetsurf` exceeds the 5 MB heuristic  | Medium     | Strip debug; LTO if needed; sd.img is 128 MB so a 12 MB binary is still fine |
| musl + freestanding has hidden ABI mismatch  | Low        | Same flags as `/bin/sqlite3` which is in production |
| RAM pressure during layout                   | Medium     | osnos heap is sized for ~16 MB. Limit to small pages; reject pages >1 MB in raw HTML |

## 8. Pointers for the future implementer

- **First file to read**: `vendor/netsurf/netsurf-core/desktop/netsurf.c`
  — entry point and option parsing. Our `oxnetsurf.c` will replace
  `main()` but call into `netsurf_init()`/`netsurf_register()`.
- **Plotter table**: `vendor/netsurf/netsurf-core/include/netsurf/plotters.h`.
  This is THE interface to implement against Ox.
- **Hubbub parser usage**: `vendor/netsurf/netsurf-core/content/handlers/html/html.c`,
  function `html_create_html_data`. Copy that pattern in Stage 1.
- **CSS lookup tables**: `vendor/netsurf/libcss/src/select/autogenerated_*.c`
  — these are the pre-generated files we want to keep verbatim.
- **NetSurf options system**: `desktop/options.h`. We hardcode for v1.
- The framework expects `messages_load` to be called with translated
  strings from a `Messages` file. For v1, stub `messages_get` to
  return the key.

## 9. Open questions for the maintainer

1. Should we vendor the libs as `git subtree` (preserving history) or
   keep them as flat extracted tarballs? Current pick: flat tarballs,
   matching how BearSSL/SQLite/Lighttpd are vendored.
2. Do we want a single fat `libnetsurf.a` (all seven libs ar'd
   together) or seven per-lib archives? Current pick: seven, mirroring
   NetSurf's upstream split — simpler to debug, slightly larger total
   on disk.
3. JS engine eventually: duktape is already vendored and oxjs runs it.
   But NetSurf's nsgenbind output is huge (103K LOC of generated
   bindings). We'd be better off hand-writing a minimal duktape↔DOM
   bridge for the dozen most-used APIs (`document.getElementById`,
   `addEventListener`, `style.x = …`). Out of scope here.

## 10. Source provenance

| Vendor dir                | Tarball                                       | SHA (TODO post-fetch)  |
| ------------------------- | --------------------------------------------- | ---------------------- |
| `libwapcaplet/`           | libwapcaplet-0.4.3-src.tar.gz                 | —                      |
| `libparserutils/`         | libparserutils-0.2.5-src.tar.gz               | —                      |
| `libnsutils/`             | libnsutils-0.1.1-src.tar.gz                   | —                      |
| `libhubbub/`              | libhubbub-0.3.8-src.tar.gz                    | —                      |
| `libdom/`                 | libdom-0.4.2-src.tar.gz                       | —                      |
| `libcss/`                 | libcss-0.9.2-src.tar.gz                       | —                      |
| `libnslog/`               | from netsurf-all-3.11.tar.gz (subdir)         | —                      |
| `libnspsl/`               | libnspsl-0.1.7-src.tar.gz                     | —                      |
| `libnsbmp/`               | libnsbmp-0.1.7-src.tar.gz                     | —                      |
| `libnsgif/`               | libnsgif-1.0.0-src.tar.gz                     | —                      |
| `libnsfb/`                | libnsfb-0.2.2-src.tar.gz                      | — (kept for reference) |
| `libsvgtiny/`             | libsvgtiny-0.1.8-src.tar.gz                   | —                      |
| `buildsystem/`            | buildsystem-1.10.tar.gz                       | — (reference only)     |
| `netsurf-core/`           | from netsurf-all-3.11.tar.gz (subdir)         | —                      |

All tarballs published at:
- https://download.netsurf-browser.org/libs/releases/
- https://download.netsurf-browser.org/netsurf/releases/source-full/

License: every library is **MIT** (libwapcaplet, libparserutils,
libhubbub, libdom, libcss, libns*) except the NetSurf core itself
which is **GPLv2**. Our `/bin/oxnetsurf` ELF therefore inherits GPLv2.
osnos itself stays under its current license (no kernel change), and
the GPL boundary is the user-mode binary.
