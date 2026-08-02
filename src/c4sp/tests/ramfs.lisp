;; C4KE RAM filesystem smoke test: write, read back, verify. Under
;; anything else file:write either hits the host (native) or errors.
(begin
	(define Msg "written by c4sp inside c4ke")
	(if (file:write "c4sp-note.txt" Msg)
		(print "write ok")
		(print "write failed"))
	(define Got (file:read "c4sp-note.txt"))
	(print "read back:" Got)
	(if (= Msg Got) (print "ramfs roundtrip ok") (print "MISMATCH")))
