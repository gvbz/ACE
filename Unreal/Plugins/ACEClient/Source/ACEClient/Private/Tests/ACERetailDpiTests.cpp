#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/ACETestDatDirectory.h"
#include "ACEDatSubsystem.h"
#include "ACERuntimeOptions.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACERetailTextBlock.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACERetailDpiTest,"ACE.RetailParity.FractionalDPI",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FACERetailDpiTest::RunTest(const FString&)
{
    const float OriginalScale=ACERuntimeOptions::Get(TEXT("DesktopUIScale"));
    ON_SCOPE_EXIT{ACERuntimeOptions::Set(TEXT("DesktopUIScale"),OriginalScale);};
    ACERuntimeOptions::Set(TEXT("DesktopUIScale"),1.f);
    TestEqual(TEXT("Windows 135% scales the entire desktop UI once"),ACERuntimeOptions::DesktopUIScale({2560,1440},false,1.35f),1.35f);
    TestEqual(TEXT("Windows DPI never enlarges the VR render target"),ACERuntimeOptions::DesktopUIScale({2560,1440},true,1.35f),1.f);
    TestEqual(TEXT("Small windows retain access to all controls at 135%"),ACERuntimeOptions::DesktopUIScale({800,600},false,1.35f),1.f);
    if(!FApp::CanEverRender())return false;
    auto* GI=NewObject<UGameInstance>();GI->Init();ON_SCOPE_EXIT{GI->Shutdown();};
    auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();
    if(!Dat->LoadDatDirectory(ACETestDatDirectory()))return false;
    auto* Resources=NewObject<UACEUIResourceResolver>();Resources->Initialize(Dat);
    auto* Label=NewObject<UACERetailTextBlock>();
    Label->SetText(FText::FromString(TEXT("Inventory Equipment Journal")));
    Label->SetColorAndOpacity(FLinearColor::White);
    Label->SetRetailElement(Resources,nullptr,FVector2D(1,1),0,false);
    const auto Text=Label->TakeWidget();Text->SlatePrepass();
    const FVector2D Size=Text->GetDesiredSize();
    FWidgetRenderer Renderer(true,true);
    auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(640,160),TF_Bilinear,true);
    // Draw exactly the same text with layout DPI and render DPI. The latter
    // reproduced the mismatch between glyph transforms and layout-only clips.
    auto Draw=[&](float LayoutScale,float RenderScale)
    {
        auto Box=SNew(SBox).WidthOverride(Size.X).HeightOverride(Size.Y)
            .RenderTransformPivot(FVector2D::ZeroVector)
            .RenderTransform(FSlateRenderTransform(FScale2D(RenderScale)))[Text];
        auto Root=SNew(SBox).HAlign(HAlign_Left).VAlign(VAlign_Top)
            [SNew(SDPIScaler).DPIScale(LayoutScale)[Box]];
        Renderer.DrawWidget(Target,Root,FVector2D(640,160),0);FlushRenderingCommands();
        TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
        FIntRect Bounds(640,160,0,0);int32 Count=0;
        for(int Y=0;Y<160;++Y)for(int X=0;X<640;++X)
            if(Pixels[Y*640+X].R>80)
            {++Count;Bounds.Min.X=FMath::Min(Bounds.Min.X,X);Bounds.Min.Y=FMath::Min(Bounds.Min.Y,Y);Bounds.Max.X=FMath::Max(Bounds.Max.X,X);Bounds.Max.Y=FMath::Max(Bounds.Max.Y,Y);}
        TestTrue(TEXT("DPI render contains real DAT glyphs"),Count>100);
        return Bounds;
    };
    for(float Scale:{1.f,1.25f,1.35f,1.5f,2.f})
    {
        const auto Layout=Draw(Scale,1),Render=Draw(1,Scale);
        TestTrue(FString::Printf(TEXT("Text's right edge survives %.0f%% scaling"),Scale*100),FMath::Abs(Layout.Max.X-Render.Max.X)<=2);
        TestTrue(FString::Printf(TEXT("Text's bottom edge survives %.0f%% scaling"),Scale*100),FMath::Abs(Layout.Max.Y-Render.Max.Y)<=2);
    }
    return true;
}
#endif
