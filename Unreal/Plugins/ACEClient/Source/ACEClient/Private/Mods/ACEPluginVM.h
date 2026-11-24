#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
struct lua_State;

/** One VM per running plugin; text scripts, bounded heap and work per callback. */
class FACEPluginVM
{
public:
    // Includes host-marshalled inventory, spellbook and custom loot rules as
    // well as script state. The former 8 MiB limit could not hold those together.
    static constexpr SIZE_T MemoryLimit = 32 * 1024 * 1024;
    FACEPluginVM();
    ~FACEPluginVM();
    bool Load(const FString& Source, FString& Error, const FString& Policy = FString());
    bool Step(const TSharedPtr<FJsonObject>& Snapshot, const TSharedPtr<FJsonObject>& Profile,
        TSharedPtr<FJsonObject>& Intent, FString& Error, bool bBundledUCMInputs = false);
    SIZE_T GetMemoryUsage() const { return Bytes; }
    SIZE_T GetPeakMemoryUsage() const { return PeakBytes; }
private:
    // The stock policy reads these host-owned arrays without modifying their Lua tables.
    struct FInputTable
    {
        TWeakPtr<FJsonValue> Source;
        int Ref = -2; // LUA_NOREF
    };
    FInputTable SpellTable, InventoryTable, LootRulesTable;
    TArray<TWeakPtr<FJsonValue>> InventoryRows;
    void PushInventory(lua_State* L,const TArray<TSharedPtr<FJsonValue>>& Rows);
    static int PushUCMArgument(lua_State* L);
    lua_State* State = nullptr;
    SIZE_T Bytes = 0;
    SIZE_T PeakBytes = 0;
    int Function = 0;
    FString RuntimeError(const TCHAR* Phase, int Result, const FJsonObject* Snapshot, const FJsonObject* Profile) const;
};
