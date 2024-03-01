#ifndef CONFIG_H
#define CONFIG_H
struct lib_override {
	char *from;
	char *to;
};

extern struct lib_override *lib_override_map;
extern size_t lib_override_map_len;

void read_cfg_dir(char *cfg_dir_path);
#endif
