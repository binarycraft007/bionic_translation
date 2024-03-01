project('android_translation_layer', ['c', 'cpp'], default_options: ['b_lundef=false', 'b_asneeded=false'])

wrapper = static_library('wrapper', [
                                    	'wrapper/wrapper.c'
                                    ],
                                    c_args: [
                                    	'-fPIC',
                                    	'-D_GNU_SOURCE',
                                    	'-fvisibility=hidden'
                                    ])

shared_library('dl_bio', [
                         	'linker/config.c',
                         	'linker/dlfcn.c',
                         	'linker/linker.c',
                         	'linker/linker_environ.c',
                         	'linker/rt.c',
                         	'linker/strlcpy.c'
                         ],
                         version: '0.0.1', # this is not just a simple API shim, this is a shim dynamic linker with patches to aid us in our goals; it's possible, if unlikely, that the ABI will change
                         install: true,
                         dependencies: [
                         	dependency('dl'),
                         	dependency('egl'),
                         ],
                         link_with: [
                         	wrapper
                         ],
                         c_args: [
                         	'-fPIC',
                         	'-D_GNU_SOURCE',
                         	'-DLINKER_DEBUG=1',
                         	'-DVERBOSE_FUNCTIONS=1',
                         ],
                         link_args: [
                         	'-Wl,--no-as-needed',
                         	'-lpthread' # dependency('threads') doesn't express that we specifically need pthreads
                         ])

pthread_bio = shared_library('pthread_bio', [
                                            	'pthread_wrapper/libpthread.c'
                                            ],
                                            soversion: 0, # we're shimming a stable ABI; this should never need to change
                                            install: true,
                                            dependencies: [
                                            	dependency('dl'),
                                            ],
                                            c_args: [
                                            	'-fPIC',
                                            	'-D_GNU_SOURCE',
                                            ],
                                            link_args: [
                                            	'-Wl,--no-as-needed', # this really shouldn't be needed, since symbols from libpthread are used, but somehow it is
                                            	'-lpthread' # dependency('threads') doesn't express that we specifically need pthreads
                                            ])

# libc_bio.so - c_bio looks weird, but remember that 'lib' will be prepended automatically
shared_library('c_bio', [
                        	'libc/libc.c',
                        	'libc/libc-chk.c',
                        	'libc/libc-math.c',
                        	'libc/libc-misc.c',
                           	'libc/libc-stdio.c',
                           	'libc/libc-sha1.c',
                           	'libc/libc-antiantidebug.c',
                        ],
                        soversion: 0, # we're shimming a stable ABI; this should never need to change
                        install: true,
                        dependencies: [
                        	dependency('libbsd'), dependency('libunwind'),
                        ],
                        link_with: [
                        	wrapper,
                        	pthread_bio
                        ],
                        c_args: [
                        	'-fPIC',
                        	'-DPAGESIZE=4096',
                        	'-D_GNU_SOURCE'
                        ],
                        link_args: [
                        	'-Wl,-wrap,_IO_file_xsputn',
                        	'-Wl,--no-as-needed', # needed to put stuff in DT_NEEDED that we don't necessarily directly use (sadly this doesn't apply to the link_with libraries, since they are specified before this on the cmdline)
                        	pthread_bio.full_path() # as-needed is the default on some platforms, and the explicit no-as-needed above only applies to libraries specified after it
                        ])

shared_library('stdc++_bio', [
                             	'libstdc++_standalone/new.cpp',
                             	'libstdc++_standalone/__cxa_pure_virtual.cpp',
                             	'libstdc++_standalone/__cxa_guard.cpp'
                             ],
                             soversion: 0, # this tiny library's stable ABI will definitely never change, it's deprecated; this should never need to change
                             install: true,
                             include_directories: [
                             	'libstdc++_standalone/include/'
                             ],
                             cpp_args: [
                             	'-fPIC',
                             	'--std=c++23'
                             ])

install_data([
             	'cfg.d/bionic_translation.cfg'
             ],
             install_dir: get_option('datadir') / 'bionic_translation/cfg.d')
