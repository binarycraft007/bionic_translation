#include <errno.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// this seems to not be a stable ABI, so hopefully nobody treats it as such
// FIXME: move this out into it's own file whenever we get to implementing it more properly
struct prop_info {
	const char *name;
};

#define PROP_NAME_MAX  32
#define PROP_VALUE_MAX 92

const struct prop_info *bionic___system_property_find(const char *name)
{
	struct prop_info *ret = malloc(sizeof(struct prop_info));
	ret->name = name;

	return ret;
}

int bionic___system_property_read(const struct prop_info *prop_info, char *name, char *value)
{
	if (name)
		strncpy(name, prop_info->name, PROP_NAME_MAX);

	if (!value)
		return 0;

	if (!strcmp(prop_info->name, "ro.build.fingerprint")) {
		strncpy(value, "", PROP_VALUE_MAX); // there is no good reason that apps should need this, so just return an empty string
	} else {
		printf("__system_property_find: >%s< not handled yet\n", prop_info->name);
		strncpy(value, "", PROP_VALUE_MAX);
	}

	return strlen(value);
}

// actually misc stuff

void android_set_abort_message(const char *msg)
{
	printf("android_set_abort_message called: '%s'\n", msg);
	//	exit(1);
}

// setlocale is a bit special on bionic, try to mimic the behavior
char *bionic_setlocale(int category, const char *locale)
{
	if(!locale)
		return setlocale(category, NULL);

	// list of allowed locales from bionic
	if (!strcmp(locale, "") ||
	    !strcmp(locale, "C.UTF-8") ||
	    !strcmp(locale, "en_US.UTF-8")) {
		return setlocale(category, "C.UTF-8");
	} else if (!strcmp(locale, "C") ||
		   !strcmp(locale, "POSIX")) {
		return setlocale(category, "C");
	} else {
		errno = ENOENT;
		return NULL;
	}
}
