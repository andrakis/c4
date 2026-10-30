# One copy of c4lc's helpers per image

c4rlink folds the helpers that c4lc puts into every object, so a linked
image has one copy of each where it had one per object. The change is in
c4rlink only. Objects are the same bytes as before, and `--no-fold`
links the images exactly as before.

## The copies

For what the VM has no instruction for, c4lc emits a call to a routine
and splices the routine into the unit, as code with no symbol:

- the M4 helpers `<udiv>`, `<umod>`, `<ld16s>`, `<ld16u>`, `<st16>` and
  `<lsr>` (`g:HELPERS`, c4lc-gen.lisp);
- the soft-float `__c4d_*`, `__c4f_*` and `__c4fp_*` routines
  (`p:fpsrc`, made from src/c4fp/softfp.c by src/c4fp/mkfpsrc.py);
- the `long long` `__c4ll_*` routines (`p:llsrc`).

Each unit that uses one gets its own copy, and the linker kept them all.
`c4ix-wolf3d6.c4r` had 18 copies of `<ld16s>`, 20 of `<ld16u>`, 19 of
`<st16>`, 10 of `<udiv>` and 5 of each `__c4d_to_*`. In all, 290 helper
copies past the first came to 35,661 words, a fifth of its code. The
IDE's Code tab lists each copy with its unit. libjs/c4r.js's
`readC4r(...).smap.funcs`, grouped by name, counts them.

## The linker, and what must match what

There is one linker, src/c4ke/bin/c4rlink.c. It is built five ways, and
every way runs the same C:

| Build | Made by | Used by |
|---|---|---|
| `./c4rlink` | gcc | the 64-bit tests (test-link, test-srcmap) |
| `c4rlink32` | gcc -m32 | build-images.sh, the game images, c4sc32, libc-bare links |
| `c4rlink.c4r` | c4cc | the 64-bit VM: B4KE under C4KE (test-b4ke, test-srcmap) |
| `c4rlink32.c4r` | c4cc32 | C4IX's disk (`cc` links with it), the C4DOS floppies |
| `c4rstrip`, `c4rstrip32` | the same file | strip only: gc and so fold are off |

c4r.lisp is not a linker. It decodes and encodes images for c4opt and
the round-trip tests. libjs/c4r.js only reads them. So no second
linker had to learn this.

The byte-identity gates, and what folding does to each:

- **Objects** are untouched, since c4lc, c4fc and c4sc are unchanged.
  So test-c4fc (c4fc's objects equal c4lc's), test-c4sc-back and the
  corpus diffs see nothing new.
- **test-c4ix-rebuild** compares images C4IX rebuilds with the host's.
  C4IX links with `c4rlink32.c4r` and the host with `c4rlink32`. Both are
  this file, so both fold the same way. fold-link.sh checks the same
  for `c4rlink.c4r` in the 64-bit VM.
- **Pins.** test-b4ke's `wrote 1898 bytes` is c4cc code, which has no
  helpers, so it is unchanged. test-desktop's `ls:` line matches a
  pattern, not a count.
- **-g or not.** An image must be the same code whether its objects
  were compiled with `-g` or not, because C4IX's own build compiles
  without. Folding never reads the source map.
- **`--no-fold`** links each image in the table below to the bytes of
  the image that was shipped before.

## How it folds

It goes in gc (c4rlink.c, `gc_fold`, called by `gc_functions`), after gc
has cut the code into regions and before it works out what is live.

- **What may fold.** A region with no defined function symbol in it.
  The regions are gc's: a region starts at each function symbol, each
  input's first word, and each `ENT` after a jump or return. A region
  that runs on into the next is glued to it. A region that has no
  symbol is code that no C name reaches: c4lc's helpers, its exit-255
  stubs, its `__c4lc_init`. Nothing can tell two copies of such code
  apart, so keeping one is safe. Static functions are never folded,
  even when identical, because each has a symbol and C gives distinct
  functions distinct addresses.
- **When two are the same.** The code words must match, with the words
  that patches fill blanked to 0, as `blank_patch_targets` blanks them
  anyway. The code-resident patches must match too: the same type at
  the same place in the region. A CODE patch into the region itself
  must be at the same place in it. Any other patch must have the same
  value: the same callee, the same data offset, or the same symbol. A
  copy that touches its own unit's data, as a DATA patch, folds only
  with a copy that touches the same data, which in practice means none.
- **Moving the callers.** For each candidate, in code order, c4rlink
  looks for an earlier one that is the same. It uses a hash of length
  and code words, with each candidate's chain leading to earlier ones.
  Every code address into a folded copy then moves to the same place
  in the copy kept: CODE and DCODE patch values, the entry point,
  constructors and destructors. Nothing reaches the folded copy any
  more, so gc drops it with its patches. Everything after it moves
  down.
- **Rounds.** A copy that calls its own unit's copy of another helper
  matches the next unit's only after those callees have folded. So the
  pass repeats until nothing folds: wolf3d takes 8 rounds, libc 6.
  Helpers that called each other in a cycle would never fold this way,
  but no such copies are left: every copy left after folding is
  `__c4fp_hw` or a wrapper of it.
- **The source map.** `gc_srcmap` already writes the map of what gc
  keeps. The kept copy keeps its function entry and rows. The dropped
  copies' entries and rows go, and the rows after them move down with
  the code. test-srcmap's new `fold-link.sh` checks that every
  function and row lies in the code.
- **`-r` and `-fpic`.** A `-r` library folds its own copies; a later
  link folds them again with the program's. A `-fpic` object reaches
  data as `IMM off; PSH; IMM __c4_dbase; LI; ADD`. The `__c4_dbase`
  reference is a symbol patch, which is the same master symbol in every
  unit, and the offset is a DATA patch compared by value, so pic
  helpers fold like any others (libc.c4s below).
- **`-l`.** A program linked against a shared library keeps its own
  helpers: the library's copies have no names, so the program cannot
  reach them.

`-v` reports, for example, `fold: 251 copies of 34103 words folded, of
368 regions with no symbol, in 8 rounds` (wolf3d). That count includes
copies that nothing reached, which gc would have dropped anyway.

## What it saves

Code is in words, 4 bytes each in these 32-bit images. The file size
also drops by each dropped copy's patches (12 bytes each) and its map
rows. "Before" is the image `--no-fold` links, the same bytes as the
image shipped from 9f81a64.

| Image | Code before | Code after | Saved | File before | File after | Saved |
|---|---:|---:|---:|---:|---:|---:|
| `c4ix-wolf3d.c4r` | 166,060 | 131,957 | 34,103 (20.5%) | 1,171,547 | 1,010,471 | 161,076 (13.7%) |
| `c4ix-wolf3d6.c4r` | 161,100 | 126,997 | 34,103 (21.2%) | 1,144,194 | 983,114 | 161,080 (14.1%) |
| `c4ix-doom.c4r` | 186,943 | 179,897 | 7,046 (3.8%) | 1,597,027 | 1,563,771 | 33,256 (2.1%) |
| `c4ix-rott.c4r` | 417,426 | 400,274 | 17,152 (4.1%) | 3,036,645 | 2,956,969 | 79,676 (2.6%) |
| `c4ix-tyrian.c4r` | 308,445 | 259,135 | 49,310 (16.0%) | 2,364,815 | 2,128,979 | 235,836 (10.0%) |
| `c4ix-desktop.c4r` | 225,839 | 216,753 | 9,086 (4.0%) | 1,268,138 | 1,227,066 | 41,072 (3.2%) |
| `c4ix-libc.c4s` | 100,780 | 93,499 | 7,281 (7.2%) | 493,595 | 460,763 | 32,832 (6.7%) |
| `c4ix.c4r` (kernel) | 70,095 | 68,750 | 1,345 (1.9%) | 458,596 | 452,740 | 5,856 (1.3%) |
| games/`wolf3d.c4r` | 211,372 | 176,037 | 35,335 (16.7%) | 1,389,977 | 1,223,469 | 166,508 (12.0%) |
| games/`wolf3d6.c4r` | 206,098 | 170,763 | 35,335 (17.1%) | 1,360,776 | 1,194,264 | 166,512 (12.2%) |
| games/`doom.c4r` | 219,768 | 212,435 | 7,333 (3.3%) | 1,763,906 | 1,729,390 | 34,516 (2.0%) |
| games/`rott.c4r` | 449,857 | 432,478 | 17,379 (3.9%) | 3,203,583 | 3,122,907 | 80,676 (2.5%) |
| games/`tyrian.c4r` | 370,646 | 314,055 | 56,591 (15.3%) | 2,665,001 | 2,396,333 | 268,668 (10.1%) |
| `c4rlink.c4r` | 64,557 | 67,758 | −3,201 (−5.0%) | 355,774 | 370,693 | −14,919 (−4.2%) |

The `c4ix-*` rows and the kernel are on C4IX's disk; the games/ rows are
the bare-machine images (src/c4bb/images/games). Over the 91 images on
C4IX's disk, files drop from 17,030,703 bytes to 16,294,938, 736 KB
(4.3%). Everything c4cc builds, which splices no helpers, is unchanged:
c4sc, c4dos32, c4ke32, `c4ix-libc4ix.c4s` and the other tools. The one
image that grows is `c4rlink.c4r`, by the fold code itself.

What is left is the FPU wrappers. `__c4fp_hw` keeps the FPU's answer in
two `static` locals, `asked` and `has`, so each unit's copy reaches its
own data and cannot fold. The same goes for each `NAME` that calls it
beside its `NAME_s` (`__c4d_mul` beside `__c4d_mul_s`, and so on).
wolf3d keeps 39 such copies (1,558 words), tyrian 69 (2,440), and libc
4. Folding those would need softfp.c to keep that answer somewhere that
is not per unit. That is a change to `p:fpsrc`, so c4lc, c4fc and c4sc
all change, and every object is rebuilt. That is not worth it for a few
KB per image.

## Is it worth it

Yes. The change is about 200 lines in one file. Compiler output does
not change, and neither does any test's expected bytes. It takes
161 KB (14%) off wolf3d, 236 KB (10%) off tyrian, 33 KB (7%) off libc
and 41 KB off the desktop. The code that runs is the same instructions,
so speed is unchanged. Images are smaller to load and to hold in memory, and the
explorer lists one copy of each helper instead of twenty.

## Tests

src/c4sp/tests/fold-link.sh, run by `make test-srcmap`, links
src/tests/fold_a.c and fold_b.c, which use every kind of helper. It
checks the following:

- Each link runs as gcc's build does.
- Each link has less code than `--no-fold`.
- No function is in the map twice, except `__c4fp_hw` and its callers.
- The map lies in the code.
- `-g0` objects link to the same image, cut at the map.
- A `-r` library and `-fpic` objects fold.
- 32 bits fold.
- `c4rlink.c4r` in the VM writes an image of the same size, which runs.
