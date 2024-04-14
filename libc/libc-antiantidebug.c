#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPEN_NEEDS_MODE(flags) ((flags & O_CREAT) || (flags & O_TMPFILE))

/* FIXME - move most of this out of this file */
static bool starts_with(const char *string, const char *substring)
{
	return !strncmp(string, substring, strlen(substring));
}

static bool apply_path_overrides(char **path) {
	bool free_path = 0;

	/* TODO: read the overrides from a config file */
	if(!strcmp(*path, "/system/etc/fonts.xml"))
		*path = "/etc/fonts.xml";

	if(starts_with(*path, "/fonts/")) {
		char *old_path = strchr(*path + 1, '/'); // after /fonts
		*path = malloc(sizeof("/usr/share/fonts/truetype") + strlen(old_path)); // sizeof includes the NUL byte
		sprintf(*path, "/usr/share/fonts/truetype%s", old_path);
		free_path = true;
	}

	if (starts_with(*path, "/system/") || starts_with(*path, "/data/")) {
		printf("%s: !!! app trying to access >%s<, which will certainly fail\n", __func__, *path);
		fflush(stdout);
	}

	return free_path;
}

int bionic_open(char *path, int oflag, ...)
{
	int fd;
	int mode;

	bool free_path = apply_path_overrides(&path);

	// Hide TracerPid from /proc/self/status for hideous apps that check for debugger.
	// Note, since /proc/self/status doesn't get updated anymore, this may break some stuff.
	// XXX: Turn this ON/OFF with env var maybe?
	if (!strcmp(path, "/proc/self/status")) {
		static FILE *faked = NULL;

		if (!faked) {
			static char status[4096];

			{
				FILE *f = fopen(path, "rb");
				assert(f && "/proc/self/status failed to open :/");
				const size_t ret = fread(status, 1, sizeof(status), f);
				assert(ret <= sizeof(status) && "/proc/self/status doesn't fit in 4096 bytes :/");
				fclose(f);
			}

			for (char *s, *e; (s = strstr(status, "TracerPid:\t"));) {
				for (e = s; (size_t)(e - status) < sizeof(status) && *e && *e != '\n'; ++e)
					;
				memmove(s, e, sizeof(status) - (e - status));
				break;
			}

			faked = fmemopen(status, sizeof(status), "rb");
			assert(faked && "fmemopen failed :/");
		}

		return fileno(faked);
	}

	if(OPEN_NEEDS_MODE(oflag)) {
		va_list arg;
		va_start(arg, oflag);
		mode = va_arg(arg, int);
		va_end(arg);

		fd = open(path, oflag, mode);
	} else {
		fd = open(path, oflag);
	}

	if(free_path)
		free(path);

	return fd;
}

FILE *bionic_fopen(char *path, const char *restrict mode)
{
	FILE *file;
	bool free_path = apply_path_overrides(&path);

	file = fopen(path, mode);
	if(free_path)
		free(path);
	return file;
}
