#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginDesktop.h"
#include "Mods/ACEPluginPresentation.h"
#include "Mods/ACEPluginCastMotion.h"
#include "ACEPluginVM.h"
#include "Mods/ACEAutoCraft.h"
#include "ACEProfiling.h"
#include "ACEVTProfile.h"
#include "ACEUCMLootPresentation.h"
#include "Mods/ACEUCMProfiles.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEPlayerController.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/Pawn.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
    bool SafeName(const FString& S)
    {
        if (S.IsEmpty() || S.Len() > 64) return false;
        for (TCHAR C : S) if (!(FChar::IsAlnum(C) || C == '_' || C == '-')) return false;
        return true;
    }
    TSharedPtr<FJsonObject> Read(const FString& Path)
    {
        FString Text; TSharedPtr<FJsonObject> O;
        if (IFileManager::Get().FileSize(*Path) > 16 * 1024 * 1024) return nullptr;
        if (FFileHelper::LoadFileToString(Text, *Path)) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O);
        return O;
    }
    FString Encode(const TSharedPtr<FJsonObject>& O)
    { FString Text; if (O) FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text)); return Text; }
    bool Write(const FString& Path, const TSharedPtr<FJsonObject>& O)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        const FString Temp = Path + TEXT(".tmp");
        return FFileHelper::SaveStringToFile(Encode(O), *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && IFileManager::Get().Move(*Path, *Temp, true, true);
    }
    FString String(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FString& Default = FString())
    { FString V; return O && O->TryGetStringField(Key, V) ? V : Default; }
    FString UCMImportPath(const FString& Source)
    {
        const FString Full=FPaths::ConvertRelativePathToFull(Source),Folder=FPaths::GetPath(Full),Filename=FPaths::GetCleanFilename(Full);
        TArray<FString> Files;IFileManager::Get().FindFiles(Files,*(Folder/TEXT("*")),true,false);
        for(const auto& File:Files)if(File.Equals(Filename,ESearchCase::IgnoreCase))return Folder/File;
        return Full;
    }
    double Number(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double Default = 0)
    { double V; return O && O->TryGetNumberField(Key, V) && FMath::IsFinite(V) ? V : Default; }
    int32 Guid(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, uint32 Default = 0)
    {
        const double V=Number(O,Key,Default);
        return V>=0 && V<=MAX_uint32 && FMath::FloorToDouble(V)==V ? int32(uint32(V)) : 0;
    }
    TSharedPtr<FJsonObject> Position(const FACEPosition& P)
    {
        auto O = MakeShared<FJsonObject>(); O->SetNumberField(TEXT("cell"), uint32(P.CellId));
        O->SetNumberField(TEXT("x"), P.Location.X); O->SetNumberField(TEXT("y"), P.Location.Y); O->SetNumberField(TEXT("z"), P.Location.Z); return O;
    }
    bool ValidUCMProfile(const TSharedPtr<FJsonObject>& O)
    {
        // Panels replace roots and also edit nested rules in place. Cache an
        // owned, validated snapshot and compare contents, never pointer identity.
        // This skips thousands of rule checks for a route/monster/character edit
        // without accepting a modified rule or a newly loaded external file.
        static thread_local TSharedPtr<FJsonObject> ValidatedLoot;
        const auto LootValue=O->TryGetField(TEXT("loot_rules"));
        const bool SameLoot=LootValue&&ValidatedLoot&&FJsonValue::CompareEqual(
            *LootValue,*ValidatedLoot->TryGetField(TEXT("loot_rules")));
        for(const TCHAR* Key:{TEXT("monster_default_priority"),TEXT("monster_default_range")})if(O->HasField(Key))
        {double V=0;if(!O->TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<(FString(Key)==TEXT("monster_default_priority")?-100:0)||V>100||FMath::FloorToDouble(V)!=V)return false;}
        for(const TCHAR* Key:{TEXT("melee_weapon"),TEXT("melee_secondary_equip")})if(O->HasField(Key))
        {double Value=0;if(!O->TryGetNumberField(Key,Value)||!FMath::IsFinite(Value)||Value<-1||Value>MAX_uint32||FMath::FloorToDouble(Value)!=Value)return false;}
        if(O->HasField(TEXT("monster_catalog")))
        {
            const TSharedPtr<FJsonObject>* Catalog=nullptr;
            if(!O->TryGetObjectField(TEXT("monster_catalog"),Catalog)||(*Catalog)->Values.Num()>10000)return false;
            for(const auto& Pair:(*Catalog)->Values)
            {
                if(Pair.Key.IsEmpty()||Pair.Key.Len()>256||Pair.Value->Type!=EJson::Object)return false;
                for(const TCHAR* Key:{TEXT("species"),TEXT("max_health")})
                {double V=0;if(!Pair.Value->AsObject()->TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<0||V>MAX_int32||FMath::FloorToDouble(V)!=V)return false;}
            }
        }
        for(const TCHAR* Key:{TEXT("auto_craft"),TEXT("auto_fellowship"),TEXT("craft_confirm"),TEXT("fellowship_open"),TEXT("fellowship_votes"),TEXT("random_helper_buffs"),TEXT("cast_jiggle"),TEXT("jump_out_wand")})if(O->HasField(Key)&&!O->HasTypedField<EJson::Boolean>(Key))return false;
        for(const TCHAR* Key:{TEXT("craft_health_kits"),TEXT("craft_health_food"),TEXT("craft_stamina_kits"),TEXT("craft_stamina_food"),TEXT("craft_mana_kits"),TEXT("craft_mana_food"),TEXT("fellowship_capacity"),TEXT("random_helper_interval"),TEXT("debuff_selection")})if(O->HasField(Key))
        {
            const FString Name(Key);const double Low=Name==TEXT("fellowship_capacity")?2:Name==TEXT("random_helper_interval")||Name==TEXT("debuff_selection")?1:0;
            const double High=Name==TEXT("fellowship_capacity")?100:Name==TEXT("random_helper_interval")?86400:Name==TEXT("debuff_selection")?2:10000;
            double V=0;if(!O->TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<Low||V>High||(Name!=TEXT("random_helper_interval")&&V!=FMath::FloorToDouble(V)))return false;
        }
        if(O->HasField(TEXT("grenade_catalog")))
        {
            const TSharedPtr<FJsonObject>* Catalog=nullptr;if(!O->TryGetObjectField(TEXT("grenade_catalog"),Catalog)||(*Catalog)->Values.Num()>512)return false;
            for(const auto& Pair:(*Catalog)->Values)
            {
                if(Pair.Key.IsEmpty()||Pair.Key.Len()>256||Pair.Value->Type!=EJson::Object)return false;
                for(const TCHAR* Key:{TEXT("WieldReqType"),TEXT("WieldReqAttribute"),TEXT("WieldReqValue"),TEXT("Spell"),TEXT("Spellcraft")})
                {double V=0;if(!Pair.Value->AsObject()->TryGetNumberField(Key,V)||!FMath::IsFinite(V)||V<(FString(Key)==TEXT("Spell")?1:0)||V>(FString(Key)==TEXT("WieldReqType")?12:MAX_int32)||V!=FMath::FloorToDouble(V))return false;}
            }
        }
        // Validate structures consumed by native widgets, not only the script.
        for(const TCHAR* Key:{TEXT("route"),TEXT("states"),TEXT("loot_rules"),TEXT("monsters"),TEXT("buff_commands"),TEXT("assist_items"),TEXT("buff_items"),TEXT("craft_targets"),TEXT("item_buff_targets"),TEXT("recharge_handlers"),TEXT("pea_recipes"),TEXT("vendor_rules")})
        {
            if(SameLoot&&FCString::Strcmp(Key,TEXT("loot_rules"))==0)continue;
            if(!O->HasField(Key))continue;
            const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
            if(!O->TryGetArrayField(Key,Values)||Values->Num()>((FString(Key)==TEXT("loot_rules")||FString(Key)==TEXT("route"))?2048:512))return false;
            for(const auto& Value:*Values)
            {
                if(Value->Type!=EJson::Object)return false;
                auto Entry=Value->AsObject();
                if(FString(Key)==TEXT("craft_targets"))
                {
                    FString Name;double Count=0;
                    if(Values->Num()>64||!Entry->TryGetStringField(TEXT("name"),Name)||Name.IsEmpty()||Name.Len()>256||!Entry->TryGetNumberField(TEXT("count"),Count)||!FMath::IsFinite(Count)||Count<1||Count>10000||FMath::FloorToDouble(Count)!=Count)return false;
                }
                if(FString(Key)==TEXT("vendor_rules"))
                {
                    for(const TCHAR* K:{TEXT("vendor_name"),TEXT("item_name"),TEXT("server")})
                    {FString V;if(!Entry->TryGetStringField(K,V)||V.IsEmpty()||V.Len()>256)return false;}
                    for(const TCHAR* K:{TEXT("vendor_wcid"),TEXT("item_wcid"),TEXT("quantity")})
                    {double V;if(!Entry->TryGetNumberField(K,V)||!FMath::IsFinite(V)||V<1||V>(FString(K)==TEXT("quantity")?100000:MAX_int32)||FMath::FloorToDouble(V)!=V)return false;}
                }
                if(FString(Key)==TEXT("pea_recipes"))
                {
                    for(const TCHAR* TextKey:{TEXT("tool"),TEXT("input"),TEXT("output")})
                    {FString V;if(!Entry->TryGetStringField(TextKey,V)||V.IsEmpty()||V.Len()>256)return false;}
                    double Count=0;if(!Entry->TryGetNumberField(TEXT("count"),Count)||!FMath::IsFinite(Count)||Count<1||Count>10000||FMath::FloorToDouble(Count)!=Count)return false;
                }
                for(const TCHAR* TextKey:{TEXT("name"),TEXT("kind"),TEXT("label"),TEXT("combat"),TEXT("object_name"),TEXT("keyword"),TEXT("role")})
                    if(Entry->HasField(TextKey)&&Entry->Values[TextKey]->Type!=EJson::String)return false;
                if(Entry->HasField(TEXT("transitions")))
                {
                    const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;
                    if(!Entry->TryGetArrayField(TEXT("transitions"),Rules)||Rules->Num()>256)return false;
                    for(const auto& Rule:*Rules)if(Rule->Type!=EJson::Object)return false;
                }
            }
        }
        for(const TCHAR* Key:{TEXT("weapons"),TEXT("weapon_items"),TEXT("consumables"),TEXT("buffs"),TEXT("excluded_buffs"),TEXT("excluded_buff_spells")})
        {
            if(!O->HasField(Key))continue;
            const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;if(!O->TryGetArrayField(Key,Values)||Values->Num()>2048)return false;
            for(const auto& Value:*Values)if(Value->Type!=EJson::Number||Value->AsNumber()<0||Value->AsNumber()>MAX_uint32)return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Commands=nullptr;
        if(O->TryGetArrayField(TEXT("buff_commands"),Commands))
        {
            if(Commands->Num()>32)return false;TSet<FString> Keywords;
            for(const auto& V:*Commands)
            {
                const FString Key=String(V->AsObject(),TEXT("keyword")).TrimStartAndEnd().ToLower(),Role=String(V->AsObject(),TEXT("role"));
                if(Key.IsEmpty()||Key.Len()>64||Keywords.Contains(Key)||!TArray<FString>{TEXT("mage"),TEXT("heavy"),TEXT("missile"),TEXT("light"),TEXT("finesse"),TEXT("twohanded"),TEXT("unarmed"),TEXT("melee")}.Contains(Role))return false;
                Keywords.Add(Key);
            }
        }
        if(O->HasField(TEXT("salvage_policy")))
        {
            const TSharedPtr<FJsonObject>* Policy=nullptr;
            if(!O->TryGetObjectField(TEXT("salvage_policy"),Policy))return false;
            auto ValidRanges=[](const TSharedPtr<FJsonValue>& V)
            {
                const TArray<TSharedPtr<FJsonValue>>* Ranges=nullptr;if(!V->TryGetArray(Ranges)||Ranges->Num()>64)return false;
                double Previous=-1;
                for(const auto& R:*Ranges)
                {
                    if(R->Type!=EJson::Object)return false;double Min,Max;
                    if(!R->AsObject()->TryGetNumberField(TEXT("min"),Min)||!R->AsObject()->TryGetNumberField(TEXT("max"),Max)||!FMath::IsFinite(Min)||!FMath::IsFinite(Max)||Min<0||Min<Previous||Max<Min||Max>1000)return false;
                    Previous=Max;
                }
                return true;
            };
            if(!(*Policy)->HasField(TEXT("default"))||!ValidRanges((*Policy)->Values[TEXT("default")]))return false;
            for(const TCHAR* Key:{TEXT("materials"),TEXT("values")})if((*Policy)->HasField(Key))
            {
                const TSharedPtr<FJsonObject>* Map=nullptr;if(!(*Policy)->TryGetObjectField(Key,Map)||(*Map)->Values.Num()>1024)return false;
                for(const auto& Pair:(*Map)->Values)
                {
                    uint32 Id=0;if(!LexTryParseString(Id,*Pair.Key)||!Id||FString::Printf(TEXT("%u"),Id)!=FString(Pair.Key))return false;
                    if(FString(Key)==TEXT("materials")){if(!ValidRanges(Pair.Value))return false;}
                    else if(Pair.Value->Type!=EJson::Number||!FMath::IsFinite(Pair.Value->AsNumber())||Pair.Value->AsNumber()<0||Pair.Value->AsNumber()>MAX_int32)return false;
                }
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* Loot=nullptr;
        if(!SameLoot&&O->TryGetArrayField(TEXT("loot_rules"),Loot))for(const auto& V:*Loot)
        {
            const auto R=V->AsObject();
            for(const TCHAR* K:{TEXT("name"),TEXT("label")})if(FTCHARToUTF8(*String(R,K)).Length()>256)return false;
            for(const TCHAR* K:{TEXT("action"),TEXT("name_mode")})if(R->HasField(K)&&R->Values[K]->Type!=EJson::String)return false;
            if(!TArray<FString>{TEXT("keep"),TEXT("skip"),TEXT("salvage"),TEXT("sell"),TEXT("read")}.Contains(String(R,TEXT("action"),TEXT("keep"))))return false;
            if(!TArray<FString>{TEXT("prefix"),TEXT("exact"),TEXT("contains"),TEXT("any"),TEXT("all"),TEXT("regex")}.Contains(String(R,TEXT("name_mode"),TEXT("prefix"))))return false;
            if(String(R,TEXT("name_mode"))==TEXT("regex")&&!ACEVTProfile::IsSupportedPattern(String(R,TEXT("name"))))return false;
            if(R->HasField(TEXT("conditions_mode"))&&(!R->HasTypedField<EJson::String>(TEXT("conditions_mode"))||!TArray<FString>{TEXT("all"),TEXT("any")}.Contains(String(R,TEXT("conditions_mode")))))return false;
            if(R->HasField(TEXT("enabled"))&&R->Values[TEXT("enabled")]->Type!=EJson::Boolean)return false;
            if(R->HasField(TEXT("count_by_name"))&&R->Values[TEXT("count_by_name")]->Type!=EJson::Boolean)return false;
            if(R->HasField(TEXT("compatibility_issues")))
            {
                const TArray<TSharedPtr<FJsonValue>>* Problems=nullptr;bool Enabled=true;R->TryGetBoolField(TEXT("enabled"),Enabled);
                if(!R->TryGetArrayField(TEXT("compatibility_issues"),Problems)||Problems->Num()>128||(Enabled&&Problems->Num()))return false;
                for(const auto& Problem:*Problems)if(Problem->Type!=EJson::String||Problem->AsString().Len()>2048)return false;
            }
            for(const TCHAR* K:{TEXT("material"),TEXT("type"),TEXT("keep_up_to"),TEXT("min_value"),TEXT("max_value"),TEXT("min_workmanship"),TEXT("max_workmanship"),TEXT("min_burden"),TEXT("max_burden"),TEXT("min_rating"),TEXT("max_rating")})
                if(R->HasField(K)&&(R->Values[K]->Type!=EJson::Number||!FMath::IsFinite(R->Values[K]->AsNumber())||R->Values[K]->AsNumber()<0||R->Values[K]->AsNumber()>MAX_uint32))return false;
            for(const TCHAR* K:{TEXT("material"),TEXT("type"),TEXT("keep_up_to")})if(FMath::FloorToDouble(Number(R,K))!=Number(R,K))return false;
            if(R->HasField(TEXT("conditions")))
            {
                const TArray<TSharedPtr<FJsonValue>>* Conditions=nullptr;
                if(!R->TryGetArrayField(TEXT("conditions"),Conditions)||Conditions->Num()>ACEVTProfile::MaxLootConditions)return false;
                for(const auto& CV:*Conditions)
                {
                    if(CV->Type!=EJson::Object)return false;const auto C=CV->AsObject();const FString Field=String(C,TEXT("field"));
                    if(!TArray<FString>{TEXT("int"),TEXT("float"),TEXT("string"),TEXT("spells"),TEXT("legacy")}.Contains(Field))return false;
                    for(const TCHAR* K:{TEXT("value"),TEXT("key")})if(C->HasField(K)&&(C->Values[K]->Type!=EJson::Number||!FMath::IsFinite(C->Values[K]->AsNumber())))return false;
                    if(Field!=TEXT("spells")&&Field!=TEXT("legacy")&&(Number(C,TEXT("key"))<1||Number(C,TEXT("key"))>MAX_int32||FMath::FloorToDouble(Number(C,TEXT("key")))!=Number(C,TEXT("key"))))return false;
                    if(Field==TEXT("legacy"))
                    {
                        const int Key=int(Number(C,TEXT("key")));const TArray<TSharedPtr<FJsonValue>>* Args=nullptr;
                        if(Number(C,TEXT("key"))!=Key||!TArray<int>{6,7,10,14,15,16,17,1000,1001,1002,1003,1004,2000,2001,2003,2005,2006,2007,2008}.Contains(Key)||!C->TryGetArrayField(TEXT("args"),Args)||Args->Num()!=((Key==1000||Key==17||Key==2003||Key==2005)?2:(Key==1004||Key==2008)?3:Key==14||Key==15?5:Key==16?6:1))return false;
                        for(const auto& Arg:*Args)if(Arg->Type!=EJson::Number||!FMath::IsFinite(Arg->AsNumber()))return false;
                        if(Key>=14&&Key<=16&&!ACEUCMLootPresentation::ValidColorMatch(C))return false;
                    }
                    if(Field==TEXT("int")||Field==TEXT("float"))
                    {
                        if(!TArray<FString>{TEXT("le"),TEXT("ge"),TEXT("eq"),TEXT("ne"),TEXT("bits"),TEXT("in")}.Contains(String(C,TEXT("op"))))return false;
                        if(String(C,TEXT("op"))==TEXT("bits")&&(Number(C,TEXT("value"))<0||Number(C,TEXT("value"))>MAX_uint32||FMath::FloorToDouble(Number(C,TEXT("value")))!=Number(C,TEXT("value"))))return false;
                        if(String(C,TEXT("op"))==TEXT("in"))
                        {
                            const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
                            if(C->HasField(TEXT("invalid_values"))||!C->TryGetArrayField(TEXT("values"),Values)||Values->IsEmpty()||Values->Num()>64)return false;
                            for(const auto& Value:*Values)if(Value->Type!=EJson::Number||!FMath::IsFinite(Value->AsNumber()))return false;
                        }
                    }
                    if(C->HasField(TEXT("missing_zero"))&&C->Values[TEXT("missing_zero")]->Type!=EJson::Boolean)return false;
                    if(!ACEUCMLootPresentation::ValidTextMatch(C))return false;
                    if(Field==TEXT("spells")&&(Number(C,TEXT("value"))<0||Number(C,TEXT("value"))>1024||FMath::FloorToDouble(Number(C,TEXT("value")))!=Number(C,TEXT("value"))))return false;
                }
            }
            for(const TCHAR* Field:{TEXT("value"),TEXT("workmanship"),TEXT("burden"),TEXT("rating")})
            {
                const double Lo=Number(R,*(FString(TEXT("min_"))+Field)),Hi=Number(R,*(FString(TEXT("max_"))+Field));
                if(Hi>0&&Hi<Lo)return false;
            }
        }
        if(LootValue&&!SameLoot)
        {
            auto Snapshot=MakeShared<FJsonObject>();Snapshot->SetField(TEXT("loot_rules"),LootValue);
            ValidatedLoot=ACEUCMProfiles::Copy(Snapshot);
        }
        return true;
    }
    bool ValidItemToolsProfile(const TSharedPtr<FJsonObject>& O)
    {
        for(const TCHAR* Key:{TEXT("give_limit"),TEXT("buy_limit"),TEXT("delay_ms"),TEXT("chest_cycles"),TEXT("chest_delay")})
            if(O->HasField(Key)){double N=0;if(!O->TryGetNumberField(Key,N)||!FMath::IsFinite(N)||N<0||N>100000||FMath::FloorToDouble(N)!=N)return false;}
        for(const TCHAR* Key:{TEXT("auto_restock"),TEXT("redeem_notes")})if(O->HasField(Key)&&O->Values[Key]->Type!=EJson::Boolean)return false;
        for(const TCHAR* Key:{TEXT("give"),TEXT("buy"),TEXT("sell"),TEXT("key"),TEXT("target"),TEXT("chest"),TEXT("mule1"),TEXT("mule2")})
            for(const TCHAR* Suffix:{TEXT("_name"),TEXT("_file")}){const FString K=FString(Key)+Suffix;if(O->HasField(K)&&(O->Values[*K]->Type!=EJson::String||O->Values[*K]->AsString().Len()>256))return false;}
        const TArray<TSharedPtr<FJsonValue>>* Rows=nullptr;
        if(O->HasField(TEXT("restock")))
        {
            if(!O->TryGetArrayField(TEXT("restock"),Rows)||Rows->Num()>256)return false;
            for(const auto& R:*Rows){if(R->Type!=EJson::Object)return false;auto V=R->AsObject();double N;
                if(String(V,TEXT("name")).IsEmpty()||String(V,TEXT("name")).Len()>256)return false;
                for(const TCHAR* Key:{TEXT("wcid"),TEXT("quantity")})if(!V->TryGetNumberField(Key,N)||!FMath::IsFinite(N)||N<0||N>(FString(Key)==TEXT("wcid")?MAX_uint32:100000)||FMath::FloorToDouble(N)!=N)return false;}
        }
        for(const TCHAR* Key:{TEXT("profile_rules"),TEXT("chest_rules"),TEXT("mule1_rules"),TEXT("mule2_rules")})
        {
            if(!O->HasField(Key))continue;if(!O->TryGetArrayField(Key,Rows))return false;
            auto Rules=MakeShared<FJsonObject>();Rules->SetArrayField(TEXT("loot_rules"),*Rows);if(!ValidUCMProfile(Rules))return false;
        }
        return true;
    }

}

void UACEPluginSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogTemp,Display,TEXT("Client plugin capability: ACE.LocalCosmetics/1"));
    Collection.InitializeDependency<UACEClientSubsystem>();
    GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->OnAppraisalObserved.AddDynamic(this,&UACEPluginSubsystem::ObserveAppraisal);
    GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->OnChatMessage.AddDynamic(this,&UACEPluginSubsystem::ObservePluginChat);
    GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->OnVitalsUpdated.AddDynamic(this,&UACEPluginSubsystem::ObserveLedgerVitals);
    Settings = Read(UserDirectory() / TEXT("settings.json"));
    if (!Settings) Settings = MakeShared<FJsonObject>();
    UCMLog.Load(UserDirectory()/TEXT("ucm-log.json"));
    Discover();
    Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UACEPluginSubsystem::Tick), .5f);
}
void UACEPluginSubsystem::Deinitialize()
{
    SaveItemBuffTimers();
    SaveEquipmentAppraisals();
    RestoreWorldRendering();
    UnbindCombatEvents();
    if(RouteMesh){RouteMesh->DestroyComponent();RouteMesh=nullptr;}
    if(RouteActor){RouteActor->Destroy();RouteActor=nullptr;}
    if(PluginMarkerActor){PluginMarkerActor->Destroy();PluginMarkerActor=nullptr;PluginMarkerMesh=nullptr;}
    if(WorldPlateActor){WorldPlateActor->Destroy();WorldPlateActor=nullptr;WorldPlateComponents.Reset();}
    ClearPluginTerrain();
    GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->OnAppraisalObserved.RemoveDynamic(this,&UACEPluginSubsystem::ObserveAppraisal);
    GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->OnChatMessage.RemoveDynamic(this,&UACEPluginSubsystem::ObservePluginChat);
    GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->OnVitalsUpdated.RemoveDynamic(this,&UACEPluginSubsystem::ObserveLedgerVitals);
    if(auto S=ObservedSession.Pin()){S->OnItemUseRequested.Remove(ItemUseHandle);S->OnUseDone.Remove(UseDoneHandle);S->OnSpellCastRequested.Remove(SpellCastHandle);S->OnPlayerTell.Remove(TellHandle);S->OnChatMessage.Remove(MetaChatHandle);S->OnCombatFeedback.Remove(CombatFeedbackHandle);}
    StopAll(TEXT("Client closing")); ReleaseLocalCosmetics(); FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
    FlushUCMLog();
    FTSTicker::GetCoreTicker().RemoveTicker(DecisionWakeTicker);DecisionWakeTicker.Reset();
    if (DesktopPanel) DesktopPanel->SaveOverlayLayout();
    if (DesktopPanel) if (auto* V = GetGameInstance()->GetGameViewportClient()) V->RemoveViewportWidgetContent(DesktopPanel.ToSharedRef());
    RemoveDesktopDock();
    DesktopPanel.Reset(); Plugins.Reset(); Super::Deinitialize();
}
FString UACEPluginSubsystem::UserDirectory() const { return StorageRoot.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("ClientPlugins") : StorageRoot; }
void UACEPluginSubsystem::SaveItemBuffTimers()
{
    if(!DataPlayer)return;
    const double Now=FPlatformTime::Seconds(),UTC=FDateTime::UtcNow().ToUnixTimestampDecimal();
    auto Saved=MakeShared<FJsonObject>();Saved->SetNumberField(TEXT("deaths"),ItemBuffDeaths);
    TArray<TSharedPtr<FJsonValue>> Entries;
    for(const auto& Value:ItemBuffs)
    {
        const auto E=Value->AsObject();double Expiry=0;
        if(!E||!E->TryGetNumberField(TEXT("expires"),Expiry)||Expiry<=Now||!E->HasField(TEXT("spell")))continue;
        // The world object list may already be gone during logout. Ownership
        // was checked when the server confirmed the cast, while it was present.
        bool Owned=false;if(!E->TryGetBoolField(TEXT("owned"),Owned)||!Owned)continue;
        auto Copy=MakeShared<FJsonObject>(*E);Copy->RemoveField(TEXT("expires"));Copy->SetNumberField(TEXT("expires_utc"),UTC+Expiry-Now);
        Entries.Add(MakeShared<FJsonValueObject>(Copy));if(Entries.Num()==256)break;
    }
    Saved->SetArrayField(TEXT("buffs"),Entries);
    Write(UserDirectory()/TEXT("BuffTimers")/(FMD5::HashAnsiString(*DataServer)+FString::Printf(TEXT(".%08X.json"),uint32(DataPlayer))),Saved);
}
void UACEPluginSubsystem::LoadItemBuffTimers()
{
    ItemBuffs.Empty();
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    ItemBuffDeaths=C->GetPlayerVitalsView().NumDeaths;
    const auto Saved=Read(UserDirectory()/TEXT("BuffTimers")/(FMD5::HashAnsiString(*DataServer)+FString::Printf(TEXT(".%08X.json"),uint32(DataPlayer))));
    double Deaths=0;const TArray<TSharedPtr<FJsonValue>>* Entries=nullptr;
    if(!Saved||!Saved->TryGetNumberField(TEXT("deaths"),Deaths)||Deaths!=ItemBuffDeaths||!Saved->TryGetArrayField(TEXT("buffs"),Entries)||Entries->Num()>256)return;
    const double Now=FPlatformTime::Seconds(),UTC=FDateTime::UtcNow().ToUnixTimestampDecimal();
    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    for(const auto& Value:*Entries)
    {
        if(Value->Type!=EJson::Object)continue;const auto E=Value->AsObject();
        double Expiry=0,Spell=0,Target=0,Duration=0;uint32 School=0,Power=0,Category=0,Flags=0;
        if(!E->TryGetNumberField(TEXT("expires_utc"),Expiry)||!FMath::IsFinite(Expiry)||Expiry<=UTC||Expiry>UTC+7*86400
            ||!E->TryGetNumberField(TEXT("spell"),Spell)||!E->TryGetNumberField(TEXT("target"),Target)
            ||!FMath::IsFinite(Spell)||Spell<1||Spell>MAX_uint32||Spell!=FMath::FloorToDouble(Spell)
            ||!FMath::IsFinite(Target)||Target<1||Target>MAX_uint32||Target!=FMath::FloorToDouble(Target)
            ||!Dat->TryGetPluginSpellInfo(uint32(Spell),School,Power,Category,Flags,Duration)||School!=3||!(Flags&4)||Duration<=0)continue;
        // Item GUIDs and timers are scoped to this character and server. Never
        // persist other-player requests or use estimates for character buffs.
        // Equipped item creates can follow login. Lua still requires the
        // matching owned/equipped target before using any saved item timer.
        bool Owned=false;if(!E->TryGetBoolField(TEXT("owned"),Owned)||!Owned)continue;
        auto Copy=MakeShared<FJsonObject>(*E);Copy->RemoveField(TEXT("expires_utc"));Copy->SetNumberField(TEXT("expires"),Now+Expiry-UTC);
        Copy->SetNumberField(TEXT("category"),Category);Copy->SetNumberField(TEXT("power"),Power);
        ItemBuffs.Add(MakeShared<FJsonValueObject>(Copy));
    }
    ValidateItemBuffTimers();
}
void UACEPluginSubsystem::ValidateItemBuffTimers()
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto Session=C->GetSession();
    if(!Session||DataPlayer!=C->GetPlayerGuid())return;
    if(ItemBuffDeaths!=C->GetPlayerVitalsView().NumDeaths)
    {ItemBuffs.Empty();ItemBuffDeaths=C->GetPlayerVitalsView().NumDeaths;SaveItemBuffTimers();return;}
    const double Now=FPlatformTime::Seconds();
    ItemBuffs.RemoveAll([&](const auto& Value)
    {
        const auto E=Value->AsObject();const int32 Target=int32(uint32(Number(E,TEXT("target"))));
        if(Number(E,TEXT("expires"))<=Now)return true;
        const TArray<TSharedPtr<FJsonValue>>* Equipment=nullptr;
        if(Target==DataPlayer&&E->TryGetArrayField(TEXT("equipment"),Equipment))
        {
            // A shield can be removed to wield the casting wand. Do not erase
            // every worn-armor timer when combat equips it again: that produces
            // an endless wand -> shield -> wand cycle. Retain uncovered items
            // until their individual cast completes, even while in inventory.
            const TArray<TSharedPtr<FJsonValue>>* Previous=nullptr;
            TArray<TSharedPtr<FJsonValue>> Missing;
            if(E->TryGetArrayField(TEXT("missing_equipment"),Previous))Missing=*Previous;
            for(const auto& Pair:Session->GetWorldObjects())
                if(Pair.Value.WielderId==DataPlayer&&(Pair.Value.ItemType&(ACEItemType::Armor|ACEItemType::Clothing))
                    &&!Equipment->ContainsByPredicate([&](const auto& Id){return Id->AsNumber()==double(uint32(Pair.Key));})
                    &&!Missing.ContainsByPredicate([&](const auto& Id){return Id->AsNumber()==double(uint32(Pair.Key));}))
                    Missing.Add(MakeShared<FJsonValueNumber>(uint32(Pair.Key)));
            Missing.RemoveAll([&](const auto& Id)
            {
                FACEWorldObject Item;
                const int32 Guid=int32(uint32(Id->AsNumber()));
                const auto* Appraisal=Appraisals.Find(Guid);
                // Retail's Unenchantable property (ResistMagic) and ACE's
                // IsEnchantable use the same threshold, including Covenant.
                // Prune saved queues too when the appraisal arrives later.
                return !C->GetWorldObject(Guid,Item)||!IsOwnedPluginItem(Item)
                    ||(Appraisal&&Appraisal->bSuccess&&Appraisal->IntProperties.FindRef(36)>=9999);
            });
            E->SetArrayField(TEXT("missing_equipment"),Missing);
        }
        return false;
    });
}
TSharedPtr<FACEClientPlugin> UACEPluginSubsystem::Find(const FString& Id) const
{ for (const auto& P : Plugins) if (P->Id == Id) return P; return nullptr; }
void UACEPluginSubsystem::Discover()
{
    ++PluginOrderRevision;
    RemoveDesktopDock();
    TMap<FString,uint64> ChatResume;
    for(const auto& P:Plugins)ChatResume.Add(P->Id,P->ChatCursor);
    StopAll(TEXT("Reloading plugins")); ReleaseLocalCosmetics(); Plugins.Reset(); Notice.Empty();
    AutoCraft.Reset();
    CompanionSession.Reset();CompanionPlayer=0;CompanionServer.Empty();
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("ACEClient"));
    TArray<FString> Roots; if (Plugin) Roots.Add(Plugin->GetBaseDir() / TEXT("ClientMods"));
    Roots.Add(UserDirectory() / TEXT("Installed"));
    for (const FString& Root : Roots)
    {
        TArray<FString> Dirs; IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true); Dirs.Sort();
        for (const FString& Dir : Dirs)
        {
            auto M = Read(Root / Dir / TEXT("plugin.json"));
            const FString Id = String(M, TEXT("id"));
            if (!M || !SafeName(Id) || Find(Id) || (Number(M, TEXT("api")) != 1 && Number(M, TEXT("api")) != 2))
            { Notice += TEXT("Skipped invalid, duplicate, or incompatible plugin: ") + Dir + TEXT("\n"); continue; }
            const TSharedPtr<FJsonObject>* Presentation=nullptr;FString PresentationError;
            if(M->HasField(TEXT("presentation"))&&(!M->TryGetObjectField(TEXT("presentation"),Presentation)||!ACEPluginPresentation::Valid(*Presentation,PresentationError)))
            {Notice+=TEXT("Invalid presentation for ")+Id+TEXT(": ")+PresentationError+TEXT("\n");continue;}
            auto P = MakeShared<FACEClientPlugin>(); P->Id = Id; P->Manifest = M; P->Api=int32(Number(M,TEXT("api"),1));
            P->Name = String(M, TEXT("name"), Id); P->Version = String(M, TEXT("version"));
            P->Description = String(M, TEXT("description")); P->Directory = Root / Dir;
            P->LocalService = String(M,TEXT("mode"))==TEXT("local");
            P->Companion = String(M,TEXT("mode"))==TEXT("companion");
            P->ChatCursor=ChatResume.FindRef(Id);
            const TArray<TSharedPtr<FJsonValue>>* Permissions = nullptr;
            bool Valid = TArray<FString>{TEXT(""),TEXT("automation"),TEXT("companion"),TEXT("local")}.Contains(String(M,TEXT("mode"))) && M->TryGetArrayField(TEXT("permissions"), Permissions);
            if (Permissions) for (const auto& V : *Permissions)
            {
                const FString Permission = V->AsString();
                if (Permission != TEXT("identify") && Permission != TEXT("sound") && Permission != TEXT("admin_content") && Permission != TEXT("terrain") && Permission != TEXT("profiles") && Permission != TEXT("data") && Permission != TEXT("chat") && Permission != TEXT("chat_filter") && Permission != TEXT("state") && Permission != TEXT("ui") && Permission != TEXT("markers") && Permission != TEXT("waypoint") && Permission != TEXT("messaging") && Permission != TEXT("commands") && Permission != TEXT("local_tools") && Permission != TEXT("local_cosmetics") && Permission != TEXT("cast") && Permission != TEXT("combat") && Permission != TEXT("navigation") && Permission != TEXT("inventory") && Permission != TEXT("loot") && Permission != TEXT("say") && Permission != TEXT("fellowship") && Permission != TEXT("confirm")) Valid = false;
                else P->Permissions.AddUnique(Permission);
            }
            if(P->Companion)for(const auto& Permission:P->Permissions)
                if(!TArray<FString>{TEXT("identify"),TEXT("sound"),TEXT("admin_content"),TEXT("terrain"),TEXT("chat"),TEXT("chat_filter"),TEXT("state"),TEXT("ui"),TEXT("markers"),TEXT("waypoint"),TEXT("messaging"),TEXT("commands"),TEXT("profiles"),TEXT("data"),TEXT("say")}.Contains(Permission))Valid=false;
            if(P->LocalService)
            {
                if(Number(M,TEXT("service_api"))!=1 || !P->Permissions.Contains(TEXT("local_cosmetics"))) Valid=false;
                for(const auto& Permission:P->Permissions) if(Permission!=TEXT("local_cosmetics")&&Permission!=TEXT("local_tools")) Valid=false;
                if(String(M,TEXT("service"))==TEXT("local_cosmetics")&&!P->Permissions.Contains(TEXT("local_tools"))) Valid=false;
            }
            else if(P->Permissions.Contains(TEXT("local_cosmetics"))||P->Permissions.Contains(TEXT("local_tools"))) Valid=false;
            if(M->HasField(TEXT("update_interval")))
            {
                Valid &= P->Companion && M->HasTypedField<EJson::Number>(TEXT("update_interval"))
                    && M->TryGetNumberField(TEXT("update_interval"),P->UpdateInterval)
                    && FMath::IsFinite(P->UpdateInterval) && P->UpdateInterval>=.5 && P->UpdateInterval<=60;
            }
            if(M->HasField(TEXT("snapshot_fields")))
            {
                const TArray<TSharedPtr<FJsonValue>>* Fields=nullptr;
                P->SelectiveSnapshot=true;
                if(!P->Companion || P->Api<2 || !M->TryGetArrayField(TEXT("snapshot_fields"),Fields) || Fields->Num()>128)Valid=false;
                else for(const auto& Field:*Fields)
                {
                    FString Key;
                    if(!Field || Field->Type!=EJson::String || !Field->TryGetString(Key) || Key.IsEmpty() || Key.Len()>64) {Valid=false;break;}
                    for(TCHAR Ch:Key)if(!FChar::IsAlnum(Ch)&&Ch!='_')Valid=false;
                    P->SnapshotFields.Add(Key);
                }
            }
            if(M->HasField(TEXT("chat_history")))Valid&=M->HasTypedField<EJson::Boolean>(TEXT("chat_history"))&&M->TryGetBoolField(TEXT("chat_history"),P->ChatHistory);
            if(M->HasField(TEXT("chat_subscriptions")))
            {
                const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;FString Error;
                P->SelectiveChat=true;
                Valid&=P->Permissions.Contains(TEXT("chat"))&&M->TryGetArrayField(TEXT("chat_subscriptions"),Rules)&&ValidatePluginChatFilters(*Rules,P->ChatSubscriptions,Error);
            }
            if (!Valid) { Notice += TEXT("Unsupported permissions, snapshot fields, chat subscriptions, update interval or local service API: ") + Id + TEXT("\n"); continue; }
            // Bind consent to the actual capability set, including after plugin upgrades.
            P->Permissions.Sort();
            const FString Grant = FString::Join(P->Permissions, TEXT(","));
            P->Enabled = String(Settings, *Id) == TEXT("enabled:") + Grant;
            if((Id==TEXT("looteditor")||Id==TEXT("waypoint")||Id==TEXT("ucmmicro")||IsLedger(Id))&&!Settings->HasField(Id))P->Enabled=true;
            P->Profile = Read(P->Directory / TEXT("default.json"));
            if (!P->Profile) P->Profile = MakeShared<FJsonObject>();
            Plugins.Add(P);
            if(P->LocalService && String(M,TEXT("service"))==TEXT("local_cosmetics")) CreateLocalCosmetic(*P);
            const FString Profile = String(Settings, *(Id + TEXT(".profile")), TEXT("Default"));
            if(Id==TEXT("ucm"))InitializeUCMSelection();
            else if(IFileManager::Get().FileExists(*(UserDirectory()/TEXT("Profiles")/Id/(Profile+TEXT(".json")))))LoadProfile(Id, Profile);
        }
    }
    // Discovery must include disabled plugins too. Apply the allowance only to
    // enabled entries, preferring saved choices over newly installed defaults.
    const int32 Limit=GetEnabledPluginLimit();
    int32 EnabledCount=0;
    for(bool SavedChoice:{true,false})for(const auto& P:Plugins)
    {
        if(!P->Enabled || Settings->HasField(P->Id)!=SavedChoice)continue;
        if(Limit==0 || EnabledCount<Limit){++EnabledCount;continue;}
        P->Enabled=false;
        P->Status=TEXT("Enabled plugin limit reached. Raise the limit in Plugin Manager to enable this plugin.");
        Notice+=P->Name+TEXT(": ")+P->Status+TEXT("\n");
        // Keep the saved permission grant so Refresh can restore the choice
        // after the user raises the limit. No script has started at this point.
    }
}
bool UACEPluginSubsystem::ValidatePublicProfile(const TSharedPtr<FJsonObject>& Profile) const { return Profile&&ValidUCMProfile(Profile)&&ValidItemToolsProfile(Profile); }
void UACEPluginSubsystem::SaveSettings() { if (!Write(UserDirectory() / TEXT("settings.json"), Settings)) Notice = TEXT("Could not save plugin preferences"); }
int32 UACEPluginSubsystem::GetEnabledPluginCount() const
{
    int32 Count=0;
    for(const auto& P:Plugins)if(P->Enabled)++Count;
    return Count;
}
int32 UACEPluginSubsystem::GetEnabledPluginLimit() const
{
    const double Limit=Number(Settings,TEXT("plugins.enabled_limit"),16);
    return Limit>=0 && Limit<=MAX_int32 && FMath::FloorToDouble(Limit)==Limit ? int32(Limit) : 16;
}
bool UACEPluginSubsystem::SetEnabledPluginLimit(int32 Limit)
{
    Notice.Empty();
    if(Limit<0){Notice=TEXT("Use 0 for unlimited, or a positive enabled plugin limit.");return false;}
    const int32 Count=GetEnabledPluginCount();
    if(Limit!=0 && Limit<Count)
    {
        Notice=FString::Printf(TEXT("%d plugins are enabled. Disable plugins first, or choose a limit of at least %d (0 means unlimited)."),Count,Count);
        return false;
    }
    Settings->SetNumberField(TEXT("plugins.enabled_limit"),Limit);
    SaveSettings();
    return Notice.IsEmpty();
}
void UACEPluginSubsystem::SetEnabled(const FString& Id, bool Enabled)
{
    if (auto P = Find(Id))
    {
        Notice.Empty();
        const int32 Limit=GetEnabledPluginLimit();
        if(Enabled && !P->Enabled && Limit!=0 && GetEnabledPluginCount()>=Limit)
        {
            Notice=FString::Printf(TEXT("Enabled plugin limit (%d) reached. Disable another plugin or raise the limit in Plugin Manager. Disabled plugins do not count."),Limit);
            return;
        }
        Stop(Id); P->Enabled = Enabled;
        if(P->LocalService) P->LocalServiceStopped=false;
        if(P->Companion) P->CompanionStopped=false;
        Settings->SetStringField(Id, Enabled ? TEXT("enabled:") + FString::Join(P->Permissions, TEXT(",")) : TEXT("disabled")); SaveSettings();
    }
}
void UACEPluginSubsystem::ToggleUCM()
{
    if(auto P=Find(TEXT("ucm")))
    {if(P->Running)Stop(P->Id,TEXT("Stopped"),true);else Start(P->Id);}
}
bool UACEPluginSubsystem::Start(const FString& Id)
{
    if(Id==TEXT("autocraft"))return StartAutoCraft();
    auto P = Find(Id); auto* C = GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(P && P->LocalService) return StartLocalService(*P);
    if(P && P->Companion) return StartCompanion(*P);
    if(P && P->Running)return true;
    if (!P || !P->Enabled || C->GetSessionState() != EACESessionState::InWorld || !C->GetPlayerVitalsView().bValid || (C->GetPlayerVitalsView().Health <= 0 && !P->Profile->HasField(TEXT("vt_meta"))))
    { Notice = TEXT("Enable the plugin and log into a living character before starting."); return false; }
    if(Id==TEXT("looteditor")||Id==TEXT("waypoint")||Id==TEXT("ucmmicro")||Id==TEXT("actiondeck")||IsLedger(Id)){Notice=TEXT("This plugin has no automation to start. Open its window from the plugin bar.");return false;}
    if(P->Api>=2&&!ValidatePublicProfile(P->Profile)){Notice=TEXT("Invalid automation profile structures");return false;}
    if (Id == TEXT("ucm") && !P->Running && P->ProfileLinks && !P->ProfileLinks->Values.IsEmpty())
    {
        TSharedPtr<FJsonObject> Reloaded, Links;
        if (!ACEUCMProfiles::Load(UserDirectory(), P->ProfileName, Reloaded, Links, Notice)) return false;
        if (!ValidUCMProfile(Reloaded)) { Notice = TEXT("Invalid linked UCM profile; Start cancelled"); return false; }
        // Starting an ordinary hunt only reads the selected files. Imported
        // metas may materialize dependencies and therefore still need a save.
        if(Reloaded->HasField(TEXT("vt_meta")))
        {
            auto Before=ACEUCMProfiles::Copy(Reloaded);
            if(!MaterializeUCMMetaProfiles(Reloaded,String(Links,TEXT("Metas"),TEXT("Imported")))
                ||!ACEUCMProfiles::Save(UserDirectory(),ACEUCMProfiles::ActiveName(),Reloaded,Links,Before,Notice,ValidUCMProfile))return false;
        }
        // Keep the VM and stable JSON ordering when only re-reading unchanged
        // files, so Stop/Resume still resumes an imported meta in place.
        if (!FJsonValue::CompareEqual(FJsonValueObject(P->Profile), FJsonValueObject(Reloaded))) P->Profile = Reloaded;
        P->ProfileLinks = Links; P->ProfileBaseline = ACEUCMProfiles::Copy(P->Profile);
        if (C->GetPlayerVitalsView().Health <= 0 && !P->Profile->HasField(TEXT("vt_meta"))) return false;
    }
    if(Id==TEXT("itemtools"))
    {
        for(const auto& Other:Plugins)if(Other->Running&&!Other->LocalService&&!Other->Companion&&Other->Id!=TEXT("ucm"))Stop(Other->Id,TEXT("Item Tools started"));
        MovementOwner.Empty();MoveExpires=0;FastCastOwner.Empty();OptionalCastOwner.Empty();FaceHeading.Reset();
        DeferredBuffIntent.Reset();DeferredBuffOwner.Empty();
        if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController())){PC->EndUseApproach();PC->CancelPluginJump();}UseApproachOwner.Empty();
        if(C->GetSession())C->GetSession()->SendCancelAttack();
        PhysicalAttackOwner.Empty();PhysicalAttackTarget=0;
    }
    else for(const auto& Other:Plugins) if(Other->Running&&!Other->LocalService&&!Other->Companion) Stop(Other->Id,TEXT("Another plugin started"));
    FString Source, Error;
    const FString Path = P->Directory / TEXT("main.lua");
    if (IFileManager::Get().FileSize(*Path) > 256 * 1024 || !FFileHelper::LoadFileToString(Source, *Path))
    { P->Status = TEXT("Cannot read main.lua (maximum 256 KiB)"); LogUCMEvent(*P,P->Status,EACEUCMLogLevel::Error);return false; }
    if(P->Api>=2 && !InitializePluginState(*P))return false;
    const uint32 SourceHash=GetTypeHash(Source);
    const uint32 ProfileHash=P->Profile->HasField(TEXT("vt_meta"))?GetTypeHash(Encode(P->Profile)):0;
    const bool Resume=P->CanResumeMeta&&P->VM.IsValid()&&P->VMSession.Pin()==C->GetSession()
        &&P->Player==C->GetPlayerGuid()&&P->Server==C->GetServerName()
        &&P->VMSourceHash==SourceHash&&P->VMProfileHash==ProfileHash;
    if(!Resume)
    {
        P->VM = MakeShared<FACEPluginVM>();
        if (!P->VM->Load(Source, Error, P->Api>=2?String(P->Manifest,TEXT("policy")):FString())) { P->Status = Error; LogUCMEvent(*P,TEXT("Script load failed: ")+Error,EACEUCMLogLevel::Error);P->VM.Reset();P->CanResumeMeta=false;return false; }
    }
    P->CanResumeMeta=false;P->ResumeMetaPending=Resume;P->VMSourceHash=SourceHash;P->VMProfileHash=ProfileHash;P->VMSession=C->GetSession();
    P->CombatTransitionIntent.Reset();P->Running = true; P->ActivityFailure.Reset(); P->Player = C->GetPlayerGuid(); P->Server = C->GetServerName();
    if(P->IsPolicy()){IdleManaVM.Reset();IdleManaOwner.Empty();IdleManaFailed=false;bRouteJoinRequested=true;RouteVisibilityOffset=0;
        bTrackRouteMovement=false;RouteMovementSamples.Reset();bRouteMovementOverflow=false;}
    if(P->IsPolicy()){CombatTarget=0;PhysicalAttackNextSendAt=0;PhysicalAttackTarget=0;PhysicalAttackOwner.Empty();CombatOutcomes.Empty();ClearBuffRequests();if(!Resume){RuntimeRoute.Reset();RoutePoint=1;}RouteRebuiltAt=0;}
    CachedSpells.Reset();
    P->LoggedActivity.Empty();P->LoggedRouteStatus.Empty();
    LogUCMEvent(*P,Resume?TEXT("UCM resumed"):TEXT("UCM started"));
    P->NextAction = 0; P->NextDecision=0;P->WaitAction.Empty();P->Status = TEXT("Running"); Notice.Empty(); return true;
}
bool UACEPluginSubsystem::RequestForceBuff()
{
    auto P=Find(TEXT("ucm"));if(!P)return false;
    if(ForceBuffRequest)return true; // Repeated clicks must not restart completed families.
    const bool WasRunning=P->Running&&!bUCMCommandOnly;
    if(!WasRunning&&!Start(P->Id))return false;
    bForceBuffOnly=!WasRunning;ForceBuffRequest=++ForceBuffSerial;
    if(!ForceBuffRequest)ForceBuffRequest=++ForceBuffSerial;
    MovementOwner.Empty();MoveExpires=0;P->NextDecision=0;
    P->Status=TEXT("Force Buff queued; refreshing enabled buffs");LogUCMEvent(*P,P->Status);return true;
}
bool UACEPluginSubsystem::RunUCMCommand(const FString& Arguments)
{
    const FString Args=Arguments.TrimStartAndEnd();
    if(Args.IsEmpty()){TogglePluginWindow(TEXT("ucm"));return true;}
    if(Args.Equals(TEXT("log"),ESearchCase::IgnoreCase)){TogglePluginWindow(TEXT("ucm.log"));return true;}
    if(Args.Equals(TEXT("stop"),ESearchCase::IgnoreCase)){Stop(TEXT("ucm"),TEXT("Stopped"),true);Notice=TEXT("UCM stopped.");return true;}
    if(Args.Equals(TEXT("start"),ESearchCase::IgnoreCase))return Start(TEXT("ucm"));
    if(Args.Equals(TEXT("help"),ESearchCase::IgnoreCase))
    {Notice=TEXT("/acueucm start | stop | log | fellow <command> | forcebuff | cancelforcebuff | jump <heading> <true/false> <milliseconds> [forward/strafeleft/straferight] | tapjump | opt set <option> <value> | enablecombat/enablebuffing/enablenav/enablelooting <true/false> | meta/nav/loot/looting/settings load <file> | setmetastate <state> | mexec <expression> | echo <text>");return true;}
    if(Args.StartsWith(TEXT("fellow "),ESearchCase::IgnoreCase))
    {auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Self;C->GetWorldObject(C->GetPlayerGuid(),Self);QueueFellowshipTell(Args.Mid(7),Self.Name,C->GetPlayerGuid(),true);Notice=TEXT("Fellowship command queued if automatic management is running.");return true;}
    TArray<FString> Issues;const auto Command=ACEVTProfile::CompileCommand(TEXT("/acueucm ")+Args,Issues);
    if(!Command||Issues.Num()){Notice=FString::Join(Issues,TEXT("\n"));return false;}
    auto P=Find(TEXT("ucm"));if(!P||!P->Enabled){Notice=TEXT("Enable UCM before using its commands.");return false;}
    const FString Op=String(Command,TEXT("op"));
    if(Op==TEXT("load"))
    {
        const FString Requested=String(Command,TEXT("profile"));
        for(const auto& File:GetProfileFiles(String(Command,TEXT("kind"))))if(File.Equals(Requested,ESearchCase::IgnoreCase))return SelectProfileFile(File);
        Notice=TEXT("Profile not found in the configured folder: ")+Requested;return false;
    }
    if(Op==TEXT("echo")){Notice=String(Command,TEXT("text"));return true;}
    if(Op==TEXT("forcebuff"))
    {
        if(Command->GetBoolField(TEXT("value")))return RequestForceBuff();
        CancelForceBuff();if(!P->Running)return true;
        // Also cancel a force-buff initiated by an imported meta command.
    }
    if(Op==TEXT("option")&&!P->Running)
    {
        auto Updated=MakeShared<FJsonObject>(*P->Profile);
        Updated->SetField(String(Command,TEXT("key")),Command->TryGetField(TEXT("value")));
        return SaveProfile(TEXT("ucm"),P->ProfileName,Encode(Updated));
    }
    if(Op==TEXT("state")&&!P->Running){Notice=TEXT("Start UCM before changing its active meta state.");return false;}
    if(UCMCommands.Num()>=64){Notice=TEXT("UCM command queue is full.");return false;}
    if(!P->Running)
    {
        if(!Start(TEXT("ucm")))return false;
        bUCMCommandOnly=true; // A manual jump must not also start hunting or looting.
    }
    UCMCommands.Add(MakeShared<FJsonValueObject>(Command));P->NextDecision=0;
    Notice=TEXT("Queued ")+ACEVTProfile::NativeCommand(TEXT("/acueucm ")+Args);return true;
}
void UACEPluginSubsystem::CancelForceBuff()
{
    ForceBuffRequest=0;
    if(bForceBuffOnly)Stop(TEXT("ucm"),TEXT("Force Buff cancelled"),true);
    bForceBuffOnly=false;
}
void UACEPluginSubsystem::Stop(const FString& Id, const FString& Reason, bool PreserveMeta)
{
    if (auto P = Find(Id))
    {
        P->ChatFilters.Reset();
        P->ChatInbox.Empty();P->ChatInboxBytes=0;P->ChatInboxDropped=0;
        const bool OwnsPolicy=P->IsPolicy()&&(P->Running||PolicyPlugin()==P);
        if(OwnsPolicy){RestoreWorldRendering();bTrackRouteMovement=false;RouteMovementSamples.Reset();bRouteMovementOverflow=false;
            FollowMovementSamples.Reset();FollowMovementTarget=0;bFollowMovementOverflow=false;}
        if(P->Companion){StopCompanion(*P,Reason);return;}
        if(P->HasPresentation()){FlushPluginState(*P);P->Terrain={};P->Markers.Reset();P->WorldPlates.Reset();++P->WorldPlateRevision;P->PanelRows.Reset();P->PluginEvents.Reset();++P->PanelRevision;}
        P->TargetingStatus.Empty();
        P->MetaStatus.Empty();if(!PreserveMeta)P->MetaState.Empty();
        if(P->LocalService)
        {
            if(const auto* Service=LocalServices.Find(Id)) (*Service)->Stop();
            P->Running=false; P->LocalServiceStopped=true; P->VM.Reset(); P->Status=Reason;
            return; // Cosmetic lifecycle must never cancel attacks, casts or movement.
        }
        if (P->Running)
        {
            LogUCMEvent(*P,TEXT("UCM stopped: ")+Reason,Reason.StartsWith(TEXT("Script error"))?EACEUCMLogLevel::Error:
                Reason.Contains(TEXT("died"))||Reason.Contains(TEXT("failed"))||Reason.Contains(TEXT("blocked"))?EACEUCMLogLevel::Warning:EACEUCMLogLevel::Info);
            FlushUCMLog();
            auto* C = GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            if (C && C->GetSession()) C->GetSession()->SendCancelAttack();
        }
        if((Id==TEXT("itemtools")||Id==TEXT("autocraft"))&&P->Running)if(auto UCM=Find(TEXT("ucm"));UCM&&UCM->Running)
        {UCM->ResumeMetaPending=true;UCM->NextDecision=0;UCM->NextAction=0;UCM->Status=TEXT("Resuming after inventory job");}
        P->CanResumeMeta=PreserveMeta&&P->IsPolicy()&&P->VM.IsValid()&&P->Profile->HasField(TEXT("vt_meta"));
        P->CombatTransitionIntent.Reset();P->Running = false;P->WaitAction.Empty();P->ResumeMetaPending=false;if(!P->CanResumeMeta)P->VM.Reset();P->Status=Reason;
        if(OwnsPolicy)if(auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>())C->SetPluginFellowshipUpdates(false);
        if(OwnsPolicy){ClearBuffRequests();FellowshipTells.Reset();FellowshipManagerStatus.Empty();UCMCraft.Reset();UCMServicesProfile.Reset();ForceBuffRequest=0;bForceBuffOnly=false;bUCMCommandOnly=false;UCMCommands.Reset();if(!P->CanResumeMeta){RuntimeRoute.Reset();RoutePoint=1;}RouteRebuiltAt=0;}
        if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
        {if(OwnsPolicy||MovementOwner==Id)PC->CancelPluginJump();if(UseApproachOwner==Id){PC->EndUseApproach();UseApproachOwner.Empty();}}
        if (MovementOwner == Id) { MovementOwner.Empty(); MoveExpires = 0; }
        if (FastCastOwner == Id) FastCastOwner.Empty();
        if (OptionalCastOwner == Id) OptionalCastOwner.Empty();
        if(OwnsPolicy)CombatTarget=0;
        if(PhysicalAttackOwner==Id){PhysicalAttackOwner.Empty();PhysicalAttackTarget=0;PhysicalAttackNextSendAt=0;}
        if (PendingSpellOwner == Id) { PendingSpell=0; PendingSpellOwner.Empty(); PendingSpellActivity.Empty(); }
        if(DeferredBuffOwner==Id){DeferredBuffIntent.Reset();DeferredBuffOwner.Empty();}
        if(!PendingSpell){BuffRetryAt=0;PendingBuffResultDriven=false;}
        OffensiveCasts.RemoveAll([&](const auto& E){return E.Owner==Id;});
    }
}
void UACEPluginSubsystem::StopAll(const FString& Reason, bool PreserveMeta) { for (const auto& P : Plugins) if (P->Running) Stop(P->Id, Reason, PreserveMeta); }
FString UACEPluginSubsystem::ProfileJson(const FString& Id) const { auto P = Find(Id); return P ? Encode(P->Profile) : FString(); }
bool UACEPluginSubsystem::SaveProfile(const FString& Id, const FString& Name, const FString& Json, bool bStopRunning)
{
    auto P = Find(Id); TSharedPtr<FJsonObject> O;
    if (!P || !SafeName(Name) || Json.Len() > 8 * 1024 * 1024 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), O) || !O)
    { Notice = TEXT("Use a profile name of letters, numbers, _ or -, and valid JSON (maximum 8 MiB)."); return false; }
    return SaveProfileData(Id,Name,O,bStopRunning);
}
bool UACEPluginSubsystem::SaveCurrentProfile(const FString& Id, bool bStopRunning)
{
    auto P=Find(Id);if(!P||!P->Profile)return false;
    // Native UCM controls already edited JSON objects. Save only replaces root
    // fields, so an owned root is enough; the rollback baseline remains deep-owned.
    // Avoid formatting, parsing and cloning every unchanged loot condition.
    // Shared-profile Save enforces the resolved 8 MiB limit before writing.
    if(Id==TEXT("ucm"))
    {
        auto Candidate=MakeShared<FJsonObject>();Candidate->Values=P->Profile->Values;
        return SaveProfileData(Id,P->ProfileName,Candidate,bStopRunning);
    }
    return SaveProfile(Id,P->ProfileName,ProfileJson(Id),bStopRunning);
}
bool UACEPluginSubsystem::SaveProfileData(const FString& Id, const FString& Name, TSharedPtr<FJsonObject> O, bool bStopRunning)
{
    auto P=Find(Id);if(!P||!O||!SafeName(Name))return false;
    if(Id==TEXT("ucm")&&!ValidUCMProfile(O)){Notice=TEXT("Invalid UCM profile: check rule types, name lengths (256 bytes), nonnegative values and minimum/maximum ranges. Routes/states must contain objects and item/spell lists numeric IDs.");return false;}
    if(P->Api>=2&&!ValidatePublicProfile(O)){Notice=TEXT("Invalid public profile structures");return false;}
    if(Id==TEXT("itemtools")&&!ValidItemToolsProfile(O)){Notice=TEXT("Invalid Item Tools quantities or loot rules.");return false;}
    // Invalidate a cooperative loot cursor even when edited rules have the same count.
    if(P->IsPolicy())O->SetNumberField(TEXT("loot_revision"),Number(P->Profile,TEXT("loot_revision"))+1);
    auto Links = P->ProfileLinks;
    if (Id == TEXT("ucm"))
    {
        if (!ACEUCMProfiles::Save(UserDirectory(), ACEUCMProfiles::ActiveName(), O, Links, P->ProfileBaseline, Notice, ValidUCMProfile))
        { if (P->ProfileBaseline) P->Profile = ACEUCMProfiles::Copy(P->ProfileBaseline); return false; }
    }
    else if (!Write(UserDirectory() / TEXT("Profiles") / Id / (Name + TEXT(".json")), O)) { Notice = TEXT("Profile could not be saved"); return false; }
    if(bStopRunning) Stop(Id, TEXT("Profile changed; press Start to resume")); P->Profile = O; P->ProfileName = Id==TEXT("ucm")?ACEUCMProfiles::ActiveName():Name;
    if (Id == TEXT("ucm")) { P->ProfileLinks = Links; P->ProfileBaseline = ACEUCMProfiles::Copy(O); }
    Settings->SetStringField(Id + TEXT(".profile"), P->ProfileName); SaveSettings(); Notice = TEXT("Profile saved"); return true;
}
bool UACEPluginSubsystem::LoadProfile(const FString& Id, const FString& Name)
{
    auto P = Find(Id); if (!P || !SafeName(Name)) return false;
    TSharedPtr<FJsonObject> O, Links;
    if (Id == TEXT("ucm"))
    {
        if (!ACEUCMProfiles::Load(UserDirectory(), Name, O, Links, Notice)) return false;
    }
    else O = Read(UserDirectory() / TEXT("Profiles") / Id / (Name + TEXT(".json")));
    if (!O || (Id==TEXT("ucm")&&!ValidUCMProfile(O)) || (P->Api>=2&&!ValidatePublicProfile(O)) || (Id==TEXT("itemtools")&&!ValidItemToolsProfile(O))) {Notice=TEXT("Profile is missing or has invalid data");return false;}
    if (Id == TEXT("ucm") && Links->Values.IsEmpty()
        && !ACEUCMProfiles::Save(UserDirectory(), Name, O, Links, nullptr, Notice, ValidUCMProfile)) return false;
    if(Id==TEXT("ucm"))
    {
        auto Before=ACEUCMProfiles::Copy(O);
        if(!MaterializeUCMMetaProfiles(O,String(Links,TEXT("Metas"),TEXT("Imported")))
            ||!ACEUCMProfiles::Save(UserDirectory(),ACEUCMProfiles::ActiveName(),O,Links,Before,Notice,ValidUCMProfile))return false;
    }
    Stop(Id); P->Profile = O; P->ProfileName = Id==TEXT("ucm")?ACEUCMProfiles::ActiveName():Name;
    if (Id == TEXT("ucm")) { P->ProfileLinks = Links; P->ProfileBaseline = ACEUCMProfiles::Copy(O); }
    Settings->SetStringField(Id + TEXT(".profile"), P->ProfileName); SaveSettings(); return true;
}
TArray<FString> UACEPluginSubsystem::UCMSectionProfiles(const FString& Section) const
{ return ACEUCMProfiles::List(UserDirectory(), Section); }
FString UACEPluginSubsystem::UCMSectionName(const FString& Section) const
{
    auto P = Find(TEXT("ucm")); FString Name;
    if (P && P->ProfileLinks) P->ProfileLinks->TryGetStringField(Section, Name);
    return Name;
}
bool UACEPluginSubsystem::SelectUCMSection(const FString& Section, const FString& Name)
{
    auto P = Find(TEXT("ucm")); if (!P) return false;
    auto O = P->Profile, Links = P->ProfileLinks;
    if (!ACEUCMProfiles::Select(UserDirectory(), Section, Name, O, Links, Notice)) return false;
    if (!ValidUCMProfile(O)) { Notice = TEXT("Invalid shared profile data"); return false; }
    auto Before=ACEUCMProfiles::Copy(O);
    if(Section==TEXT("Metas")&&!MaterializeUCMMetaProfiles(O,Name))return false;
    if(Section==TEXT("Metas")&&O->HasField(TEXT("vt_meta")))O->SetBoolField(TEXT("meta_enabled"),true);
    // Selection replaces a reference, never overwrites the previously selected file.
    if (!ACEUCMProfiles::Save(UserDirectory(), ACEUCMProfiles::ActiveName(), O, Links, Before, Notice, ValidUCMProfile)) return false;
    Stop(P->Id, TEXT("Profile changed; press Start to resume"));
    P->Profile = O; P->ProfileName=ACEUCMProfiles::ActiveName(); P->ProfileLinks = Links; P->ProfileBaseline = ACEUCMProfiles::Copy(O);
    Notice = TEXT("Selected ") + ACEUCMProfiles::DisplaySection(Section) + TEXT(": ") + Name; return true;
}
bool UACEPluginSubsystem::SaveUCMSectionAs(const FString& Section, const FString& Name)
{
    auto P = Find(TEXT("ucm")); if (!P || !ValidUCMProfile(P->Profile)) return false;
    if (!ACEUCMProfiles::SafeName(Name) || !ACEUCMProfiles::Sections().Contains(Section))
    { Notice = TEXT("Use up to 64 letters, numbers, spaces, _ or -. Avoid reserved filenames such as CON."); return false; }
    if (IFileManager::Get().FileExists(*(UserDirectory()/TEXT("Profiles")/Section/(Name+TEXT(".json")))))
    { Notice = TEXT("That profile already exists. Choose it from the list to edit it, or use a new name for a copy."); return false; }
    if (!SaveProfile(P->Id, P->ProfileName, ProfileJson(P->Id))) return false;
    if (!ACEUCMProfiles::SaveSectionAs(UserDirectory(), Section, Name, P->Profile, Notice)) return false;
    return SelectUCMSection(Section, Name);
}

bool UACEPluginSubsystem::NewUCMSection(const FString& Section, const FString& Name)
{
    auto P=Find(TEXT("ucm"));if(!P)return false;
    auto Defaults=Read(P->Directory/TEXT("default.json"));
    if(!Defaults||!ValidUCMProfile(Defaults)){Notice=TEXT("Cannot read bundled UCM defaults");return false;}
    if(!ACEUCMProfiles::SaveSectionAs(UserDirectory(),Section,Name,Defaults,Notice))return false;
    if(SelectUCMSection(Section,Name))
    {Notice=TEXT("Created and selected ")+ACEUCMProfiles::DisplaySection(Section)+TEXT(": ")+Name+TEXT(".json. Changes save automatically.");return true;}
    IFileManager::Get().Delete(*(UserDirectory()/TEXT("Profiles")/Section/(Name+TEXT(".json"))));return false;
}
bool UACEPluginSubsystem::DeleteUCMSection(const FString& Section, const FString& Name)
{
    auto P=Find(TEXT("ucm"));if(!P)return false;
    if(!ACEUCMProfiles::EnsureDefaults(UserDirectory(),Read(P->Directory/TEXT("default.json")),Notice))return false;
    auto Candidate=P->Profile,Links=P->ProfileLinks;
    if(!ACEUCMProfiles::Select(UserDirectory(),Section,TEXT("Default"),Candidate,Links,Notice)||!ValidUCMProfile(Candidate))
    {Notice=TEXT("Default contains invalid settings; profile was not deleted.");return false;}
    if(!SaveProfile(P->Id,P->ProfileName,ProfileJson(P->Id),false))return false;
    if(!ACEUCMProfiles::DeleteSection(UserDirectory(),Section,Name,Notice))return false;
    if(!LoadProfile(P->Id,P->ProfileName))return false;
    Notice=TEXT("Deleted ")+ACEUCMProfiles::DisplaySection(Section)+TEXT(": ")+Name+TEXT(". References now use Default; a backup is in Profiles/Deleted.");return true;
}
bool UACEPluginSubsystem::InitializeUCMSelection()
{
    auto P=Find(TEXT("ucm"));if(!P)return false;
    auto O=Read(P->Directory/TEXT("default.json"));TSharedPtr<FJsonObject> Links=MakeShared<FJsonObject>();
    if(!ACEUCMProfiles::EnsureDefaults(UserDirectory(),O,Notice))return false;
    // Preserve old files and expose their individual sections. Existing linked
    // profiles retain their names; monolithic files get recoverable backups.
    for(const auto& Legacy:ACEUCMProfiles::List(UserDirectory(),TEXT("ucm")))
    {
        TSharedPtr<FJsonObject> Old,Refs;FString Error;
        if(!ACEUCMProfiles::Load(UserDirectory(),Legacy,Old,Refs,Error)||!ValidUCMProfile(Old))continue;
        if(Refs->Values.IsEmpty()&&!ACEUCMProfiles::Save(UserDirectory(),Legacy,Old,Refs,nullptr,Notice,ValidUCMProfile))return false;
    }
    const FString Previous=String(Settings,TEXT("ucm.profile"),TEXT("Default"));
    if(!ACEUCMProfiles::MigrateAnonymousImports(UserDirectory(),Notice))return false;
    if(IFileManager::Get().FileExists(*(UserDirectory()/TEXT("ucm-selection.json"))))return LoadProfile(P->Id,ACEUCMProfiles::ActiveName());
    if(IFileManager::Get().FileExists(*(UserDirectory()/TEXT("Profiles/ucm")/(Previous+TEXT(".json")))))return LoadProfile(P->Id,Previous);
    for(const auto& Section:ACEUCMProfiles::Sections())
        if(!ACEUCMProfiles::Select(UserDirectory(),Section,TEXT("Default"),O,Links,Notice))return false;
    if(!ACEUCMProfiles::SaveSelection(UserDirectory(),O,Links,Notice))return false;
    return LoadProfile(P->Id,ACEUCMProfiles::ActiveName());
}
bool UACEPluginSubsystem::ImportUCMSection(const FString& Filename)
{
    auto P=Find(TEXT("ucm"));const FString Ext=FPaths::GetExtension(Filename).ToLower();
    if(!P||(Ext!=TEXT("nav")&&Ext!=TEXT("utl"))||Filename!=FPaths::GetCleanFilename(Filename)||!GetProfileFiles(Ext).Contains(Filename))
    {Notice=TEXT("Choose a .nav or .utl file from the import folder.");return false;}
    if(!ImportedUCMProfiles(GetProfileFolder()/Filename)->Values.IsEmpty())return ReuseUCMImport(GetProfileFolder()/Filename);
    TArray<FString> Issues;auto Converted=ACEVTProfile::ConvertFile(GetProfileFolder()/Filename,Issues);
    if(!Converted||Issues.Num()){Notice=TEXT("Cannot import ")+Filename+TEXT(": ")+FString::Join(Issues,TEXT("\n"));return false;}
    const FString Section=Ext==TEXT("nav")?TEXT("Routes"):TEXT("Loot");
    auto O=Read(P->Directory/TEXT("default.json"));if(!O){Notice=TEXT("Cannot read bundled UCM defaults");return false;}
    for(const auto& Pair:Converted->Values)if(ACEUCMProfiles::SectionFor(FString(Pair.Key))==Section)O->Values.Add(Pair.Key,Pair.Value);
    O->SetStringField(Ext+TEXT("_source"),GetProfileFolder()/Filename);
    if(Ext==TEXT("nav"))O->SetBoolField(TEXT("route_preview"),true);
    if(!ValidUCMProfile(O)){Notice=TEXT("Imported profile contains invalid settings");return false;}
    const FString Name=ACEUCMProfiles::UniqueName(UserDirectory(),Section,FPaths::GetBaseFilename(Filename));
    if(!ACEUCMProfiles::SaveSectionAs(UserDirectory(),Section,Name,O,Notice))return false;
    if(!SelectUCMSection(Section,Name)){IFileManager::Get().Delete(*(UserDirectory()/TEXT("Profiles")/Section/(Name+TEXT(".json"))));return false;}
    auto Refs=MakeShared<FJsonObject>();Refs->SetStringField(Section,Name);
    if(!RememberUCMImport(GetProfileFolder()/Filename,Refs))return false;
    Notice=TEXT("Imported ")+Filename+TEXT(" as ")+Section+TEXT(": ")+Name+TEXT(". Original file unchanged.");return true;
}

TSharedPtr<FJsonObject> UACEPluginSubsystem::ImportedUCMProfiles(const FString& Source) const
{
    const FString Full=UCMImportPath(Source);
    auto Result=MakeShared<FJsonObject>();auto Index=Read(UserDirectory()/TEXT("import-index.json"));
    const TSharedPtr<FJsonObject>* Entry=nullptr;
    if(Index&&Index->TryGetObjectField(Full,Entry))for(const auto& Pair:(*Entry)->Values)
        if(Pair.Value->Type==EJson::String&&!Pair.Value->AsString().Equals(TEXT("Default"),ESearchCase::IgnoreCase)
            &&ACEUCMProfiles::Sections().Contains(FString(Pair.Key))&&ACEUCMProfiles::SafeName(Pair.Value->AsString())
            &&IFileManager::Get().FileExists(*(UserDirectory()/TEXT("Profiles")/FString(Pair.Key)/(Pair.Value->AsString()+TEXT(".json")))))
            Result->Values.Add(Pair.Key,Pair.Value);
    // Adopt imports made before the index existed, including dependencies
    // exported by a meta. Never confuse same-named files in different folders.
    const FString Ext=FPaths::GetExtension(Source).ToLower();
    const FString Section=Ext==TEXT("nav")?TEXT("Routes"):Ext==TEXT("utl")?TEXT("Loot"):Ext==TEXT("met")?TEXT("Metas"):TEXT("Character");
    if(!Result->HasField(Section))for(const auto& Name:UCMSectionProfiles(Section))
    {
        auto Part=Read(UserDirectory()/TEXT("Profiles")/Section/(Name+TEXT(".json")));
        const FString Original=String(Part,*(Ext+TEXT("_source")));if(Original.IsEmpty())continue;
        const FString Candidate=Original==FPaths::GetCleanFilename(Original)?GetProfileFolder()/Original:Original;
        if(FPaths::IsSamePath(Full,UCMImportPath(Candidate)))
        {Result->SetStringField(Section,Name);break;}
    }
    // A deleted primary profile cannot be considered imported just because
    // a companion section (such as Monsters from a USD) still exists.
    if(!Result->HasField(Section))Result->Values.Empty();
    return Result;
}
bool UACEPluginSubsystem::RememberUCMImport(const FString& Source,const TSharedPtr<FJsonObject>& Links)
{
    const FString Path=UserDirectory()/TEXT("import-index.json"),Full=UCMImportPath(Source);
    auto Index=Read(Path);if(!Index)Index=MakeShared<FJsonObject>();
    auto Refs=ImportedUCMProfiles(Source);for(const auto& Pair:Links->Values)Refs->Values.Add(Pair.Key,Pair.Value);
    const TSharedPtr<FJsonObject>* Existing=nullptr;
    if(Index->TryGetObjectField(Full,Existing)&&FJsonValue::CompareEqual(FJsonValueObject(*Existing),FJsonValueObject(Refs)))return true;
    Index->SetObjectField(Full,Refs);
    if(!Write(Path,Index)){Notice=TEXT("Could not save the import index.");return false;}return true;
}
bool UACEPluginSubsystem::ReuseUCMImport(const FString& Source)
{
    auto P=Find(TEXT("ucm"));if(!P)return false;
    const auto Refs=ImportedUCMProfiles(Source);if(Refs->Values.IsEmpty())return false;
    auto O=P->Profile,Links=P->ProfileLinks;
    for(const auto& Pair:Refs->Values)if(!ACEUCMProfiles::Select(UserDirectory(),FString(Pair.Key),Pair.Value->AsString(),O,Links,Notice))return false;
    if(Refs->HasField(TEXT("Metas"))&&O->HasField(TEXT("vt_meta")))O->SetBoolField(TEXT("meta_enabled"),true);
    if(!ValidUCMProfile(O)){Notice=TEXT("Previously imported profile contains invalid settings; current selection was kept.");return false;}
    if(!ACEUCMProfiles::SaveSelection(UserDirectory(),O,Links,Notice)||!LoadProfile(P->Id,ACEUCMProfiles::ActiveName()))return false;
    Notice=TEXT("Already imported: ")+FPaths::GetCleanFilename(Source)+TEXT(". Selected existing profiles; your edits were kept.");return true;
}

TSharedPtr<FJsonObject> UACEPluginSubsystem::InspectLegacyProfile(const FString& Path)
{
    const int64 Bytes=IFileManager::Get().FileSize(*Path);FString Source,Error;
    if(Bytes<0||Bytes>4*1024*1024||!FFileHelper::LoadFileToString(Source,*Path)){Notice=TEXT("Cannot read profile (maximum 4 MiB).");return nullptr;}
    auto D=ACEVTProfile::Read(Source,FPaths::GetExtension(Path),Error);
    if(!D)Notice=Error;
    return D;
}
bool UACEPluginSubsystem::SaveLegacyLoot(const FString& Name,const TSharedPtr<FJsonObject>& Document)
{
    if(!SafeName(Name)){Notice=TEXT("Choose a filename using letters, numbers, _ or - (maximum 64).");return false;}
    FString Error;const FString Source=ACEVTProfile::WriteLoot(Document,Error);if(!Error.IsEmpty()){Notice=Error;return false;}
    const FString Path=UserDirectory()/TEXT("LegacyLootProfiles")/(Name+TEXT(".utl"));IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    if(!FFileHelper::SaveStringToFile(Source,*(Path+TEXT(".tmp")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)||!IFileManager::Get().Move(*Path,*(Path+TEXT(".tmp")),true,true)){Notice=TEXT("Could not save UTL copy.");return false;}
    Notice=TEXT("Saved UTL copy: ")+Path+TEXT(". Automation has not changed.");return true;
}
FString UACEPluginSubsystem::GetProfileFolder() const
{
    FString Folder;if(Settings)Settings->TryGetStringField(TEXT("ucm.folder"),Folder);
    return Folder.IsEmpty()?UserDirectory()/TEXT("ImportInbox"):Folder;
}
bool UACEPluginSubsystem::SetProfileFolder(const FString& Folder)
{
    const FString Full=FPaths::ConvertRelativePathToFull(Folder.TrimStartAndEnd());
    if(Folder.TrimStartAndEnd().IsEmpty()||!IFileManager::Get().DirectoryExists(*Full)){Notice=TEXT("Profile folder does not exist.");return false;}
    Settings->SetStringField(TEXT("ucm.folder"),Full);SaveSettings();Notice=TEXT("Profile folder refreshed: ")+Full;return true;
}
TArray<FString> UACEPluginSubsystem::GetProfileFiles(const FString& Extension) const
{
    TArray<FString> Files;if(Extension!=TEXT("nav")&&Extension!=TEXT("met")&&Extension!=TEXT("utl")&&Extension!=TEXT("usd"))return Files;
    // Filter ourselves so a folder copied from Windows works on case-sensitive hosts.
    TArray<FString> All;IFileManager::Get().FindFiles(All,*(GetProfileFolder()/TEXT("*")),true,false);
    for(const auto& File:All)if(FPaths::GetExtension(File).Equals(Extension,ESearchCase::IgnoreCase))Files.Add(File);
    Files.Sort();return Files;
}
bool UACEPluginSubsystem::SelectProfileFile(const FString& Filename)
{
    const auto P=Find(TEXT("ucm"));const FString Ext=FPaths::GetExtension(Filename).ToLower();
    if(!P||Filename!=FPaths::GetCleanFilename(Filename)||!GetProfileFiles(Ext).Contains(Filename))return false;
    if(Ext==TEXT("nav")||Ext==TEXT("utl"))
    {
        if(!ImportUCMSection(Filename))return false;
        // Explicit route selection is ready to follow on Start. Shared route
        // data and inactive meta dependencies do not own the activity switch.
        // Bulk imports restore the previous selection and switches per file.
        bool Navigation=false;P->Profile->TryGetBoolField(TEXT("navigation"),Navigation);
        if(Ext==TEXT("nav")&&!Navigation)
        {
            const FString ImportNotice=Notice;
            auto Updated=MakeShared<FJsonObject>(*P->Profile);Updated->SetBoolField(TEXT("navigation"),true);
            if(!SaveProfile(P->Id,P->ProfileName,Encode(Updated)))return false;
            Notice=ImportNotice;
        }
        return true;
    }
    if(!ImportedUCMProfiles(GetProfileFolder()/Filename)->Values.IsEmpty())return ReuseUCMImport(GetProfileFolder()/Filename);
    TArray<FString> Issues;const auto Converted=ACEVTProfile::ConvertFile(GetProfileFolder()/Filename,Issues);
    if(!Converted||Issues.Num()){Notice=TEXT("Cannot load ")+Filename+TEXT(": ")+(Issues.Num()?FString::Join(Issues,TEXT("\n")):TEXT("Invalid profile"));return false;}
    auto Updated=MakeShared<FJsonObject>(*P->Profile);
    if(Ext==TEXT("usd")){const TArray<TSharedPtr<FJsonValue>>* Keys=nullptr;if(Updated->TryGetArrayField(TEXT("usd_keys"),Keys)){const auto Copy=*Keys;for(const auto& Key:Copy)Updated->RemoveField(Key->AsString());}}
    if(Ext==TEXT("nav"))for(const TCHAR* Key:{TEXT("route"),TEXT("loop_route"),TEXT("reverse_route"),TEXT("follow_target"),TEXT("follow_distance")})Updated->RemoveField(Key);
    if(Ext==TEXT("met")){Updated->RemoveField(TEXT("vt_meta"));Updated->RemoveField(TEXT("vt_library"));Updated->RemoveField(TEXT("meta_profile_refs"));}
    if(Ext==TEXT("utl")){Updated->RemoveField(TEXT("salvage_policy"));Updated->RemoveField(TEXT("loot_profile"));Updated->RemoveField(TEXT("loot_modified"));}
    for(const auto& Pair:Converted->Values)
    {
        if(Pair.Key==TEXT("vt_library"))
        {auto Library=MakeShared<FJsonObject>();const TSharedPtr<FJsonObject>* Old=nullptr;if(Updated->TryGetObjectField(TEXT("vt_library"),Old))Library->Values=(*Old)->Values;
         for(const auto& Entry:Pair.Value->AsObject()->Values)Library->Values.Add(Entry.Key,Entry.Value);Updated->SetObjectField(Pair.Key,Library);}
        else Updated->Values.Add(Pair.Key,Pair.Value);
    }
    Updated->SetStringField(Ext+TEXT("_source"),GetProfileFolder()/Filename);
    if(Ext==TEXT("nav"))
    {
        Updated->SetBoolField(TEXT("route_preview"),true);
        // Conversion is also used for inactive meta dependencies. A route the
        // user explicitly selects is ready to follow when they press Start.
        Updated->SetBoolField(TEXT("navigation"),true);
    }
    return SaveImportedProfile(FPaths::GetBaseFilename(Filename),Updated,GetProfileFolder()/Filename);
}
bool UACEPluginSubsystem::ClearProfileFile(const FString& Ext)
{
    const auto P=Find(TEXT("ucm"));if(!P||(Ext!=TEXT("nav")&&Ext!=TEXT("met")&&Ext!=TEXT("utl")&&Ext!=TEXT("usd")))return false;
    TSharedPtr<FJsonObject> Updated=MakeShared<FJsonObject>(*P->Profile);Updated->RemoveField(Ext+TEXT("_source"));
    if(Ext==TEXT("usd")){const TArray<TSharedPtr<FJsonValue>>* Keys=nullptr;if(Updated->TryGetArrayField(TEXT("usd_keys"),Keys)){const auto Copy=*Keys;for(const auto& Key:Copy)Updated->RemoveField(Key->AsString());}Updated->RemoveField(TEXT("usd_keys"));}
    if(Ext==TEXT("nav")){Updated->SetArrayField(TEXT("route"),{});Updated->SetBoolField(TEXT("navigation"),false);Updated->SetStringField(TEXT("follow_name"),TEXT(""));Updated->SetNumberField(TEXT("follow_id"),0);}
    if(Ext==TEXT("met")){Updated->RemoveField(TEXT("vt_meta"));Updated->RemoveField(TEXT("vt_library"));Updated->SetBoolField(TEXT("meta_enabled"),false);}
    if(Ext==TEXT("utl")){Updated->SetArrayField(TEXT("loot_rules"),{});Updated->RemoveField(TEXT("salvage_policy"));Updated->RemoveField(TEXT("loot_profile"));Updated->RemoveField(TEXT("loot_modified"));Updated->SetBoolField(TEXT("looting"),false);}
    // Clearing a selection must never erase a file referenced by another setup.
    auto Links = P->ProfileLinks ? ACEUCMProfiles::Copy(P->ProfileLinks) : MakeShared<FJsonObject>();
    for (const auto& Section : ACEUCMProfiles::Sections())
    {
        if (FJsonValue::CompareEqual(FJsonValueObject(ACEUCMProfiles::Extract(Updated, Section)),
            FJsonValueObject(ACEUCMProfiles::Extract(P->Profile, Section)))) continue;
        const FString EmptyName = ACEUCMProfiles::UniqueName(UserDirectory(), Section, P->ProfileName + TEXT("_Cleared"));
        if (!ACEUCMProfiles::SaveSectionAs(UserDirectory(), Section, EmptyName, Updated, Notice)) return false;
        Links->SetStringField(Section, EmptyName);
    }
    if (!ACEUCMProfiles::Save(UserDirectory(), P->ProfileName, Updated, Links, Updated, Notice, ValidUCMProfile)) return false;
    return LoadProfile(P->Id, P->ProfileName);
}
bool UACEPluginSubsystem::ImportLegacyProfile(const FString& Path,const FString& Name)
{
    auto P=Find(TEXT("ucm"));if(!P||!SafeName(Name)){Notice=TEXT("Choose an import name using letters, numbers, _ or - (maximum 64).");return false;}
    if(!ImportedUCMProfiles(Path)->Values.IsEmpty())return ReuseUCMImport(Path);
    auto D=InspectLegacyProfile(Path);if(!D)return false;
    TArray<FString> Issues;auto Converted=ACEVTProfile::ConvertFile(Path,Issues);
    auto Archive=MakeShared<FJsonObject>();Archive->SetObjectField(TEXT("document"),D);Archive->SetObjectField(TEXT("converted"),Converted);
    TArray<TSharedPtr<FJsonValue>> Warnings;for(const auto& Issue:Issues)Warnings.Add(MakeShared<FJsonValueString>(Issue));Archive->SetArrayField(TEXT("issues"),Warnings);
    const FString Report=Name+TEXT("_")+FMD5::HashAnsiString(*FPaths::ConvertRelativePathToFull(Path)).Left(12);
    if(!Write(UserDirectory()/TEXT("Imports")/(Report+TEXT(".json")),Archive)){Notice=TEXT("Could not save import report.");return false;}
    if(Issues.Num()){Notice=FString::Printf(TEXT("%d compatibility issues. Original data and report saved in Imports/%s.json; active settings were not changed."),Issues.Num(),*Report);return false;}
    auto Updated=MakeShared<FJsonObject>();Updated->Values=P->Profile->Values;
    const FString Kind=FPaths::GetExtension(Path).ToLower();
    if(Kind==TEXT("met")){Updated->RemoveField(TEXT("vt_meta"));Updated->RemoveField(TEXT("vt_library"));Updated->RemoveField(TEXT("meta_profile_refs"));}
    for(const auto& Pair:Converted->Values)Updated->Values.Add(Pair.Key,Pair.Value);
    Updated->SetStringField(Kind+TEXT("_source"),Path);
    return SaveImportedProfile(Name,Updated,Path);
}
bool UACEPluginSubsystem::SaveImportedProfile(const FString& Name,const TSharedPtr<FJsonObject>& Profile,const FString& Source)
{
    if(!ImportedUCMProfiles(Source)->Values.IsEmpty())return ReuseUCMImport(Source);
    Profile->SetStringField(TEXT("import_source"),FPaths::ConvertRelativePathToFull(Source));
    Profile->SetNumberField(TEXT("ucm_format_version"),1);
    if(!ValidUCMProfile(Profile)||Encode(Profile).Len()>8*1024*1024){Notice=TEXT("Converted profile exceeds UCM limits.");return false;}
    auto P = Find(TEXT("ucm")); if (!P) return false;
    auto O = ACEUCMProfiles::Copy(Profile);
    auto Links = P->ProfileLinks ? ACEUCMProfiles::Copy(P->ProfileLinks) : MakeShared<FJsonObject>();
    auto ImportedLinks=MakeShared<FJsonObject>();
    const bool NewMeta=!FJsonValue::CompareEqual(FJsonValueObject(ACEUCMProfiles::Extract(O,TEXT("Metas"))),FJsonValueObject(ACEUCMProfiles::Extract(P->Profile,TEXT("Metas"))));
    const FString MetaName=NewMeta?ACEUCMProfiles::UniqueName(UserDirectory(),TEXT("Metas"),Name):String(Links,TEXT("Metas"),Name);
    if(!MaterializeUCMMetaProfiles(O,MetaName))return false;
    for (const auto& Section : ACEUCMProfiles::Sections())
    {
        const bool Same = FJsonValue::CompareEqual(FJsonValueObject(ACEUCMProfiles::Extract(O, Section)),
            FJsonValueObject(ACEUCMProfiles::Extract(P->Profile, Section)));
        if (Links->HasField(Section) && Same) continue;
        const FString PartName = Section==TEXT("Metas")&&NewMeta?MetaName:ACEUCMProfiles::UniqueName(UserDirectory(), Section, Name);
        if (!ACEUCMProfiles::SaveSectionAs(UserDirectory(), Section, PartName, O, Notice)) return false;
        Links->SetStringField(Section, PartName);
        ImportedLinks->SetStringField(Section,PartName);
    }
    if (!ACEUCMProfiles::SaveSelection(UserDirectory(), O, Links, Notice) || !LoadProfile(TEXT("ucm"), ACEUCMProfiles::ActiveName())) return false;
    if(!RememberUCMImport(Source,ImportedLinks))return false;
    Notice = TEXT("Imported profiles: ") + Name + TEXT(". Original files are unchanged. Press Start to run."); return true;
}
bool UACEPluginSubsystem::MaterializeUCMMetaProfiles(const TSharedPtr<FJsonObject>& Profile,const FString& Name)
{
    if(!Profile->HasField(TEXT("vt_meta")))return true;
    auto P=Find(TEXT("ucm"));if(!P)return false;
    auto Defaults=Read(P->Directory/TEXT("default.json"));if(!Defaults)return false;
    const TSharedPtr<FJsonObject>* OldRefs=nullptr;
    auto Refs=Profile->TryGetObjectField(TEXT("meta_profile_refs"),OldRefs)?ACEUCMProfiles::Copy(*OldRefs):MakeShared<FJsonObject>();
    const FString RootFile=FPaths::GetCleanFilename(String(Profile,TEXT("met_source"),String(Profile,TEXT("import_source")))).ToLower();
    if(RootFile.EndsWith(TEXT(".met")))
    {auto RootRef=MakeShared<FJsonObject>();RootRef->SetStringField(TEXT("Metas"),Name);Refs->SetObjectField(RootFile,RootRef);}
    struct FPending { FString Section,Name;TSharedPtr<FJsonObject> Data; };
    TArray<FPending> Pending;
    bool RoutesValid=true;
    TSet<FString> Reserved;
    auto Allocate=[&](const FString& Section,const FString& Stem)
    {
        FString Candidate=ACEUCMProfiles::UniqueName(UserDirectory(),Section,Stem);
        for(int32 I=2;Reserved.Contains(Section+TEXT("/")+Candidate);++I)
            Candidate=ACEUCMProfiles::UniqueName(UserDirectory(),Section,Stem.Left(40)+FString::Printf(TEXT("_%d"),I));
        Reserved.Add(Section+TEXT("/")+Candidate);return Candidate;
    };
    // Export every embedded route, retaining its state and source name. Empty
    // routes are intentional (VT uses them to clear navigation) and stay empty.
    auto Routes=[&](const TSharedPtr<FJsonObject>& Meta,const FString& Stem)
    {
        int32 Number=0;
        TFunction<void(const TSharedPtr<FJsonValue>&,const FString&)> Visit;
        Visit=[&](const TSharedPtr<FJsonValue>& V,const FString& State)
        {
            if(V->Type==EJson::Array){for(const auto& Child:V->AsArray())Visit(Child,State);return;}
            if(V->Type!=EJson::Object)return;
            auto Node=V->AsObject();const FString Current=String(Node,TEXT("state"),State);
            const TSharedPtr<FJsonObject>* Route=nullptr;
            if(Node->TryGetObjectField(TEXT("route"),Route))
            {
                ++Number;FString RouteName=String(Node,TEXT("route_profile"));
                if(!ACEUCMProfiles::List(UserDirectory(),TEXT("Routes")).Contains(RouteName))
                {
                    FString Label=String(Node,TEXT("route_name"));
                    if(Label.IsEmpty()||Label==TEXT("[None]"))Label=Current+FString::Printf(TEXT("_Route_%d"),Number);
                    RouteName=Allocate(TEXT("Routes"),Stem+TEXT("_")+FPaths::GetBaseFilename(Label));
                    auto Data=ACEUCMProfiles::Copy(Defaults);
                    for(const auto& Pair:(*Route)->Values)Data->Values.Add(Pair.Key,Pair.Value);
                    Data->SetBoolField(TEXT("route_preview"),true);
                    Pending.Add({TEXT("Routes"),RouteName,Data});Node->SetStringField(TEXT("route_profile"),RouteName);
                }
                else
                {
                    // Named routes are reusable: edits in the Route tab must
                    // also reach the Lua policy, not only the native preview.
                    auto Resolved=ACEUCMProfiles::Copy(Defaults);TSharedPtr<FJsonObject> RouteLinks=MakeShared<FJsonObject>();
                    if(ACEUCMProfiles::Select(UserDirectory(),TEXT("Routes"),RouteName,Resolved,RouteLinks,Notice))
                        Node->SetObjectField(TEXT("route"),ACEUCMProfiles::Extract(Resolved,TEXT("Routes")));
                    else RoutesValid=false;
                }
                return;
            }
            for(const auto& Pair:Node->Values)Visit(Pair.Value,Current);
        };
        const TArray<TSharedPtr<FJsonValue>>* Rules=nullptr;
        if(Meta->TryGetArrayField(TEXT("vt_meta"),Rules))for(const auto& Rule:*Rules)Visit(Rule,TEXT("Default"));
    };
    Routes(Profile,Name);
    const TSharedPtr<FJsonObject>* Library=nullptr;
    if(Profile->TryGetObjectField(TEXT("vt_library"),Library))
    {
        TArray<FString> Files;for(const auto& Pair:(*Library)->Values)Files.Add(FString(Pair.Key));Files.Sort();
        for(const auto& File:Files)
        {
            const FString Ext=FPaths::GetExtension(File).ToLower();if(File==RootFile)continue;
            const auto Data=(*Library)->GetObjectField(File);
            FString Stem=FPaths::GetBaseFilename(File);
            for(const auto& Original:GetProfileFiles(Ext))if(Original.Equals(File,ESearchCase::IgnoreCase)){Stem=FPaths::GetBaseFilename(Original);break;}
            const TSharedPtr<FJsonObject>* Existing=nullptr;
            auto FileRefs=Refs->TryGetObjectField(File,Existing)?ACEUCMProfiles::Copy(*Existing):MakeShared<FJsonObject>();
            const FString Primary=Ext==TEXT("nav")?TEXT("Routes"):Ext==TEXT("utl")?TEXT("Loot"):Ext==TEXT("usd")?TEXT("Character"):TEXT("Metas");
            for(const auto& Section:ACEUCMProfiles::Sections())
            {
                // A USD carries character/monster rules plus global options,
                // not a replacement NAV path or UTL rule set.
                if(Section!=Primary&&(Ext!=TEXT("usd")||Section!=TEXT("Monsters")||!Data->HasField(TEXT("monsters"))))continue;
                FString Part=String(FileRefs,*Section);
                if(Part.IsEmpty())Part=String(ImportedUCMProfiles(FPaths::GetPath(FPaths::ConvertRelativePathToFull(String(Profile,TEXT("met_source"))))/File),*Section);
                if(!Part.IsEmpty())FileRefs->SetStringField(Section,Part);
                if(!ACEUCMProfiles::List(UserDirectory(),Section).Contains(Part))
                {
                    Part=Allocate(Section,Stem);auto Resolved=ACEUCMProfiles::Copy(Defaults);
                    for(const auto& Pair:Data->Values)Resolved->Values.Add(Pair.Key,Pair.Value);
                    Resolved->SetStringField(Ext+TEXT("_source"),FPaths::GetPath(FPaths::ConvertRelativePathToFull(String(Profile,TEXT("met_source"))))/File);
                    if(Section==TEXT("Metas")){Routes(Resolved,Part);Resolved->SetObjectField(TEXT("vt_library"),*Library);}
                    Pending.Add({Section,Part,Resolved});FileRefs->SetStringField(Section,Part);
                }
            }
            Refs->SetObjectField(File,FileRefs);
        }
    }
    if(!RoutesValid)return false;
    // Validate the complete import before writing any of its component files.
    for(const auto& Entry:Pending)if(!ValidUCMProfile(Entry.Data)){Notice=TEXT("Invalid meta dependency: ")+Entry.Name;return false;}
    TArray<FString> Written;
    for(auto& Entry:Pending)
    {
        if(Entry.Section==TEXT("Metas"))Entry.Data->SetObjectField(TEXT("meta_profile_refs"),Refs);
        if(!ACEUCMProfiles::SaveSectionAs(UserDirectory(),Entry.Section,Entry.Name,Entry.Data,Notice))
        {for(const auto& File:Written)IFileManager::Get().Delete(*File);return false;}
        Written.Add(UserDirectory()/TEXT("Profiles")/Entry.Section/(Entry.Name+TEXT(".json")));
    }
    for(const auto& Pair:Refs->Values)if(FString(Pair.Key)!=RootFile)
        if(!RememberUCMImport(FPaths::GetPath(FPaths::ConvertRelativePathToFull(String(Profile,TEXT("met_source"))))/FString(Pair.Key),Pair.Value->AsObject()))return false;
    Profile->SetObjectField(TEXT("meta_profile_refs"),Refs);return true;
}
bool UACEPluginSubsystem::SaveLootProfile(const FString& Name)
{
    if (Name == UCMSectionName(TEXT("Loot")))
        return SaveProfile(TEXT("ucm"), Find(TEXT("ucm"))->ProfileName, ProfileJson(TEXT("ucm")));
    return SaveUCMSectionAs(TEXT("Loot"), Name);
}
bool UACEPluginSubsystem::LoadLootProfile(const FString& Name)
{
    if (UCMSectionProfiles(TEXT("Loot")).Contains(Name)) return SelectUCMSection(TEXT("Loot"), Name);
    // Preserve old standalone loot exports, and migrate them on first selection.
    const auto P = Find(TEXT("ucm")); if (!P || !SafeName(Name)) return false;
    auto O = Read(UserDirectory()/TEXT("LootProfiles")/(Name+TEXT(".json")));
    if (!O || !ValidUCMProfile(O) || !O->HasTypedField<EJson::Array>(TEXT("loot_rules")))
    { Notice = TEXT("Loot profile is missing or invalid"); return false; }
    O->RemoveField(TEXT("version")); O->SetStringField(TEXT("loot_profile"), Name);
    if (!ACEUCMProfiles::SaveSectionAs(UserDirectory(), TEXT("Loot"), Name, O, Notice)) return false;
    return SelectUCMSection(TEXT("Loot"), Name);
}

void UACEPluginSubsystem::RecordWaypoint(const FString& Id, const TSharedPtr<FJsonObject>& Details)
{
    auto P = Find(Id); auto* C = GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if (!P || C->GetSessionState() != EACESessionState::InWorld) { Notice = TEXT("Enter the world before recording a waypoint"); return; }
    auto Pos = C->GetPlayerPosition();
    if (auto* PC = Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController())) PC->TryGetLocallyPredictedPosition(Pos);
    const TArray<TSharedPtr<FJsonValue>>* Old = nullptr; TArray<TSharedPtr<FJsonValue>> Points;
    if (P->Profile->TryGetArrayField(TEXT("route"), Old)) Points = *Old;
    if (Points.Num() >= 2048) { Notice = TEXT("Route limit: 2048 waypoints"); return; }
    auto Point=Position(Pos);
    if(Details)for(const auto& Field:Details->Values)Point->Values.Add(Field.Key,Field.Value);
    Points.Add(MakeShared<FJsonValueObject>(Point)); P->Profile->SetArrayField(TEXT("route"), Points);
    SaveCurrentProfile(Id,true);
}
void UACEPluginSubsystem::AddBuff(const FString& Id, int32 SpellId)
{
    auto P = Find(Id); if (!P) return;
    const TArray<TSharedPtr<FJsonValue>>* Old = nullptr; TArray<TSharedPtr<FJsonValue>> Buffs;
    if (P->Profile->TryGetArrayField(TEXT("buffs"), Old)) Buffs = *Old;
    bool Found = false;
    for (int I = Buffs.Num() - 1; I >= 0; --I) if (Buffs[I]->AsNumber() == SpellId) { Buffs.RemoveAt(I); Found = true; }
    if (!Found) Buffs.Add(MakeShared<FJsonValueNumber>(SpellId));
    P->Profile->SetArrayField(TEXT("buffs"), Buffs); SaveProfile(Id, P->ProfileName, Encode(P->Profile));
}
TSharedPtr<FJsonObject> UACEPluginSubsystem::Snapshot()
{
    ACE_PROFILE_SCOPE(PluginSnapshot);
    TRACE_CPUPROFILER_EVENT_SCOPE(ACEPluginSnapshotCommon);
    if(CompanionReadBatch && CompanionReadBatch->Data)
        return MakeShared<FJsonObject>(*CompanionReadBatch->Data);
    auto O = MakeShared<FJsonObject>(); auto* C = GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* D = GetGameInstance()->GetSubsystem<UACEDatSubsystem>(); const auto& V = C->GetPlayerVitalsView();
    O->SetNumberField(TEXT("time"), FPlatformTime::Seconds());
    O->SetNumberField(TEXT("force_buff_request"),ForceBuffRequest);
    O->SetBoolField(TEXT("force_buff_only"),bForceBuffOnly);
    O->SetNumberField(TEXT("health"), V.Health); O->SetNumberField(TEXT("max_health"), V.MaxHealth);
    O->SetNumberField(TEXT("mana"), V.Mana); O->SetNumberField(TEXT("max_mana"), V.MaxMana);
    O->SetNumberField(TEXT("stamina"), V.Stamina); O->SetNumberField(TEXT("max_stamina"), V.MaxStamina);
    O->SetBoolField(TEXT("busy"), C->IsUseBusy() || PendingSpell!=0);
    bool Ready = true;
    for(const auto& P:Plugins)if(P->Running && !P->LocalService && !P->Companion && FPlatformTime::Seconds()<P->NextAction)Ready=false;
    O->SetBoolField(TEXT("ready"), Ready);
    if(auto P=PolicyPlugin())O->SetBoolField(TEXT("loot_approaching"),CanPauseLootApproach(*P));
    FACEPosition Pos = C->GetPlayerPosition();
    auto* PC = Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    if (PC) {PC->TryGetLocallyPredictedPosition(Pos);if(PC->IsWorldTransitionActive())O->SetBoolField(TEXT("ready"),false);
        if(PC->IsUseApproachActive())O->SetBoolField(TEXT("busy"),true);}
    O->SetObjectField(TEXT("position"), Position(Pos));
    if(RouteMovementTeleport==C->GetTeleportSeq())
    {
        TArray<TSharedPtr<FJsonValue>> Samples;
        for(const auto& Sample:RouteMovementSamples)Samples.Add(MakeShared<FJsonValueObject>(Position(Sample)));
        O->SetArrayField(TEXT("route_motion"),Samples);
        O->SetBoolField(TEXT("route_motion_overflow"),bRouteMovementOverflow);
    }
    O->SetObjectField(TEXT("server_position"),Position(C->GetPlayerPosition()));
    if(FollowMovementTeleport==C->GetTeleportSeq())
    {
        TArray<TSharedPtr<FJsonValue>> Samples;
        for(const auto& Sample:FollowMovementSamples)Samples.Add(MakeShared<FJsonValueObject>(Position(Sample)));
        O->SetArrayField(TEXT("follow_motion"),Samples);
        O->SetNumberField(TEXT("follow_motion_target"),uint32(FollowMovementTarget));
        O->SetBoolField(TEXT("follow_motion_overflow"),bFollowMovementOverflow);
    }
    O->SetBoolField(TEXT("moving"), !MovementOwner.IsEmpty() || (!FastCastOwner.IsEmpty() && FastCastStarted) || (!OptionalCastOwner.IsEmpty() && OptionalCastStarted));
    O->SetNumberField(TEXT("heading"),PC&&PC->GetPawn()?PC->GetPawn()->GetActorRotation().Yaw:0);
    const auto Known = C->GetKnownSpells(); TSet<int32> KnownIds(Known);
    auto SpellIds=Known;
    // Profile references may be a higher tier learned by a different character.
    // Include their DAT family metadata, but never treat them as castable.
    for(const auto& P:Plugins)if(P->Running && !P->LocalService && !P->Companion)
    {
        const TArray<TSharedPtr<FJsonValue>>* Buffs=nullptr;
        for(const TCHAR* Key:{TEXT("buffs"),TEXT("excluded_buff_spells")})
            if(P->Profile->TryGetArrayField(Key,Buffs))for(const auto& Buff:*Buffs)SpellIds.AddUnique(int32(Buff->AsNumber()));
        for(const TCHAR* Key:{TEXT("buff_items"),TEXT("item_buff_targets")})
            if(P->Profile->TryGetArrayField(Key,Buffs))for(const auto& Buff:*Buffs)if(Buff->Type==EJson::Object)SpellIds.AddUnique(int32(Number(Buff->AsObject(),TEXT("spell"))));
    }
    SpellIds.Sort();
    const int32 SkillIds[] = {0,34,33,32,31,43}; TArray<int32> SchoolSkills; SchoolSkills.Init(0,UE_ARRAY_COUNT(SkillIds));
    for(const auto& Entry:V.Skills)if(Entry.AdvancementClass>=2)
        for(int32 School=1;School<SchoolSkills.Num();++School)if(Entry.SkillId==SkillIds[School])SchoolSkills[School]=Entry.Current;
    const uint64 Revision=C->GetSession()?C->GetSession()->GetSpellDataRevision():0;
    if(CachedSpells.IsEmpty() || CachedSpellIds!=SpellIds || CachedSchoolSkills!=SchoolSkills || Revision!=CachedSpellRevision)
    {
    CachedSpells.Reset();CachedSpellIds=SpellIds;CachedSchoolSkills=SchoolSkills;CachedSpellRevision=Revision;
    CachedKnownSpellValues.Reset(Known.Num());for(int32 Id:Known)CachedKnownSpellValues.Add(MakeShared<FJsonValueNumber>(Id));
    for (int32 Id : SpellIds)
    {
        uint32 School = 0, Power = 0, Category = 0, Flags = 0; double Duration = 0;
        if (!D->TryGetPluginSpellInfo(Id, School, Power, Category, Flags, Duration)) continue;
        auto S = MakeShared<FJsonObject>(); S->SetNumberField(TEXT("id"), Id);
        S->SetBoolField(TEXT("known"), KnownIds.Contains(Id));
        TMap<FString,int32> ScarabCounts;auto Scarabs=MakeShared<FJsonObject>();
        S->SetBoolField(TEXT("components_known"),D->TryGetPluginSpellScarabs(Id,ScarabCounts));
        for(const auto& Pair:ScarabCounts)Scarabs->SetNumberField(Pair.Key,Pair.Value);
        S->SetObjectField(TEXT("scarabs"),Scarabs);
        uint32 Level=0,LevelSchool=0;D->TryGetSpellSchoolAndLevel(Id,LevelSchool,Level);
        S->SetNumberField(TEXT("level"),Level);
        S->SetNumberField(TEXT("school"), School); S->SetNumberField(TEXT("power"), Power); S->SetNumberField(TEXT("category"), Category);
        FString SpellName;uint32 Icon=0,TargetFlags=0,TargetType=0;bool Projectile=false;
        D->TryGetSpellInfo(Id,SpellName,Icon);D->TryGetRetailSpellTargeting(Id,TargetFlags,TargetType,Projectile);
        S->SetStringField(TEXT("name"),SpellName);S->SetNumberField(TEXT("icon"),Icon);
        S->SetNumberField(TEXT("target_type"),TargetType);S->SetNumberField(TEXT("flags"),Flags);
        S->SetNumberField(TEXT("duration"),Duration);S->SetBoolField(TEXT("projectile"),Projectile);
        S->SetBoolField(TEXT("beneficial"),(Flags&4)!=0);S->SetBoolField(TEXT("caster_target"),(Flags&8)!=0);
        S->SetBoolField(TEXT("self_buff"), (Flags & 8) && (Flags & 4) && Duration > 0);
        S->SetNumberField(TEXT("skill"), SchoolSkills.IsValidIndex(School)?SchoolSkills[School]:0);
        CachedSpells.Add(MakeShared<FJsonValueObject>(S));
    }
    }
    O->SetArrayField(TEXT("spells"), CachedSpells);
    O->SetArrayField(TEXT("known_spells"),CachedKnownSpellValues);
    TArray<TSharedPtr<FJsonValue>> Enchantments,Harmful;auto Cooldowns=MakeShared<FJsonObject>();
    for (const auto& E : C->GetActiveEnchantments(true))
    {
        if(E.bCooldown)Cooldowns->SetNumberField(FString::FromInt(uint32(E.SpellId)&0x7fff),E.Duration<0?1.e9:FMath::Max(0.,E.Duration+E.StartTime-(FPlatformTime::Seconds()-E.ReceivedAt)));
        if(!E.bCooldown&&!E.bBeneficial&&E.Duration>=0)
        {
            uint32 School=0,Power=0,Category=0,Flags=0;double Duration=0;
            const double Remaining=E.Duration+E.StartTime-(FPlatformTime::Seconds()-E.ReceivedAt);
            if(Remaining>0&&D->TryGetPluginSpellInfo(E.SpellId,School,Power,Category,Flags,Duration)&&!(Flags&8))
            {
                auto A=MakeShared<FJsonObject>();A->SetNumberField(TEXT("id"),E.SpellId);A->SetNumberField(TEXT("category"),E.SpellCategory);
                A->SetNumberField(TEXT("power"),Power);A->SetNumberField(TEXT("remaining"),Remaining);Harmful.Add(MakeShared<FJsonValueObject>(A));
            }
        }
        if (!E.bBeneficial || E.bCooldown) continue;
        auto A = MakeShared<FJsonObject>(); A->SetNumberField(TEXT("id"), E.SpellId); A->SetNumberField(TEXT("category"), E.SpellCategory);
        A->SetNumberField(TEXT("power"), E.PowerLevel);
        A->SetNumberField(TEXT("duration"), E.Duration);
        A->SetNumberField(TEXT("remaining"), E.Duration < 0 ? 1.e9 : FMath::Max(0., E.Duration + E.StartTime - (FPlatformTime::Seconds() - E.ReceivedAt)));
        Enchantments.Add(MakeShared<FJsonValueObject>(A));
    }
    O->SetArrayField(TEXT("enchantments"), Enchantments);
    O->SetArrayField(TEXT("harmful_enchantments"),Harmful);
    O->SetObjectField(TEXT("cooldowns"),Cooldowns);
    const auto Selection=C->GetSelectedObject();
    const int32 Dead=Selection.bShowHealth && Selection.HealthFraction<=0 ? Selection.Guid:0;
    const int32 Target = PC ? PC->FindNearbyTarget(true,0,Dead) : 0; FACEWorldObject Obj;
    O->SetNumberField(TEXT("nearest"), uint32(Target));
    if (Target && C->GetWorldObject(Target, Obj)) O->SetNumberField(TEXT("distance"), FVector::Distance(Pos.ToUnrealLocation(), Obj.Position.ToUnrealLocation()) / 100.);
    ExtendSnapshot(O);
    if(CompanionReadBatch)
    {
        CompanionReadBatch->Data=O;
        // Item Tools and PublicSnapshot add fields to their own root object.
        return MakeShared<FJsonObject>(*O);
    }
    return O;
}
void UACEPluginSubsystem::CheckPendingSpellTimeout()
{
    if(!PendingSpell||FPlatformTime::Seconds()-PendingSpellAt<=30)return;
    auto Owner=Find(PendingSpellOwner);
    auto Intent=MakeShared<FJsonObject>();Intent->SetStringField(TEXT("action"),TEXT("cast"));
    Intent->SetStringField(TEXT("activity"),PendingSpellActivity);
    // Release the same bookkeeping as a failed cast, including its serial and
    // fast-cast movement state. Policy must observe that this attempt ended.
    CompletePendingAction(0x001D);
    PendingSpellActivity.Empty();
    if(Owner&&Owner->Running)ReportActivityFailure(*Owner,Intent,TEXT("Cast confirmation timed out; check connection and spell requirements"));
}
void UACEPluginSubsystem::ScheduleDecisionWake(double ReadyAt)
{
    if(DecisionWakeTicker.IsValid()&&DecisionWakeAt<=ReadyAt)return;
    FTSTicker::GetCoreTicker().RemoveTicker(DecisionWakeTicker);
    DecisionWakeAt=ReadyAt;
    DecisionWakeTicker=FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this,&UACEPluginSubsystem::RunDecisionWake),
        float(FMath::Max(0.,ReadyAt-FPlatformTime::Seconds())));
}
bool UACEPluginSubsystem::RunDecisionWake(float DeltaTime)
{
    DecisionWakeTicker.Reset();DecisionWakeAt=0;
    Tick(DeltaTime);
    return false;
}
bool UACEPluginSubsystem::CanPauseLootApproach(const FACEClientPlugin& P) const
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    FACEWorldObject Corpse;
    return P.IsPolicy()&&P.Running&&UseApproachOwner==P.Id&&P.WaitAction==TEXT("open_corpse")
        &&!PendingSpell&&PC&&PC->IsUseApproachActive()&&!PC->IsWorldTransitionActive()
        &&C->GetSession()&&(!C->IsUseBusy()||C->GetSession()->IsPendingWorldUse(P.WaitItem))
        &&C->GetWorldObject(P.WaitItem,Corpse)&&Corpse.IsCorpse()&&Corpse.bHasPosition
        &&!Corpse.ContainerId&&!Corpse.WielderId;
}
bool UACEPluginSubsystem::IsBuffCastIntent(const TSharedPtr<FJsonObject>& I) const
{
    if(String(I,TEXT("action"))!=TEXT("cast"))return false;
    const FString Activity=String(I,TEXT("activity"));
    if(Activity!=TEXT("buffs")&&Activity!=TEXT("buff_others"))return false;
    uint32 School,Power,Category,Flags;double Duration;
    return GetGameInstance()->GetSubsystem<UACEDatSubsystem>()->TryGetPluginSpellInfo(Guid(I,TEXT("spell")),School,Power,Category,Flags,Duration)
        &&(Flags&4)&&Duration>0;
}
bool UACEPluginSubsystem::ResolvePluginSpellTarget(int32 Spell,int32 Selected,int32& Target) const
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(C->ResolveSpellCastTarget(Spell,Selected,Target))return true;
    FACEWorldObject Item;uint32 Flags=0,Type=0;bool Projectile=false;
    auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
    if(Dat->TryGetRetailSpellTargeting(Spell,Flags,Type,Projectile)&&(Flags&4)&&C->GetWorldObject(Selected,Item)
        &&(IsOwnedPluginItem(Item)||(BuffRequests.Num()&&(Item.WielderId==int32(uint32(Number(BuffRequests[0]->AsObject(),TEXT("player"))))
            ||(Item.CurrentWieldedLocation&&Item.ParentGuid==int32(uint32(Number(BuffRequests[0]->AsObject(),TEXT("player"))))))))
        &&(uint32(Item.ItemType)&Type)&&Item.StackSize<=1){Target=Selected;return true;}
    return false;
}
void UACEPluginSubsystem::UpdateBuffDispatch()
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    const double Now=FPlatformTime::Seconds();
    CompleteConfirmedBuff();
    if(ConfirmedBuffAckAt&&Now-ConfirmedBuffAckAt>30)
    {
        auto Owner=Find(ConfirmedBuffOwner);ConfirmedBuffAckAt=0;ConfirmedBuffOwner.Empty();
        // Lua already advanced when it emitted the deferred intent. Dropping
        // it strands completion/profile-load/weapon-switch state in the policy.
        // The buff result was confirmed; only its trailing UseDone is missing.
        // Keep the intent and let Execute revalidate it once the gate releases.
        if(Owner&&Owner->Running)
        {
            Owner->Status=TEXT("Buff acknowledgment timed out; resuming queued action");
            LogUCMEvent(*Owner,Owner->Status,EACEUCMLogLevel::Warning);
            Owner->NextDecision=0;ScheduleDecisionWake(Now);
        }
    }
    if(BuffRetryAt&&Now>=BuffRetryAt)
    {
        auto Owner=Find(PendingSpellOwner);
        if(!PendingSpell||!Owner||!Owner->Running){BuffRetryAt=0;return;}
        if(Now-PendingSpellAt>=5){CompletePendingAction(0x001D);return;}
        if(!C->GetSession()||C->GetSessionState()!=EACESessionState::InWorld||C->GetPlayerGuid()!=Owner->Player||C->GetServerName()!=Owner->Server||C->GetPlayerVitalsView().Health<=0)return;
        auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
        if(C->IsUseBusy()||InventoryInteractionPending()||(PC&&PC->IsWorldTransitionActive()))return;
        int32 Target=0;
        if(!C->GetKnownSpells().Contains(PendingSpell)||!ResolvePluginSpellTarget(PendingSpell,PendingSpellTarget,Target)||Target!=PendingSpellTarget)
        {CompletePendingAction(0x001D);return;}
        // A manual/queued stance change can land between fast-buff retries.
        // Let policy prepare magic again; never cast against optimistic state.
        if(!C->GetSession()->IsCombatModeConfirmed(8)){CompletePendingAction(0x001D);return;}
        BuffRetryAt=0;C->GetSession()->SendCastSpell(PendingSpell,Target);
    }
}
void UACEPluginSubsystem::RestoreWorldRendering()
{
    if(auto* Viewport=SuspendedWorldViewport.Get())Viewport->bDisableWorldRendering=PreviousWorldRenderingDisabled;
    SuspendedWorldViewport.Reset();
}
void UACEPluginSubsystem::UpdateWorldRendering()
{
    auto* GI=GetGameInstance();auto* Viewport=GI->GetGameViewportClient();
    auto* PC=Cast<AACEPlayerController>(GI->GetFirstLocalPlayerController());
    auto* Client=GI->GetSubsystem<UACEClientSubsystem>();
    const auto UCM=PolicyPlugin();bool Requested=false;
    if(UCM&&UCM->Enabled&&UCM->Running)UCM->Profile->TryGetBoolField(TEXT("suspend_world_rendering"),Requested);
    const bool Suspend=Requested&&Viewport&&PC&&!PC->IsVRActive()&&Client&&Client->GetSessionState()==EACESessionState::InWorld;
    if(!Suspend||SuspendedWorldViewport.Get()!=Viewport)RestoreWorldRendering();
    if(Suspend)
    {
        if(!SuspendedWorldViewport.IsValid())
        {SuspendedWorldViewport=Viewport;PreviousWorldRenderingDisabled=Viewport->bDisableWorldRendering;}
        // Skip only the world draw. Slate, world ticks, collision and networking
        // continue at their normal cadence; never pause or throttle the game.
        Viewport->bDisableWorldRendering=true;
    }
}
bool UACEPluginSubsystem::InventoryInteractionPending() const
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    return (PC&&PC->IsInventoryInteractionActive())||(C->GetSession()&&C->GetSession()->IsInventoryTransferPending());
}
void UACEPluginSubsystem::TickIdleEquipmentMana()
{
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    // VT ManaChargesWhenOff is deliberately independent of Start/Stop. Use a
    // separate VM restricted to equipment recharge; no meta/combat/nav can run.
    TSharedPtr<FACEClientPlugin> IdlePolicy;
    for(const auto& Candidate:Plugins)if(Candidate->IsPolicy()&&Candidate->Enabled&&!Candidate->Running&&Candidate->Permissions.Contains(TEXT("inventory")))
    {bool Enabled=false;if(Candidate->Profile->TryGetBoolField(TEXT("mana_charges_when_off"),Enabled)&&Enabled){IdlePolicy=Candidate;break;}}
    if(auto P=IdlePolicy;P&&C->GetSessionState()==EACESessionState::InWorld&&C->GetPlayerVitalsView().Health>0)
    {
        bool Enabled=false;P->Profile->TryGetBoolField(TEXT("mana_charges_when_off"),Enabled);
        const double Now=FPlatformTime::Seconds();
        if(Enabled&&!Plugins.ContainsByPredicate([](const auto& Other){return Other->Running&&!Other->LocalService&&!Other->Companion;})&&Now>=NextIdleManaDecision)
        {
            NextIdleManaDecision=Now+2;
            const uint32 Hash=GetTypeHash(Encode(P->Profile));
            if(IdleManaOwner!=P->Id||IdleManaSession.Pin()!=C->GetSession()||IdleManaPlayer!=C->GetPlayerGuid()||Hash!=IdleManaProfileHash)
            {if(auto Previous=Find(IdleManaOwner))Previous->CombatTransitionIntent.Reset();P->CombatTransitionIntent.Reset();IdleManaOwner=P->Id;IdleManaVM.Reset();IdleManaFailed=false;IdleManaSession=C->GetSession();IdleManaPlayer=C->GetPlayerGuid();IdleManaProfileHash=Hash;}
            if(!IdleManaFailed)
            {
                FString Error,Source;
                if(!IdleManaVM)
                {
                    IdleManaVM=MakeShared<FACEPluginVM>();
                    // Stopped API 2 plugins opt into the stock recharge service,
                    // not arbitrary author callbacks executing after Stop.
                    if(P->Api>=2)IdleManaFailed=!IdleManaVM->Load(TEXT("return function(s,p) return builtinpolicy(s,p) end"),Error,TEXT("ucm"));
                    else if(!FFileHelper::LoadFileToString(Source,*(P->Directory/TEXT("main.lua")))||!IdleManaVM->Load(Source,Error))IdleManaFailed=true;
                }
                if(!IdleManaFailed&&P->CombatTransitionIntent)UpdateCombatTransition(*P);
                else if(!IdleManaFailed&&!InventoryInteractionPending())
                {
                    auto Profile=MakeShared<FJsonObject>(*P->Profile);Profile->RemoveField(TEXT("vt_library"));Profile->SetBoolField(TEXT("ucm_mana_only"),true);
                    // Operational failures use the same bounded activity retry
                    // as running UCM; only a VM error disables this service.
                    if(P->ActivityFailure){Profile->SetObjectField(TEXT("ucm_activity_failure"),P->ActivityFailure);P->ActivityFailure.Reset();}
                    TSharedPtr<FJsonObject> Intent;
                    if(!IdleManaVM->Step(Snapshot(),Profile,Intent,Error))IdleManaFailed=true;
                    else if(Intent)
                    {
                        const FString Action=String(Intent,TEXT("action"));
                        if(Action==TEXT("stop")){IdleManaFailed=true;P->Status=String(Intent,TEXT("status"));}
                        else if(Action==TEXT("use_item")||Action==TEXT("apply_item")||Action==TEXT("activity_failed"))Execute(*P,Intent);
                        else if(!String(Intent,TEXT("status")).IsEmpty())P->Status=String(Intent,TEXT("status"));
                    }
                }
                if(!Error.IsEmpty())P->Status=TEXT("Idle mana recharge stopped: ")+Error;
            }
        }
        else if(!Enabled){P->CombatTransitionIntent.Reset();IdleManaVM.Reset();IdleManaFailed=false;}
    }
    else if(!IdlePolicy){if(auto Previous=Find(IdleManaOwner))Previous->CombatTransitionIntent.Reset();IdleManaOwner.Empty();IdleManaVM.Reset();IdleManaFailed=false;}
}
bool UACEPluginSubsystem::Tick(float)
{
    ACE_PROFILE_SCOPE(Plugins);
    TickLocalServices();
    if(FPlatformTime::Seconds()>=NextUCMLogSave){NextUCMLogSave=FPlatformTime::Seconds()+10;FlushUCMLog();}
    UpdateWorldRendering();
    // VT cLogic.SchedulePoke wakes on cast completion instead of waiting for
    // the idle poll. Keep expensive presentation/background scans at their
    // original cadence when several actions finish between periodic ticks.
    if(FPlatformTime::Seconds()>=NextMaintenanceAt)
    {
        NextMaintenanceAt=FPlatformTime::Seconds()+.5;
        UpdateLedgers();UpdateDesktopDock();RefreshAutomationData();DrawRoute();
        TickLootNotifications();
    }
    auto* C = GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    UpdateItemToolsCharacter();
    TickAutoCraft();
    UpdateBuffDispatch();
    CheckPendingSpellTimeout();
    FCompanionReadBatch Batch;
    TGuardValue<FCompanionReadBatch*> ReadGuard(CompanionReadBatch,&Batch);
    TickCompanions();
    TickIdleEquipmentMana();
    for (const auto& P : Plugins) if (P->Running && !P->LocalService && !P->Companion && P->Id!=TEXT("autocraft"))
    {
        if (C->GetSessionState() != EACESessionState::InWorld || C->GetPlayerGuid() != P->Player || C->GetServerName() != P->Server || (C->GetPlayerVitalsView().Health <= 0 && !P->Profile->HasField(TEXT("vt_meta"))))
        { Stop(P->Id, TEXT("Stopped: character died, changed, or left world")); continue; }
        if(P->IsPolicy())if(auto Items=Find(TEXT("itemtools"));Items&&Items->Running)
        {P->Status=TEXT("Paused while Item Tools runs");continue;}
        if(P->IsPolicy())if(auto Craft=Find(TEXT("autocraft"));Craft&&Craft->Running)
        {P->Status=TEXT("Paused while Auto Craft runs");continue;}
        if(InventoryInteractionPending())
        {
            if(MovementOwner==P->Id){MovementOwner.Empty();MoveExpires=0;}
            P->Status=TEXT("Waiting for inventory interaction");ScheduleDecisionWake(FPlatformTime::Seconds()+.05);continue;
        }
        if(P->CombatTransitionIntent){UpdateCombatTransition(*P);continue;}
        if(DeferredBuffIntent&&DeferredBuffOwner==P->Id)
        {
            if(ConfirmedBuffAckAt||PendingSpell||C->IsUseBusy())continue;
            auto I=DeferredBuffIntent;DeferredBuffIntent.Reset();DeferredBuffOwner.Empty();
            P->NextAction=0;Execute(*P,I);P->NextDecision=FPlatformTime::Seconds()+.25;continue;
        }
        // UCM cannot issue a second action while the first is pending. Avoid
        // copying a large spellbook/inventory into Lua just to return "waiting".
        RefreshActionWait(*P);
        if(FPlatformTime::Seconds()<P->NextDecision)
        {
            // An event wake can arrive before a short decision/action gate.
            // Preserve it at that deadline instead of losing it to the .5s poll.
            ScheduleDecisionWake(FMath::Max(P->NextDecision,P->NextAction));
            continue;
        }
        auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
        if(PC&&!PC->IsUseApproachActive())UseApproachOwner.Empty();
        if(P->IsPolicy() && !(UCMCraft&&UCMCraft->Selected!=INDEX_NONE) && !P->Profile->HasField(TEXT("vt_meta")) && !CanPauseLootApproach(*P) && (C->IsUseBusy() || PendingSpell || FPlatformTime::Seconds()<P->NextAction || (PC&&PC->IsUseApproachActive())))
        {
            if(FPlatformTime::Seconds()<P->NextAction)ScheduleDecisionWake(P->NextAction);
            continue;
        }
        TSharedPtr<FJsonObject> Intent; FString Error;
        auto TickProfile=MakeShared<FJsonObject>(*P->Profile);TickProfile->RemoveField(TEXT("vt_library"));
        if(P->IsPolicy()){TickProfile->SetArrayField(TEXT("ucm_commands"),UCMCommands);TickProfile->SetBoolField(TEXT("ucm_command_only"),bUCMCommandOnly);UCMCommands.Reset();}
        TickProfile->SetBoolField(TEXT("ucm_resume"),P->ResumeMetaPending);P->ResumeMetaPending=false;
        if(P->ActivityFailure){TickProfile->SetObjectField(TEXT("ucm_activity_failure"),P->ActivityFailure);if(P->Api<2)P->ActivityFailure.Reset();}
        if(P->Id==TEXT("itemtools"))
        {
            // Only the active rule set crosses into Lua. Chest mule sets are also
            // required, but giving/restocking must not marshal unrelated profiles.
            const FString Mode=String(TickProfile,TEXT("mode"));
            for(const TCHAR* Key:{TEXT("profile_rules"),TEXT("chest_rules"),TEXT("mule1_rules"),TEXT("mule2_rules")})
                if((Mode==TEXT("profile")&&FString(Key)!=TEXT("profile_rules"))||(Mode!=TEXT("chest")&&Mode!=TEXT("profile")))TickProfile->RemoveField(Key);
            if(Mode==TEXT("chest"))TickProfile->RemoveField(TEXT("profile_rules"));
        }
        auto PolicySnapshot=P->Api>=2?PublicSnapshot(*P):(P->Id==TEXT("itemtools")?ItemToolsSnapshot():Snapshot());
        if(P->Id==TEXT("ucm"))
        {
            // Manager tells belong only to UCM, not to unrelated data snapshots.
            PolicySnapshot=MakeShared<FJsonObject>(*PolicySnapshot);
            PolicySnapshot->SetArrayField(TEXT("fellowship_tells"),FellowshipTells);
            ExtendUCMCraftSnapshot(PolicySnapshot);
            ExtendUCMCombatSnapshot(PolicySnapshot);
        }
        if (!P->VM->Step(PolicySnapshot, TickProfile, Intent, Error)) { Stop(P->Id, TEXT("Script error: ") + Error); continue; }
        if(P->IsPolicy()){RouteMovementSamples.Reset();bRouteMovementOverflow=false;FollowMovementSamples.Reset();bFollowMovementOverflow=false;}
        if (Intent)
        {
            if(P->Api>=2 && !ApplyPublicOutput(*P,Intent,Error)){Stop(P->Id,TEXT("API error: ")+Error);continue;}
            if(P->Running)Execute(*P, Intent);
        }
        if(P->Api>=2 && FPlatformTime::Seconds()>=P->NextStateSave){FlushPluginState(*P);P->NextStateSave=FPlatformTime::Seconds()+5;}
        // Idle/blocked policies need not rebuild world sight and inventory every frame.
        bool ContinueWork=false;if(Intent)Intent->TryGetBoolField(TEXT("continue_work"),ContinueWork);
        P->NextDecision=FPlatformTime::Seconds()+(ContinueWork||CombatTarget?.05:.25);
        if(ContinueWork||CombatTarget||!PhysicalAttackOwner.IsEmpty())ScheduleDecisionWake(FMath::Max(P->NextDecision,P->NextAction));
    }
    return true;
}
void UACEPluginSubsystem::ReportActivityFailure(FACEClientPlugin& P, const TSharedPtr<FJsonObject>& I, const FString& Reason, bool NotifyPolicy)
{
    if(!P.IsPolicy()){Stop(P.Id,Reason);return;}
    FString Activity=String(I,TEXT("activity"));
    if(Activity.IsEmpty())
    {
        // Legacy intents have no activity tag. New policy intents always carry
        // one, since equip/cast/identify may belong to several activities.
        const FString Action=String(I,TEXT("action"));
        if(Action==TEXT("buy")||Action==TEXT("sell_note")||Action==TEXT("split_note"))Activity=TEXT("vendors");
        else if(Action==TEXT("combine_salvage"))Activity=TEXT("combine");
        else if(Action==TEXT("loot")||Action==TEXT("salvage")||Action==TEXT("read")||Action==TEXT("sell")||Action==TEXT("open_corpse")||Action==TEXT("close_corpse"))Activity=TEXT("loot");
        else if(Action==TEXT("store_item")||Action==TEXT("merge"))Activity=TEXT("inventory");
        else if(Action==TEXT("attack"))Activity=TEXT("combat");
        else if(Action==TEXT("move")||Action==TEXT("jump")||I->HasField(TEXT("runtime_route")))Activity=TEXT("navigation");
        else Activity=TEXT("meta");
    }
    P.Status=NotifyPolicy?Activity+TEXT(" failed: ")+Reason+TEXT(". UCM remains active"):Reason;
    LogUCMEvent(P,P.Status,EACEUCMLogLevel::Warning,I);
    Notice=P.Status;
    if(NotifyPolicy)
    {
        P.ActivityFailure=MakeShared<FJsonObject>();
        P.ActivityFailure->SetStringField(TEXT("activity"),Activity);
        P.ActivityFailure->SetStringField(TEXT("status"),Reason);
    }
    if(MovementOwner==P.Id){MovementOwner.Empty();MoveExpires=0;FaceHeading.Reset();}
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
    {if(UseApproachOwner==P.Id)PC->EndUseApproach();if(Activity==TEXT("navigation"))PC->CancelPluginJump();}
    if(UseApproachOwner==P.Id)UseApproachOwner.Empty();
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    if(C&&C->GetSession())
    {
        if(Activity==TEXT("combat")){C->GetSession()->SendCancelAttack();PhysicalAttackOwner.Empty();PhysicalAttackTarget=0;}
        if(Activity==TEXT("loot"))
        {
            const int32 Container=C->GetOpenExternalContainerGuid();FACEWorldObject Object;
            if(Container&&C->GetWorldObject(Container,Object)&&Object.IsCorpse())
                C->SendNoLongerViewingContents(Container);
        }
    }
    P.NextAction=0;P.NextDecision=0;
}
void UACEPluginSubsystem::Execute(FACEClientPlugin& P, const TSharedPtr<FJsonObject>& I)
{
    if(CompanionReadBatch)CompanionReadBatch->Reset();
    if(P.Id==TEXT("ucm"))I->TryGetStringField(TEXT("fellowship_status"),FellowshipManagerStatus);
    if(P.Id==TEXT("ucm"))
    {
        const TSharedPtr<FJsonObject>* Services=nullptr;
        if(I->TryGetObjectField(TEXT("services_profile"),Services))
        {
            UCMServicesProfile=*Services;bool Enabled=false;
            UCMServicesProfile->TryGetBoolField(TEXT("auto_fellowship"),Enabled);if(!Enabled){FellowshipTells.Reset();FellowshipManagerStatus.Empty();}
            Enabled=false;UCMServicesProfile->TryGetBoolField(TEXT("auto_craft"),Enabled);if(!Enabled)UCMCraft.Reset();
        }
    }
    if(InventoryInteractionPending())return;
    // Only another validated buff may overlap the previous buff's recoil.
    // Hold other policy intents without stepping Lua again until its untagged
    // UseDone is drained, including force-buff completion and equipment changes.
    if(ConfirmedBuffAckAt && !IsBuffCastIntent(I))
    {DeferredBuffIntent=MakeShared<FJsonObject>(*I);DeferredBuffOwner=P.Id;return;}
    if(P.IsPolicy())
    {
        I->TryGetBoolField(TEXT("route_join_pending"),bRouteJoinRequested);
        bTrackRouteMovement=false;I->TryGetBoolField(TEXT("route_track_movement"),bTrackRouteMovement);
    }
    if(P.IsPolicy() && I->HasField(TEXT("route_point")))RoutePoint=FMath::Clamp(int32(Number(I,TEXT("route_point"),1)),1,2049);
    if(P.IsPolicy()&&I->HasField(TEXT("runtime_route")))
    {
        const TArray<TSharedPtr<FJsonValue>>* Route=nullptr;
        if(!I->TryGetArrayField(TEXT("runtime_route"),Route)||Route->Num()>2048){ReportActivityFailure(P,I,TEXT("Invalid runtime route"));return;}
        for(const auto& V:*Route)if(V->Type!=EJson::Object){ReportActivityFailure(P,I,TEXT("Invalid runtime waypoint"));return;}
        const FString RouteProfile=String(I,TEXT("runtime_route_profile"));
        if(P.Id==TEXT("ucm")&&!RouteProfile.IsEmpty()&&RouteProfile!=UCMSectionName(TEXT("Routes")))
        {
            auto Updated=P.Profile,Links=P.ProfileLinks;
            if(!ACEUCMProfiles::Select(UserDirectory(),TEXT("Routes"),RouteProfile,Updated,Links,Notice)
                ||!ACEUCMProfiles::SaveSelection(UserDirectory(),Updated,Links,Notice))
            {ReportActivityFailure(P,I,Notice);return;}
            P.Profile=Updated;P.ProfileLinks=Links;P.ProfileBaseline=ACEUCMProfiles::Copy(Updated);
        }
        RuntimeRoute=MakeShared<FJsonObject>();RuntimeRoute->SetArrayField(TEXT("route"),*Route);RouteRebuiltAt=0;MovementOwner.Empty();MoveExpires=0;
        bool Loop=false,Reverse=false;I->TryGetBoolField(TEXT("runtime_loop_route"),Loop);I->TryGetBoolField(TEXT("runtime_reverse_route"),Reverse);
        RuntimeRoute->SetBoolField(TEXT("loop_route"),Loop);RuntimeRoute->SetBoolField(TEXT("reverse_route"),Reverse);
    }
    const FString MetaState=String(I,TEXT("meta_state"));
    if(P.Id==TEXT("ucm") && P.Permissions.Contains(TEXT("loot")))
    {
        const double Completed=Number(I,TEXT("completed_corpse"));
        if(Completed>0 && Completed<=MAX_uint32 && FMath::FloorToDouble(Completed)==Completed)
            if(auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>())
            {
                FACEWorldObject Corpse;
                if(Client->GetWorldObject(int32(uint32(Completed)),Corpse)&&Corpse.IsCorpse())
                    LootedCorpses.Add(Corpse.Guid,{Corpse.Position,FPlatformTime::Seconds()});
            }
    }
    LogUCMIntent(P,I);
    bool NotifyPickup=true;
    P.Profile->TryGetBoolField(TEXT("loot_notify_pickup"),NotifyPickup);
    if(P.IsPolicy()&&P.Permissions.Contains(TEXT("loot"))&&NotifyPickup)
    {
        const FString LootNotice=String(I,TEXT("loot_notification"));
        if(!LootNotice.IsEmpty())ShowLootNotification(LootNotice);
    }
    if(!MetaState.IsEmpty())P.MetaState=MetaState;
    if(I->HasTypedField<EJson::String>(TEXT("meta_status")))P.MetaStatus=String(I,TEXT("meta_status")).Left(1600);
    const FString Status = String(I, TEXT("status")); if (!Status.IsEmpty()) P.Status = Status;
    if(I->HasTypedField<EJson::String>(TEXT("targeting_status")))P.TargetingStatus=String(I,TEXT("targeting_status"));
    const FString Action = String(I, TEXT("action"));
    if(P.Api>=2 && (Action==TEXT("waypoint")||Action==TEXT("command")||Action==TEXT("select_item")))return;
    if(P.Id==TEXT("itemtools"))ItemToolsProgress=MakeShared<FJsonObject>(*I);
    if(Action.StartsWith(TEXT("it_"))){ExecuteItemTools(P,I);return;}
    if(P.IsPolicy()&&Action==TEXT("activity_failed")){ReportActivityFailure(P,I,Status,false);return;}
    if(P.IsPolicy())if(auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>()){bool Updates=false;I->TryGetBoolField(TEXT("helper_updates"),Updates);Client->SetPluginFellowshipUpdates(Updates);}
    if(P.IsPolicy()&&Action==TEXT("force_buff_done"))
    {
        if(ForceBuffRequest&&Number(I,TEXT("request"))==ForceBuffRequest)
        {
            ForceBuffRequest=0;
            if(bForceBuffOnly)Stop(P.Id,Status.IsEmpty()?TEXT("Force Buff complete"):Status,true);
            bForceBuffOnly=false;
        }
        return;
    }
    if(P.IsPolicy()&&Action==TEXT("notice")){Notice=String(I,TEXT("text")).Left(1024);return;}
    if(P.IsPolicy()&&Action==TEXT("pause_loot_approach"))
    {
        if(P.Permissions.Contains(TEXT("loot"))&&CanPauseLootApproach(P))
        {
            // Same local cancellation as manual movement, followed by a normal
            // movement stop. Never release pending pickup/equipment/cast gates.
            auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
            Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController())->EndUseApproach();
            Client->GetSession()->CancelWorldUseApproach();Client->StopMovement();
            UseApproachOwner.Empty();P.WaitAction.Empty();P.NextAction=0;P.NextDecision=0;
        }
        return;
    }
    if(P.IsPolicy()&&Action==TEXT("pause_navigation"))
    {
        LogUCMEvent(P,Status,EACEUCMLogLevel::Warning,I);
        // A blocked route must not discard the VM or stop combat/recovery.
        // Reflect the safety pause in the same switch used by UCM and Micro.
        P.Profile->SetBoolField(TEXT("navigation"),false);
        if(MovementOwner==P.Id){MovementOwner.Empty();MoveExpires=0;FaceHeading.Reset();}
        if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
        {PC->CancelPluginJump();if(UseApproachOwner==P.Id)PC->EndUseApproach();}
        if(UseApproachOwner==P.Id)UseApproachOwner.Empty();
        bRouteJoinRequested=true;RouteVisibilityOffset=0;P.NextAction=0;P.NextDecision=0;
        Notice=Status;
        return;
    }
    if(P.IsPolicy() && Action==TEXT("buff_request_start"))
    {
        if(BuffRequests.Num()&&uint32(Number(I,TEXT("request")))==uint32(Number(BuffRequests[0]->AsObject(),TEXT("id"))))
        {
            auto Request=BuffRequests[0]->AsObject();bool Started=false;Request->TryGetBoolField(TEXT("started"),Started);
            if(!Started){Request->SetBoolField(TEXT("started"),true);ReplyBuffRequest(Guid(Request,TEXT("player")),String(Request,TEXT("name")),TEXT("UCM: it is your turn for buffs. Please stay nearby with your equipment on."));}
        }
        return;
    }
    if(P.IsPolicy() && Action==TEXT("buff_request_done"))
    {
        const uint32 Request=uint32(Number(I,TEXT("request")));
        if(BuffRequests.Num() && uint32(Number(BuffRequests[0]->AsObject(),TEXT("id")))==Request)
        {const auto Completed=BuffRequests[0]->AsObject();ReplyBuffRequest(Guid(Completed,TEXT("player")),String(Completed,TEXT("name")),TEXT("UCM: ")+Status);BuffRequests.RemoveAt(0);BuffRequestStatus=Status;Notice=Status;NotifyBuffQueuePositions();}
        return;
    }
    if (Action.IsEmpty()) return;
    if(Action==TEXT("plugin_command")&&P.IsPolicy())
    {
        if(!P.Permissions.Contains(TEXT("say"))&&!P.Permissions.Contains(TEXT("commands"))){ReportActivityFailure(P,I,TEXT("Local meta commands require the say capability"));return;}
        if(!RunPluginCommand(String(I,TEXT("text"))))ReportActivityFailure(P,I,Notice);
        return;
    }
    if (Action == TEXT("stop")) { bool Preserve=false;I->TryGetBoolField(TEXT("preserve_meta"),Preserve);Stop(P.Id, Status.IsEmpty() ? TEXT("Stopped by plugin") : Status,Preserve);return; }
    if(Action==TEXT("profile_load")&&P.IsPolicy())
    {
        const FString Name=String(I,TEXT("profile")),Kind=String(I,TEXT("kind"));const TSharedPtr<FJsonObject>* Library=nullptr;
        if(!TArray<FString>{TEXT("met"),TEXT("nav"),TEXT("utl"),TEXT("usd")}.Contains(Kind)||!Name.EndsWith(TEXT(".")+Kind)){ReportActivityFailure(P,I,TEXT("Invalid profile load type"));return;}
        if(!P.Profile->TryGetObjectField(TEXT("vt_library"),Library)||!(*Library)->HasTypedField<EJson::Object>(Name))
        {ReportActivityFailure(P,I,TEXT("Referenced profile was not validated during import: ")+Name);return;}
        const auto Loaded=(*Library)->GetObjectField(Name);TSharedPtr<FJsonObject> Updated=MakeShared<FJsonObject>(*P.Profile);
        auto Links=P.ProfileLinks;
        const TSharedPtr<FJsonObject>* Refs=nullptr;const TSharedPtr<FJsonObject>* FileRefs=nullptr;
        const bool Shared=P.Id==TEXT("ucm")&&P.Profile->TryGetObjectField(TEXT("meta_profile_refs"),Refs)&&(*Refs)->TryGetObjectField(Name,FileRefs);
        if(Shared)
        {
            for(const auto& Section:ACEUCMProfiles::Sections())
            {
                if(Kind==TEXT("usd")&&Section!=TEXT("Character")&&Section!=TEXT("Monsters"))continue;
                FString Part;if(!(*FileRefs)->TryGetStringField(Section,Part))continue;
                if(!ACEUCMProfiles::Select(UserDirectory(),Section,Part,Updated,Links,Notice))
                {ReportActivityFailure(P,I,Notice);return;}
            }
        }
        const auto BeforeLoad=ACEUCMProfiles::Copy(Updated);
        if(Shared&&Kind==TEXT("met")&&!MaterializeUCMMetaProfiles(Updated,String(Links,TEXT("Metas"))))
        {ReportActivityFailure(P,I,Notice);return;}
        if(!Shared&&Kind==TEXT("usd")){const TArray<TSharedPtr<FJsonValue>>* Keys=nullptr;if(Updated->TryGetArrayField(TEXT("usd_keys"),Keys)){const auto Copy=*Keys;for(const auto& Key:Copy)Updated->RemoveField(Key->AsString());}}
        if(!Shared&&Kind==TEXT("utl")){Updated->RemoveField(TEXT("salvage_policy"));Updated->RemoveField(TEXT("loot_profile"));Updated->RemoveField(TEXT("loot_modified"));}
        // Loading a dependency replaces its route, not the meta's current
        // navigation switch. Converted files are deliberately inactive.
        for(const auto& Pair:Loaded->Values)
        {
            const FString Section=ACEUCMProfiles::SectionFor(FString(Pair.Key));
            const bool GlobalOption=Kind==TEXT("usd")&&Section!=TEXT("Character")&&Section!=TEXT("Monsters")&&Section!=TEXT("Metas");
            if((!Shared||Section==TEXT("ucm")||GlobalOption)&&(Kind!=TEXT("nav")||Pair.Key!=TEXT("navigation")))Updated->Values.Add(Pair.Key,Pair.Value);
        }
        if(!Shared)Updated->SetStringField(Kind+TEXT("_source"),Name);
        Updated->SetNumberField(TEXT("vt_revision"),Number(P.Profile,TEXT("vt_revision"))+1);Updated->SetStringField(TEXT("vt_loaded_kind"),Kind);
        if(Kind==TEXT("usd")){auto* Client=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();if(Client&&Client->GetSession())Client->GetSession()->SendCancelAttack();}
        if(Shared&&!ACEUCMProfiles::Save(UserDirectory(),ACEUCMProfiles::ActiveName(),Updated,Links,BeforeLoad,Notice,ValidUCMProfile)){ReportActivityFailure(P,I,Notice);return;}
        P.Profile=Updated;
        if(Shared){P.ProfileLinks=Links;P.ProfileBaseline=ACEUCMProfiles::Copy(Updated);}
        // VT character/loot loads leave its current embedded route intact.
        if(Kind==TEXT("nav")||Kind==TEXT("met")){RuntimeRoute.Reset();RouteRebuiltAt=0;bRouteJoinRequested=true;MovementOwner.Empty();MoveExpires=0;}
        return;
    }
    const FString Permission = Action==TEXT("cast_item") ? TEXT("cast") : ((Action == TEXT("move") || Action == TEXT("face") || Action == TEXT("jump") || Action == TEXT("recall") || Action == TEXT("use_world") || Action == TEXT("close_vendor") || Action == TEXT("select") || Action == TEXT("logout")) ? TEXT("navigation") : (Action == TEXT("attack") || Action == TEXT("cancel_attack") || Action == TEXT("combat_mode")) ? TEXT("combat") : (Action == TEXT("store_item") || Action == TEXT("give") || Action == TEXT("apply_item") || Action == TEXT("equip") || Action == TEXT("use_item") || Action == TEXT("identify") || Action == TEXT("merge")) ? TEXT("inventory") : (Action == TEXT("buy") || Action == TEXT("split_note") || Action == TEXT("sell_note") || Action == TEXT("combine_salvage") || Action == TEXT("salvage") || Action == TEXT("sell") || Action == TEXT("read") || Action == TEXT("loot") || Action == TEXT("open_corpse") || Action == TEXT("close_corpse")) ? TEXT("loot") : Action);
    if (!P.Permissions.Contains(Action==TEXT("attack_bar")?TEXT("combat"):Action==TEXT("craft")?TEXT("inventory"):Permission)) { ReportActivityFailure(P,I, TEXT("Action not permitted: ") + Action); return; }
    auto* C = GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
    auto* PC = Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController());
    if (!PC || !C->GetSession() || (C->IsUseBusy() && Action!=TEXT("confirm")&&Action!=TEXT("craft")) || PC->IsWorldTransitionActive()) return;
    const double Now = FPlatformTime::Seconds();
    if (Now < P.NextAction || P.CombatTransitionIntent) return;
    // Steering does not send a use/cast action. Reserving the action throttle
    // here blocked the arrival wake and stopped at every short return segment.
    P.NextAction = (Action==TEXT("move") || Action==TEXT("face")) ? Now : Now + .5;
    P.WaitAction.Empty();
    if(P.IsPolicy()&&Action==TEXT("identify")&&String(I,TEXT("activity"))==TEXT("combat"))
    {
        FACEWorldObject Target;
        if(C->GetWorldObject(Guid(I,TEXT("item")),Target)&&Target.IsAttackable()&&!Target.bIsPlayer&&!Target.PetOwnerId)CombatTarget=Target.Guid;
    }
    if(Action!=TEXT("move")&&Action!=TEXT("face")){MovementOwner.Empty();MoveExpires=0;FaceHeading.Reset();}
    if(Action==TEXT("confirm"))
    {
        bool Accept=false;const double Type=Number(I,TEXT("type"),-1),Context=Number(I,TEXT("context"),-1);
        if(Type<0||Type>MAX_uint32||Context<0||Context>MAX_uint32||FMath::FloorToDouble(Type)!=Type||FMath::FloorToDouble(Context)!=Context||!I->TryGetBoolField(TEXT("accept"),Accept)||!C->GetSession()->RespondToConfirmation(uint32(Type),uint32(Context),Accept))ReportActivityFailure(P,I,TEXT("Server confirmation changed or disappeared"));
        return;
    }
    if (ExecuteInventory(P,I,Action)) return;
    if(Action==TEXT("craft")){ExecuteUCMCraft(P,I);return;}
    if(Action==TEXT("face")){MovementOwner=P.Id;FaceHeading=float(Number(I,TEXT("heading")));MoveExpires=Now+1.5;return;}
    if(Action==TEXT("fellowship"))
    {
        const FString Op=String(I,TEXT("operation")),Name=String(I,TEXT("name"));
        const auto F=C->GetFellowship();const int32 Player=C->GetPlayerGuid(),TargetId=Guid(I,TEXT("target"));
        const bool Leader=F.bValid&&F.LeaderGuid==Player;
        if(Op==TEXT("create"))
        {
            if(F.bValid){if(!F.Name.Equals(Name,ESearchCase::IgnoreCase))ReportActivityFailure(P,I,TEXT("Already in another fellowship"));return;}
            if(Name.TrimStartAndEnd().IsEmpty()||Name.Len()>80){ReportActivityFailure(P,I,TEXT("Invalid fellowship name"));return;}
            C->SendFellowshipCreate(Name);return;
        }
        if(!F.bValid)
        {if(Op!=TEXT("quit")&&Op!=TEXT("disband"))ReportActivityFailure(P,I,TEXT("Not in a fellowship"));return;}
        if((Op==TEXT("disband")||Op==TEXT("open")||Op==TEXT("close")||Op==TEXT("dismiss")||Op==TEXT("leader")
            ||(Op==TEXT("recruit")&&!F.bOpen))&&!Leader)
        {ReportActivityFailure(P,I,TEXT("Fellowship leader permission is required"));return;}
        if(Op==TEXT("quit")||Op==TEXT("disband"))C->SendFellowshipQuit(Op==TEXT("disband"));
        else if(Op==TEXT("open")||Op==TEXT("close"))
        {
            if(F.bOpen==(Op==TEXT("open")))return;
            if(F.bLocked)ReportActivityFailure(P,I,TEXT("Fellowship is locked"));else C->SendFellowshipChangeOpenness(Op==TEXT("open"));
        }
        else if(Op==TEXT("dismiss")||Op==TEXT("leader"))
        {
            if(TargetId==Player||!F.Members.ContainsByPredicate([&](const auto& M){return M.Guid==TargetId;}))
                ReportActivityFailure(P,I,TEXT("Choose another current fellowship member"));
            else if(Op==TEXT("dismiss"))C->SendFellowshipDismiss(TargetId);
            else C->SendFellowshipAssignNewLeader(TargetId);
        }
        else if(Op==TEXT("recruit"))
        {
            if(F.Members.ContainsByPredicate([&](const auto& M){return M.Guid==TargetId;}))return;
            FACEWorldObject Target;if(TargetId!=Player&&C->GetWorldObject(TargetId,Target)&&Target.bIsPlayer)C->SendFellowshipRecruit(TargetId);
            else ReportActivityFailure(P,I,TEXT("Recruit target is unavailable"));
        }
        else ReportActivityFailure(P,I,TEXT("Invalid fellowship action"));return;
    }
    if(Action==TEXT("select"))
    {
        FACEWorldObject Target;const int32 Id=Guid(I,TEXT("target"));
        if(C->GetWorldObject(Id,Target)&&(Target.IsSelectableWorldObject()||IsOwnedPluginItem(Target)))C->SelectObject(Id);
        else ReportActivityFailure(P,I,TEXT("Meta selection target disappeared"));return;
    }
    if(Action==TEXT("cancel_attack")){PhysicalAttackUntil=Now+2;PhysicalAttackNextSendAt=0;CombatTarget=0;C->GetSession()->SendCancelAttack();P.NextAction=Now;return;}
    if(Action==TEXT("combat_mode")){const int Mode=int(Number(I,TEXT("mode")));if(Mode==1||Mode==2||Mode==4||Mode==8)if(PrepareCombatAction(P,I,Mode))P.NextAction=Now;return;}
    if(Action==TEXT("attack_bar")){if(PC)PC->SetPluginAttackPower(Number(I,TEXT("power")));return;}
    if(Action==TEXT("logout")){Stop(P.Id,TEXT("Logged out by meta"));C->Logout();return;}
    if(Action==TEXT("recall"))
    {
        const FString Destination=String(I,TEXT("destination"));
        if(Destination==TEXT("marketplace"))C->SendTeleToMarketplace();else if(Destination==TEXT("lifestone"))C->SendTeleToLifestone();else if(Destination==TEXT("allegiance"))C->SendRecallAllegianceHometown();else if(Destination==TEXT("house"))C->SendTeleToHouse();else if(Destination==TEXT("mansion"))C->SendTeleToMansion();else ReportActivityFailure(P,I,TEXT("Unknown recall destination"));return;
    }
    if(Action==TEXT("say"))
    {
        const FString Text=String(I,TEXT("text"));
        const FString Recipient=String(I,TEXT("recipient"));
        if(!Text.IsEmpty()&&Text.Len()<=500&&!Text.Contains(TEXT("\n"))&&!Text.Contains(TEXT("\r")))
        {if(!Recipient.IsEmpty()){if(Recipient.Len()<=128&&!Recipient.Contains(TEXT("\n"))&&!Recipient.Contains(TEXT("\r")))C->SendTell(Recipient,Text);}
         else if(Number(I,TEXT("channel"))==2048)C->SendChatChannel(2048,Text);else C->GetSession()->SendTalk(Text);}
        return;
    }
    if (Action == TEXT("move"))
    {
        // Pursuit is already a combat acquisition, before reaching firing range.
        // Keep the HUD/health subscription and target-death wake tied to that mob.
        const int32 PursuitTarget=Guid(I,TEXT("target"));
        if(PursuitTarget)
        {
            FACEWorldObject Monster;
            if(!P.Permissions.Contains(TEXT("combat"))||!C->GetWorldObject(PursuitTarget,Monster)
                ||!Monster.IsSelectableWorldObject()||!Monster.IsAttackable()||Monster.bIsPlayer||Monster.PetOwnerId
                ||DefeatedTargets.Contains(PursuitTarget)||!HasClearCombatSight(Monster))
            {MovementOwner.Empty();MoveExpires=0;P.NextDecision=0;return;}
            if(C->GetSelectedObject().Guid!=PursuitTarget)C->SelectObject(PursuitTarget);
            if(P.IsPolicy())CombatTarget=PursuitTarget;
        }
        FACEPosition Dest; Dest.CellId = Guid(I,TEXT("cell"));
        Dest.Location = FVector(Number(I, TEXT("x")), Number(I, TEXT("y")), Number(I, TEXT("z")));
        FACEPosition Pos = C->GetPlayerPosition(); PC->TryGetLocallyPredictedPosition(Pos);
        bool CoordinateRoute=false;
        if(I->TryGetBoolField(TEXT("coordinate_route"),CoordinateRoute)&&CoordinateRoute&&(uint32(Pos.CellId)&0xffff)>=0x100)
        {
            const uint32 From=uint32(Dest.CellId)>>16,To=uint32(Pos.CellId)>>16;
            Dest.Location.X+=(int32(From>>8)-int32(To>>8))*192;
            Dest.Location.Y+=(int32(From&255)-int32(To&255))*192;
            Dest.CellId=Pos.CellId;
        }
        if (!Dest.IsValid() || (((uint32(Dest.CellId)>>16)!=(uint32(Pos.CellId)>>16)) && ((uint32(Dest.CellId)&0xffff)>=0x100 || (uint32(Pos.CellId)&0xffff)>=0x100)) || FVector::Distance(Dest.ToUnrealLocation(), Pos.ToUnrealLocation()) > 40000.)
        { ReportActivityFailure(P,I, TEXT("Route needs a portal between these cells, or a closer waypoint (maximum 400m)")); return; }
        if (MovementOwner != P.Id || FVector::Distance(Dest.ToUnrealLocation(), MoveTarget.ToUnrealLocation()) > .1)
        { LastMovePosition = Pos.ToUnrealLocation(); LastProgress = Now; bMoveArrived = false; }
        // Stay inside the policy's arrival radius. A fixed 70cm host stop
        // stranded routes configured with a smaller NavCloseStopRange.
        MoveArrivalRadius = FMath::Clamp(float(Number(I,TEXT("arrival_radius"),.8)) * 100.f,1.f,500.f) * .875f;
        // A fresh policy request may legitimately revisit the same coordinate.
        // Between decisions, latch arrival so a small correction cannot turn
        // the player back toward the waypoint that was just reached.
        bMoveArrived=false;
        FaceHeading.Reset();MovementOwner = P.Id; MoveTarget = Dest; MoveExpires = Now + 1.5; return;
    }
    MovementOwner.Empty(); MoveExpires = 0;
    if (Action == TEXT("cast"))
    {
        const int32 Spell = Guid(I,TEXT("spell")); int32 Target = 0;
        if (!C->GetKnownSpells().Contains(Spell)) { P.Status = TEXT("Spell is not known"); return; }
        const int32 Selected = Guid(I,TEXT("target"),uint32(C->GetSelectedObject().Guid));
        if (!ResolvePluginSpellTarget(Spell,Selected,Target))
        {P.Status=TEXT("Spell target is unavailable");return;}
        FACEWorldObject CastTargetObject;
        if(DefeatedTargets.Contains(Target)){P.NextAction=0;P.NextDecision=0;ScheduleDecisionWake(Now);return;}
        // ACE also marks inventory items Attackable. Their stale world positions
        // must not block equipment buffs with the hostile-creature sight check.
        const bool HostileCreature=C->GetWorldObject(Target,CastTargetObject)&&(CastTargetObject.ItemType&ACEItemType::Creature)
            &&CastTargetObject.IsAttackable()&&!CastTargetObject.bIsPlayer&&!CastTargetObject.PetOwnerId;
        if(HostileCreature&&!HasClearCombatSight(CastTargetObject))
        {C->GetSession()->SendCancelAttack();P.Status=TEXT("Target behind an obstruction; continuing route");return;}
        if(Target && Target!=C->GetPlayerGuid() && Target!=C->GetSelectedObject().Guid)C->SelectObject(Target);
        if(!PrepareCombatAction(P,I,8))return;
        // SendCastSpell's public UI path selects only in VR. Automation uses the same
        // resolved target and session cast action in both modes, without a fake trigger.
        PhysicalAttackTarget=0;PhysicalAttackOwner.Empty();
        PendingSpell=Spell;PendingSpellTarget=Target;PendingSpellAt=Now;PendingSpellOwner=P.Id;PendingSpellActivity=String(I,TEXT("activity"));
        PendingBuffResultDriven=P.IsPolicy()&&IsBuffCastIntent(I);PendingSpellWords=false;BuffRetryAt=0;
        PendingSpellConfirmation.Empty();PendingSpellItemConfirmations.Empty();PendingSpellRecoveryConfirmations.Empty();PendingSpellConfirmed=false;PendingSpellFizzled=false;
        // UseDone also succeeds for resisted spells. Track the specific server
        // magic-chat confirmation before considering an offensive enchantment active.
        uint32 School=0,Power=0,Category=0,Flags=0,Icon=0;double Duration=0;FString SpellName;
        auto* Dat=GetGameInstance()->GetSubsystem<UACEDatSubsystem>();
        bool FastCast=false;I->TryGetBoolField(TEXT("fast_cast"),FastCast);FastCastOwner.Empty();FastCastStarted=false;
        FastCastStartedAt=0;FastCastMovementApplied=false;
        if(Dat->TryGetSpellInfo(Spell,SpellName,Icon))
        {
            PrepareSpellConfirmation(SpellName);
        }
        if(HostileCreature&&Dat->IsPluginSingleTargetOffensiveSpell(Spell)&&Dat->TryGetSpellInfo(Spell,SpellName,Icon))
        {
            if(P.IsPolicy())this->CombatTarget=Target;
            FOffensiveCast Record;Record.Spell=Spell;Record.Target=Target;Record.Owner=P.Id;
            Record.SpellName=SpellName;Record.TargetName=CastTargetObject.Name;Record.Sent=Now;
            OffensiveCasts.Add(MoveTemp(Record));if(OffensiveCasts.Num()>8)OffensiveCasts.RemoveAt(0);
        }
        if(FastCast&&P.Permissions.Contains(TEXT("navigation"))&&Dat->TryGetPluginSpellInfo(Spell,School,Power,Category,Flags,Duration)
            &&ACEPluginCastMotion::FastBuff(School,Power,Flags,Duration,Category))FastCastOwner=P.Id;
        ConfigureCastMotion(P,I,Spell,false);
        C->GetSession()->SendCastSpell(Spell, Target); P.NextAction = Now + 3.5;
    }
    else if (Action == TEXT("attack"))
    {
        const auto Selection=C->GetSelectedObject();
        const int32 Target = Guid(I,TEXT("target"),uint32(PC->FindNearbyTarget(true,0,Selection.bShowHealth && Selection.HealthFraction<=0?Selection.Guid:0)));
        FACEWorldObject Monster;if(!C->GetWorldObject(Target,Monster)||!Monster.IsAttackable()||Monster.bIsPlayer||Monster.PetOwnerId||DefeatedTargets.Contains(Target))return;
        if(!HasClearCombatSight(Monster)){C->GetSession()->SendCancelAttack();P.Status=TEXT("Target behind an obstruction; continuing route");return;}
        if (!Target) { P.Status = TEXT("No eligible monster"); return; }
        const int32 Mode = Guid(I,TEXT("mode"),2);
        if (Mode != 2 && Mode != 4) { ReportActivityFailure(P,I, TEXT("Attack mode must be melee (2) or missile (4)")); return; }
        const double RequestedDebuff=Number(I,TEXT("debuff_spell"));
        if(!FMath::IsFinite(RequestedDebuff)||RequestedDebuff<0||RequestedDebuff>MAX_int32||RequestedDebuff!=FMath::FloorToDouble(RequestedDebuff))
        {ReportActivityFailure(P,I,TEXT("Invalid debuff spell"));return;}
        const uint32 DebuffSpell=uint32(RequestedDebuff);
        if(DebuffSpell)
        {
            FACEWorldObject Source;const int32 SourceId=Guid(I,TEXT("item"));
            const bool Equipped=C->GetWorldObject(SourceId,Source)&&(Source.WielderId==C->GetPlayerGuid()||(Source.CurrentWieldedLocation&&Source.ParentGuid==C->GetPlayerGuid()));
            auto Spell=Equipped&&P.Id==TEXT("ucm")&&Mode==4?UCMGrenadeSpell(Source):nullptr;
            if(!Spell&&Equipped&&P.Id==TEXT("ucm")&&!(Source.CurrentWieldedLocation&ACEEquipMask::Shield))
            {
                const auto Snapshot=PluginItemSnapshot(Source)->AsObject();const TSharedPtr<FJsonObject>* Proc=nullptr;
                bool Available=false,CanWield=false;
                const bool ModeMatches=Mode==2?(Source.ItemType&ACEItemType::MeleeWeapon)!=0:(Source.ItemType&ACEItemType::MissileWeapon)!=0;
                if(ModeMatches&&Snapshot->TryGetBoolField(TEXT("proc_debuff_available"),Available)&&Available
                    &&Snapshot->TryGetBoolField(TEXT("can_wield"),CanWield)&&CanWield&&Snapshot->TryGetObjectField(TEXT("proc_debuff"),Proc))Spell=*Proc;
            }
            if(!Spell||Number(Spell,TEXT("id"))!=DebuffSpell)
            {ReportActivityFailure(P,I,TEXT("Debuff weapon is unavailable or its requirements changed"));return;}
        }
        if(C->GetSelectedObject().Guid!=Target)C->SelectObject(Target);
        if(!PrepareCombatAction(P,I,Mode))return;
        const int32 Height = int32(FMath::Clamp(Number(I, TEXT("height"), 2), 1., 3.));
        const float Power = FMath::Clamp(float(Number(I, TEXT("power"), .5)), 0.f, 1.f);
        if(P.IsPolicy())CombatTarget=Target;
        // A proc can be requested while this exact attack is already repeating.
        // Track its result even when no duplicate attack packet is necessary.
        if(DebuffSpell)ObserveTargetSpellCast(DebuffSpell,Target);
        // Recovery/death events may wake Lua during an ongoing server repeat.
        // Re-evaluate policy immediately without restarting the same attack.
        if(PhysicalAttackOwner==P.Id&&PhysicalAttackTarget==Target&&PhysicalAttackMode==Mode
            &&PhysicalAttackHeight==Height&&FMath::IsNearlyEqual(PhysicalAttackPower,Power,.001f)&&Now<PhysicalAttackNextSendAt)
        {P.NextAction=Now;return;}
        PhysicalAttackTarget=Target;PhysicalAttackOwner=P.Id;PhysicalAttackUntil=Now+30;
        PhysicalAttackMode=Mode;PhysicalAttackHeight=Height;PhysicalAttackPower=Power;PhysicalAttackNextSendAt=Now+3.5;
        if (Mode == 2) C->SendTargetedMeleeAttack(Target, Height, Power); else C->SendTargetedMissileAttack(Target, Height, Power);
        PC->ShowPluginAttack(Target, Height, Power);
        // Throttle duplicate network attacks above, not the entire policy.
        // Death and vital updates must be able to choose a different action.
        P.NextAction = Now;
    }
    else ReportActivityFailure(P,I, TEXT("Unknown action: ") + Action);
}
void UACEPluginSubsystem::RecordRouteMovement(const FACEPosition& Position,uint32 TeleportSequence)
{
    if(!bTrackRouteMovement||!Position.IsValid())return;
    if(RouteMovementTeleport!=TeleportSequence){RouteMovementSamples.Reset();bRouteMovementOverflow=false;}
    RouteMovementTeleport=TeleportSequence;
    if(!RouteMovementSamples.IsEmpty()&&FVector::DistSquared(RouteMovementSamples.Last().ToUnrealLocation(),Position.ToUnrealLocation())<3600.)return;
    if(RouteMovementSamples.Num()>=256){bRouteMovementOverflow=true;return;}
    RouteMovementSamples.Add(Position);
}
void UACEPluginSubsystem::ConfigureCastMotion(const FACEClientPlugin& P,const TSharedPtr<FJsonObject>& I,int32 Spell,bool Item)
{
    OptionalCastOwner.Empty();if(!P.Permissions.Contains(TEXT("navigation")))return;
    bool Jiggle=false,Jump=false;I->TryGetBoolField(TEXT("cast_jiggle"),Jiggle);I->TryGetBoolField(TEXT("jump_out_wand"),Jump);
    uint32 School=0,Power=0,Category=0,Flags=0;double Duration=0;
    if(!GetGameInstance()->GetSubsystem<UACEDatSubsystem>()->TryGetPluginSpellInfo(Spell,School,Power,Category,Flags,Duration))return;
    if((Item&&Jump)||(Jiggle&&ACEPluginCastMotion::Jiggle(School,Power,Flags,Duration,Category,Item)))
    {
        OptionalCastOwner=P.Id;OptionalCastAt=FPlatformTime::Seconds();OptionalCastItem=Item;OptionalCastJump=Item&&Jump;OptionalCastStarted=Item;
        FastCastOwner.Empty();FastCastStarted=false;
    }
}

void UACEPluginSubsystem::ApplyMovement(AACEPlayerController* PC, float& F, float& R, float& T, bool Manual, bool Blocked, bool VR, const FVector& Facing, float MaxFrameTravel)
{
    // Sample the ordinary predicted path even while Lua waits for a server
    // attack/use acknowledgment. No new movement or networking is generated.
    if(bTrackRouteMovement&&PC)
    {
        auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
        FACEPosition Position=C->GetPlayerPosition();PC->TryGetLocallyPredictedPosition(Position);
        RecordRouteMovement(Position,C->GetTeleportSeq());
    }
    if (Manual)
    {
        // Player input wins this frame, but independent activities (especially
        // loot during manual combat) keep their VM, queues and running state.
        // Discard stale steering so it cannot snap back after input is released;
        // the next normal plugin decision can request fresh movement.
        MovementOwner.Empty();MoveExpires=0;FaceHeading.Reset();
        FastCastOwner.Empty();FastCastStarted=false;
        OptionalCastOwner.Empty();
        if(PC)
        {
            PC->CancelPluginJump();
            if(!UseApproachOwner.IsEmpty())PC->EndUseApproach();
        }
        UseApproachOwner.Empty();
        return;
    }
    if(!OptionalCastOwner.IsEmpty())
    {
        const auto Owner=Find(OptionalCastOwner);double Elapsed=FPlatformTime::Seconds()-OptionalCastAt;
        if(!Owner||!Owner->Running||Blocked||Elapsed>=5||Elapsed<0||!PC||!PC->GetPawn())
        {OptionalCastOwner.Empty();return;}
        if(OptionalCastJump)
        {
            if(Elapsed>=.2)
            {
                OptionalCastOwner.Empty();
                if(Elapsed<.75&&!PC->IsPluginJumpPending())PC->QueuePluginJump(0,PC->GetPawn()->GetActorRotation().Yaw,0);
            }
            return;
        }
        if(!OptionalCastStarted&&PendingSpellWords){OptionalCastStarted=true;OptionalCastAt=FPlatformTime::Seconds();Elapsed=0;}
        if(OptionalCastStarted){F=-1;R=ACEPluginCastMotion::JiggleStrafe(Elapsed);T=0;}
        return;
    }
    if(!FastCastOwner.IsEmpty())
    {
        // VT's words/result callbacks press/release Backward as input events.
        // Do not convert that into held locomotion: even one integrated frame
        // moves the caster, and a complete buff cycle can back into a portal.
        if(!PendingSpell || FPlatformTime::Seconds()-PendingSpellAt>30
            || (FastCastStarted && (FPlatformTime::Seconds()-FastCastStartedAt>=ACEPluginCastMotion::TapRequestSeconds
                || PendingSpellFizzled || FastCastMovementApplied)))
        {FastCastOwner.Empty();FastCastStarted=false;CompleteConfirmedBuff();return;}
        else
        {
            // Send both ordinary movement edges before prediction can advance.
            // Late/missing results can never extend the tap or keep a key held.
            if(FastCastStarted&&!Blocked&&PC&&PC->GetPawn())
            {
                F=R=T=0;
                FastCastMovementApplied=PC->TryFastBuffTap();
                if(FastCastMovementApplied){FastCastOwner.Empty();FastCastStarted=false;CompleteConfirmedBuff();}
            }
            return;
        }
    }
    if (MovementOwner.IsEmpty()) return;
    const double Now = FPlatformTime::Seconds();
    if (Blocked || Now > MoveExpires || !PC->GetPawn()) { MovementOwner.Empty(); return; }
    FACEPosition Pos; if (!PC->TryGetLocallyPredictedPosition(Pos)) Pos = GetGameInstance()->GetSubsystem<UACEClientSubsystem>()->GetPlayerPosition();
    if(FaceHeading.IsSet())
    {
        const float Delta=FMath::FindDeltaAngleDegrees(Pos.ToUnrealQuat().Rotator().Yaw,FaceHeading.GetValue());
        F=R=0;T=FMath::Abs(Delta)<1?0:FMath::Clamp(Delta/45.f,-1.f,1.f);return;
    }
    const FVector Location = Pos.ToUnrealLocation();
    if (FVector::Dist2D(Location, LastMovePosition) > 20) { LastMovePosition = Location; LastProgress = Now; }
    if (Now - LastProgress > 5)
    {
        // Let UCM backtrack the approach and blacklist an unreachable target.
        // Other plugins retain the safety stop; no teleport or collision bypass.
        if(auto Owner=Find(MovementOwner);Owner&&Owner->IsPolicy())
        {
            ++MovementBlockedSerial;bRouteJoinRequested=true;RouteVisibilityOffset=0;MovementOwner.Empty();MoveExpires=0;F=R=T=0;
            if(auto P=PolicyPlugin()){LogUCMEvent(*P,TEXT("No movement progress for 5 seconds; requesting route recovery"),EACEUCMLogLevel::Warning);P->NextDecision=0;}
            if(auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>())if(auto Session=C->GetSession())Session->SendCancelAttack();
        }
        else for(const auto& P:Plugins) if(P->Running&&!P->LocalService&&!P->Companion) Stop(P->Id,TEXT("Route blocked; manual reposition required"),true);
        return;
    }
    const FVector Delta = MoveTarget.ToUnrealLocation() - Location;
    if (bMoveArrived || Delta.Size() < MoveArrivalRadius || Delta.Size2D() < 1.)
    {
        F=R=T=0;
        if (!bMoveArrived && Delta.Size() < MoveArrivalRadius)
        {
            bMoveArrived = true;
            // Advance on the next policy tick instead of waiting a quarter
            // second at every breadcrumb. Never bypass pending use/cast gates.
            if (auto P=Find(MovementOwner)) { P->NextDecision=0; ScheduleDecisionWake(Now); }
        }
        // Use the policy's 3D arrival metric on ramps. An XY-only stop at
        // 70cm could remain almost a metre away along a slope forever.
        // If the recorded height is unreachable, retain the progress watchdog.
        // Recreating an expired movement every tick used to wait forever here.
        return;
    }
    bMoveArrived = false;
    // The AC avatar travels along local +Y, not Unreal's actor +X. Steer
    // from the predicted physics pose, independently of camera/visual rotation.
    const FVector Direction = Delta.GetSafeNormal2D(), Forward = (VR ? Facing : Pos.GetAceForwardVector()).GetSafeNormal2D();
    const float Dot = FVector::DotProduct(Direction, Forward);
    if (VR) { F = Dot; R = FVector::DotProduct(Direction, FVector::CrossProduct(FVector::UpVector, Forward)); }
    else
    {
        const float Angle = FMath::Atan2(FVector::CrossProduct(Forward, Direction).Z, Dot);
        // Controller integrates -T in AC space; the X reflection reverses
        // that sign in Unreal space. Positive T therefore increases UE yaw.
        T = FMath::Clamp(Angle * 2.f, -1.f, 1.f); F = FMath::Abs(Angle) < .3 ? 1.f : 0.f; R = 0;
    }
    // A high Run skill or slow frame can cross an entire small arrival sphere
    // between polls. Bound this frame's normal analog input to the destination;
    // prediction, collision and MoveToState still use the same movement axes.
    if (MaxFrameTravel > 0.f)
    {
        const float Scale=FMath::Min(1.f, float(Delta.Size2D()) / MaxFrameTravel);
        F*=Scale; R*=Scale;
    }
}
