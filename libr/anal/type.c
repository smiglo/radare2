/* radare - LGPL - Copyright 2019 - pancake, oddcoder, Anton Kochkov */

#include <string.h>
#include <r_anal.h>
#include "sdb/sdb.h"

static char* is_type(char *type) {
	char *name = NULL;
	if ((name = strstr (type, "=type")) ||
		(name = strstr (type, "=struct")) ||
		(name = strstr (type, "=union")) ||
		(name = strstr (type, "=enum")) ||
		(name = strstr (type, "=typedef")) ||
		(name = strstr (type, "=func"))) {
		return name;
	} else return NULL;
}

/*!
 * \brief Save the size of the given datatype in sdb
 * \param sdb_types pointer to the sdb for types
 * \param name the datatype whose size if to be stored
 */
static void save_type_size(Sdb *sdb_types, char *name) {
	char *type = NULL;
	r_return_if_fail (sdb_types && name);
	if (!sdb_exists (sdb_types, name) || !(type = sdb_get (sdb_types, name, 0))) {
		return;
	}
	char *type_name_size = r_str_newf ("%s.%s.%s", type, name, "!size");
	r_return_if_fail (type_name_size);
	int size = r_type_get_bitsize (sdb_types, name);
	sdb_set (sdb_types, type_name_size, sdb_fmt ("%d", size), 0);
	free (type);
	free (type_name_size);
}

/*!
 * \brief Save the sizes of the datatypes which have been parsed
 * \param core pointer to radare2 core
 * \param parsed the parsed c string in sdb format
 */
static void save_parsed_type_size(RAnal *anal, const char *parsed) {
	r_return_if_fail (anal && parsed);
	char *str = strdup (parsed);
	if (str) {
		char *ptr = NULL;
		int offset = 0;
		while ((ptr = strstr (str + offset, "=struct\n")) ||
				(ptr = strstr (str + offset, "=union\n"))) {
			*ptr = 0;
			if (str + offset == ptr) {
				break;
			}
			char *name = ptr - 1;
			while (name > str && *name != '\n') {
				name--;
			}
			if (*name == '\n') {
				name++;
			}
			save_type_size (anal->sdb_types, name);
			*ptr = '=';
			offset = ptr + 1 - str;
		}
		free (str);
	}
}

R_API void r_anal_remove_parsed_type(RAnal *anal, const char *name) {
	r_return_if_fail (anal && name);
	Sdb *TDB = anal->sdb_types;
	SdbKv *kv;
	SdbListIter *iter;
	const char *type = sdb_const_get (TDB, name, 0);
	if (!type) {
		return;
	}
	int tmp_len = strlen (name) + strlen (type);
	char *tmp = malloc (tmp_len + 1);
	r_type_del (TDB, name);
	if (tmp) {
		snprintf (tmp, tmp_len + 1, "%s.%s.", type, name);
		SdbList *l = sdb_foreach_list (TDB, true);
		ls_foreach (l, iter, kv) {
			if (!strncmp (sdbkv_key (kv), tmp, tmp_len)) {
				r_type_del (TDB, sdbkv_key (kv));
			}
		}
		ls_free (l);
		free (tmp);
	}
}

R_API void r_anal_save_parsed_type(RAnal *anal, const char *parsed) {
	r_return_if_fail (anal && parsed);
	// First, if this exists, let's remove it.
	char *type = strdup (parsed);
	if (type) {
		char *name = is_type (type);
		if (name) {
			*name = 0;
			while (name - 1 >= type && *(name - 1) != '\n') {
				name--;
			}
		}
		if (name) {
			r_anal_remove_parsed_type (anal, name);
			// Now add the type to sdb.
			sdb_query_lines (anal->sdb_types, parsed);
			save_parsed_type_size (anal, parsed);
		}
		free (type);
	}
}

