/* radare - LGPL - Copyright 2013-2019 - pancake */

#include <r_bin.h>
#include "../i/private.h"
#include "./cxx/demangle.h"

static inline bool is_cxx_symbol (const char *name) {
	r_return_val_if_fail (name, false);
	if (!strncmp (name, "_Z", 2)) {
		return true;
	}
	if (!strncmp (name, "__Z", 3)) {
		return true;
	}
	return false;
}

// TODO: deprecate
R_API bool r_bin_is_cxx (RBinFile *bf) {
	RListIter *iter;
	RBinImport *import;
	RBinObject *o = bf->o;
	// XXX this is too slow
	r_list_foreach (o->imports, iter, import) {
		if (is_cxx_symbol (import->name)) {
			return true;
		}
	}
	return false;
}

R_API char *r_bin_demangle_cxx(RBinFile *bf, const char *str, ut64 vaddr) {
	// DMGL_TYPES | DMGL_PARAMS | DMGL_ANSI | DMGL_VERBOSE
	// | DMGL_RET_POSTFIX | DMGL_TYPES;
	int i;
#if WITH_GPL
	int flags = DMGL_NO_OPTS | DMGL_PARAMS;
#endif
	const char *prefixes[] = {
		"__symbol_stub1_",
		"reloc.",
		"sym.imp.",
		"imp.",
		NULL
	};
	char *tmpstr = strdup (str);
	char *p = tmpstr;

	if (p[0] == p[1] && *p == '_') {
		p++;
	}
	for (i = 0; prefixes[i]; i++) {
		int plen = strlen (prefixes[i]);
		if (!strncmp (p, prefixes[i], plen)) {
			p += plen;
			break;
		}
	}
	// remove CXXABI suffix
	char *cxxabi = strstr (p, "@@CXXABI");
	char *glibcxx = strstr (p, "@GLIBCXX");
	if (cxxabi) {
		*cxxabi = '\0';
	} else if (glibcxx) {
		if (p < glibcxx && glibcxx[-1] == '@') {
			glibcxx[-1] = '\0';
		} else {
			*glibcxx = '\0';
		}
	}
#if WITH_GPL
	char *out = cplus_demangle_v3 (p, flags);
#else
	/* TODO: implement a non-gpl alternative to c++v3 demangler */
	char *out = NULL;
#endif
	free (tmpstr);
	if (out) {
		r_str_replace_char (out, ' ', 0);
		char *sign = (char *)strchr (out, '(');
		if (sign) {
			char *str = out;
			char *ptr = NULL;
			char *nerd = NULL;
			for (;;) {
				ptr = strstr (str, "::");
				if (!ptr || ptr > sign) {
					break;
				}
				nerd = ptr;
				str = ptr + 1;
			}
			if (nerd && *nerd) {
				*nerd = 0;
				if (bf) {
					RBinSymbol *sym = r_bin_file_add_method (bf, out, nerd + 2, 0);
					if (sym) {
						if (sym->vaddr != 0 && sym->vaddr != vaddr) {
							if (bf && bf->rbin && bf->rbin->verbose) {
								eprintf ("Dupped method found: %s\n", sym->name);
							}
						}
						if (sym->vaddr == 0) {
							sym->vaddr = vaddr;
						}
					}
				}
				*nerd = ':';
			}
		}
	}
	return out;
}
