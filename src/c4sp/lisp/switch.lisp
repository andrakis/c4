;; switch macro
;; Implements a switch/case statement processor.
;; Translates from ((case X) Y) into a series of (if ...) statements.
(begin
	(define switch-translate (lambda (X Conds) (begin
		(sw-tr X Conds (list))
	)))
	;; Convert a list from:
	;;   ((if (= X 1) "one") (if (= X 2) "two") "other")
	;; To:
	;;   (if (= X 1) "one" (if (= X 2) "two" "other"))
	(define sw-tr-late (lambda (Conds)
		(if (empty? Conds)
			(list)
			(if (empty? (tail Conds))
				(head Conds)
				(+ (head Conds) (list (sw-tr-late (tail Conds))))))))
	;; Translate a series of ((case X) Y) statements
	(define sw-tr (lambda (X Conds Acc)
		(if (empty? Conds)
			(sw-tr-late Acc)
			(sw-tr X (tail Conds) (sw-tr-section X (head Conds) Acc)))))
	;; Translate a single ((case X) Y) or (true Y) statement
	(define sw-tr-section (lambda (X Cond Acc) (begin
		(define CondCheck (index Cond 0))
		(define CondEq (index Cond 1))
		(define CaseOrTrue (index CondCheck 0))
		(define CompareValue (index CondCheck 1))
		(if (= CaseOrTrue (quote case))
			(push-end Acc
				(list (quote if) (list (quote =) X CompareValue) CondEq))
			(if (= CondCheck true)
				(push-end Acc CondEq)
				(begin
					(print "Unexpected CaseOrTrue:" CondCheck)
					(error "unexpected"))
			)
		))))
	(define push-end (lambda (List Item) (push-end/3 List Item (list))))
	(define push-end/3 (lambda (List Item Acc) (begin
		(if (empty? List)
			(+ Acc (list Item))
			(push-end/3 (tail List) Item (+ Acc (list (head List)))))
	)))
	(define switch (macro Conds (switch-translate (head Conds) (tail Conds))))
	(define X 5)
	(if (env:defined (quote argv))
		(if (not (empty? argv)) (begin
			(set! X (head argv))
			(print "Using" X "for value"))))
	(switch X
		((case 1) "one")
		((case 2) "two")
		((case 3) "three")
		((case 4) "four")
		(true "not sure (try numbers 1 - 4)"))
)
