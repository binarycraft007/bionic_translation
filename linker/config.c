#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "config.h"

#define LIB_OVERRIDE_MAP_DEFAULT_SIZE 8
struct lib_override *lib_override_map = NULL;
size_t lib_override_map_len = 0;
static size_t lib_override_map_size = 0;

static void lib_override_map_append(char *from, char *to)
{
	if(!lib_override_map) {
		lib_override_map_size = LIB_OVERRIDE_MAP_DEFAULT_SIZE;
		lib_override_map = malloc(lib_override_map_size * sizeof(struct lib_override));
	}

	if(lib_override_map_len == lib_override_map_size) {
		lib_override_map_size *= 2;
		lib_override_map = realloc(lib_override_map, lib_override_map_size * sizeof(struct lib_override));
	}

	lib_override_map[lib_override_map_len].from = from;
	lib_override_map[lib_override_map_len].to = to;
	lib_override_map_len += 1;
}

static void process_cfg_line(char *line, size_t len, char *path, int linenum)
{
	char *from;
	char *to;
	int ret;

	// skip empty lines and comments
	if(line[0] == '#' || line[0] == '\n')
		return;

	ret = sscanf(line, "%ms %ms", &from, &to);
	if(ret != 2) {
		printf("error reading cfg: %s:%d\n", path, linenum);
		exit(1);
	}

	lib_override_map_append(from, to);
}

static void read_cfg_file(char *path)
{
	char *line = NULL;
	size_t line_len;
	int linenum = 1;

	FILE *cfg = fopen(path, "r");
	if(!cfg) {
		printf("failed to open %s (%m)\n", path);
		exit(1);
	}

	while(getline(&line, &line_len, cfg) > 0) {
		process_cfg_line(line, line_len, path, linenum++);
		free(line);
		line = NULL;
	}

	fclose(cfg);
}

void read_cfg_dir(char *cfg_dir_path)
{
	struct dirent *entry;

	DIR *cfg_dir = opendir(cfg_dir_path);
	if(!cfg_dir)
		return;

	while(entry = readdir(cfg_dir)) {
		if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;

		char *full_path = malloc(strlen(cfg_dir_path) + 1 + strlen(entry->d_name) + 1); // +1 for /, +1 for NUL
		sprintf(full_path, "%s/%s", cfg_dir_path, entry->d_name);
		read_cfg_file(full_path);
	}

}
