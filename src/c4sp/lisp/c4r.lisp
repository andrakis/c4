;; c4r.lisp -- read and write C4 Relocatable images (M5, design 9.2)
;;
;; A .c4r's patch table identifies every address word in the code segment
;; together with its target, so the code decodes into labelled instruction
;; lists and encodes back byte-identically:
;;
;;   ((label 1) (ENT 0) (IMM (data 0 R)) ... (JSR (extern 4 R)) ...)
;;
;; Operand references are (code OFFSET RAW), (data OFFSET RAW) or
;; (extern SYMID RAW), where OFFSET/SYMID regenerate the patch entry and
;; RAW is the original in-file operand word -- dead weight the loader
;; overwrites, preserved so the round trip is byte-identical. Optimizer
;; passes emit references without RAW; the encoder then writes the target
;; offset (or 0), which loads identically.
;;
;; Label ids are the ORIGINAL word offsets of their targets, so no label
;; counter is needed and listings read like c4rdump output.
;;
;; A decoded module is a positional list:
;;   (version wordbits entry code data syms cons des dpatches)
;;     entry: -1 or (code N)
;;     code:  instruction list as above; (word W) holds a raw word that
;;            could not be an instruction (e.g. the dead zero word asm-c4r
;;            leaves at offset 0)
;;     data:  the data segment as a byte string
;;     syms:  (id type class attrs name value), value (code N) for defined
;;            functions
;;     cons/des: lists of (code N)
;;     dpatches: data-resident patches (type -3/-4): (dcode BYTEOFF LABEL)
;;               relocates a data word to a code address, (ddata BYTEOFF
;;               DATAOFF) to a data address. Written after the
;;               code-resident patches, matching asm-c4r's canonical order.
;;               In an object (M7b), (dsym BYTEOFF SYMID) is type -5: a data
;;               word holding a symbol's address, which c4rlink resolves.
;;     chunks:   the extension trailer's chunks, (TAG PAYLOAD) with TAG a
;;               four-letter string (docs/c4-explorer.md 2.1). Optional: a
;;               module with nine elements has none.
;;
;; Uses the c4sp byte builtins: string:word, string:byte, string:alloc,
;; string:word!, string:byte!, bit:and. All offsets in BYTES unless noted.

(begin

;; ---- small list library ----

(define reverse (lambda (l) (next reverse/2 l (list))))
(define reverse/2 (lambda (l acc)
	(if (empty? l) acc (next reverse/2 (tail l) (+ (list (head l)) acc)))))

;; (cons x l) is (+ (list x) l): shares l, O(1)
(define cons (lambda (x l) (+ (list x) l)))

(define second (lambda (l) (index l 1)))
(define third (lambda (l) (index l 2)))

;; ---- opcode table ----

(define c4r:ops '(LEA IMM JMP JSR BZ BNZ ENT ADJ LEV LI LC SI SC PSH
	OR XOR AND EQ NE LT GT LE GE SHL SHR ADD SUB MUL DIV MOD
	OPEN READ CLOS PRTF MALC FREE MSET MCMP EXIT
	PUTC PUTS RALC MCPY STRC ITH _OPC _BLT _TRP OPCD
	_JMP _ADJ C4CF C4CY TIME SIGH SIGI USLP INFO OPSL
	C4IV FLT JSRI JSRS JMPA TLEV DBG
	;; c4mp only: processor control. Appended, never inserted -- the
	;; list index IS the opcode number, and it is mirrored in c4m.c,
	;; load-c4r.c, oisc4.c and c4cc.c.
	CPUI CPUN CPUS CPUH CAS XCHG FADD CWAI CWAK IPI
	;; c4mp only, M12: fused array-element load/store, emitted by
	;; c4lc-gen.lisp only under -mcisc. Same rule: c4m does not know
	;; these numbers either, appended not inserted.
	LXI SXI
	;; raw-terminal-mode toggle: a c4mp host syscall (tcsetattr) that
	;; c4or1k uses to forward Ctrl+C to the guest. Appended, not inserted.
	TRAW
	;; The fused opcodes -- docs/fused-opcodes.md. Two- and three-
	;; instruction sequences a third of the instructions real workloads
	;; execute are made of, emitted only under -mfuse. Appended, never
	;; inserted.
	LDL LDG PSHL PSHG LEAP IMMP LIP ADDL STL POPA
	;; F3: IEEE arithmetic (docs/c4-float.md). Appended.
	FPU))
(define c4r:nops (length c4r:ops))

;; does opcode n take an operand word? LEA..ADJ are 0..7; JSRI 61 JSRS 62;
;; of the fused opcodes LDL..IMMP are 79..84 and STL is 87 -- LIP, ADDL
;; and POPA take none.
;; No `and` here: it lives in macros.lisp, and c4r.lisp is loaded by
;; programs that do not load that.
(define c4r:has-operand (lambda (n)
	(if (<= n 7) true
	(if (= n 61) true
	(if (= n 62) true
	(if (= n 87) true
	(if (< n 79) false
	    (<= n 84))))))))

(define c4r:opname (lambda (n) (index c4r:ops n)))
(define c4r:opnum (lambda (name) (next c4r:opnum/3 name c4r:ops 0)))
(define c4r:opnum/3 (lambda (name l n)
	(if (empty? l) -1
		(if (= name (head l)) n (next c4r:opnum/3 name (tail l) (+ n 1))))))

;; ---- byte cursor helpers over the file string ----

(define W (sys:wordsize))

;; ---- reading ----

;; Read the file at Path into a decoded module list, or print and return
;; nil on a malformed file.
(define c4r:read (lambda (Path) (c4r:decode (file:read (file:path Path)))))

(define c4r:decode (lambda (F) (begin
	(if (not (= 67 (string:byte F 0))) (error "not a C4R file"))  ;; C
	(if (not (= 52 (string:byte F 1))) (error "not a C4R file"))  ;; 4
	(if (not (= 82 (string:byte F 2))) (error "not a C4R file"))  ;; R
	(define Version (string:byte F 3))
	(define Wordbits (string:byte F 4))
	(if (not (= Wordbits (* 8 W))) (error "c4r: word size mismatch"))
	;; header words start after 3+1+1 bytes plus 8 padding bytes; the
	;; padding word (byte 5) is the data MEMSZ in v3, filler in v2
	(define H 13)
	(define Entry     (string:word F H))
	(define Codelen   (string:word F (+ H W)))
	(define Datalen   (string:word F (+ H (* 2 W))))
	;; MEMSZ: v3 reads the padding word; v2 has none (no BSS)
	(define Memsz (if (>= Version 3) (string:word F 5) Datalen))
	(if (< Memsz Datalen) (set! Memsz Datalen) nil)
	(define Patchlen  (string:word F (+ H (* 3 W))))
	(define Symslen   (string:word F (+ H (* 4 W))))
	(define Conslen   (string:word F (+ H (* 5 W))))
	(define Deslen    (string:word F (+ H (* 6 W))))
	;; segment positions: each is preceded by a one-word marker
	(define CodeAt (+ H (* 8 W)))                 ;; 7 header words + 'C'
	(define DataAt (+ CodeAt (* Codelen W) W))    ;; skip 'D'
	(define PatchAt (+ DataAt Datalen W))         ;; skip 'P'
	(define ConsAt (+ PatchAt (* Patchlen (* 3 W)) W))
	(define DesAt (+ ConsAt (* Conslen W) W))
	(define SymsAt (+ DesAt (* Deslen W) W))

	;; patches, in file (= address) order: (type address value)
	(define AllPatches (reverse (c4r:read-patches F PatchAt Patchlen (list))))
	(define Patches (c4r:code-patches AllPatches))
	(define DPatches (c4r:data-patches AllPatches))
	;; constructors/destructors: raw code offsets
	(define Cons (reverse (c4r:read-words F ConsAt Conslen (list))))
	(define Des (reverse (c4r:read-words F DesAt Deslen (list))))
	;; symbols: (id type class attrs name value)
	(define Syms (reverse (c4r:read-syms F SymsAt Symslen (list))))

	;; every label target: a byte map over code offsets
	(define Labels (string:alloc Codelen))
	(if (not (= -1 Entry)) (string:byte! Labels Entry 1))
	(c4r:mark-patch-labels AllPatches Labels)
	(c4r:mark-word-labels Cons Labels)
	(c4r:mark-word-labels Des Labels)
	(c4r:mark-sym-labels Syms Labels)

	;; decode the instruction stream
	(define Code (reverse (c4r:decode-code F CodeAt Codelen Labels Patches 0 (list))))

	;; the module's Data is always the FULL in-memory image (length =
	;; MEMSZ): the stored bytes, then a zeroed BSS tail. string:alloc
	;; zeroes, so copying the stored prefix in is enough. Keeping Data
	;; full-length means encode/roundtrip and c4rlink see it uniformly;
	;; encode re-derives MEMSZ as (length Data) and re-trims.
	(define DataFull (string:alloc Memsz))
	(c4r:copy-string (string:substr F DataAt Datalen) DataFull 0 0)

	(list Version Wordbits
		(if (= -1 Entry) -1 (list 'code Entry))
		Code
		DataFull
		(c4r:label-syms Syms)
		(c4r:label-offsets Cons)
		(c4r:label-offsets Des)
		DPatches
		(c4r:read-trailer F (+ SymsAt (c4r:syms-size Syms 0))))
)))

;; ---- the extension trailer (docs/c4-explorer.md 2.1) ----
;;
;; After S: a word 'X', then chunks of a tag word (four ASCII bytes, low
;; first), a length word and that many bytes padded to a word, up to a 0
;; tag. No trailer is no chunks.
(define c4r:read-trailer (lambda (F At)
	(if (> (+ At W) (length F)) (list)
	(if (not (= 88 (string:byte F At))) (list)
	(reverse (c4r:read-chunks F (+ At W) (list)))))))
(define c4r:read-chunks (lambda (F At Acc)
	(if (> (+ At W) (length F)) Acc
	(if (= 0 (string:word F At)) Acc
	(begin
		(define Len (string:word F (+ At W)))
		(next c4r:read-chunks F (+ At (* 2 W) (c4r:padded Len))
			(cons (list (string:substr F At 4) (string:substr F (+ At (* 2 W)) Len)) Acc)))))))
(define c4r:padded (lambda (N) (* W (/ (+ N (- W 1)) W))))

;; A tag's fourth letter says what a tool that changes the code may do
;; with the chunk: upper case, drop it (it describes the code as it was);
;; lower case, keep it. c4opt does this to an image it optimises.
(define c4r:code-safe (lambda (Chunks) (next c4r:code-safe/2 Chunks (list))))
(define c4r:code-safe/2 (lambda (Chunks Acc)
	(if (empty? Chunks) (reverse Acc)
	(begin
		(define C (string:byte (head (head Chunks)) 3))
		(next c4r:code-safe/2 (tail Chunks)
			(if (if (>= C 65) (<= C 90) false) Acc (cons (head Chunks) Acc)))))))

;; ---- SMAP, the source map (docs/c4-explorer.md 2.2) ----
;;
;; Made from the code's zero-size items: (line LOC) starts a row, (func
;; NAME LOC) a function. A location is file * 2^20 + line, the file
;; numbered as in c4r:srcfiles. Rows: where two start at one place the
;; later wins, a row that says what the one before said goes, and so
;; does one at the end of the code. A function runs to the next one, or
;; to a (func false 0) before it: the end of one that c4r:smap-carry read
;; out of a map, where code with no function follows it (a linked object
;; that had no map). Files: only those something points at, renumbered
;; in order.
(define c4r:LOCF 1048576)
(define c4r:smap (lambda (Code Codelen)
	(begin
		(define Got (c4r:smap-collect Code 0 (list) (list)))
		(define Rows (c4r:smap-dedup (c4r:smap-later (head Got) (list)) -1 (list)))
		(define Rows2 (c4r:smap-before Rows Codelen (list)))
		(define Funcs (c4r:smap-named (second Got)))
		(define NF (length c4r:srcfiles))
		;; file number -> new number + 1, 0 = unused
		(define Map (string:alloc (* W (+ NF 1))))
		(c4r:smap-mark Rows2 Map NF)
		(c4r:smap-mark Funcs Map NF)
		(define Used (c4r:smap-number Map 0 NF 0))
		(define B (list))
		(set! B (cons 1 (cons 1 B)))                  ;; version 1; files carry a CRC
		(set! B (c4r:uleb Used B))
		(set! B (c4r:smap-files c4r:srcfiles Map 0 B))
		(set! B (c4r:uleb (length Funcs) B))
		(set! B (c4r:smap-funcs (second Got) Map Codelen 0 B))
		(set! B (c4r:uleb (length Rows2) B))
		(set! B (c4r:smap-rows Rows2 Map 0 0 0 B))
		(c4r:bytes (reverse B)))))

;; ((POS LOC) ...) and ((POS NAME LOC) ...), in code order
(define c4r:smap-collect (lambda (Code Pos Rows Funcs)
	(if (empty? Code) (list (reverse Rows) (reverse Funcs))
	(begin
		(define I (head Code))
		(define H (head I))
		(if (= 'line H) (set! Rows (cons (list Pos (second I)) Rows))
		(if (= 'func H) (set! Funcs (cons (list Pos (second I) (third I)) Funcs)) nil))
		(next c4r:smap-collect (tail Code)
			(if (= 'label H) Pos (+ Pos (c4r:size I))) Rows Funcs)))))
;; of rows at one place, the last
(define c4r:smap-later (lambda (Rows Acc)
	(if (empty? Rows) (reverse Acc)
	(if (if (empty? (tail Rows)) false (= (head (head Rows)) (head (head (tail Rows)))))
		(next c4r:smap-later (tail Rows) Acc)
	(next c4r:smap-later (tail Rows) (cons (head Rows) Acc))))))
;; a row that says what the one before it said
(define c4r:smap-dedup (lambda (Rows Prev Acc)
	(if (empty? Rows) (reverse Acc)
	(if (= (second (head Rows)) Prev) (next c4r:smap-dedup (tail Rows) Prev Acc)
	(next c4r:smap-dedup (tail Rows) (second (head Rows)) (cons (head Rows) Acc))))))
(define c4r:smap-before (lambda (Rows End Acc)
	(if (empty? Rows) (reverse Acc)
	(if (>= (head (head Rows)) End) (reverse Acc)
	(next c4r:smap-before (tail Rows) End (cons (head Rows) Acc))))))

(define c4r:smap-file (lambda (Loc) (bit:shr Loc 20)))
(define c4r:smap-line (lambda (Loc) (bit:and Loc 1048575)))
(define c4r:smap-mark (lambda (L Map NF)
	(if (empty? L) Map
	(begin
		(define F (c4r:smap-file (index (head L) (- (length (head L)) 1))))
		(if (< F NF) (string:word! Map (* W F) 1) nil)
		(next c4r:smap-mark (tail L) Map NF)))))
(define c4r:smap-number (lambda (Map F NF N)
	(if (>= F NF) N
	(if (= 0 (string:word Map (* W F))) (next c4r:smap-number Map (+ F 1) NF N)
	(begin
		(string:word! Map (* W F) (+ N 1))
		(next c4r:smap-number Map (+ F 1) NF (+ N 1)))))))
(define c4r:smap-new (lambda (Map Loc)
	(begin
		(define F (c4r:smap-file Loc))
		(if (< F (/ (length Map) W)) (- (string:word Map (* W F)) 1) 0))))

(define c4r:smap-files (lambda (Fs Map F B)
	(if (empty? Fs) B
	(begin
		(if (= 0 (string:word Map (* W F))) nil
			(begin
				(define P (head (head Fs)))
				(set! B (c4r:uleb (length P) B))
				(set! B (c4r:push-string P 0 B))
				(set! B (c4r:uleb (second (head Fs)) B))
				(set! B (c4r:push-word32 (third (head Fs)) B))))
		(next c4r:smap-files (tail Fs) Map (+ F 1) B)))))
(define c4r:smap-named (lambda (Fs) (next c4r:smap-named/2 Fs (list))))
(define c4r:smap-named/2 (lambda (Fs Acc)
	(if (empty? Fs) (reverse Acc)
	(next c4r:smap-named/2 (tail Fs)
		(if (= false (second (head Fs))) Acc (cons (head Fs) Acc))))))
(define c4r:smap-funcs (lambda (Fs Map End Prev B)
	(if (empty? Fs) B
	(if (= false (second (head Fs))) (next c4r:smap-funcs (tail Fs) Map End Prev B)
	(begin
		(define F (head Fs))
		(define Start (head F))
		(define Next (if (empty? (tail Fs)) End (head (head (tail Fs)))))
		(define Name (+ "" (second F)))
		(set! B (c4r:uleb (- Start Prev) B))
		(set! B (c4r:uleb (- Next Start) B))
		(set! B (c4r:uleb (length Name) B))
		(set! B (c4r:push-string Name 0 B))
		(set! B (c4r:uleb (c4r:smap-new Map (third F)) B))
		(set! B (c4r:uleb (c4r:smap-line (third F)) B))
		(next c4r:smap-funcs (tail Fs) Map End Start B))))))
(define c4r:smap-rows (lambda (Rs Map PrevOff PrevFile PrevLine B)
	(if (empty? Rs) B
	(begin
		(define R (head Rs))
		(define File (c4r:smap-new Map (second R)))
		(define Line (c4r:smap-line (second R)))
		(define New (if (= File PrevFile) 0 1))
		(set! B (c4r:uleb (+ (* 2 (- (head R) PrevOff)) New) B))
		(if (= New 1) (set! B (c4r:uleb File B)) nil)
		(set! B (c4r:sleb (- Line PrevLine) B))
		(next c4r:smap-rows (tail Rs) Map (head R) File Line B)))))

;; LEB128 onto a reversed byte list
(define c4r:uleb (lambda (V B)
	(if (< V 128) (cons V B)
	(next c4r:uleb (bit:shr V 7) (cons (+ 128 (bit:and V 127)) B)))))
(define c4r:sleb (lambda (V B)
	(begin
		(define Byte (bit:and V 127))
		(define Rest (bit:shr V 7))                 ;; arithmetic
		(if (if (= Rest 0) (= 0 (bit:and Byte 64)) (if (= Rest -1) (= 64 (bit:and Byte 64)) false))
			(cons Byte B)
		(next c4r:sleb Rest (cons (+ 128 Byte) B))))))
(define c4r:push-string (lambda (S I B)
	(if (>= I (length S)) B
	(next c4r:push-string S (+ I 1) (cons (string:byte S I) B)))))
(define c4r:push-word32 (lambda (V B)
	(cons (bit:and (bit:shr V 24) 255) (cons (bit:and (bit:shr V 16) 255)
		(cons (bit:and (bit:shr V 8) 255) (cons (bit:and V 255) B))))))
(define c4r:bytes (lambda (L) (c4r:poke L (string:alloc (length L)) 0)))
(define c4r:poke (lambda (L S I)
	(if (empty? L) S
	(begin (string:byte! S I (head L)) (next c4r:poke (tail L) S (+ I 1))))))

;; ---- SMAP back into items, for a tool that changes an image's code ----
;;
;; c4opt-run optimises a finished image, whose map would otherwise be
;; dropped with the code it describes. c4r:smap-carry puts the map back
;; into the decoded code as the items c4lc's generator makes, (func NAME
;; LOC) and (line LOC), ahead of whatever is at their offset, labels
;; included, as the generator puts a function's. Its files go in
;; c4r:srcfiles. The passes then step over the items as they do over
;; c4lc's, and c4r:encode writes the map of the code as it comes out. A
;; map is used only if it is version 1 with CRCs, as every compiler here
;; writes; M comes back as it was without one.
(define c4r:smap-carry (lambda (M)
	(begin
		(define P (c4r:chunk (if (> (length M) 9) (index M 9) (list)) "SMAP"))
		(define D (if (= P false) false (c4r:smap-decode P)))
		(if (= D false) M
		(begin
			(set! c4r:srcfiles (head D))
			(+ (list (head M) (second M) (third M)
				(c4r:smap-items (index M 3) 0 (second D) (third D) (list)))
				(tail (tail (tail (tail M))))))))))
(define c4r:chunk (lambda (Chunks Tag)
	(if (empty? Chunks) false
	(if (= (head (head Chunks)) Tag) (second (head Chunks))
	(next c4r:chunk (tail Chunks) Tag)))))

;; (FILES FUNCS ROWS): ((PATH SIZE CRC) ...), ((POS NAME LOC) ...) and
;; ((POS LOC) ...), as c4r:smap reads them out of the items; or false.
;; Where a function ends before the next starts, (END false 0) follows it.
(define c4r:mat 0)
(define c4r:smap-decode (lambda (S)
	(if (< (length S) 3) false
	(if (not (= 1 (string:byte S 0))) false
	(if (= 0 (bit:and 1 (string:byte S 1))) false
	(begin
		(set! c4r:mat 2)
		(define Files (c4r:smd-files S (c4r:mu S) (list)))
		(define Funcs (c4r:smd-ends (c4r:smd-funcs S (c4r:mu S) 0 (list)) (list)))
		(define Rows (c4r:smd-rows S (c4r:mu S) 0 0 0 (list)))
		(list Files Funcs Rows)))))))
(define c4r:smd-files (lambda (S N Acc)
	(if (if (= N 0) true (>= c4r:mat (length S))) (reverse Acc)
	(begin
		(define L (c4r:mu S))
		(define P (string:substr S c4r:mat L))
		(set! c4r:mat (+ c4r:mat L))
		(define Size (c4r:mu S))
		(define Crc (c4r:word32 S c4r:mat))
		(set! c4r:mat (+ c4r:mat 4))
		(next c4r:smd-files S (- N 1) (cons (list P Size Crc) Acc))))))
(define c4r:smd-funcs (lambda (S N Start Acc)
	(if (if (= N 0) true (>= c4r:mat (length S))) (reverse Acc)
	(begin
		(define Pos (+ Start (c4r:mu S)))
		(define Words (c4r:mu S))
		(define L (c4r:mu S))
		(define Name (string:substr S c4r:mat L))
		(set! c4r:mat (+ c4r:mat L))
		(define F (c4r:mu S))
		(define Line (c4r:mu S))
		(next c4r:smd-funcs S (- N 1) Pos (cons (list Pos Name (+ (* F c4r:LOCF) Line) (+ Pos Words)) Acc))))))
;; (POS NAME LOC END) -> (POS NAME LOC), and (END false 0) where END is
;; short of the next start (always after the last: an end at the end of
;; the code is dropped with the items there)
(define c4r:smd-ends (lambda (Fs Acc)
	(if (empty? Fs) (reverse Acc)
	(begin
		(define F (head Fs))
		(define Item (list (head F) (second F) (third F)))
		(if (if (empty? (tail Fs)) false (>= (index F 3) (head (head (tail Fs)))))
			(next c4r:smd-ends (tail Fs) (cons Item Acc))
			(next c4r:smd-ends (tail Fs) (cons (list (index F 3) false 0) (cons Item Acc))))))))
(define c4r:smd-rows (lambda (S N Off F Line Acc)
	(if (if (= N 0) true (>= c4r:mat (length S))) (reverse Acc)
	(begin
		(define X (c4r:mu S))
		(define Off2 (+ Off (bit:shr X 1)))
		(define F2 (if (= 1 (bit:and X 1)) (c4r:mu S) F))
		(define Line2 (+ Line (c4r:ms S)))
		(next c4r:smd-rows S (- N 1) Off2 F2 Line2
			(cons (list Off2 (+ (* F2 c4r:LOCF) Line2)) Acc))))))
;; LEB128 at c4r:mat, which moves past it
(define c4r:mu (lambda (S) (next c4r:mu/3 S 0 1)))
(define c4r:mu/3 (lambda (S V M)
	(begin
		(define C (string:byte S c4r:mat))
		(set! c4r:mat (+ c4r:mat 1))
		(define V2 (+ V (* (bit:and C 127) M)))
		(if (= 0 (bit:and C 128)) V2 (next c4r:mu/3 S V2 (* M 128))))))
(define c4r:ms (lambda (S) (next c4r:ms/3 S 0 1)))
(define c4r:ms/3 (lambda (S V M)
	(begin
		(define C (string:byte S c4r:mat))
		(set! c4r:mat (+ c4r:mat 1))
		(define V2 (+ V (* (bit:and C 127) M)))
		(if (= 0 (bit:and C 128))
			(if (= 0 (bit:and C 64)) V2 (- V2 (* M 128)))
			(next c4r:ms/3 S V2 (* M 128))))))
(define c4r:word32 (lambda (S At)
	(bit:or (string:byte S At)
		(bit:or (bit:shl (string:byte S (+ At 1)) 8)
			(bit:or (bit:shl (string:byte S (+ At 2)) 16) (bit:shl (string:byte S (+ At 3)) 24))))))

;; Code with the items at their offsets. A row that a damaged map put
;; inside an instruction goes before the next one; anything at the end of
;; the code or past it is dropped, as encode would drop it.
(define c4r:smap-items (lambda (Code Pos Funcs Rows Acc)
	(if (empty? Code) (reverse Acc)
	(if (if (empty? Rows) false (< (head (head Rows)) Pos))
		(next c4r:smap-items Code Pos Funcs (tail Rows) (cons (list 'line (second (head Rows))) Acc))
	(if (if (empty? Funcs) false (<= (head (head Funcs)) Pos))
		(next c4r:smap-items Code Pos (tail Funcs) Rows
			(cons (list 'func (second (head Funcs)) (third (head Funcs))) Acc))
	(if (if (empty? Rows) false (= (head (head Rows)) Pos))
		(next c4r:smap-items Code Pos Funcs (tail Rows) (cons (list 'line (second (head Rows))) Acc))
	(next c4r:smap-items (tail Code)
		(if (= 'label (head (head Code))) Pos (+ Pos (c4r:size (head Code))))
		Funcs Rows (cons (head Code) Acc))))))))

(define c4r:trailer-size (lambda (Chunks N)
	(if (empty? Chunks) N
	(next c4r:trailer-size (tail Chunks)
		(+ N (* 2 W) (c4r:padded (length (second (head Chunks)))))))))

(define c4r:emit-trailer (lambda (Chunks Out At)
	(begin
		(string:byte! Out At 88)                   ;; X
		(next c4r:emit-chunks Chunks Out (+ At W)))))
(define c4r:emit-chunks (lambda (Chunks Out At)
	(if (empty? Chunks) Out                         ;; the 0 tag: string:alloc zeroed it
	(begin
		(define C (head Chunks))
		(c4r:copy-string (head C) Out At 0)
		(string:word! Out (+ At W) (length (second C)))
		(c4r:copy-string (second C) Out (+ At (* 2 W)) 0)
		(next c4r:emit-chunks (tail Chunks) Out
			(+ At (* 2 W) (c4r:padded (length (second C)))))))))

(define c4r:read-patches (lambda (F At N Acc)
	(if (= 0 N) Acc
		(next c4r:read-patches F (+ At (* 3 W)) (- N 1)
			(cons (list (string:word F At)
			            (string:word F (+ At W))
			            (string:word F (+ At (* 2 W)))) Acc)))))

;; split the patch list: code-resident (-1, -2, symbols) drive the
;; instruction walk; data-resident (-3, -4) ride along as module data
;; These and the other whole-image walks below loop (next, into an
;; accumulator, reversed at the end) rather than recurse. A recursion
;; here is a stack frame per patch, symbol or function: c4sc's opt mode
;; on c4ix32.c4r was 3,618 frames deep in c4r:data-patches, 145 KB, more
;; than half of a C4KE task's stack. c4lc's g:resolvecode, the same
;; shape over a module's code, is what overran it (c4lc-gen.lisp).
(define c4r:code-patches (lambda (Ps) (next c4r:code-patches/2 Ps (list))))
(define c4r:code-patches/2 (lambda (Ps Acc)
	(if (empty? Ps) (reverse Acc)
		(next c4r:code-patches/2 (tail Ps)
			(if (< (head (head Ps)) -2) Acc (cons (head Ps) Acc))))))
(define c4r:data-patches (lambda (Ps) (next c4r:data-patches/2 Ps (list))))
(define c4r:data-patches/2 (lambda (Ps Acc)
	(if (empty? Ps) (reverse Acc)
		(next c4r:data-patches/2 (tail Ps)
			(if (= -3 (head (head Ps)))
				(cons (list 'dcode (second (head Ps)) (third (head Ps))) Acc)
			(if (= -4 (head (head Ps)))
				(cons (list 'ddata (second (head Ps)) (third (head Ps))) Acc)
			(if (= -5 (head (head Ps)))
				(cons (list 'dsym (second (head Ps)) (third (head Ps))) Acc)
				Acc)))))))

(define c4r:read-words (lambda (F At N Acc)
	(if (= 0 N) Acc
		(next c4r:read-words F (+ At W) (- N 1) (cons (string:word F At) Acc)))))

(define c4r:read-syms (lambda (F At N Acc) (begin
	(if (= 0 N) Acc (begin
		(define Id (string:word F At))
		(define Ty (string:word F (+ At W)))
		(define Cl (string:word F (+ At (* 2 W))))
		(define Attrs (string:word F (+ At (* 3 W))))
		(define NameLen (string:byte F (+ At (* 4 W))))
		(define Name (string:substr F (+ At (* 4 W) 1) NameLen))
		(define Value (string:word F (+ At (* 4 W) 1 NameLen)))
		(next c4r:read-syms F (+ At (* 5 W) 1 NameLen) (- N 1)
			(cons (list Id Ty Cl Attrs Name Value) Acc)))))))

(define c4r:mark-patch-labels (lambda (Patches Labels)
	(if (empty? Patches) nil (begin
		(define P (head Patches))
		(if (= -1 (head P)) (string:byte! Labels (third P) 1))
		;; data-resident code patches (-3) target code as well
		(if (= -3 (head P)) (string:byte! Labels (third P) 1))
		(next c4r:mark-patch-labels (tail Patches) Labels)))))

(define c4r:mark-word-labels (lambda (Offs Labels)
	(if (empty? Offs) nil (begin
		(string:byte! Labels (head Offs) 1)
		(next c4r:mark-word-labels (tail Offs) Labels)))))

;; a defined function's value is a code offset (class 129 Fun, and not
;; ATTR_EXTERN 0x10 whose value is compile-time garbage)
(define c4r:sym-defined (lambda (S)
	(if (= 129 (third S)) (= 0 (bit:and 16 (index S 3))) false)))

(define c4r:mark-sym-labels (lambda (Syms Labels)
	(if (empty? Syms) nil (begin
		(define S (head Syms))
		(if (c4r:sym-defined S) (string:byte! Labels (index S 5) 1))
		(next c4r:mark-sym-labels (tail Syms) Labels)))))

;; rewrite symbol values / constructor lists as (code N) label references
(define c4r:label-syms (lambda (Syms) (next c4r:label-syms/2 Syms (list))))
(define c4r:label-syms/2 (lambda (Syms Acc)
	(if (empty? Syms) (reverse Acc)
		(next c4r:label-syms/2 (tail Syms) (cons (c4r:label-sym (head Syms)) Acc)))))
(define c4r:label-sym (lambda (S)
	(if (c4r:sym-defined S)
		(list (head S) (second S) (third S) (index S 3) (index S 4)
			(list 'code (index S 5)))
		S)))
(define c4r:label-offsets (lambda (Offs) (next c4r:label-offsets/2 Offs (list))))
(define c4r:label-offsets/2 (lambda (Offs Acc)
	(if (empty? Offs) (reverse Acc)
		(next c4r:label-offsets/2 (tail Offs) (cons (list 'code (head Offs)) Acc)))))

;; The decoder walk. Pos is a word offset; Patches is the not-yet-consumed
;; patch list, ascending by address (asm-c4r emits them in code order).
;; A word is decoded as an instruction unless that would swallow a label
;; or a patch target -- then it is kept as a raw (word W), which is how
;; the dead zero word at offset 0 survives.
(define c4r:decode-code (lambda (F At Codelen Labels Patches Pos Acc) (begin
	(if (>= Pos Codelen) Acc (begin
		;; the patch list must stay in step with the walk
		(if (if (empty? Patches) false (< (second (head Patches)) Pos))
			(error "c4r: patch list out of sync with instruction stream"))
		(if (= 1 (string:byte Labels Pos))
			(set! Acc (cons (list 'label Pos) Acc)))
		(define Wv (string:word F (+ At (* Pos W))))
		(if (if (empty? Patches) false (= Pos (second (head Patches))))
			;; a patch aimed at this word itself, not an operand slot: a
			;; standalone patched code word (a switch jump table entry)
			(begin
				(define P (head Patches))
				(define T (head P))
				(set! Acc (cons (list 'cword
					(if (= -1 T) (list 'code (third P) Wv)
						(if (= -2 T) (list 'data (third P) Wv)
							(list 'extern T Wv (third P))))) Acc))
				(next c4r:decode-code F At Codelen Labels (tail Patches) (+ Pos 1) Acc))
			(begin
		(define TakesOp
			(if (< Wv 0) false
				(if (>= Wv c4r:nops) false (c4r:has-operand Wv))))
		;; an operand cannot sit on a label, and the operand slot of a
		;; real instruction is exactly where a patch points
		(if TakesOp
			(if (>= (+ Pos 1) Codelen) (set! TakesOp false)
				(if (= 1 (string:byte Labels (+ Pos 1))) (set! TakesOp false))))
		(if (not TakesOp) (begin
			;; bare instruction or raw word
			(if (if (< Wv 0) false (< Wv c4r:nops))
				(set! Acc (cons (list (c4r:opname Wv)) Acc))
				(set! Acc (cons (list 'word Wv) Acc)))
			(next c4r:decode-code F At Codelen Labels Patches (+ Pos 1) Acc)
		) (begin
			(define Operand (string:word F (+ At (* (+ Pos 1) W))))
			;; does the next patch target this operand slot?
			(if (if (empty? Patches) false (= (+ Pos 1) (second (head Patches))))
				(begin
					(define P (head Patches))
					(define T (head P))
					(set! Acc (cons (list (c4r:opname Wv)
						(if (= -1 T) (list 'code (third P) Operand)
							(if (= -2 T) (list 'data (third P) Operand)
								;; extern keeps the patch value word too:
								;; it is compile-time garbage, preserved
								;; for the byte-identical round trip
								(list 'extern T Operand (third P))))) Acc))
					(next c4r:decode-code F At Codelen Labels (tail Patches) (+ Pos 2) Acc))
				(begin
					(set! Acc (cons (list (c4r:opname Wv) Operand) Acc))
					(next c4r:decode-code F At Codelen Labels Patches (+ Pos 2) Acc)))
		))))))
)))

;; ---- writing ----

;; Encode a module back to a byte string.
;; ---- format v3: data MEMSZ + trailing-zero (BSS) trim ----
;;
;; v3 stores only the data bytes up to the last NON-zero byte; the
;; full in-memory size (MEMSZ) rides in the header padding word (byte
;; offset 5), and the loader zero-fills [stored, MEMSZ). That trailing
;; zero region is BSS: it occupies no image bytes. c4r:v3 gates this;
;; The compiler (c4lc.lisp) sets it to force v3 on every image it emits.
;; Left false, encode instead PRESERVES the module's own version -- so
;; roundtrip (decode then encode) reproduces a v2 image as v2 and a v3
;; image as v3, byte-identically.
(define c4r:v3 false)

;; extra in-memory data bytes beyond the module's Data string: the
;; word-align pad plus the BSS size the compiler segregated out (see
;; c4lc-gen's gen:bssextra). MEMSZ = (length Data) + this. Set by the
;; caller (c4lc.lisp) before each encode; reset to 0 afterward so a
;; later encode without BSS is unaffected.
(define c4r:bss-extra 0)

;; chunks the next encode appends to the module's own (test-srcmap's
;; fixture). Reset by whoever sets it, as c4r:bss-extra is.
(define c4r:extra-chunks (list))

;; The compiler's source files, (PATH SIZE CRC) by file number, when it
;; compiled with -g: then encode makes a source map of the code's line
;; and func items (c4lc.lisp sets it, and resets it).
(define c4r:srcfiles (list))

;; stored length = full length minus the trailing run of zero bytes.
(define c4r:stored-len (lambda (Data n)
	(if (= n 0) 0
	(if (= 0 (string:byte Data (- n 1))) (next c4r:stored-len Data (- n 1))
	n))))

;; copy exactly N bytes (c4r:copy-string copies all of Src, which is
;; longer than the stored length once trailing zeros are trimmed)
(define c4r:copy-string-n (lambda (Src Out At I N)
	(if (>= I N) Out (begin
		(string:byte! Out (+ At I) (string:byte Src I))
		(next c4r:copy-string-n Src Out At (+ I 1) N)))))

(define c4r:encode (lambda (M) (begin
	(define Version (head M))
	(define Wordbits (second M))
	(define Entry (third M))
	(define Code (index M 3))
	(define Data (index M 4))
	(define Syms (index M 5))
	(define Cons (index M 6))
	(define Des (index M 7))
	;; emit v3 (MEMSZ + BSS trim) when the compiler forces it, or when
	;; the module itself is already v3 (roundtrip preserves the version)
	(define EmitV3 (if c4r:v3 true (>= Version 3)))

	;; pass 1: instruction sizes give each label its new offset. The
	;; label table is a word array indexed by label id.
	(define MaxLabel (c4r:max-label Code 0))
	(define LT (string:alloc (* W (+ MaxLabel 2))))
	(define Codelen (c4r:place Code LT 0))

	;; patch count = reference operands + data-resident patches
	(define DPatches (index M 8))
	(define Patchlen (+ (c4r:count-refs Code 0) (length DPatches)))
	(define Memsz (+ (length Data) c4r:bss-extra)) ;; full in-memory data size (+ BSS)
	(define Datalen (if EmitV3 (c4r:stored-len Data (length Data)) (length Data)))  ;; stored bytes
	(define Conslen (length Cons))
	(define Deslen (length Des))
	(define Symslen (length Syms))
	;; the trailer's chunks: the module's own (a decoded image's), then
	;; whatever this encode adds
	(define Chunks (+ (if (> (length M) 9) (index M 9) (list))
		(+ (if (empty? c4r:srcfiles) (list) (list (list "SMAP" (c4r:smap Code Codelen))))
			c4r:extra-chunks)))
	(define TrailerLen (if (empty? Chunks) 0 (+ W (c4r:trailer-size Chunks 0) W)))

	;; total file size (only the STORED data bytes are written)
	(define Total (+ 13 (* 7 W)
		W (* Codelen W)
		W Datalen
		W (* Patchlen (* 3 W))
		W (* Conslen W)
		W (* Deslen W)
		W (c4r:syms-size Syms 0)
		TrailerLen))
	(define Out (string:alloc Total))

	;; header
	(string:byte! Out 0 67) (string:byte! Out 1 52) (string:byte! Out 2 82)
	(string:byte! Out 3 (if EmitV3 3 Version))
	(string:byte! Out 4 Wordbits)
	(define I 5)
	;; padding word (byte 5): MEMSZ in v3, else 'p' filler as asm-c4r uses
	(if EmitV3
		(begin (c4r:fill-bytes Out 5 8 0) (string:word! Out 5 Memsz))
		(c4r:fill-bytes Out 5 8 112))
	(string:word! Out 13 (if (= -1 Entry) -1 (c4r:label LT (second Entry))))
	(string:word! Out (+ 13 W) Codelen)
	(string:word! Out (+ 13 (* 2 W)) Datalen)
	(string:word! Out (+ 13 (* 3 W)) Patchlen)
	(string:word! Out (+ 13 (* 4 W)) Symslen)
	(string:word! Out (+ 13 (* 5 W)) Conslen)
	(string:word! Out (+ 13 (* 6 W)) Deslen)

	;; code (marker then words); patches are collected while walking
	(define CodeAt (+ 13 (* 8 W)))
	(string:byte! Out (- CodeAt W) 67)  ;; C
	(define DataAt (+ CodeAt (* Codelen W) W))
	(string:byte! Out (- DataAt W) 68)  ;; D
	(define PatchAt (+ DataAt Datalen W))
	(string:byte! Out (- PatchAt W) 80) ;; P
	(define ConsAt (+ PatchAt (* Patchlen (* 3 W)) W))
	(string:byte! Out (- ConsAt W) 99)  ;; c
	(define DesAt (+ ConsAt (* Conslen W) W))
	(string:byte! Out (- DesAt W) 100)  ;; d
	(define SymsAt (+ DesAt (* Deslen W) W))
	(string:byte! Out (- SymsAt W) 83)  ;; S

	(c4r:emit-code Code Out CodeAt PatchAt LT 0)
	(c4r:emit-dpatches DPatches Out
		(+ PatchAt (* (c4r:count-refs Code 0) (* 3 W))) LT)
	(c4r:copy-string-n Data Out DataAt 0 Datalen)  ;; stored bytes only
	(c4r:emit-words (c4r:resolve-offsets Cons LT) Out ConsAt)
	(c4r:emit-words (c4r:resolve-offsets Des LT) Out DesAt)
	(c4r:emit-syms Syms Out SymsAt LT)
	(if (empty? Chunks) nil
		(c4r:emit-trailer Chunks Out (+ SymsAt (c4r:syms-size Syms 0))))
	Out
)))

(define c4r:max-label (lambda (Code M)
	(if (empty? Code) M (begin
		(define I (head Code))
		(if (= 'label (head I))
			(if (> (second I) M) (set! M (second I))))
		(next c4r:max-label (tail Code) M)))))

;; label id -> new word offset
(define c4r:label (lambda (LT N) (string:word LT (* W N))))

;; pass 1: assign offsets; returns the code length in words
(define c4r:place (lambda (Code LT Pos)
	(if (empty? Code) Pos (begin
		(define I (head Code))
		(define H (head I))
		(if (= 'label H)
			(string:word! LT (* W (second I)) Pos)
			(set! Pos (+ Pos (c4r:size I))))
		(next c4r:place (tail Code) LT Pos)))))
;; an item's words: (OP) 1, (OP operand) 2, a raw or patched word 1, and
;; -g's (line LOC) and (func NAME LOC) none
(define c4r:size (lambda (I)
	(begin
		(define H (head I))
		(if (= 'word H) 1
		(if (= 'cword H) 1
		(if (= 'line H) 0
		(if (= 'func H) 0
		(length I))))))))

(define c4r:is-ref (lambda (Op)
	(if (= 'list (typeof Op))
		(if (= 'code (head Op)) true
			(if (= 'data (head Op)) true (= 'extern (head Op))))
		false)))

(define c4r:count-refs (lambda (Code N)
	(if (empty? Code) N (begin
		(define I (head Code))
		(if (= 2 (length I))
			(if (c4r:is-ref (second I)) (set! N (+ N 1))))
		(next c4r:count-refs (tail Code) N)))))

(define c4r:fill-bytes (lambda (S At N V)
	(if (= 0 N) S (begin
		(string:byte! S At V)
		(next c4r:fill-bytes S (+ At 1) (- N 1) V)))))

(define c4r:copy-string (lambda (Src Out At I)
	(if (>= I (length Src)) Out (begin
		(string:byte! Out (+ At I) (string:byte Src I))
		(next c4r:copy-string Src Out At (+ I 1))))))

(define c4r:resolve-offsets (lambda (Refs LT) (next c4r:resolve-offsets/2 Refs LT (list))))
(define c4r:resolve-offsets/2 (lambda (Refs LT Acc)
	(if (empty? Refs) (reverse Acc)
		(next c4r:resolve-offsets/2 (tail Refs) LT
			(cons (c4r:label LT (second (head Refs))) Acc)))))

;; data-resident patches, after the code-resident ones. dcode targets are
;; labels so the optimizer can move code without breaking jump tables.
(define c4r:emit-dpatches (lambda (DP Out At LT)
	(if (empty? DP) Out (begin
		(define P (head DP))
		(string:word! Out At (if (= 'dcode (head P)) -3 (if (= 'dsym (head P)) -5 -4)))
		(string:word! Out (+ At W) (second P))
		(string:word! Out (+ At (* 2 W))
			(if (= 'dcode (head P)) (c4r:label LT (third P)) (third P)))
		(next c4r:emit-dpatches (tail DP) Out (+ At (* 3 W)) LT)))))

(define c4r:emit-words (lambda (Words Out At)
	(if (empty? Words) Out (begin
		(string:word! Out At (head Words))
		(next c4r:emit-words (tail Words) Out (+ At W))))))

;; pass 2: emit instruction words and, for reference operands, a patch.
;; The operand word is the preserved RAW value when present, else the
;; resolved target offset (dead either way: the loader overwrites it).
(define c4r:emit-code (lambda (Code Out At PatchAt LT Pos)
	(if (empty? Code) Pos (begin
		(define I (head Code))
		(define H (head I))
		(if (if (= 'label H) true (if (= 'line H) true (= 'func H))) nil
		(if (= 'word H) (begin
			(string:word! Out (+ At (* Pos W)) (second I))
			(set! Pos (+ Pos 1)))
		(if (= 'cword H) (begin
			;; one word carrying its own patch
			(define Op (second I))
			(define T (head Op))
			(string:word! Out PatchAt
				(if (= 'code T) -1 (if (= 'data T) -2 (second Op))))
			(string:word! Out (+ PatchAt W) Pos)
			(string:word! Out (+ PatchAt (* 2 W))
				(if (= 'code T) (c4r:label LT (second Op))
					(if (= 'data T) (second Op)
						(if (= 4 (length Op)) (index Op 3) 0))))
			(set! PatchAt (+ PatchAt (* 3 W)))
			(string:word! Out (+ At (* Pos W))
				(if (>= (length Op) 3) (third Op)
					(if (= 'code T) (c4r:label LT (second Op)) 0)))
			(set! Pos (+ Pos 1)))
		(begin
			(string:word! Out (+ At (* Pos W)) (c4r:opnum H))
			(if (= 2 (length I)) (begin
				(define Op (second I))
				(if (c4r:is-ref Op) (begin
					;; patch entry: (type address value)
					(define T (head Op))
					(string:word! Out PatchAt
						(if (= 'code T) -1 (if (= 'data T) -2 (second Op))))
					(string:word! Out (+ PatchAt W) (+ Pos 1))
					(string:word! Out (+ PatchAt (* 2 W))
						(if (= 'code T) (c4r:label LT (second Op))
							(if (= 'data T) (second Op)
								;; extern: the preserved garbage value word,
								;; or 0 for optimizer-created references
								(if (= 4 (length Op)) (index Op 3) 0))))
					(set! PatchAt (+ PatchAt (* 3 W)))
					;; operand word: raw if preserved, else the target
					(string:word! Out (+ At (* (+ Pos 1) W))
						(if (>= (length Op) 3) (third Op)
							(if (= 'code T) (c4r:label LT (second Op)) 0)))
				) (begin
					(string:word! Out (+ At (* (+ Pos 1) W)) Op)
				))
				(set! Pos (+ Pos 2))
			) (set! Pos (+ Pos 1)))
		))))
		(next c4r:emit-code (tail Code) Out At PatchAt LT Pos)))))

(define c4r:syms-size (lambda (Syms N)
	(if (empty? Syms) N
		(next c4r:syms-size (tail Syms)
			(+ N (* 4 W) 1 (length (index (head Syms) 4)) W)))))

(define c4r:emit-syms (lambda (Syms Out At LT)
	(if (empty? Syms) Out (begin
		(define S (head Syms))
		(define Name (index S 4))
		(define V (index S 5))
		(string:word! Out At (head S))
		(string:word! Out (+ At W) (second S))
		(string:word! Out (+ At (* 2 W)) (third S))
		(string:word! Out (+ At (* 3 W)) (index S 3))
		(string:byte! Out (+ At (* 4 W)) (length Name))
		(c4r:copy-string Name Out (+ At (* 4 W) 1) 0)
		(string:word! Out (+ At (* 4 W) 1 (length Name))
			(if (= 'list (typeof V)) (c4r:label LT (second V)) V))
		(next c4r:emit-syms (tail Syms) Out (+ At (* 5 W) 1 (length Name)) LT)))))

;; ---- listing (debug aid, and the optimizer's view) ----

(define c4r:print-code (lambda (Code)
	(if (empty? Code) nil (begin
		(print "  " (head Code))
		(next c4r:print-code (tail Code))))))

)
