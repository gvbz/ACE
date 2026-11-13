#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace ACEAutoCraft
{
constexpr int32 MaxUnits=100000, MaxOperations=10000, MaxDepth=24;
struct FItem { int32 Id=0, MaxStack=1; uint32 Icon=0, Type=0; FString Name; bool PackSlot=false; };
struct FRecipe
{
    FString Id, Category, FailMessage;
    int32 Source=0, Target=0, Output=0, Yield=1, Skill=0, Difficulty=0;
    int32 SourceCount=0, TargetCount=0, FailSource=0, FailTarget=0, FailOutput=0, FailYield=0;
    bool Chain=false, Conditional=false, Modified=false;
    bool OptionalSource=false, OptionalTarget=false, OptionalFailSource=false, OptionalFailTarget=false;
    bool SourceReusable() const { return !SourceCount&&!FailSource&&!Modified; }
    bool TargetReusable() const { return !TargetCount&&!FailTarget&&!Modified; }
};
struct FCatalog
{
    TMap<int32,FItem> Items;
    TArray<FRecipe> Recipes;
    TMap<FString,int32> ById;
    TMap<int32,TArray<int32>> ByOutput;
    TMap<FString,TArray<int32>> ByName;
    TArray<FString> Categories;
    FString Version, Error;
    bool Load(const FString& Path);
    bool Parse(const TSharedPtr<FJsonObject>& Json);
    bool Merge(const FCatalog& Extra);
    FString Name(int32 Id) const;
};
struct FInventory
{
    TMap<int32,int64> Counts, RetainedTools;
    // Receipts include protected/equipped items and nonempty output containers;
    // eligibility limits planning, not whether the server delivered a result.
    TMap<int32,int64> ReceiptCounts;
    bool HasReceipts=false;
    int64 Available(int32 Id,bool Tool=false) const {return Counts.FindRef(Id)+(Tool?RetainedTools.FindRef(Id):0);}
    int64 Receipt(int32 Id) const {return HasReceipts?ReceiptCounts.FindRef(Id):Available(Id,true);}
};
struct FStep { int32 Recipe=INDEX_NONE, Batches=0; };
struct FPlan
{
    TArray<FStep> Steps;
    TMap<int32,int64> Consumed, Tools, Missing;
    FString Error;
    int32 Units=0, Operations=0;
    bool Valid=false;
};
// Quantities are additional output units. Whole-recipe batches round up.
FPlan Plan(const FCatalog& Catalog,int32 Recipe,const FInventory& Inventory,int32 Units,bool Ingredients);
FPlan Maximum(const FCatalog& Catalog,int32 Recipe,const FInventory& Inventory,bool Ingredients);
// Shopping-list preview only. Missing supplies are simulated so both operands
// and recursive dependencies can be inspected; never use its steps to execute.
FPlan InspectMissing(const FCatalog& Catalog,int32 Recipe,const FInventory& Inventory,int32 Units,bool Ingredients);
enum class EOutcome : uint8 { Waiting, Success, Failure, Unexpected };
// Inventory changes, not a successful UseDone alone, confirm a production attempt.
EOutcome Outcome(const FRecipe& Recipe,const FInventory& Before,const FInventory& After);
}

struct FACEAutoCraftState
{
    ACEAutoCraft::FCatalog Catalog;
    ACEAutoCraft::FPlan Preview;
    ACEAutoCraft::FInventory Inventory, Before;
    FString Character, CatalogServer, ProfileSignature, Details, InventorySummary;
    bool Loaded=false, Paused=false, Pending=false, Confirmed=false, ResultSeen=false, FailureMessage=false;
    FString Preparation;
    int32 PrepareAmount=0, SourceBefore=0, TargetBefore=0;
    TSet<int32> BeforeIds;
    int32 Selected=INDEX_NONE, PendingRecipe=INDEX_NONE, SourceGuid=0, TargetGuid=0;
    int32 Goal=0, Completed=0, Attempts=0, Failures=0;
    uint64 Revision=MAX_uint64;
    uint32 Serial=0, InventoryFailureSerial=0;
    double NextRefresh=0, NextAction=0, Deadline=0, ResultAt=0, RequestedAt=0;
    TArray<TSharedPtr<FJsonValue>> Candidates;
    TMap<FString,TArray<int32>> SupplyRecipes;
    TSet<FString> BlockedSupplies;
};
