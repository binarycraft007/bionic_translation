#FIXME: automagically use the same flags as dalvik

all: 
	gcc -m32 -g -c -D_GNU_SOURCE -fvisibility=hidden linker/wrapper/wrapper.c -o wrapper.a 
	gcc -m32 -g -DVERBOSE_FUNCTIONS -DANDROID_X86_LINKER -D_GNU_SOURCE -DLINKER_DEBUG=1 -DVERBOSE_FUNCTIONS=1 -Wno-pedantic -Wno-variadic-macros -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -ldl -lpthread -shared -o libdl_bio.so wrapper.a linker/dlfcn.c linker/linker.c linker/linker_environ.c linker/rt.c linker/strlcpy.c
clean:
	rm libdl_bio.so || true
	rm wrapper.a || true

