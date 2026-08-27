;; M0: a kernel with c4lc's actual instruction mix. Profiling
;; c4lc -O -c on src/c4ix/sched.c says 76% of applications are
;; builtins, led by head 20.7%, = 16.3%, tail 11.2%, empty? 10.4%,
;; + 6.8%, cons 1.3%. This walks a list doing exactly those.
;; One form per file, c4sp's rule.
(begin
	(define cons (lambda (x l) (+ (list x) l)))
	(define build (lambda (n acc)
		(if (= n 0) acc (next build (- n 1) (cons n acc)))))
	(define scan (lambda (l acc)
		(if (empty? l) acc
			(next scan (tail l) (if (= (head l) 7) (+ acc 1) (+ acc 0))))))
	(define runs (lambda (l k acc)
		(if (= k 0) acc (next runs l (- k 1) (+ acc (scan l 0))))))
	(define L (build 2000 (list)))
	(runs L 5000 0))
