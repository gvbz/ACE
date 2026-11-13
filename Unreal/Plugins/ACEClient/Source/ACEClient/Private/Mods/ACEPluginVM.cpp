#include "ACEPluginVM.h"
#include "ACEProfiling.h"
#include "ACEVTRegex.h"
#include "ACEVTProfile.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include <string_view>
extern "C" {
#include "Lua/lua.h"
#include "Lua/lauxlib.h"
#include "Lua/lualib.h"
}

namespace
{
    int PlainContains(lua_State* L)
    {
        size_t HayLength=0,NeedleLength=0;
        const char* Hay=luaL_checklstring(L,1,&HayLength);const char* Needle=luaL_checklstring(L,2,&NeedleLength);
        if(HayLength>4096||NeedleLength>256)return luaL_error(L,"contains text exceeds 4096/256 bytes");
        lua_pushboolean(L,std::string_view(Hay,HayLength).find(std::string_view(Needle,NeedleLength))!=std::string_view::npos);
        return 1;
    }
    int PlainFind(lua_State* L)
    {
        size_t HayLength=0,NeedleLength=0;
        const char* Hay=luaL_checklstring(L,1,&HayLength);const char* Needle=luaL_checklstring(L,2,&NeedleLength);
        if(HayLength>16384||NeedleLength>8192)return luaL_error(L,"findplain text exceeds 16384/8192 bytes");
        const lua_Integer Start=luaL_optinteger(L,3,1);
        const size_t Offset=Start<=1?0:size_t(FMath::Min<lua_Integer>(Start-1,lua_Integer(HayLength)+1));
        const size_t Found=std::string_view(Hay,HayLength).find(std::string_view(Needle,NeedleLength),Offset);
        if(Found==std::string_view::npos)lua_pushnil(L);else lua_pushinteger(L,lua_Integer(Found)+1);
        return 1;
    }
    FString LuaError(lua_State* L)
    { return lua_type(L,-1)==LUA_TSTRING ? FString(UTF8_TO_TCHAR(lua_tostring(L,-1))) : TEXT("Plugin raised a non-text error"); }
    int StoreTick(lua_State* L)
    { const int Ref=luaL_ref(L,LUA_REGISTRYINDEX); lua_pushinteger(L,Ref); return 1; }
    thread_local int Instructions=0;
    thread_local double RegexWork=0;
    void Budget(lua_State* L, lua_Debug*)
    {
        Instructions+=1000;
        if(Instructions>=250000)luaL_error(L,"plugin instruction budget exceeded");
    }
    // Cooperative checkpoint, not an extension of the hard sandbox quota.
    // Leave room to finish one bounded condition and return an intent.
    int WorkAvailable(lua_State* L)
    {
        lua_pushboolean(L,Instructions<200000&&RegexWork<.005);
        return 1;
    }
    thread_local int SpellIndexWork=0;
    double NumberField(lua_State* L,int Index,const char* Key,double Default=0)
    {lua_getfield(L,Index,Key);const double Value=lua_isnumber(L,-1)?lua_tonumber(L,-1):Default;lua_pop(L,1);return Value;}
    bool NotFalse(lua_State* L,int Index,const char* Key)
    {lua_getfield(L,Index,Key);const bool Value=lua_isnil(L,-1)||lua_toboolean(L,-1);lua_pop(L,1);return Value;}
    bool BoolField(lua_State* L,int Index,const char* Key)
    {lua_getfield(L,Index,Key);const bool Value=lua_toboolean(L,-1)!=0;lua_pop(L,1);return Value;}
    void AppendSpell(lua_State* L,int Array,int Spell)
    {lua_pushvalue(L,Spell);lua_rawseti(L,Array,lua_rawlen(L,Array)+1);}
    // Index snapshot tables without copying spell records or spending the Lua
    // instruction quota repeatedly walking the full book. Native work is also
    // bounded per decision; arbitrary plugins cannot bypass the work limit.
    int SpellIndex(lua_State* L)
    {
        luaL_checktype(L,1,LUA_TTABLE);luaL_checktype(L,2,LUA_TTABLE);luaL_checktype(L,5,LUA_TTABLE);
        const double Now=luaL_checknumber(L,3),Margin=luaL_checknumber(L,4);
        // Only an explicit server exemption bypasses component validation.
        const bool ComponentsRequired=lua_type(L,6)!=LUA_TBOOLEAN||lua_toboolean(L,6);
        const size_t Count=lua_rawlen(L,1),Inventory=lua_rawlen(L,2);
        if(Count>16384||Inventory>4096)return luaL_error(L,"spell index exceeds snapshot limit");
        lua_newtable(L);const int Supplies=lua_gettop(L);
        for(size_t I=1;I<=Inventory;++I)
        {
            if(++SpellIndexWork>131072)return luaL_error(L,"spell index work limit exceeded");
            lua_rawgeti(L,2,I);const int Item=lua_gettop(L);luaL_checktype(L,Item,LUA_TTABLE);
            lua_getfield(L,Item,"name");
            if(lua_type(L,-1)==LUA_TSTRING)
            {
                lua_pushvalue(L,-1);lua_rawget(L,Supplies);const double Old=lua_tonumber(L,-1);lua_pop(L,1);
                lua_pushnumber(L,Old+NumberField(L,Item,"count",1));lua_rawset(L,Supplies);
            }
            else lua_pop(L,1);
            lua_pop(L,1);
        }
        lua_newtable(L);const int ById=lua_gettop(L);
        lua_newtable(L);const int Supplied=lua_gettop(L);
        lua_newtable(L);const int Usable=lua_gettop(L);int UsableCount=0;
        // The fourth result partitions the same records, retaining their order
        // and every tier. Policy still chooses the spell; these are only views.
        // Buff candidates deliberately retain blocked spells so the policy can
        // explain missing components/skills and honor its separate buff margin.
        lua_newtable(L);const int Groups=lua_gettop(L);
        lua_newtable(L);const int Buffs=lua_gettop(L);
        lua_newtable(L);const int InstantSelf=lua_gettop(L);
        lua_newtable(L);const int Offense=lua_gettop(L);
        lua_newtable(L);const int OtherBuffs=lua_gettop(L);
        lua_newtable(L);const int Categories=lua_gettop(L);
        for(size_t I=1;I<=Count;++I)
        {
            if(++SpellIndexWork>131072)return luaL_error(L,"spell index work limit exceeded");
            lua_rawgeti(L,1,I);const int Spell=lua_gettop(L);luaL_checktype(L,Spell,LUA_TTABLE);
            lua_getfield(L,Spell,"id");const int Id=lua_gettop(L);luaL_checktype(L,Id,LUA_TNUMBER);
            lua_pushvalue(L,Id);lua_pushvalue(L,Spell);lua_rawset(L,ById);
            bool HasSupply=!ComponentsRequired||NotFalse(L,Spell,"components_known");
            lua_getfield(L,Spell,"scarabs");
            if(ComponentsRequired&&HasSupply&&lua_istable(L,-1))
            {
                lua_pushnil(L);
                while(lua_next(L,-2))
                {
                    if(++SpellIndexWork>131072)return luaL_error(L,"spell index work limit exceeded");
                    const double Required=luaL_checknumber(L,-1);lua_pushvalue(L,-2);lua_rawget(L,Supplies);
                    HasSupply&=lua_tonumber(L,-1)>=Required;lua_pop(L,2);
                }
            }
            lua_pop(L,1);lua_pushvalue(L,Id);lua_pushboolean(L,HasSupply);lua_rawset(L,Supplied);
            lua_pushvalue(L,Id);lua_rawget(L,5);const double Unavailable=lua_tonumber(L,-1);lua_pop(L,1);
            const bool Known=NotFalse(L,Spell,"known"),Beneficial=BoolField(L,Spell,"beneficial"),Caster=BoolField(L,Spell,"caster_target"),SelfBuff=BoolField(L,Spell,"self_buff");
            const double Duration=NumberField(L,Spell,"duration");
            if(Known&&(SelfBuff||(Beneficial&&NumberField(L,Spell,"school")==3&&Duration>0)))AppendSpell(L,Buffs,Spell);
            if(HasSupply&&Known&&Now>=Unavailable&&NumberField(L,Spell,"skill")>=NumberField(L,Spell,"power")+Margin)
            {
                lua_pushvalue(L,Spell);lua_rawseti(L,Usable,++UsableCount);
                if(Caster&&Duration==0)AppendSpell(L,InstantSelf,Spell);
                if(!Beneficial)AppendSpell(L,Offense,Spell);
                if(Beneficial&&!Caster&&!SelfBuff&&Duration>0)AppendSpell(L,OtherBuffs,Spell);
                lua_getfield(L,Spell,"category");
                if(lua_isnumber(L,-1))
                {
                    lua_pushvalue(L,-1);lua_rawget(L,Categories);
                    if(lua_isnil(L,-1))
                    {lua_pop(L,1);lua_newtable(L);lua_pushvalue(L,-2);lua_pushvalue(L,-2);lua_rawset(L,Categories);}
                    AppendSpell(L,lua_gettop(L),Spell);lua_pop(L,1);
                }
                lua_pop(L,1);
            }
            lua_pop(L,2);
        }
        lua_setfield(L,Groups,"by_category");lua_setfield(L,Groups,"other_buffs");
        lua_setfield(L,Groups,"offense");lua_setfield(L,Groups,"instant_self");lua_setfield(L,Groups,"buffs");
        return 4;
    }
    void PushJson(lua_State* L, const FJsonValue* V, int Depth);
    void PushObject(lua_State* L, const FJsonObject* O, int Depth)
    {
        if(Depth>64){luaL_error(L,"Plugin data nesting exceeds 64 levels");return;}
        lua_createtable(L,0,O?O->Values.Num():0);
        if(!O)return;
        for (const auto& Pair : O->Values)
        {
            PushJson(L, Pair.Value.Get(), Depth + 1);
            lua_setfield(L, -2, TCHAR_TO_UTF8(*Pair.Key));
        }
    }
    void PushJson(lua_State* L, const FJsonValue* V, int Depth)
    {
        if(Depth>64){luaL_error(L,"Plugin data nesting exceeds 64 levels");return;}
        if (!V) { lua_pushnil(L); return; }
        switch (V->Type)
        {
        case EJson::Boolean: lua_pushboolean(L, V->AsBool()); break;
        case EJson::Number: lua_pushnumber(L, V->AsNumber()); break;
        case EJson::String: lua_pushstring(L, TCHAR_TO_UTF8(*V->AsString())); break;
        case EJson::Object: PushObject(L, V->AsObject().Get(), Depth); break;
        case EJson::Array:
            lua_createtable(L,V->AsArray().Num(),0);
            for (int I = 0; I < V->AsArray().Num(); ++I) { PushJson(L, V->AsArray()[I].Get(), Depth + 1); lua_rawseti(L, -2, I + 1); }
            break;
        default: lua_pushnil(L);
        }
    }
    int PushArgument(lua_State* L);
    struct FRegexPair {TArray<ANSICHAR> Key,Value;};
    int PushRegexCaptures(lua_State* L)
    {
        const auto* Pairs=static_cast<const TArray<FRegexPair>*>(lua_touserdata(L,1));
        lua_createtable(L,0,Pairs->Num());
        for(const auto& Pair:*Pairs)
        {
            lua_pushlstring(L,Pair.Key.GetData(),Pair.Key.Num());
            lua_pushlstring(L,Pair.Value.GetData(),Pair.Value.Num());
            lua_rawset(L,-3);
        }
        return 1;
    }
    int RegexTest(lua_State* L)
    {
        size_t TextBytes=0,PatternBytes=0;const char* Text=luaL_checklstring(L,1,&TextBytes);const char* Pattern=luaL_checklstring(L,2,&PatternBytes);
        if(TextBytes>16384||PatternBytes>8192)return luaL_error(L,"Regex input exceeds limit");
        bool Failed=false,Found=false;
        {
            const double Start=FPlatformTime::Seconds();FString Error;
            Found=ACEVTRegex::Test(UTF8_TO_TCHAR(Pattern),UTF8_TO_TCHAR(Text),Error);
            RegexWork+=FPlatformTime::Seconds()-Start;Failed=!Error.IsEmpty()||RegexWork>.02;
        }
        if(Failed)return luaL_error(L,"Regex failed or exceeded per-tick work limit");
        lua_pushboolean(L,Found);return 1;
    }
    int RegexMatch(lua_State* L)
    {
        size_t TextBytes=0,PatternBytes=0;const char* Text=luaL_checklstring(L,1,&TextBytes);const char* Pattern=luaL_checklstring(L,2,&PatternBytes);
        if(TextBytes>16384||PatternBytes>8192)return luaL_error(L,"Regex input exceeds limit");
        // C++ temporaries are destroyed before raising the Lua longjmp exception.
        bool Failed=false;int Result=0;
        {
            const double Start=FPlatformTime::Seconds();ACEVTRegex::FCaptures Groups;FString Error;
            const bool Found=ACEVTRegex::Match(UTF8_TO_TCHAR(Pattern),UTF8_TO_TCHAR(Text),Groups,Error);
            RegexWork+=FPlatformTime::Seconds()-Start;Failed=!Error.IsEmpty()||RegexWork>.02;
            if(!Failed)
            {
                if(!Found)lua_pushnil(L);
                else
                {
                    // JSON's FString keys fold case. Build the capture table
                    // directly so groups named x and X remain distinct. Keep
                    // UTF-8 storage outside pcall's longjmp boundary.
                    TArray<FRegexPair> Pairs;
                    for(const auto& Group:Groups)
                    {
                        auto& Pair=Pairs.AddDefaulted_GetRef();
                        FTCHARToUTF8 Key(*Group.Key),Value(*Group.Value);
                        Pair.Key.Append(Key.Get(),Key.Length());Pair.Value.Append(Value.Get(),Value.Length());
                    }
                    lua_pushcfunction(L,PushRegexCaptures);lua_pushlightuserdata(L,&Pairs);
                    Failed=lua_pcall(L,1,1,0)!=LUA_OK;
                }
                Result=1;
            }
        }
        if(Failed)return luaL_error(L,"Regex failed or exceeded per-tick work limit");return Result;
    }
    int CompileCommand(lua_State* L)
    {
        size_t Bytes=0;const char* Text=luaL_checklstring(L,1,&Bytes);if(Bytes>4096)return luaL_error(L,"Command exceeds limit");
        bool Failed=false;
        {TArray<FString> Issues;auto Command=ACEVTProfile::CompileCommand(UTF8_TO_TCHAR(Text),Issues);Failed=Issues.Num()!=0;
         if(!Failed){lua_pushcfunction(L,PushArgument);lua_pushlightuserdata(L,Command.Get());Failed=lua_pcall(L,1,1,0)!=LUA_OK;}}
        if(Failed)return luaL_error(L,"Generated command has no supported native adapter");return 1;
    }
    int Open(lua_State* L)
    {
        luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
        luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
        luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
        luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
        luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
        // pcall/xpcall/coroutines could catch a quota exception and continue forever.
        for (const char* Key : {"dofile", "loadfile", "load", "collectgarbage", "print", "pcall", "xpcall", "getmetatable", "setmetatable"})
        { lua_pushnil(L); lua_setglobal(L, Key); }
        // Pattern matching can consume unbounded time entirely inside C, outside
        // the instruction hook. Keep the string library's bounded basic operations.
        lua_getglobal(L, "string");
        for (const char* Key : {"find", "match", "gmatch", "gsub", "dump"})
        { lua_pushnil(L); lua_setfield(L, -2, Key); }
        lua_pushcfunction(L,PlainContains);lua_setfield(L,-2,"contains");
        lua_pushcfunction(L,PlainFind);lua_setfield(L,-2,"findplain");
        lua_pop(L, 1);
        lua_pushcfunction(L,RegexMatch);lua_setglobal(L,"regexmatch");
        lua_pushcfunction(L,RegexTest);lua_setglobal(L,"regextest");
        lua_pushcfunction(L,CompileCommand);lua_setglobal(L,"vtcommand"); // Legacy plugin API alias.
        lua_pushcfunction(L,CompileCommand);lua_setglobal(L,"ucmcommand");
        lua_pushcfunction(L,SpellIndex);lua_setglobal(L,"spellindex");
        lua_pushcfunction(L,WorkAvailable);lua_setglobal(L,"workavailable");
        return 0;
    }
    int PushArgument(lua_State* L)
    { PushObject(L, static_cast<const FJsonObject*>(lua_touserdata(L, 1)), 0); return 1; }

    // Only explicit extension fields accept structured output. No metatables,
    // functions, sparse/mixed arrays, cycles or unbounded native traversal.
    TSharedPtr<FJsonValue> ReadOutput(lua_State* L,int Index,int Depth,int& Nodes,int& Bytes,FString& Error,bool ProfileData=false)
    {
        if(Depth>12||++Nodes>(ProfileData?32768:4096)){Error=TEXT("Structured output exceeds depth/node limit");return nullptr;}
        Index=lua_absindex(L,Index);
        switch(lua_type(L,Index))
        {
        case LUA_TBOOLEAN:return MakeShared<FJsonValueBoolean>(lua_toboolean(L,Index)!=0);
        case LUA_TNUMBER:
            if(FMath::IsFinite(lua_tonumber(L,Index)))return MakeShared<FJsonValueNumber>(lua_tonumber(L,Index));
            break;
        case LUA_TSTRING:
        {
            size_t Len=0;const char* Value=lua_tolstring(L,Index,&Len);Bytes+=int(Len);
            if(Len>4096||Bytes>(ProfileData?262144:65536)){Error=TEXT("Structured output text exceeds limit");return nullptr;}
            return MakeShared<FJsonValueString>(UTF8_TO_TCHAR(Value));
        }
        case LUA_TTABLE:
        {
            const size_t Length=lua_rawlen(L,Index);if(Length>(ProfileData?2048u:1024u)){Error=TEXT("Structured array exceeds limit");return nullptr;}
            auto Object=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Array;
            if(Length)Array.SetNum(int32(Length));
            lua_pushnil(L);
            while(lua_next(L,Index))
            {
                FString Key;int ArrayIndex=-1;
                if(Length&&lua_isinteger(L,-2)&&lua_tointeger(L,-2)>=1&&lua_tointeger(L,-2)<=int64(Length))ArrayIndex=int(lua_tointeger(L,-2))-1;
                else if(!Length&&lua_type(L,-2)==LUA_TSTRING)
                {size_t Len=0;const char* K=lua_tolstring(L,-2,&Len);Bytes+=int(Len);if(Len>128||Bytes>(ProfileData?262144:65536)){Error=TEXT("Structured key/text exceeds limit");return nullptr;}Key=UTF8_TO_TCHAR(K);}
                else {Error=TEXT("Structured tables must be objects or dense arrays");return nullptr;}
                auto Value=ReadOutput(L,-1,Depth+1,Nodes,Bytes,Error,ProfileData);if(!Value)return nullptr;
                // Empty Lua arrays are ambiguous. Normalize documented collection keys.
                static const TSet<FString> Collections={TEXT("children"),TEXT("options"),TEXT("route"),TEXT("loot_rules"),TEXT("monsters"),TEXT("states"),TEXT("buffs"),TEXT("weapons"),TEXT("weapon_items"),TEXT("consumables"),TEXT("vendor_rules"),TEXT("buff_commands"),TEXT("conditions"),TEXT("excluded_buffs"),TEXT("excluded_buff_spells"),TEXT("restock"),TEXT("profile_rules"),TEXT("chest_rules"),TEXT("mule1_rules"),TEXT("mule2_rules"),TEXT("assist_items"),TEXT("buff_items"),TEXT("item_buff_targets"),TEXT("recharge_handlers"),TEXT("pea_recipes"),TEXT("transitions"),TEXT("rules"),TEXT("requirements"),TEXT("extras")};
                if(Value->Type==EJson::Object&&Value->AsObject()->Values.IsEmpty()
                    &&Collections.Contains(Key))
                    Value=MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{});
                if(ArrayIndex>=0)Array[ArrayIndex]=Value;else Object->SetField(Key,Value);
                lua_pop(L,1);
            }
            if(Length){for(const auto& Value:Array)if(!Value){Error=TEXT("Sparse output array");return nullptr;}return MakeShared<FJsonValueArray>(Array);}
            return MakeShared<FJsonValueObject>(Object);
        }
        }
        Error=TEXT("Unsupported structured output value");return nullptr;
    }
}

FACEPluginVM::FACEPluginVM()
{
    State = lua_newstate([](void* Ud, void* Ptr, size_t Old, size_t New) -> void*
    {
        auto* Self = static_cast<FACEPluginVM*>(Ud);
        if (!Ptr) Old = 0; // Lua uses osize as a type tag for new allocations.
        if (!New) { FMemory::Free(Ptr); Self->Bytes -= Old; return nullptr; }
        if (New > MemoryLimit || Self->Bytes - Old > MemoryLimit - New) return nullptr;
        void* Result = FMemory::Realloc(Ptr, New);
        if (Result) {Self->Bytes = Self->Bytes - Old + New;Self->PeakBytes=FMath::Max(Self->PeakBytes,Self->Bytes);}
        return Result;
    }, this);
}
FACEPluginVM::~FACEPluginVM() { if (State) lua_close(State); }
bool FACEPluginVM::Load(const FString& Source, FString& Error, const FString& Policy)
{
    SpellIndexWork=0;Instructions=0;RegexWork=0;
    if (!State || Source.Len() > 256 * 1024) { Error = TEXT("Plugin exceeds source/memory limit"); return false; }
    lua_pushcfunction(State, Open);
    if (lua_pcall(State, 0, 0, 0) != LUA_OK) { Error = LuaError(State); return false; }
    lua_sethook(State, Budget, LUA_MASKCOUNT, 1000);
    // One bounded rule evaluator is shared by UCM and Item Tools. Scripts still
    // have no filesystem access; this is an engine-supplied API module.
    FString Rules;
    const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
    if(!Plugin||!FFileHelper::LoadFileToString(Rules,*(Plugin->GetBaseDir()/TEXT("ClientMods/loot_rules.lua")))||Rules.Len()>65536)
    {Error=TEXT("Shared loot evaluator is missing or invalid");return false;}
    FTCHARToUTF8 RuleUtf8(*Rules);
    if(luaL_loadbufferx(State,RuleUtf8.Get(),RuleUtf8.Length(),"loot_rules","t")!=LUA_OK||lua_pcall(State,0,1,0)!=LUA_OK)
    {Error=LuaError(State);lua_settop(State,0);return false;}
    lua_setglobal(State,"lootmatch");
    if(!Policy.IsEmpty())
    {
        if(Policy!=TEXT("ucm")&&Policy!=TEXT("itemtools")){Error=TEXT("Unknown built-in policy module");return false;}
        FString PolicySource;
        if(!FFileHelper::LoadFileToString(PolicySource,*(Plugin->GetBaseDir()/TEXT("ClientMods")/Policy/TEXT("main.lua")))){Error=TEXT("Policy module unavailable");return false;}
        FTCHARToUTF8 PolicyUTF8(*PolicySource);
        if(luaL_loadbufferx(State,PolicyUTF8.Get(),PolicyUTF8.Length(),"builtinpolicy","t")!=LUA_OK||lua_pcall(State,0,1,0)!=LUA_OK||!lua_isfunction(State,-1))
        {Error=LuaError(State);lua_settop(State,0);return false;}
        lua_setglobal(State,"builtinpolicy");Instructions=0;
        FString Defaults;TSharedPtr<FJsonObject> DefaultProfile;
        if(FFileHelper::LoadFileToString(Defaults,*(Plugin->GetBaseDir()/TEXT("ClientMods")/Policy/TEXT("default.json")))&&FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Defaults),DefaultProfile))
        {
            lua_pushcfunction(State,PushArgument);lua_pushlightuserdata(State,DefaultProfile.Get());
            if(lua_pcall(State,1,1,0)!=LUA_OK){Error=LuaError(State);return false;}lua_setglobal(State,"builtinprofile");
        }
    }
    FTCHARToUTF8 Utf8(*Source);
    if (luaL_loadbufferx(State, Utf8.Get(), Utf8.Length(), "plugin", "t") != LUA_OK || lua_pcall(State, 0, 1, 0) != LUA_OK)
    { Error = LuaError(State); lua_settop(State, 0); return false; }
    if (!lua_isfunction(State, -1)) { Error = TEXT("Plugin must return a tick(snapshot, profile) function"); return false; }
    lua_pushcfunction(State,StoreTick); lua_insert(State,-2);
    if(lua_pcall(State,1,1,0)!=LUA_OK){Error=LuaError(State);return false;}
    Function=int(lua_tointeger(State,-1));lua_pop(State,1);
    return true;
}
bool FACEPluginVM::Step(const TSharedPtr<FJsonObject>& Snapshot, const TSharedPtr<FJsonObject>& Profile,
    TSharedPtr<FJsonObject>& Intent, FString& Error)
{
    ACE_PROFILE_SCOPE(PluginLua);
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginLuaStep);
    Intent.Reset();RegexWork=0;SpellIndexWork=0;Instructions=0;
    lua_settop(State, 0);
    ON_SCOPE_EXIT { lua_settop(State, 0); };
    lua_sethook(State, Budget, LUA_MASKCOUNT, 1000);
    lua_rawgeti(State, LUA_REGISTRYINDEX, Function);
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginLuaInputs);
        for (const auto* Object : {Snapshot.Get(), Profile.Get()})
        {
            lua_pushcfunction(State, PushArgument); lua_pushlightuserdata(State, const_cast<FJsonObject*>(Object));
            const int Result=lua_pcall(State,1,1,0);
            if(Result!=LUA_OK){Error=RuntimeError(Object==Snapshot.Get()?TEXT("snapshot input"):TEXT("profile input"),Result,Snapshot.Get(),Profile.Get());return false;}
        }
    }
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginLuaCallback);
        const int Result=lua_pcall(State,2,1,0);
        if(Result!=LUA_OK){Error=RuntimeError(TEXT("callback"),Result,Snapshot.Get(),Profile.Get());return false;}
    }
    if (lua_isnil(State, -1)) return true;
    if (!lua_istable(State, -1)) { Error = TEXT("Tick must return nil or an intent table"); return false; }
    Intent = MakeShared<FJsonObject>();
    lua_pushnil(State);
    int Count = 0;
    while (lua_next(State, -2))
    {
        if (++Count > 32 || lua_type(State, -2) != LUA_TSTRING) { Error = TEXT("Invalid intent fields"); return false; }
        size_t KeyLength = 0; const char* KeyText = lua_tolstring(State, -2, &KeyLength);
        if (KeyLength > 128) { Error = TEXT("Intent key exceeds limit"); return false; }
        const FString Key = UTF8_TO_TCHAR(KeyText);
        if (lua_type(State, -1) == LUA_TSTRING)
        {
            size_t Len = 0; const char* Value = lua_tolstring(State, -1, &Len);
            if (Len > 1024) { Error = TEXT("Intent text exceeds limit"); return false; }
            Intent->SetStringField(Key, UTF8_TO_TCHAR(Value));
        }
        else if (lua_type(State, -1) == LUA_TNUMBER)
        {
            const double Value = lua_tonumber(State, -1);
            if (!FMath::IsFinite(Value)) { Error = TEXT("Non-finite intent value"); return false; }
            Intent->SetNumberField(Key, Value);
        }
        else if (lua_isboolean(State, -1)) Intent->SetBoolField(Key, lua_toboolean(State, -1) != 0);
        else if((Key==TEXT("presentation")||Key==TEXT("state")||Key==TEXT("ui")||Key==TEXT("chat_filters")||Key==TEXT("chat_subscriptions")||Key==TEXT("markers")||Key==TEXT("world_plates")||Key==TEXT("terrain_overlay")||Key==TEXT("messages")||Key==TEXT("requests")||Key==TEXT("profile_patch"))&&lua_istable(State,-1))
        {
            int Nodes=0,OutputBytes=0;auto Value=ReadOutput(State,-1,0,Nodes,OutputBytes,Error,Key==TEXT("profile_patch")||Key==TEXT("requests"));
            if(!Value){lua_settop(State,0);return false;}
            // Lua's empty table has no array/object distinction.
            if(Key!=TEXT("terrain_overlay")&&Key!=TEXT("presentation")&&Key!=TEXT("state")&&Key!=TEXT("profile_patch")&&Value->Type==EJson::Object&&Value->AsObject()->Values.IsEmpty())Value=MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{});
            Intent->SetField(Key,Value);
        }
        else if(Key==TEXT("runtime_route")&&lua_istable(State,-1))
        {
            // Narrow overlay output: no arbitrary nested Lua objects or cycles.
            const size_t Num=lua_rawlen(State,-1);if(Num>2048){Error=TEXT("Overlay route exceeds 2048 points");return false;}
            TArray<TSharedPtr<FJsonValue>> Route;
            for(size_t I=1;I<=Num;++I)
            {
                lua_rawgeti(State,-1,I);if(!lua_istable(State,-1)){Error=TEXT("Invalid overlay waypoint");return false;}
                auto Point=MakeShared<FJsonObject>();
                for(const char* Field:{"cell","x","y","z"})
                {
                    lua_getfield(State,-1,Field);
                    if(lua_type(State,-1)!=LUA_TNUMBER||!FMath::IsFinite(lua_tonumber(State,-1))){Error=TEXT("Invalid overlay coordinate");return false;}
                    Point->SetNumberField(UTF8_TO_TCHAR(Field),lua_tonumber(State,-1));lua_pop(State,1);
                }
                lua_getfield(State,-1,"kind");size_t Len=0;const char* Kind=lua_tolstring(State,-1,&Len);
                if(Kind&&Len<=32)Point->SetStringField(TEXT("kind"),UTF8_TO_TCHAR(Kind));lua_pop(State,1);
                Route.Add(MakeShared<FJsonValueObject>(Point));lua_pop(State,1);
            }
            Intent->SetArrayField(Key,Route);
        }
        lua_pop(State, 1);
    }
    lua_settop(State, 0);
    return true;
}

FString FACEPluginVM::RuntimeError(const TCHAR* Phase,int Result,const FJsonObject* Snapshot,const FJsonObject* Profile) const
{
    if(Result!=LUA_ERRMEM)return LuaError(State);
    auto Count=[](const FJsonObject* Object,const TCHAR* Key)
    {const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;return Object&&Object->TryGetArrayField(Key,Values)?Values->Num():0;};
    return FString::Printf(TEXT("Lua memory limit reached during %s (heap %.2f/%.0f MiB, peak %.2f MiB; inventory %d, spells %d, targets %d, contents %d, loot rules %d)"),
        Phase,Bytes/1048576.,MemoryLimit/1048576.,PeakBytes/1048576.,Count(Snapshot,TEXT("inventory")),Count(Snapshot,TEXT("spells")),
        Count(Snapshot,TEXT("targets")),Count(Snapshot,TEXT("contents")),Count(Profile,TEXT("loot_rules")));
}
