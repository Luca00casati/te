# te — a simple GUI text editor in C + raylib

A minimal GUI text editor, currently built and tested on **Linux** (the code
is portable C11, so other platforms are mainly a matter of adding their link
steps to `nob.c`). Single window, monospace grid, load/edit/save a plain-text
file. Built with [raylib](https://www.raylib.com/) and PCRE2 (both linked
from the system install), plus a small from-scratch Prolog engine for
scripting, using [nob](https://github.com/tsoding/nob.h) as the build system.

Text is rendered with **[UnifontEX](https://github.com/stgiga/UnifontEX)** — a
single font covering every Unicode plane — so Latin, Greek, Cyrillic, CJK,
symbols, and (monochrome) emoji all display out of the box. `UnifontExMono.ttf`
is bundled in the repo and loaded from disk at startup.

![editor](docs/screenshot.png)

## Controls

Emacs-flavored. The **leader** is a **double-tap of Ctrl**; a **triple-tap**
jumps straight to the command prompt. Every action you run is echoed on the
bottom line (e.g. `move left x3`).

**Movement** (Emacs-style; the plain arrow/Home/End/PageUp keys are off by
default — re-enable them in `src/binding.h`)

| Key | Action |
| --- | --- |
| Ctrl+B / F / P / N | Left / right / up / down |
| Ctrl+Shift+A / E | Line start / end |
| Ctrl+W / Ctrl+Shift+W | Next / previous word |
| Ctrl+D | End of word |
| Ctrl+J / Ctrl+Shift+J | Screen up / down |
| Ctrl+`<digit>` | Repeat the next action **or typed character** N times (`Ctrl+3` `w` → `www`) |

**Selection & clipboard**

| Key | Action |
| --- | --- |
| Shift + any move | Extend the selection |
| Ctrl+Space | Toggle the mark — then movement selects |
| Ctrl+C / X / V | Copy / cut / paste |
| Leader then A | Select all · Leader then Space: select line |
| Click / drag | Place caret / select |

**Editing & lines**

| Key | Action |
| --- | --- |
| Type / Enter / Tab | Insert text (Tab = 4 spaces) |
| Ctrl+Enter / Ctrl+Shift+Enter | Open a blank line below / above |
| Backspace / Delete | Delete before / at caret (or selection) |
| Ctrl+Backspace | Delete forward |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo |
| Ctrl+Shift+X / C / V | Cut / copy / paste the **whole line** |
| Ctrl+Shift+F / B | Shift line left / right one space (out/indent) |
| Ctrl+Shift+N / P | Move the whole line down / up |

**Files, search & commands**

| Key | Action |
| --- | --- |
| Ctrl+S / Ctrl+Shift+S | Incremental search / regex search (Perl/PCRE) |
| Ctrl+R / Ctrl+Shift+R | Query-replace / regex query-replace |
| Leader then S / W / O | Save / save-as / open (minibuffer) |
| Leader then a name + Enter | Run a named command |
| Leader then H / N | Commands help · key help |
| Esc / Ctrl+G | Cancel minibuffer / clear selection & mark |
| Ctrl+Q / close window | Quit (asks y/n/c if unsaved) |

**Search:** `Ctrl+S` (literal) or `Ctrl+Shift+S` (regex) opens the search
prompt. It jumps to the nearest match as you type and shows `Match X/N` in the
echo area; **`Ctrl+N` / `Ctrl+P`** step to the next / previous match. `Enter`
keeps the current match, `Esc`/`Ctrl+G` returns to where you started, and an
empty query + `Enter` repeats the last search. Regex is full **Perl/PCRE**
syntax (`\d \w \s \b`, `(?:…)`, lookaround, `*?`, backreferences, `(?i)`, …) —
the same dialect as `grep -P`, not POSIX BRE/ERE.

**Replace** (`Ctrl+R` / `Ctrl+Shift+R`, or the `replace` / `replace-regex`
commands) prompts for a pattern then a replacement and runs an **interactive
query-replace**: like search it highlights matches with `Ctrl+N`/`Ctrl+P` to move
between them, but `Enter` replaces the current match and advances, staying in
replace mode until `Esc`. `replace-all` / `replace-all-regex` substitute
**every** match at once (one undo step). In the regex variants the replacement
may use `$1`, `$0`, etc. `search-reverse` / `search-regex-reverse` (command-only)
search toward the top of the file.

Named commands (leader, then type the name): `save`, `save-as`, `open`,
`search`, `search-regex`, `search-reverse`, `search-regex-reverse`, `replace`,
`replace-regex`, `replace-all`, `replace-all-regex`, `undo`, `redo`, `copy`,
`cut`, `paste`, `select-all`, `toggle-wrap`, `quit`. (Each name is the action's
own tag, so they can't drift.)

There's an **Emacs-style minibuffer** on the bottom line: file open/save-as,
search, the quit confirmation, and transient echo messages all happen there.
Soft wrap is on by default (toggle with the `wrap` command); line numbers show
in the gutter; the caret blinks. **Saving resets the undo history**, and undoing
all the way back marks the file untouched again.

Open a file by passing it on the command line; it is created on first save if it
does not exist. `--regex <pattern> [file] [out]` is a **headless grep** — it
reads `file` (or **stdin** if omitted), writes each matching line (`lineno:text`)
to `out` (or **stdout**), and exits without a window (exit 0 if any matched, 1 if
none, 2 on error), so it pipes like `grep -P`. `--screenshot <frames> <path>`
renders that many frames, writes a PNG, and exits.

```sh
cc -o nob nob.c && ./nob            # produces ./te in the project root
./te notes.txt                     # or: ./te   (defaults to untitled.txt)
./te --regex 'foo\d+' notes.txt    # headless: print matching lines, then exit
cat notes.txt | ./te --regex '\bTODO\b'      # read stdin
./te --regex 'foo\d+' notes.txt hits.txt     # write results to hits.txt
./te notes.txt --screenshot 10 out.png
```

## Scripting

`te` loads an optional Prolog init script at startup —
`$XDG_CONFIG_HOME/te/init.pl`, falling back to `~/.config/te/init.pl` — the
way Emacs loads `.emacs` or Vim loads `init.vim`. A missing file is fine (te
just uses its `src/binding.h` defaults); a script error is echoed on the
status line instead of stopping `te` from starting. See
`docs/init.pl.example` for a starting point.

Bindings and hooks are plain **facts and rules**, re-checked fresh every time
they're needed rather than registered once as an opaque callback — so a
clause can be a rule whose body consults live editor state, not just a fixed
action:

| Predicate | Does |
| --- | --- |
| `key_binding(Key, Mod, Handler)` | Bind a top-level key to any goal, or an existing action via `te_action(Name)` |
| `leader_binding(Key, Mod, Handler)` | Same, but as a leader chord (after the double-Ctrl-tap leader) |
| `hook(Event, Handler)` | Run `Handler` whenever `Event` fires: `post_save` or `post_open` |
| `te_action(Name)` | Run a named command (anything in `COMMANDS`, `src/binding.h`) |
| `te_echo(Msg)` | Write a message to the status line |
| `te_insert(Text)` | Insert text at the cursor (or replace the selection) — goes through the same path as typing, so undo/redo work |
| `te_text(Text)` | Unify `Text` with the whole buffer |
| `te_cursor(Pos)` / `te_set_cursor(Pos)` | Get/set the cursor as a byte offset |

`Key` is a single-character atom (`g`, `3`) or one of `space`, `enter`,
`tab`, `backspace`, `delete`, `escape`. `Mod` is `any`, `ctrl`, or
`ctrl_shift` (underscore, not hyphen — a bare Prolog atom can't hold one).
`Handler` is any goal — a custom predicate, or a direct `te_action(Name)`
call (action names like `"select-all"` have a hyphen, so pass them as a
quoted string or atom rather than bare). Script bindings are checked before
the built-in `BINDINGS`/`PREFIX_BINDINGS` tables, so they can override a
default; an uncaught error inside a handler is echoed rather than crashing
`te`.

The engine (`src/prolog.h`/`prolog.c`) is a small **from-scratch Prolog**,
not a wrapper around an external library: facts/rules, unification,
backtracking, cut (`!`), if-then-else, arithmetic, `catch/3`/`throw/1`,
`assert`/`retract`, `findall/3`, and a practical-subset parser with a fixed
infix operator table (no user-defined `op/3`) — enough for real config logic,
not full ISO Prolog.

## Dependencies

Linux, X11/OpenGL. Install:

- **raylib 6.0** and **libpcre2-8**, on your compiler's default include/library
  search paths (build raylib from source and `[sudo] make install`, or your
  distro's dev packages if it has them — e.g. `libpcre2-dev` on
  Debian/Ubuntu). `nob.c` links them as plain `-lraylib -lpcre2-8`; if yours
  land somewhere nonstandard, add the matching `-I`/`-L` flags there.
- The system libraries raylib's static archive needs at link time:
  ```sh
  sudo apt install \
    libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libxext-dev libxrender-dev libxfixes-dev
  ```
  (Other distros: the equivalent `mesa`/`libX11` `-devel` packages.)

`UnifontExMono.ttf` is bundled in the repo, so nothing to fetch for it.

## Building

```sh
cc -o nob nob.c && ./nob   # produces ./te in the project root
./nob run                  # build and run
```

`nob` is a small C program (`nob.c`, using the vendored `nob.h`) that
self-rebuilds when it changes; there's no separate build-system binary to
install. It's currently Linux-only — see `nob.c` for where per-OS link steps
would go to support macOS/Windows.

## Testing

```sh
./nob test
```

Two suites, both under `tests/`:

- **`unit_te.c`** — white-box tests of the buffer/undo/search/UTF-8 logic,
  plus the Prolog scripting API (`tests/fixtures/init_test*.pl`). Since
  everything in `main.c` is `static` with no header, it `#include`s `src/main.c`
  directly (with `main()` renamed out of the way) to reach those functions;
  nothing here opens a window.
- **`cli_te.c`** — black-box tests that run the compiled `te` binary as a
  subprocess against `te --regex <pattern> [in] [out]`, the headless mode
  that exits before any window is created, checking stdout/exit codes/file
  output.

## How it works

- `src/main.c` — the editor itself: a flat 1 MiB text buffer with a caret and
  selection anchor, all edits funnelled through one `edit()` primitive that
  feeds operation-based undo/redo (typing coalesced into one step), clipboard
  via raylib, mouse hit-testing, scrolling, and rendering (gutter line numbers,
  selection highlight, blinking caret, status bar).
- **Text rendering** covers all of Unicode. A common set of codepoints is baked
  into a texture atlas; anything else (CJK, emoji, rarer scripts) is rasterized
  on demand into a per-codepoint texture cache (`src/glyphs.c`), backed by a
  small open-addressing hash table. Glyphs are drawn with `FONT_BITMAP` + point
  filtering so UnifontEX stays pixel-crisp, and the column model is width-aware
  (full-width glyphs occupy two cells).
- **The font is loaded from disk at startup**, from the same directory as the
  running executable. `UnifontExMono.ttf` is bundled in the repo rather than
  embedded into the binary; `te` exits with a clear error if it can't find it.
- `src/config.h` — all the tunables (window size, font size, colors, tab,
  buffer capacity). Font size is 16 by default; multiples of 16 stay crisp.
- `src/binding.h` — the key → action map. The leader is armed by a double-tap
  of Ctrl (`detectCtrlTaps` in `main.c`); edit `BINDINGS`/`PREFIX_BINDINGS`/
  `COMMANDS` to rebind.
- `src/prolog.c`/`prolog.h` — the from-scratch Prolog engine (see Scripting
  above): tokenizer, operator-precedence parser, term representation,
  unification, a backtracking CPS solver with cut/catch/throw, and a small
  built-in predicate library. Not editor-specific — doesn't know `te` exists.
- `src/script.c` — the Prolog integration (registers `te_*` native
  predicates, loads `init.pl`). `src/editor.h` is the small explicit surface
  `main.c` exposes to it (run an action, echo, insert text, read/move the
  cursor) so script.c never reaches into `main.c`'s static state directly;
  `handleInput`/`handlePrefix` in `main.c` check script-registered bindings
  before the built-in `BINDINGS`/`PREFIX_BINDINGS` tables, and
  `saveFile`/`openPath` call `scriptRunHook` for `hook/2` listeners.
- `nob.c` compiles `src/main.c`, `src/glyphs.c`, `src/script.c`, and
  `src/prolog.c` with `cc -std=c11` and links against the system-installed
  raylib and libpcre2-8 (plain `-lraylib -lpcre2-8`; the Prolog engine adds no
  external dependency), plus the Linux desktop system libraries raylib's
  GLFW backend needs, into `./te` in the project root.
- **Regex search** uses PCRE2's 8-bit API directly (`#define
  PCRE2_CODE_UNIT_WIDTH 8` + `#include <pcre2.h>`), linked from the system
  install — no vendored copy.

## License

This project is licensed under the [MIT License](LICENSE).

Third-party components keep their own licenses:

- **raylib** (linked from the system install) — zlib/libpng license.
- **PCRE2** (linked from the system install) — BSD license.
- **UnifontEX** (bundled as `UnifontExMono.ttf`) — derived from GNU Unifont;
  distributed under the SIL Open Font License 1.1 and the GNU GPLv2 with the
  font-embedding exception.
- **nob.h** (vendored, public domain) — https://github.com/tsoding/nob.h
