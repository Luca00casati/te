# te — a simple GUI text editor in Zig + raylib

A minimal cross-platform GUI text editor for **Linux, macOS, and Windows**.
Single window, monospace grid, load/edit/save a plain-text file. Built with
[raylib](https://www.raylib.com/) (fetched by the Zig package manager) and Zig.

Text is rendered with **[UnifontEX](https://github.com/stgiga/UnifontEX)** — a
single font covering every Unicode plane — so Latin, Greek, Cyrillic, CJK,
symbols, and (monochrome) emoji all display out of the box. The font is fetched
and checksum-verified at build time, not committed to the repo.

![editor](docs/screenshot.png)

## Controls

Emacs-flavored. The **leader** is a **double-tap of Ctrl**; a **triple-tap**
jumps straight to the command prompt. Every action you run is echoed on the
bottom line (e.g. `move left x3`).

**Movement** (Emacs-style; the plain arrow/Home/End/PageUp keys are off by
default — re-enable them in `src/binding.zig`)

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
| Ctrl+S | Search (Enter to find; Ctrl+S then Enter repeats) |
| Leader then S / W / O | Save / save-as / open (minibuffer) |
| Leader then a name + Enter | Run a named command |
| Leader then H · Ctrl+H | Commands help · key help |
| Esc / Ctrl+G | Cancel minibuffer / clear selection & mark |
| Ctrl+Q / close window | Quit (asks y/n/c if unsaved) |

Named commands (leader, then type the name): `save`, `save-as`, `open`,
`find`, `undo`, `redo`, `copy`, `cut`, `paste`, `select-all`, `wrap`, `quit`.

There's an **Emacs-style minibuffer** on the bottom line: file open/save-as,
search, the quit confirmation, and transient echo messages all happen there.
Soft wrap is on by default (toggle with the `wrap` command); line numbers show
in the gutter; the caret blinks. **Saving resets the undo history**, and undoing
all the way back marks the file untouched again.

Open a file by passing it on the command line; it is created on first save if it
does not exist. `--screenshot <frames> <path>` renders that many frames, writes a
PNG, and exits (handy for headless checks):

```sh
zig build                       # produces ./te in the project root
./te notes.txt                  # or: ./te   (defaults to untitled.txt)
./te notes.txt --screenshot 10 out.png
```

## Dependencies

`build.zig` selects the right GLFW backend and system libraries per OS:

- **Linux** — X11/OpenGL. Install the dev packages (raylib's GLFW needs the
  headers + `.so`s):
  ```sh
  sudo apt install \
    libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libxext-dev libxrender-dev libxfixes-dev
  ```
  (Other distros: the equivalent `mesa`/`libX11` `-devel` packages.)
- **macOS** — links the Cocoa/IOKit/OpenGL frameworks; needs the Xcode command
  line tools (`xcode-select --install`). `rglfw.c` is compiled as Objective-C.
- **Windows** — links `gdi32`/`winmm`/`opengl32`/`user32`/`shell32`; no extra
  install beyond the Zig toolchain.

The **first** build (or after clearing `.zig-cache/`) downloads the ~14 MB font
over HTTPS, so it needs network access once; later builds use the cache.

## Building

```sh
zig build           # Linux/macOS: ./te   •   Windows: ./te.exe   (no zig-out/)
zig build run       # build and run
```

Cross-compile with the standard Zig flag, e.g.:

```sh
zig build -Dtarget=x86_64-windows
zig build -Dtarget=aarch64-macos
```

## How it works

- `src/main.zig` — the editor itself: a flat 1 MiB text buffer with a caret and
  selection anchor, all edits funnelled through one `edit()` primitive that
  feeds operation-based undo/redo (typing coalesced into one step), clipboard
  via raylib, mouse hit-testing, scrolling, and rendering (gutter line numbers,
  selection highlight, blinking caret, status bar, unsaved-changes dialog).
- **Text rendering** covers all of Unicode. A common set of codepoints is baked
  into a texture atlas; anything else (CJK, emoji, rarer scripts) is rasterized
  on demand into a per-codepoint texture cache (`src/glyphs.zig`). Glyphs are
  drawn with `FONT_BITMAP` + point filtering so UnifontEX stays pixel-crisp, and
  the column model is width-aware (full-width glyphs occupy two cells).
- **The font is fetched at build time.** Zig's package manager only accepts
  tarballs/git, so `tools/fetch_font.zig` downloads UnifontEX over HTTPS with
  `std.http` (no `curl`), verifies its SHA-256 (a mismatch fails the build), and
  `build.zig` embeds the result via an anonymous import. It lands in the
  gitignored build cache, so the large font never enters git history.
- `src/config.zig` — all the tunables (window size, font size, colors, tab,
  buffer capacity). Font size is 16 by default; multiples of 16 stay crisp.
- `src/binding.zig` — the key → action map. The leader is armed by a double-tap
  of Ctrl (`detectCtrlTaps` in `main.zig`); edit `bindings`/`prefix_bindings`/
  `commands` to rebind.
- `main` takes a `std.process.Init`, so file I/O goes through the Zig standard
  IO layer (`std.Io.Dir.readFileAlloc` / `writeFile` with the runtime-provided
  `io`), and the filename argument comes from `init.minimal.args`.
- `build.zig` compiles raylib's C sources (`rcore`, `rglfw`, `rshapes`,
  `rtextures`, `rtext`) for the OpenGL-3.3 desktop backend, picking the X11 /
  Cocoa / Win32 GLFW backend and system libraries from the target OS. It
  translates `raylib.h` into Zig bindings via `addTranslateC` and copies the
  built binary into the project root as `./te` (`./te.exe` on Windows).
  (It compiles raylib's source directly rather than calling raylib's own
  `build.zig`, which targets stable Zig and doesn't build under this nightly.)

## License

This project is licensed under the [MIT License](LICENSE).

Third-party components keep their own licenses:

- **raylib** (fetched via `build.zig.zon`) — zlib/libpng license.
- **UnifontEX** (fetched at build time, not bundled) — derived from GNU Unifont;
  distributed under the SIL Open Font License 1.1 and the GNU GPLv2 with the
  font-embedding exception.
