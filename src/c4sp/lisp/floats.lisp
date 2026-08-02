;; M4: binary32 floats via include/c4_float.h. Integers promote when a
;; float joins the fold; integer division stays integer (a documented
;; divergence -- write (/ 10.0 4) for the float answer). Values here are
;; exactly representable in binary32, so the output matches the Node
;; alisp build byte for byte despite its doubles.
(begin
	(print (+ 1 0.5))
	(print (- 1.5 2))
	(print (* 2.5 4))
	(print (/ 10.0 4))
	(print (/ 1 2.0))
	(print (= 1 1.0))
	(print (= 0.5 0.5))
	(print (< 1.5 2) (<= 2.0 2) (> 0.5 2) (>= -0.5 -0.5))
	(print 3.14 2e3 -0.25 100.0)
	(print (typeof 0.5))
	(print (+ "pi is roughly " 3.140625)))
