;; Dump a preprocessed token stream, for differential testing against
;; gcc -E: the two must agree token for token, which is a stronger
;; check than comparing text (whitespace and line markers differ).
(begin
	(load "c4lc-lex.lisp")
	(load "c4lc-pp.lisp")
	(define Args argv)
	(define paths nil)
	(define flags (lambda ()
		(if (empty? Args) nil
		(if (= (+ "" (head Args)) "-I")
			(begin
				(set! paths (+ paths (list (+ "" (index Args 1)))))
				(set! Args (tail (tail Args)))
				(next flags))
		(if (= (+ "" (head Args)) "-D")
			(begin
				(pp:predefine (+ "" (index Args 1)))
				(set! Args (tail (tail Args)))
				(next flags))
		nil)))))
	(flags)
	(set! pp:paths paths)
	(define dump (lambda (l)
		(if (empty? l) nil
		(begin
			(define t (head l))
			(print (+ "" (head t)) (+ "" (head (tail t))))
			(next dump (tail l))))))
	(dump (pp:file (head Args))))
