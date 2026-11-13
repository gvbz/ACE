#include "Mods/ACEAutoCraft.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEItemTools.h"
#include "ACEPlayerController.h"
#include "Engine/GameInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using namespace ACEAutoCraft;
bool Option(const TSharedPtr<FJsonObject>& P,const TCHAR* Key,bool Default=false){bool V=Default;P->TryGetBoolField(Key,V);return V;}
// Retained tools may be used only where neither success nor failure consumes
// them. Protected ingredients, worn items, traded items and full packs stay out.
bool Eligible(UACEClientSubsystem* C,FACEWorldObject Item,bool Tool)
{
    if(Tool)Item.ObjectDescriptionFlags&=~ACEObjectDescFlag::Retained;
    return ACEItemTools::Transferable(C,Item);
}
FInventory Inventory(UACEClientSubsystem* C)
{
    FInventory Result;Result.HasReceipts=true;if(auto S=C->GetSession())for(const auto& Pair:S->GetWorldObjects())
    {
        const auto& I=Pair.Value;
        const int32 Player=C->GetPlayerGuid();
        const bool Equipped=Player&&(I.WielderId==Player||(I.CurrentWieldedLocation&&I.ParentGuid==Player));
        if(!C->IsOwnedInventoryItem(I)&&!Equipped)continue;
        Result.ReceiptCounts.FindOrAdd(I.WeenieClassId)+=FMath::Max(1,I.StackSize);
        if(!Eligible(C,I,true))continue;
        auto& Counts=(I.ObjectDescriptionFlags&ACEObjectDescFlag::Retained)?Result.RetainedTools:Result.Counts;
        Counts.FindOrAdd(I.WeenieClassId)+=FMath::Max(1,I.StackSize);
    }
    return Result;
}
int32 Choose(UACEClientSubsystem* C,int32 Wcid,int32 Amount,bool Tool,int32 Exclude=0)
{
    int32 Best=0,Size=MAX_int32;if(auto S=C->GetSession())for(const auto& Pair:S->GetWorldObjects())
    {
        const auto& I=Pair.Value;const int32 Count=FMath::Max(1,I.StackSize);
        if(I.Guid==Exclude||I.WeenieClassId!=Wcid||Count<Amount||!Eligible(C,I,Tool))continue;
        if(Count<Size||(Count==Size&&uint32(I.Guid)<uint32(Best))){Best=I.Guid;Size=Count;}
    }
    return Best;
}
bool OutputFits(UACEClientSubsystem* C,const FCatalog& Catalog,const FRecipe& R,int32 Source,int32 Target,bool Success)
{
    const int32 Output=Success?R.Output:R.FailOutput;
    if(!Output||(!Success&&!R.FailYield))return true;
    const auto* Item=Catalog.Items.Find(Output);if(!Item)return false;
    const bool PackSlot=Item->PackSlot||(Item->Type&ACEItemType::Container);
    auto UsesPackSlot=[](const FACEWorldObject& O){return ACEInventoryRules::IsContainer(O)||(O.ItemType&ACEItemType::Container)||(O.ObjectDescriptionFlags&ACEObjectDescFlag::RequiresPackSlot);};
    const auto S=C->GetSession();const auto Offered=C->GetTradeSelfItems();TMap<int32,int32> Counts{{C->GetPlayerGuid(),0}};
    for(const auto& Pack:C->GetPlayerPacks())if(!Offered.Contains(Pack.Guid))Counts.Add(Pack.Guid,0);
    for(const auto& Pair:S->GetWorldObjects())
    {
        const auto& O=Pair.Value;if(O.WielderId||O.ParentGuid||O.CurrentWieldedLocation||UsesPackSlot(O)!=PackSlot)continue;
        if(auto* Count=Counts.Find(O.ContainerId))
        {
            // ACE creates the result after consuming operands. A slot freed by
            // a guaranteed complete consumption can hold the result.
            const bool FreedSource=O.Guid==Source&&!(Success?R.OptionalSource:R.OptionalFailSource)&&(Success?R.SourceCount:R.FailSource)>=FMath::Max(1,O.StackSize);
            const bool FreedTarget=O.Guid==Target&&!(Success?R.OptionalTarget:R.OptionalFailTarget)&&(Success?R.TargetCount:R.FailTarget)>=FMath::Max(1,O.StackSize);
            if(!FreedSource&&!FreedTarget)++*Count;
        }
    }
    for(auto& Pair:Counts)
    {
        if(const auto* Contents=S->GetContainerContents(Pair.Key))for(const auto& Ref:*Contents)
            if(!S->GetWorldObjects().Contains(Ref.ItemGuid)&&(Ref.ContainerType!=0)==PackSlot)++Pair.Value;
        FACEWorldObject Pack;if(!C->GetWorldObject(Pair.Key,Pack))continue;
        const int32 Capacity=PackSlot?Pack.ContainersCapacity:Pack.ItemsCapacity;
        if(Capacity<0||Pair.Value<Capacity)return true;
    }
    return false;
}
FString Describe(const FCatalog& C,int32 Selected,const FPlan& P,const FInventory& I,bool Ingredients,FString& Summary)
{
    Summary=P.Valid?FString::Printf(TEXT("Inventory ready: materials for %s output items."),*FText::AsNumber(P.Units).ToString())
        :P.Error;
    FString Out;
    if(!P.Valid)
    {
        Out=P.Error+TEXT("\n");
        const auto Report=P.Missing.IsEmpty()?P:InspectMissing(C,Selected,I,FMath::Min(P.Units,MaxUnits),Ingredients);
        if(!Report.Missing.IsEmpty())
        {
            Summary=FString::Printf(TEXT("Missing %d material/tool types for %s output items."),Report.Missing.Num(),*FText::AsNumber(P.Units).ToString());
            Out=Summary+TEXT("\n\nMissing for this ingredient route:\n");
            TArray<int32> Keys;Report.Missing.GetKeys(Keys);Keys.Sort([&](int32 A,int32 B){return C.Name(A)<C.Name(B);});
            TArray<FString> Shortages;
            for(int32 Id:Keys)
            {
                if(Shortages.Num()<3)Shortages.Add(FString::Printf(TEXT("%lld %s"),Report.Missing.FindRef(Id),*C.Name(Id)));
                const bool Tool=Report.Tools.Contains(Id)&&Report.Consumed.FindRef(Id)<=0;
                Out+=FString::Printf(TEXT("  %s: missing %lld | owned %lld%s\n"),*C.Name(Id),Report.Missing.FindRef(Id),I.Receipt(Id),Tool?TEXT(" | reusable tool"):TEXT(""));
                const int64 Protected=FMath::Max<int64>(0,I.Receipt(Id)-I.Available(Id,Tool));
                if(Protected)Out+=FString::Printf(TEXT("    %lld unavailable: retained, equipped, offered in trade, or a nonempty container.\n"),Protected);
            }
            Summary=TEXT("Missing: ")+FString::Join(Shortages,TEXT("; "));
            if(Keys.Num()>3)Summary+=FString::Printf(TEXT("; and %d more below"),Keys.Num()-3);
            if(!Report.Error.IsEmpty())Out+=Report.Error+TEXT("\n");
            if(Ingredients)Out+=TEXT("Obtain these supplies, or select another recipe. Missing intermediates are expanded into their ingredients where possible.\n");
        }
    }
    else Out=FString::Printf(TEXT("%s output items  |  %s crafting operations\n"),*FText::AsNumber(P.Units).ToString(),*FText::AsNumber(P.Operations).ToString());
    const auto& SelectedRecipe=C.Recipes[Selected];const int32 Batches=FMath::Max(1,P.Units/SelectedRecipe.Yield);
    TMap<int32,int64> Inputs;
    Inputs.FindOrAdd(SelectedRecipe.Source)+=FMath::Max<int64>(1,int64(SelectedRecipe.SourceCount)*Batches);
    Inputs.FindOrAdd(SelectedRecipe.Target)+=FMath::Max<int64>(1,int64(SelectedRecipe.TargetCount)*Batches);
    Out+=TEXT("\nRecipe inputs (owned / usable / needed):\n");
    TArray<int32> InputIds;Inputs.GetKeys(InputIds);InputIds.Sort([&](int32 A,int32 B){return C.Name(A)<C.Name(B);});
    for(int32 Id:InputIds)
    {
        const bool Tool=(Id!=SelectedRecipe.Source||SelectedRecipe.SourceReusable())&&(Id!=SelectedRecipe.Target||SelectedRecipe.TargetReusable());
        Out+=FString::Printf(TEXT("  %s: %lld / %lld / %lld%s\n"),*C.Name(Id),I.Receipt(Id),I.Available(Id,Tool),Inputs[Id],Tool?TEXT(" (reusable tool)"):TEXT(""));
        const int64 Protected=FMath::Max<int64>(0,I.Receipt(Id)-I.Available(Id,Tool));
        if(Protected)Out+=FString::Printf(TEXT("    %lld unavailable: retained, equipped, offered in trade, or a nonempty container.\n"),Protected);
    }
    if(Ingredients)Out+=TEXT("Inputs not already in your packs may be made from the materials below.\n");
    Out+=TEXT("Counts include all your packs. Usable counts exclude protected items.\n");
    if(!P.Valid)return Out;
    Out+=TEXT("\nMaterials from inventory (usable / needed for a successful run):\n");
    TArray<int32> Keys;P.Consumed.GetKeys(Keys);Keys.Sort([&](int32 A,int32 B){return C.Name(A)<C.Name(B);});
    for(int32 Id:Keys)
    {
        // Intermediate products are displayed separately in the step list.
        const int64 Used=FMath::Min(P.Consumed.FindRef(Id),I.Counts.FindRef(Id));if(!Used)continue;
        Out+=FString::Printf(TEXT("  %s: %lld / %lld\n"),*C.Name(Id),I.Counts.FindRef(Id),Used);
    }
    if(!P.Tools.IsEmpty())
    {
        Out+=TEXT("Reusable tools:\n");P.Tools.GetKeys(Keys);Keys.Sort();
        for(int32 Id:Keys)Out+=FString::Printf(TEXT("  %s: %lld / %lld\n"),*C.Name(Id),I.Available(Id,true),P.Tools.FindRef(Id));
    }
    Out+=TEXT("Crafting steps:\n");int32 Shown=0;
    for(const auto& Step:P.Steps)
    {
        const auto& R=C.Recipes[Step.Recipe];
        Out+=FString::Printf(TEXT("  %d x %s + %s -> %s (%d each)\n"),Step.Batches,*C.Name(R.Source),*C.Name(R.Target),*C.Name(R.Output),R.Yield);
        if(++Shown>=40){Out+=TEXT("  Additional steps omitted from preview.\n");break;}
    }
    return Out;
}
}

void UACEPluginSubsystem::LoadAutoCraftCatalog(FACEAutoCraftState& A)
{
    A.Loaded=true;A.CatalogServer=GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->GetServerName();
    if(auto Tool=Find(TEXT("autocraft")))A.Catalog.Load(Tool->Directory/TEXT("catalog.json"));
    else {A.Catalog.Error=TEXT("Bundled crafting catalog is unavailable.");return;}
    if(!A.CatalogServer.IsEmpty())
    {
        const FString Path=UserDirectory()/TEXT("Crafting")/(FMD5::HashAnsiString(*A.CatalogServer)+TEXT(".json"));
        if(IFileManager::Get().FileExists(*Path))
        {
            ACEAutoCraft::FCatalog Custom;
            if(!Custom.Load(Path)||!A.Catalog.Merge(Custom))
                A.Catalog.Error=TEXT("The server recipe catalog is invalid. Import a corrected catalog or remove it in Auto Craft.");
        }
    }
    A.SupplyRecipes.Reset();for(const auto& Pair:A.Catalog.ByName)A.SupplyRecipes.FindOrAdd(Pair.Key.ToLower()).Append(Pair.Value);
}

bool UACEPluginSubsystem::ImportAutoCraftCatalog(const FString& Path)
{
    auto Tool=Find(TEXT("autocraft"));if(!Tool)return false;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto Fail=[&](const FString& Message){Tool->Status=Message;return false;};
    if(C->GetSessionState()!=EACESessionState::InWorld||C->GetServerName().IsEmpty())return Fail(TEXT("Log into the server this recipe catalog belongs to first."));
    if(Tool->Running||(UCMCraft&&UCMCraft->Selected!=INDEX_NONE))return Fail(TEXT("Finish or stop the active crafting job before changing recipes."));
    FString Text;ACEAutoCraft::FCatalog Custom,Combined;TSharedPtr<FJsonObject> Json;
    const int64 Size=IFileManager::Get().FileSize(*Path);
    if(Size<1||Size>16*1024*1024||!FFileHelper::LoadFileToString(Text,*Path)
        ||Text.Len()>16*1024*1024||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json)
        ||!Custom.Parse(Json)||!Combined.Load(Tool->Directory/TEXT("catalog.json"))||!Combined.Merge(Custom))
        return Fail(TEXT("Invalid or oversized recipe catalog; existing recipes were kept."));
    const FString Destination=UserDirectory()/TEXT("Crafting")/(FMD5::HashAnsiString(*C->GetServerName())+TEXT(".json"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination),true);
    if(!FFileHelper::SaveStringToFile(Text,*(Destination+TEXT(".tmp")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        ||!IFileManager::Get().Move(*Destination,*(Destination+TEXT(".tmp")),true,true))return Fail(TEXT("Could not save server recipes; existing recipes were kept."));
    AutoCraft.Reset();UCMCraft.Reset();RefreshAutoCraft(true);
    Tool->Status=FString::Printf(TEXT("Imported %d recipes for %s. Available in Auto Craft and UCM."),Custom.Recipes.Num(),*C->GetServerName());return true;
}

bool UACEPluginSubsystem::RemoveAutoCraftCatalog()
{
    auto Tool=Find(TEXT("autocraft"));if(!Tool)return false;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(C->GetSessionState()!=EACESessionState::InWorld||C->GetServerName().IsEmpty()||Tool->Running||(UCMCraft&&UCMCraft->Selected!=INDEX_NONE))
    {Tool->Status=TEXT("Log in and finish or stop active crafting before removing server recipes.");return false;}
    const FString Path=UserDirectory()/TEXT("Crafting")/(FMD5::HashAnsiString(*C->GetServerName())+TEXT(".json"));
    if(!IFileManager::Get().Delete(*Path,false,true)){Tool->Status=TEXT("Could not remove server recipes.");return false;}
    AutoCraft.Reset();UCMCraft.Reset();RefreshAutoCraft(true);Tool->Status=TEXT("Using bundled recipes for this server.");return true;
}

void UACEPluginSubsystem::RefreshAutoCraft(bool Force)
{
    using namespace ACEAutoCraft;
    auto P=Find(TEXT("autocraft"));if(!P)return;
    if(!AutoCraft)AutoCraft=MakeShared<FACEAutoCraftState>();auto& A=*AutoCraft;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto S=C->GetSession();
    if(!A.Loaded||A.CatalogServer!=C->GetServerName()){LoadAutoCraftCatalog(A);Force=true;}
    const FString Character=C->GetSessionState()==EACESessionState::InWorld?FString::Printf(TEXT("Character_%08X_%08X"),uint32(C->GetPlayerGuid()),FCrc::StrCrc32(*C->GetServerName())):FString();
    if(A.Character!=Character)
    {
        Stop(P->Id,TEXT("Ready"));A.Character=Character;A.Pending=false;A.Paused=false;A.Completed=0;A.Attempts=0;A.Failures=0;Force=true;
        if(!Character.IsEmpty()&&!LoadProfile(P->Id,Character))
        {
            FString Json;TSharedPtr<FJsonObject> Default;FFileHelper::LoadFileToString(Json,*(P->Directory/TEXT("default.json")));
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Default);P->Profile=Default?Default:MakeShared<FJsonObject>();P->ProfileName=Character;
        }
    }
    const double Now=FPlatformTime::Seconds();if(!Force&&Now<A.NextRefresh)return;A.NextRefresh=Now+.5;
    const uint64 Revision=S?S->GetInventoryDataRevision():0;
    const FString Key=FString::Printf(TEXT("%s/%s/%d/%d/%d"),*P->ProfileName,*ACEItemTools::String(P->Profile,TEXT("recipe")),int32(ACEItemTools::Number(P->Profile,TEXT("quantity"),1)),Option(P->Profile,TEXT("maximum")),Option(P->Profile,TEXT("ingredients"),true));
    auto Offered=C->GetTradeSelfItems();Offered.Sort();FString InventoryKey=Key;
    for(int32 Id:Offered)InventoryKey+=FString::Printf(TEXT("/%08X"),uint32(Id));
    if(!Force&&A.Revision==Revision&&A.ProfileSignature==InventoryKey)return;
    A.Revision=Revision;A.ProfileSignature=InventoryKey;A.Inventory=Inventory(C);
    const auto* Index=A.Catalog.ById.Find(ACEItemTools::String(P->Profile,TEXT("recipe")));A.Selected=Index?*Index:INDEX_NONE;
    if(!A.Catalog.Error.IsEmpty()){A.Preview=FPlan();A.Details=A.InventorySummary=A.Catalog.Error;return;}
    if(Character.IsEmpty()){A.Preview=FPlan();A.Details=A.InventorySummary=TEXT("Log into a character to check crafting supplies in your packs.");return;}
    if(A.Selected==INDEX_NONE){A.Preview=FPlan();A.InventorySummary=TEXT("Inventory detected. Choose an item to check its materials.");A.Details=TEXT("Choose a category and an item to see its recipe and ingredients.");return;}
    const bool Ingredients=Option(P->Profile,TEXT("ingredients"),true);
    A.Preview=Option(P->Profile,TEXT("maximum"))?Maximum(A.Catalog,A.Selected,A.Inventory,Ingredients):Plan(A.Catalog,A.Selected,A.Inventory,int32(ACEItemTools::Number(P->Profile,TEXT("quantity"),1)),Ingredients);
    A.Details=Describe(A.Catalog,A.Selected,A.Preview,A.Inventory,Ingredients,A.InventorySummary);
}

bool UACEPluginSubsystem::StartAutoCraft()
{
    using namespace ACEAutoCraft;
    auto P=Find(TEXT("autocraft"));if(!P)return false;if(P->Running)return true;
    RefreshAutoCraft(true);auto& A=*AutoCraft;auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto S=C->GetSession();
    auto Fail=[&](const FString& Message){P->Status=Message;return false;};
    if(!P->Enabled||!P->Permissions.Contains(TEXT("inventory")))return Fail(TEXT("Enable Auto Craft in Plugin Manager first."));
    if(!S||C->GetSessionState()!=EACESessionState::InWorld||!C->GetPlayerVitalsView().bValid||C->GetPlayerVitalsView().Health<=0)return Fail(TEXT("Log into a living character first."));
    if(C->IsUseBusy()||PendingSpell||ConfirmedBuffAckAt||InventoryInteractionPending()||!S->GetConfirmations().IsEmpty())return Fail(TEXT("Finish the current action or confirmation before starting crafting."));
    if(UCMCraft&&UCMCraft->Selected!=INDEX_NONE)return Fail(TEXT("Finish or stop the UCM crafting job first."));
    if(!A.Preview.Valid)return Fail(A.InventorySummary);
    const double Quantity=ACEItemTools::Number(P->Profile,TEXT("quantity"),1),Delay=ACEItemTools::Number(P->Profile,TEXT("delay_ms"),250);
    if(Quantity<1||Quantity>MaxUnits||FMath::FloorToDouble(Quantity)!=Quantity||Delay<100||Delay>5000)return Fail(TEXT("Invalid crafting quantity or delay."));
    for(const auto& Other:Plugins)if(Other->Running&&!Other->LocalService&&!Other->Companion&&Other->Id!=TEXT("ucm"))Stop(Other->Id,TEXT("Auto Craft started"));
    MovementOwner.Empty();MoveExpires=0;FastCastOwner.Empty();OptionalCastOwner.Empty();FaceHeading.Reset();DeferredBuffIntent.Reset();DeferredBuffOwner.Empty();
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController())){PC->EndUseApproach();PC->CancelPluginJump();}
    UseApproachOwner.Empty();S->SendCancelAttack();PhysicalAttackOwner.Empty();PhysicalAttackTarget=0;
    A.Goal=A.Preview.Units;A.Completed=0;A.Attempts=0;A.Failures=0;A.Paused=false;A.Pending=false;A.Preparation.Empty();A.NextAction=0;A.Deadline=FPlatformTime::Seconds()+30;
    P->Player=C->GetPlayerGuid();P->Server=C->GetServerName();P->Running=true;P->Status=TEXT("Preparing to craft");return true;
}

void UACEPluginSubsystem::PauseAutoCraft()
{
    if(auto P=Find(TEXT("autocraft"));P&&P->Running&&AutoCraft)
    {AutoCraft->Paused=!AutoCraft->Paused;P->Status=AutoCraft->Paused?TEXT("Paused; the current craft may finish"):TEXT("Resuming crafting");}
}

void UACEPluginSubsystem::ExtendUCMCraftSnapshot(const TSharedPtr<FJsonObject>& Out)
{
    using namespace ACEAutoCraft;
    auto P=PolicyPlugin();auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto S=C->GetSession();
    if(!P||!P->Running||!S)return;
    const auto Config=UCMServicesProfile?UCMServicesProfile:P->Profile;
    if(!Option(Config,TEXT("auto_craft"))){UCMCraft.Reset();return;}
    if(!UCMCraft)UCMCraft=MakeShared<FACEAutoCraftState>();auto& A=*UCMCraft;
    const FString Character=FString::Printf(TEXT("%s/%08X"),*C->GetServerName(),uint32(C->GetPlayerGuid()));
    if(A.Character!=Character){A=FACEAutoCraftState();A.Character=Character;}
    if(!A.Loaded)
    {
        LoadAutoCraftCatalog(A);
    }
    auto View=MakeShared<FJsonObject>();View->SetBoolField(TEXT("pending"),A.Selected!=INDEX_NONE);
    if(A.Selected!=INDEX_NONE){View->SetStringField(TEXT("name"),A.Catalog.Name(A.Catalog.Recipes[A.Selected].Output));Out->SetObjectField(TEXT("crafting"),View);return;}
    const double Now=FPlatformTime::Seconds();
    // Keep catalog/planner work out of the per-frame policy loop. Requests are
    // checked against fresh inventory again immediately before execution.
    if(Now>=A.NextRefresh&&A.Catalog.Error.IsEmpty())
    {
        A.NextRefresh=Now+1;A.Inventory=Inventory(C);A.Candidates.Reset();
        struct FSupply{FString Name;int32 Type,Minimum;};TArray<FSupply> Supplies;TMap<FString,int32> SupplyIndex;
        static const TCHAR* Idle[]={TEXT("craft_health_kits"),TEXT("craft_health_food"),TEXT("craft_stamina_kits"),TEXT("craft_stamina_food"),TEXT("craft_mana_kits"),TEXT("craft_mana_food")};
        for(const TCHAR* Key:{TEXT("assist_items"),TEXT("buff_items"),TEXT("craft_targets")})
        {
            const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;if(!Config->TryGetArrayField(Key,Rows))continue;
            for(const auto& V:*Rows)
            {
                const auto O=V->AsObject();if(!O)continue;
                const FString Name=ACEItemTools::String(O,TEXT("name"));const int32 Type=FString(Key)==TEXT("assist_items")?int32(ACEItemTools::Number(O,TEXT("type"),-1)):6;
                if(Name.IsEmpty()||Type<0||Type==9||Name.StartsWith(TEXT("[")))continue;
                const int32 Minimum=Type<6?FMath::Clamp(int32(ACEItemTools::Number(Config,Idle[Type],0)),0,10000):FMath::Clamp(int32(ACEItemTools::Number(O,TEXT("count"),1)),1,10000);
                const FString KeyName=Name.ToLower();
                if(auto* Existing=SupplyIndex.Find(KeyName))
                {Supplies[*Existing].Minimum=FMath::Max(Supplies[*Existing].Minimum,Minimum);Supplies[*Existing].Type=FMath::Min(Supplies[*Existing].Type,Type);}
                else {SupplyIndex.Add(KeyName,Supplies.Num());Supplies.Add({Name,Type,Minimum});}
            }
        }
        for(const auto& Supply:Supplies)
        {
            if(A.Candidates.Num()>=64)break;
            const FString Key=Supply.Name.ToLower();if(A.BlockedSupplies.Contains(Key))continue;
            const auto* Named=A.SupplyRecipes.Find(Key);if(!Named)continue;
            // Different WCIDs can share a name. Count the whole named supply,
            // rather than crafting an alternate variant beside existing stock.
            TSet<int32> Outputs;int64 Count=0;
            for(int32 Index:*Named){const int32 Output=A.Catalog.Recipes[Index].Output;if(!Outputs.Contains(Output)){Outputs.Add(Output);Count+=A.Inventory.Receipt(Output);}}
            if(Count>=FMath::Max(1,Supply.Minimum))continue;
            for(int32 Index:*Named)
            {
                const auto& R=A.Catalog.Recipes[Index];const auto Preview=Plan(A.Catalog,Index,A.Inventory,R.Yield,true);if(!Preview.Valid)continue;
                bool Trained=true;for(const auto& Step:Preview.Steps)
                {
                    const auto& Recipe=A.Catalog.Recipes[Step.Recipe];
                    if(TArray<int32>{18,28,29,30,37,38,39,48}.Contains(Recipe.Skill)&&Recipe.Difficulty)
                    {const auto* Skill=C->GetPlayerVitalsView().Skills.FindByPredicate([&](const auto& V){return V.SkillId==Recipe.Skill;});if(!Skill||Skill->AdvancementClass<2)Trained=false;}
                }
                if(!Trained)continue;
                auto Candidate=MakeShared<FJsonObject>();Candidate->SetStringField(TEXT("recipe"),R.Id);Candidate->SetStringField(TEXT("name"),Supply.Name);
                Candidate->SetStringField(TEXT("phase"),Count>0?TEXT("idle"):Supply.Type<6?TEXT("critical"):TEXT("normal"));
                Candidate->SetNumberField(TEXT("count"),Count);Candidate->SetNumberField(TEXT("minimum"),FMath::Max(1,Supply.Minimum));A.Candidates.Add(MakeShared<FJsonValueObject>(Candidate));break;
            }
        }
    }
    View->SetArrayField(TEXT("candidates"),A.Candidates);View->SetStringField(TEXT("error"),A.Catalog.Error);Out->SetObjectField(TEXT("crafting"),View);
}

void UACEPluginSubsystem::ExecuteUCMCraft(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& I)
{
    if(P.Id!=TEXT("ucm")||!Option(UCMServicesProfile?UCMServicesProfile:P.Profile,TEXT("auto_craft"))||!P.Permissions.Contains(TEXT("inventory")))return;
    if(auto Tool=Find(TEXT("autocraft"));Tool&&Tool->Running)return;
    if(UCMCraft&&UCMCraft->Selected==INDEX_NONE)UCMCraft->NextRefresh=0;
    auto Snapshot=MakeShared<FJsonObject>();ExtendUCMCraftSnapshot(Snapshot);if(!UCMCraft)return;auto& A=*UCMCraft;
    if(A.Selected==INDEX_NONE)
    {
        const FString Id=ACEItemTools::String(I,TEXT("recipe"));
        if(!A.Candidates.ContainsByPredicate([&](const auto& V){return ACEItemTools::String(V->AsObject(),TEXT("recipe"))==Id;}))return;
        const auto* Index=A.Catalog.ById.Find(Id);if(!Index)return;
        auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();if(C->IsUseBusy()||!C->GetSession()->GetConfirmations().IsEmpty())return;
        A.Selected=*Index;A.Goal=A.Catalog.Recipes[*Index].Yield;A.Completed=0;A.Attempts=0;A.Failures=0;A.Pending=false;A.Preparation.Empty();A.NextAction=0;A.Deadline=FPlatformTime::Seconds()+30;
    }
    TickAutoCraft(&P);
}

void UACEPluginSubsystem::TickAutoCraft(FACEClientPlugin* Policy)
{
    using namespace ACEAutoCraft;
    auto Standalone=Find(TEXT("autocraft"));auto* P=Policy?Policy:Standalone.Get();auto State=Policy?UCMCraft:AutoCraft;
    if(!P||!P->Running||!State)return;
    auto& A=*State;auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto S=C->GetSession();const double Now=FPlatformTime::Seconds();
    auto Finish=[&](const FString& Message){
        if(Policy&&A.Completed<A.Goal&&A.Catalog.Recipes.IsValidIndex(A.Selected))A.BlockedSupplies.Add(A.Catalog.Name(A.Catalog.Recipes[A.Selected].Output).ToLower());
        A.Pending=false;A.Paused=false;if(Policy)A.Selected=INDEX_NONE;A.Revision=MAX_uint64;A.NextRefresh=0;
        if(!Policy)Stop(P->Id,Message);
        else if(A.Completed>=A.Goal)P->Status=Message;
        else {auto I=MakeShared<FJsonObject>();I->SetStringField(TEXT("activity"),TEXT("crafting"));ReportActivityFailure(*P,I,Message+TEXT(" This supply is skipped until UCM restarts or automatic crafting is toggled."));}};
    if(!S||C->GetSessionState()!=EACESessionState::InWorld||C->GetPlayerGuid()!=P->Player||C->GetServerName()!=P->Server||C->GetPlayerVitalsView().Health<=0)
    {Finish(TEXT("Stopped: character died, changed, or left the world."));return;}
    if(A.Pending)
    {
        const auto& Recipe=A.Catalog.Recipes[A.PendingRecipe];
        if(!A.Preparation.IsEmpty())
        {
            if(S->GetInventoryFailureSerial()!=A.InventoryFailureSerial
                &&(S->GetInventoryFailureItem()==A.SourceGuid||(A.TargetGuid&&S->GetInventoryFailureItem()==A.TargetGuid)))
            {Finish(TEXT("Server rejected ingredient stack preparation. No further actions sent."));return;}
            FACEWorldObject Source,Target;
            const int32 SourceCount=C->GetWorldObject(A.SourceGuid,Source)&&Eligible(C,Source,false)?FMath::Max(1,Source.StackSize):0;
            bool Ready=false;
            if(A.Preparation==TEXT("merge"))
                Ready=SourceCount==A.SourceBefore-A.PrepareAmount&&C->GetWorldObject(A.TargetGuid,Target)&&Eligible(C,Target,false)&&Target.StackSize==A.TargetBefore+A.PrepareAmount;
            else if(SourceCount==A.SourceBefore-A.PrepareAmount)
                for(const auto& Pair:S->GetWorldObjects())
                    if(!A.BeforeIds.Contains(Pair.Key)&&Pair.Value.WeenieClassId==Source.WeenieClassId&&Eligible(C,Pair.Value,false)&&Pair.Value.StackSize==A.PrepareAmount){Ready=true;break;}
            if(Ready)
            {
                if(!A.ResultSeen){A.ResultSeen=true;A.ResultAt=Now;}
                if(Now-A.ResultAt<.35)return;
                A.Pending=false;A.Preparation.Empty();A.BeforeIds.Reset();A.NextAction=Now+.25;A.Revision=MAX_uint64;
            }
            else if(Now>=A.Deadline)Finish(TEXT("Ingredient stack change was not confirmed. Stopped without repeating the transfer."));
            return;
        }
        const auto Confirmations=S->GetConfirmations();
        if(!Confirmations.IsEmpty())
        {
            // Start and dispatch require an empty confirmation queue. Only the
            // single crafting prompt arriving for this outstanding use qualifies.
            if(Confirmations.Num()!=1||Confirmations[0].Type!=5){Finish(TEXT("Another confirmation interrupted crafting."));return;}
            P->Status=TEXT("Waiting for crafting confirmation");
            if(!A.Confirmed&&!A.Paused&&Option(Policy&&UCMServicesProfile?UCMServicesProfile:P->Profile,Policy?TEXT("craft_confirm"):TEXT("confirm"),true)&&P->Permissions.Contains(TEXT("confirm")))
            {
                A.Confirmed=true;A.Serial=ActionSerial;A.Deadline=Now+60;
                S->RespondToConfirmation(Confirmations[0].Type,Confirmations[0].Context,true);
            }
            if(Now>=A.Deadline)Finish(TEXT("Craft confirmation timed out; no further attempts sent."));return;
        }
        // UseDone is shared with manual actions and other plugins. Only a
        // receipt for this request and these two ingredients can reject it.
        // A successful receipt still needs the inventory result below.
        for(const auto& Value:ActionOutcomes)
        {
            const auto Event=Value->AsObject();FString Type;
            if(!Event||!Event->TryGetStringField(TEXT("type"),Type)||Type!=TEXT("use"))continue;
            const auto Number=[&](const TCHAR* Key){return ACEItemTools::Number(Event,Key,0);};
            if(Number(TEXT("source"))!=uint32(A.SourceGuid)||Number(TEXT("target"))!=uint32(A.TargetGuid)
                ||Number(TEXT("requested_at"))<A.RequestedAt||uint32(Number(TEXT("action_serial")))==A.Serial)continue;
            const uint32 Error=uint32(Number(TEXT("error")));
            if(Error){Finish(FString::Printf(TEXT("Server rejected the craft (error %u). No further attempts sent."),Error));return;}
        }
        const auto Current=Inventory(C);auto Result=Outcome(Recipe,A.Before,Current);
        if(A.FailureMessage&&Result==EOutcome::Waiting&&(!Recipe.FailSource||Recipe.OptionalFailSource)&&(!Recipe.FailTarget||Recipe.OptionalFailTarget)&&(!Recipe.FailOutput||!Recipe.FailYield))Result=EOutcome::Failure;
        // An ingredient decrement can arrive seconds before the output create.
        // Never classify it as failure without this recipe's server message.
        if((Result==EOutcome::Failure&&!A.FailureMessage)||Result==EOutcome::Unexpected)
        {
            A.ResultSeen=false;
            if(Now>=A.Deadline)Finish(TEXT("Inventory result did not match the recipe. Stopped without retrying an uncertain craft."));
            return;
        }
        if(Result!=EOutcome::Waiting)
        {
            // Creates, stack decrements and UseDone can be separate messages.
            // Let the transaction settle before classifying partial delivery.
            if(!A.ResultSeen){A.ResultSeen=true;A.ResultAt=Now;}
            if(Now-A.ResultAt<.35)return;
            if(C->IsUseBusy())
            {if(Now>=A.Deadline)Finish(TEXT("Craft result arrived, but the action did not finish. Stopped without sending another craft."));return;}
            ++A.Attempts;A.Pending=false;A.Inventory=Current;A.Revision=MAX_uint64;
            if(Result==EOutcome::Success){if(A.PendingRecipe==A.Selected)A.Completed+=Recipe.Yield;}
            else {++A.Failures;if(Policy||Option(P->Profile,TEXT("stop_on_failure"))){Finish(TEXT("Stopped after a failed crafting attempt."));return;}}
            A.NextAction=Now+FMath::Clamp(ACEItemTools::Number(P->Profile,TEXT("delay_ms"),250),100.,5000.)/1000.;
            P->Status=FString::Printf(TEXT("Crafted %d / %d items  |  %d attempts  |  %d failures"),A.Completed,A.Goal,A.Attempts,A.Failures);
            if(A.Completed>=A.Goal){Finish(FString::Printf(TEXT("Complete: %d items crafted (%d attempts, %d failures)."),A.Completed,A.Attempts,A.Failures));return;}
            if(A.Attempts>=MaxOperations){Finish(TEXT("Reached 10,000 attempts. Start a new job to continue."));return;}
        }
        else {A.ResultSeen=false;if(Now>=A.Deadline)Finish(TEXT("Craft result was not confirmed within 60 seconds. Stopped without retrying an uncertain action."));}
        return;
    }
    if(A.Paused||Now<A.NextAction)return;
    if(InventoryInteractionPending()||C->IsUseBusy()||PendingSpell||ConfirmedBuffAckAt){P->Status=TEXT("Waiting for the current inventory action");return;}
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    if(PC&&PC->IsWorldTransitionActive()){P->Status=TEXT("Waiting for world transition");return;}
    if(!S->GetConfirmations().IsEmpty()){Finish(TEXT("Resolve the pending confirmation, then start crafting again."));return;}
    if(!S->IsCombatModeConfirmed(1))
    {
        if(Now>=A.Deadline){Finish(TEXT("Could not enter peace mode."));return;}
        S->SendChangeCombatMode(1);A.NextAction=Now+1;P->Status=TEXT("Entering peace mode");return;
    }
    A.Inventory=Inventory(C);auto PlanNow=Plan(A.Catalog,A.Selected,A.Inventory,A.Goal-A.Completed,Policy||Option(P->Profile,TEXT("ingredients"),true));
    if(!PlanNow.Valid)
    {
        // Failed attempts can reduce an initial maximum estimate. Continue with
        // one attainable batch, rather than requiring the original full job.
        PlanNow=Plan(A.Catalog,A.Selected,A.Inventory,A.Catalog.Recipes[A.Selected].Yield,Policy||Option(P->Profile,TEXT("ingredients"),true));
        if(!PlanNow.Valid){Finish(FString::Printf(TEXT("Stopped at %d / %d items. %s"),A.Completed,A.Goal,*PlanNow.Error));return;}
    }
    const int32 Index=PlanNow.Steps[0].Recipe;const auto& R=A.Catalog.Recipes[Index];
    const int32 SourceNeed=FMath::Max(1,R.SourceCount),TargetNeed=FMath::Max(1,R.TargetCount);
    const int32 Source=Choose(C,R.Source,SourceNeed,R.SourceReusable());
    const int32 Target=Choose(C,R.Target,TargetNeed,R.TargetReusable(),Source);
    if(!Source||!Target)
    {
        // Two operands must be separate object IDs. Only prepare unprotected
        // stacks; a retained tool is never split, merged or otherwise altered.
        auto Prepare=[&](const FACEWorldObject& From,const FACEWorldObject& To,int32 Amount,int32 Pack)
        {
            A.Pending=true;A.PendingRecipe=Index;A.SourceGuid=From.Guid;A.TargetGuid=To.Guid;A.PrepareAmount=Amount;
            A.SourceBefore=From.StackSize;A.TargetBefore=To.StackSize;A.ResultSeen=false;A.Deadline=Now+60;
            A.InventoryFailureSerial=S->GetInventoryFailureSerial();A.BeforeIds.Reset();
            if(Pack)
            {
                A.Preparation=TEXT("split");for(const auto& Pair:S->GetWorldObjects())A.BeforeIds.Add(Pair.Key);
                P->Status=TEXT("Splitting ingredients into two recipe operands");C->SendStackableSplitToContainer(From.Guid,Pack,0,Amount);
            }
            else {A.Preparation=TEXT("merge");P->Status=TEXT("Combining ingredient stacks");C->SendStackableMerge(From.Guid,To.Guid,Amount);}
        };
        if(R.Source==R.Target)
        {
            FACEWorldObject Stack;const int32 Id=Choose(C,R.Source,SourceNeed+TargetNeed,false);
            if(Id&&C->GetWorldObject(Id,Stack))
            {
                const int32 Pack=C->FindPickupContainer(Id,0,TargetNeed);
                if(!Pack){Finish(TEXT("Free one inventory slot to split ingredients for this recipe."));return;}
                Prepare(Stack,FACEWorldObject(),TargetNeed,Pack);return;
            }
        }
        const bool Same=R.Source==R.Target;
        const int32 Wcid=!Source?R.Source:R.Target,Needed=Same?SourceNeed+TargetNeed:(!Source?SourceNeed:TargetNeed),Exclude=Same?0:(!Source?0:Source);
        FACEWorldObject Largest;
        for(const auto& Pair:S->GetWorldObjects())
        {
            const auto& I=Pair.Value;
            if(I.Guid!=Exclude&&I.WeenieClassId==Wcid&&Eligible(C,I,false)&&I.MaxStackSize>=Needed&&I.StackSize<Needed&&(!Largest.Guid||I.StackSize>Largest.StackSize))Largest=I;
        }
        if(Largest.Guid)for(const auto& Pair:S->GetWorldObjects())
        {
            const auto& I=Pair.Value;if(I.Guid==Exclude||!Eligible(C,I,false))continue;
            const int32 Amount=FMath::Min(Needed-Largest.StackSize,ACEInventoryRules::MergeAmount(I,Largest));
            if(Amount>0){Prepare(I,Largest,Amount,0);return;}
        }
        Finish(TEXT("Recipe needs two eligible item stacks with enough units. Check protected items and stack limits."));return;
    }
    if(TArray<int32>{18,28,29,30,37,38,39,48}.Contains(R.Skill)&&R.Difficulty)
    {
        const auto* Skill=C->GetPlayerVitalsView().Skills.FindByPredicate([&](const auto& V){return V.SkillId==R.Skill;});
        if(!Skill||Skill->AdvancementClass<2){Finish(FString::Printf(TEXT("Train the skill required for %s before crafting."),*A.Catalog.Name(R.Output)));return;}
    }
    if(!OutputFits(C,A.Catalog,R,Source,Target,true)||!OutputFits(C,A.Catalog,R,Source,Target,false))
    {Finish(TEXT("Free an inventory slot for the crafting result before continuing."));return;}
    // Send the exact cookbook direction; never switch source and target by name.
    A.Before=A.Inventory;A.SourceGuid=Source;A.TargetGuid=Target;A.PendingRecipe=Index;A.Pending=true;A.Preparation.Empty();A.Confirmed=false;A.ResultSeen=false;A.FailureMessage=false;A.Serial=ActionSerial;A.RequestedAt=Now;A.Deadline=Now+60;
    P->Status=FString::Printf(TEXT("Making %s  |  %d / %d finished items"),*A.Catalog.Name(R.Output),A.Completed,A.Goal);
    C->SendUseWithTarget(Source,Target);
}
