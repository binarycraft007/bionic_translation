LIBC_SRC := libc/libc.c \
            libc/libc-stdio.c \
            libc/libc-sha1.c \
            libc/libc-antiantidebug.c

LIBC_HEADERS := libc/libc-ctype.h \
                libc/libc-sysconf.h \
                libc/libc-verbose.h

libc.so: private override CPPFLAGS += -D_GNU_SOURCE
libc.so: private override LDFLAGS += -Wl,-wrap,_IO_file_xsputn
libc.so: private override CFLAGS += -Wno-deprecated-declarations
libc.so: private override LDLIBS += `pkg-config --libs libbsd libunwind`
libc.so: $(WRAPPER_OUT) $(LIBC_SRC) $(LIBC_HEADERS) 
