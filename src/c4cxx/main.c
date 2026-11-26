// main.c -- c4cxx's driver
//
//   c4cxx [-E] [-x c|c++] [-I dir] [-D name[=value]] [-U name] [-o out] file
//
// -E writes the preprocessed tokens. Without it (M1b on), the unit is
// parsed and written out as C. A .cpp, .cc or .cxx file is C++ (defines
// __cplusplus); -x says which.
#include "ast.h"

static void usage (void)
{
	fprintf (stderr, "usage: c4cxx [-E] [-x c|c++] [-I dir] [-D name[=value]] [-U name] [-o out] file\n");
	exit (2);
}

int main (int argc, char **argv)
{
	int i, opt_E = 0, lang = -1;
	char *out = 0, *in = 0;
	char **undefs = xalloc (sizeof (char *) * argc), **defs = xalloc (sizeof (char *) * argc);
	int nundef = 0, ndef = 0;
	FILE *o;
	Token *t;
	for (i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!strcmp (a, "-E"))
			opt_E = 1;
		else if (!strcmp (a, "-o") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp (a, "-x") && i + 1 < argc) {
			i++;
			lang = !strcmp (argv[i], "c++");
		} else if (!strncmp (a, "-I", 2))
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
	// -D and -U after the predefined names, as a compiler does: preprocess()
	// sets those up, so they are given to it through these
	pp_set_command_line (defs, ndef, undefs, nundef);
	t = preprocess (in);
	o = out ? fopen (out, "w") : stdout;
	if (!o)
		fatal ("cannot write %s", out);
	if (opt_E)
		print_tokens (o, t);
	else
		emit_unit (o, parse (t));
	if (out)
		fclose (o);
	return 0;
}
