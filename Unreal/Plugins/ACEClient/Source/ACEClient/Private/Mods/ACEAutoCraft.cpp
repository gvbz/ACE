#include "Mods/ACEAutoCraft.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

namespace ACEAutoCraft
{
namespace
{
bool Int(const TSharedPtr<FJsonObject>& J,const TCHAR* Key,int32& Out,int32 Min=0,int32 Max=MAX_int32)
{
    double N=0;if(!J||!J->TryGetNumberField(Key,N)||!FMath::IsFinite(N)||N<Min||N>Max||N!=FMath::FloorToDouble(N))return false;
    Out=int32(N);return true;
}
class FPlanner
{
public:
    const FCatalog& C; FInventory Stock; FPlan Result; bool Ingredients; bool Inspect=false; int32 Work=0;
    TSet<int32> Visiting;
    FPlanner(const FCatalog& In,const FInventory& I,bool Auto):C(In),Stock(I),Ingredients(Auto){}
    bool Need(int32 Item,int64 Amount,bool Tool,int32 Depth)
    {
        if(++Work>4096||Depth>MaxDepth){Result.Error=TEXT("Ingredient chain is too complex; make an intermediate item first.");return false;}
        if(Stock.Available(Item,Tool)>=Amount)
        {
            if(Tool)Result.Tools.FindOrAdd(Item)=FMath::Max(Result.Tools.FindRef(Item),Amount);
            else {Stock.Counts.FindOrAdd(Item)-=Amount;Result.Consumed.FindOrAdd(Item)+=Amount;}
            return true;
        }
        const auto* Candidates=C.ByOutput.Find(Item);
        if(Ingredients&&Candidates&&!Visiting.Contains(Item))
        {
            Visiting.Add(Item);
            // Prefer ingredients that are already present, then the least
            // difficult route. Stable ordering keeps alternate recipes predictable.
            auto Ordered=*Candidates;
            Ordered.StableSort([&](int32 A,int32 B){auto Score=[&](int32 Index){const auto& R=C.Recipes[Index];return
                (Stock.Available(R.Source,R.SourceReusable())>=FMath::Max(1,R.SourceCount)?0:100000)+
                (Stock.Available(R.Target,R.TargetReusable())>=FMath::Max(1,R.TargetCount)?0:100000)+R.Difficulty;};return Score(A)<Score(B);});
            for(int32 Index:Ordered)
            {
                const auto& R=C.Recipes[Index];if(!R.Chain)continue;
                const auto SavedStock=Stock;const auto SavedResult=Result;
                const int64 Missing=Amount-Stock.Available(Item,Tool);
                const int64 Batches=(Missing+R.Yield-1)/R.Yield;
                if(Batches<=MaxOperations&&Expand(Index,int32(Batches),Depth+1)&&Stock.Available(Item,Tool)>=Amount)
                {
                    Visiting.Remove(Item);
                    if(Tool)Result.Tools.FindOrAdd(Item)=FMath::Max(Result.Tools.FindRef(Item),Amount);
                    else {Stock.Counts.FindOrAdd(Item)-=Amount;Result.Consumed.FindOrAdd(Item)+=Amount;}
                    return true;
                }
                Stock=SavedStock;Result=SavedResult;
                if(Work>4096)break;
            }
            Visiting.Remove(Item);
        }
        Result.Missing.FindOrAdd(Item)+=FMath::Max<int64>(0,Amount-Stock.Available(Item,Tool));
        if(Inspect)
        {
            // Add only the shortage. Consumables reserve the remaining stock;
            // a missing reusable tool is acquired once and shared across steps.
            Stock.Counts.FindOrAdd(Item)+=FMath::Max<int64>(0,Amount-Stock.Available(Item,Tool));
            if(Tool)Result.Tools.FindOrAdd(Item)=FMath::Max(Result.Tools.FindRef(Item),Amount);
            else {Stock.Counts.FindOrAdd(Item)-=Amount;Result.Consumed.FindOrAdd(Item)+=Amount;}
            return true;
        }
        Result.Error=TEXT("Missing ingredient or tool: ")+C.Name(Item);return false;
    }
    bool Expand(int32 Index,int32 Batches,int32 Depth)
    {
        const auto& R=C.Recipes[Index];
        if(Batches<1||Batches>MaxOperations||Result.Operations>MaxOperations-Batches){Result.Error=TEXT("Job exceeds 10,000 crafting operations; choose a smaller quantity.");return false;}
        // Reserve consumables that are on hand before recursively making the
        // other operand, so a branch cannot spend the parent's ingredients.
        struct FNeed {int32 Id;int64 Count;bool Tool,Consumed;};
        TArray<FNeed> Needs={{R.Source,FMath::Max<int64>(1,int64(R.SourceCount)*Batches),R.SourceReusable(),R.SourceCount>0},
                             {R.Target,FMath::Max<int64>(1,int64(R.TargetCount)*Batches),R.TargetReusable(),R.TargetCount>0}};
        Needs.StableSort([&](const auto& A,const auto& B){return (Stock.Available(A.Id,A.Tool)>=A.Count)>(Stock.Available(B.Id,B.Tool)>=B.Count);});
        TMap<int32,int64> ReservedTools;
        for(const auto& N:Needs)
        {
            // A zero-consumption operand still has to exist, but is never
            // silently removed from the simulated inventory.
            if(!Need(N.Id,N.Count,N.Tool,Depth))return false;
            if(!N.Consumed)
            {
                if(N.Tool){const int64 Hold=FMath::Max<int64>(0,N.Count-Stock.RetainedTools.FindRef(N.Id));Stock.Counts.FindOrAdd(N.Id)-=Hold;ReservedTools.FindOrAdd(N.Id)+=Hold;}
                else {ReservedTools.FindOrAdd(N.Id)+=N.Count;Result.Consumed.FindOrAdd(N.Id)-=N.Count;Result.Tools.FindOrAdd(N.Id)=FMath::Max(Result.Tools.FindRef(N.Id),N.Count);}
            }
        }
        for(const auto& Pair:ReservedTools)Stock.Counts.FindOrAdd(Pair.Key)+=Pair.Value;
        Result.Steps.Add({Index,Batches});Result.Operations+=Batches;
        Stock.Counts.FindOrAdd(R.Output)+=int64(R.Yield)*Batches;return true;
    }
};
}
FString FCatalog::Name(int32 Id)const {const auto* I=Items.Find(Id);return I?I->Name:FString::Printf(TEXT("Item %d"),Id);}
bool FCatalog::Load(const FString& Path)
{
    FString Text;TSharedPtr<FJsonObject> Json;
    if(IFileManager::Get().FileSize(*Path)>16*1024*1024||!FFileHelper::LoadFileToString(Text,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json))
    {Error=TEXT("Cannot read the crafting catalog.");return false;}
    return Parse(Json);
}
bool FCatalog::Parse(const TSharedPtr<FJsonObject>& Json)
{
    *this=FCatalog();const TArray<TSharedPtr<FJsonValue>> *ItemRows=nullptr,*RecipeRows=nullptr;int32 Schema=0;
    auto Fail=[&](){Items.Reset();Recipes.Reset();ById.Reset();ByOutput.Reset();ByName.Reset();Categories.Reset();Error=TEXT("Invalid crafting catalog; no crafting actions are available.");return false;};
    if(!Int(Json,TEXT("schema"),Schema,1,1)||!Json->TryGetArrayField(TEXT("items"),ItemRows)||!Json->TryGetArrayField(TEXT("recipes"),RecipeRows)||ItemRows->Num()>30000||RecipeRows->Num()>30000)return Fail();
    Json->TryGetStringField(TEXT("version"),Version);
    for(const auto& V:*ItemRows)
    {
        if(!V||V->Type!=EJson::Object)return Fail();const auto J=V->AsObject();FItem I;int32 Icon=0,Type=0;
        if(!Int(J,TEXT("id"),I.Id,1)||!Int(J,TEXT("max_stack"),I.MaxStack,1,MaxUnits)||!Int(J,TEXT("icon"),Icon)||!Int(J,TEXT("type"),Type)||!J->TryGetStringField(TEXT("name"),I.Name)||I.Name.IsEmpty()||I.Name.Len()>256||Items.Contains(I.Id))return Fail();
        I.Icon=uint32(Icon);I.Type=uint32(Type);J->TryGetBoolField(TEXT("pack_slot"),I.PackSlot);Items.Add(I.Id,MoveTemp(I));
    }
    for(const auto& V:*RecipeRows)
    {
        if(!V||V->Type!=EJson::Object)return Fail();const auto J=V->AsObject();FRecipe R;
        if(!J->TryGetStringField(TEXT("id"),R.Id)||R.Id.IsEmpty()||R.Id.Len()>64||ById.Contains(R.Id)||!J->TryGetStringField(TEXT("category"),R.Category)||R.Category.IsEmpty()||R.Category.Len()>80
            ||!Int(J,TEXT("source"),R.Source,1)||!Int(J,TEXT("target"),R.Target,1)||!Int(J,TEXT("output"),R.Output,1)
            ||!Int(J,TEXT("yield_count"),R.Yield,1,MaxUnits)||!Int(J,TEXT("skill"),R.Skill,0,100)||!Int(J,TEXT("difficulty"),R.Difficulty,0,100000)
            ||!Int(J,TEXT("source_count"),R.SourceCount,0,MaxUnits)||!Int(J,TEXT("target_count"),R.TargetCount,0,MaxUnits)
            ||!Int(J,TEXT("fail_source"),R.FailSource,0,MaxUnits)||!Int(J,TEXT("fail_target"),R.FailTarget,0,MaxUnits)
            ||!Int(J,TEXT("fail_output"),R.FailOutput)||!Int(J,TEXT("fail_yield"),R.FailYield,0,MaxUnits)
            ||!Items.Contains(R.Source)||!Items.Contains(R.Target)||!Items.Contains(R.Output))return Fail();
        J->TryGetBoolField(TEXT("chain"),R.Chain);J->TryGetBoolField(TEXT("conditional"),R.Conditional);J->TryGetBoolField(TEXT("modified"),R.Modified);
        J->TryGetStringField(TEXT("fail_message"),R.FailMessage);if(R.FailMessage.Len()>4096)return Fail();
        J->TryGetBoolField(TEXT("optional_source"),R.OptionalSource);J->TryGetBoolField(TEXT("optional_target"),R.OptionalTarget);
        J->TryGetBoolField(TEXT("optional_fail_source"),R.OptionalFailSource);J->TryGetBoolField(TEXT("optional_fail_target"),R.OptionalFailTarget);
        R.Chain=R.Chain&&!R.Modified&&R.Output!=R.Source&&R.Output!=R.Target&&(R.SourceCount||R.TargetCount);
        ById.Add(R.Id,Recipes.Num());ByOutput.FindOrAdd(R.Output).Add(Recipes.Num());ByName.FindOrAdd(Name(R.Output)).Add(Recipes.Num());Categories.AddUnique(R.Category);Recipes.Add(MoveTemp(R));
    }
    Categories.Sort();return !Recipes.IsEmpty();
}
bool FCatalog::Merge(const FCatalog& Extra)
{
    if(!Error.IsEmpty()||!Extra.Error.IsEmpty()||Extra.Recipes.IsEmpty())return false;
    FCatalog Combined=*this;
    for(const auto& Pair:Extra.Items)Combined.Items.Add(Pair.Key,Pair.Value);
    for(const auto& R:Extra.Recipes)
    {
        if(const auto* Index=Combined.ById.Find(R.Id))Combined.Recipes[*Index]=R;
        else {Combined.ById.Add(R.Id,Combined.Recipes.Num());Combined.Recipes.Add(R);}
    }
    if(Combined.Items.Num()>30000||Combined.Recipes.Num()>30000)return false;
    Combined.ByOutput.Reset();Combined.ByName.Reset();Combined.Categories.Reset();
    for(int32 I=0;I<Combined.Recipes.Num();++I)
    {
        const auto& R=Combined.Recipes[I];Combined.ByOutput.FindOrAdd(R.Output).Add(I);
        Combined.ByName.FindOrAdd(Combined.Name(R.Output)).Add(I);Combined.Categories.AddUnique(R.Category);
    }
    Combined.Categories.Sort();Combined.Version+=TEXT(" + ")+Extra.Version;*this=MoveTemp(Combined);return true;
}
FPlan Plan(const FCatalog& C,int32 Recipe,const FInventory& Inventory,int32 Units,bool Ingredients)
{
    FPlan Invalid;if(!C.Recipes.IsValidIndex(Recipe)||Units<1||Units>MaxUnits){Invalid.Error=TEXT("Choose a recipe and a quantity from 1 to 100,000.");return Invalid;}
    const auto& R=C.Recipes[Recipe];FPlanner P(C,Inventory,Ingredients);P.Visiting.Add(R.Output);
    const int32 Batches=(Units+R.Yield-1)/R.Yield;
    P.Result.Valid=P.Expand(Recipe,Batches,0);P.Result.Units=Batches*R.Yield;return MoveTemp(P.Result);
}
FPlan Maximum(const FCatalog& C,int32 Recipe,const FInventory& Inventory,bool Ingredients)
{
    if(!C.Recipes.IsValidIndex(Recipe))return Plan(C,Recipe,Inventory,0,Ingredients);
    const auto& R=C.Recipes[Recipe];
    if((!R.SourceCount&&!R.TargetCount)||R.Output==R.Source||R.Output==R.Target)
    {FPlan P;P.Error=TEXT("This recipe has no finite ingredient limit. Set a quantity instead of Max available.");return P;}
    int32 Low=0,High=FMath::Min(MaxOperations,MaxUnits/R.Yield);FPlan Best;
    while(Low<High)
    {
        const int32 Mid=Low+(High-Low+1)/2;auto P=Plan(C,Recipe,Inventory,Mid*R.Yield,Ingredients);
        if(P.Valid){Low=Mid;Best=MoveTemp(P);}else High=Mid-1;
    }
    return Low?Best:Plan(C,Recipe,Inventory,R.Yield,Ingredients);
}
FPlan InspectMissing(const FCatalog& C,int32 Recipe,const FInventory& Inventory,int32 Units,bool Ingredients)
{
    if(!C.Recipes.IsValidIndex(Recipe)||Units<1||Units>MaxUnits)return Plan(C,Recipe,Inventory,Units,Ingredients);
    const auto& R=C.Recipes[Recipe];FPlanner P(C,Inventory,Ingredients);P.Inspect=true;P.Visiting.Add(R.Output);
    const int32 Batches=(Units+R.Yield-1)/R.Yield;
    P.Expand(Recipe,Batches,0);P.Result.Units=Batches*R.Yield;
    // Diagnostic steps can depend on hypothetical supplies. Keep Valid false
    // even if an alternate ingredient route happens to need no purchases.
    return MoveTemp(P.Result);
}
EOutcome Outcome(const FRecipe& R,const FInventory& Before,const FInventory& After)
{
    TSet<int32> Keys={R.Source,R.Target,R.Output};if(R.FailOutput)Keys.Add(R.FailOutput);
    bool Changed=false;for(int32 Id:Keys)
    {
        Changed|=After.Receipt(Id)!=Before.Receipt(Id);
    }
    if(!Changed)return EOutcome::Waiting;
    auto Matches=[&](bool Success)
    {
        const bool OptionalSource=Success?R.OptionalSource:R.OptionalFailSource,OptionalTarget=Success?R.OptionalTarget:R.OptionalFailTarget;
        for(int32 Mask=0;Mask<4;++Mask)
        {
            if(((Mask&1)&&!OptionalSource)||((Mask&2)&&!OptionalTarget))continue;
            TMap<int32,int64> Expected;
            Expected.FindOrAdd(R.Source)-=(Mask&1)?0:(Success?R.SourceCount:R.FailSource);
            Expected.FindOrAdd(R.Target)-=(Mask&2)?0:(Success?R.TargetCount:R.FailTarget);
            Expected.FindOrAdd(Success?R.Output:R.FailOutput)+=Success?R.Yield:R.FailYield;
            bool Match=true;for(int32 Id:Keys)if(After.Receipt(Id)-Before.Receipt(Id)!=Expected.FindRef(Id)){Match=false;break;}
            if(Match)return true;
        }
        return false;
    };
    if(Matches(true))return EOutcome::Success;
    if(Matches(false))return EOutcome::Failure;
    return EOutcome::Unexpected;
}
}
