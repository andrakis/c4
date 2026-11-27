// main.c -- c4cxx's driver
//
//   c4cxx [-E | -parse | -check-types] [-x c|c++] [-I dir] [-isystem dir] [-D name[=value]]
//         [-U name] [-o out] file
//
// -E writes the preprocessed tokens. Without it (M1b on), the unit is
// parsed, typed (sema.c) and written out as C. -check-types adds
// _Static_asserts of every expression's type, every struct's layout and every
// global's size, for gcc -m32 to confirm (test-types.sh). -isystem is -I for
// headers of C: in C++, their functions keep their C names. -parse stops after
// parsing (to see that a unit parses). A .cpp, .cc or .cxx file is C++ (defines
// __cplusplus); -x says which.
#include "ast.h"

char *opt_unit;
int opt_short_enums, opt_ms_bitfields, opt_unsigned_char;

static void usage (void)
{
	fprintf (stderr, "usage: c4cxx [-E] [-x c|c++] [-I dir] [-D name[=value]] [-U name] [-o out] file\n");
	exit (2);
}

int main (int argc, char **argv)
{
	int i, opt_E = 0, opt_parse = 0, lang = -1;
	char *out = 0, *in = 0;
	char **undefs = xalloc (sizeof (char *) * argc), **defs = xalloc (sizeof (char *) * argc);
	int nundef = 0, ndef = 0;
	FILE *o;
	Token *t;
	for (i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strcmp (a, "-E"))
			opt_E = 1;
		else if (!strcmp (a, "-parse"))
			opt_parse = 1;
		else if (!strcmp (a, "-check-types"))
			emit_check = 1;
		else if (!strcmp (a, "-fshort-enums"))
			opt_short_enums = 1;
		else if (!strcmp (a, "-mms-bitfields"))
			opt_ms_bitfields = 1;
		else if (!strcmp (a, "-funsigned-char"))
			opt_unsigned_char = 1;
		else if (!strcmp (a, "-fargs-rtl"))
			opt_args_rtl = 1;
		else if (!strcmp (a, "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp (a, "-x") && i + 1 < argc) {
			i++;
			lang = !strcmp (argv[i], "c++");
		} else if (!strcmp (a, "-isystem") && i + 1 < argc)
			pp_add_c_include_path (argv[++i]);
		else if (!strncmp (a, "-I", 2))
			pp_add_include_path (a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : (usage (), "")));
		else if (!strncmp (a, "-D", 2))
			defs[ndef++] = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : (usage (), ""));
		else if (!strncmp (a, "-U", 2))
			undefs[nundef++] = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : (usage (), ""));
		else if (a[0] == '-' && a[1])
			usage ();
		else if (!in)
			in = a;
		else
			usage ();
	}
	if (!in)
		usage ();
	if (lang < 0) {
		const char *dot = strrchr (in, '.');
		lang = dot && (!strcmp (dot, ".cpp") || !strcmp (dot, ".cc") || !strcmp (dot, ".cxx"));
	}
	opt_cplusplus = lang;
	opt_unit = in;
	// -D and -U after the predefined names, as a compiler does: preprocess()
	// sets those up, so they are given to it through these
	pp_set_command_line (defs, ndef, undefs, nundef);
	t = preprocess (in);
	o = out ? fopen (out, "w") : stdout;
	if (!o)
		fatal ("cannot write %s", out);
	if (opt_E)
		print_tokens (o, t);
	else {
		Node *unit = parse (t);
		if (opt_parse)
			return 0;
		sema (unit);
		emit_unit (o, unit);
	}
	if (out)
		fclose (o);
	return 0;
}
