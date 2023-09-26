### a set of libraries for loading bionic-linked .so files on musl/glibc

the bionic linker under `bionic_translation/linker/` is taken from https://github.com/Cloudef/android2gnulinux and partly modified for our purposes  
the pthread wrapper under `bionic_translation/pthread_wrapper/` is taken from the same place, and augmented with additional missing wrapper functions  
same with `bionic_translation/libc/`  
`bionic_translation/wrapper/` is a helper for these, also from android2gnulinux  
`bionic_translation/libstdc++_standalone` is taken from bionic sources and coerced to compile; it's just "a minimum implementation of libc++ functionality not provided by compiler",
and things break when it's not linked in and android libs try to call into it and instead end up in the glibc or llvm libc++ implementations  
