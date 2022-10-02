PTHREAD_SRC := pthread_wrapper/libpthread.c

libpthread_bio.so: private override CPPFLAGS += -D_GNU_SOURCE
libpthread_bio.so: private override LDLIBS += -ldl -lpthread
libpthread_bio.so: $(PTHREAD_SRC) 
