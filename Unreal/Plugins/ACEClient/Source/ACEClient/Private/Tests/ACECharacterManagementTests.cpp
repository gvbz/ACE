#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "ACESession.h"
#include "Protocol/ACEBinaryReader.h"
#include "Protocol/ACEBinaryWriter.h"
#include "ACEDatSubsystem.h"
#include "UI/ACEUICharSelectBinder.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACERetailTextEntry.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "ImageUtils.h"
#include "RenderingThread.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECharacterManagementTest,"ACE.RetailParity.CharacterManagement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECharacterManagementTest::RunTest(const FString&)
{
    FACESession Session;
    Session.State=EACESessionState::CharacterSelect; Session.AccountName=TEXT("test-account");
    FACECharacterInfo First; First.CharacterId=0x50000011; First.Name=TEXT("First adventurer");
    FACECharacterInfo Second; Second.CharacterId=0x50000022; Second.Name=TEXT("Second adventurer");
    Session.Characters={First,Second};
    TArray<uint8> Bytes;
    TestTrue(TEXT("Delete resolves selected GUID to original server slot"),Session.BuildCharacterMutation(Second.CharacterId,false,Bytes));
    FACEBinaryReader Delete(Bytes);
    TestEqual(TEXT("Delete carries account"),Delete.ReadString16L(),Session.AccountName);
    TestEqual(TEXT("Delete carries slot, not GUID"),Delete.ReadUInt32(),1u);
    TestEqual(TEXT("No extra delete payload"),Delete.Remaining(),0);
    TestFalse(TEXT("Unknown GUID cannot delete slot zero"),Session.BuildCharacterMutation(123,false,Bytes));
    TestFalse(TEXT("Live character cannot be restored"),Session.BuildCharacterMutation(Second.CharacterId,true,Bytes));
    Session.Characters[1].DeleteSeconds=3600;
    TestFalse(TEXT("Pending deletion cannot be deleted again"),Session.BuildCharacterMutation(Second.CharacterId,false,Bytes));
    TestFalse(TEXT("Pending deletion cannot enter world"),Session.EnterWorld(Second.CharacterId));
    TestTrue(TEXT("Restore request available during grace period"),Session.BuildCharacterMutation(Second.CharacterId,true,Bytes));
    FACEBinaryReader Restore(Bytes); TestEqual(TEXT("Restore carries GUID"),Restore.ReadUInt32(),uint32(Second.CharacterId));
    Session.PendingCharacterMutation=Second.CharacterId; Session.bRestoringCharacter=true;
    TestFalse(TEXT("Pending request prevents a duplicate"),Session.BuildCharacterMutation(First.CharacterId,false,Bytes));
    FACEBinaryWriter Reply; Reply.WriteUInt32(1); Reply.WriteUInt32(Second.CharacterId); Reply.WriteString16L(Second.Name); Reply.WriteUInt32(0);
    FACEBinaryReader ReplyReader(Reply.GetData()); Session.HandleCharacterRestored(ReplyReader);
    TestEqual(TEXT("Restore updates authoritative roster"),Session.Characters[1].DeleteSeconds,0);
    TestFalse(TEXT("Restore releases pending state"),Session.IsCharacterManagementPending());
    Session.State=EACESessionState::InWorld;
    TestFalse(TEXT("In-world deletion rejected"),Session.BuildCharacterMutation(First.CharacterId,false,Bytes));

    auto* GI=NewObject<UGameInstance>(); auto* Dat=NewObject<UACEDatSubsystem>(GI);
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if (!Dat->LoadDatDirectory(DatDirectory)) return false;
    auto* Resources=NewObject<UACEUIResourceResolver>(); Resources->Initialize(Dat);
    auto* Manager=NewObject<UACEUIElementManager>(); Manager->Initialize();
    auto* Layout=NewObject<UACEUILayoutResolver>(); Layout->Initialize(Dat,Manager);
    if (!Layout->LoadLayout(0x21000004)) return false;
    auto* Canvas=NewObject<UACEUICanvasWidget>(); Canvas->Initialize();
    Canvas->InitializeCanvas(Manager); Canvas->SetResourceResolver(Resources); auto Slate=Canvas->TakeWidget();
    auto* Binder=NewObject<UACEUICharSelectBinder>();
    Binder->Initialize(nullptr,Manager,Canvas,nullptr,{First,Second},TEXT("Retail parity test")); Canvas->SetCharSelectBinder(Binder);
    Binder->SelectCharacterIndex(1); Binder->ShowDeleteConfirmation();
    TestEqual(TEXT("Retail deletion confirmation keyword"),Binder->ConfirmationWord,FString(TEXT("DELETE")));
    AddInfo(FString::Printf(TEXT("Retail delete prompt: %s"),*Binder->DialogMessage));
    TestTrue(TEXT("Prompt names the selected character"),Binder->DialogMessage.Contains(Second.Name));
    TestTrue(TEXT("Delete opens a modal"),Binder->Dialog && Binder->Dialog->bVisible);
    const auto List=Manager->FindElementByName(TEXT("CharacterListBox"));
    Binder->TryHandleOverlayClick(FVector2D(List->GetScreenOrigin())+FVector2D(10,5));
    TestEqual(TEXT("Modal blocks roster selection"),Binder->SelectedCharacterId,Second.CharacterId);
    Binder->ConfirmationEntry->SetText(FText::FromString(TEXT("no"))); Binder->ConfirmDelete();
    TestEqual(TEXT("Incorrect response cannot approve deletion"),Binder->DeleteCandidateId,Second.CharacterId);
    Binder->ConfirmationEntry->SetText(FText::FromString(TEXT("DELETE")));
    Binder->ConfirmationCommitted(FText::FromString(TEXT("DELETE")),ETextCommit::OnUserMovedFocus);
    TestTrue(TEXT("Closing keyboard does not approve deletion"),Binder->Dialog->bVisible);

    auto Capture=[&](const TCHAR* Name)
    {
        if (!FApp::CanEverRender()) return;
        FWidgetRenderer Renderer(true,true);
        const FVector2D Size(1280,960); auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
        for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Slate,Size,0.f);FlushRenderingCommands();Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f);}
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(1280,960,Pixels,PNG);
        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/RetailParity")/Name));
    };
    Capture(TEXT("DeleteConfirmation.png"));
    Binder->CloseDialog(); TestEqual(TEXT("Cancel discards candidate"),Binder->DeleteCandidateId,0);
    Binder->Characters[1].DeleteSeconds=3600; Binder->SyncButtonStates();
    TestTrue(TEXT("Restore replaces Delete"),Manager->FindElementByName(TEXT("RestoreCharacterButton"))->bVisible);
    TestFalse(TEXT("Delete hidden during grace period"),Manager->FindElementByName(TEXT("DeleteCharacterButton"))->bVisible);
    Binder->SetCreditsVisible(true);
    TestTrue(TEXT("Credits opens"),Binder->bCredits);
    TestTrue(TEXT("Credits uses original DAT text"),Binder->CreditText.Len()>1000);
    AddInfo(FString::Printf(TEXT("Credits text sample: %s"),*Binder->CreditText.Left(650)));
    TestEqual(TEXT("Credits has scrolling retail pictures"),Binder->CreditPictures.Num(),3);
    Binder->TickCredits(15); Capture(TEXT("Credits.png"));
    TestTrue(TEXT("Credits advances without changing session"),Binder->CreditsText && Binder->CreditsText->Y<600);
    Binder->TryHandleOverlayClick(FVector2D(400,300)); TestFalse(TEXT("Click exits credits"),Binder->bCredits);
    TestEqual(TEXT("Credits preserves selected character"),Binder->SelectedCharacterId,Second.CharacterId);
    Binder->Shutdown(); Canvas->SetCharSelectBinder(nullptr);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACECharacterScreenRenderingTest,"ACE.RetailParity.CharacterScreenRendering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACECharacterScreenRenderingTest::RunTest(const FString&)
{
    if (!FApp::CanEverRender()) { AddError(TEXT("Character screen regression requires rendering.")); return false; }
    auto* GI=NewObject<UGameInstance>(); auto* Dat=NewObject<UACEDatSubsystem>(GI);
    FString DatDirectory=TEXT("C:/Turbine/Asheron's Call");
    FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),DatDirectory);
    if (!TestTrue(TEXT("Retail DAT opens"),Dat->LoadDatDirectory(DatDirectory))) return false;
    auto* Resources=NewObject<UACEUIResourceResolver>(); Resources->Initialize(Dat);
    auto* Manager=NewObject<UACEUIElementManager>(); Manager->Initialize();
    auto* Layout=NewObject<UACEUILayoutResolver>(); Layout->Initialize(Dat,Manager);
    if (!TestTrue(TEXT("Retail character layout loads"),Layout->LoadLayout(0x21000004))) return false;
    auto* Canvas=NewObject<UACEUICanvasWidget>(); Canvas->Initialize();
    Canvas->InitializeCanvas(Manager); Canvas->SetResourceResolver(Resources); auto Slate=Canvas->TakeWidget();
    FACECharacterInfo Character; Character.CharacterId=0x50000011; Character.Name=TEXT("Adventurer");
    auto* Binder=NewObject<UACEUICharSelectBinder>();
    Binder->Initialize(nullptr,Manager,Canvas,nullptr,{Character},TEXT("Rendering test")); Canvas->SetCharSelectBinder(Binder);
    FWidgetRenderer Renderer(true,true);
    TArray<FColor> Reference;
    // The reference is native retail size. Larger screens must translate the
    // same artwork without clipping it to the ownership-only synthetic root.
    for (const FIntPoint Size : {FIntPoint(800,600),FIntPoint(1280,720),FIntPoint(1920,1080),
        FIntPoint(2400,1200),FIntPoint(2560,1440),FIntPoint(3440,1440),FIntPoint(3840,2160),
        FIntPoint(3840,1080),FIntPoint(5120,1440),FIntPoint(7680,2160),
        FIntPoint(800,1800),FIntPoint(1100,900),FIntPoint(800,600)})
    {
        auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(Size),TF_Bilinear,true);
        for (int Pass=0;Pass<4;++Pass)
        {
            Renderer.DrawWidget(Target,Slate,FVector2D(Size),0.f); FlushRenderingCommands();
            Canvas->NativeTick(Canvas->GetCachedGeometry(),0.f);
        }
        TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        if (!TestEqual(TEXT("Capture has the requested dimensions"),Pixels.Num(),Size.X*Size.Y)) continue;
        const FIntPoint Offset((Size.X-800)/2,(Size.Y-600)/2);
        TArray<FColor> Panel; Panel.Reserve(800*600);
        for (int Y=0;Y<600;++Y) for (int X=0;X<800;++X) Panel.Add(Pixels[(Y+Offset.Y)*Size.X+X+Offset.X]);
        if (Reference.IsEmpty())
        {
            Reference=Panel;
            int32 ArtworkPixels=0;
            for (const FColor& Color : Reference) if (Color.R+int32(Color.G)+Color.B>90) ++ArtworkPixels;
            TestTrue(TEXT("Reference contains the retail panel artwork"),ArtworkPixels>300000);
        }
        int32 Changed=0;
        for (int I=0;I<Panel.Num();++I)
            if (FMath::Max3(FMath::Abs(int32(Panel[I].R)-Reference[I].R),FMath::Abs(int32(Panel[I].G)-Reference[I].G),
                FMath::Abs(int32(Panel[I].B)-Reference[I].B))>4) ++Changed;
        TestTrue(*FString::Printf(TEXT("%dx%d preserves centered retail artwork (%d changed pixels)"),Size.X,Size.Y,Changed),Changed<1000);
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
        FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(
            TEXT("Automation/RetailParity/CharacterSelect%dx%d.png"),Size.X,Size.Y)));
    }
    Binder->Shutdown(); Canvas->SetCharSelectBinder(nullptr);
    return true;
}
#endif
