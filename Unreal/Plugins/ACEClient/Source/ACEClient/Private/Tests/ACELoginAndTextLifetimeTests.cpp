#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACELoginSettings.h"
#include "ACELoginWidget.h"
#include "UI/ACERetailTextBlock.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SWidget.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GarbageCollection.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderingThread.h"
#if PLATFORM_LINUX || PLATFORM_MAC
#include <sys/stat.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACELocalLoginSettingsTest,"ACE.RetailParity.LocalLoginSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACELocalLoginSettingsTest::RunTest(const FString& Parameters)
{
    const auto* Defaults=GetDefault<UACELoginWidget>();
    TestTrue(TEXT("Fresh login has no supplied server or credentials"),
        Defaults->DefaultHost.IsEmpty() && Defaults->DefaultAccount.IsEmpty() && Defaults->DefaultPassword.IsEmpty());
    const FString Path=FPaths::ProjectSavedDir()/TEXT("Automation/LoginSettings-")+FGuid::NewGuid().ToString()+TEXT(".dat");
    FACELoginSettings Entered;
    Entered.Host=TEXT("test.example.invalid"); Entered.Port=TEXT("9001");
    Entered.Account=TEXT("Local persistence fixture"); Entered.Password=TEXT("Test-only secret \u2603 \u00e9");
    FACELoginSettings Loaded;
    TestFalse(TEXT("Missing file does not invent saved values"),ACELoginSettings::Load(Loaded,Path));
    if (!TestTrue(TEXT("Local entries save successfully"),ACELoginSettings::Save(Entered,Path))) return false;
    TestTrue(TEXT("Entries restore in a new settings instance"),ACELoginSettings::Load(Loaded,Path));
    TestEqual(TEXT("Address round trips"),Loaded.Host,Entered.Host);
    TestEqual(TEXT("Port round trips"),Loaded.Port,Entered.Port);
    TestEqual(TEXT("Account round trips"),Loaded.Account,Entered.Account);
#if PLATFORM_LINUX || PLATFORM_MAC
    TestTrue(TEXT("Local restores account metadata without a saved password"),Loaded.Password.IsEmpty());
#else
    TestTrue(TEXT("Unicode password round trips without printing it"),Loaded.Password==Entered.Password);
#endif
    TArray<uint8> Bytes; FFileHelper::LoadFileToArray(Bytes,*Path);
    const FTCHARToUTF8 Secret(*Entered.Password);
    bool ContainsSecret=false;
    for (int32 I=0;I+Secret.Length()<=Bytes.Num();++I)
        ContainsSecret |= FMemory::Memcmp(Bytes.GetData()+I,Secret.Get(),Secret.Length())==0;
    TestFalse(TEXT("Local file does not contain the plaintext password"),ContainsSecret);
    TArray<uint8> Tampered = Bytes; Tampered.Last() ^= 1;
    FFileHelper::SaveArrayToFile(Tampered,*Path);
    TestFalse(TEXT("Damaged profile data is rejected"),ACELoginSettings::Load(Loaded,Path));
#if PLATFORM_LINUX || PLATFORM_MAC
    TestTrue(TEXT("Damaged Local profile leaves account metadata unchanged"),Loaded.Account==Entered.Account && Loaded.Password.IsEmpty());
#else
    TestTrue(TEXT("Invalid ciphertext cannot replace previously loaded credentials"),Loaded.Password==Entered.Password);
#endif
    Entered.Host.Empty(); Entered.Account.Empty(); Entered.Password.Empty(); Entered.Port.Empty();
    TestTrue(TEXT("Clearing fields replaces previously saved values"),ACELoginSettings::Save(Entered,Path));
    TestTrue(TEXT("Cleared fields restore"),ACELoginSettings::Load(Loaded,Path));
    TestTrue(TEXT("Cleared credentials cannot return at the next launch"),Loaded.Host.IsEmpty()
        && Loaded.Port.IsEmpty() && Loaded.Account.IsEmpty() && Loaded.Password.IsEmpty());
    FFileHelper::SaveArrayToFile(TArray<uint8>{1,2,3,4},*Path);
    TestFalse(TEXT("Damaged settings are rejected without crashing"),ACELoginSettings::Load(Loaded,Path));
    TestTrue(TEXT("Damaged settings leave empty defaults untouched"),Loaded.Host.IsEmpty() && Loaded.Password.IsEmpty());
#if PLATFORM_LINUX || PLATFORM_MAC
    Entered.Host=TEXT("test.example.invalid");Entered.Port=TEXT("9001");Entered.Account=TEXT("Saved login fixture");Entered.Password=TEXT("Local-only test secret \u2603");
    Entered.MigrateLegacy();Entered.bSavePasswordsLocally=true;
    TestTrue(TEXT("Local password opt-in saves"),ACELoginSettings::Save(Entered,Path));
    FACELoginSettings Restarted;
    TestTrue(TEXT("Local password preference restores on restart"),ACELoginSettings::Load(Restarted,Path)&&Restarted.bSavePasswordsLocally);
    TestTrue(TEXT("Legacy and account password fields both round trip"),Restarted.Password==Entered.Password&&Restarted.Accounts.Num()==1&&Restarted.Accounts[0].Password==Entered.Password);
    struct stat Info{};
    TestTrue(TEXT("Local profile is private from the moment it is created"),stat(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Path)),&Info)==0&&(Info.st_mode&0777)==0600);
    Entered.bSavePasswordsLocally=false;
    TestTrue(TEXT("Disabling password saving rewrites the file"),ACELoginSettings::Save(Entered,Path));
    TestTrue(TEXT("Disabled option persists and clears both saved password fields"),ACELoginSettings::Load(Restarted,Path)&&!Restarted.bSavePasswordsLocally&&Restarted.Password.IsEmpty()&&Restarted.Accounts[0].Password.IsEmpty());
    FString Plain;FFileHelper::LoadFileToString(Plain,*Path);
    TestFalse(TEXT("Disabling opt-in removes the secret from disk"),Plain.Contains(Entered.Password));
    TestFalse(TEXT("Disabling persistence preserves this session's credentials"),Entered.Password.IsEmpty());
#endif
    IFileManager::Get().Delete(*Path,false,true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailTextLifetimeTest,"ACE.RetailParity.TextLifetime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FACERetailTextLifetimeTest::RunTest(const FString& Parameters)
{
    // Slate may retain a detached label after its UMG owner is collected. In
    // the reported stack this fell into PlainTextLayoutMarshaller::SetText,
    // reading the StrikeBrush pointer installed by UTextBlock::SynchronizeProperties.
    TStrongObjectPtr<UACERetailTextBlock> Owner(NewObject<UACERetailTextBlock>());
    Owner->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),14));
    Owner->SetText(FText::FromString(TEXT("Detached retail label")));
    auto Slate=Owner->TakeWidget();
    Slate->SlatePrepass(1.f);
    TestTrue(TEXT("Live label still supports fallback before DAT is available"),Slate->GetDesiredSize().X>0);
    TWeakObjectPtr<UACERetailTextBlock> Released(Owner.Get());
    Owner.Reset();
    CollectGarbage(RF_NoFlags);
    TestFalse(TEXT("Retained Slate does not keep the detached UObject alive"),Released.IsValid());
    Slate->Invalidate(EInvalidateWidgetReason::Layout);
    Slate->SlatePrepass(1.f);
    TestTrue(TEXT("Prepass after owner release does not access stale text style"),Slate->GetDesiredSize().IsNearlyZero());
    if (FApp::CanEverRender())
    {
        FWidgetRenderer Renderer(true,true);
        TStrongObjectPtr<UTextureRenderTarget2D> Target(FWidgetRenderer::CreateTargetFor(FVector2D(256,32),TF_Bilinear,true));
        Renderer.DrawWidget(Target.Get(),Slate,FVector2D(256,32),0.f);
        FlushRenderingCommands();
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        bool Clear=!Pixels.IsEmpty();
        for (const FColor Pixel:Pixels) Clear &= Pixel.A==0;
        TestTrue(TEXT("Paint after owner release emits no stale text"),Clear);
    }
    return true;
}
#endif
