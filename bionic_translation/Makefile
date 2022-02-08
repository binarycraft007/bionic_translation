#FIXME: automagically use the same flags as dalvik

default: all

libdl_bio.so: linker/wrapper/wrapper.c linker/dlfcn.c linker/linker.c linker/linker_environ.c linker/rt.c linker/strlcpy.c
	gcc -m32 -g -fPIC -c -D_GNU_SOURCE -fvisibility=hidden linker/wrapper/wrapper.c -o wrapper.a 
	gcc -m32 -g -fPIC -DANDROID_X86_LINKER -D_GNU_SOURCE -DLINKER_DEBUG=1 -DVERBOSE_FUNCTIONS=1 -Wno-pedantic -Wno-variadic-macros -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -ldl -lpthread -shared -o libdl_bio.so wrapper.a linker/dlfcn.c linker/linker.c linker/linker_environ.c linker/rt.c linker/strlcpy.c

libpthread_bio.so: pthread_wrapper/libpthread.c
	gcc -m32 -g -fPIC -D_GNU_SOURCE -ldl -lpthread -shared -o libpthread_bio.so pthread_wrapper/libpthread.c

all: libdl_bio.so libpthread_bio.so

clean:
	rm libpthread_bio.so || true
	rm libdl_bio.so || true
	rm wrapper.a || true

