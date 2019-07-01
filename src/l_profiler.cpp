#include <lua.hpp>

#include "core_profiler.h"

static int lstart(lua_State *L) {
	ProfilerStart(L);
	return 0;
}

static int ldump(lua_State *L) {
	ProfilerDump(L);
	return 0;
}

static int lcoroutine_create(lua_State *L) {
	CoroutineCreate(L);
	return 0;
}

static int lrecord_save(lua_State *L) {
	RecordSave(L);
	return 0;
}

extern "C"
int luaopen_profiler_c(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{"start", lstart},
		{"dump", ldump},
		{"coroutine_create", lcoroutine_create},
		{"record_save", lrecord_save},
		{NULL, NULL}
	};

	luaL_newlib(L, l);
	return 1;
}