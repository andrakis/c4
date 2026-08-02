// c4sp: the recursive evaluator
//
// alisp's SimpleEval, cons-based. The while(1) loop replaces the C++
// 'goto recurse': tail positions (if branches, final begin expression,
// lambda bodies, next) reassign x/env and loop instead of recursing, so
// tail calls cost no C4 stack. Non-tail positions (arguments, define/set!
// values, macro expansions) recurse.
//
// This is the reference implementation the CEK machine (design 6.2/6.3)
// will be diffed against; it stays alive as the oracle afterwards.
//
// Only the atom 'false' is falsy in an if: nil, 0 and everything else take
// the consequent branch, exactly as alisp.

int *eval (int *x, int *env) {
	int *sym, *proc, *exps, *tail, *e, *body;
	int  id, isnext, t;

	while (1) {
		if (c4sp_err) return 0;
		t = cell_type(x);
		if (t == T_ATOM) return env_get_named(env, x[CELL_A]);
		if (t == T_INT || t == T_FLOAT || t == T_STRING) return x;
		if (t != T_CONS) return x; // nil evaluates to nil; other cells to themselves

		isnext = 0;
		sym = (int *)x[CELL_A];
		if (cell_type(sym) == T_ATOM) {
			id = sym[CELL_A];
			if (id == A_QUOTE) {          // (quote exp)
				return list_index(x, 1);
			} else if (id == A_IF) {      // (if test conseq [alt])
				proc = eval(list_index(x, 1), env);
				if (c4sp_err) return 0;
				x = cell_is_false(proc) ? list_index(x, 3) : list_index(x, 2);
				continue;
			} else if (id == A_DEFINE) {  // (define var exp)
				sym = list_index(x, 1);
				if (cell_type(sym) != T_ATOM) { c4sp_error("define needs a symbol"); return 0; }
				proc = eval(list_index(x, 2), env);
				if (c4sp_err) return 0;
				return env_define(env, sym[CELL_A], proc);
			} else if (id == A_SET) {     // (set! var exp)
				sym = list_index(x, 1);
				if (cell_type(sym) != T_ATOM) { c4sp_error("set! needs a symbol"); return 0; }
				proc = eval(list_index(x, 2), env);
				if (c4sp_err) return 0;
				return env_set_named(env, sym[CELL_A], proc);
			} else if (id == A_LAMBDA) {  // (lambda (var*) exp)
				return mk_closure(T_LAMBDA, list_index(x, 1), list_index(x, 2), env);
			} else if (id == A_MACRO) {   // (macro (var*) exp)
				return mk_closure(T_MACRO, list_index(x, 1), list_index(x, 2), env);
			} else if (id == A_FASTMACRO) { // (fastmacro (var*) exp)
				// A fastmacro owns a persistent child of its declaration
				// environment, recaptured on every call.
				return mk_closure(T_FASTMACRO, list_index(x, 1), list_index(x, 2), mk_env(env));
			} else if (id == A_BEGIN) {   // (begin exp*)
				tail = (int *)x[CELL_B];
				if (!tail) return 0;
				while ((int *)tail[CELL_B]) {
					eval((int *)tail[CELL_A], env);
					if (c4sp_err) return 0;
					tail = (int *)tail[CELL_B];
				}
				x = (int *)tail[CELL_A];
				continue;
			} else if (id == A_NEXT) {    // (next fun args*): reuse this env
				isnext = 1;
				x = (int *)x[CELL_B];
				if (!x) return 0;
			}
		}

		// (proc exp*)
		proc = eval((int *)x[CELL_A], env);
		if (c4sp_err) return 0;
		t = cell_type(proc);
		if (t == T_MACRO || t == T_FASTMACRO) {
			// Macros receive their arguments unevaluated
			exps = (int *)x[CELL_B];
		} else {
			// Evaluate arguments left to right into a fresh list
			exps = tail = 0;
			sym = (int *)x[CELL_B];
			while (cell_type(sym) == T_CONS) {
				e = cons(eval((int *)sym[CELL_A], env), 0);
				if (c4sp_err) return 0;
				if (tail) { tail[CELL_B] = (int)e; tail = e; }
				else exps = tail = e;
				sym = (int *)sym[CELL_B];
			}
		}

		if (t == T_LAMBDA) {
			body = (int *)proc[CELL_B];
			if (isnext) {
				// next: rebind the parameters in the current environment
				env_bind(env, (int *)proc[CELL_A], exps);
			} else {
				e = mk_env((int *)proc[CELL_C]);
				env_bind(e, (int *)proc[CELL_A], exps);
				env = e;
			}
			x = body;
			continue;
		}
		if (t == T_MACRO) {
			// Expand in a fresh environment over the macro's closure, then
			// evaluate the expansion in the caller's environment.
			e = mk_env((int *)proc[CELL_C]);
			env_bind(e, (int *)proc[CELL_A], exps);
			x = eval((int *)proc[CELL_B], e);
			if (c4sp_err) return 0;
			continue;
		}
		if (t == T_FASTMACRO) {
			// Recapture into the macro's own persistent environment
			e = (int *)proc[CELL_C];
			env_bind(e, (int *)proc[CELL_A], exps);
			x = eval((int *)proc[CELL_B], e);
			if (c4sp_err) return 0;
			continue;
		}
		if (t == T_PROC || t == T_PROCENV)
			return builtin_call(proc[CELL_A], exps, env);

		c4sp_error("not an executable cell");
		return 0;
	}
}
