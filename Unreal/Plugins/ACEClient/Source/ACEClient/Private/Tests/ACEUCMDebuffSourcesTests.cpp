#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEVTProfile.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMDebuffSourcesTest,"ACE.Plugins.DebuffSourcesAndIdleHelpers",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMDebuffSourcesTest::RunTest(const FString&)
{
 auto Json=[](const TCHAR* T){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),O);return O;};
 FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
 auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"position":{"cell":30998795,"x":0,"y":0,"z":0},"targets":[],"spells":[],"route_objects":[{"id":2,"name":"Ally","object_class":24,"distance":10,"line_of_sight":true}],"inventory":[{"id":51,"name":"Wand","type":32768,"slots":16777216,"equipped_slot":16777216,"identified":true,"can_wield":true,"usable":true,"equipped":true,"count":1}]})"));};
 auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":false,"item_mana":false,"navigation":false})"));};
 auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
 auto Str=[](auto I,const TCHAR* K){FString V;I->TryGetStringField(K,V);return V;};
 auto Number=[](auto I,const TCHAR* K){double V=0;I->TryGetNumberField(K,V);return V;};
 auto One=[](auto O){return TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(O)};};
 {
  auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("random_helper_buffs"),true);P->SetNumberField(TEXT("random_helper_interval"),15);
  S->SetArrayField(TEXT("spells"),One(Json(TEXT(R"({"id":101,"name":"Endurance Other","category":3,"school":4,"power":50,"skill":300,"duration":60,"beneficial":true,"caster_target":false})"))));
  FACEPluginVM VM;TestTrue(TEXT("Policy loads"),VM.Load(Source,Error));auto I=Step(VM,S,P);
  TestEqual(TEXT("Idle helper buffs nearby non-fellow"),Number(I,TEXT("target")),2.);TestEqual(TEXT("Only eligible family is chosen"),Number(I,TEXT("spell")),101.);
  S->SetNumberField(TEXT("time"),114);TestTrue(TEXT("Helper minimum interval enforced"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("cast"));
  S->SetNumberField(TEXT("time"),115);TestEqual(TEXT("Helper resumes after interval"),Str(Step(VM,S,P),TEXT("action")),FString(TEXT("cast")));
  for(int Case=0;Case<4;++Case)
  {
   auto Recipient=S->GetArrayField(TEXT("route_objects"))[0]->AsObject();Recipient->SetNumberField(TEXT("object_class"),Case==0?16:24);Recipient->SetNumberField(TEXT("distance"),Case==1?19:10);Recipient->SetBoolField(TEXT("line_of_sight"),Case!=2);Recipient->SetNumberField(TEXT("id"),Case==3?1:2);
   FACEPluginVM Fresh;Fresh.Load(Source,Error);TestTrue(TEXT("NPC, distant, blocked and self targets excluded"),Str(Step(Fresh,S,P),TEXT("action"))!=TEXT("cast"));
  }
 }
 for(int SourceKind=0;SourceKind<4;++SourceKind)
 {
  const bool Grenade=SourceKind==1,Proc=SourceKind>=2,Missile=SourceKind==3,Physical=Grenade||Proc;
  auto S=Snapshot(),P=Profile();P->SetStringField(TEXT("combat"),TEXT("magic"));P->SetBoolField(TEXT("debuff_imperil"),true);
  P->SetArrayField(TEXT("monsters"),One(Json(TEXT(R"({"name":"Default","default":true,"bolt":false,"streak":false})"))));
  S->SetArrayField(TEXT("targets"),One(Json(TEXT(R"({"id":9,"name":"Drudge","distance":1,"identified":true,"line_of_sight":true})"))));
  auto Learned=Json(TEXT(R"({"id":101,"name":"Imperil Other","category":116,"school":3,"power":200,"skill":300,"duration":60,"beneficial":false,"caster_target":false})"));S->SetArrayField(TEXT("spells"),One(Learned));
  auto Innate=Json(TEXT(R"({"id":201,"name":"Item Imperil","category":116,"school":3,"power":150,"skill":500,"duration":60,"beneficial":false,"caster_target":false})"));auto Item=S->GetArrayField(TEXT("inventory"))[0]->AsObject();
  if(Grenade){auto G=MakeShared<FJsonObject>();G->SetNumberField(TEXT("item"),51);G->SetObjectField(TEXT("spell"),Innate);S->SetArrayField(TEXT("grenade_debuffs"),One(G));Item->SetNumberField(TEXT("type"),256);}
  else if(Proc){Item->SetObjectField(TEXT("proc_debuff"),Innate);Item->SetBoolField(TEXT("proc_debuff_available"),true);Item->SetNumberField(TEXT("type"),Missile?256:1);}
  else {Item->SetObjectField(TEXT("innate_spell"),Innate);Item->SetBoolField(TEXT("innate_spell_available"),true);}
  FACEPluginVM VM;VM.Load(Source,Error);auto I=Step(VM,S,P);
  TestEqual(TEXT("Skill preference chooses stronger item casting skill"),Str(I,TEXT("action")),FString(Physical?TEXT("attack"):TEXT("cast_item")));
  TestEqual(TEXT("Uses selected inventory item"),Number(I,TEXT("item")),51.);
  if(Grenade)TestEqual(TEXT("Grenades use full accuracy"),Number(I,TEXT("power")),1.);
  if(Proc){TestEqual(TEXT("Proc attacks use the weapon's combat mode"),Number(I,TEXT("mode")),Missile?4.:2.);TestEqual(TEXT("Proc speed/accuracy follows VT dz"),Number(I,TEXT("power")),Missile?1.:0.);}
  S->SetArrayField(TEXT("action_events"),One(Json(TEXT(R"({"type":"use","source":51,"target":9,"action_serial":1,"requested_at":100,"error":0})"))));
  TestTrue(TEXT("Successful use alone cannot confirm debuff"),Str(Step(VM,S,P),TEXT("action")).IsEmpty());
  S->SetArrayField(TEXT("debuffs"),One(Json(TEXT(R"({"target":9,"category":116,"power":150,"expires":170})"))));
  I=Step(VM,S,P);TestEqual(TEXT("Physical auto-repeat is cancelled on confirmation"),Str(I,TEXT("action")),FString(Physical?TEXT("cancel_attack"):TEXT("")));
  if(Proc)
  {
   S->SetArrayField(TEXT("debuffs"),{});
   FACEPluginVM Timeout;Timeout.Load(Source,Error);Step(Timeout,S,P);S->SetNumberField(TEXT("time"),111);
   TestEqual(TEXT("Unconfirmed proc cancels after ten seconds"),Str(Step(Timeout,S,P),TEXT("action")),FString(TEXT("cancel_attack")));S->SetNumberField(TEXT("time"),100);
   FACEPluginVM Gone;Gone.Load(Source,Error);Step(Gone,S,P);auto Targets=S->GetArrayField(TEXT("targets"));S->SetArrayField(TEXT("targets"),{});
   TestEqual(TEXT("Proc cancels when target leaves"),Str(Step(Gone,S,P),TEXT("action")),FString(TEXT("cancel_attack")));S->SetArrayField(TEXT("targets"),Targets);
   if(!Missile)
   {
    Targets[0]->AsObject()->SetNumberField(TEXT("distance"),10);P->SetBoolField(TEXT("approach"),false);FACEPluginVM Range;Range.Load(Source,Error);
    TestTrue(TEXT("Proc cannot charge when approach disabled"),Str(Step(Range,S,P),TEXT("action")).IsEmpty());Targets[0]->AsObject()->SetNumberField(TEXT("distance"),1);
   }
   else
   {
    Item->SetNumberField(TEXT("ammo_type"),2);FACEPluginVM Empty;Empty.Load(Source,Error);
    // The learned spell remains usable only with a separate casting implement.
    S->SetArrayField(TEXT("spells"),{});
    TestNotEqual(TEXT("Proc launcher without ammo never attacks"),Str(Step(Empty,S,P),TEXT("action")),FString(TEXT("attack")));
    auto Ammo=Json(TEXT(R"({"id":52,"type":256,"slots":8388608,"ammo_type":2,"count":10,"identified":true,"can_wield":true,"damage":20,"name":"Bolt"})"));
    auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Ammo));S->SetArrayField(TEXT("inventory"),Items);FACEPluginVM Reload;Reload.Load(Source,Error);
    TestEqual(TEXT("Proc launcher equips its correct ammunition"),Number(Step(Reload,S,P),TEXT("item")),52.);
    Ammo->SetBoolField(TEXT("equipped"),true);TestEqual(TEXT("Equipped ammunition enables proc attack"),Str(Step(Reload,S,P),TEXT("action")),FString(TEXT("attack")));
    S->SetArrayField(TEXT("inventory"),One(Item));Item->RemoveField(TEXT("ammo_type"));S->SetArrayField(TEXT("spells"),One(Learned));
   }
  }
  S->SetArrayField(TEXT("debuffs"),{});P->SetNumberField(TEXT("debuff_selection"),1);
  // A learned cast needs a wand; restore one for the independent strength test.
  Item->SetNumberField(TEXT("type"),32768);FACEPluginVM Strength;Strength.Load(Source,Error);
  TestEqual(TEXT("Strength preference chooses stronger learned spell"),Number(Step(Strength,S,P),TEXT("spell")),101.);
  Innate->SetNumberField(TEXT("power"),200);Innate->SetNumberField(TEXT("skill"),300);FACEPluginVM Tie;Tie.Load(Source,Error);
  TestEqual(TEXT("Ties prefer learned spells"),Str(Step(Tie,S,P),TEXT("action")),FString(TEXT("cast")));
 }
 {
  TArray<FString> Issues;auto D=Json(TEXT(R"({"format":"usd","tables":[{"name":"Settings","columns":["Setting","Value"],"rows":[["RandomHelperBuffs",true],["RandomHelperIntervalSeconds",7],["DebuffSelectionMethod",1],["DoJiggle",true],["JumpOutWandCasting",true]]},{"name":"AssistItems","columns":["Object","Type"],"rows":[["Grenade",7]]}],"grenade_catalog":{"Grenade":{"WieldReqType":2,"WieldReqAttribute":47,"WieldReqValue":100,"Spell":101,"Spellcraft":500}}})"));
  auto P=ACEVTProfile::Convert(D,Issues);TestEqual(TEXT("Helper and grenade options import"),Issues.Num(),0);TestTrue(TEXT("Helper enabled"),P->GetBoolField(TEXT("random_helper_buffs")));TestEqual(TEXT("Preference imported"),P->GetNumberField(TEXT("debuff_selection")),1.);
  TestTrue(TEXT("Optional casting movement settings import"),P->GetBoolField(TEXT("cast_jiggle"))&&P->GetBoolField(TEXT("jump_out_wand")));
  D->RemoveField(TEXT("grenade_catalog"));Issues.Reset();ACEVTProfile::Convert(D,Issues);TestTrue(TEXT("Missing grenade database cannot silently import"),!Issues.IsEmpty());
 }
 {
  const FString Folder=FPaths::ProjectSavedDir()/TEXT("Tests/GrenadeDatabase")/FGuid::NewGuid().ToString();IFileManager::Get().MakeDirectory(*Folder,true);
  const FString Settings=TEXT("1\nAssistItems\n2\nObject\nType\nn\nn\n1\ns\nFixture grenade\ni\n7\n");
  const FString Database=TEXT("1\nGrenadeOptions\n6\nGrenName\nWieldReqType\nWieldReqAttribute\nWieldReqValue\nSpell\nSpellcraft\nn\nn\nn\nn\nn\nn\n1\ns\nFixture grenade\ni\n7\ni\n0\ni\n100\ni\n101\ni\n500\n");
  FFileHelper::SaveStringToFile(Settings,*(Folder/TEXT("Grenades.usd")));FFileHelper::SaveStringToFile(Database,*(Folder/TEXT("gameinfodb.ugd")));
  TArray<FString> Issues;auto P=ACEVTProfile::ConvertFile(Folder/TEXT("Grenades.usd"),Issues);
  TestEqual(TEXT("Typed UGD companion import succeeds"),Issues.Num(),0);
  if(TestTrue(TEXT("Imported grenade catalog retained"),P&&P->HasTypedField<EJson::Object>(TEXT("grenade_catalog"))))
   TestEqual(TEXT("UGD spellcraft preserved"),P->GetObjectField(TEXT("grenade_catalog"))->GetObjectField(TEXT("Fixture grenade"))->GetNumberField(TEXT("Spellcraft")),500.);
  FFileHelper::SaveStringToFile(TEXT("invalid"),*(Folder/TEXT("gameinfodb.ugd")));ACEVTProfile::ConvertFile(Folder/TEXT("Grenades.usd"),Issues);
  TestTrue(TEXT("Invalid UGD rejects activation"),!Issues.IsEmpty());
 }
 return true;
}
#endif
