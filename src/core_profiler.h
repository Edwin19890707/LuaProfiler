#pragma once

#include "lua.hpp"

int ProfilerStart(lua_State *L);
int ProfilerDump(lua_State *L);
int CoroutineCreate(lua_State *L);
int RecordSave(lua_State *L);