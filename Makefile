.PHONY: all clean

PLAT ?= linux
LUA_STATICLIB := lua-5.3.5/src/liblua.a
LUA_LIB ?= $(LUA_STATICLIB)
LUA_INC ?= lua-5.3.5

INCLUDE_DIR ?= $(LUA_INC)/src
CLUALIB_DIR ?= luaclib

CFLAGS = -std=c++0x -g3 -O2 -rdynamic -Wall -I$(INCLUDE_DIR)
CFLAGS += -DUSE_RDTSCP
SHARED = -fPIC --shared
LDFLAGS = #-lrt

all: $(LUA_STATICLIB) $(CLUALIB_DIR) $(CLUALIB_DIR)/profiler.so FlameGraph

$(LUA_STATICLIB):
	cd lua-5.3.5 && $(MAKE) CC='$(CC) -std=gnu99' $(PLAT)

$(CLUALIB_DIR):
	mkdir $(CLUALIB_DIR)
	
$(CLUALIB_DIR)/profiler.so: src/l_profiler.cpp src/core_profiler.cpp
	g++ $(CFLAGS) $(SHARED) -o $@ $^ $(LDFLAGS)
	
.PHONY: FlameGraph

FlameGraph:
	git submodule update --init

clean:
	rm -rf $(CLUALIB_DIR)/profiler.so
	cd lua-5.3.5 && $(MAKE) clean