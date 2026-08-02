// c4sp: the CEK machine
//
// The defunctionalized-continuation evaluator from design 6.2: machine
// state is control (x), env, kont and a value register, and the loop
// alternates two modes -- EVAL decomposes an expression, RETURN consumes a
// value by popping a kont frame. Native stack depth is O(1) no matter how
// deeply the Lisp program nests: what the recursive evaluator kept in C4
// frames lives in kont frames in the arena, where the collector already
// knows how to find it (and where saving three registers suspends the
// whole computation, which is what C4KE will eventually want).
//
// Frames are immutable: advancing a K_ARG or K_BEGIN pops it and pushes a
// fresh one, so capturing a continuation later is copying a pointer.
//
// eval() in eval.h is the reference implementation this was converted
// from; make test-c4sp diffs the two across the whole corpus. Their
// observable behaviour must stay identical, down to the order environments
// are created in (seval's (#env N) trace checks that for free).

enum { CEK_EVAL, CEK_RETURN };

int *eval_cek (int *x, int *env) {
	int *kont, *value, *frame, *nf, *sym, *proc, *vals, *remaining, *e, *args, *p;
	int  mode, id, t, pt, isnext;

	kont = 0;
	value = 0;
	mode = CEK_EVAL;

	while (1) {
		if (c4sp_err) return 0;

		if (mode == CEK_EVAL) {
			t = cell_type(x);
			if (t == T_ATOM) { value = env_get_named(env, x[CELL_A]); mode = CEK_RETURN; continue; }
			if (t == T_INT || t == T_FLOAT || t == T_STRING) { value = x; mode = CEK_RETURN; continue; }
			if (t != T_CONS) { value = x; mode = CEK_RETURN; continue; }

			sym = (int *)x[CELL_A];
			if (cell_type(sym) == T_ATOM) {
				id = sym[CELL_A];
				if (id == A_QUOTE) {
					value = list_index(x, 1);
					mode = CEK_RETURN;
					continue;
				}
				if (id == A_IF) {         // descend into the test
					frame = cell_new(T_KIF);
					frame[CELL_A] = (int)cdr(cdr(x));
					frame[CELL_B] = (int)env;
					kont = cons(frame, kont);
					x = list_index(x, 1);
					continue;
				}
				if (id == A_DEFINE || id == A_SET) { // descend into the value
					sym = list_index(x, 1);
					if (cell_type(sym) != T_ATOM) {
						c4sp_error(id == A_DEFINE ? "define needs a symbol" : "set! needs a symbol");
						return 0;
					}
					frame = cell_new(id == A_DEFINE ? T_KDEF : T_KSET);
					frame[CELL_A] = (int)sym;
					frame[CELL_B] = (int)env;
					kont = cons(frame, kont);
					x = list_index(x, 2);
					continue;
				}
				if (id == A_LAMBDA) {
					value = mk_closure(T_LAMBDA, list_index(x, 1), list_index(x, 2), env);
					mode = CEK_RETURN;
					continue;
				}
				if (id == A_MACRO) {
					value = mk_closure(T_MACRO, list_index(x, 1), list_index(x, 2), env);
					mode = CEK_RETURN;
					continue;
				}
				if (id == A_FASTMACRO) {
					value = mk_closure(T_FASTMACRO, list_index(x, 1), list_index(x, 2), mk_env(env));
					mode = CEK_RETURN;
					continue;
				}
				if (id == A_BEGIN) {
					p = cdr(x);
					if (!p) { value = 0; mode = CEK_RETURN; continue; }
					if (!cdr(p)) { x = car(p); continue; } // tail: no frame
					frame = cell_new(T_KBEGIN);
					frame[CELL_A] = (int)cdr(p);
					frame[CELL_B] = (int)env;
					kont = cons(frame, kont);
					x = car(p);
					continue;
				}
				if (id == A_NEXT) {       // (next f args*): K_ARGN flavour
					p = cdr(x);
					if (!p) { value = 0; mode = CEK_RETURN; continue; }
					frame = cell_new(T_KARGN);
					frame[CELL_B] = (int)p;
					frame[CELL_C] = (int)env;
					kont = cons(frame, kont);
					x = car(p);
					continue;
				}
			}
			// (proc exp*): evaluate the proc position first
			frame = cell_new(T_KARG);
			frame[CELL_B] = (int)x;
			frame[CELL_C] = (int)env;
			kont = cons(frame, kont);
			x = (int *)x[CELL_A];
			continue;
		}

		// CEK_RETURN: value holds a finished result
		if (!kont) return value;
		frame = (int *)kont[CELL_A];
		t = frame[CELL_TYPE];

		if (t == T_KIF) {
			kont = (int *)kont[CELL_B];
			env = (int *)frame[CELL_B];
			p = (int *)frame[CELL_A];
			x = cell_is_false(value) ? list_index(p, 1) : list_index(p, 0);
			mode = CEK_EVAL;
			continue;
		}
		if (t == T_KDEF) {
			kont = (int *)kont[CELL_B];
			env_define((int *)frame[CELL_B], ((int *)frame[CELL_A])[CELL_A], value);
			continue; // value passes through, still RETURN
		}
		if (t == T_KSET) {
			kont = (int *)kont[CELL_B];
			env_set_named((int *)frame[CELL_B], ((int *)frame[CELL_A])[CELL_A], value);
			continue;
		}
		if (t == T_KBEGIN) {
			kont = (int *)kont[CELL_B];
			remaining = (int *)frame[CELL_A];
			env = (int *)frame[CELL_B];
			if (!cdr(remaining)) { x = car(remaining); mode = CEK_EVAL; continue; } // tail
			nf = cell_new(T_KBEGIN);
			nf[CELL_A] = (int)cdr(remaining);
			nf[CELL_B] = (int)env;
			kont = cons(nf, kont);
			x = car(remaining);
			mode = CEK_EVAL;
			continue;
		}
		if (t == T_KMACRO) {
			// The expansion becomes the new expression, evaluated in the
			// environment of the call site
			kont = (int *)kont[CELL_B];
			env = (int *)frame[CELL_A];
			x = value;
			mode = CEK_EVAL;
			continue;
		}
		if (t == T_KARG || t == T_KARGN) {
			vals = (int *)frame[CELL_A];
			remaining = (int *)frame[CELL_B];

			if (!vals) {
				// value is the evaluated proc; macros take their
				// arguments unevaluated, so apply them right here
				pt = cell_type(value);
				if (pt == T_MACRO || pt == T_FASTMACRO) {
					proc = value;
					kont = (int *)kont[CELL_B];
					if (pt == T_MACRO) {
						e = mk_env((int *)proc[CELL_C]);
						env_bind(e, (int *)proc[CELL_A], cdr(remaining));
					} else {
						e = (int *)proc[CELL_C];
						env_bind(e, (int *)proc[CELL_A], cdr(remaining));
					}
					nf = cell_new(T_KMACRO);
					nf[CELL_A] = frame[CELL_C]; // caller env
					kont = cons(nf, kont);
					env = e;
					x = (int *)proc[CELL_B];
					mode = CEK_EVAL;
					continue;
				}
			}

			vals = cons(value, vals);
			p = cdr(remaining);
			if (p) {
				// More to evaluate: replace the frame and descend
				kont = (int *)kont[CELL_B];
				nf = cell_new(t);
				nf[CELL_A] = (int)vals;
				nf[CELL_B] = (int)p;
				nf[CELL_C] = frame[CELL_C];
				kont = cons(nf, kont);
				x = (int *)p[CELL_A];
				env = (int *)frame[CELL_C];
				mode = CEK_EVAL;
				continue;
			}

			// Everything evaluated. vals is (lastarg ... firstarg proc);
			// unreverse into proc and an argument list.
			kont = (int *)kont[CELL_B];
			isnext = (t == T_KARGN);
			args = 0;
			while (cdr(vals)) {
				args = cons(car(vals), args);
				vals = cdr(vals);
			}
			proc = car(vals);
			e = (int *)frame[CELL_C]; // the caller's environment

			pt = cell_type(proc);
			if (pt == T_LAMBDA) {
				if (isnext) {
					// next: rebind the parameters in the caller's env
					env_bind(e, (int *)proc[CELL_A], args);
					env = e;
				} else {
					env = mk_env((int *)proc[CELL_C]);
					env_bind(env, (int *)proc[CELL_A], args);
				}
				x = (int *)proc[CELL_B];
				mode = CEK_EVAL; // frames already popped: a real tail call
				continue;
			}
			if (pt == T_PROC || pt == T_PROCENV) {
				value = builtin_call(proc[CELL_A], args, e);
				if (c4sp_err) return 0;
				mode = CEK_RETURN;
				continue;
			}
			c4sp_error("not an executable cell");
			return 0;
		}

		c4sp_error("corrupt continuation");
		return 0;
	}
}
