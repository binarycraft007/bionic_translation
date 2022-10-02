#FIXME: automagically compile for the same architecture as art

CFLAGS ?= -g -O2
CFLAGS += -fPIC -D__x86_64__

default: all

%.a:
	$(LINK.c) $(CFLAGS) -c $(filter %.c,$^) -o $@

%.so:
	$(LINK.c) $(CFLAGS) -shared $(filter %.c %.a,$^) $(LDLIBS) -o $@


include wrapper/Makefile

include libc/Makefile
include linker/Makefile
include pthread_wrapper/Makefile

all: libc.so libdl_bio.so libpthread_bio.so

clean:
	rm libc.so || true
	rm libdl_bio.so || true
	rm libpthread_bio.so || true
	rm $(WRAPPER_OUT) || true

