LIBSTDCPP_SRC := libstdc++_standalone/new.cpp \
                 libstdc++_standalone/__cxa_pure_virtual.cpp \
                 libstdc++_standalone/__cxa_guard.cpp

LIBSTDCPP_HEADERS := libstdc++_standalone/include/new \
                     libstdc++_standalone/include/bionic_futex.h


libstdc++.so: private override CPPFLAGS += -I libstdc++_standalone/include/
libstdc++.so: private override CFLAGS += --std=c++23
libstdc++.so: private override CC = g++
libstdc++.so: $(LIBSTDCPP_SRC) $(LIBSTDCPP_HEADERS) 
