#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Mods/ACEPluginVM.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACEPluginDesktop.h"
#include "Mods/ACEPluginOrder.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginCastMotion.h"
#include "Mods/ACEVTProfile.h"
#include "Mods/ACEUCMProfiles.h"
#include "ACEOpcodes.h"
#include "ACERuntimeOptions.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUIResourceResolver.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "Slate/WidgetRenderer.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"

namespace
{
    TSharedPtr<FJsonObject> Json(const TCHAR* Text)
    { TSharedPtr<FJsonObject> O; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O); return O; }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginVMTest,"ACE.Plugins.Runtime",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginVMTest::RunTest(const FString&)
{
    FString Error; auto Empty=MakeShared<FJsonObject>();TSharedPtr<FJsonObject> Intent;
    FACEPluginVM VM;
    TestTrue(TEXT("Source plugin loads"),VM.Load(TEXT("local n=0; return function(s,p) n=n+1; return {status=tostring(n), action=p.action, spell=s.spell} end"),Error));
    TestTrue(TEXT("Snapshot and profile passed to script"),VM.Step(Json(TEXT("{\"spell\":42}")),Json(TEXT("{\"action\":\"cast\"}")),Intent,Error));
    TestEqual(TEXT("Intent spell"),Intent->GetNumberField(TEXT("spell")),42.);
    VM.Step(Empty,Empty,Intent,Error); TestEqual(TEXT("State survives callbacks"),Intent->GetStringField(TEXT("status")),FString(TEXT("2")));
    FACEPluginVM Restricted;
    TestTrue(TEXT("Sandbox has no filesystem, native loader, process, debug or coroutine APIs"),Restricted.Load(TEXT("assert(io==nil and os==nil and package==nil and require==nil and debug==nil and coroutine==nil and load==nil and dofile==nil and pcall==nil); return function() end"),Error));
    FACEPluginVM Literal;
    TestTrue(TEXT("Literal contains supports punctuation without enabling patterns"),Literal.Load(TEXT("assert(string.contains('a.*b','.*') and not string.contains('ab','.*') and string.find==nil); return function() end"),Error));
    FACEPluginVM LongContains;
    TestFalse(TEXT("Native literal search has bounded input"),LongContains.Load(TEXT("string.contains(string.rep('a',4097),'a'); return function() end"),Error));
    FACEPluginVM Infinite;
    TestFalse(TEXT("Initialization loop is interrupted"),Infinite.Load(TEXT("while true do end"),Error));
    FACEPluginVM TickLoop;
    TestTrue(TEXT("Loop fixture loads"),TickLoop.Load(TEXT("return function() while true do end end"),Error));
    TestFalse(TEXT("Callback loop is interrupted"),TickLoop.Step(Empty,Empty,Intent,Error));
    FACEPluginVM Cooperative;
    TestTrue(TEXT("Cooperative checkpoint fixture loads"),Cooperative.Load(TEXT("return function() assert(workavailable()); while workavailable() do end; return {status='yielded'} end"),Error));
    for(int Tick=0;Tick<2;++Tick)
    {
        TestTrue(TEXT("Checkpoint leaves room to return and resets each callback"),Cooperative.Step(Empty,Empty,Intent,Error));
        if(Intent)TestEqual(TEXT("Cooperative callback returns normally"),Intent->GetStringField(TEXT("status")),FString(TEXT("yielded")));
    }
    FACEPluginVM IgnoredCheckpoint;
    IgnoredCheckpoint.Load(TEXT("return function() while true do workavailable() end end"),Error);
    TestFalse(TEXT("Polling checkpoint does not extend hard quota"),IgnoredCheckpoint.Step(Empty,Empty,Intent,Error));
    FACEPluginVM Allocation;
    TestFalse(TEXT("Oversized allocation fails without killing host"),Allocation.Load(TEXT("local s=string.rep('a',64000000); return function() end"),Error));
    FACEPluginVM TickAllocation;
    TestTrue(TEXT("Callback allocation fixture loads"),TickAllocation.Load(TEXT("return function() local s=string.rep('a',64000000) end"),Error));
    TestFalse(TEXT("Callback still has a hard heap limit"),TickAllocation.Step(Empty,Empty,Intent,Error));
    TestTrue(TEXT("Memory errors report callback phase and heap statistics"),Error.Contains(TEXT("during callback"))&&Error.Contains(TEXT("MiB")));
    TestTrue(TEXT("Rejected allocations cannot exceed the cap"),TickAllocation.GetPeakMemoryUsage()<=FACEPluginVM::MemoryLimit);
    FACEPluginVM InputAllocation;
    InputAllocation.Load(TEXT("return function() end"),Error);
    auto Oversized=MakeShared<FJsonObject>();Oversized->SetStringField(TEXT("data"),FString::ChrN(40*1024*1024,TEXT('x')));
    TestFalse(TEXT("Host inputs also retain a hard limit"),InputAllocation.Step(Oversized,Empty,Intent,Error));
    TestTrue(TEXT("Input memory failures identify the input phase"),Error.Contains(TEXT("during snapshot input")));
    TestTrue(TEXT("Failed input leaves no stack references behind"),InputAllocation.Step(Empty,Empty,Intent,Error));
    FACEPluginVM Broken;
    TestFalse(TEXT("Syntax error isolated"),Broken.Load(TEXT("not valid lua"),Error));
    FACEPluginVM BadNumber;
    BadNumber.Load(TEXT("return function() return {spell=0/0} end"),Error);
    TestFalse(TEXT("Nonfinite action rejected"),BadNumber.Step(Empty,Empty,Intent,Error));
    FACEPluginVM ErrorTable; ErrorTable.Load(TEXT("return function() error({}) end"),Error);
    TestFalse(TEXT("Non-string errors cannot crash error reporting"),ErrorTable.Step(Empty,Empty,Intent,Error));
    FACEPluginVM SpellCatalog;
    TestTrue(TEXT("Spell indexing helper loads"),SpellCatalog.Load(TEXT(R"(
        return function()
          local spells={{id=1,power=100,skill=200,scarabs={Scarab=3}},
            {id=2,power=100,skill=200,scarabs={Scarab=4}},
            {id=3,power=100,skill=200,known=false},
            {id=4,power=180,skill=200},{id=5,power=100,skill=200},
            {id=6,power=100,skill=200,components_known=false}}
          local index,supply,usable=spellindex(spells,{{name='Scarab',count=1},{name='Scarab',count=2}},100,30,{[5]=110})
          assert(index[1]==spells[1] and supply[1] and not supply[2] and not supply[6])
          assert(#usable==1 and usable[1]==spells[1])
          local _,new_supply=spellindex(spells,{},100,30,{})
          assert(not new_supply[1])
          local _,exempt,castable=spellindex(spells,{},100,30,{[5]=110},false)
          assert(exempt[1] and exempt[2] and exempt[6] and #castable==3)
          local _,required=spellindex(spells,{},100,30,{},true)
          assert(not required[1] and not required[6])
          return {status='indexed'}
        end
    )"),Error));
    TestTrue(TEXT("Index combines stacks, respects skill/known/cooldown/supplies and shares records"),SpellCatalog.Step(Empty,Empty,Intent,Error));
    FACEPluginVM SpellGroups;
    TestTrue(TEXT("Partitioned spell index fixture loads"),SpellGroups.Load(TEXT(R"(
        return function()
          local spells={
            {id=1,category=1,self_buff=true,beneficial=true,caster_target=true,duration=300,power=100,skill=200},
            {id=2,category=1,self_buff=true,beneficial=true,caster_target=true,duration=300,power=180,skill=200},
            {id=3,category=1,beneficial=true,duration=300,power=100,skill=200},
            {id=4,category=67,beneficial=true,caster_target=true,duration=0,power=100,skill=200},
            {id=5,category=117,power=100,skill=200},
            {id=6,category=117,power=50,skill=200},
            {id=7,category=115,school=3,beneficial=true,duration=300,power=100,skill=200,components_known=false},
            {id=8,category=117,power=150,skill=200,known=false},
            {id=9,category=117,power=140,skill=200}}
          local index,supplied,usable,g=spellindex(spells,{},100,30,{[9]=110})
          assert(#usable==5 and #g.buffs==3 and g.buffs[2]==spells[2] and g.buffs[3]==spells[7])
          assert(#g.instant_self==1 and g.instant_self[1]==spells[4])
          assert(#g.other_buffs==1 and g.other_buffs[1]==spells[3])
          assert(#g.offense==2 and g.offense[1]==spells[5] and g.offense[2]==spells[6])
          assert(#g.by_category[117]==2 and g.by_category[117][2]==spells[6])
          assert(index[8]==spells[8] and not supplied[7] and not g.by_category[115])
          local _,_,_,eligible=spellindex(spells,{},120,0,{},false)
          assert(#eligible.by_category[1]==3 and #eligible.by_category[117]==3)
          assert(#eligible.other_buffs==2 and eligible.other_buffs[2]==spells[7])
          return {status='partitioned'}
        end
    )"),Error));
    TestTrue(TEXT("Partitions preserve record identity, order, lower tiers, eligibility and blocked buff candidates"),SpellGroups.Step(Empty,Empty,Intent,Error));
    FACEPluginVM IndexLoop;
    TestTrue(TEXT("Native index budget fixture loads"),IndexLoop.Load(TEXT("local spells={};for i=1,1000 do spells[i]={id=i} end;return function() for i=1,200 do spellindex(spells,{},0,0,{}) end end"),Error));
    TestFalse(TEXT("Repeated native indexing cannot bypass work quota"),IndexLoop.Step(Empty,Empty,Intent,Error));
    TestTrue(TEXT("Native work guard caused the failure"),Error.Contains(TEXT("spell index work limit")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEUCMTest,"ACE.Plugins.UCM",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEUCMTest::RunTest(const FString&)
{
    FString Script,Error;TestTrue(TEXT("Shipped plugin exists"),FFileHelper::LoadFileToString(Script,*(IPluginManager::Get().FindPlugin(TEXT("ACEClient"))->GetBaseDir()/TEXT("ClientMods/ucm/main.lua"))));
    auto P=Json(TEXT(R"({"initial_state":"Default","states":[{"name":"Default"}],"buffing":true,"buffs":[10],"skill_margin":30,"refresh_seconds":60,"combat":"off"})"));
    auto S=Json(TEXT(R"({"time":100,"health":100,"max_health":100,"mana":100,"max_mana":100,"nearest":0,"busy":false,"position":{"cell":2103705613,"x":25,"y":97,"z":12},"spells":[{"id":10,"category":1,"power":100,"skill":250,"self_buff":true},{"id":11,"category":1,"power":200,"skill":250,"self_buff":true},{"id":12,"category":1,"power":300,"skill":250,"self_buff":true}],"enchantments":[]})"));
    FACEPluginVM VM;TestTrue(TEXT("UCM loads"),VM.Load(Script,Error));TSharedPtr<FJsonObject> I;
    TestTrue(TEXT("UCM tick"),VM.Step(S,P,I,Error));TestEqual(TEXT("Strongest known spell within skill margin"),I->GetNumberField(TEXT("spell")),11.);
    S->SetNumberField(TEXT("time"),102);VM.Step(S,P,I,Error);TestFalse(TEXT("No repeated cast while awaiting enchantment"),I->HasField(TEXT("action")));
    auto E=Json(TEXT(R"({"category":1,"power":200,"remaining":1000})"));S->SetArrayField(TEXT("enchantments"),{MakeShared<FJsonValueObject>(E)});
    S->SetNumberField(TEXT("time"),120);VM.Step(S,P,I,Error);TestFalse(TEXT("Active adequate buff prevents recast"),I->HasField(TEXT("action")));
    S->SetNumberField(TEXT("health"),1);VM.Step(S,P,I,Error);TestFalse(TEXT("Low health alone does not stop UCM"),I->HasField(TEXT("action")));
    S->SetNumberField(TEXT("health"),100);P->SetBoolField(TEXT("buffing"),false);P->SetStringField(TEXT("combat"),TEXT("missile"));P->SetBoolField(TEXT("manual_combat"),true);S->SetNumberField(TEXT("nearest"),123);S->SetNumberField(TEXT("distance"),5);
    VM.Step(S,P,I,Error);TestEqual(TEXT("Missile action"),I->GetNumberField(TEXT("mode")),4.);
    S->SetNumberField(TEXT("distance"),100);VM.Step(S,P,I,Error);TestFalse(TEXT("Targets beyond profile radius not attacked"),I->HasField(TEXT("action")));
    P->SetBoolField(TEXT("navigation"),true);P->SetArrayField(TEXT("route"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"cell":2103705613,"x":27,"y":97,"z":12})")))});
    VM.Step(S,P,I,Error);TestEqual(TEXT("Route requests collision-driven movement"),I->GetStringField(TEXT("action")),FString(TEXT("move")));
    auto Rules=Json(TEXT(R"({"name":"Default","transitions":[{"when":"elapsed","value":10,"next":"Done"}]})"));
    auto Done=Json(TEXT(R"({"name":"Done","buffing":false,"combat":"off","navigation":false})"));
    P->SetArrayField(TEXT("states"),{MakeShared<FJsonValueObject>(Rules),MakeShared<FJsonValueObject>(Done)});
    VM.Step(S,P,I,Error);TestEqual(TEXT("Meta transition"),I->GetStringField(TEXT("status")),FString(TEXT("State: Done")));
    VM.Step(S,P,I,Error);TestEqual(TEXT("Disabling combat cancels the prior missile attack"),I->GetStringField(TEXT("action")),FString(TEXT("cancel_attack")));
    VM.Step(S,P,I,Error);TestFalse(TEXT("State remains idle without repeatedly cancelling"),I->HasField(TEXT("action")));
    // A large spellbook must fit the callback budget without nested scans of
    // every enchantment for every spell. Unknown profile tiers remain references.
    FACEPluginVM Large;Large.Load(Script,Error);
    P=Json(TEXT(R"({"initial_state":"Default","states":[{"name":"Default"}],"buffing":true,"buffs":[9999],"skill_margin":30,"combat":"off"})"));
    TArray<TSharedPtr<FJsonValue>> Many;
    for(int32 Index=1;Index<=2000;++Index)
    {
        auto Spell=MakeShared<FJsonObject>();Spell->SetNumberField(TEXT("id"),Index);Spell->SetNumberField(TEXT("category"),Index%100+1);
        Spell->SetNumberField(TEXT("power"),100+Index%8*25);Spell->SetNumberField(TEXT("skill"),300);Spell->SetBoolField(TEXT("known"),true);Spell->SetBoolField(TEXT("self_buff"),true);
        Many.Add(MakeShared<FJsonValueObject>(Spell));
    }
    auto Reference=Json(TEXT(R"({"id":9999,"category":1,"power":500,"skill":300,"known":false,"self_buff":true})"));
    Many.Add(MakeShared<FJsonValueObject>(Reference));S->SetArrayField(TEXT("spells"),Many);S->SetArrayField(TEXT("enchantments"),{});
    const double Begin=FPlatformTime::Seconds();const bool Worked=Large.Step(S,P,I,Error);
    TestTrue(TEXT("Large spellbook remains within script budget"),Worked);
    if(Worked){TestEqual(TEXT("Higher unknown profile tier uses a known tier"),I->GetStringField(TEXT("action")),FString(TEXT("cast")));TestNotEqual(TEXT("Unknown reference is never cast"),I->GetNumberField(TEXT("spell")),9999.);}
    AddInfo(FString::Printf(TEXT("2,001-spell UCM snapshot and tick: %.2f ms"),(FPlatformTime::Seconds()-Begin)*1000));
    TArray<TSharedPtr<FJsonValue>> FullInventory,Active;
    for(int Index=1;Index<=1024;++Index)
    {
        auto Item=Json(TEXT(R"({"name":"Stored item","type":128,"count":1,"max_stack":1,"identified":true,"can_wield":false})"));
        Item->SetNumberField(TEXT("id"),Index);Item->SetNumberField(TEXT("wcid"),Index);FullInventory.Add(MakeShared<FJsonValueObject>(Item));
    }
    for(int Category=1;Category<=100;++Category)
    {auto Buff=Json(TEXT(R"({"power":1000,"remaining":3600})"));Buff->SetNumberField(TEXT("category"),Category);Active.Add(MakeShared<FJsonValueObject>(Buff));}
    S->SetArrayField(TEXT("inventory"),FullInventory);S->SetArrayField(TEXT("enchantments"),Active);
    FACEPluginVM Full;Full.Load(Script,Error);const double FullBegin=FPlatformTime::Seconds();const bool FullWorked=Full.Step(S,P,I,Error);
    TestTrue(*FString::Printf(TEXT("2,001 spells / 1,024 items fit memory and instruction limits: %s"),*Error),FullWorked);
    if(FullWorked)TestFalse(TEXT("Fully buffed large inventory creates no redundant action"),I->HasField(TEXT("action")));
    AddInfo(FString::Printf(TEXT("Full spellbook/inventory decision: %.2f ms"),(FPlatformTime::Seconds()-FullBegin)*1000));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEPluginHostTest,"ACE.Plugins.HostAndPanel",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACEPluginHostTest::RunTest(const FString&)
{
    const float PreviousBounds=ACERuntimeOptions::Get(TEXT("ConstrainUIWindows"));
    ON_SCOPE_EXIT{ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),PreviousBounds);};
    ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),1);
    auto* GI=NewObject<UGameInstance>(); GI->Init();
    auto* H=GI->GetSubsystem<UACEPluginSubsystem>();
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if(FApp::CanEverRender()&&FPaths::DirectoryExists(DatDirectory))
    {
        // The login Plugins tab can be built before background DAT loading finishes.
        // Keep that exact widget alive: opening another plugin used to hide the bug.
        auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
        TestFalse(TEXT("Initial plugin panel starts before DAT artwork is ready"),Dat->IsDatReady());
        const auto FirstPlugin=H->Plugins[0];const bool WasEnabled=FirstPlugin->Enabled;FirstPlugin->Enabled=true;
        ON_SCOPE_EXIT{FirstPlugin->Enabled=WasEnabled;};
        auto Panel=H->MakePanel();auto Style=H->WindowPluginStyles.FindChecked(TEXT("__manager"));
        TestTrue(TEXT("Unloaded checkbox artwork has distinct checked/unchecked fallbacks"),
            Style->Check.CheckedImage.GetResourceName()!=Style->Check.UncheckedImage.GetResourceName()
            ||Style->Check.CheckedImage.GetResourceObject()!=Style->Check.UncheckedImage.GetResourceObject());
        TArray<TSharedRef<SCheckBox>> Checks;
        TFunction<void(TSharedRef<SWidget>)> FindChecks=[&](TSharedRef<SWidget> W)
        {if(W->GetType()==TEXT("SCheckBox"))Checks.Add(StaticCastSharedRef<SCheckBox>(W));auto* C=W->GetChildren();for(int32 I=0;I<C->Num();++I)FindChecks(C->GetChildAt(I));};
        FindChecks(Panel);
        if(TestTrue(TEXT("Initial manager has checked and unchecked controls"),Checks.Num()>=2))
        {
            const auto* PluginCheck=Checks.FindByPredicate([&](const auto& C){return C->GetTag()==FName(*(TEXT("PluginEnabled.")+FirstPlugin->Id));});
            if(!TestNotNull(TEXT("Enabled plugin has a summary checkbox"),PluginCheck))return false;
            TestTrue(TEXT("Summary checkbox reads enabled state on its first draw"),(*PluginCheck)->IsChecked());
            const FVector2D Size(740,530);FWidgetRenderer Renderer(false,true);
            auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            auto Draw=[&](){for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(Target,Panel,Size,.016f);FlushRenderingCommands();}};
            Draw();
            TestTrue(TEXT("Render plugin UI using retail fonts and artwork"),Dat->LoadDatDirectory(DatDirectory));
            Draw(); // No Use(), Open, Refresh button or panel reconstruction.
            auto* Resources=GI->GetSubsystem<UACEClientSubsystem>()->GetUIResourceResolver();
            TestTrue(TEXT("Existing checked brush refreshes when DAT becomes ready"),Style->Check.CheckedImage.GetResourceObject()==Resources->ResolveTexture(0x0600191A));
            TestTrue(TEXT("Existing unchecked brush refreshes when DAT becomes ready"),Style->Check.UncheckedImage.GetResourceObject()==Resources->ResolveTexture(0x0600191F));
            TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);
            auto GreenPixels=[&](const TSharedRef<SCheckBox>& Check)
            {
                const auto& G=Check->GetCachedGeometry();const auto At=Panel->GetCachedGeometry().AbsoluteToLocal(G.GetAbsolutePosition());int32 Count=0;
                for(int32 Y=FMath::Max(0,FMath::FloorToInt(At.Y));Y<FMath::Min(530,FMath::CeilToInt(At.Y+G.GetLocalSize().Y));++Y)
                    for(int32 X=FMath::Max(0,FMath::FloorToInt(At.X));X<FMath::Min(740,FMath::CeilToInt(At.X+24));++X)
                    {const auto C=Pixels[Y*740+X];if(C.G>120&&int32(C.G)>int32(C.R)+40&&int32(C.G)>int32(C.B)+40)++Count;}
                return Count;
            };
            TestTrue(TEXT("Initial enabled plugin draws its green retail marker without reopening"),GreenPixels(*PluginCheck)>20);
            TestTrue(TEXT("Initial global checkbox also draws its green marker"),GreenPixels(Checks[0])>20);
            TestEqual(TEXT("Initial unchecked control remains visually empty"),GreenPixels(Checks[1]),0);
            TArray<TSharedRef<SCheckBox>> Original=Checks;Checks.Reset();FindChecks(Panel);
            TestTrue(TEXT("Artwork arrival preserves the existing controls"),Checks==Original);
            TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(740,530,Pixels,Png);
            IFileManager::Get().MakeDirectory(*(FPaths::ProjectSavedDir()/TEXT("Automation")),true);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/PluginManager-InitialArtwork.png")));
            Target->ReleaseResource();
        }
        else TestTrue(TEXT("Load retail artwork"),Dat->LoadDatDirectory(DatDirectory));
    }
    {
        auto* Viewport=NewObject<UGameViewportClient>(GEngine);
        for(bool Previous:{false,true})
        {
            H->SuspendedWorldViewport=Viewport;H->PreviousWorldRenderingDisabled=Previous;
            Viewport->bDisableWorldRendering=true;
            H->Stop(TEXT("ucm"),TEXT("Rendering test"));
            TestEqual(TEXT("Stopping UCM restores the prior world-rendering state"),bool(Viewport->bDisableWorldRendering),Previous);
            TestFalse(TEXT("Stopping relinquishes ownership of the viewport"),H->SuspendedWorldViewport.IsValid());
        }
    }
    TestTrue(TEXT("High-power war can use optional jiggle"),ACEPluginCastMotion::Jiggle(1,300,0,0,0,false));
    TestFalse(TEXT("Instant war skips jiggle"),ACEPluginCastMotion::Jiggle(1,25,0,0,0,false));
    TestTrue(TEXT("Learned non-war instant spells can jiggle"),ACEPluginCastMotion::Jiggle(4,300,12,300,0,false));
    TestFalse(TEXT("Instant item spell skips jiggle"),ACEPluginCastMotion::Jiggle(4,300,12,300,0,true));
    TestTrue(TEXT("Timed hostile item spell can jiggle"),ACEPluginCastMotion::Jiggle(4,300,0,120,0,true));
    TestEqual(TEXT("Jiggle starts in one direction"),ACEPluginCastMotion::JiggleStrafe(0),1.f);
    TestEqual(TEXT("Jiggle alternates every 131 ms"),ACEPluginCastMotion::JiggleStrafe(.132),-1.f);
    TestEqual(TEXT("Jiggle alternates back"),ACEPluginCastMotion::JiggleStrafe(.263),1.f);
    H->OptionalCastOwner=TEXT("ucm");float Forward=1,Strafe=0,Turn=0;
    H->ApplyMovement(nullptr,Forward,Strafe,Turn,true,false,false,FVector::ForwardVector,0);
    TestTrue(TEXT("Manual movement cancels optional casting motion"),H->OptionalCastOwner.IsEmpty());
    TestEqual(TEXT("Manual forward input survives cancellation"),Forward,1.f);
    H->OptionalCastOwner=TEXT("ucm");H->Stop(TEXT("ucm"),TEXT("Test cancellation"));
    TestTrue(TEXT("Stopping policy cancels optional motion"),H->OptionalCastOwner.IsEmpty());
    TestTrue(TEXT("Fast buff motion applies to self creature buffs"),ACEPluginCastMotion::FastBuff(4,300,12,300));
    TestTrue(TEXT("Fast buff motion applies to low-difficulty life spells"),ACEPluginCastMotion::FastBuff(2,25,4,0));
    TestFalse(TEXT("Fellowship buffs retain ordinary casting"),ACEPluginCastMotion::FastBuff(4,300,12|0x2000,300));
    TestFalse(TEXT("Other-target buffs retain ordinary casting"),ACEPluginCastMotion::FastBuff(4,300,4,300));
    TestFalse(TEXT("Item enchantments retain ordinary casting"),ACEPluginCastMotion::FastBuff(3,300,12,300));
    TestFalse(TEXT("War and void never use fast-buff movement"),ACEPluginCastMotion::FastBuff(1,25,0,0)||ACEPluginCastMotion::FastBuff(5,25,0,0));
    const FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation")/(TEXT("PluginTest-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    H->StorageRoot=Root;H->Settings=MakeShared<FJsonObject>();H->Discover();
    TestTrue(TEXT("UCM Micro is discovered as a controls-only plugin"),H->Find(TEXT("ucmmicro"))&&H->Find(TEXT("ucmmicro"))->Permissions.IsEmpty());
    {
        auto Queue=[&](){auto E=Json(TEXT(R"({"target":100,"spell":42,"category":42,"power":100,"duration":120,"confirmation":"You cast Magic Yield Other I on Target","resist":"Target resists your spell"})"));E->SetNumberField(TEXT("sent"),FPlatformTime::Seconds());H->PendingDebuffCasts.Add(MakeShared<FJsonValueObject>(E));};
        Queue();H->ObserveUseDone(0);TestTrue(TEXT("UseDone alone never confirms offensive enchantments"),H->Debuffs.IsEmpty());
        H->ObserveMetaChat(TEXT("You cast Magic Yield Other I on Target"),TEXT("Another player"),ACEChatMessageType::Magic);
        H->ObserveMetaChat(TEXT("You cast Magic Yield Other I on Target"),TEXT(""),ACEChatMessageType::Tell);
        H->ObserveMetaChat(TEXT("You cast Magic Yield Other I on Target Two"),TEXT(""),ACEChatMessageType::Magic);
        TestTrue(TEXT("Player chat, wrong channel and target prefix cannot confirm a debuff"),H->Debuffs.IsEmpty());
        H->ObserveMetaChat(TEXT("Target resists your spell"),TEXT(""),ACEChatMessageType::Magic);TestTrue(TEXT("Resist consumes attempt without granting duration"),H->PendingDebuffCasts.IsEmpty()&&H->Debuffs.IsEmpty());
        TestEqual(TEXT("Resist is visible to bounded retry policy"),H->DebuffRevision,1u);
        Queue();H->ObserveMetaChat(TEXT("You cast Magic Yield Other I on Target, refreshing Magic Yield Other I"),TEXT(""),ACEChatMessageType::Magic);
        TestEqual(TEXT("Matching server success records duration"),H->Debuffs.Num(),1);TestEqual(TEXT("Confirmed target preserved"),H->LastDebuffTarget,100);
        if(!H->Debuffs.IsEmpty())TestTrue(TEXT("Duration expires at the known base duration"),H->Debuffs[0]->AsObject()->GetNumberField(TEXT("expires"))>FPlatformTime::Seconds()+119);
        H->Debuffs.Empty();Queue();H->PendingDebuffCasts.Last()->AsObject()->SetNumberField(TEXT("sent"),FPlatformTime::Seconds()-31);
        H->ObserveMetaChat(TEXT("You cast Magic Yield Other I on Target"),TEXT(""),ACEChatMessageType::Magic);TestTrue(TEXT("Stale confirmation cannot mark a new target"),H->Debuffs.IsEmpty());
        Queue();H->PendingSpell=42;H->PendingSpellTarget=100;H->ObserveUseDone(0x400);TestTrue(TEXT("Rejected cast clears confirmation candidate"),H->PendingDebuffCasts.IsEmpty());
    }
    ON_SCOPE_EXIT {GI->Shutdown(); IFileManager::Get().DeleteDirectory(*Root,false,true);};
    TestTrue(TEXT("Built-in UCM discovered"),H->Find(TEXT("ucm")).IsValid());
    auto P=H->Find(TEXT("ucm")); if(!P)return false;
    TestFalse(TEXT("Plugins default to disabled"),P->Enabled);
    TestFalse(TEXT("Disabled plugin cannot start"),H->Start(TEXT("ucm")));
    H->SetEnabled(TEXT("ucm"),true);
    TestFalse(TEXT("Enabling never auto-starts"),P->Running);
    TestFalse(TEXT("Login screen cannot issue game actions"),H->Start(TEXT("ucm")));
    TestFalse(TEXT("Force Buff cannot start automation outside the world"),H->RequestForceBuff());
    P->Running=true;
    TestTrue(TEXT("Force Buff can refresh an existing run"),H->RequestForceBuff());
    const uint32 FirstRequest=H->ForceBuffRequest;
    H->RequestForceBuff();TestEqual(TEXT("Repeated click does not restart a queued cycle"),H->ForceBuffRequest,FirstRequest);
    auto Complete=MakeShared<FJsonObject>();Complete->SetStringField(TEXT("action"),TEXT("force_buff_done"));Complete->SetNumberField(TEXT("request"),FirstRequest);
    H->Execute(*P,Complete);
    TestFalse(TEXT("Completion clears the transient request"),H->IsForceBuffRequested());
    TestTrue(TEXT("Existing run resumes after forced refresh"),P->Running);
    H->RequestForceBuff();H->Execute(*P,Complete);
    TestTrue(TEXT("Old completion cannot clear a newer request"),H->IsForceBuffRequested());
    H->CancelForceBuff();TestTrue(TEXT("Cancelling refresh preserves an existing run"),P->Running);
    H->RequestForceBuff();H->bForceBuffOnly=true;Complete->SetNumberField(TEXT("request"),H->ForceBuffRequest);H->Execute(*P,Complete);
    TestFalse(TEXT("A buff-only run stops after completion"),P->Running);
    TestFalse(TEXT("Profile path traversal rejected"),H->SaveProfile(TEXT("ucm"),TEXT("../escape"),TEXT("{}")));
    TestFalse(TEXT("Malformed profile rejected"),H->SaveProfile(TEXT("ucm"),TEXT("Broken"),TEXT("{")));
    TestFalse(TEXT("Invalid native route data rejected"),H->SaveProfile(TEXT("ucm"),TEXT("Broken"),TEXT("{\"route\":[42]}")));
    TestFalse(TEXT("Malformed monster catalog rejected"),H->SaveProfile(TEXT("ucm"),TEXT("Broken"),TEXT(R"({"monster_catalog":{"Monster":{"species":"bad","max_health":100}}})")));
    TestFalse(TEXT("Fractional pea output rejected"),H->SaveProfile(TEXT("ucm"),TEXT("Broken"),TEXT(R"({"pea_recipes":[{"tool":"Tool","input":"Pea","output":"Scarab","count":0.5}]})")));
    TestFalse(TEXT("Partially converted loot rule cannot be enabled"),H->SaveProfile(TEXT("ucm"),TEXT("Broken"),TEXT(R"({"loot_rules":[{"enabled":true,"compatibility_issues":["requirement 14"]}]})")));
    TestFalse(TEXT("Partial loot rule defaults cannot bypass activation validation"),H->SaveProfile(TEXT("ucm"),TEXT("Broken"),TEXT(R"({"loot_rules":[{"compatibility_issues":["requirement 14"]}]})")));
    TestTrue(TEXT("Disabled unsupported rule can be preserved"),H->SaveProfile(TEXT("ucm"),TEXT("Mage"),TEXT(R"({"loot_rules":[{"enabled":false,"compatibility_issues":["requirement 14"]}]})")));
    TestTrue(TEXT("Named profile saved"),H->SaveProfile(TEXT("ucm"),TEXT("Mage"),TEXT("{\"skill_margin\":45}")));
    const TCHAR* LootProfile=TEXT(R"({"skill_margin":45,"loot_rules":[{"label":"Copper","material":59,"min_workmanship":7,"max_workmanship":10}]})");
    TestTrue(TEXT("Loot rules saved with UCM setup"),H->SaveProfile(TEXT("ucm"),TEXT("Mage"),LootProfile));
    P->Profile->SetArrayField(TEXT("vendor_rules"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"server":"Test","vendor_name":"Arcanist","vendor_wcid":2,"item_name":"Scarab","item_wcid":3,"quantity":100})")))});
    TestTrue(TEXT("Loot-only profile saved"),H->SaveLootProfile(TEXT("Copper_W7")));
    TestFalse(TEXT("Loot profile path traversal rejected"),H->SaveLootProfile(TEXT("../escape")));
    TestTrue(TEXT("Separate loot variation created before editing"),H->SaveUCMSectionAs(TEXT("Loot"),TEXT("EmptyLoot")));
    TestTrue(TEXT("Other hunt saved"),H->SaveProfile(TEXT("ucm"),TEXT("Mage"),TEXT("{\"skill_margin\":45,\"loot_rules\":[]}")));
    P->Profile->SetStringField(TEXT("utl_source"),TEXT("Previous.utl"));P->Profile->SetBoolField(TEXT("loot_modified"),true);
    TestTrue(TEXT("Loot-only profile loads"),H->LoadLootProfile(TEXT("Copper_W7")));
    TestEqual(TEXT("Active loot name identifies the loaded rules"),P->Profile->GetStringField(TEXT("loot_profile")),FString(TEXT("Copper_W7")));
    TestFalse(TEXT("Loading native rules clears previous UTL label"),P->Profile->HasField(TEXT("utl_source")));
    TestFalse(TEXT("Loaded rules are not labeled as local edits"),P->Profile->HasField(TEXT("loot_modified")));
    TestEqual(TEXT("Loot profile leaves unrelated settings alone"),P->Profile->GetNumberField(TEXT("skill_margin")),45.);
    TestFalse(TEXT("Selecting loot does not restore vendor settings from a different configuration"),P->Profile->HasField(TEXT("vendor_rules")));
    TestEqual(TEXT("Saved rules restored"),P->Profile->GetArrayField(TEXT("loot_rules")).Num(),1);
    TestFalse(TEXT("Missing loot profile rejected"),H->LoadLootProfile(TEXT("Missing")));
    TestEqual(TEXT("Failed load preserves active rules"),P->Profile->GetArrayField(TEXT("loot_rules")).Num(),1);
    for(const TCHAR* Bad:{TEXT(R"({"loot_rules":[{"action":"unsupported_user_action"}]})"),TEXT(R"({"loot_rules":[{"name_mode":"regex","name":"["}]})"),TEXT(R"({"loot_rules":[{"min_workmanship":8,"max_workmanship":7}]})"),TEXT(R"({"loot_rules":[{"min_value":"lots"}]})"),TEXT(R"({"loot_rules":[{"material":1.5}]})")})
        TestFalse(*FString::Printf(TEXT("Malformed loot rules rejected before activating: %s"),Bad),H->SaveProfile(TEXT("ucm"),TEXT("BadLoot"),Bad));
    for(const TCHAR* Bad:{TEXT(R"({"salvage_policy":[]})"),TEXT(R"({"salvage_policy":{"default":[{"min":8,"max":2}]}})"),TEXT(R"({"salvage_policy":{"default":[],"values":{"6":-1}}})"),TEXT(R"({"salvage_policy":{"default":[],"materials":{"x":[]}}})")})
        TestFalse(TEXT("Malformed salvage policies rejected"),H->SaveProfile(TEXT("ucm"),TEXT("BadSalvage"),Bad));
    const FString PreviousProfile=H->ProfileJson(TEXT("ucm"));
    TestTrue(TEXT("Salvage policy saves"),H->SaveProfile(TEXT("ucm"),TEXT("Mage"),TEXT(R"({"skill_margin":45,"loot_rules":[],"salvage_policy":{"default":[{"min":1,"max":10}],"materials":{"6":[{"min":7,"max":10}]},"values":{"6":10000}}})")));
    TestTrue(TEXT("Salvage policy exported with loot rules"),H->SaveLootProfile(TEXT("Combine")));
    H->SelectUCMSection(TEXT("Loot"),TEXT("EmptyLoot"));
    H->SaveProfile(TEXT("ucm"),TEXT("Mage"),PreviousProfile);
    TestTrue(TEXT("Salvage policy restored"),H->LoadLootProfile(TEXT("Combine")));
    TestEqual(TEXT("Combining target survives save/load"),P->Profile->GetObjectField(TEXT("salvage_policy"))->GetObjectField(TEXT("values"))->GetNumberField(TEXT("6")),10000.);
    H->SaveProfile(TEXT("ucm"),TEXT("Mage"),PreviousProfile);
    const FString SavedCharacterProfile=H->UCMSectionName(TEXT("Character"));
    H->Discover();P=H->Find(TEXT("ucm"));
    TestTrue(TEXT("Waypoint discovered and ready without automation"),H->IsWaypointEnabled());
    TestTrue(TEXT("Chat/manual waypoint accepted"),H->SetWaypoint(TEXT("42.0N, 33.6E")));
    H->SetWaypointOption(TEXT("distance"),false);
    H->SavePluginWindowPosition(TEXT("waypoint.arrow"),FVector2D(350,20));
    H->Discover();P=H->Find(TEXT("ucm"));FVector2D Destination;uint32 Dungeon=0;
    TestTrue(TEXT("Waypoint destination survives reload"),H->GetWaypoint(Destination,Dungeon));
    TestEqual(TEXT("Destination preserved"),Destination,FVector2D(33.6,42));
    TestFalse(TEXT("Distance preference persists"),H->WaypointOption(TEXT("distance")));
    TestEqual(TEXT("Arrow position persists"),H->GetPluginWindowPosition(TEXT("waypoint.arrow"),FVector2D::ZeroVector),FVector2D(350,20));
    H->SetEnabled(TEXT("waypoint"),false);H->SetWaypoint(FVector2D(1,2));H->GetWaypoint(Destination,Dungeon);
    TestEqual(TEXT("Disabled plugin cannot navigate"),Destination,FVector2D(33.6,42));H->SetEnabled(TEXT("waypoint"),true);
    const FString Atlas=Root/TEXT("atlas.xml");
    FFileHelper::SaveStringToFile(TEXT("<locations><loc name=\"Test portal\" type=\"Portal\" NS=\"42.0\" EW=\"33.6\" /></locations>"),*Atlas);
    TestTrue(TEXT("GoArrow atlas imports without a DLL"),H->ImportWaypointLocations(Atlas));
    TestEqual(TEXT("Imported location count"),H->GetWaypointLocations().Num(),1);
    FFileHelper::SaveStringToFile(TEXT("<locations><loc name=\"Invalid\" NS=\"NaN\" EW=\"3\" /></locations>"),*Atlas);
    TestFalse(TEXT("Invalid atlas rejected"),H->ImportWaypointLocations(Atlas));
    TestEqual(TEXT("Rejected import preserves atlas"),H->GetWaypointLocations().Num(),1);
    FString AtlasFixture;
    if(FParse::Value(FCommandLine::Get(),TEXT("WaypointAtlas="),AtlasFixture))
    {
        TestTrue(*H->Notice,H->ImportWaypointLocations(AtlasFixture));
        AddInfo(FString::Printf(TEXT("GoArrow atlas imported: %d locations"),H->GetWaypointLocations().Num()));
    }

    TestTrue(TEXT("Independent loot editor is discovered"),H->Find(TEXT("looteditor")).IsValid());
    TestTrue(TEXT("Editor requests no game actions"),H->Find(TEXT("looteditor"))->Permissions.IsEmpty());
    TestTrue(TEXT("Capability grant persists"),P->Enabled);
    TestFalse(TEXT("Reload never resumes automation"),P->Running);
    TestEqual(TEXT("Active configuration persists"),P->ProfileName,ACEUCMProfiles::ActiveName());
    TestEqual(TEXT("Selected character profile persists"),H->UCMSectionName(TEXT("Character")),SavedCharacterProfile);
    TestEqual(TEXT("Profile values persist"),P->Profile->GetNumberField(TEXT("skill_margin")),45.);
    const FString BeforeImport=H->ProfileJson(TEXT("ucm"));
    const FString SourcePath=Root/TEXT("source.utl");
    FFileHelper::SaveStringToFile(TEXT("UTL\n1\n1\nUnsupported skip\n\n0;0;999\n4\nabc\n"),*SourcePath);
    TestFalse(TEXT("Unsupported import refused"),H->ImportLegacyProfile(SourcePath,TEXT("Unsupported")));
    TestEqual(TEXT("Failed import preserves active profile"),H->ProfileJson(TEXT("ucm")),BeforeImport);
    TArray<FString> Reports;IFileManager::Get().FindFiles(Reports,*(Root/TEXT("Imports/Unsupported_*.json")),true,false);
    TestEqual(TEXT("Blocked import saves full compatibility archive"),Reports.Num(),1);
    FFileHelper::SaveStringToFile(TEXT("UTL\n1\n1\nKeep ring\n\n0;1;1\n7\nRing\n1\n"),*SourcePath);
    TestTrue(TEXT("Supported import applied"),H->ImportLegacyProfile(SourcePath,TEXT("Imported")));
    TestEqual(TEXT("Import retains unrelated settings"),P->Profile->GetNumberField(TEXT("skill_margin")),45.);
    TestFalse(TEXT("Import stays stopped"),P->Running);
    FString Original,FirstCopy,SecondCopy;FFileHelper::LoadFileToString(Original,*SourcePath);
    TestEqual(TEXT("Import selects its native loot profile"),H->UCMSectionName(TEXT("Loot")),FString(TEXT("Imported")));
    TestTrue(TEXT("Imported loot is saved separately"),FFileHelper::LoadFileToString(FirstCopy,*(Root/TEXT("Profiles/Loot/Imported.json"))));
    TestTrue(TEXT("Switch away before importing another copy"),H->SelectUCMSection(TEXT("Loot"),TEXT("EmptyLoot")));
    TestTrue(TEXT("Repeated import reuses the existing profile"),H->ImportLegacyProfile(SourcePath,TEXT("Imported")));
    TestEqual(TEXT("Repeat selects the original profile"),H->UCMSectionName(TEXT("Loot")),FString(TEXT("Imported")));
    TestFalse(TEXT("Repeat does not create a numbered duplicate"),IFileManager::Get().FileExists(*(Root/TEXT("Profiles/Loot/Imported_2.json"))));
    TestTrue(TEXT("Original native loot file remains readable"),FFileHelper::LoadFileToString(SecondCopy,*(Root/TEXT("Profiles/Loot/Imported.json"))));
    TestEqual(TEXT("Previous native copy preserved byte for byte"),FirstCopy,SecondCopy);
    FFileHelper::LoadFileToString(SecondCopy,*SourcePath);TestEqual(TEXT("Legacy source untouched"),Original,SecondCopy);
    Reports.Reset();IFileManager::Get().FindFiles(Reports,*(Root/TEXT("Imports/Imported_*.json")),true,false);
    TestEqual(TEXT("Reimport keeps one source report"),Reports.Num(),1);
    FFileHelper::SaveStringToFile(TEXT("invalid source no longer required"),*SourcePath);
    TestTrue(TEXT("Imported copy reloads without a valid legacy source"),H->SelectUCMSection(TEXT("Loot"),TEXT("Imported")));
    FFileHelper::SaveStringToFile(Original,*SourcePath);
    TestTrue(TEXT("Native echo command works"),H->RunUCMCommand(TEXT("echo /vt jump is literal chat text")));
    TestEqual(TEXT("Echo body is not rewritten"),H->Notice,FString(TEXT("/vt jump is literal chat text")));
    TestTrue(TEXT("Native option edits stopped setup"),H->RunUCMCommand(TEXT("opt set EnableLooting true")));
    TestTrue(TEXT("Option is applied"),P->Profile->GetBoolField(TEXT("looting")));
    TestFalse(TEXT("Unknown command gives explicit error"),H->RunUCMCommand(TEXT("notacommand")));
    TestTrue(TEXT("Command help available"),H->RunUCMCommand(TEXT("help")));

    TestTrue(TEXT("Existing folder can be selected"),H->SetProfileFolder(Root));
    const FString MonsterSource=TEXT("1\nMyMonsters\n2\nMonsterName\nAttackPriority\ny\nn\n1\ns\nspecies==Olthoi&&maxhp>1000\ni\n9\n");
    const FString MonsterDatabase=TEXT("1\nSpeciesMembers\n3\nMonster\nSpecies\nMaximumHealth\ny\nn\nn\n1\ns\nOlthoi Warrior\ni\n1\ni\n2500\n");
    FFileHelper::SaveStringToFile(MonsterSource,*(Root/TEXT("Monsters.usd")));
    FFileHelper::SaveStringToFile(MonsterDatabase,*(Root/TEXT("GameInfoDB.UGD")));
    TArray<FString> MonsterIssues;auto MonsterProfile=ACEVTProfile::ConvertFile(Root/TEXT("Monsters.usd"),MonsterIssues);
    TestEqual(TEXT("Sibling database loads with mixed-case filename"),MonsterIssues.Num(),0);
    TestEqual(TEXT("Database health retained in native copy"),MonsterProfile->GetObjectField(TEXT("monster_catalog"))->GetObjectField(TEXT("Olthoi Warrior"))->GetNumberField(TEXT("max_health")),2500.);
    FString UnchangedDatabase;FFileHelper::LoadFileToString(UnchangedDatabase,*(Root/TEXT("GameInfoDB.UGD")));TestEqual(TEXT("Game information source remains unchanged"),UnchangedDatabase,MonsterDatabase);
    TestTrue(TEXT("Folder browser lists loot files without importing"),H->GetProfileFiles(TEXT("utl")).Contains(TEXT("source.utl")));
    TestTrue(TEXT("Switch away before direct folder import"),H->SelectUCMSection(TEXT("Loot"),TEXT("EmptyLoot")));
    TestTrue(TEXT("Direct folder selection works"),H->SelectProfileFile(TEXT("source.utl")));
    TestEqual(TEXT("Active source is recorded"),P->Profile->GetStringField(TEXT("utl_source")),Root/TEXT("source.utl"));
    TestEqual(TEXT("Direct selection reuses the named native profile"),H->UCMSectionName(TEXT("Loot")),FString(TEXT("Imported")));
    const FString Loaded=H->ProfileJson(TEXT("ucm"));
    FFileHelper::SaveStringToFile(TEXT("invalid"),*(Root/TEXT("broken.met")));
    TestFalse(TEXT("Unsupported file cannot replace active configuration"),H->SelectProfileFile(TEXT("broken.met")));
    TestEqual(TEXT("Failed selection is transactional"),H->ProfileJson(TEXT("ucm")),Loaded);
    TestFalse(TEXT("Folder selection cannot escape root"),H->SelectProfileFile(TEXT("../source.utl")));
    TestFalse(TEXT("Invalid folder does not replace configured folder"),H->SetProfileFolder(Root/TEXT("missing")));
    TestEqual(TEXT("Configured folder retained"),H->GetProfileFolder(),Root);
    FFileHelper::SaveStringToFile(TEXT("uTank2 NAV 1.2\n2\n2\n0\n0\n0\n0.05\n0\n3\n0.01\n0\n0.05\n0\n1500\n"),*(Root/TEXT("Walk home.NAV")));
    TestTrue(TEXT("Browser handles uppercase extensions and spaces"),H->GetProfileFiles(TEXT("nav")).Contains(TEXT("Walk home.NAV")));
    TestTrue(TEXT("Route loads directly from original folder"),H->SelectProfileFile(TEXT("Walk home.NAV")));
    TestEqual(TEXT("Route points applied to active profile"),P->Profile->GetArrayField(TEXT("route")).Num(),2);
    TestTrue(TEXT("Loaded route immediately enables world preview"),P->Profile->GetBoolField(TEXT("route_preview")));
    TestTrue(TEXT("Selected route is ready to follow when Start is pressed"),P->Profile->GetBoolField(TEXT("navigation")));
    TestFalse(TEXT("Selecting a route does not start movement before Start"),P->Running);
    TestTrue(TEXT("Navigation can be disabled independently"),H->RunUCMCommand(TEXT("enablenav false")));
    TestTrue(TEXT("Selecting the same imported route succeeds"),H->SelectProfileFile(TEXT("Walk home.NAV")));
    TestTrue(TEXT("Reselecting a route also enables navigation"),P->Profile->GetBoolField(TEXT("navigation")));
    FFileHelper::SaveStringToFile(TEXT("uTank2 NAV 1.2\n1\n1\n0\n0.02\n0\n0.05\n0\n"),*(Root/TEXT("Other.nav")));
    TestTrue(TEXT("Switching to another navigation profile succeeds"),H->SelectProfileFile(TEXT("Other.nav")));
    TestEqual(TEXT("Switching replaces rather than appends old waypoints"),P->Profile->GetArrayField(TEXT("route")).Num(),1);
    TestTrue(TEXT("New circular route replaces previous linear traversal"),P->Profile->GetBoolField(TEXT("loop_route"))&&!P->Profile->GetBoolField(TEXT("reverse_route")));
    TestTrue(TEXT("Switching keeps navigation enabled"),P->Profile->GetBoolField(TEXT("navigation")));
    auto Dependency=MakeShared<FJsonObject>(*P->Profile);Dependency->SetBoolField(TEXT("navigation"),false);
    auto Library=MakeShared<FJsonObject>();Library->SetObjectField(TEXT("other.nav"),Dependency);P->Profile->SetObjectField(TEXT("vt_library"),Library);
    H->Execute(*P,Json(TEXT(R"({"action":"profile_load","kind":"nav","profile":"other.nav"})")));
    TestTrue(TEXT("Meta loading another NAV preserves its active navigation switch"),P->Profile->GetBoolField(TEXT("navigation")));
    TestEqual(TEXT("Selecting route preserves loot selection"),P->Profile->GetStringField(TEXT("utl_source")),Root/TEXT("source.utl"));
    TestTrue(TEXT("None clears route"),H->ClearProfileFile(TEXT("nav")));
    TestEqual(TEXT("Cleared route cannot leave stale world geometry"),P->Profile->GetArrayField(TEXT("route")).Num(),0);
    TestTrue(TEXT("Imported rules save independently"),H->SaveLootProfile(TEXT("ImportedRules")));
    FString BeforeSource,AfterSource;FFileHelper::LoadFileToString(BeforeSource,*SourcePath);
    TestTrue(TEXT("Legacy editor saves a copy"),H->SaveLegacyLoot(TEXT("Copy"),H->InspectLegacyProfile(SourcePath)));
    FFileHelper::LoadFileToString(AfterSource,*SourcePath);TestEqual(TEXT("Original file never overwritten"),AfterSource,BeforeSource);
    H->Settings->SetStringField(TEXT("ucm"),TEXT("enabled:cast"));H->Discover();P=H->Find(TEXT("ucm"));
    TestFalse(TEXT("Changed permission set requires re-enabling"),P->Enabled);
    P->Running=true;float F=1,R=0,T=0;H->FastCastOwner=TEXT("ucm");
    H->ApplyMovement(nullptr,F,R,T,true,false,false,FVector::ForwardVector);
    TestTrue(TEXT("Manual movement leaves UCM running"),P->Running);
    const auto ActiveVM=P->VM;const auto ActiveProfile=P->Profile;
    for(bool VR:{false,true})for(int Frame=0;Frame<4;++Frame)
    {
        H->MovementOwner=TEXT("ucm");H->MoveExpires=FPlatformTime::Seconds()+2;H->FaceHeading=90;
        H->FastCastOwner=TEXT("ucm");H->FastCastStarted=true;H->UseApproachOwner=TEXT("ucm");
        H->LastProgress=FPlatformTime::Seconds()-60; // Stale route must not trip its watchdog during manual play.
        F=-.5f;R=.75f;T=.25f;
        H->ApplyMovement(nullptr,F,R,T,true,false,VR,FVector::ForwardVector);
        TestTrue(TEXT("Desktop and VR manual input retains running VM and activity profile"),P->Running&&P->VM==ActiveVM&&P->Profile==ActiveProfile);
        TestEqual(TEXT("Manual forward/backward input wins"),F,-.5f);TestEqual(TEXT("Manual strafe input wins"),R,.75f);TestEqual(TEXT("Manual turn input wins"),T,.25f);
        TestFalse(TEXT("Manual input releases plugin steering ownership"),H->IsDrivingMovement());
        TestTrue(TEXT("Manual input clears stale heading and approach owner"),!H->FaceHeading.IsSet()&&H->UseApproachOwner.IsEmpty());
    }
    F=R=T=0;H->ApplyMovement(nullptr,F,R,T,false,false,false,FVector::ForwardVector);
    TestTrue(TEXT("Releasing input needs no restart and cannot revive stale steering"),P->Running&&F==0&&R==0&&T==0);
    TestTrue(TEXT("Manual movement releases fast-cast input"),H->FastCastOwner.IsEmpty());
    H->FastCastOwner=TEXT("ucm");H->ObserveUseDone(0);TestTrue(TEXT("Cast completion releases fast-cast input"),H->FastCastOwner.IsEmpty());
    P->Running=true;H->Tick(.5f);
    TestFalse(TEXT("Leaving world stops automation"),P->Running);
    auto Permissions=P->Permissions;P->Permissions.Empty();P->Running=true;
    H->Execute(*P,Json(TEXT("{\"action\":\"cast\",\"spell\":42}")));
    TestTrue(TEXT("Permission rejection preserves other UCM activities"),P->Running&&P->ActivityFailure.IsValid());
    TestEqual(TEXT("Denied spell is not dispatched"),H->PendingSpell,0);
    TestTrue(TEXT("Permission rejection is reported to the policy"),P->ActivityFailure&&P->ActivityFailure->GetStringField(TEXT("status")).Contains(TEXT("Action not permitted")));
    P->ActivityFailure.Reset();P->Permissions=Permissions;
    // Exercise actual shipped/import-folder loot files through activation and
    // reload, not just the converter or an in-memory profile assignment.
    const FString Inbox=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("ClientPlugins/ImportInbox"));
    if(IFileManager::Get().DirectoryExists(*Inbox))
    {
        H->SetProfileFolder(Inbox);
        for(const auto& File:H->GetProfileFiles(TEXT("utl")))
        {
            const bool LootActivated=H->SelectProfileFile(File);
            if(!TestTrue(*(TEXT("Loot activates: ")+File+TEXT(" - ")+H->Notice),LootActivated))continue;
            const FString Setup=P->ProfileName;
            TestTrue(TEXT("Loot setup reloads"),H->LoadProfile(TEXT("ucm"),Setup));
            TestEqual(TEXT("Reload preserves active source"),FPaths::GetCleanFilename(P->Profile->GetStringField(TEXT("utl_source"))),File);
            TestTrue(TEXT("Reload preserves loot rules"),P->Profile->GetArrayField(TEXT("loot_rules")).Num()>0);
        }
        FString SelectedLoot;P->Profile->TryGetStringField(TEXT("utl_source"),SelectedLoot);
        H->Discover();P=H->Find(TEXT("ucm"));
        FString ReloadedLoot;P->Profile->TryGetStringField(TEXT("utl_source"),ReloadedLoot);
        TestEqual(TEXT("Plugin rediscovery restores selected loot from disk"),ReloadedLoot,SelectedLoot);
    }
    FString Defaults;FFileHelper::LoadFileToString(Defaults,*(P->Directory/TEXT("default.json")));
    H->SaveProfile(TEXT("ucm"),TEXT("Default"),Defaults);H->Stop(TEXT("ucm"));
    H->Notice=TEXT("Offline validation. Plugins are stopped.");
    H->SetEnabled(TEXT("ucm"),true);
    H->SavePluginWindowPosition(TEXT("ucm"),FVector2D(150,100));
    H->SavePluginWindowPosition(TEXT("__bar"),FVector2D(8,80));
    H->SetWaypointOption(TEXT("dungeon_overlay"),true);H->SetWaypointOption(TEXT("heading_up"),true);
    H->SetWaypointOption(TEXT("map_locked"),true);
    auto* Ui=GI->GetSubsystem<UACEClientSubsystem>()->GetUIElementManager();
    Ui->SetUiLocked(false);
    TestTrue(TEXT("Global unlock icon overrides retired pinned map lock preference"),H->IsWaypointMapUnlocked());
    H->SetWaypointOption(TEXT("locked"),true);
    TestTrue(TEXT("Global unlock overrides retired arrow lock preference"),H->IsWaypointUnlocked());
    Ui->SetUiLocked(true);
    H->SavePluginWindowPosition(TEXT("waypoint.dungeon"),FVector2D(800,100));
    H->SavePluginWindowPosition(TEXT("waypoint.dungeon.size"),FVector2D(300,300));
    TestEqual(TEXT("Window position saved separately from profile"),H->GetPluginWindowPosition(TEXT("ucm"),FVector2D::ZeroVector),FVector2D(150,100));
    H->Discover();P=H->Find(TEXT("ucm"));
    TestTrue(TEXT("Pinned mode and orientation persist"),H->WaypointOption(TEXT("dungeon_overlay"))&&H->WaypointOption(TEXT("heading_up")));
    TestFalse(TEXT("Pinned map is click-through when locked"),H->IsWaypointMapUnlocked());
    TestEqual(TEXT("Plugin reload preserves window positions"),H->GetPluginWindowPosition(TEXT("ucm"),FVector2D::ZeroVector),FVector2D(150,100));
    {
        H->SetWaypointOption(TEXT("arrow"),true);
        H->SavePluginWindowPosition(TEXT("waypoint.arrow"),FVector2D(2100,800));
        H->SavePluginWindowPosition(TEXT("waypoint.dungeon"),FVector2D(1800,700));
        auto LoginDock=SNew(SACEPluginDesktop).Host(H);H->DesktopDock=LoginDock;
        Ui->SetQuickShowUi(true);
        for(const TCHAR* Id:{TEXT("waypoint.arrow"),TEXT("waypoint.dungeon")})
        {
            auto& Window=LoginDock->Windows[Id];const auto Saved=H->GetPluginWindowPosition(Id,FVector2D::ZeroVector);
            TestEqual(TEXT("Login construction preserves full-resolution overlay position"),Window.Position,Saved);
            LoginDock->ViewSize=FVector2D(800,600);LoginDock->Layout(Window);
            TestEqual(TEXT("Temporary small viewport does not replace saved position"),Window.Position,Saved);
            LoginDock->ViewSize=FVector2D(2560,1440);LoginDock->Layout(Window);
            const auto Offset=Window.Slot->GetOffset();
            TestEqual(TEXT("Restored viewport displays saved position"),FVector2D(Offset.Left,Offset.Top),Saved);
            LoginDock->Move(Id,FVector2D(-20,30),false); // Logout before mouse-up.
        }
        LoginDock->Resize(TEXT("waypoint.dungeon"),FVector2D(40,50),false);
        Ui->SetQuickShowUi(false);
        H->RemoveDesktopDock();
        FString DiskSettings;TestTrue(TEXT("Overlay settings are written to disk"),FFileHelper::LoadFileToString(DiskSettings,*(Root/TEXT("settings.json"))));
        H->Settings=Json(*DiskSettings);
        auto RelogDock=SNew(SACEPluginDesktop).Host(H);
        TestEqual(TEXT("Arrow position restores from disk after logout"),RelogDock->Windows[TEXT("waypoint.arrow")].Position,FVector2D(2080,830));
        TestEqual(TEXT("Pinned map position restores from disk after logout"),RelogDock->Windows[TEXT("waypoint.dungeon")].Position,FVector2D(1780,730));
        TestEqual(TEXT("Pinned map size restores from disk after logout"),RelogDock->Windows[TEXT("waypoint.dungeon")].Size,FVector2D(340,350));
        // Restore the compact fixture for the existing panel render checks.
        H->SavePluginWindowPosition(TEXT("waypoint.arrow"),FVector2D(350,20));
        H->SavePluginWindowPosition(TEXT("waypoint.dungeon"),FVector2D(800,100));
        H->SavePluginWindowPosition(TEXT("waypoint.dungeon.size"),FVector2D(300,300));
    }
    auto Second=MakeShared<FACEClientPlugin>();Second->Id=TEXT("monitor");Second->Name=TEXT("Example Monitor");Second->Enabled=true;
    Second->Manifest=Json(TEXT("{\"short_name\":\"MON\"}"));Second->Profile=MakeShared<FJsonObject>();H->Plugins.Add(Second);
    auto Dock=SNew(SACEPluginDesktop).Host(H);H->DesktopDock=Dock;
    TestTrue(TEXT("Pinned map reopens with desktop dock"),Dock->IsOpen(TEXT("waypoint.dungeon")));
    TestEqual(TEXT("Pinned map restores its size"),Dock->Windows[TEXT("waypoint.dungeon")].Size,FVector2D(300,300));
    {
        auto Session=GI->GetSubsystem<UACEClientSubsystem>()->GetSession();
        const auto Geometry=FGeometry::MakeRoot(FVector2D(1920,1080),FSlateLayoutTransform());
        const auto OriginalWidget=Dock->Windows[TEXT("waypoint.dungeon")].Widget;
        for(bool Unlocked:{false,true})for(uint32 Cell:{0x7D640014u,0x01430171u,0x7D64001Au})
        {
            Ui->SetUiLocked(!Unlocked);
            FACEPosition Position;Position.CellId=int32(Cell);Position.Location=FVector(50,50,0);Session->SetLocalPosition(Position);
            Dock->Tick(Geometry,1,.016f);
            const auto& Window=Dock->Windows[TEXT("waypoint.dungeon")];
            TestTrue(TEXT("World/interior transitions reuse pinned widget"),Window.Widget==OriginalWidget);
            TestTrue(TEXT("Pinned map keeps its minimize button clickable even when locked"),Window.Widget->GetVisibility()==EVisibility::Visible);
            TestEqual(TEXT("World/interior transitions preserve pinned position"),Window.Position,FVector2D(800,100));
            TestEqual(TEXT("World/interior transitions preserve pinned size"),Window.Size,FVector2D(300,300));
        }
        const auto ExpandedSize=Dock->Windows[TEXT("waypoint.dungeon")].Size;
        H->SetWaypointOption(TEXT("map_minimized"),true);Dock->Tick(Geometry,2,.016f);
        TestEqual(TEXT("Minimized map retains expanded size"),Dock->Windows[TEXT("waypoint.dungeon")].Size,ExpandedSize);
        TestEqual(TEXT("Minimized map uses a compact restore button"),Dock->Windows[TEXT("waypoint.dungeon")].Slot->GetOffset().Right,70.f);
        TestTrue(TEXT("Minimizing map leaves arrow open"),Dock->IsOpen(TEXT("waypoint.arrow")));
        H->SetWaypointOption(TEXT("map_minimized"),false);Dock->Tick(Geometry,3,.016f);
        TestEqual(TEXT("Restoring map recovers saved width"),double(Dock->Windows[TEXT("waypoint.dungeon")].Slot->GetOffset().Right),ExpandedSize.X);
        H->SetWaypointMapOpacity(.4f);TestEqual(TEXT("Pinned opacity is independently configurable"),H->WaypointMapOpacity(),.4f);
        H->SetWaypointMapOpacity(1.f);
        Ui->SetUiLocked(true);
    }
    H->SetWaypointOption(TEXT("dungeon_overlay"),false);Dock->Refresh();
    TestFalse(TEXT("Normal-map-only mode hides pinned overlay"),Dock->IsOpen(TEXT("waypoint.dungeon")));
    Dock->Tick(FGeometry::MakeRoot(FVector2D(1920,1080),FSlateLayoutTransform()),2,.016f);
    TestTrue(TEXT("Unpinned overlay stays hidden outdoors"),Dock->Windows[TEXT("waypoint.dungeon")].Widget->GetVisibility()==EVisibility::Collapsed);
    Dock->Toggle(TEXT("ucm"));Dock->Toggle(TEXT("monitor"));
    TestTrue(TEXT("Multiple independent plugin windows can be open"),H->IsPluginWindowOpen(TEXT("ucm"))&&H->IsPluginWindowOpen(TEXT("monitor")));
    P->Running=true;Dock->Toggle(TEXT("ucm"));
    TestTrue(TEXT("Hiding a plugin does not stop it or hide another"),P->Running&&!Dock->IsOpen(TEXT("ucm"))&&Dock->IsOpen(TEXT("monitor")));
    {
        Dock->Toggle(TEXT("ucm.log"));
        Ui->SetUiLocked(false);
        Dock->Move(TEXT("monitor"),FVector2D(20,30),false);
        Dock->Resize(TEXT("monitor"),FVector2D(-100,-100),false);
        const auto Position=Dock->Windows[TEXT("monitor")].Position,Size=Dock->Windows[TEXT("monitor")].Size;
        Dock->SaveOverlayLayout();
        FString Saved;TestTrue(TEXT("Window visibility is written to disk"),FFileHelper::LoadFileToString(Saved,*(Root/TEXT("settings.json"))));
        H->Settings=Json(*Saved);auto Restored=SNew(SACEPluginDesktop).Host(H);
        TestTrue(TEXT("Login restores open custom plugin and UCM log windows"),Restored->IsOpen(TEXT("monitor"))&&Restored->IsOpen(TEXT("ucm.log")));
        TestFalse(TEXT("Login preserves an explicitly closed UCM window"),Restored->IsOpen(TEXT("ucm")));
        TestEqual(TEXT("Logout mid-drag saves third-party window position"),Restored->Windows[TEXT("monitor")].Position,Position);
        TestEqual(TEXT("Logout mid-resize saves third-party window size"),Restored->Windows[TEXT("monitor")].Size,Size);
        Second->Enabled=false;Restored->Refresh();
        TestTrue(TEXT("Disabling hides UI without clearing its login preference"),!Restored->IsOpen(TEXT("monitor"))&&H->WasPluginWindowOpen(TEXT("monitor")));
        auto Disabled=SNew(SACEPluginDesktop).Host(H);TestFalse(TEXT("Disabled plugin cannot reopen at login"),Disabled->IsOpen(TEXT("monitor")));
        Second->Enabled=true;Restored->Refresh();TestTrue(TEXT("Re-enabling restores its saved open window"),Restored->IsOpen(TEXT("monitor")));
        Restored->Toggle(TEXT("monitor"));Restored->Toggle(TEXT("ucm.log"));
        auto Closed=SNew(SACEPluginDesktop).Host(H);TestFalse(TEXT("Explicitly closed custom/log windows stay closed"),Closed->IsOpen(TEXT("monitor"))||Closed->IsOpen(TEXT("ucm.log")));
        // Continue the original dock fixture without changing its intended state.
        H->SavePluginWindowVisibility(TEXT("monitor"),true);Dock->Hide(TEXT("ucm.log"));
        Ui->SetUiLocked(true);
    }
    {
        const auto Geometry=FGeometry::MakeRoot(FVector2D(1920,1080),FSlateLayoutTransform(1.5f));
        auto& W=Dock->Windows[TEXT("monitor")];const auto OriginalPlacement=W.Position;
        Ui->SetUiLocked(false);
        ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),0);Dock->Tick(Geometry,4,.016f);
        Dock->Move(TEXT("monitor"),FVector2D(-3000,-2000),true);
        TestTrue(TEXT("Unrestrained plugins can be dragged beyond either viewport edge"),W.Slot->GetOffset().Left<0&&W.Slot->GetOffset().Top<0);
        const auto Offscreen=W.Position;
        Dock->Tick(Geometry,5,.016f);TestEqual(TEXT("Layout does not undo allowed offscreen placement"),W.Position,Offscreen);
        ACERuntimeOptions::Set(TEXT("ConstrainUIWindows"),1);Dock->Tick(Geometry,6,.016f);
        TestTrue(TEXT("Enabling bounds recovers an offscreen window immediately"),W.Slot->GetOffset().Left>=0&&W.Slot->GetOffset().Top>=0);
        Dock->Move(TEXT("monitor"),FVector2D(5000,5000),false);
        const auto Bounds=W.Slot->GetOffset();
        TestTrue(TEXT("Plugin drag remains entirely inside the logical DPI viewport"),Bounds.Left+Bounds.Right<=1920&&Bounds.Top+Bounds.Bottom<=1080);
        W.Position=OriginalPlacement;Dock->Layout(W);Dock->Move(TEXT("monitor"),FVector2D::ZeroVector,true);Ui->SetUiLocked(true);
    }
    TestTrue(TEXT("Plugin bar is visible by default"),H->IsPluginBarVisible());
    H->SetPluginBarVisible(false);
    TestTrue(TEXT("Hiding bar removes its entire frame and hit targets"),Dock->Windows[TEXT("__bar")].Widget->GetVisibility()==EVisibility::Collapsed);
    TestTrue(TEXT("Hiding bar keeps plugins running and other windows open"),P->Running&&Dock->IsOpen(TEXT("monitor")));
    Dock->Toggle(TEXT("ucm"));
    TestTrue(TEXT("Plugin window remains accessible without the bar"),Dock->IsOpen(TEXT("ucm")));
    Dock->Toggle(TEXT("ucm"));
    FString BarSettings;TestTrue(TEXT("Bar visibility saves to disk"),FFileHelper::LoadFileToString(BarSettings,*(Root/TEXT("settings.json"))));
    H->Settings=Json(*BarSettings);auto HiddenDock=SNew(SACEPluginDesktop).Host(H);
    TestTrue(TEXT("Hidden bar stays hidden after recreation and preference reload"),HiddenDock->Windows[TEXT("__bar")].Widget->GetVisibility()==EVisibility::Collapsed);
    TestTrue(TEXT("Manager remains constructible while bar is hidden"),H->MakePanel()->GetVisibility()!=EVisibility::Collapsed);
    H->SetPluginBarVisible(true);
    TestTrue(TEXT("Overview can restore the existing bar"),Dock->Windows[TEXT("__bar")].Widget->GetVisibility()==EVisibility::Visible);
    {
        auto& Bar=Dock->Windows[TEXT("__bar")];
        const auto Initial=Bar.Position;const auto Expanded=Bar.Size;
        Ui->SetUiLocked(true);Dock->Move(TEXT("__bar"),FVector2D(40,60),true);
        TestEqual(TEXT("Global UI lock prevents moving the plugin bar"),Bar.Position,Initial);
        Ui->SetUiLocked(false);Dock->Move(TEXT("__bar"),FVector2D(40,60),true);
        TestEqual(TEXT("Unlocked expanded bar can be moved"),Bar.Position,Initial+FVector2D(40,60));
        H->SetPluginBarMinimized(true);
        TestEqual(TEXT("Minimized bar is only an 18px handle"),Bar.Slot->GetOffset().Bottom,18.f);
        TestEqual(TEXT("Minimizing retains expanded size"),Bar.Size,Expanded);
        TestTrue(TEXT("Minimizing leaves automation and other windows untouched"),P->Running&&Dock->IsOpen(TEXT("monitor")));
        Dock->Move(TEXT("__bar"),FVector2D(4000,4000),false);
        TestEqual(TEXT("Compact handle can reach the bottom right without its hidden body blocking movement"),Bar.Position,Dock->ViewSize-FVector2D(38,18));
        Dock->SaveOverlayLayout(); // Logout during a drag also persists the bar.
        FString Saved;FFileHelper::LoadFileToString(Saved,*(Root/TEXT("settings.json")));
        H->Settings=Json(*Saved);auto Restored=SNew(SACEPluginDesktop).Host(H);
        TestTrue(TEXT("Collapsed state survives settings reload"),H->IsPluginBarMinimized());
        TestEqual(TEXT("Collapsed position survives dock recreation"),Restored->Windows[TEXT("__bar")].Position,Bar.Position);
        H->SetPluginBarMinimized(false);
        TestEqual(TEXT("Expanding keeps saved position even when layout fits above the bottom edge"),Bar.Position,Dock->ViewSize-FVector2D(38,18));
        TestTrue(TEXT("Expanded body fits onscreen"),Bar.Slot->GetOffset().Top+Bar.Slot->GetOffset().Bottom<=Dock->ViewSize.Y);
        Bar.Position=Initial;Dock->Layout(Bar);Dock->Move(TEXT("__bar"),FVector2D::ZeroVector,true);
        Ui->SetUiLocked(true);
    }
    {
        auto& Bar=Dock->Windows[TEXT("__bar")];const auto Position=Bar.Position;
        const int32 Count=Dock->Buttons->NumSlots();
        TestFalse(TEXT("Plugin bar defaults to vertical"),H->IsPluginBarHorizontal());
        H->SetPluginBarHorizontal(true);
        TestTrue(TEXT("Orientation updates the existing icon row and scroll direction"),Dock->Buttons->GetOrientation()==Orient_Horizontal&&Dock->BarScroll->GetOrientation()==Orient_Horizontal);
        TestEqual(TEXT("Horizontal orientation preserves every icon"),Dock->Buttons->NumSlots(),Count);
        TestEqual(TEXT("Horizontal bar keeps square icon cells"),Bar.Size,FVector2D(4+34*Count,52));
        TestEqual(TEXT("Rotating the bar keeps its saved position"),Bar.Position,Position);
        TestTrue(TEXT("Rotating the bar keeps automation and other windows active"),P->Running&&Dock->IsOpen(TEXT("monitor")));
        FString Saved;TestTrue(TEXT("Orientation saves to disk"),FFileHelper::LoadFileToString(Saved,*(Root/TEXT("settings.json"))));
        H->Settings=Json(*Saved);auto Restored=SNew(SACEPluginDesktop).Host(H);
        TestTrue(TEXT("Horizontal bar restores across login"),H->IsPluginBarHorizontal()&&Restored->Buttons->GetOrientation()==Orient_Horizontal);
        H->SetPluginBarMinimized(true);
        TestEqual(TEXT("Horizontal bar minimizes to a short handle"),Dock->DisplaySize(Bar),FVector2D(70,18));
        H->SetPluginBarHorizontal(false);
        TestEqual(TEXT("Changing orientation while minimized updates the handle"),Dock->DisplaySize(Bar),FVector2D(38,18));
        H->SetPluginBarMinimized(false);
        TestEqual(TEXT("Returning to vertical restores one column"),Bar.Size,FVector2D(38,18+34*Count));
    }
    P->Running=false;Dock->Toggle(TEXT("ucm"));
    TestTrue(TEXT("Second click reopens its existing window"),Dock->IsOpen(TEXT("ucm")));
    const auto LockedSize=Dock->Windows[TEXT("ucm")].Size;
    Dock->Resize(TEXT("ucm"),FVector2D(-300,-150),true);
    TestEqual(TEXT("Shared UI lock prevents plugin resizing"),Dock->Windows[TEXT("ucm")].Size,LockedSize);
    Ui->SetUiLocked(false);
    Dock->Resize(TEXT("ucm"),FVector2D(-300,-150),true);
    TestTrue(TEXT("Window resize respects reduced minimum bounds"),Dock->Windows[TEXT("ucm")].Size.X>=96);
    TestEqual(TEXT("Window size is persisted"),H->GetPluginWindowPosition(TEXT("ucm.size"),FVector2D::ZeroVector),Dock->Windows[TEXT("ucm")].Size);
    Dock->Resize(TEXT("ucm"),FVector2D(300,150),false);
    Dock->Toggle(TEXT("monitor"));
    H->SetEnabled(TEXT("monitor"),false);Dock->Refresh();Dock->Toggle(TEXT("monitor"));
    TestFalse(TEXT("Disabled plugin window cannot open"),Dock->IsOpen(TEXT("monitor")));
    {
        Ui->SetUiLocked(false);
        const auto ExecutionOrder=H->Plugins;
        auto Manager=SNew(SACEPluginDesktop).Host(H).ManagerOnly(true);
        Manager->Tick(FGeometry::MakeRoot(FVector2D(1400,900),FSlateLayoutTransform()),1,.016f);
        TestTrue(TEXT("Plugin Manager opens through the shared window frame"),Manager->IsOpen(TEXT("__manager")));
        TestEqual(TEXT("Manager has no duplicate launcher bar"),Manager->Windows.Num(),1);
        Manager->Move(TEXT("__manager"),FVector2D(70,35),true);
        Manager->Resize(TEXT("__manager"),FVector2D(100,80),true);
        const auto Position=Manager->Windows[TEXT("__manager")].Position,Size=Manager->Windows[TEXT("__manager")].Size;
        H->SetPluginWindowPinned(TEXT("__manager"),true);
        Manager->Move(TEXT("__manager"),FVector2D(100,100),true);Manager->Resize(TEXT("__manager"),FVector2D(100,100),true);
        TestEqual(TEXT("Pinned manager cannot move"),Manager->Windows[TEXT("__manager")].Position,Position);
        TestEqual(TEXT("Pinned manager cannot resize"),Manager->Windows[TEXT("__manager")].Size,Size);
        TestFalse(TEXT("Pinned manager grip cannot initiate ordering"),ACEPluginOrder::BeginDrag(H,TEXT("huntledger"),TEXT("__manager")).IsEventHandled());
        Ui->SetQuickShowUi(true);
        TestTrue(TEXT("Quick-show allows manager reordering"),ACEPluginOrder::BeginDrag(H,TEXT("huntledger"),TEXT("__manager")).IsEventHandled());
        Ui->SetQuickShowUi(false);H->SetPluginWindowPinned(TEXT("__manager"),false);
        TFunction<TSharedPtr<SWidget>(TSharedRef<SWidget>,FName)> FindTag=[&](TSharedRef<SWidget> W,FName Tag)->TSharedPtr<SWidget>
        {if(W->GetTag()==Tag)return W;auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)if(auto Found=FindTag(Children->GetChildAt(I),Tag))return Found;return nullptr;};
        auto Grip=FindTag(Manager,TEXT("PluginOrderGrip.huntledger"));
        auto TargetRow=FindTag(Manager,TEXT("PluginManagerOrder.supplyledger"));
        auto PreservedCard=FindTag(Manager,TEXT("PluginOpen.huntledger"));
        if(TestTrue(TEXT("Manager exposes a grip and a full-row drop target"),Grip&&TargetRow))
        {
            const auto G=FGeometry::MakeRoot(FVector2D(500,100),FSlateLayoutTransform());
            const FPointerEvent Mouse(0,FVector2D(10,90),FVector2D(10,90),{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
            const auto Drag=Grip->OnDragDetected(G,Mouse).GetDragDropContent();
            if(TestTrue(TEXT("Manager grip starts a plugin drag"),Drag.IsValid()))
            {
                TestTrue(TEXT("Drop on lower half moves after target"),TargetRow->OnDrop(G,FDragDropEvent(Mouse,Drag)).IsEventHandled());
                auto Order=H->PluginDisplayOrder();TestEqual(TEXT("Manager drop sets shared order"),Order.IndexOfByKey(TEXT("huntledger")),Order.IndexOfByKey(TEXT("supplyledger"))+1);
                // Process the same deferred tick used by live manager ordering.
                TFunction<void(TSharedRef<SWidget>)> TickTree=[&](TSharedRef<SWidget> W){W->Tick(G,2,.016f);auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)TickTree(Children->GetChildAt(I));};TickTree(Manager);
                TestTrue(TEXT("Ordering preserves existing manager controls"),PreservedCard==FindTag(Manager,TEXT("PluginOpen.huntledger")));
            }
        }
        Dock->Refresh();H->SetPluginBarHorizontal(true);Dock->Refresh();
        auto BarButton=FindTag(Dock,TEXT("PluginBarButton.huntledger")),BarTarget=FindTag(Dock,TEXT("PluginBarOrder.supplyledger"));
        if(TestTrue(TEXT("Bar icons support dragging in horizontal layout"),BarButton&&BarTarget))
        {
            const auto G=FGeometry::MakeRoot(FVector2D(32,32),FSlateLayoutTransform());
            const FPointerEvent Mouse(0,FVector2D(2,16),FVector2D(2,16),{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
            auto Drag=BarButton->OnDragDetected(G,Mouse).GetDragDropContent();
            if(TestTrue(TEXT("Bar icon starts ordering rather than opening its window"),Drag.IsValid()))
            {
                Ui->SetUiLocked(true);TestFalse(TEXT("Locking during drag cancels reordering"),BarTarget->OnDrop(G,FDragDropEvent(Mouse,Drag)).IsEventHandled());Ui->SetUiLocked(false);
                TestTrue(TEXT("Left-half drop moves before horizontal icon"),BarTarget->OnDrop(G,FDragDropEvent(Mouse,Drag)).IsEventHandled());
            }
        }
        TestTrue(TEXT("UI ordering never changes plugin execution ordering"),H->Plugins==ExecutionOrder);
        const auto Order=H->PluginDisplayOrder();
        TestEqual(TEXT("Bar ordering is reflected in manager order"),Order.IndexOfByKey(TEXT("huntledger")),Order.IndexOfByKey(TEXT("supplyledger"))-1);
        H->SetPluginWindowPinned(TEXT("__manager"),true);Manager->SaveOverlayLayout();
        FString Saved;FFileHelper::LoadFileToString(Saved,*(Root/TEXT("settings.json")));H->Settings=Json(*Saved);
        TestTrue(TEXT("Shared plugin order survives disk reload"),H->PluginDisplayOrder()==Order);
        auto Reopened=SNew(SACEPluginDesktop).Host(H).ManagerOnly(true);
        TestEqual(TEXT("Manager position survives reload"),Reopened->Windows[TEXT("__manager")].Position,Position);
        TestEqual(TEXT("Manager size survives reload"),Reopened->Windows[TEXT("__manager")].Size,Size);
        TestTrue(TEXT("Manager pin survives reload"),H->IsPluginWindowPinned(TEXT("__manager")));
        TestFalse(TEXT("Unknown plugin cannot corrupt order"),H->MovePluginInOrder(TEXT("missing"),TEXT("ucm"),false));
        H->SetPluginWindowPinned(TEXT("__manager"),false);H->SetPluginBarHorizontal(false);Dock->Refresh();
    }
    if(FApp::CanEverRender())
    {
        // sRGB targets already encode linear colors. Avoid applying gamma twice.
        auto Panel=SNew(SACEPluginDesktop).Host(H).ManagerOnly(true);FWidgetRenderer Renderer(false,true);
        int32 LootTitles=0,OpenButtons=0,EditorControls=0;
        TFunction<void(TSharedRef<SWidget>)> InspectManager=[&](TSharedRef<SWidget> Widget)
        {
            if(Widget->GetTag().ToString().StartsWith(TEXT("PluginOpen.")))++OpenButtons;
            if((Widget->GetTypeAsString()==TEXT("SSlider")&&!Widget->GetTag().ToString().StartsWith(TEXT("PluginOpacity.")))
                ||Widget->GetTypeAsString()==TEXT("SMultiLineEditableTextBox"))++EditorControls;
            if((Widget->GetTypeAsString()==TEXT("STextBlock")||Widget->GetTypeAsString()==TEXT("SACEPluginText"))&&StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString()==TEXT("Loot Profile Editor"))++LootTitles;
            auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)InspectManager(Children->GetChildAt(I));
        };
        InspectManager(Panel);
        TestEqual(TEXT("Manager provides an explicit open control for every plugin"),OpenButtons,H->Plugins.Num());
        TestEqual(TEXT("Loot editor is listed once rather than embedded in manager"),LootTitles,1);
        TestEqual(TEXT("Manager does not build plugin editors until requested"),EditorControls,0);
        auto VisibleText=[](const TSharedRef<SWidget>& Root)
        {
            TArray<FString> Labels;
            TFunction<void(TSharedRef<SWidget>)> Visit=[&](TSharedRef<SWidget> W)
            {
                if(!W->GetVisibility().IsVisible())return;
                if(W->GetType()==TEXT("SACEPluginText")||W->GetType()==TEXT("STextBlock"))Labels.Add(StaticCastSharedRef<STextBlock>(W)->GetText().ToString());
                auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)Visit(Children->GetChildAt(I));
            };Visit(Root);return Labels;
        };
        const FVector2D PanelSize(740,800);
        auto* PanelTarget=FWidgetRenderer::CreateTargetFor(PanelSize,TF_Bilinear,true);
        int32 CenteredButtons=0;
        TFunction<void(TSharedRef<SWidget>)> CheckButtonLayout=[&](TSharedRef<SWidget> W)
        {
            if(!W->GetVisibility().IsVisible())return;
            if(W->GetType()==TEXT("SACEPluginButton")||W->GetType()==TEXT("SACEPluginTab"))
            {
                auto Button=StaticCastSharedRef<SButton>(W);
                const auto& G=Button->GetCachedGeometry();const auto& C=Button->GetContent()->GetCachedGeometry();
                if(G.GetLocalSize().X>0&&G.GetLocalSize().Y>0)
                {
                    ++CenteredButtons;
                    TestTrue(TEXT("Native plugin button content stays centered after layout"),
                        G.AbsoluteToLocal(C.LocalToAbsolute(C.GetLocalSize()*.5)).Equals(G.GetLocalSize()*.5,.1f));
                    const auto At=G.AbsoluteToLocal(C.GetAbsolutePosition());
                    TestTrue(TEXT("Native button content fits within all four borders"),At.X>=-.1&&At.Y>=-.1);
                }
            }
            auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)CheckButtonLayout(Children->GetChildAt(I));
        };
        for(const TCHAR* Id:{TEXT("ucm"),TEXT("looteditor"),TEXT("itemtools"),TEXT("waypoint"),TEXT("waypoint.map"),TEXT("ucmmicro"),TEXT("ucm.log"),TEXT("supplyledger"),TEXT("huntledger"),TEXT("autocraft"),TEXT("actiondeck"),TEXT("thirdeye")})
        {
            auto NativePanel=H->MakePanel(Id);const auto Labels=VisibleText(NativePanel);
            for(int32 Pass=0;Pass<3;++Pass){++GFrameCounter;Renderer.DrawWidget(PanelTarget,NativePanel,PanelSize,0);FlushRenderingCommands();}
            CheckButtonLayout(NativePanel);
            if(FString(Id)==TEXT("ucm")||FString(Id)==TEXT("itemtools"))
            {
                TArray<FColor> Pixels;FReadSurfaceDataFlags Flags;Flags.SetLinearToGamma(false);
                PanelTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,Flags);TArray64<uint8> Png;
                FImageUtils::PNGCompressImageArray(int32(PanelSize.X),int32(PanelSize.Y),Pixels,Png);
                FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("PluginButtons-%s.png"),Id)));
            }
            for(const TCHAR* Title:{TEXT("UCM"),TEXT("Loot Profile Editor"),TEXT("Item Tools"),TEXT("Waypoint"),TEXT("Explore Dereth"),TEXT("UCM Micro"),TEXT("UCM Log"),TEXT("Supply Ledger"),TEXT("Hunt Ledger")})
                TestFalse(FString(Id)+TEXT(" has no duplicate content heading: ")+Title,Labels.Contains(Title));
            if(FString(Id)==TEXT("ucm"))TestTrue(TEXT("Compact UCM header keeps Start and current status"),
                Labels.Contains(TEXT("Start"))&&Labels.Contains(TEXT("Current status: ")+H->UCMCurrentStatus())&&!Labels.Contains(TEXT("Setup: ")+P->ProfileName));
            if(FString(Id)==TEXT("looteditor"))TestTrue(TEXT("Loot editor keeps its tabs"),Labels.Contains(TEXT("Loot rules"))&&Labels.Contains(TEXT("Vendors")));
            if(FString(Id)==TEXT("itemtools"))TestTrue(TEXT("Item Tools keeps stop and task controls"),Labels.Contains(TEXT("Stop"))&&Labels.Contains(TEXT("Give")));
        }
        PanelTarget->ReleaseResource();
        TestTrue(TEXT("Layout regression covers native panels and their shared controls"),CenteredButtons>40);
        const auto ManagerLabels=VisibleText(Panel);
        TestEqual(TEXT("Manager has one title in its window header"),ManagerLabels.FilterByPredicate([](const FString& S){return S==TEXT("Plugin Manager");}).Num(),1);
        TestFalse(TEXT("Manager has no duplicate Plugins content heading"),ManagerLabels.Contains(TEXT("Plugins")));
        TestTrue(TEXT("Embedded UCM retains a heading without a frame"),VisibleText(H->MakeUCMPanel()).Contains(TEXT("UCM")));
        TestTrue(TEXT("Embedded loot editor retains its heading"),VisibleText(H->MakeUCMPanel(TEXT("Standalone loot"))).Contains(TEXT("Loot Profile Editor")));
        TestTrue(TEXT("Embedded Item Tools retains its heading"),VisibleText(H->MakeItemToolsPanel()).Contains(TEXT("Item Tools")));
        TestTrue(TEXT("Embedded map retains its heading"),VisibleText(H->MakeWaypointPanel(true)).Contains(TEXT("Explore Dereth")));
        auto MapPanel=H->MakePanel(TEXT("waypoint.map"));TSharedPtr<SButton> Switch;
        TFunction<void(TSharedRef<SWidget>)> FindMapSwitch=[&](TSharedRef<SWidget> W)
        {
            if(W->GetType()==TEXT("SACEPluginButton")&&VisibleText(W).Contains(TEXT("Waypoint settings")))Switch=StaticCastSharedRef<SButton>(W);
            auto* Children=W->GetChildren();for(int32 I=0;I<Children->Num();++I)FindMapSwitch(Children->GetChildAt(I));
        };FindMapSwitch(MapPanel);
        if(TestTrue(TEXT("Map settings switch is available"),Switch.IsValid()))
        {Switch->SimulateClick();TestFalse(TEXT("Switching map to settings does not reintroduce a heading"),VisibleText(MapPanel).Contains(TEXT("Waypoint")));}
        FReadSurfaceDataFlags ReadFlags;ReadFlags.SetLinearToGamma(false);
        auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(740,530),TF_Bilinear,true);
        for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Panel,FVector2D(740,530),0);FlushRenderingCommands();}
        TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
        TestEqual(TEXT("Plugin panel renders"),Pixels.Num(),740*530);
        TArray64<uint8> Png;FImageUtils::PNGCompressImageArray(740,530,Pixels,Png);
        FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/Plugins.png")));
        Target->ReleaseResource();
        auto* DockTarget=FWidgetRenderer::CreateTargetFor(FVector2D(1280,720),TF_Bilinear,true);
        for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(DockTarget,Dock,FVector2D(1280,720),0);FlushRenderingCommands();}
        for(const auto& Pair:Dock->Windows)
        {
            const auto& G=Pair.Value.Widget->GetCachedGeometry();
            const FVector2D Local=Dock->GetCachedGeometry().AbsoluteToLocal(G.GetAbsolutePosition());
            TestTrue(TEXT("Plugin window starts inside the viewport"),Local.X>=0&&Local.Y>=0);
            TestTrue(TEXT("Plugin window ends inside the viewport"),Local.X+G.GetLocalSize().X<=1281&&Local.Y+G.GetLocalSize().Y<=721);
        }
        Pixels.Reset();DockTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
        TestEqual(TEXT("Plugin bar and window render"),Pixels.Num(),1280*720);
        Png.Reset();FImageUtils::PNGCompressImageArray(1280,720,Pixels,Png);
        FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/PluginDock.png")));
        H->SetPluginBarHorizontal(true);
        for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(DockTarget,Dock,FVector2D(1280,720),0);FlushRenderingCommands();}
        Pixels.Reset();DockTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
        Png.Reset();FImageUtils::PNGCompressImageArray(1280,720,Pixels,Png);
        FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/PluginDock-Horizontal.png")));
        H->SetPluginBarHorizontal(false);
        TSharedPtr<SWidget> Handle,Minimize;
        TFunction<void(TSharedRef<SWidget>)> FindBarControls=[&](TSharedRef<SWidget> Widget)
        {
            if(Widget->GetTag()==TEXT("PluginBarHandle"))Handle=Widget;
            if(Widget->GetTag()==TEXT("PluginBarMinimize"))Minimize=Widget;
            auto* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)FindBarControls(Children->GetChildAt(I));
        };
        FindBarControls(Dock->Windows[TEXT("__bar")].Widget.ToSharedRef());
        TestTrue(TEXT("Bar exposes a grip and minimize control"),Handle.IsValid()&&Minimize.IsValid());
        if(Handle&&Minimize)
        {
            const auto G=Handle->GetCachedGeometry();const auto At=G.LocalToAbsolute(FVector2D(10,9));
            const FPointerEvent Down(0,At,At,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState());
            Ui->SetUiLocked(true);TestFalse(TEXT("Locked grip never captures mouse"),Handle->OnMouseButtonDown(G,Down).GetMouseCaptor().IsValid());
            Ui->SetUiLocked(false);TestTrue(TEXT("Unlocked grip captures the frame for dragging"),Handle->OnMouseButtonDown(G,Down).GetMouseCaptor()==Dock->Windows[TEXT("__bar")].Widget);
            Dock->Windows[TEXT("__bar")].Widget->OnMouseButtonUp(G,FPointerEvent(0,At,At,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
            Ui->SetUiLocked(true);
            auto ClickMinimize=[&]()
            {
                // FWidgetRenderer has no native window to acquire mouse capture.
                // Invoke Slate's test click on the actual rendered button.
                StaticCastSharedPtr<SButton>(Minimize)->SimulateClick();
            };
            ClickMinimize();TestTrue(TEXT("Minimize button works while UI is locked"),H->IsPluginBarMinimized());
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(DockTarget,Dock,FVector2D(1280,720),0);FlushRenderingCommands();}
            Pixels.Reset();DockTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
            Png.Reset();FImageUtils::PNGCompressImageArray(1280,720,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/PluginDock-Minimized.png")));
            ClickMinimize();TestFalse(TEXT("Compact handle restores the bar while locked"),H->IsPluginBarMinimized());
        }
        DockTarget->ReleaseResource();
        {
            const FVector2D Size(270,360);auto Micro=H->MakeUCMMicroPanel();
            auto* MicroTarget=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(MicroTarget,Micro,Size,0);FlushRenderingCommands();}
            TArray<TSharedRef<SCheckBox>> Toggles;
            TFunction<void(TSharedRef<SWidget>)> Visit=[&](TSharedRef<SWidget> Widget)
            {if(Widget->GetTypeAsString()==TEXT("SCheckBox"))Toggles.Add(StaticCastSharedRef<SCheckBox>(Widget));auto* Children=Widget->GetChildren();for(int I=0;I<Children->Num();++I)Visit(Children->GetChildAt(I));};
            Visit(Micro);TestEqual(TEXT("Micro includes ten activities plus Follow Character and Corners"),Toggles.Num(),12);
            const auto Labels=VisibleText(Micro);
            for(const TCHAR* Label:{TEXT("F"),TEXT("CF"),TEXT("FC"),TEXT("C"),TEXT("Current status")})
                TestTrue(FString(TEXT("Micro includes quick control/status: "))+Label,Labels.Contains(Label));
            const auto* LootToggle=Toggles.FindByPredicate([&](const auto& Toggle){return VisibleText(Toggle).Contains(TEXT("Looting"));});
            if(TestNotNull(TEXT("Micro has a looting control"),LootToggle))
            {
                auto Loot=*LootToggle;const bool Before=Loot->IsChecked();const auto Geometry=Loot->GetCachedGeometry();const FVector2D At=Geometry.LocalToAbsolute(Geometry.GetLocalSize()*.5);
                Loot->OnMouseButtonDown(Geometry,FPointerEvent(0,At,At,{EKeys::LeftMouseButton},EKeys::LeftMouseButton,0,FModifierKeysState()));
                Loot->OnMouseButtonUp(Geometry,FPointerEvent(0,At,At,{},EKeys::LeftMouseButton,0,FModifierKeysState()));
                bool Enabled=false;P->Profile->TryGetBoolField(TEXT("looting"),Enabled);
                TestEqual(TEXT("Micro changes the actual active loot setting"),Enabled,!Before);
                const FString Name=P->ProfileName;TestTrue(TEXT("Micro setting persists across profile reload"),H->LoadProfile(TEXT("ucm"),Name));
                TestEqual(TEXT("Micro follows reloaded profile instead of retaining a stale copy"),Loot->IsChecked(),!Before);
            }
            Pixels.Reset();MicroTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
            Png.Reset();FImageUtils::PNGCompressImageArray(270,360,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation/UCM-Micro.png")));MicroTarget->ReleaseResource();
        }
        for(bool Map:{false,true})for(int Width:{780,450})
        {
            const FVector2D Size(Width,760);auto Widget=H->MakeWaypointPanel(Map,false,false);
            auto* WaypointTarget=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(WaypointTarget,Widget,Size,0);FlushRenderingCommands();}
            Pixels.Reset();WaypointTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
            TestEqual(TEXT("Waypoint panel renders"),Pixels.Num(),Width*760);
            Png.Reset();FImageUtils::PNGCompressImageArray(Width,760,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("Waypoint-%s-%d.png"),Map?TEXT("Map"):TEXT("Settings"),Width)));WaypointTarget->ReleaseResource();
        }
        for(const FString Page:{TEXT("Overview"),TEXT("Buffs"),TEXT("Buff others"),TEXT("Combat"),TEXT("Recovery"),TEXT("Route"),TEXT("Loot"),TEXT("Vendors"),TEXT("Loot editor"),TEXT("Salvage groups"),TEXT("Rules"),TEXT("Metas"),TEXT("Profiles"),TEXT("Standalone loot"),TEXT("Import")})
        for(int Width:{780,450})
        {
            const int Height=Page==TEXT("Loot editor")?1800:(Page==TEXT("Recovery")||Page==TEXT("Combat"))?2400:760;
            if(Page==TEXT("Vendors"))P->Profile->SetArrayField(TEXT("vendor_rules"),{MakeShared<FJsonValueObject>(Json(TEXT(R"({"server":"Test","vendor_name":"Master Arcanist","vendor_wcid":2,"item_name":"Platinum Scarab","item_wcid":3,"quantity":100})")))});
            auto PageWidget=H->MakeUCMPanel(Page,false);const FVector2D Size(Width,Height);
            auto* PageTarget=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
            for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(PageTarget,PageWidget,Size,0);FlushRenderingCommands();}
            Pixels.Reset();PageTarget->GameThread_GetRenderTargetResource()->ReadPixels(Pixels,ReadFlags);
            TestEqual(*(Page+TEXT(" renders at ")+FString::FromInt(Width)),Pixels.Num(),Width*Height);
            Png.Reset();FImageUtils::PNGCompressImageArray(Width,Height,Pixels,Png);
            FFileHelper::SaveArrayToFile(Png,*(FPaths::ProjectSavedDir()/TEXT("Automation")/FString::Printf(TEXT("UCM-%s-%d.png"),*Page,Width)));
            PageTarget->ReleaseResource();
        }
    }
    return true;
}
#endif
