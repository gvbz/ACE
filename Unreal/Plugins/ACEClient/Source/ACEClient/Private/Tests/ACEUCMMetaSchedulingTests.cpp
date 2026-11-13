#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMMetaSchedulingTest,"ACE.Plugins.MetaInteractionScheduling",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMMetaSchedulingTest::RunTest(const FString&)
{
 auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
 FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
 auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_events":[],"inventory_failure_serial":0,"teleport_sequence":1,"position":{"cell":30998795,"x":0,"y":0,"z":0},"spells":[],"targets":[],"route_objects":[{"id":10,"name":"Collector"}],"inventory":[{"id":51,"name":"Potion","type":32,"identified":true,"usable":true,"count":5,"boost_vital":2,"boost":50},{"id":52,"name":"Token","count":2}]})"));};
 auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"off","looting":false,"item_mana":false,"navigation":true,"route":[{"cell":30998795,"x":15,"y":0,"z":0}],"ucm_commands":[{"op":"use","item":"Collector"},{"op":"say","text":"next command"}]})"));};
 auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
 auto Action=[](auto I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  auto Commands=Json(TEXT(R"({"v":[{"op":"fellowship","operation":"create","name":"Hunting party"},{"op":"fellowship","operation":"recruit","name":"Ally"},{"op":"say","text":"ready"}]})"));
  P->SetArrayField(TEXT("ucm_commands"),Commands->GetArrayField(TEXT("v")));
  S->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":2,"name":"Ally","object_class":24})")))});
  auto I=Step(VM,S,P);TestEqual(TEXT("Create is submitted first"),I->GetStringField(TEXT("operation")),FString(TEXT("create")));P->RemoveField(TEXT("ucm_commands"));
  TestEqual(TEXT("Recruit waits for fellowship creation"),Action(Step(VM,S,P)),FString());
  S->SetNumberField(TEXT("health"),10);TestEqual(TEXT("Recovery runs while fellowship update is delayed"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);S->SetNumberField(TEXT("health"),100);
  auto F=Json(TEXT(R"({"valid":true,"name":"Hunting party","leader":1,"open":false,"members":[{"id":1,"name":"Self"}]})"));S->SetObjectField(TEXT("fellowship"),F);
  I=Step(VM,S,P);TestEqual(TEXT("Server creation permits recruit"),I->GetStringField(TEXT("operation")),FString(TEXT("recruit")));TestEqual(TEXT("Recruit resolves player ID"),I->GetNumberField(TEXT("target")),2.);
  S->SetNumberField(TEXT("teleport_sequence"),2);S->SetNumberField(TEXT("action_serial"),1);
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":51,"target":51,"action_serial":1,"requested_at":101,"error":123})")))});
  TestEqual(TEXT("Teleport and unrelated error cannot accept an invitation"),Action(Step(VM,S,P)),FString());
  auto Members=F->GetArrayField(TEXT("members"));Members.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":2,"name":"Ally"})"))));F->SetArrayField(TEXT("members"),Members);
  TestEqual(TEXT("Confirmed member advances command sequence"),Action(Step(VM,S,P)),FString(TEXT("say")));
 }
 for(const TCHAR* Op:{TEXT("open"),TEXT("close"),TEXT("dismiss"),TEXT("leader"),TEXT("quit"),TEXT("disband")})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  auto F=Json(TEXT(R"({"valid":true,"name":"Party","leader":1,"open":false,"members":[{"id":1,"name":"Self"},{"id":2,"name":"Ally"}]})"));S->SetObjectField(TEXT("fellowship"),F);
  const FString Operation(Op);F->SetBoolField(TEXT("open"),Operation==TEXT("close"));
  auto Command=Json(TEXT(R"({"op":"fellowship","name":"Ally"})"));Command->SetStringField(TEXT("operation"),Op);
  auto Commands=P->GetArrayField(TEXT("ucm_commands"));Commands[0]=MakeShared<FJsonValueObject>(Command);P->SetArrayField(TEXT("ucm_commands"),Commands);
  TestEqual(TEXT("Management command submits once"),Action(Step(VM,S,P)),FString(TEXT("fellowship")));P->RemoveField(TEXT("ucm_commands"));
  TestEqual(TEXT("Unchanged fellowship cannot complete management"),Action(Step(VM,S,P)),FString());
  if(Operation==TEXT("open")||Operation==TEXT("close"))F->SetBoolField(TEXT("open"),Operation==TEXT("open"));
  if(Operation==TEXT("dismiss"))F->SetArrayField(TEXT("members"),{F->GetArrayField(TEXT("members"))[0]});
  if(Operation==TEXT("leader"))F->SetNumberField(TEXT("leader"),2);
  if(Operation==TEXT("quit")||Operation==TEXT("disband"))F->SetBoolField(TEXT("valid"),false);
  TestEqual(TEXT("Observed server result completes management"),Action(Step(VM,S,P)),FString(TEXT("say")));
 }
 for(bool Changed:{false,true})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  auto F=Json(TEXT(R"({"valid":true,"name":"Party","leader":1,"open":false,"members":[]})"));S->SetObjectField(TEXT("fellowship"),F);
  auto Commands=P->GetArrayField(TEXT("ucm_commands"));Commands[0]=MakeShared<FJsonValueObject>(Json(TEXT(R"({"op":"fellowship","operation":"open"})")));P->SetArrayField(TEXT("ucm_commands"),Commands);
  Step(VM,S,P);P->RemoveField(TEXT("ucm_commands"));
  if(Changed){F->SetStringField(TEXT("name"),TEXT("Another party"));F->SetBoolField(TEXT("open"),true);}else S->SetNumberField(TEXT("time"),131);
  TestEqual(TEXT("Wrong fellowship or timeout fails without replay"),Action(Step(VM,S,P)),FString(TEXT("activity_failed")));
 }
 for(bool Failed:{false,true})
 {
  FACEPluginVM VM;TestTrue(TEXT("Meta policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();
  TestEqual(TEXT("World command submits once"),Action(Step(VM,S,P)),FString(TEXT("use_world")));P->RemoveField(TEXT("ucm_commands"));
  S->SetNumberField(TEXT("health"),10);S->SetBoolField(TEXT("busy"),true);
  TestEqual(TEXT("Native busy gate is respected"),Action(Step(VM,S,P)),FString());S->SetBoolField(TEXT("busy"),false);
  TestEqual(TEXT("Healing may run during the receipt wait"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);S->SetNumberField(TEXT("health"),100);
  S->SetNumberField(TEXT("action_serial"),3);S->SetNumberField(TEXT("action_error"),123);
  S->SetArrayField(TEXT("action_events"),{
   MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":51,"target":51,"action_serial":1,"requested_at":101,"error":0})"))),
   MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":10,"target":10,"action_serial":2,"requested_at":99,"error":0})"))),
   MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"cast","spell":10,"target":10,"action_serial":3,"requested_at":101,"error":123})")))});
  auto I=Step(VM,S,P);TestEqual(TEXT("Unrelated and old receipts cannot advance commands or navigation"),Action(I),FString());
  TestEqual(TEXT("Pending interaction remains visible"),I->GetStringField(TEXT("status")),FString(TEXT("Waiting for meta interaction")));
  auto Receipt=Json(TEXT(R"({"type":"use","source":10,"target":10,"action_serial":4,"requested_at":100,"error":0})"));
  if(Failed)Receipt->SetNumberField(TEXT("error"),123);S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});
  I=Step(VM,S,P);TestEqual(TEXT("Matching result advances or fails only this meta activity"),Action(I),FString(Failed?TEXT("activity_failed"):TEXT("say")));
 }
 for(int32 Outcome=0;Outcome<4;++Outcome)
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  auto Commands=P->GetArrayField(TEXT("ucm_commands"));Commands[0]=MakeShared<FJsonValueObject>(Json(TEXT(R"({"op":"give","item":"Token","target":"Collector"})")));P->SetArrayField(TEXT("ucm_commands"),Commands);
  TestEqual(TEXT("Give submits once"),Action(Step(VM,S,P)),FString(TEXT("give")));P->RemoveField(TEXT("ucm_commands"));
  S->SetNumberField(TEXT("action_serial"),1);
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":51,"action_serial":1,"requested_at":100,"error":0})")))});
  TestEqual(TEXT("Give cannot succeed from an unrelated UseDone"),Action(Step(VM,S,P)),FString());
  S->SetNumberField(TEXT("teleport_sequence"),2);
  TestEqual(TEXT("Unrelated teleport cannot confirm delivery"),Action(Step(VM,S,P)),FString());
  if(Outcome==0)S->GetArrayField(TEXT("inventory"))[1]->AsObject()->SetNumberField(TEXT("count"),1);
  if(Outcome==1)S->SetArrayField(TEXT("inventory"),{S->GetArrayField(TEXT("inventory"))[0]});
  if(Outcome==2){S->SetNumberField(TEXT("inventory_failure_serial"),1);S->SetNumberField(TEXT("inventory_failure_item"),52);}
  if(Outcome==3)S->SetNumberField(TEXT("time"),145);
  TestEqual(TEXT("ACE/GDLE inventory result or timeout settles give"),Action(Step(VM,S,P)),FString(Outcome<2?TEXT("say"):TEXT("activity_failed")));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  auto Commands=P->GetArrayField(TEXT("ucm_commands"));Commands[0]=MakeShared<FJsonValueObject>(Json(TEXT(R"({"op":"give","item":"Token","target":"Collector"})")));P->SetArrayField(TEXT("ucm_commands"),Commands);
  Step(VM,S,P);P->RemoveField(TEXT("ucm_commands"));TArray<TSharedPtr<FJsonValue>> Capped;
  for(int32 N=0;N<1024;++N){auto Item=MakeShared<FJsonObject>();Item->SetNumberField(TEXT("id"),1000+N);Capped.Add(MakeShared<FJsonValueObject>(Item));}
  S->SetArrayField(TEXT("inventory"),Capped);TestEqual(TEXT("Snapshot cap cannot masquerade as a delivered item"),Action(Step(VM,S,P)),FString());
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("ucm_command_only"),true);
  TestEqual(TEXT("Stopped command starts"),Action(Step(VM,S,P)),FString(TEXT("use_world")));P->RemoveField(TEXT("ucm_commands"));
  TestEqual(TEXT("Command-only does not finish while a meta wait yields"),Action(Step(VM,S,P)),FString());
  S->SetNumberField(TEXT("time"),145);TestEqual(TEXT("Command-only wait is bounded"),Action(Step(VM,S,P)),FString(TEXT("activity_failed")));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->RemoveField(TEXT("ucm_commands"));
  P->SetArrayField(TEXT("vt_meta"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"state":"Default","condition":{"type":1},"action":{"type":2,"command":{"op":"expression","expression":{"op":"call","name":"actiontrycastbyid","args":[{"op":"literal","value":20}]}}}})")))});
  S->SetNumberField(TEXT("combat_mode"),8);
  S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":20,"name":"Heal Self I","power":50,"skill":300,"caster_target":true,"known":true})")))});
  auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":99,"name":"Wand","type":32768,"equipped":true,"identified":true,"can_wield":true})"))));S->SetArrayField(TEXT("inventory"),Inventory);
  TestEqual(TEXT("Meta expression casts"),Action(Step(VM,S,P)),FString(TEXT("cast")));
  auto Receipt=Json(TEXT(R"({"type":"cast","spell":20,"target":2,"action_serial":1,"requested_at":100,"error":0,"confirmed":true})"));
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});
  TestEqual(TEXT("Same spell on another target cannot finish meta cast"),Action(Step(VM,S,P)),FString());
  Receipt->SetNumberField(TEXT("target"),1);TestEqual(TEXT("Exact cast receipt releases navigation"),Action(Step(VM,S,P)),FString(TEXT("move")));
 }
 // Pending assistance results are independent of navigation and of unrelated
 // failed actions, just like the meta receipt waits above.
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->RemoveField(TEXT("ucm_commands"));
  P->SetNumberField(TEXT("helper_health_threshold"),60);
  auto Fellow=Json(TEXT(R"({"id":2,"name":"Ally","health":20,"max_health":100,"vitals_age":1,"distance":10,"line_of_sight":true})"));
  auto Fellowship=Json(TEXT(R"({"valid":true})"));Fellowship->SetArrayField(TEXT("members"),{MakeShared<FJsonValueObject>(Fellow)});S->SetObjectField(TEXT("fellowship"),Fellowship);
  S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":30,"name":"Heal Other","category":79,"power":50,"skill":300,"duration":0,"caster_target":false,"beneficial":true})")))});
  auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":99,"name":"Wand","type":32768,"equipped":true,"identified":true,"can_wield":true})"))));S->SetArrayField(TEXT("inventory"),Items);
  TestEqual(TEXT("Assist casts on injured fellow"),Action(Step(VM,S,P)),FString(TEXT("cast")));
  S->SetBoolField(TEXT("busy"),true);TestEqual(TEXT("Assist still honors native busy gate"),Action(Step(VM,S,P)),FString());S->SetBoolField(TEXT("busy"),false);
  S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),3);
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"cast","spell":30,"target":3,"action_serial":1,"requested_at":100,"error":3})")))});
  TestEqual(TEXT("Pending fellow update yields to route without repeating cast"),Action(Step(VM,S,P)),FString(TEXT("move")));
  Fellow->SetNumberField(TEXT("health"),100);Step(VM,S,P);Fellow->SetNumberField(TEXT("health"),20);
  TestEqual(TEXT("Other target's error did not impose helper backoff"),Action(Step(VM,S,P)),FString(TEXT("cast")));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->RemoveField(TEXT("ucm_commands"));P->SetBoolField(TEXT("dispel_items"),true);
  auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":81,"name":"Society Gem of Dispelling","usable":true,"count":5})"))));S->SetArrayField(TEXT("inventory"),Items);
  auto Effects=TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":77,"category":102,"power":400,"remaining":100})")))};S->SetArrayField(TEXT("harmful_enchantments"),Effects);
  TestEqual(TEXT("Dispel uses supply"),Step(VM,S,P)->GetNumberField(TEXT("item")),81.);
  S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),3);
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":51,"action_serial":1,"requested_at":100,"error":3})")))});
  TestEqual(TEXT("Waiting for enchantment removal yields without consuming again"),Action(Step(VM,S,P)),FString(TEXT("move")));
  S->SetArrayField(TEXT("harmful_enchantments"),{});Step(VM,S,P);S->SetArrayField(TEXT("harmful_enchantments"),Effects);
  TestEqual(TEXT("Unrelated use error cannot back off a successful dispel"),Step(VM,S,P)->GetNumberField(TEXT("item")),81.);
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->RemoveField(TEXT("ucm_commands"));P->SetBoolField(TEXT("navigation"),false);S->SetNumberField(TEXT("combat_mode"),1);
  P->SetArrayField(TEXT("assist_items"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"name":"[All Peas]","type":11})")))});
  P->SetArrayField(TEXT("pea_recipes"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"tool":"Splitter","input":"Pea","output":"Scarab","count":50})")))});
  auto Items=S->GetArrayField(TEXT("inventory"));auto Supplies=Json(TEXT(R"({"v":[{"id":71,"name":"Splitter","usable":true},{"id":72,"name":"Pea","count":2}]})"));
  for(const auto& Item:Supplies->GetArrayField(TEXT("v")))Items.Add(Item);S->SetArrayField(TEXT("inventory"),Items);
  TestEqual(TEXT("Component split starts"),Action(Step(VM,S,P)),FString(TEXT("apply_item")));
  S->SetNumberField(TEXT("health"),10);TestEqual(TEXT("Component replication wait cannot postpone a heal"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);S->SetNumberField(TEXT("health"),100);
  auto Receipt=Json(TEXT(R"({"type":"use","source":71,"target":99,"action_serial":1,"requested_at":100,"error":3})"));S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Receipt)});S->SetNumberField(TEXT("action_serial"),1);S->SetNumberField(TEXT("action_error"),3);
  TestEqual(TEXT("Same splitter with wrong target cannot reject pending split"),Action(Step(VM,S,P)),FString());
  Receipt->SetNumberField(TEXT("target"),72);TestEqual(TEXT("Matching split rejection pauses components"),Action(Step(VM,S,P)),FString(TEXT("activity_failed")));
 }
 return !HasAnyErrors();
}
#endif
