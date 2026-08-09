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
	LXI SXI))
(define c4r:nops (length c4r:ops))

;; does opcode n take an operand word? LEA..ADJ are 0..7; JSRI 61 JSRS 62
(define c4r:has-operand (lambda (n)
	(if (<= n 7) true (if (= n 61) true (= n 62)))))

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
		DPatches)
)))

(define c4r:read-patches (lambda (F At N Acc)
	(if (= 0 N) Acc
		(next c4r:read-patches F (+ At (* 3 W)) (- N 1)
			(cons (list (string:word F At)
			            (string:word F (+ At W))
			            (string:word F (+ At (* 2 W)))) Acc)))))

;; split the patch list: code-resident (-1, -2, symbols) drive the
;; instruction walk; data-resident (-3, -4) ride along as module data
(define c4r:code-patches (lambda (Ps)
	(if (empty? Ps) (list)
		(if (< (head (head Ps)) -2)
			(c4r:code-patches (tail Ps))
			(cons (head Ps) (c4r:code-patches (tail Ps)))))))
(define c4r:data-patches (lambda (Ps)
	(if (empty? Ps) (list)
		(if (= -3 (head (head Ps)))
			(cons (list 'dcode (second (head Ps)) (third (head Ps)))
				(c4r:data-patches (tail Ps)))
			(if (= -4 (head (head Ps)))
				(cons (list 'ddata (second (head Ps)) (third (head Ps)))
					(c4r:data-patches (tail Ps)))
				(c4r:data-patches (tail Ps)))))))

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
(define c4r:label-syms (lambda (Syms)
	(if (empty? Syms) (list)
		(cons (c4r:label-sym (head Syms)) (c4r:label-syms (tail Syms))))))
(define c4r:label-sym (lambda (S)
	(if (c4r:sym-defined S)
		(list (head S) (second S) (third S) (index S 3) (index S 4)
			(list 'code (index S 5)))
		S)))
(define c4r:label-offsets (lambda (Offs)
	(if (empty? Offs) (list)
		(cons (list 'code (head Offs)) (c4r:label-offsets (tail Offs))))))

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

	;; total file size (only the STORED data bytes are written)
	(define Total (+ 13 (* 7 W)
		W (* Codelen W)
		W Datalen
		W (* Patchlen (* 3 W))
		W (* Conslen W)
		W (* Deslen W)
		W (c4r:syms-size Syms 0)))
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
			(if (= 'word H)
				(set! Pos (+ Pos 1))
				(if (= 'cword H)
					(set! Pos (+ Pos 1))
					(set! Pos (+ Pos (length I)))))) ;; (OP) 1 word, (OP operand) 2
		(next c4r:place (tail Code) LT Pos)))))

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

(define c4r:resolve-offsets (lambda (Refs LT)
	(if (empty? Refs) (list)
		(cons (c4r:label LT (second (head Refs)))
			(c4r:resolve-offsets (tail Refs) LT)))))

;; data-resident patches, after the code-resident ones. dcode targets are
;; labels so the optimizer can move code without breaking jump tables.
(define c4r:emit-dpatches (lambda (DP Out At LT)
	(if (empty? DP) Out (begin
		(define P (head DP))
		(string:word! Out At (if (= 'dcode (head P)) -3 -4))
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
		(if (= 'label H) nil
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
