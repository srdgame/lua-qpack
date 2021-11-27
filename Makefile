##### Available defines for QPACK_CFLAGS #####
##
## USE_INTERNAL_ISINF:      Workaround for Solaris platforms missing isinf().
## DISABLE_INVALID_NUMBERS: Permanently disable invalid JSON numbers:
##                          NaN, Infinity, hex.
##
## Optional built-in number conversion uses the following defines:
## USE_INTERNAL_FPCONV:     Use builtin strtod/dtoa for numeric conversions.
## IEEE_BIG_ENDIAN:         Required on big endian architectures.
## MULTIPLE_THREADS:        Must be set when Lua QPACK may be used in a
##                          multi-threaded application. Requries _pthreads_.

##### Build defaults #####
LUA_VERSION =       5.3
TARGET =            qpack.so
PREFIX =            /usr
#CFLAGS =            -g -Wall -pedantic -fno-inline
CFLAGS =            -O3 -Wall -pedantic -DNDEBUG
QPACK_CFLAGS =      -fpic
QPACK_LDFLAGS =     -shared
LUA_INCLUDE_DIR =   $(PREFIX)/include/lua5.3
LUA_CMODULE_DIR =   $(PREFIX)/lib/lua/$(LUA_VERSION)
LUA_MODULE_DIR =    $(PREFIX)/share/lua/$(LUA_VERSION)
LUA_BIN_DIR =       $(PREFIX)/bin

CC= gcc
AR= gcc -o

##### Platform overrides #####
##
## Tweak one of the platform sections below to suit your situation.
##
## See http://lua-users.org/wiki/BuildingModules for further platform
## specific details.

## Linux

## FreeBSD
#LUA_INCLUDE_DIR =   $(PREFIX)/include/lua51

## MacOSX (Macports)
#PREFIX =            /opt/local
#QPACK_LDFLAGS =     -bundle -undefined dynamic_lookup

## Solaris
#PREFIX =            /home/user/opt
#CC =                gcc
#QPACK_CFLAGS =      -fpic -DUSE_INTERNAL_ISINF

## Windows (MinGW)
#TARGET =            qpack.dll
#PREFIX =            /home/user/opt
#QPACK_CFLAGS =      -DDISABLE_INVALID_NUMBERS
#QPACK_LDFLAGS =     -shared -L$(PREFIX)/lib -llua51
#LUA_BIN_SUFFIX =    .lua

##### End customisable sections #####

DATAPERM =          644
EXECPERM =          755

BUILD_CFLAGS =      -I$(LUA_INCLUDE_DIR) -I. $(QPACK_CFLAGS)
OBJS =              lua_qpack.o qpack/qpack.o 

.PHONY: all clean install install-extra doc

.c.o:
	$(CC) -c $(CFLAGS) $(CPPFLAGS) $(BUILD_CFLAGS) -o $@ $<

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) $@ $(LDFLAGS) $(QPACK_LDFLAGS) $(OBJS)

install: $(TARGET)
	mkdir -p $(DESTDIR)/$(LUA_CMODULE_DIR)
	cp $(TARGET) $(DESTDIR)/$(LUA_CMODULE_DIR)
	chmod $(EXECPERM) $(DESTDIR)/$(LUA_CMODULE_DIR)/$(TARGET)

clean:
	rm -f *.o $(TARGET)
