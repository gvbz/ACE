#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMAutoServicesTest,"ACE.Plugins.AutomaticFellowshipAndCrafting",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMAutoServicesTest::RunTest(const FString&)
{
 auto Json=[](const TCHAR* T){TSharedPtr<FJsonObject> O;FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(T),O);return O;};
 FString Source,Error;FFileHelper::LoadFileToString(Source,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua")));
 auto Snapshot=[&](){return Json(TEXT(R"({"time":100,"player":1,"health":100,"max_health":100,"stamina":100,"max_stamina":100,"mana":100,"max_mana":100,"ready":true,"busy":false,"position":{"cell":30998795,"x":0,"y":0,"z":0},"spells":[],"targets":[],"route_objects":[{"id":3,"name":"Visitor","object_class":24,"distance":9}],"inventory":[{"id":51,"name":"Potion","type":32,"identified":true,"usable":true,"count":5,"boost_vital":2,"boost":50}],"fellowship":{"valid":true,"name":"Party","leader":1,"open":true,"locked":false,"members":[{"id":1,"name":"Self"},{"id":2,"name":"Ally"}]}})"));};
 auto Profile=[&](){return Json(TEXT(R"({"buffing":false,"recovery":false,"combat":"off","looting":false,"item_mana":false,"navigation":false,"auto_fellowship":true,"auto_craft":true})"));};
 auto Step=[&](FACEPluginVM& VM,auto S,auto P){TSharedPtr<FJsonObject> I;TestTrue(*Error,VM.Step(S,P,I,Error));return I?I:MakeShared<FJsonObject>();};
 auto Str=[](auto I,const TCHAR* K){FString V;I->TryGetStringField(K,V);return V;};
 uint32 Serial=0;
 auto Tell=[&](auto S,int32 Id,const TCHAR* Name,const TCHAR* Text,bool Local=false)
 {auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("serial"),++Serial);E->SetNumberField(TEXT("time"),S->GetNumberField(TEXT("time")));E->SetNumberField(TEXT("id"),Id);E->SetStringField(TEXT("name"),Name);E->SetStringField(TEXT("text"),Text);E->SetBoolField(TEXT("local"),Local);S->SetArrayField(TEXT("fellowship_tells"),{MakeShared<FJsonValueObject>(E)});};
 {
  FACEPluginVM VM;TestTrue(TEXT("Policy loads"),VM.Load(Source,Error));auto S=Snapshot(),P=Profile();
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));auto I=Step(VM,S,P);
  TestEqual(TEXT("Recruitment reply is a private tell"),Str(I,TEXT("recipient")),FString(TEXT("Visitor")));
  S->GetArrayField(TEXT("route_objects"))[0]->AsObject()->SetNumberField(TEXT("distance"),11);
  TestTrue(TEXT("Far player is never invited"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("fellowship"));
  S->GetArrayField(TEXT("route_objects"))[0]->AsObject()->SetNumberField(TEXT("distance"),9);
  I=Step(VM,S,P);TestEqual(TEXT("Nearby applicant invited"),Str(I,TEXT("operation")),FString(TEXT("recruit")));
  TestEqual(TEXT("Manager has independent failure domain"),Str(I,TEXT("activity")),FString(TEXT("fellowship")));
  TestTrue(TEXT("Unconfirmed invitation is not resent"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("fellowship"));
  S->SetNumberField(TEXT("time"),131);I=Step(VM,S,P);TestTrue(TEXT("Invitation timeout tells applicant"),Str(I,TEXT("text")).Contains(TEXT("not confirmed")));
  S->SetNumberField(TEXT("time"),170);TestTrue(TEXT("Timeout does not requeue applicant"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("fellowship"));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetNumberField(TEXT("fellowship_capacity"),2);
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));Step(VM,S,P);
  TestEqual(TEXT("Full fellowship closes for waiting list"),Str(Step(VM,S,P),TEXT("operation")),FString(TEXT("close")));
  const auto F=S->GetObjectField(TEXT("fellowship"));F->SetBoolField(TEXT("open"),false);F->SetArrayField(TEXT("members"),{F->GetArrayField(TEXT("members"))[0]});
  auto I=Step(VM,S,P);TestTrue(TEXT("Free spot reserved for five minutes"),Str(I,TEXT("text")).Contains(TEXT("5 minutes")));
  TestTrue(TEXT("Reserved player must confirm readiness"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("fellowship"));
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));Step(VM,S,P);
  TestEqual(TEXT("New xp claims reserved spot"),Str(Step(VM,S,P),TEXT("operation")),FString(TEXT("recruit")));
  auto Members=F->GetArrayField(TEXT("members"));Members.Add(MakeShared<FJsonValueObject>(Json(TEXT(R"({"id":3,"name":"Visitor"})"))));F->SetArrayField(TEXT("members"),Members);
  TestEqual(TEXT("Confirmed member releases reservation and reopens"),Str(Step(VM,S,P),TEXT("operation")),FString(TEXT("open")));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetNumberField(TEXT("fellowship_capacity"),2);
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));Step(VM,S,P);Step(VM,S,P);auto F=S->GetObjectField(TEXT("fellowship"));F->SetBoolField(TEXT("open"),false);F->SetArrayField(TEXT("members"),{F->GetArrayField(TEXT("members"))[0]});Step(VM,S,P);
  S->SetNumberField(TEXT("time"),401);TestTrue(TEXT("Unused reservation expires"),Str(Step(VM,S,P),TEXT("text")).Contains(TEXT("expired")));
  TestEqual(TEXT("Empty queue reopens fellowship"),Str(Step(VM,S,P),TEXT("operation")),FString(TEXT("open")));
 }
 for(bool Departed:{false,true})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  Tell(S,3,TEXT("Visitor"),TEXT("startvote setopen false"));TestTrue(TEXT("Outsider cannot start votes"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("say"));
  Tell(S,2,TEXT("Ally"),TEXT("startvote setopen false"));TestTrue(TEXT("Member starts a numbered vote"),Str(Step(VM,S,P),TEXT("text")).Contains(TEXT("vote 1")));
  if(Departed){auto F=S->GetObjectField(TEXT("fellowship"));F->SetArrayField(TEXT("members"),{F->GetArrayField(TEXT("members"))[0]});}
  S->SetNumberField(TEXT("time"),221);TestTrue(TEXT("Vote expires at two minutes"),Str(Step(VM,S,P),TEXT("text")).Contains(Departed?TEXT("failed"):TEXT("passed")));
  auto I=Step(VM,S,P);TestEqual(TEXT("Only remaining members' ballots count"),Str(I,TEXT("operation")),Departed?FString():FString(TEXT("close")));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();Tell(S,2,TEXT("Ally"),TEXT("startvote giveleader Ally"));Step(VM,S,P);
  Tell(S,1,TEXT("Self"),TEXT("vote 1 abort"),true);TestTrue(TEXT("Local leader can abort vote"),Str(Step(VM,S,P),TEXT("text")).Contains(TEXT("cancelled")));
  Tell(S,2,TEXT("Ally"),TEXT("startvote setopen false"));TestTrue(TEXT("Proposal cooldown survives abort"),Str(Step(VM,S,P),TEXT("text")).Contains(TEXT("four minutes")));
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));Step(VM,S,P);S->GetObjectField(TEXT("fellowship"))->SetNumberField(TEXT("leader"),2);
  TestTrue(TEXT("Losing leader clears queued recruitment"),Str(Step(VM,S,P),TEXT("text")).Contains(TEXT("leadership changed")));
  S->SetNumberField(TEXT("time"),500);TestTrue(TEXT("No stale vote executes after leadership change"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("fellowship"));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();
  for(int32 N=0;N<12;++N){Tell(S,3,TEXT("Visitor"),TEXT("ordinary conversation"));Step(VM,S,P);}
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));TestEqual(TEXT("Ordinary chat cannot exhaust command allowance"),Str(Step(VM,S,P),TEXT("recipient")),FString(TEXT("Visitor")));
 }
 for(const TCHAR* Phase:{TEXT("critical"),TEXT("normal"),TEXT("idle")})
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();auto Craft=Json(TEXT(R"({"pending":false,"candidates":[{"recipe":"test","name":"Supply","phase":"critical"}]})"));
  Craft->GetArrayField(TEXT("candidates"))[0]->AsObject()->SetStringField(TEXT("phase"),Phase);S->SetObjectField(TEXT("crafting"),Craft);
  auto I=Step(VM,S,P);TestEqual(TEXT("Each replenishment tier reaches crafting"),Str(I,TEXT("action")),FString(TEXT("craft")));
  TestEqual(TEXT("Crafting failure domain is independent"),Str(I,TEXT("activity")),FString(TEXT("crafting")));
  P->SetBoolField(TEXT("auto_craft"),false);TestTrue(TEXT("Disable stops planning"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("craft"));
 }
 {
  FACEPluginVM VM;VM.Load(Source,Error);auto S=Snapshot(),P=Profile();P->SetBoolField(TEXT("recovery"),true);S->SetNumberField(TEXT("health"),10);S->SetObjectField(TEXT("crafting"),Json(TEXT(R"({"pending":true,"name":"Supply"})")));
  Tell(S,3,TEXT("Visitor"),TEXT("xp"));auto I=Step(VM,S,P);TestEqual(TEXT("Emergency recovery outranks active craft and recruitment"),I->GetNumberField(TEXT("item")),51.);
  S->SetNumberField(TEXT("health"),100);TestEqual(TEXT("Craft resumes after recovery"),Str(Step(VM,S,P),TEXT("action")),FString(TEXT("craft")));
  P->SetObjectField(TEXT("ucm_activity_failure"),Json(TEXT(R"({"activity":"crafting","status":"Rejected"})")));Step(VM,S,P);P->RemoveField(TEXT("ucm_activity_failure"));
  TestTrue(TEXT("Craft rejection allows other activities"),Str(Step(VM,S,P),TEXT("action"))!=TEXT("craft"));
 }
 return true;
}
#endif
