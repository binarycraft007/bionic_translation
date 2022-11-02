#include <stdio.h>

void android_set_abort_message(const char* msg) {
	printf("android_set_abort_message called: '%s'\n", msg);
//	exit(1);
}
