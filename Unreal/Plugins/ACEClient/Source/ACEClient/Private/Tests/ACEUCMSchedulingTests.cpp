#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMSchedulingTest,"ACE.Plugins.SchedulingParity",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMSchedulingTest::RunTest(const FString&)
{
 auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
 FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
 auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"teleport_sequence":1,"position":{"cell":30998795,"x":30,"y":-34,"z":0},"spells":[],"targets":[],"inventory":[{"id":99,"wcid":99,"count":1,"type":1,"equipped":true,"identified":true,"can_wield":true,"weapon_skill":47,"damage":10,"name":"Sword"},{"id":51,"wcid":51,"name":"Potion","type":32,"identified":true,"usable":true,"count":5,"boost_vital":2,"boost":50}]})"));};
 auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
 // Repeated successful assessments must not accumulate into a combat failure.
 // Actual descriptor/resource changes can require the same weapon/ammo again.
 for(bool Ammunition:{false,true})
 {
  FACEPluginVM VM;TestTrue(TEXT("Appraisal policy loads"),VM.Load(Source,Error));auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"melee","looting":false,"item_mana":false})"));
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  auto Item=S->GetArrayField(TEXT("inventory"))[0]->AsObject();
  if(Ammunition)
  {
   P->SetStringField(TEXT("combat"),TEXT("missile"));
   Item->SetNumberField(TEXT("type"),256);Item->SetNumberField(TEXT("ammo_type"),1);
   auto Ammo=Json(TEXT(R"({"id":100,"wcid":100,"name":"Greater Deadly Blunt Atlatl Dart","count":100,"type":256,"slots":8388608,"ammo_type":1,"equipped":true,"identified":true,"can_wield":true,"damage":30,"damage_type":2})"));
   auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(Ammo));S->SetArrayField(TEXT("inventory"),Inventory);Item=Ammo;
  }
  for(int Cycle=0;Cycle<6;++Cycle)
  {
   S->SetNumberField(TEXT("time"),100.+Cycle*40);Item->SetBoolField(TEXT("identified"),false);
   auto I=Step(VM,S,P);
   TestEqual(TEXT("A new appraisal cycle still requests the needed item"),I->GetStringField(TEXT("action")),FString(TEXT("identify")));
   TestEqual(TEXT("Appraisal is for the selected equipment or its ammunition"),I->GetNumberField(TEXT("item")),Item->GetNumberField(TEXT("id")));
   Item->SetBoolField(TEXT("identified"),true);S->SetNumberField(TEXT("time"),100.5+Cycle*40);
   TestEqual(TEXT("Successful appraisal resumes combat without a five-second pause"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
  }
  // Genuinely unanswered assessments remain bounded; no invented item stats.
  Item->SetBoolField(TEXT("identified"),false);
  for(int Attempt=0;Attempt<3;++Attempt)
  {
   S->SetNumberField(TEXT("time"),400.+Attempt*31);
   TestEqual(TEXT("Consecutive missing replies allow only three requests"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
  }
  S->SetNumberField(TEXT("time"),494);
  TestEqual(TEXT("Missing equipment data still fails safely"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("activity_failed")));
 }
 for(bool IdlePeace:{false,true})for(bool LootFirst:{false,true})for(bool Open:{false,true})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","looting":true,"loot_rules":[{"action":"keep"}]})"));
  P->SetBoolField(TEXT("idle_peace"),IdlePeace);P->SetBoolField(TEXT("loot_priority"),LootFirst);
  S->SetNumberField(TEXT("combat_mode"),8);
  S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":700,"name":"Corpse","distance":1,"identified":true})")))});
  if(Open){S->SetNumberField(TEXT("container"),700);S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":701,"wcid":701,"name":"Scroll","count":1,"identified":true})")))});}
  auto I=Step(VM,S,P);
  TestEqual(TEXT("Idle peace precedes corpse approach and pickup only when enabled"),I->GetStringField(TEXT("action")),FString(IdlePeace?TEXT("combat_mode"):Open?TEXT("loot"):TEXT("open_corpse")));
  if(IdlePeace)
  {
   TestEqual(TEXT("Clear area requests peace"),I->GetNumberField(TEXT("mode")),1.);
   S->SetNumberField(TEXT("combat_mode"),1);
   TestEqual(TEXT("Confirmed peace continues looting"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(Open?TEXT("loot"):TEXT("open_corpse")));
  }
 }
 for(bool CombatEnabled:{false,true})for(bool Ignored:{false,true})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","looting":true,"idle_peace":true,"loot_priority":true,"loot_rules":[{"action":"keep"}]})"));
  if(!CombatEnabled)P->SetStringField(TEXT("combat"),TEXT("off"));
  if(Ignored)P->SetArrayField(TEXT("ignore_monsters"),{MakeShared<FJsonValueString>(TEXT("Rat"))});
  S->SetNumberField(TEXT("combat_mode"),8);
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":700,"name":"Corpse","distance":1,"identified":true})")))});
  TestEqual(TEXT("Nearby monster keeps combat stance during priority/manual-combat looting"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","looting":true,"idle_peace":true})"));
  S->SetNumberField(TEXT("combat_mode"),8);S->SetNumberField(TEXT("health"),10);
  TestEqual(TEXT("Healing remains ahead of idle peace and looting"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
 }
 // Full inventory suspends corpse pickup until inventory changes, without
 // periodic failed transfers or suppressing combat, recovery and navigation.
 {
  FACEPluginVM VM;TestTrue(TEXT("Capacity policy loads"),VM.Load(Source,Error));auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_priority":true,"loot_rules":[{"action":"keep"}]})"));
  S->SetNumberField(TEXT("inventory_revision"),1);S->SetNumberField(TEXT("pack_slots"),0);
  S->SetNumberField(TEXT("container"),700);
  S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":701,"wcid":701,"name":"Scroll","count":1,"identified":true})")))});
  TestEqual(TEXT("Policy asks host for matching loot"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("loot")));
  P->SetObjectField(TEXT("ucm_activity_failure"),Json(TEXT(R"({"activity":"loot","inventory_full":true,"status":"No pack has space"})")));
  TestTrue(TEXT("Capacity reports a clear pause"),Step(VM,S,P)->GetStringField(TEXT("status")).Contains(TEXT("inventory full")));
  P->RemoveField(TEXT("ucm_activity_failure"));S->SetNumberField(TEXT("container"),0);S->SetArrayField(TEXT("contents"),{});
  S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":700,"name":"Corpse","distance":1,"identified":true})")))});
  for(int32 Time:{101,131,10000})
  {
   S->SetNumberField(TEXT("time"),Time);const auto I=Step(VM,S,P);
   TestFalse(TEXT("Full packs do not reopen corpses on a retry timer"),I->HasField(TEXT("action")));
   TestTrue(TEXT("Full inventory remains visible while otherwise idle"),I->GetStringField(TEXT("status")).Contains(TEXT("inventory full")));
  }
  P->SetStringField(TEXT("combat"),TEXT("melee"));
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  TestEqual(TEXT("Combat runs even with loot priority and every pack full"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
  P->SetBoolField(TEXT("recovery"),true);S->SetNumberField(TEXT("health"),10);
  TestEqual(TEXT("Full inventory never blocks healing"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("action_serial"),1);S->SetArrayField(TEXT("targets"),{});P->SetStringField(TEXT("combat"),TEXT("off"));
  Step(VM,S,P); // Release the old combat target.
  P->SetBoolField(TEXT("navigation"),true);
  P->SetArrayField(TEXT("route"),Json(TEXT(R"({"v":[{"cell":30998795,"x":40,"y":-34,"z":0}]})"))->GetArrayField(TEXT("v")));
  TestEqual(TEXT("Route travel continues with looting paused"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("move")));
  P->SetBoolField(TEXT("navigation"),false);S->SetNumberField(TEXT("pack_slots"),1);S->SetNumberField(TEXT("inventory_revision"),2);
  TestEqual(TEXT("Space in any pack resumes looting without restarting UCM"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("open_corpse")));
 }
 // Ordinary NAV use waits are cooperative too. Once native use is idle,
 // recovery and combat keep running without their receipts advancing the route.
 for(bool Boost:{false,true}) for(int32 Outcome:{0,1,2})
 {
  FACEPluginVM VM;TestTrue(TEXT("Use waypoint policy loads"),VM.Load(Source,Error));auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","navigation":true,"route":[{"kind":"use","object_id":800,"cell":30998795,"x":30,"y":-34,"z":0},{"cell":30998795,"x":45,"y":-34,"z":0}]})"));
  P->SetBoolField(TEXT("nav_priority"),Boost);S->SetArrayField(TEXT("action_events"),{});
  S->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":800,"name":"Lever","distance":0})")))});
  TestEqual(TEXT("Use waypoint sends the recorded item once"),Step(VM,S,P)->GetNumberField(TEXT("item")),800.);
  S->SetNumberField(TEXT("health"),10);S->SetNumberField(TEXT("time"),101);S->SetBoolField(TEXT("busy"),true);
  TestFalse(TEXT("Use waypoint retains the native busy gate"),Step(VM,S,P)->HasField(TEXT("action")));
  S->SetBoolField(TEXT("busy"),false);
  TestEqual(TEXT("Waiting use waypoint allows health recovery"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),1);
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"type":"use","source":51,"action_serial":1,"requested_at":101,"error":0})")))});
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  TestEqual(TEXT("Waiting use waypoint allows combat regardless of navigation priority"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
  S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
  auto Old=Json(TEXT(R"({"type":"use","source":800,"action_serial":2,"requested_at":99,"error":1})"));
  S->SetArrayField(TEXT("action_events"),{MakeShared<FJsonValueObject>(Old)});
  TestEqual(TEXT("Neither unrelated nor stale receipt advances/rejects the waypoint"),Step(VM,S,P)->GetStringField(TEXT("status")),FString(TEXT("Waiting for route action")));
  if(Outcome<2){Old->SetNumberField(TEXT("requested_at"),100);Old->SetNumberField(TEXT("error"),Outcome);}
  else S->SetNumberField(TEXT("time"),131);
  const auto Done=Step(VM,S,P);
  TestEqual(TEXT("Only matching success resumes walking; error/timeout pause navigation"),Done->GetStringField(TEXT("action")),FString(Outcome==0?TEXT("move"):TEXT("pause_navigation")));
 }
 for(bool Recall:{false,true}) for(bool Boost:{false,true}) for(bool Arrives:{false,true})
 {
  FACEPluginVM VM;TestTrue(TEXT("Portal scheduling policy loads"),VM.Load(Source,Error));auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","navigation":true,"route":[{"kind":"portal","object_id":800,"cell":30998795,"x":30,"y":-34,"z":0},{"cell":30998795,"x":130,"y":-34,"z":0},{"cell":30998795,"x":145,"y":-34,"z":0}]})"));
  P->SetBoolField(TEXT("nav_priority"),Boost);
  S->SetArrayField(TEXT("route_objects"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":800,"name":"Portal","distance":0})")))});
  if(Recall)
  {
   auto V=P->GetArrayField(TEXT("route"))[0]->AsObject();V->SetStringField(TEXT("kind"),TEXT("recall"));V->SetNumberField(TEXT("spell"),900);
   S->SetArrayField(TEXT("spells"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":900,"name":"Recall","category":100,"power":50,"skill":300,"known":true})")))});
   auto Items=S->GetArrayField(TEXT("inventory"));Items.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":98,"name":"Orb","type":32768,"equipped":true,"identified":true,"can_wield":true})"))));S->SetArrayField(TEXT("inventory"),Items);
  }
  TestEqual(TEXT("Route action is submitted once"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(Recall?TEXT("cast"):TEXT("use_world")));
  S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("health"),10);S->SetBoolField(TEXT("busy"),true);
  TestFalse(TEXT("An actual game action remains locked"),Step(VM,S,P)->HasField(TEXT("action")));
  S->SetBoolField(TEXT("busy"),false);
  TestEqual(TEXT("Pending portal or recall cannot starve healing"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("time"),102);S->SetNumberField(TEXT("action_serial"),1);
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  TestEqual(TEXT("Pending teleport yields to combat even with navigation priority"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
  S->SetArrayField(TEXT("targets"),{});Step(VM,S,P); // Cancel completed attack.
  auto I=Step(VM,S,P);
  TestFalse(TEXT("Unrelated UseDone never advances or reissues the portal"),I->HasField(TEXT("action")));
  TestEqual(TEXT("Waiting portal remains visible in status"),I->GetStringField(TEXT("status")),FString(TEXT("Waiting for route action")));
  if(Arrives)
  {
   S->SetNumberField(TEXT("teleport_sequence"),2);S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),130);
   Step(VM,S,P);I=Step(VM,S,P);
   TestEqual(TEXT("Confirmed arrival advances to the next walking segment"),I->GetNumberField(TEXT("x")),145.);
  }
  else
  {
   S->SetNumberField(TEXT("time"),146);
   TestEqual(TEXT("Missing portal pauses only navigation"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("pause_navigation")));
   P->SetBoolField(TEXT("navigation"),false);S->SetNumberField(TEXT("health"),10);
   TestEqual(TEXT("Recovery remains active after portal failure"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  }
 }
 for(bool Boost:{false,true}) for(bool Legacy:{false,true})
 {
  FACEPluginVM VM;TestTrue(TEXT("UCM loads"),VM.Load(Source,Error));auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","navigation":true,"route":[{"kind":"pause","seconds":20,"cell":30998795,"x":30,"y":-34,"z":0},{"cell":30998795,"x":45,"y":-34,"z":0}]})"));
  P->SetBoolField(TEXT("nav_priority"),Boost);
  P->GetArrayField(TEXT("route"))[0]->AsObject()->SetBoolField(TEXT("legacy"),Legacy);
  TestEqual(TEXT("Pause starts at native or imported waypoint"),Step(VM,S,P)->GetStringField(TEXT("status")),FString(TEXT("Pausing on route")));
  S->SetNumberField(TEXT("time"),101);S->SetNumberField(TEXT("health"),10);
  TestEqual(TEXT("Route pause never prevents emergency healing"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("time"),102);
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  TestEqual(TEXT("Combat proceeds during pause even with navigation priority"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
  S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),34);Step(VM,S,P);
  S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),-38);Step(VM,S,P);
  S->SetArrayField(TEXT("targets"),{});Step(VM,S,P); // Cancel completed attack.
  TestFalse(TEXT("Navigation still respects remaining pause duration"),Step(VM,S,P)->HasField(TEXT("action")));
  S->SetNumberField(TEXT("time"),121);
  auto I=Step(VM,S,P);
  TestEqual(TEXT("Pause completion remains an ordered route boundary"),I->GetStringField(TEXT("status")),FString(TEXT("Route pause complete")));
  I=Step(VM,S,P);
  TestEqual(TEXT("Resume retraces combat detour rather than cutting through walls"),I->GetStringField(TEXT("status")),FString(TEXT("Returning to route")));
  TestEqual(TEXT("Detour returns to observed corner"),I->GetNumberField(TEXT("x")),34.);
  TestEqual(TEXT("Detour preserves corner depth"),I->GetNumberField(TEXT("y")),-34.);
  S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),-34);Step(VM,S,P);
  S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("x"),30);I=Step(VM,S,P);
  TestEqual(TEXT("Timer advances exactly one waypoint after combat"),I->GetNumberField(TEXT("x")),45.);
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":true,"combat":"melee","looting":true,"loot_rules":[{"material":59,"min_workmanship":7,"action":"keep"},{"action":"skip"}]})"));
  S->SetNumberField(TEXT("container"),700);S->SetBoolField(TEXT("container_is_corpse"),true);
  TArray<TSharedPtr<FJsonValue>> Items;
  for(int Id=701;Id<=710;++Id)
  {
   auto Item=Json(TEXT(R"({"name":"Ring","count":1,"type":8,"material":59,"identified":false})"));Item->SetNumberField(TEXT("id"),Id);Item->SetNumberField(TEXT("wcid"),Id);Items.Add(MakeShared<FJsonValueObject>(Item));
  }
  S->SetArrayField(TEXT("contents"),Items);
  for(int Id=701;Id<=708;++Id)
  {
   auto I=Step(VM,S,P);TestEqual(TEXT("Appraisals advance without waiting for first reply"),I->GetNumberField(TEXT("item")),double(Id));
   TestEqual(TEXT("Unresolved first rule requests appraisal, never falls through to Skip"),I->GetStringField(TEXT("action")),FString(TEXT("identify")));
  }
  auto I=Step(VM,S,P);TestFalse(TEXT("At most eight unresolved appraisals; no premature corpse close"),I->HasField(TEXT("action")));
  TestEqual(TEXT("Outstanding appraisals are visible to user"),I->GetStringField(TEXT("status")),FString(TEXT("Waiting for loot appraisal")));
  // Replies can arrive out of order. A completed item should be collected now.
  Items[2]->AsObject()->SetBoolField(TEXT("identified"),true);Items[2]->AsObject()->SetNumberField(TEXT("workmanship"),8);
  I=Step(VM,S,P);TestEqual(TEXT("Out-of-order matching appraisal loots immediately"),I->GetNumberField(TEXT("item")),703.);
  TestEqual(TEXT("Matching item is transferred"),I->GetStringField(TEXT("action")),FString(TEXT("loot")));
  auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(Items[2]);S->SetArrayField(TEXT("inventory"),Inventory);Items.RemoveAt(2);S->SetArrayField(TEXT("contents"),Items);
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":500,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  TestEqual(TEXT("Pending appraisal/receipt does not outrank combat"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("attack")));
  S->SetNumberField(TEXT("health"),10);
  TestEqual(TEXT("Appraisal queue never prevents healing"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
  for(auto& Item:Items){Item->AsObject()->SetBoolField(TEXT("identified"),true);Item->AsObject()->SetNumberField(TEXT("material"),1);}
  TestEqual(TEXT("Corpse closes only after all rules are resolved"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("close_corpse")));
 }
 for(bool Reply:{false,true})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot();
  auto P=Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":true,"loot_rules":[{"material":59,"min_workmanship":7,"action":"keep"}]})"));
  auto Item=Json(TEXT(R"({"id":701,"wcid":701,"count":1,"name":"Ring","type":8,"material":59,"identified":false})"));
  S->SetNumberField(TEXT("container"),700);S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Item)});
  for(int Attempt=0;Attempt<3;++Attempt)
  {
   S->SetNumberField(TEXT("time"),100+Attempt*10);
   TestEqual(TEXT("Missing appraisal is retried within bounded policy"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(TEXT("identify")));
   S->SetNumberField(TEXT("time"),101+Attempt*10);
   TestFalse(TEXT("Every attempt, including the last, gets time to receive its reply"),Step(VM,S,P)->HasField(TEXT("action")));
  }
  S->SetNumberField(TEXT("time"),Reply?129:130);
  if(Reply){Item->SetBoolField(TEXT("identified"),true);Item->SetNumberField(TEXT("workmanship"),8);}
  TestEqual(TEXT("Final reply is usable; exhausted requests pause only looting"),Step(VM,S,P)->GetStringField(TEXT("action")),FString(Reply?TEXT("loot"):TEXT("activity_failed")));
 }
 return !HasAnyErrors();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMPostCombatLootTest,"ACE.Plugins.PostCombatLootHandoff",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMPostCombatLootTest::RunTest(const FString&)
{
 auto Json=[](const TCHAR* Text){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),O);return O;};
 FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
 auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"action_serial":0,"action_error":0,"container":0,"teleport_sequence":1,"position":{"cell":30998795,"x":30,"y":0,"z":0},"spells":[],"targets":[],"inventory":[{"id":99,"wcid":99,"type":256,"equipped":true,"identified":true,"can_wield":true,"damage":10,"name":"Thrown weapon"},{"id":51,"wcid":51,"name":"Potion","type":32,"identified":true,"usable":true,"count":5,"boost_vital":2,"boost":50}]})"));};
 auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"missile","approach_range":60,"radius":1,"navigation":true,"looting":true,"loot_rules":[{"action":"keep"}],"route":[{"cell":30998795,"x":25,"y":0,"z":0},{"cell":30998795,"x":45,"y":0,"z":0}]})"));};
 auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
 auto Action=[](auto I){FString A;I->TryGetStringField(TEXT("action"),A);return A;};
 auto Kill=[&](FACEPluginVM& VM,auto S,auto P,bool Confirmed=true)
 {
  TestTrue(TEXT("Post-combat policy loads"),VM.Load(Source,Error));
  S->SetObjectField(TEXT("route_visible"),Json(TEXT(R"({"1":false,"2":true})")));
  TestEqual(TEXT("Start along ordered route"),Action(Step(VM,S,P)),FString(TEXT("move")));
  auto Enemy=Json(TEXT(R"({"id":500,"name":"Rat","distance":6,"identified":true,"line_of_sight":true,"cell":30998795,"x":30,"y":6,"z":0})"));
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Enemy)});
  TestEqual(TEXT("Leave route to approach enemy"),Step(VM,S,P)->GetNumberField(TEXT("target")),500.);
  S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),5);Enemy->SetNumberField(TEXT("distance"),1);
  S->SetNumberField(TEXT("time"),100.5);
  TestEqual(TEXT("Attack after approaching"),Action(Step(VM,S,P)),FString(TEXT("attack")));
  S->SetArrayField(TEXT("targets"),{});
  if(Confirmed)S->SetObjectField(TEXT("combat_death"),Json(TEXT(R"({"id":500,"time":100.5,"teleport_sequence":1,"cell":30998795,"x":30,"y":6,"z":0})")));
  TestEqual(TEXT("Cancel old attack before other activities"),Action(Step(VM,S,P)),FString(TEXT("cancel_attack")));
 };
 for(bool NavigationPriority:{false,true})
 {
  FACEPluginVM VM;auto S=Snapshot(),P=Profile();Kill(VM,S,P);
  P->SetBoolField(TEXT("nav_priority"),NavigationPriority);
  // An already-looted old corpse must not be mistaken for this kill's spawn.
  S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":600,"name":"Old corpse","distance":1,"identified":true,"looted":true,"observed_age":50})")))});
  for(double Time:{100.5,101.,103.})
  {
   S->SetNumberField(TEXT("time"),Time);auto I=Step(VM,S,P);
   TestEqual(TEXT("Only navigation priority bypasses the delayed corpse handoff"),Action(I),FString(NavigationPriority?TEXT("move"):TEXT("")));
   if(!NavigationPriority)TestEqual(TEXT("Pending corpse wait is visible"),I->GetStringField(TEXT("status")),FString(TEXT("Waiting for defeated creature's corpse")));
  }
  auto Corpse=Json(TEXT(R"({"id":700,"name":"Corpse of Rat","distance":1,"identified":true,"line_of_sight":true,"observed_age":0})"));
  S->SetArrayField(TEXT("corpses"),{MakeShared<FJsonValueObject>(Corpse)});
  TestEqual(TEXT("Corpse arrival starts loot immediately, before the wait deadline"),Action(Step(VM,S,P)),FString(TEXT("open_corpse")));
  TestEqual(TEXT("Route cannot interrupt pending corpse open"),Action(Step(VM,S,P)),FString());
  S->SetNumberField(TEXT("container"),700);S->SetBoolField(TEXT("container_is_corpse"),true);S->SetBoolField(TEXT("contents_ready"),false);
  TestEqual(TEXT("Route waits for delayed contents"),Action(Step(VM,S,P)),FString());
  S->SetBoolField(TEXT("contents_ready"),true);
  auto Ring=Json(TEXT(R"({"id":701,"wcid":701,"name":"Ring","count":1,"identified":true})"));
  S->SetArrayField(TEXT("contents"),{MakeShared<FJsonValueObject>(Ring)});
  TestEqual(TEXT("Take the loot before returning to the route"),Action(Step(VM,S,P)),FString(TEXT("loot")));
  TestEqual(TEXT("Route cannot interrupt transfer confirmation"),Action(Step(VM,S,P)),FString());
  auto Inventory=S->GetArrayField(TEXT("inventory"));Inventory.Add(MakeShared<FJsonValueObject>(Ring));S->SetArrayField(TEXT("inventory"),Inventory);
  S->SetArrayField(TEXT("contents"),{});
  TestEqual(TEXT("Close completed corpse before route rejoin"),Action(Step(VM,S,P)),FString(TEXT("close_corpse")));
  S->SetNumberField(TEXT("container"),0);auto I=Step(VM,S,P);
  TestEqual(TEXT("Resume return trail immediately after loot, without remaining spawn delay"),I->GetStringField(TEXT("status")),FString(TEXT("Returning to route")));
  TestEqual(TEXT("Rejoin follows the observed departure position"),I->GetNumberField(TEXT("y")),0.);
 }
 {
  FACEPluginVM VM;auto S=Snapshot(),P=Profile();Kill(VM,S,P);
  P->SetBoolField(TEXT("recovery"),true);S->SetNumberField(TEXT("health"),10);
  TestEqual(TEXT("Recovery still runs while awaiting a corpse"),Step(VM,S,P)->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);S->SetNumberField(TEXT("action_serial"),1);
  S->SetArrayField(TEXT("targets"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":501,"name":"Rat","distance":1,"identified":true,"line_of_sight":true})")))});
  TestEqual(TEXT("Another living target still takes combat priority"),Action(Step(VM,S,P)),FString(TEXT("attack")));
  S->SetArrayField(TEXT("targets"),{});Step(VM,S,P);
  S->SetNumberField(TEXT("time"),109);
  TestEqual(TEXT("Missing corpse cannot stall navigation indefinitely"),Action(Step(VM,S,P)),FString(TEXT("move")));
 }
 for(int32 Case=0;Case<5;++Case)
 {
  FACEPluginVM VM;auto S=Snapshot(),P=Profile();Kill(VM,S,P,Case!=0);
  if(Case==1)P->SetBoolField(TEXT("looting"),false);
  if(Case==2)P->SetArrayField(TEXT("loot_rules"),{});
  if(Case==3)S->GetObjectField(TEXT("position"))->SetNumberField(TEXT("y"),100);
  if(Case==4)S->SetNumberField(TEXT("teleport_sequence"),2);
  const auto I=Step(VM,S,P);
  TestEqual(*FString::Printf(TEXT("Excluded corpse-wait case %d resumes route: %s"),Case,*I->GetStringField(TEXT("status"))),Action(I),FString(TEXT("move")));
 }
 return !HasAnyErrors();
}
#endif
