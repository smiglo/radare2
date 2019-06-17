/* radare - LGPL - Copyright 2012-2019 - pancake */

#include <stdio.h>
#include <string.h>
#include <r_main.h>
#include <r_util.h>

static RMain foo[] = {
	{ "r2", r_main_radare2 },
	{ "rax2", r_main_rax2 },
	{ "radiff2", r_main_radiff2 },
	{ "rafind2", r_main_rafind2 },
	{ "rarun2", r_main_rarun2 },
	{ "rasm2", r_main_rasm2 },
	{ "ragg2", r_main_ragg2 },
	{ "rabin2", r_main_rabin2 },
	{ "radare2", r_main_radare2 },
	{ NULL, NULL }
};

R_API RMain *r_main_new (const char *name) {
	int i = 0;
	while (foo[i].name) {
		if (!strcmp (name, foo[i].name)) {
			RMain *m = R_NEW0 (RMain);
			if (m) {
				m->name = foo[i].name;
				m->main = foo[i].main;
			}
			return m;
		}
		i++;
	}
	return NULL;
}
#if 0
	// list
	for (i = 0; foo[i].name; i++) {
		printf ("%s\n", foo[i].name);
	}
#endif

R_API void r_main_free(RMain *m) {
	free (m->name);
	free (m);
}

R_API int r_main_run(RMain *m, int argc, char **argv) {
	r_return_val_if_fail (m && m->main, -1);
	return m->main (argc, argv);
}
