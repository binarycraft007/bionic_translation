DL_SRC := linker/bionic_tls.h \
          linker/dlfcn.h \
          linker/linker_debug.h \
          linker/linker_environ.h \
          linker/linker_format.h \
          linker/linker.h \
          linker/strlcpy.h

DL_HEADERS := linker/dlfcn.c \
         linker/linker.c \
         linker/linker_environ.c \
         linker/rt.c \
         linker/strlcpy.c

libdl_bio.so: private override CPPFLAGS += -D_GNU_SOURCE -DLINKER_DEBUG=1 -DVERBOSE_FUNCTIONS=1
libdl_bio.so: private WARNINGS += -Wno-pedantic -Wno-variadic-macros -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
libdl_bio.so: private override LDLIBS += -ldl -lpthread
libdl_bio.so: $(WRAPPER_OUT) $(DL_SRC) $(DL_HEADERS) 
