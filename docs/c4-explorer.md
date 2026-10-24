# Source maps, Code Explorer, and windowed programs on the C4IX desktop

A plan, written 2026-10-22, before any of it is built. Nothing here is
done yet. It covers three pieces that depend on each other, and a fourth:

1. **Source maps in C4R** (track S): an optional section that maps each
   code word to a file, a line and a function. Every compiler emits it,
   c4rlink merges it, and every loader skips it.
2. **Code Explorer** (track E), after Matt Godbolt's Compiler
   Explorer: C source next to the C4 VM code it became, each source line
   linked to its instructions. It must cope with a program the size of
   Wolfenstein 3D (36,000 lines of C, 167,347 code words).
3. **Windowed programs on the C4IX desktop** (track W): any program can
   open real windows, not only the tools built into the desktop. The IDE
   leaves the desktop and becomes the first such program; the explorer is
   the second. System Monitor stays built in, because it is what you need
   when a program goes wrong.

A fourth piece came out of the review: **Wolfenstein 3D moves to a
repository of its own** (track G), which a user can choose to fetch.
When it is there, the game and its source go into the C4IX image, ready
to explore.

The facts below were read at `7b166ea` on `c4ix-userland`. File and line
references are to that commit.

Decisions taken (the user, 2026-10-22):
- **The version byte stays as it is.** The source map is a trailer that
  announces itself (2.1).
- **Maps are on by default** in every compiler, kernels and libraries
  included, because the point of this is learning. `-g0` turns them off.
  `c4rlink -s` and c4rstrip remove them, which is what C4SK's sealed
  builds already run (`c4rlink32 -s`).
- **Each source file's CRC-32 is in the map**, so a reader can tell at
  once whether a source has changed since it was compiled.
- **The window protocol uses fds 3 and 4.** A client's stdout and stderr
  go to a log.
- **One IDE process with many windows**, rather than one process per
  window.
- **Wolfenstein 3D becomes an optional repository** with its port as a
  history of commits (track G).
- **The name is "Code Explorer"**, `/bin/explore`, for now.
- **An explorer outside the VM** is wanted at some point; when is not
  decided (E4).
- **The game's repository is `~/git/c4-wolf3d`**, cloned from the
  Bitbucket project it came from, with its own remote on GitHub,
  `git@github.com:andrakis/c4-Wolf3D.git`. Nothing goes back to
  Bitbucket. The old worktree `~/git/c4-wolf` is removed once
  it is checked again. `make wolf3d-fetch` downloads the shareware data
  instead of a repository carrying it.

## 1. Where things stand

**Source positions in the compilers.** No compiler keeps a line number
past its parser.

| | file | line | where code is emitted | optimizer |
|---|---|---|---|---|
| c4lc | not on tokens: `pp:include` splices a header's tokens between `PPDir` markers that the parser never sees (pp.lisp:247-256, 720-726) | token `(KIND VALUE LINE [flag])`; read only for errors (parse.lisp:71-76) | `g:emit` onto a labelled instruction list (gen.lisp:79-89); word offsets exist only inside `c4r:encode` (c4r.lisp:442-453) | tree passes T1-T3 on the AST, then the peephole passes on the list (c4opt.lisp:405-443) |
| c4fc | `t.file`, a serial per lexed buffer (lex.f:190-210, pp.f:614-635) | `t.line` | straight into the CODE buffer (emit.f:145-165) | under `-O`, `opt.f` decodes the words into items, mirrors c4opt's passes and re-encodes through `LMAP` (opt.f:115-184) |
| c4cc | none: several sources are concatenated into one buffer (c4cc.c:2016-2033) | one global `line` (c4cc.c:82, 610-619) | `*++asmc4r_e = ...` as it parses | none |

Things that get in the way:

- `#line` and gcc's `# N "file"` markers are ignored by all three
  (pp.lisp:703-705, pp.f:700, 722, c4cc.c:620-622). With `gcc -E` input,
  which several tests and builds use, the lines are lines of the `.i`
  stream.
- c4lc's lexer does not count a backslash-newline (lex.lisp:32-34), so
  every line after a continuation is short by one (c4lc-design.md:564).
- c4lc's passes match exact shapes. An extra AST field changes T3's
  inlining budget (`t:size` counts nodes, tree.lisp:481-487). An extra
  element on an instruction breaks `c4r:place`, which sizes an item by
  its length. A marker in the list blocks fold, jmpnext, thread, tail and
  fuse just as a label does, and unreferenced labels already cost about
  6% of fusion (fused-opcodes.md:366-392).
- c4cc's one-token lookahead credits code to the line where the lexer
  saw the next newline: a function's ENT lands on the line after its
  declarator.

**The listing that exists.** `c4cc -S` prints each source line and then
the words emitted while it was read (asm-c4r.c:472-487). That is a line
table in all but name. c4lc has no listing. `c4rdump -c` disassembles
with no labels and no symbol names, from code that the loader has
already patched (c4rdump.c:19-38).

**C4R readers.** There are 24 of them; appendix A lists them. Two facts
decide the format:

- **No reader looks past the S segment, and none checks the file's
  length against the end of S.** Bytes appended after S are ignored by
  all of them. c4rlink made the same observation about symbols when it
  put `__c4_needed` in one (c4rlink.c:57-60).
- **Two readers reject a version they do not know:** load-c4r.c accepts
  0, 2 and 3 (load-c4r.c:395-401), and oisc4 the same (oisc4.c:707).
  load-c4r.c is inside C4KE, c4rlink, c4rdump and c4cc. A v4 image makes
  c4rlink dereference a null pointer, because `cl_load` never checks
  `C4R_LOADCOMPLETE` (c4rlink.c:712-746), and makes c4rdump crash in
  `c4r_dump_dynamic` (c4rdump.c:46-51).

Trailing bytes still cost memory wherever the whole file is read. C4IX's
loader keeps the file buffer of a cached `-fpic` library for as long as
the machine runs (loader.c:547-562). c4l.c, c4mp's loader and C4DOS's
RUN read into fixed buffers (8 MB, 4 MB, 4 MB) with no bound check.

**The desktop.** It is one task. It owns the display's only ring, and
the last task to write `GUI_RING` wins (gui-device.js:131-144). It draws
every window itself, and wm.c reaches each tool through if-ladders on
`w->kind` in nine places. A program can already draw, but only inside a
Terminal: `window.h` writes `ESC _ G op args ESC \` to stdout, and
events come back on stdin (window.c, term.c:247-314). That canvas is a
fixed 640x400. A command is at most 300 bytes and a display list at most
6,000 words. It cannot have menus or dialogs, and it must be started
from a shell.

C4IX has no memory protection between tasks. `PRIV_USER` traps the host
I/O opcodes and nothing else, and c4m's SEGV trap is commented out
(c4m.c:2103). Wolfenstein's `win_fb` already hands the desktop a pointer
into the game's memory. Pipes never block a writer and grow without
bound (vfs.c:674-683). `SYS_EVWAIT` wakes on any readable pipe the task
holds. A task has at most 16 descriptors (`FD_MAX`, c4ix.h:113).

**The IDE.** `edit.c` is 2,460 lines and the desktop's largest module
(a 225 KB object). It needs a 4 MB c4sc stack to compile. It reaches into
the desktop for:

- the `d_*` drawing layer, about 40 calls;
- ui.c's scroll bars and buttons;
- wm.c's `win_open`, `win_menus`, `win_title` and `win_close`;
- the dialogs: `msgbox`, `inputbox` and `dialog_text`;
- `run_command`, which opens a Terminal;
- the dirty marks.

Its file pane, build and error list are its own. It does not use
files.c.

## 2. Source maps in C4R (track S)

### 2.1 Where the section goes, and the version byte

The source map goes in an **extension trailer** after the S segment. It
is the first entry of a general mechanism:

```
... S segment (as today, possibly empty) ...
W   'X'                 marker: extension chunks follow (absent = none)
    repeat:
W     tag               four ASCII bytes, low byte first ("SMAP")
W     length            payload bytes
B*    payload           then zero bytes to the next word boundary
W   0                   end of chunks
```

- A reader that does not know a tag skips its length.
- A tag's fourth letter says what a tool may do with the chunk when it
  moves or deletes code. Upper case means drop it; lower case means
  copy it unchanged. This is PNG's safe-to-copy bit. `SMAP` must be
  dropped by a tool that changes code without understanding the map.
- Readers find the trailer by walking S, which every reader that wants
  symbols already does. A stripped image still has its S marker
  (c4rlink.c:667).

**The version is not bumped** (decided).
- The version byte guards what a loader must understand to load an
  image correctly. v3 needed a bump because MEMSZ changed the meaning of
  header bytes, and a v2 loader would have lost the BSS. A source map
  changes nothing a loader reads, and every loader in the repo already
  ignores it.
- Bumping would break load-c4r.c's strict check in every built copy of
  C4KE, c4rlink and c4rdump. It would also tie the map to v3+: c4cc
  writes v2, and a v2 image has no MEMSZ, so `c4cc -g` would also have to
  start writing MEMSZ.
- The precedent is followed where it applies:
  - load-c4r.c keeps accepting the current and the prior version;
  - an image with no trailer is written exactly as today, byte for byte,
    so every pinned size and `cmp` stays;
  - a future change to the loadable part still bumps the version.

c4rlink and c4rdump still gain a `LOADCOMPLETE` check (S0). Today a
damaged or future image makes them crash instead of saying why.

### 2.2 The SMAP chunk

Its payload is bytes, with unsigned and signed LEB128 numbers (`u`,
`s`). Code offsets are counted in words, so a map is the same at 32 and
64 bits, just as the code word counts are.

```
u8   map version (1)
u8   flags (bit 0: files carry a CRC-32; set by every compiler here)
u    nfiles
     per file:   u pathlen, path bytes, u size in bytes, [u32 crc32]
u    nfuncs
     per func:   u start (delta from the previous start), u length in words,
                 u namelen, name bytes, u file, u line
u    nrows
     per row:    u (dcode << 1 | newfile), [u file], s dline
```

- **Files.** A file's path is stored as the compiler was given it, after
  `-I` resolution, normalised (no `./`, no `a/../`). The repository's
  builds give repo-relative paths (`games/wolf3d/wl_draw.c`). C4IX has the
  repository at `/usr`, so a reader looks for a relative path under the
  image's own directory, then `/usr`, then the search roots it is given.
  The size and CRC let the explorer say "this source has changed since
  it was compiled" rather than show the wrong lines.
  - The CRC is IEEE CRC-32, as libarc's and gzip's, over the bytes the
    compiler read.
  - Each compiler computes it once per file, as it reads the file: in
    Lisp for c4lc, in Forth for c4fc, in C for c4cc.
  - The cost is a table lookup, a shift and two XORs per byte. That is
    noticeable in interpreted c4lc, and S1 measures it on wolf3d's
    largest unit. If it is too slow, c4sp gets a `string:crc32` builtin,
    as it has other string primitives.
- **Functions.** There is one per emitted body:
  - static functions, whose plain names repeat across units (wolf3d has
    60 such);
  - the code c4lc emits without a symbol: the M4 helpers (`ld16s`,
    `udiv`, ...), the exit-255 stubs and `__c4lc_init`, under names such
    as `<ld16s>`.

  The table is the map's own, not the S segment's, so it works in a
  stripped image, and the profiler can stop charging the helpers to the
  function before them (c4ix-wolf3d.md, last bullet).
- **Rows.** A row says that the code from its offset up to the next
  row's offset came from FILE:LINE. Line 0 means compiler-made code with
  no source line. Rows are sorted by offset, so a lookup is a binary
  search.

**What gets a row.** This is the same in every compiler, so that c4fc's
map can be byte-identical to c4lc's:

- every statement, at the line of its first token;
- a function's entry (ENT), at the line of its name;
- its exit (LEV, and the jump to a shared exit), at the line of the
  closing brace;
- a `for`'s condition and step, at the line where each begins;
- a declaration with an initializer, as a statement;
- code that comes from a macro, at the line where the macro was used, as
  gcc does it;
- code that T3 inlined, at the callee's own lines. Its rows say where
  the inlined code came from, as gcc's line table does. There is no
  separate "inlined at" record in version 1.

There are no columns in version 1.

**Size.** I estimate 2 to 4 bytes per row. Wolfenstein 3D has about
30,000 statements, which would give a map of 100-150 KB against a
1.13 MB image. S1 measures it.

### 2.3 How the compilers carry lines (the one hard part)

**The invariant.** A map must not change one byte of the C, D, P, c, d
or S segments: `-g` and `-g0` differ only in the trailer. Every
milestone tests this on the whole corpus. It is what lets "compile
everything with source maps" ship the same code, and it keeps c4fc and
c4lc identical.

**The default.** While S1-S5 build each compiler's maps, `-g` is a
flag the tests pass. The default turns on in S6, in all four compilers
at once (c4lc, c4sc, c4fc, c4cc), once all of them produce maps.
Turning it on earlier would break test-c4fc's `cmp` of c4lc against a
c4fc that has no maps yet. `-g0` turns it off. These keep their exact
bytes by passing `-g0`, or by linking with `-s`:
- the reference images that tests `cmp` against;
- the pinned sizes (test-b4ke's 1,898 bytes);
- C4SK's sealed images.

**c4lc.**

- **Tokens carry a location.** A location is one integer,
  `file * 2^20 + line`, in the third field. The fourth field is taken.
  - `p:line`, error messages and `__LINE__` read it through one
    accessor.
  - Errors can then name their file. Today c4lc's `line N:` names none,
    and the IDE has to guess which source it was.
  - The pp marks each include's tokens with that file's number.
  - The pp honours `#line` and gcc's `# N "file"` markers, so `gcc -E`
    input maps to the original files.
  - The backslash-newline count is fixed first, on its own, because it
    changes `__LINE__`.

  Whether 2^20 lines and the c4sp32 integer range leave enough files is
  checked in S1. The fallback is a side table indexed by token.
- **Statements carry it in the tree.** The parser wraps each statement
  in a `(Line LOC stmt)` node, but only under `-g`, so the AST golden
  output is unchanged.
  - `t:size` counts a `Line` node as zero, so T3 inlines exactly the
    same calls.
  - T1 and T2 look through `Line` nodes wherever they match a statement
    shape (the last statement is a return, an empty block, and so on).
  - The `-g` invariant over the corpus is what finds a pass that does
    not look through.
- **Instructions carry it as a zero-size item.** `g:stmt` emits
  `(line LOC)` before a statement's code. Every peephole window steps
  over these items, which one helper does (`c4opt:next-real`).
  - When a pass deletes or merges instructions, the line items before
    them stay, and so describe what follows.
  - A fused or folded instruction takes the line of its first part.
  - Two line items in a row become one, and the later one wins.
  - Dead code takes its line items with it, as far as the next label.
  - `c4r:place` gives a line item size 0. `c4r:encode` turns the line
    items into rows, and the function labels into the function table.
- **c4r.lisp decodes and encodes the trailer.** `c4r:decode` turns an
  `SMAP` back into line items. Then `c4opt-run.lisp`, which optimises an
  existing image, carries the map through its own passes the same way,
  and `c4r-roundtrip.lisp` stays byte-exact on mapped images. Chunks it
  does not know are kept or dropped by their tag's case (2.1).
- **c4sc.** c4sc is c4lc's Lisp turned into C. It gets all of this by
  regenerating `src/c4sc/c4*_gen.c`. Its flag parser is hand-written
  (body.h:151-245) and needs `-g` added. C4IX's `cc` passes `-g`
  through.

**c4fc.**

- Tokens already have a file and a line. File serials are per buffer,
  so a header included twice has two serials. The map's file table is
  keyed by path, not by serial.
- `(STATEMENT)` is the one statement hook (parse.f:757-833). It records
  `(CHERE, file, line)` into a row buffer, which `EMIT-RESET` clears,
  because `-O` runs `PROGRAM` twice (c4fc.f:105-125).
- `opt.f`'s DECODE turns the rows into line items, which its passes step
  over by the same rules as c4opt's. ENCODE turns them back, through
  `LMAP`.
- T3 in Forth counts tokens, not nodes (inline.f), so rows cannot change
  its decisions.

**c4cc.**

- `next()` keeps the line of the token it returns, not the line the
  lexer has reached. Each statement and function start records
  `(word offset, file, that line)`.
- The concatenation buffer knows where each source starts, and `# N
  "file"` lines set the file and line instead of being skipped.
- `emit_rewind_li` and `emit_rewind_lc` take back a word, so a row past
  the new end is dropped.
- `-g` already means "keep static symbols" (asm-c4r.c:155), and it keeps
  that meaning. The map follows the default of the other compilers: on
  from S6, off with `-g0`.

### 2.4 c4rlink, c4rstrip and c4rdump

- **load-c4r.c** gains `C4ROPT_SRCMAP`, which reads the trailer into a
  new `C4R_SRCMAP` slot after S (load-c4r.c:576-604). Without the option
  it stops where it stops today.
- **c4rlink** concatenates code in command-line order and never drops a
  function (c4rlink.c:22-30), so merging is a rebase:
  - file tables are unioned by path;
  - function starts move by the module's `CL_OFFSET_CODE`;
  - rows are appended in module order.

  A module with no map contributes one line-0 row at its start, so the
  previous module's last row does not cover it. `-s` drops the map with
  the symbols. `-g0` drops only the map. That is for a dynamic image,
  which needs its symbols and so cannot take `-s`.
- **c4rstrip** drops the trailer as well as the symbols.
- **C4SK** (the closed security kit, `~/git/c4sk`) seals images linked
  with `c4rlink32 -s`, so they carry no map. Its `verify.mjs` checks
  only that the segments fit inside the file ("format", `body <=
  image.length`). When maps arrive, it should require that nothing
  follows S. That is a one-line change in C4SK's own repository, noted
  here because the default-on map is what makes it matter.
- **c4rdump** gets three new modes:
  - `-l` dumps the map: files, functions and rows;
  - `-S` is `objdump -S`: each function's source lines, each followed
    by its instructions;
  - `-c` gains labels and symbolic operands. It reads the code
    unpatched and the patch table beside it, so a `JSR` shows the
    function's name, an `IMM` of data shows `name+off`, or a string's
    first characters, and a jump shows `.L12`.

  These go in a small library both c4rdump and the explorer use (E0).
- **The loaders** need no change to load a mapped image. Two changes
  are about memory:
  - C4IX's loader keeps only the bytes up to the end of S in a cached
    library's buffer (loader.c:547-562);
  - the unbounded reads in c4l.c, c4mp and C4DOS's RUN get a bound,
    since mapped images are larger.

### 2.5 What reads the map besides the explorer

These come once maps are everywhere (S7), each small:

- load-c4r.c's `c4r_print_stacktrace` prints `wl_draw.c:345 ScalePost+12`;
- libjs's STRC stack traces (boot.js `parseSymbols`, c4m.js:520-540);
- C4IX's "killed: illegal opcode" message reads the map of the task's
  image lazily, from its file, only when it kills a task;
- `prof-c4ix.mjs USERIMGS=` charges cycles by line as well as by function.

## 3. Code Explorer (track E)

**E0, a library and a text explorer.** There is a library of C4R
reading: the header, segments, patches, symbols and the trailer. With it
goes a disassembler that gives symbolic operands, and source line
lookup both ways: from a line to its code ranges, and from an offset to
its row and function. c4rdump uses it for `-l` and `-S`, on the host
and in C4IX, and that is already a useful explorer: `c4rdump -S
/usr/games/wolf3d/wolf3d | less`.

There must be one opcode table, so names cannot drift. It includes the
fused opcodes (`-mfuse`, which wolf3d uses). A test checks it against
c4m.c's enum.

**E1, Code Explorer's window** (`/bin/explore`), a program on the
desktop (track W). It has three panes:

- **Functions and files**, with a filter box: wolf3d has 888 functions in
  28 units.
- **Source**: the function's file. Each line that produced code gets a
  colour band, from a small palette cycled by row. Lines with no code are
  plain.
- **Code**: the function's disassembly. Each instruction is in its row's
  colour, with offsets, labels and symbolic operands.

How the panes link:

- Clicking a source line highlights all its instructions and scrolls to
  the first. There may be several ranges: a `for` condition, or copies
  of an inlined callee.
- Clicking an instruction highlights its line.
- A `JSR` operand is a link to its function.
- The status bar shows the function, the offset, its size in words, and
  how much of the function has rows.

Opening:

- Files' "Open with Code Explorer" on any `.c4r`, `.c4o` or `.c4s`;
- `explore FILE` at a shell.

Any program on `/bin` can be opened, the desktop and the IDE included,
so C4IX can be read in itself.
Sources are found by the rule in 2.2. If one is missing or has changed,
the source pane says so and the code pane still works. Only the open
function is disassembled, and the rows are binary-searched. Nothing is
proportional to the image except loading it.

**E2, live mode**, the part that is Godbolt proper. There is a scratch C
buffer on the left. When typing pauses, the explorer compiles it with
`cc -c -g` into `/tmp` and reloads it. Toggles for `-O`, `-mfuse` and
`-minline` show what each one does. A second pane can hold the same
source with other flags, to compare. It needs c4sc's `-g` (S3). A small
file takes seconds at the page's 100 MHz.

**E3, later.** Maybe:

- a heat column from the profiler (S7);
- a diff of two images of one source.

**E4, outside the VM** (wanted; when is not decided). There are two ways, and the
first leads to the second:

- **A static export.** `c4rdump -S --html DIR` writes a page per unit:
  - the source with its colour bands, beside the code;
  - links both ways, and calls linked to their functions;
  - a little inline JavaScript for the highlighting.

  It is cheap once E0 exists, and it lets the repository's own builds,
  wolf3d's included, be read in any browser with nothing running.
- **A Godbolt page.** `libjs/web/explore.html` is a page of its own
  beside the C4IX page. It reads C4R in JavaScript: E0's reader ported
  beside `src/c4bb/sim/loader.js`, which already parses the format. It
  shows the three panes in the browser's own widgets, and fetches
  sources from the server that already serves the disk. For live mode
  it runs `c4sc` in a headless libjs machine in a worker: C4IX's
  compiler, with no desktop and no boot. Typing C on the left would
  show C4 code on the right, as Godbolt does, in a page anyone can
  open.

## 4. Windowed programs (track W)

### 4.1 The shape

The desktop becomes a **window server**. A windowed program is a
**client**, a task of its own that the desktop spawns. Each client has
two pipes, one for requests and one for events, as each Terminal has
for its shell.

- **Retained display lists.** A client draws a frame as a list of
  operations, then presents it. The desktop keeps each window's last
  presented list and replays it whenever it has to redraw that window:
  a move, a raise, damage from another window. There is no round trip
  to the client, so a slow or hung client never leaves a hole in the
  screen. This is the Terminal canvas's model (`gcur` and `gshow`), with
  lists that grow instead of a fixed 6,000 words.
- **Binary, word-aligned messages.** Each message is a header word,
  opcode and length, then argument words, then strings packed into
  words. This replaces the canvas's ASCII decimal: the desktop parses
  each canvas byte by byte today, and words are cheaper in a word VM.
- **The drawing operations are the `d_*` layer's:**
  - rectangle, outline, line, disc;
  - text with font and advance, and clipping;
  - image definition and image (as the wallpaper's tiles);
  - an 8-bit framebuffer by address (`win_fb`, kept for games).
- **Window operations:**
  - open (title, size, resizable or not), which returns a window id;
  - close, title, a size request and the pointer shape;
  - **menus**: the client declares its menu bar's titles and items, the
    desktop draws them as it draws its own, and a choice comes back as
    an event;
  - **dialogs**: a message box or an input box, drawn by wm.c, whose
    answer comes back as an event;
  - **launch**: open a Terminal running a command, or open a file with
    the program registered for it.
- **Events:**
  - mouse down, up, move and double click, and the wheel;
  - key down and up with modifiers;
  - resize, focus, a close request (the client may ask "save
    changes?"), a menu command, a dialog's answer;
  - **open**: arguments for a program that is already running (below);
  - frame drawn, the flow control Wolfenstein already uses;
  - **theme**: the scheme's colours and the look, sent at open and on
    every change. The client's widgets then draw in the desktop's look,
    and Settings changes every window at once.
- **One client, several windows.** Every operation names its window.

The client side is a `-fpic` library, `/lib/libdesk.c4s`, in three
parts:

- the protocol;
- a `d_*` layer that encodes operations instead of drawing;
- **ui.c, compiled a second time**.

ui.c depends only on `d_*`, the colours, `ui_style` and the fonts. So
the same widget code draws inside the desktop and inside every client,
and the two cannot drift. `desk_wait(ms)` is `uevwait` on the events
pipe.

Choices behind this design:

- **Pipes, not shared memory.** Every task can read every other's
  memory today, so a client could leave its list in its own memory and
  pass the address. That is quicker, but the desktop would read a list
  the client may be rewriting, and the address dangles when the client
  exits. It would also stop working the day C4IX protects memory. Pipes
  are what C4IX's fds were built for. The framebuffer stays by address,
  for speed, with the flow control it has.
- **The protocol on fds 3 and 4, not stdin and stdout** (decided). A
  client's `printf` must not corrupt its window. The desktop dup2s the pipe ends
  onto 3 and 4 when it spawns the client, as it already does for 0-2,
  and sets `C4IX_DESK=3,4` in the client's environment. stdout and
  stderr go to `/log/NAME.log`, a file per program that is cut back to
  its last 64 KB when it is opened. System Monitor's Programs tab can
  show it.
- **A client cannot break the desktop by being slow.** The desktop never
  blocks on a client. Writes to the events pipe do not block. To stop a
  client that never reads from growing its pipe without end, mouse
  moves are coalesced, and events stop being queued past 64 KB unread.
  A client that has not acknowledged a frame or a ping for 5 machine
  seconds is shown as "(not responding)" in its title and in System
  Monitor's Programs tab, which offers Kill. A client that exits or
  dies closes its pipes, and the desktop closes its windows and reaps
  it.
- **What it does not protect against.** Without memory protection, a
  client's stray write can still corrupt the desktop. Separate tasks
  give crash isolation for traps (an illegal opcode or a stack overrun
  kills only the client), not for wild pointers.
- **Descriptors.** Each client costs the desktop 2 fds, on top of each
  Terminal's 2 and its own 3. With `FD_MAX` 16 that is about six
  windows' worth. W1 raises `FD_MAX` to 64. The task struct grows by
  the fd and cloexec words, and `x5-c4m.txt` is re-pinned.

**How programs start.**

- **From the desktop.** Go menu entries and Files' "Open with" come from
  `/etc/desktop.apps`. Each line is:
  - a name and a program;
  - the file suffixes it opens, and an icon;
  - `single` if the program keeps one process for all its windows.

  The desktop spawns the program as a client. For a `single` program
  that is already running, it does not spawn a second one. It sends the
  running one an **open** event with the arguments, and that program
  opens another window. Installing a program means adding its line, so
  an optional one, such as the game (track G), appears in the Go menu
  only when it is there.
- **From a shell.** `start PROG ARGS` writes one canvas escape to its
  Terminal (`ESC _ G x;PROG ARGS ESC \`), and the desktop spawns PROG
  as a client, a child of the desktop rather than of the shell.
  `libdesk` does the same itself when a program finds no `C4IX_DESK`.
  So `ide hello.c`, typed in a Terminal, opens the IDE in a window of
  its own.
- A kernel rendezvous (a named service that any task can connect to) is
  not needed for either path. It can come later if something needs it.

### 4.2 The IDE as a program

`edit.c` moves to `src/c4ix/user/ide/` and becomes `/bin/ide`, linked
against libdesk:

- **Drawing:** its `d_*` calls and widgets are unchanged, now libdesk's.
- **Desktop API:** `win_open`, `win_menus`, `win_title` and `win_close`
  become protocol requests.
- **Dialogs:** `msgbox` and `inputbox` are dialog requests, and
  `edit_answer` becomes the handler for their answers.
- **`:run`** sends a launch request for a Terminal.
- **State:** `struct edit` becomes ordinary allocations instead of
  `w->st`. That also retires the review's B9 for the IDE (its 72 KB
  fixed state).
- **Opening files:** Files, the IDE icon and Go > Editor launch `/bin/ide
  FILE`.

**One IDE, many windows** (decided). The IDE is `single` in
`/etc/desktop.apps`. The first launch starts it, and every later one
reaches it as an open event, which opens a window on that file. If the
file is already open, that window is raised instead.
- Each window is a `struct edit` with its own text, undo, file pane and
  build. The process holds a list of them, as the desktop holds its
  windows.
- What the windows can now share comes for free: the yank register,
  the `:` and `/` histories, and the build state. A second `:make` in
  the same tree waits for the first instead of racing it.
- **The cost.** One crash or illegal opcode in the IDE closes every IDE
  window, not one. Unsaved text is the risk. The IDE therefore writes
  each modified buffer to `/tmp/ide/NAME.swp` every 30 machine seconds,
  and offers to recover it at the next open, as vim does.
- Each running build costs the IDE 2 descriptors, which `FD_MAX` 64
  allows for.

What the desktop gains:

- it is about 225 KB smaller;
- it needs a smaller c4sc stack to build;
- a crash in the IDE no longer takes the desktop with it;
- the desktop's build no longer compiles `edit.c`, which needs a 4 MB
  c4sc stack.

System Monitor, Files, the Terminal, Settings and the Text Editor stay
inside the desktop.

## 5. Wolfenstein 3D as a repository of its own (track G)

Today the port lives in `games/wolf3d/`. It arrived in c4's history as a
few large commits (`0125961` and the four after it): there is no record
there of what was changed in Wolf4SDL and why. The user wants it moved
out:

- it becomes a repository of its own, whose history *is* the port;
- c4 stops carrying it;
- a user who wants it fetches it, and then the game and its source are
  in the C4IX image, ready to play and to explore.

**Where the source came from** (checked 2026-10-22 against both hosts):
- The port is Wolf4SDL at KS-Presto's GitHub commit `dc8b250`
  ("Farewell GitHub", 2024-05-20). That commit adds nine lines to
  `README.TXT` and nothing else. Its parent is `10186e4` ("Fix music
  playing too fast", 2024-04-11).
- The project carries on at **bitbucket.org/ks-presto/wolf4sdl**.
  `dc8b250` is not in that history, but `10186e4` is. Bitbucket's
  `master` (`62cb942`, 2026-07-22) is 27 commits past it, and they
  include large clean-ups: VH and VL merged, `id_vl` renamed, map and
  window code reworked and partly undone.

**The new repository, `~/git/c4-wolf3d`**, carries the Bitbucket
repository's whole history, but its home is GitHub:
1. **Clone it from Bitbucket, then point it at GitHub.** Bitbucket stays
   as `upstream`, only to fetch from. `origin` is the user's new and
   empty `git@github.com:andrakis/c4-Wolf3D.git`, which helios can
   reach (checked 2026-10-22). There is no fork on Bitbucket. Nothing
   is pushed until the user says so.
2. **A branch `c4ix` from `10186e4`**, the commit the port really
   starts from. Its first commit is `dc8b250`'s README lines, so that
   every file the port did not touch is byte-identical to what c4 has.
3. **Then the port, one change to a commit, in the order it makes
   sense.** `games/wolf3d/` is diffed against that commit, and the hunks
   are sorted by the list in its README, each one marked `c4sdl:` in the
   source:
   - what C4IX does not build is removed (the MAME OPL emulator, the
     SDL-specific files the subset replaces);
   - the SDL subset (`sdl/`) and `c4sdl.c` over `plat.h`;
   - `plat-host.c`, so the game builds with gcc at every step after
     this;
   - `plat-c4ix.c`, the window and `win_fb`;
   - whole-file I/O (`c4file.c`);
   - the 8-bit 320x200 output;
   - sound through libjs's card;
   - the `itoa`/`ltoa` bug, which may be worth offering upstream;
   - the speed changes;
   - `--timedemo`;
   - the first-run installer (`wolfinst.c`, `explode.c`, `arc.c`) and
     the shareware zip's SHA-256. The zip itself is not committed (see
     below).

   Each commit says what it changes and why, taken from
   docs/c4ix-wolf3d.md, which moves there too. The game must build and
   pass its host timedemo at every commit where it can.
4. **Moving up to Bitbucket's `master` is not part of this plan.** It is
   possible later as a rebase of `c4ix`, but those 27 commits rework the
   very files the port changes, and they would need a fresh test of
   c4lc on the result.
5. **Its build is a makefile fragment, `c4.mk`**, holding the rules that
   are in c4's Makefile today (`WOLF_UNITS`, `WOLF_CC`, `wolf3d-objs`,
   `wolf3d-host`, `test-wolf3d`). It runs c4's toolchain from the
   directory it is given.

**In c4:**
- `make wolf3d-fetch` does two things (decided), into `ext/`, which git
  ignores:
  - it clones the game's repository into `ext/wolf3d`, from
    `WOLF3D_REPO`. That defaults to the copy beside c4 (`../c4-wolf3d`)
    when there is one, and otherwise to the GitHub repository, over
    https so that no key is needed;
  - it **downloads the shareware data**, `1wolf14.zip`, into
    `ext/wolf3d-data/`, and refuses it unless its SHA-256 is
    `cb2a2ef7ecef14152c65ff93cc3b84fbd3e8eb0c5c1de41a6fc8cdef559451a8`.

    It tries a short list of mirrors in order. 3D Realms' FTP, the
    original, is gone. G2 finds and records mirrors that still serve
    the exact file: the RPM Fusion source package it came from, and
    archive copies. A file the user already has can be given with
    `WOLF3D_ZIP=path`.
- The Makefile includes `ext/wolf3d/c4.mk` when it is there. Without it,
  `test-wolf3d` says it was skipped, and passes.
- When the game is there, build-images.sh does three things:
  - links it as today, and puts the downloaded zip on the disk;
  - copies its sources to `/usr/games/wolf3d`;
  - adds its line to `/etc/desktop.apps`.

  Lazy disk files cost nothing until they are opened. Without the game,
  there is no menu entry and no `/usr/games/wolf3d`.
- The desktop's hard-coded Go > Wolfenstein 3D (wm.c) goes; the apps
  file replaces it (W2).
- test-desktop.mjs's wolf3d checks run only when the game is on the
  disk.
- `games/wolf3d/` is removed in one commit that names the new repository,
  and the zip leaves c4 with it. docs/c4ix-wolf3d.md becomes a short
  pointer.
- **The old worktree `~/git/c4-wolf` goes** (decided, G0). It is a
  worktree of `~/git/c4`, not a clone, on branch `c4lc-full`. Checked on
  2026-10-22:
  - its status is clean and there is no stash;
  - `c4lc-full` (`178bb38`) and every other branch it shows are
    ancestors of `c4ix-userland`;
  - its 169 ignored files are build output: binaries, images, objects
    and c4sc's generated C.

  So `git worktree remove --force ~/git/c4-wolf` and `git branch -d
  c4lc-full` lose nothing. They are checked again just before they run.

The game is c4lc's largest program, and the explorer's best example.
Two things depend on it today, and both become conditional:
`c4bb-images` (Makefile:2341, through `wolf3d-objs`) and
compiler-speed.md's figures. Every target that uses the game says it was
skipped when the game is absent.

## 6. Milestones

Each milestone ends green on the targets named, plus `make test`,
`test-c4ix` and `test-libjs`, which every change here touches. New
targets are marked *new*.

### Track S: source maps

| | what | tests |
|---|---|---|
| **S0** | **The trailer, with no compiler emitting it.** `X` chunks in load-c4r.c (`C4ROPT_SRCMAP`), c4r.lisp (decode, encode, keep or drop by tag), c4rdump (`-l`, and skipping), and c4rstrip. `LOADCOMPLETE` checks in c4rlink and c4rdump. Loaders' bounds. A hand-made SMAP fixture. | *new* `test-srcmap`: the fixture loads in every loader (load-c4r.c, C4IX, libjs, c4bb, oisc4, c4mp, c4l, dosload) and runs the same; `c4r-roundtrip` is exact on it; c4rstrip removes it. `test-c4sp`, `test-c4lc`, `test-c4th` (roundtrip legs), `test-c4rstrip`, `test-link`. |
| **S1** | **c4lc `-g`.** Fix the backslash-newline count. Locations with files, `#line` and gcc markers. `Line` nodes, line items, rows and function table from `c4r:encode`. Errors that name their file. | `test-srcmap` gains: the invariant (every `src/tests/*.c` and the C4IX modules, `-g` against `-g0`, equal after c4rstrip); the CRC's cost on wolf3d's largest unit; a golden `c4rdump -l` of a probe with includes, a macro, an inlined call and a `for`; the map's size on wolf3d recorded here. `test-c4lc` (AST golden unchanged), `test-c4sp-opt`. |
| **S2** | **c4rlink merges, c4opt-run carries.** Rebased rows and functions, file union, line-0 rows for unmapped modules, `-s`. C4IX's loader trims cached library buffers. | `test-srcmap`: a linked map equals the concatenation of its objects' maps, rebased; `c4opt-run` on a mapped image keeps every row on an instruction of its line. `test-link`, `test-respfile`, `test-b4ke` (its 1898-byte pin stays: `-g` is not the default yet). |
| **S3** | **c4sc and cc.** Regenerate, add `-g` to body.h, `cc -g`. | `test-c4sc-back` and `test-c4sc-run` also under `-g` (c4sc-host = c4lc, trailer included). `test-c4ix-rebuild` with a `-g` program in its list. |
| **S4** | **c4fc `-g`.** Rows from `(STATEMENT)`, line items in opt.f, the same rules. | `test-c4fc`: the C4IX leg (F10, both compilers preprocess themselves) also with `-g`, `cmp` of whole files trailer and all. The `gcc -E` legs (c4th.c, C4KE) compare code only, since the line maps of a `.i` file and of the source differ, unless S1's marker support makes them equal, which this milestone checks. |
| **S5** | **c4cc `-g`.** | `test-srcmap`: c4cc's map of a probe (one-file and `gcc -E` input) checked against the source; `test-c4sp-opt` (the `-S` listing format is pinned). |
| **S6** | **Maps on by default**, the user's goal: in all four compilers at once, kernels and libraries included. `-g0` where a test compares against a reference image made without a map; pins that measure images re-pinned (test-b4ke's 1,898 bytes, the size tables in c4ix-design.md). Measure what it costs. | `make test` and every suite S0-S5 named, now with the default; `test-libjs` (the whole desktop suite on mapped images); `test-c4ix-rebuild`; `test-wolf3d` when the game is fetched; C4SK's own build still yields images with nothing after S. Recorded: the root archive's size, compressed, before and after; each image's map size; the heap after boot. |
| **S7** | **Readers of maps.** Stack traces in load-c4r.c and libjs, C4IX's kill message, the profiler by line. | `test-srcmap`: a deliberate illegal opcode reports `file:line function`; `prof-c4ix` output by line for wolf3d. |

### Track W: windowed programs

| | what | tests |
|---|---|---|
| **W0** | **Groundwork in the desktop** (desktop review M1, B9, D1): one `struct tool` table instead of the if-ladders; per-tool state sized by `sizeof`; one `spawn_piped`, with its checks. No visible change. | `test-desktop.mjs` (its 164 checks), `test-c4ix-rebuild` (`desktop` byte-identical with itself after rebuilding inside). |
| **W1** | **The server and libdesk.** `K_APP` windows; the protocol (4.1); `/lib/libdesk.c4s` with ui.c in it; `FD_MAX` 64; logs for clients' stdout. A demo client: bounce.c ported, with a menu and a dialog. | *new* `test-desk-client.mjs` (in `test-libjs`): the demo opens two windows, draws, gets clicks and keys, uses a menu and a dialog, resizes, follows a theme change; a client that stops reading is shown "(not responding)" and killed from System Monitor; one that exits loses its windows; no zombies. `test-c4ix`, `test-c4ix-c4ke` (x5 re-pinned for the task struct). |
| **W2** | **Starting programs.** `/etc/desktop.apps` with `single`, the open event, Go entries, Files' "Open with", `start` and libdesk's self-start. | `test-desk-client.mjs`: from the Go menu, from Files, and `start` and `bounce` typed in a Terminal; a `single` demo launched twice is one task with two windows. |
| **W3** | **The IDE as `/bin/ide`**, one process with many windows, and swap files. | `test-desktop.mjs`'s IDE checks (VIM operations, `:make`, errors, the file pane, colours, Cancel) unchanged in what they check, now against the client. New ones: two files opened from Files are two windows of one `ide` task in System Monitor; opening an open file raises its window; text yanked in one window pastes in the other; an IDE killed with a modified buffer offers to recover it at the next open. `test-c4ix-rebuild` builds `/bin/ide`. Recorded: the desktop's image size before and after. |

### Track G: Wolfenstein 3D as its own repository

| | what | tests |
|---|---|---|
| **G0** | **Retire `~/git/c4-wolf`.** Check again that it is clean and merged, then remove the worktree and the merged branch `c4lc-full`. | `git worktree list` no longer shows it; `git branch --contains 178bb38` still shows `c4ix-userland`. |
| **G1** | **The repository**, `~/git/c4-wolf3d`: a clone of Bitbucket's wolf4sdl (`upstream`) with `origin` on GitHub, branch `c4ix` from `10186e4`, then `dc8b250`'s README lines, then the port as a series of commits, `c4.mk` and the doc. Never pushed. | At the last commit, the new tree equals `games/wolf3d/` file for file, except the zip. The host build and `--timedemo 1` pass at every commit from the platform layer on (a script walks the history). |
| **G2** | **c4 uses it when it is there.** `make wolf3d-fetch` (the clone, and the zip from a mirror, checked by SHA-256), `ext/wolf3d`, the Makefile including `c4.mk`, build-images.sh's copy to `/usr/games/wolf3d` and its apps line. Needs W2 for the apps file; until then wm.c's entry is shown only if the game is on the disk. | `test-wolf3d` and test-desktop.mjs's game checks, with the game fetched, give the same results as before; with `ext/wolf3d` absent, `make test-libjs` passes and says the game's checks were skipped; a zip with one byte changed is refused. |
| **G3** | **c4 stops carrying it.** Remove `games/wolf3d/`; docs/c4ix-wolf3d.md becomes a pointer. | As G2, both ways, on a fresh clone of c4. |

### Track E: the explorer

| | what | tests |
|---|---|---|
| **E0** | **The library and c4rdump `-S`.** One opcode table, symbolic operands, lookups both ways. Needs S1. | `test-srcmap`: a golden `c4rdump -S` of the probe; the opcode table checked against c4m.c; `c4rdump -S` of wolf3d (when fetched) gives every function rows, and the share of code words with a line is recorded. |
| **E1** | **Code Explorer's window.** Needs W1 and S2; wolf3d's sources in `/usr/src` come with G2. | *new* `test-explorer.mjs` (in `test-libjs`): open `/bin/hello`; click a source line, and its instructions are the ones `c4rdump -S` gives; click a `JSR` and it goes to the callee; open wolf3d (when fetched; the desktop otherwise) and find `ScalePost` by the filter in under N machine-seconds. Then a look in the browser. |
| **E2** | **Live mode.** Needs S3. | `test-explorer.mjs`: type a function, see its code; `-O` on and off changes it, as `cc` does by hand. |
| **E4** | **Outside the VM** (wanted, not yet scheduled): the static export, then the Godbolt page. | *new* `test-explore-web.mjs`: the export of a probe has the rows `c4rdump -l` gives; the page, driven headless, compiles a typed function in its worker and shows the same code as `cc`. |

**Order.** Three tracks do not depend on each other and can run side by
side: S0-S2 (compilers and formats), W0-W1 (the desktop), and G0-G1
(retiring the old worktree, and a new repository that touches nothing
in c4).
- E0 follows S1.
- W3 follows W1, and G2 follows W2.
- E1 needs S2 and W1, and is best with G2, so that it has the game to
  open.
- S4 (c4fc) can wait until c4lc's rules have settled.
- S6, the default, comes once S5 is done and the user has seen what S1
  measured.

## 7. Risks

- **c4lc's passes and line items.** This is the largest risk. The
  invariant test finds a pass that does not step over line items, but
  each such pass needs care, and fusion's windows are the most
  delicate. Fallback: under `-g`, line items are removed before the
  peephole passes and put back after them, each anchored to the
  instruction that followed it, matched by `eq`. The fallback is
  coarser: merged instructions lose their own lines.
- **c4sc's memory in the VM.** It grows with a `Line` node for every
  statement and a line item for every row. The 4 MB stack the IDE's
  compiles get, and C4DOS's 200,000-cell budget, are measured under
  `-g` in S3.
- **c4fc parity.** Line items must survive opt.f exactly as they survive
  c4opt. This is why S4 comes after the rules are proved in c4lc.
- **Default-on maps cost memory everywhere a whole file is held.**
  C4KE's RAM filesystem holds every image whole, and C4IX's files are
  RAM. Archive size is measured in S1 and S6 before the default flips.
  If it is too much, the fallback is to keep maps on for programs and
  libraries, and off (`-g0`) for the images the boot needs before
  `/usr` exists.
- **One IDE process holds every IDE window.** A crash closes them all.
  The swap files (4.2) bound the loss to 30 machine seconds of typing.
- **The window protocol's cost.** The cost is replaying lists and
  parsing messages in the desktop task. The explorer's busiest frame is
  about 300 text operations. W1 measures cycles per frame against the
  Terminal canvas.

## 8. Questions

All answered on 2026-10-22, and written into the sections above:

| | question | answer |
|---|---|---|
| Q1 | bump the version byte? | no: the trailer announces itself (2.1) |
| Q2 | maps by default? | on everywhere, kernels and libraries too; C4SK strips (2.3, 2.4, S6) |
| Q3 | wolf3d's source in C4IX? | the game becomes an optional repository; when fetched, its source goes in `/usr/games/wolf3d` (5, track G) |
| Q4 | a CRC per source file? | yes (2.2) |
| Q5 | protocol on fds 3 and 4? | yes (4.1) |
| Q6 | IDE processes? | one IDE, many windows (4.2) |
| Q7 | an explorer outside the VM? | yes, at some point; not scheduled (E4) |
| Q8 | the name? | "Code Explorer", `/bin/explore`, for now |
| Q9 | the game's repository? | `~/git/c4-wolf3d`, cloned from bitbucket.org/ks-presto/wolf4sdl, its remote `github.com:andrakis/c4-Wolf3D`; `~/git/c4-wolf` removed (5, G0, G1) |
| Q10 | the shareware zip? | downloaded by `make wolf3d-fetch` and checked by SHA-256 (5, G2) |

Left for when the work starts:
- **Pushing `c4ix` to GitHub** waits for the user's word, as every push
  does. Whether `c4ix` is the GitHub repository's default branch is
  decided then.
- **The mirrors for the zip** are found and recorded in G2. Each must
  serve the exact file, by its SHA-256.

## Appendix A. Every reader and writer of C4R

At `7b166ea`. "v>=3" means no version is refused, and 3 or more reads
MEMSZ. "Trailer" says what bytes after S would do.

| program | reads/writes | version check | trailer | uses symbols for |
|---|---|---|---|---|
| load-c4r.c (inside C4KE, c4rlink, c4rdump, c4cc) | R | 0, 2, 3 only (:395-401) | ignored; streams, stops after `d` or S | stack traces (:887) |
| C4KE (src/c4ke/c4ke.c) | R, through load-c4r.c | as load-c4r.c | ignored; its RAM filesystem holds the whole file | stack traces |
| c4l.c | R | none, and ignores MEMSZ | ignored; 8 MB buffer, unbounded read (:147-150) | - |
| src/c4mp/loader.c | R | v>=3 | ignored; 4 MB buffer, unbounded read (:39-49) | - |
| src/c4ix/loader.c | R | v>=3 (:213) | ignored; whole file, 4 MB cap; **a cached `-fpic` library's buffer is kept for ever** (:547-562) | linking: `__c4_dbase`, `__c4_needed`, exports |
| src/c4bb/sim/loader.js | R | v>=3; 32-bit only | ignored | - |
| libjs/boot.js `parseSymbols` | R | none | ignored (bounded loop) | STRC stack traces (c4m.js:520-540) |
| libjs/tests/prof-c4ix.mjs | R | through the above | - | the profiler, per function |
| src/c4bb/tools (opscan, lockstep, memcensus) | R | through loader.js | - | - |
| src/c4bb/fw/fw.c | R | v>=3 | ignored; whole file, never freed | - |
| src/c4dos/c4dos.c (RUN) | R | v>=3 | ignored; 4 MB scratch, unbounded read (:539) | `__c4dos_api` |
| src/c4dos/dosload.c | R | v>=3 | ignored; freed after parsing | `__c4dos_api` |
| src/oisc4/oisc4.c | R | 0, 2, 3 only (:707); 64-bit only | ignored; 16 MB cap | - |
| src/c4sp/lisp/c4r.lisp | R+W | v>=3; writes 3 | **dropped on encode** | labels |
| c4r-roundtrip.lisp | R+W | - | **fails** on a trailer | - |
| c4opt-run.lisp, c4lc, c4lc-eq, c4sc's `c4c4r_gen.c` | through c4r.lisp | | dropped | |
| src/c4fc/emit.f | W | writes 3 | - | - |
| src/c4th/forth/c4r.f, self.f | W | writes 2 | - | - |
| src/c4cc/asm-c4r.c | W | writes 2, no MEMSZ | - | - |
| src/c4ke/bin/c4rlink.c, c4rstrip | R+W | through load-c4r.c; **no `LOADCOMPLETE` check** (:712-746) | dropped | linking |
| src/c4ke/bin/c4rdump.c | R | through load-c4r.c; would crash on v4 (:46-51) | ignored | `-s`, "Needs" |
| src/c4ix/vfs.c:332 | sniffs the signature | - | - | - |
| src/c4lm/load-c4r.c | R | 0, 2 only | on hold, no targets | - |
