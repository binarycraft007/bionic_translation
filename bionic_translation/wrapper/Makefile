WRAPPER_SRC := wrapper/wrapper.c

WRAPPER_HEADERS := wrapper/verbose.h \
                   wrapper/wrapper.h

WRAPPER_OUT := wrapper/wrapper.a

$(WRAPPER_OUT): private override CPPFLAGS += -D_GNU_SOURCE
$(WRAPPER_OUT): private override CFLAGS += -fvisibility=hidden
$(WRAPPER_OUT): $(WRAPPER_SRC) $(WRAPPER_HEADERS) 
