#include "ACEItemTools.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEVTProfile.h"
#include "ACEPlayerController.h"
#include "ACEVendorPricing.h"
#include "ACESession.h"
#include "UI/ACERetailObjectNames.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

using namespace ACEItemTools;

void UACEPluginSubsystem::UpdateItemToolsCharacter()
{
    auto P=Find(TEXT("itemtools"));auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(!P||C->GetSessionState()!=EACESessionState::InWorld){ItemToolsCharacter.Empty();ItemToolsLastVendor=0;return;}
    const FString Key=FString::Printf(TEXT("Character_%08X_%08X"),uint32(C->GetPlayerGuid()),FCrc::StrCrc32(*C->GetServerName()));
    if(ItemToolsCharacter!=Key)
    {
        Stop(P->Id,TEXT("Ready"));ItemToolsCharacter=Key;ItemToolsLastVendor=0;
        if(!LoadProfile(P->Id,Key))
        {
            FString Json;TSharedPtr<FJsonObject> Default;
            FFileHelper::LoadFileToString(Json,*(P->Directory/TEXT("default.json")));
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json),Default);
            P->Profile=Default?Default:MakeShared<FJsonObject>();P->ProfileName=Key;
        }
        // World GUIDs can be reused. Saved names remain, but each login requires
        // fresh explicit recipient/chest selection before a transfer.
        for(const TCHAR* Field:{TEXT("target_id"),TEXT("chest_id"),TEXT("mule1_id"),TEXT("mule2_id"),TEXT("vendor_id")})P->Profile->RemoveField(Field);
        P->Status=TEXT("Ready");Notice.Empty();
    }
    const int32 Vendor=C->GetOpenVendorGuid();
    const bool Opened=Vendor&&Vendor!=ItemToolsLastVendor;ItemToolsLastVendor=Vendor;
    bool Auto=false;P->Profile->TryGetBoolField(TEXT("auto_restock"),Auto);
    if(Opened&&Auto&&P->Enabled&&!P->Running)
    {
        const TArray<TSharedPtr<FJsonValue>>* List=nullptr;
        if(P->Profile->TryGetArrayField(TEXT("restock"),List))for(const auto& Row:*List)
            for(const auto& Stock:C->GetVendorMerchandise())if(ACERetailObjectNames::Name(Stock)==String(Row->AsObject(),TEXT("name"))&&Stock.WeenieClassId==Number(Row->AsObject(),TEXT("wcid")))
            {StartItemTools(TEXT("restock"));return;}
    }
}

bool UACEPluginSubsystem::StartItemTools(const FString& Mode)
{
    if(!TArray<FString>{TEXT("give"),TEXT("shop"),TEXT("profile"),TEXT("chest"),TEXT("restock"),TEXT("merge")}.Contains(Mode))return false;
    auto P=Find(TEXT("itemtools"));if(!P)return false;
    UpdateItemToolsCharacter();
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    P->Profile->SetStringField(TEXT("mode"),Mode);
    P->Profile->SetNumberField(TEXT("vendor_id"),uint32(C->GetOpenVendorGuid()));
    // Reuse profile validation before allowing any automation to start.
    if(!SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false))return false;
    const TArray<TSharedPtr<FJsonValue>>* ChestRules=nullptr;
    if(Mode==TEXT("chest")&&String(P->Profile,TEXT("key_name")).IsEmpty()&&(!P->Profile->TryGetArrayField(TEXT("chest_rules"),ChestRules)||ChestRules->IsEmpty()))
    {Notice=TEXT("Choose a key or a chest collection profile before starting.");return false;}
    ItemToolsProgress=MakeShared<FJsonObject>();
    if(!Start(P->Id))return false;
    if(Mode==TEXT("merge"))ScheduleDecisionWake(FPlatformTime::Seconds());
    return true;
}

bool UACEPluginSubsystem::RunItemToolsCommand(const FString& Arguments)
{
    const FString Command=Arguments.TrimStartAndEnd().ToLower();
    if(Command.IsEmpty()){TogglePluginWindow(TEXT("itemtools"));return true;}
    if(Command==TEXT("stop")){Stop(TEXT("itemtools"),TEXT("Stopped by player"));return true;}
    if(Command==TEXT("recomp"))return StartItemTools(TEXT("restock"));
    if(TArray<FString>{TEXT("give"),TEXT("shop"),TEXT("profile"),TEXT("chest"),TEXT("restock"),TEXT("merge")}.Contains(Command))return StartItemTools(Command);
    Notice=TEXT("/acueitems [give | shop | profile | chest | restock | merge | stop].");return false;
}

bool UACEPluginSubsystem::SelectItemToolsObject(const FString& Key,const FString& Kind)
{
    UpdateItemToolsCharacter();auto P=Find(TEXT("itemtools"));if(!P||P->Running)return false;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Item;
    const int32 Id=C->GetSelectedObject().Guid;
    bool Found=C->GetWorldObject(Id,Item);
    if(Kind==TEXT("stock"))for(const auto& Stock:C->GetVendorMerchandise())if(Stock.Guid==Id){Item=Stock;Found=true;break;}
    const bool Recipient=Found&&Id!=C->GetPlayerGuid()&&Item.IsSelectableWorldObject()&&!Item.bDying&&Item.IsGiveOrCreatureTarget()&&!Item.IsVendor();
    const bool Chest=Found&&Item.IsSelectableWorldObject()&&Item.IsOpenable()&&!Item.IsDoor()&&!Item.IsCorpse();
    if(!Found||(Kind==TEXT("recipient")&&!Recipient)||(Kind==TEXT("chest")&&!Chest)
        ||(Kind==TEXT("item")&&!Transferable(C,Item))||(Kind==TEXT("stock")&&!C->GetVendorMerchandise().ContainsByPredicate([Id](const auto& V){return V.Guid==Id;})))
    {Notice=TEXT("Select a valid ")+Kind+TEXT(" in the game first. Equipped, retained and traded items are excluded.");return false;}
    if(Kind==TEXT("recipient")||Kind==TEXT("chest"))P->Profile->SetNumberField(*(Key+TEXT("_id")),uint32(Id));
    P->Profile->SetStringField(*(Key+TEXT("_name")),ACERetailObjectNames::Name(Item));
    P->Profile->SetNumberField(*(Key+TEXT("_wcid")),Item.WeenieClassId);
    return SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false);
}

bool UACEPluginSubsystem::LoadItemToolsRules(const FString& Key,const FString& Filename)
{
    if(!TArray<FString>{TEXT("profile"),TEXT("chest"),TEXT("mule1"),TEXT("mule2")}.Contains(Key))return false;
    auto P=Find(TEXT("itemtools"));if(!P||P->Running)return false;
    if(Filename.IsEmpty()){P->Profile->SetArrayField(*(Key+TEXT("_rules")),{});P->Profile->RemoveField(*(Key+TEXT("_file")));}
    else
    {
        if(Filename!=FPaths::GetCleanFilename(Filename)||!GetProfileFiles(TEXT("utl")).Contains(Filename))return false;
        TArray<FString> Issues;const auto Converted=ACEVTProfile::ConvertFile(GetProfileFolder()/Filename,Issues);
        const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;
        if(!Converted||Issues.Num()||!Converted->TryGetArrayField(TEXT("loot_rules"),Rules))
        {Notice=TEXT("Profile not applied: ")+FString::Join(Issues,TEXT("; "));return false;}
        P->Profile->SetArrayField(*(Key+TEXT("_rules")),*Rules);P->Profile->SetStringField(*(Key+TEXT("_file")),Filename);
    }
    return SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false);
}

TSharedPtr<FJsonObject> UACEPluginSubsystem::ItemToolsSnapshot(FACEClientPlugin* Policy)
{
    auto S=Policy?Snapshot():MakeShared<FJsonObject>();auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    const double Now=FPlatformTime::Seconds();auto Builtin=Find(TEXT("itemtools"));auto* P=Policy?Policy:Builtin.Get();
    const bool Merging=!Policy&&P&&String(P->Profile,TEXT("mode"))==TEXT("merge");
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    if(!Policy)
    {
        S->SetNumberField(TEXT("time"),Now);S->SetBoolField(TEXT("ready"),!P||Now>=P->NextAction);
        S->SetBoolField(TEXT("busy"),C->IsUseBusy()||PendingSpell||ConfirmedBuffAckAt||(PC&&(PC->IsWorldTransitionActive()||PC->IsUseApproachActive())));
        if(!Merging){S->SetObjectField(TEXT("position"),MakeShared<FJsonObject>());ExtendSnapshot(S);}
    }
    S->SetNumberField(TEXT("it_failure_serial"),Session->GetInventoryFailureSerial());
    S->SetNumberField(TEXT("it_failure_item"),uint32(Session->GetInventoryFailureItem()));
    S->SetNumberField(TEXT("it_failure_error"),Session->GetInventoryFailureError());
    if(Merging)
    {
        S->SetNumberField(TEXT("action_serial"),ActionSerial);
        S->SetNumberField(TEXT("action_error"),LastActionError);
        S->SetBoolField(TEXT("portal_space"),MetaPortalSpace);
        TArray<TSharedPtr<FJsonValue>> Stacks;
        for(const auto& Pair:Session->GetWorldObjects())
        {
            const auto& Item=Pair.Value;
            if(Item.MaxStackSize<=1||!IsOwnedPluginItem(Item))continue;
            const int32 Count=FMath::Max(1,Item.StackSize);
            auto Row=MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("id"),uint32(Item.Guid));
            Row->SetNumberField(TEXT("wcid"),Item.WeenieClassId);
            Row->SetNumberField(TEXT("count"),Count);
            Row->SetNumberField(TEXT("max_stack"),Item.MaxStackSize);
            Row->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));
            // Full targets still belong in the receipt view.
            Row->SetBoolField(TEXT("transferable"),Count<Item.MaxStackSize&&Transferable(C,Item));
            Stacks.Add(MakeShared<FJsonValueObject>(Row));
        }
        S->SetArrayField(TEXT("inventory"),Stacks);
        return S;
    }
    S->SetNumberField(TEXT("it_currency"),Session->GetVendorCurrencyCount());
    TArray<TSharedPtr<FJsonValue>> Targets;
    FACEPosition Here=C->GetPlayerPosition();if(PC)PC->TryGetLocallyPredictedPosition(Here);
    ExtendItemInventorySnapshot(S);
    for(const auto& Pair:Session->GetWorldObjects())
    {
        const auto& Item=Pair.Value;if(!Item.IsSelectableWorldObject()||Item.bDying)continue;
        const double Distance=FVector::Distance(Here.ToUnrealLocation(),Item.Position.ToUnrealLocation())/100.;
        if(Distance>10)continue;
        auto J=MakeShared<FJsonObject>();J->SetNumberField(TEXT("id"),uint32(Item.Guid));J->SetStringField(TEXT("name"),ACERetailObjectNames::Name(Item));
        J->SetNumberField(TEXT("distance"),Distance);J->SetBoolField(TEXT("recipient"),Item.Guid!=C->GetPlayerGuid()&&Item.IsGiveOrCreatureTarget()&&!Item.IsVendor());
        J->SetBoolField(TEXT("player"),Item.bIsPlayer);J->SetBoolField(TEXT("chest"),Item.IsOpenable()&&!Item.IsDoor()&&!Item.IsCorpse());
        const auto* A=Appraisals.Find(Item.Guid);J->SetBoolField(TEXT("identified"),A&&A->bSuccess);
        if(A&&A->bSuccess)J->SetBoolField(TEXT("locked"),A->BoolProperties.FindRef(3));
        Targets.Add(MakeShared<FJsonValueObject>(J));
    }
    S->SetArrayField(TEXT("it_objects"),Targets);
    TSet<FString> Wanted;
    if(P){Wanted.Add(String(P->Profile,TEXT("buy_name")));const TArray<TSharedPtr<FJsonValue>>* Supplies=nullptr;
        if(P->Profile->TryGetArrayField(TEXT("restock"),Supplies))for(const auto& Row:*Supplies)Wanted.Add(String(Row->AsObject(),TEXT("name")));}
    TArray<TSharedPtr<FJsonValue>> Stock;const auto Merchandise=C->GetVendorMerchandise();
    for(const auto& V:S->GetArrayField(TEXT("vendor_stock")))
    {
        auto J=MakeShared<FJsonObject>(*V->AsObject());int32 Room=0;
        for(const auto& Item:Merchandise)if(Item.Guid==Guid(J,TEXT("id"))&&Wanted.Contains(String(J,TEXT("name"))))
        {
            Room=PurchaseRoom(C,Item);break;
        }
        J->SetNumberField(TEXT("room"),Room);Stock.Add(MakeShared<FJsonValueObject>(J));
    }
    S->SetArrayField(TEXT("vendor_stock"),Stock);return S;
}

bool UACEPluginSubsystem::ExecuteItemTools(FACEClientPlugin& P,const TSharedPtr<FJsonObject>& I)
{
    const FString Action=String(I,TEXT("action"));
    if(!Action.StartsWith(TEXT("it_")))return false;
    auto Fail=[&](const FString& Reason){if(P.Api>=2)ReportActivityFailure(P,I,Reason);else Stop(P.Id,Reason);return true;};
    if((P.Id!=TEXT("itemtools")&&P.Api<2)||!P.Permissions.Contains(TEXT("inventory"))||!P.Permissions.Contains(TEXT("loot")))return Fail(TEXT("Item Tools actions are not enabled."));
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    if(!Session||C->GetSessionState()!=EACESessionState::InWorld)return Fail(TEXT("Character left the world."));
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    if(C->IsUseBusy()||PendingSpell||(PC&&PC->IsWorldTransitionActive()))return true;
    const int32 Id=Guid(I,TEXT("item")),TargetId=Guid(I,TEXT("target"));FACEWorldObject Item,Target;
    const bool Found=C->GetWorldObject(Id,Item),HasTarget=C->GetWorldObject(TargetId,Target);
    FACEPosition Here=C->GetPlayerPosition();if(PC)PC->TryGetLocallyPredictedPosition(Here);
    auto Nearby=[&](){return HasTarget&&Target.IsSelectableWorldObject()&&!Target.bDying&&FVector::DistSquared(Here.ToUnrealLocation(),Target.Position.ToUnrealLocation())<=FMath::Square(500.);};
    const double RawAmount=Number(I,TEXT("amount"),1);
    if(RawAmount<1||RawAmount>100000||FMath::FloorToDouble(RawAmount)!=RawAmount)return Fail(TEXT("Invalid transfer quantity."));
    const int32 Amount=int32(RawAmount);const int32 Vendor=C->GetOpenVendorGuid();
    if(Action==TEXT("it_refresh_chest"))
    {if(!Nearby()||!Target.IsOpenable())return Fail(TEXT("Chest is no longer available."));Appraisals.Remove(TargetId);C->RequestBackgroundAppraisal(TargetId);P.NextAction=FPlatformTime::Seconds()+1;return true;}
    if(Action==TEXT("it_close"))
    {if(C->GetOpenExternalContainerGuid()==TargetId)C->SendNoLongerViewingContents(TargetId);return true;}
    if(Action==TEXT("it_give"))
    {
        if(!Found||!Transferable(C,Item)||Amount>FMath::Max(1,Item.StackSize)||!Nearby()||TargetId==C->GetPlayerGuid()||Target.IsVendor()||!Target.IsGiveOrCreatureTarget()||(Target.bIsPlayer&&Item.Attuned!=0))return Fail(TEXT("Item or recipient changed; transfer stopped."));
        if(PC){PC->BeginUseApproach(TargetId,Target.UseRadius>0?Target.UseRadius:.6f,false,false);UseApproachOwner=P.Id;}
        C->SendGiveObjectRequest(TargetId,Id,Amount);
    }
    else if(Action==TEXT("it_buy"))
    {
        if(!Vendor||Vendor!=Guid(I,TEXT("vendor")))return Fail(TEXT("Vendor changed; purchase stopped."));
        const auto Stock=C->GetVendorMerchandise();const auto* V=Stock.FindByPredicate([&](const auto& O){return O.Guid==Id;});
        bool ServerBank=false;P.Profile->TryGetBoolField(TEXT("server_bank"),ServerBank);
        if(!V||Amount>ACEInventoryRules::VendorPurchaseLimit(*V)||(!ServerBank&&ACEVendorPricing::VendorSellCost(*V,Amount,C->GetVendorSellRate())>uint32(FMath::Max(0,Session->GetVendorCurrencyCount()))))return Fail(TEXT("Vendor stock or currency changed; purchase stopped."));
        if(PurchaseRoom(C,*V)<Amount)return Fail(TEXT("Inventory space changed; purchase stopped."));
        C->SendBuyItems(Vendor,{{Amount,Id}});
    }
    else if(Action==TEXT("it_sell"))
    {
        if(!Found||!Transferable(C,Item)||!Vendor||Vendor!=Guid(I,TEXT("vendor"))||!Session->CanVendorBuyItem(Item)||Amount!=FMath::Max(1,Item.StackSize))return Fail(TEXT("Sale changed; no items sold. Partial stacks must be split first."));
        C->SendSellItems(Vendor,{{Amount,Id}});
    }
    else if(Action==TEXT("it_merge"))
    {
        if(!Found||!HasTarget||!Transferable(C,Item)||!Transferable(C,Target)
            ||Amount>ACEInventoryRules::MergeAmount(Item,Target))return Fail(TEXT("Merge items or stack space changed; merge stopped."));
        C->SendStackableMerge(Id,TargetId,Amount);
    }
    else if(Action==TEXT("it_split"))
    {
        if(!Found||!Transferable(C,Item)||Amount>=Item.StackSize)return Fail(TEXT("Split item changed."));
        const int32 Pack=C->PlanInventoryPickup(Id,0,Amount,false).Container;
        if(!Pack)return Fail(TEXT("Free one slot in any pack to split a funding stack."));
        C->SendStackableSplitToContainer(Id,Pack,0,Amount);
    }
    else if(Action==TEXT("it_key")||Action==TEXT("it_open"))
    {
        if(!Nearby()||!Target.IsOpenable()||Target.IsDoor()||Target.IsCorpse())return Fail(TEXT("Chest moved or is no longer available."));
        if(Action==TEXT("it_key"))
        {
            if(!Found||!Transferable(C,Item)||!ACEInventoryRules::IsUsable(Item.ItemUseable))return Fail(TEXT("Key is no longer available."));
            if(PC){PC->BeginUseApproach(TargetId,Target.UseRadius>0?Target.UseRadius:.6f,false,false);UseApproachOwner=P.Id;}
            C->SendUseWithTarget(Id,TargetId);Appraisals.Remove(TargetId);
        }
        else
        {
            if(PC){PC->BeginUseApproach(TargetId,Target.UseRadius>0?Target.UseRadius:.6f,false,false);UseApproachOwner=P.Id;}
            C->SendUseItem(TargetId);
        }
    }
    else if(Action==TEXT("it_pickup"))
    {
        if(!Found||!Nearby()||!Target.IsOpenable()||Target.IsDoor()||Item.ContainerId!=TargetId||C->GetOpenExternalContainerGuid()!=TargetId||Amount>FMath::Max(1,Item.StackSize))return Fail(TEXT("Chest or loot changed; pickup stopped."));
        const auto Pickup=C->PlanInventoryPickup(Id,0,Amount);
        if(!Pickup.Container)return Fail(TEXT("All packs are full; chest items remain untouched."));
        if(Pickup.MergeTarget)C->SendStackableMerge(Id,Pickup.MergeTarget,Amount);
        else if(Amount<Item.StackSize)C->SendStackableSplitToContainer(Id,Pickup.Container,0,Amount);
        else C->SendPutItemInContainer(Id,Pickup.Container,0,false);
    }
    else return Fail(TEXT("Unknown Item Tools action."));
    // The built-in merge policy waits for both stack updates before dispatching again.
    P.NextAction=FPlatformTime::Seconds()+(P.Id==TEXT("itemtools")&&Action==TEXT("it_merge")?0.:.1);
    return true;
}

void UACEPluginSubsystem::ExtendItemInventorySnapshot(const TSharedPtr<FJsonObject>& S)
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();if(!Session)return;
    ExtendInventorySnapshot(S);TArray<TSharedPtr<FJsonValue>> Inventory;
    for(const auto& V:S->GetArrayField(TEXT("inventory")))
    {
        auto J=MakeShared<FJsonObject>(*V->AsObject());FACEWorldObject Item;
        if(C->GetWorldObject(Guid(J,TEXT("id")),Item))
        {
            const bool Allowed=Transferable(C,Item);J->SetBoolField(TEXT("transferable"),Allowed);J->SetBoolField(TEXT("attuned"),Item.Attuned!=0);
            J->SetBoolField(TEXT("can_sell"),Allowed&&Session->CanVendorBuyItem(Item));
            J->SetBoolField(TEXT("trade_note"),Item.ItemType==ACEItemType::PromissoryNote);
            J->SetNumberField(TEXT("sell_unit"),ACEVendorPricing::VendorBuyPayout(Item,1,C->GetVendorBuyRate()));
        }
        Inventory.Add(MakeShared<FJsonValueObject>(J));
    }
    S->SetArrayField(TEXT("inventory"),Inventory);
}
