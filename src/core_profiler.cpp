#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unordered_map>
#include <map>
#include <set>
#include <algorithm>
#include <vector>

#include "core_profiler.h"
#include "stack.h"
#include "clocks.h"

using namespace std;

static int kProfilerStateId;
static const char *kLuaApiFilterList[] = {"next", "require", "assert", "error", "getmetatable", "setmetatable", 
										"ipairs", "pairs", "xpcall", "pcall", "rawequal", "rawget", "rawset", 
										"rawlen", "select", "tonumber", "tostring", "type", "for iterator", NULL};
static const size_t kMutiStackBufferInitCount = 10240;

struct FunctionInfo {
	string name_;
	string source_;
	int linedefined_;

	FunctionInfo(const char *_name, const char *_source, int _line)
		: name_(_name ? _name : "?")
		, linedefined_(_line) {
		if (_source) {
			if (_source[0] == '@' || _source[0] == '=') {
				source_ = _source;
			} else {
				source_ = "(string)";
			}
		}
	}
};

#pragma pack(1)
typedef struct RecordData {
	uint32_t call_count_;
	uint64_t inner_elapse_;

	RecordData(void) : call_count_(0), inner_elapse_(0) {}
} RecordData;
#pragma pack()

typedef MultiStackBuffer<RecordData> RecordBuffer;
typedef StaticBuffer<RecordData> RecordCopy;

struct Record {
	RecordBuffer &buffer_;
	FunctionInfo *func_info_;
	uint64_t temp_inner_elapse_;
	uint64_t temp_full_elapse_;
	uint32_t temp_call_count_;
	size_t index_;
	RecordData *data_;

	typedef map<FunctionInfo *, Record *> ChildrenFunctionMap;
	ChildrenFunctionMap children_;

	typedef vector<Record *> ChildrenList;
	ChildrenList children_list_;

	struct RecordSort {
		bool operator() (const Record *t1, const Record *t2) {
			return t1->temp_full_elapse_ > t2->temp_full_elapse_;
		}
	};

	Record(RecordBuffer &_buffer, FunctionInfo *_info) 
		: buffer_(_buffer) 
		, func_info_(_info)
		, temp_inner_elapse_(0)
		, temp_full_elapse_(0)
		, temp_call_count_(0) {
		const RecordBuffer::ElementPair& element = buffer_.Get();
		index_ = element.first;
		data_ = element.second;
	}

	~Record(void) {
		ChildrenList::const_iterator ibegin = children_list_.begin();
		ChildrenList::const_iterator iend = children_list_.end();
		for (; ibegin != iend; ++ibegin) {
			Record *record = *ibegin;
			delete record;
		}

		children_list_.clear();
		children_.clear();
	}

	inline void AddCount(void) {
		data_->call_count_++;
	}

	inline void AddInnerElapse(uint64_t elapse) {
		data_->inner_elapse_ += elapse;
	}

	inline Record *GetChildRecord(FunctionInfo *_info) {
		ChildrenFunctionMap::const_iterator citr = children_.find(_info);
		if (citr != children_.end()) {
			return citr->second;
		}

		Record *new_record = new Record(buffer_, _info);
		children_list_.push_back(new_record);
		children_.insert(make_pair(_info, new_record));
		return new_record;
	}

	uint64_t CalcChildrenElapse(void) {
		uint64_t total_children_elapse = 0;
		if (!children_list_.empty()) {
			ChildrenList::const_iterator ibegin = children_list_.begin();
			ChildrenList::const_iterator iend = children_list_.end();
			for (; ibegin != iend; ++ibegin) {
				Record *record = *ibegin;
				total_children_elapse += record->CalcChildrenElapse();
			}

			if (children_list_.size() > 1) {
				sort(children_list_.begin(), children_list_.end(), RecordSort());
			}
		}

		temp_inner_elapse_ = data_->inner_elapse_;
		temp_call_count_ = data_->call_count_;
		temp_full_elapse_ = temp_inner_elapse_ + total_children_elapse;

		return temp_full_elapse_;
	}

	uint64_t CalcChildrenElapse(const RecordCopy *_start_record, const RecordCopy *_end_record) {
		uint64_t total_children_elapse = 0;
		if (!children_list_.empty()) {
			ChildrenList::const_iterator ibegin = children_list_.begin();
			ChildrenList::const_iterator iend = children_list_.end();
			for (; ibegin != iend; ++ibegin) {
				Record *record = *ibegin;
				total_children_elapse += record->CalcChildrenElapse(_start_record, _end_record);
			}

			if (children_list_.size() > 1) {
				sort(children_list_.begin(), children_list_.end(), RecordSort());
			}
		}

		temp_inner_elapse_ = 0;
		temp_call_count_ = 0;

		const RecordData *start = NULL;
		if (_start_record) {
			start = _start_record->At(index_);
		}

		const RecordData *end = _end_record->At(index_);
		if (end) {
			if (start) {
				temp_inner_elapse_ = end->inner_elapse_ - start->inner_elapse_;
				temp_call_count_ = end->call_count_ - start->call_count_;
			} else {
				temp_inner_elapse_ = end->inner_elapse_;
				temp_call_count_ = end->call_count_;
			}
		}

		temp_full_elapse_ = temp_inner_elapse_ + total_children_elapse;

		return temp_full_elapse_;
	}
};

#pragma pack(1)
typedef struct CallInfo {
	const void *func_;
	Record *record_;
	uint64_t enter_time_;

	CallInfo(const void *_f, Record *_record)
		: func_(_f)
		, record_(_record)
		, enter_time_(0) { }

	inline Record *ChildCallEnter(uint64_t _time, FunctionInfo *_info) {
		record_->AddInnerElapse(_time - enter_time_);
		enter_time_ = 0;
		return record_->GetChildRecord(_info);
	}

	inline void ChildCallBack(uint64_t _time) {
		enter_time_ = _time;
	}

	inline void OnEnter(uint64_t _time) {
		enter_time_ = _time;
		record_->AddCount();
	}

	inline void OnExit(uint64_t _time) {
		if (enter_time_ != 0) {
			record_->AddInnerElapse(_time - enter_time_);
			enter_time_ = 0;
		}
	}

	inline void CoroutineJump(void) {
		record_->AddInnerElapse(GetTime() - enter_time_);
		enter_time_ = 0;
	}
} CallInfo;
#pragma pack()

class LuaProfilerState {
	typedef set<string> LuaFilterApiNameMap;
	typedef set<const void *> LuaFilterApiMap;
	
	typedef unordered_map<int, FunctionInfo *> FunctionInfoMap;
	typedef unordered_map<const void *, FunctionInfoMap *> LuaProfilerfuncsMap;
	typedef unordered_map<const void *, FunctionInfo *> CProfilerfuncsMap;

	typedef StackBuffer<CallInfo> CallInfoStack;
	typedef unordered_map<lua_State *, CallInfoStack *> CallInfoStackMap;

public:
	LuaProfilerState(void) 
		: record_buffer_(kMutiStackBufferInitCount)
		, root_profiler_record_(record_buffer_, NULL)
		, curr_lua_state_(NULL)
		, curr_call_info_(NULL)
		, curr_call_info_stack_(NULL) {}

	~LuaProfilerState(void) {
		lua_filter_api_.clear();

		for (CallInfoStackMap::const_iterator citr = call_info_stack_map_.begin();
			citr != call_info_stack_map_.end(); ++citr) {
			CallInfoStack *call_info_stack = citr->second;
			delete call_info_stack;
		}
		call_info_stack_map_.clear();

		for (LuaProfilerfuncsMap::const_iterator citr = lua_profiler_funcs_.begin();
			citr != lua_profiler_funcs_.end(); ++citr) {
			FunctionInfoMap *func_infos = citr->second;

			for (FunctionInfoMap::const_iterator citr2 = func_infos->begin();
				citr2 != func_infos->end(); ++citr2) {
				FunctionInfo *info = citr2->second;
				delete info;
			}

			func_infos->clear();
			delete func_infos;
		}
		lua_profiler_funcs_.clear();

		for (CProfilerfuncsMap::const_iterator citr = c_profiler_funcs_.begin();
			citr != c_profiler_funcs_.end(); ++citr) {
			FunctionInfo *info = citr->second;
			delete info;
		}
		c_profiler_funcs_.clear();
	}

	CallInfoStack *GetCallInfoStack(lua_State *L) {
		CallInfoStackMap::const_iterator citr = call_info_stack_map_.find(L);
		if (citr != call_info_stack_map_.end()) {
			return citr->second;
		}

		return NULL;
	}

	void CreateCallInfoStack(lua_State *L) {
		CallInfoStack *call_info_stack = GetCallInfoStack(L);
		if (call_info_stack) {
			call_info_stack->Clear();
		} else {
			CallInfoStack *new_call_info_stack = new CallInfoStack();
			call_info_stack_map_.insert(make_pair(L, new_call_info_stack));
		}
	}

	void Init(lua_State *L) {
		lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
		lua_State *main_L = lua_tothread(L, -1);
		lua_pop(L, 1);
		CreateCallInfoStack(main_L);

		const char **temp = kLuaApiFilterList;
		while (*temp) {
			lua_filter_api_name_.insert(*temp);
			temp++;
		}
	}

	FunctionInfo *GetFunctionInfo(lua_Debug *ar, const void *_f) {
		if (ar->what[0] == 'C') {
			CProfilerfuncsMap::const_iterator citr = c_profiler_funcs_.find(_f);
			if (citr != c_profiler_funcs_.end()) {
				return citr->second;
			}

			lua_getinfo(curr_lua_state_, "n", ar);

			FunctionInfo *new_func_info = new FunctionInfo(ar->name, ar->source, ar->linedefined);
			c_profiler_funcs_.insert(make_pair(_f, new_func_info));

			if (ar->name && lua_filter_api_name_.find(ar->name) != lua_filter_api_name_.end()) {
				lua_filter_api_.insert(_f);
				return NULL;
			}

			return new_func_info;
		} else {
			FunctionInfoMap *func_info_map = NULL;
			LuaProfilerfuncsMap::const_iterator citr = lua_profiler_funcs_.find((const void *)ar->source);
			if (citr != lua_profiler_funcs_.end()) {
				func_info_map = citr->second;

				FunctionInfoMap::const_iterator nitr = func_info_map->find(ar->linedefined);
				if (nitr != func_info_map->end()) {
					return nitr->second;
				}
			} else {
				func_info_map = new FunctionInfoMap();
				lua_profiler_funcs_.insert(make_pair((const void *)ar->source, func_info_map));
			}

			lua_getinfo(curr_lua_state_, "n", ar);

			FunctionInfo *new_func_info = new FunctionInfo(ar->name, ar->source, ar->linedefined);
			func_info_map->insert(make_pair(ar->linedefined, new_func_info));

			return new_func_info;
		}
	}

	void CallHookIn(lua_Debug *ar, const void *_f) {
		FunctionInfo *func_info = GetFunctionInfo(ar, _f);
		if (!func_info) return;

		uint64_t curr_time = GetTime();
		Record *record = NULL;
		if (curr_call_info_) {
			record = curr_call_info_->ChildCallEnter(curr_time, func_info);

			if (ar->event == LUA_HOOKTAILCALL) {
				curr_call_info_stack_->Pop();
			}
		} else {
			record = root_profiler_record_.GetChildRecord(func_info);
		}

		curr_call_info_ = curr_call_info_stack_->Get(_f, record);
		curr_call_info_->OnEnter(curr_time);
	}

	void CallHookOut(const void *_f) {
		if (!curr_call_info_) {
			return;
		}

		if (curr_call_info_->func_ != _f) {
			do {
				curr_call_info_stack_->Pop();
				if (curr_call_info_stack_->Empty()) {
					curr_call_info_ = NULL;
					return;
				}

				curr_call_info_ = curr_call_info_stack_->Top();
			} while (curr_call_info_->func_ != _f);
		}

		uint64_t curr_time = GetTime();
		curr_call_info_->OnExit(curr_time);
		curr_call_info_stack_->Pop();

		if (curr_call_info_stack_->Empty()) {
			curr_call_info_ = NULL;
		} else {
			curr_call_info_ = curr_call_info_stack_->Top();
			curr_call_info_->ChildCallBack(curr_time);
		}
	}

	int Hook(lua_State *L, lua_Debug *ar) {
		if (curr_lua_state_ != L) {
			if (curr_call_info_) {
				curr_call_info_->CoroutineJump();
				curr_call_info_ = NULL;
			}

			curr_lua_state_ = L;
			curr_call_info_stack_ = GetCallInfoStack(L);
			if (!curr_call_info_stack_) {
				// error long jump
				return luaL_error(L, "profiler lua_State[%p] stack not find", L);
			} else {
				if (!curr_call_info_stack_->Empty()) {
					curr_call_info_ = curr_call_info_stack_->Top();
				}
			}
		}

		if (!curr_call_info_stack_) {
			return 0;
		}

		if (ar->event == LUA_HOOKRET) {
			lua_getinfo(L, "f", ar);
		} else {
			lua_getinfo(L, "Sf", ar);
		}

		const void *f = lua_topointer(L, -1);
		lua_pop(L, 1);

		if (lua_filter_api_.find(f) != lua_filter_api_.end()) {
			return 0;
		}

		if (ar->event == LUA_HOOKRET) {
			CallHookOut(f);
		} else {
			CallHookIn(ar, f);
		}

		return 0;
	}

	void Save(void) {
		record_buffer_.Save();
	}

	uint64_t CalcRecord(lua_State *L) {
		uint64_t temp_full_elapse = 0;
		int n = lua_gettop(L);
		if (n == 1) {
			temp_full_elapse = root_profiler_record_.CalcChildrenElapse();
		} else if (n == 3) {
			int start_index = (int)luaL_checknumber(L, 2);
			if (start_index >= (int)record_buffer_.GetRecordCount()) {
				// error long jump
				return luaL_error(L, "profiler dump start index error");
			}

			int end_index = (int)luaL_checknumber(L, 3);
			if (end_index < 0 || end_index >= (int)record_buffer_.GetRecordCount()) {
				// error long jump
				return luaL_error(L, "profiler dump end index error");
			}

			if (start_index >= end_index) {
				// error long jump
				return luaL_error(L, "profiler dump start_index >= end_index error");
			}

			const RecordCopy *start_record = NULL;
			if (start_index >= 0) {
				start_record = record_buffer_.GetRecordByIndex(start_index);
			}

			const RecordCopy *end_record = record_buffer_.GetRecordByIndex(end_index);
			if (!end_record) {
				return luaL_error(L, "profiler dump end index error");
			}

			temp_full_elapse = root_profiler_record_.CalcChildrenElapse(start_record, end_record);
		} else {
			// error long jump
			return luaL_error(L, "profiler args error");
		}

		return temp_full_elapse;
	}

	void Data2Json(FILE *fp, double total_elapse, Record *_record) {
		double full_per = _record->temp_full_elapse_ / total_elapse * 100;
		double self_per = _record->temp_inner_elapse_ / total_elapse * 100;

		const FunctionInfo *func_info = _record->func_info_;
		if (func_info) {
			fprintf(fp, "'call':'%s:%s:%d','count':%u,'total':%ld,'totalPercent':%.3lf,'self':%ld,'selfPercent':%.3lf",
				func_info->name_.c_str(), func_info->source_.c_str(), func_info->linedefined_, _record->temp_call_count_, _record->temp_full_elapse_, full_per, _record->temp_inner_elapse_, self_per);
		} else {
			fprintf(fp, "'call':'root','count':1,'total':%ld,'totalPercent':100,'self':0,'selfPercent':0", _record->temp_full_elapse_);
		}

		if (!_record->children_list_.empty()) {
			fprintf(fp, ",'subcall':[");
			Record::ChildrenList::const_iterator ibegin = _record->children_list_.begin();
			Record::ChildrenList::const_iterator iend = _record->children_list_.end();
			for (; ibegin != iend; ++ibegin) {
				fprintf(fp, "{");
				Data2Json(fp, total_elapse, *ibegin);
				fprintf(fp, "},");
			}
			fprintf(fp, "]");
		}
	}

	int Dump2json(lua_State *L) {
		uint64_t temp_full_elapse = CalcRecord(L);
		if (temp_full_elapse == 0) {
			return luaL_error(L, "profiler CalcRecord error");
		}

		const char *file_name = luaL_checkstring(L, 1);
		if (!file_name) {
			return luaL_error(L, "profiler file_name == NULL error");
		}

		FILE *fp = fopen(file_name, "w+");
		if (!fp) {
			return luaL_error(L, "profiler file_name[%s] open error", file_name);
		}

		fprintf(fp, "{");
		Data2Json(fp, temp_full_elapse, &root_profiler_record_);
		fprintf(fp, "}");
		fflush(fp);
		fclose(fp);

		return 0;
	}

private:
	LuaFilterApiNameMap lua_filter_api_name_;
	LuaFilterApiMap lua_filter_api_;

	LuaProfilerfuncsMap lua_profiler_funcs_;
	CProfilerfuncsMap c_profiler_funcs_;

	RecordBuffer record_buffer_;

	Record root_profiler_record_;

	CallInfoStackMap call_info_stack_map_;
	lua_State *curr_lua_state_;
	CallInfo *curr_call_info_;
	CallInfoStack *curr_call_info_stack_;
};

static void Profilerhook(lua_State *L, lua_Debug *ar) {
	lua_rawgeti(L, LUA_REGISTRYINDEX, (int64_t)&kProfilerStateId);
	LuaProfilerState *S = (LuaProfilerState *)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (S) S->Hook(L, ar);
}

int ProfilerStart(lua_State *L) {
	lua_rawgeti(L, LUA_REGISTRYINDEX, (int64_t)&kProfilerStateId);
	if (!lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return luaL_error(L, "profiler is already running");
	}
	lua_pop(L, 1);

	LuaProfilerState *S = new LuaProfilerState();
	S->Init(L);
	lua_pushlightuserdata(L, S);
	lua_rawseti(L, LUA_REGISTRYINDEX, (int64_t)&kProfilerStateId);

	lua_sethook(L, (lua_Hook)Profilerhook, LUA_MASKCALL | LUA_MASKRET, 0);

	return 0;
}

int ProfilerDump(lua_State *L) {
	lua_rawgeti(L, LUA_REGISTRYINDEX, (int64_t)&kProfilerStateId);
	LuaProfilerState *S = (LuaProfilerState *)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (!S) {
		return luaL_error(L, "profiler not running");
	}

	int n = lua_gettop(L);
	if (n != 1 && n != 3) {
		return luaL_error(L, "profiler ProfilerDump args error");
	}

	return S->Dump2json(L);
}

int CoroutineCreate(lua_State *L) {
	lua_rawgeti(L, LUA_REGISTRYINDEX, (int64_t)&kProfilerStateId);
	LuaProfilerState *S = (LuaProfilerState *)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (!S) {
		return luaL_error(L, "profiler not running");
	}

	lua_State *co = lua_tothread(L, 1);
	if (!co) {
		return luaL_error(L, "profiler CoroutineCreate co == NULL");
	}

	S->CreateCallInfoStack(co);

	return 0;
}

int RecordSave(lua_State *L) {
	lua_rawgeti(L, LUA_REGISTRYINDEX, (int64_t)&kProfilerStateId);
	LuaProfilerState *S = (LuaProfilerState *)lua_touserdata(L, -1);
	lua_pop(L, 1);

	if (!S) {
		return luaL_error(L, "profiler not running");
	}

	S->Save();

	return 0;
}
