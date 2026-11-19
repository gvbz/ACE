#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginSubsystem.h"
#include "Mods/ACELedger.h"
#include "ACEClientSubsystem.h"
#include "ACEHoverTooltipWidget.h"
#include "ACEDatSubsystem.h"
#include "ACERuntimeOptions.h"
#include "Dat/ACEDatTextureResolver.h"
#include "UI/ACEUIFontStyles.h"
#include "UI/ACEUIResourceResolver.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateColorBrush.h"
#include "Styling/CoreStyle.h"
#include "Rendering/DrawElements.h"
#include "Widgets/SToolTip.h"
#include "Widgets/Layout/SBox.h"

namespace { TWeakPtr<FACEPluginStyle> CurrentStyle; }
namespace
{
    class SACEPluginTooltip : public SToolTip, public FGCObject
    {
    public:
        SLATE_BEGIN_ARGS(SACEPluginTooltip) {} SLATE_ATTRIBUTE(FText,Text)
            SLATE_ARGUMENT(UACEUIResourceResolver*,Resources)
        SLATE_END_ARGS()
        void Construct(const FArguments& Args)
        {
            Text=Args._Text;Resources=Args._Resources;
            SToolTip::Construct(SToolTip::FArguments().TextMargin(0).BorderImage(FCoreStyle::Get().GetBrush("NoBrush"))[SNew(SBox)]);
        }
        virtual void AddReferencedObjects(FReferenceCollector& Collector) override
        {Collector.AddReferencedObject(Resources);Collector.AddReferencedObject(Tip);}
        virtual FString GetReferencerName() const override {return TEXT("SACEPluginTooltip");}
        virtual bool IsEmpty() const override {return Text.Get().IsEmpty();}
        virtual void OnOpening() override {Refresh(true);}
        virtual TSharedRef<SWidget> AsWidget() override {Refresh();return SToolTip::AsWidget();}
        virtual TSharedRef<SWidget> GetContentWidget() override {Refresh();return SToolTip::GetContentWidget();}
        virtual void Tick(const FGeometry& G,double Time,float Delta) override
        {SToolTip::Tick(G,Time,Delta);Refresh();}
    private:
        TAttribute<FText> Text;
        TObjectPtr<UACEUIResourceResolver> Resources=nullptr;
        TObjectPtr<UACEHoverTooltipWidget> Tip=nullptr;
        void Refresh(bool Opening=false)
        {
            const FText Value=Text.Get();
            if(!Tip)
            {
                if(Value.IsEmpty())return;
                Tip=NewObject<UACEHoverTooltipWidget>();Tip->Initialize();Tip->SetResources(Resources);
                Tip->SetTooltipText(Value.ToString(),UACEHoverTooltipWidget::ObjectTemplate);
                SetContentWidget(Tip->TakeWidget());
            }
            else
            {
                // DAT resources may have finished loading since the last hover.
                if(Opening)Tip->SetResources(Resources);
                Tip->SetTooltipText(Value.ToString(),UACEHoverTooltipWidget::ObjectTemplate);
            }
        }
    };
}
TSharedRef<IToolTip> FACEPluginStyle::Tooltip(TAttribute<FText> Text)
{
    return SNew(SACEPluginTooltip).Text(Text).Resources(Current()->Resolver);
}
TSharedRef<FACEPluginStyle> FACEPluginStyle::Use(UACEPluginSubsystem* Host,const FString& WindowId)
{
    auto& Style=WindowId.IsEmpty()?Host->RetailPluginStyle:Host->WindowPluginStyles.FindOrAdd(WindowId);
    if(!Style)
    {
        auto* C=Host->GetGameInstance()->GetSubsystem<UACEClientSubsystem>();
        Style=MakeShared<FACEPluginStyle>(C?C->GetUIResourceResolver():nullptr);
    }
    Style->Refresh();
    if(!WindowId.IsEmpty())Style->SetWindowAppearance(Host->GetPluginWindowOpacity(WindowId),Host->IsPluginWindowChromeHidden(WindowId));
    CurrentStyle=Style;return Style.ToSharedRef();
}
TSharedRef<FACEPluginStyle> FACEPluginStyle::Current()
{
    if(auto S=CurrentStyle.Pin())return S.ToSharedRef();
    static TSharedRef<FACEPluginStyle> Fallback=MakeShared<FACEPluginStyle>(nullptr);return Fallback;
}
FSlateFontInfo FACEPluginStyle::NativeFont(FName Typeface,int32 Size)
{
    const int32 ConsistentSize=Size==13?12:Size>=14&&Size<=16?15:Size>=17&&Size<=20?18:Size;
    return FCoreStyle::GetDefaultFontStyle(Typeface,ConsistentSize);
}
FSlateFontInfo FACEPluginStyle::ScaleFont(FSlateFontInfo Font)
{
    Font.Size=FMath::Max(1.f,Font.Size*ACERuntimeOptions::PluginTextScale());
    return Font;
}
TAttribute<FSlateFontInfo> FACEPluginStyle::ScaledFont(TAttribute<FSlateFontInfo> Font)
{
    return TAttribute<FSlateFontInfo>::CreateLambda([Font](){return ScaleFont(Font.Get());});
}
void FACEPluginStyle::AddReferencedObjects(FReferenceCollector& C)
{C.AddReferencedObject(Resolver);C.AddReferencedObject(Atlas);for(auto& Pair:Fonts)C.AddReferencedObject(Pair.Value->Atlas);for(auto& Texture:ButtonTextures)C.AddReferencedObject(Texture);}
const FSlateBrush* FACEPluginStyle::Image(uint32 Id)
{
    auto& B=Images.FindOrAdd(Id);if(!B)B=MakeShared<FSlateBrush>();
    if(B->GetResourceObject())return B.Get();
    B->DrawAs=ESlateBrushDrawType::Image;
    if(Resolver)if(auto* T=Resolver->ResolveTexture(Id)){B->SetResourceObject(T);B->ImageSize=FVector2D(T->GetSizeX(),T->GetSizeY());}
    return B.Get();
}
const FSlateBrush* FACEPluginStyle::SpellImage(uint32 SpellId)
{
    auto& B=ItemImages.FindOrAdd(FString::Printf(TEXT("spell.%u"),SpellId));
    if(!B)B=MakeShared<FSlateBrush>();
    if(!B->GetResourceObject()&&Resolver)B->SetResourceObject(Resolver->ResolveSpellIcon(SpellId));
    B->DrawAs=B->GetResourceObject()?ESlateBrushDrawType::Image:ESlateBrushDrawType::NoDrawType;
    B->ImageSize=FVector2D(32,32);
    return B.Get();
}
const FSlateBrush* FACEPluginStyle::ItemImage(const FACESupplyEntry& Item,bool Foreground)
{
    const FString Key=Foreground?FString::Printf(TEXT("fg.%d.%d.%d"),Item.Icon,Item.Overlay,Item.Effects)
        :FString::Printf(TEXT("bg.%d.%d"),Item.ItemType,Item.Underlay);
    auto& B=ItemImages.FindOrAdd(Key);if(!B)B=MakeShared<FSlateBrush>();
    if(!B->GetResourceObject()&&Resolver)
    {
        B->SetResourceObject(Foreground?Resolver->ResolveItemForeground(Item.Icon,Item.Overlay,Item.Effects)
            :Resolver->ResolveItemBackground(Item.ItemType,Item.Underlay));
        B->DrawAs=ESlateBrushDrawType::Image;B->ImageSize=FVector2D(32,32);
    }
    return B.Get();
}
FACEPluginStyle::FACEPluginStyle(UACEUIResourceResolver* InResolver):Resolver(InResolver) { InitializeArtwork(); }
void FACEPluginStyle::Refresh()
{
    // A font atlas is not proof that checkbox/slider images have resolved.
    // The login manager can be constructed before asynchronous DAT loading ends.
    if(!DatArtworkLoaded&&Resolver&&Resolver->GetDatSubsystem()&&Resolver->GetDatSubsystem()->IsDatReady())InitializeArtwork();
}
void FACEPluginStyle::InitializeArtwork()
{
    DatArtworkLoaded=Resolver&&Resolver->GetDatSubsystem()&&Resolver->GetDatSubsystem()->IsDatReady();
    const FLinearColor Gold(.65f,.43f,.15f),Ivory(1.f,.98f,.92f);
    Frame=FSlateRoundedBoxBrush(FLinearColor(.008f,.006f,.003f,.96f),0.f,Gold,2.f);
    const FLinearColor TitleColor(.025f,.012f,.004f,1);
    TitleBackground=FSlateColorBrush(TitleColor);
    SectionFrame=FSlateRoundedBoxBrush(TitleColor,0.f,Gold,2.f);
    SectionHeader=FSlateColorBrush(FLinearColor(Gold.R,Gold.G,Gold.B,.25f));
    Background=*Image(0x06004CC2);
    if(!Background.GetResourceObject())Background=FSlateRoundedBoxBrush(FLinearColor(.005f,.004f,.002f,.96f),0.f);
    Button=FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
    Button.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.19f,.002f,.001f),0.f,Gold,2.f))
        .SetHovered(FSlateRoundedBoxBrush(FLinearColor(.29f,.006f,.002f),0.f,FLinearColor(1,.75f,.25f),2.f))
        .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.075f,.001f,0),0.f,Gold,2.f))
        .SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.035f,.02f,.01f),0.f,FLinearColor(.25f,.18f,.08f),1.f))
        .SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0))
        .SetNormalForeground(Ivory).SetHoveredForeground(FLinearColor(1,.85f,.35f)).SetPressedForeground(Ivory);
    // Preserve the authored bevel while stretching the center.
    auto Beveled=[this](uint32 Id)
    {
        auto B=*Image(Id);
        B.DrawAs=ESlateBrushDrawType::Box;
        B.Margin=FMargin(2.f/FMath::Max(1.f,float(B.ImageSize.X)),2.f/FMath::Max(1.f,float(B.ImageSize.Y)));
        return B;
    };
    // Button_RedBar (layout 0x21000040) is retail's resizable action button.
    // Join its three strips once, then let Slate preserve the gold bevel.
    auto RedBar=[this](uint32 Left,uint32 Middle,uint32 Right,FSlateBrush& Out)
    {
        auto* Dat=Resolver?Resolver->GetDatSubsystem():nullptr;
        auto* Textures=Dat?Dat->GetTextureResolver():nullptr;
        if(!Textures)return false;
        FACEDatDecodedSurface Parts[3];const uint32 Ids[]={Left,Middle,Right};
        int32 Width=0,Height=0;
        for(int32 I=0;I<3;++I)
        {
            FACEDatTexture Raw;
            if(!Textures->LoadTextureForUi(Ids[I],Raw)||!Textures->DecodeTextureForUi(Raw,Parts[I])||!Parts[I].bHasPixels)return false;
            if(I&&Height!=Parts[I].Height)return false;
            Width+=Parts[I].Width;Height=Parts[I].Height;
        }
        TArray<FColor> Pixels;Pixels.SetNumUninitialized(Width*Height);int32 Offset=0;
        for(const auto& Part:Parts)
        {
            for(int32 Y=0;Y<Height;++Y)
                FMemory::Memcpy(Pixels.GetData()+Y*Width+Offset,Part.Pixels.GetData()+Y*Part.Width,Part.Width*sizeof(FColor));
            Offset+=Part.Width;
        }
        auto* Texture=FACEDatTextureResolver::CreateTransientRgbaUi(Width,Height,Pixels);
        if(!Texture)return false;
        ButtonTextures.Add(Texture);Out.SetResourceObject(Texture);Out.ImageSize=FVector2D(Width,Height);
        Out.DrawAs=ESlateBrushDrawType::Box;Out.Margin=FMargin(4.f/Width,4.f/Height);return true;
    };
    FSlateBrush Up,Down,Disabled;
    if(RedBar(0x06004C96,0x06004C99,0x06004C9C,Up)&&RedBar(0x06004C95,0x06004C98,0x06004C9B,Down)
        &&RedBar(0x06004C94,0x06004C97,0x06004C9A,Disabled))
        Button.SetNormal(Up).SetHovered(Up).SetPressed(Down).SetDisabled(Disabled);
    DockButton=Button;
    // Small frame/icon controls cannot fit the action button's four-pixel
    // bevel. Keep their one-pixel border in every interaction state.
    DockButton.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.008f,.005f,.002f,.9f),0.f,FLinearColor(.4f,.26f,.09f),1.f))
        .SetHovered(FSlateRoundedBoxBrush(FLinearColor(.055f,.039f,.018f),0.f,FLinearColor(.6f,.43f,.2f),1.f))
        .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.025f,.016f,.007f),0.f,FLinearColor(.9f,.66f,.25f),1.f))
        .SetDisabled(FSlateRoundedBoxBrush(FLinearColor(.008f,.005f,.002f,.6f),0.f,FLinearColor(.25f,.18f,.08f),1.f));
    Tab=Button;
    Tab.SetNormal(FSlateRoundedBoxBrush(FLinearColor(.012f,.01f,.007f),0.f,FLinearColor(.24f,.18f,.1f),1.f))
        .SetHovered(FSlateRoundedBoxBrush(FLinearColor(.055f,.039f,.018f),0.f,FLinearColor(.6f,.43f,.2f),1.f))
        .SetPressed(FSlateRoundedBoxBrush(FLinearColor(.16f,.095f,.025f),0.f,FLinearColor(.9f,.66f,.25f),1.f))
        .SetPressedPadding(FMargin(0));
    Check=FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Checkbox");
    auto Small=[this](uint32 Id){auto B=*Image(Id);B.ImageSize=FVector2D(14,14);return B;};
    // Keep Slate's distinct states until each retail image is available. Empty
    // image brushes paint identical squares for both enabled and disabled states.
    const auto Unchecked=Small(0x0600191F),Checked=Small(0x0600191A);
    if(Unchecked.GetResourceObject())Check.SetUncheckedImage(Unchecked).SetUncheckedHoveredImage(Unchecked).SetUncheckedPressedImage(Unchecked);
    if(Checked.GetResourceObject())Check.SetCheckedImage(Checked).SetCheckedHoveredImage(Checked).SetCheckedPressedImage(Checked);
    auto Handle=*Image(0x06001286);Handle.ImageSize=FVector2D(12,20);
    Slider=FCoreStyle::Get().GetWidgetStyle<FSliderStyle>("Slider");Slider.SetBarThickness(4);
    if(const auto* Bar=Image(0x06001285);Bar->GetResourceObject())Slider.SetNormalBarImage(*Bar).SetHoveredBarImage(*Bar);
    if(Handle.GetResourceObject())Slider.SetNormalThumbImage(Handle).SetHoveredThumbImage(Handle);
    Edit=FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
    FSlateRoundedBoxBrush Well(FLinearColor(.012f,.008f,.004f),0.f,Gold,1.f);
    Edit.SetBackgroundImageNormal(Well).SetBackgroundImageHovered(Well).SetBackgroundImageFocused(Well).SetBackgroundImageReadOnly(Well).SetForegroundColor(Ivory);
    // Config's green selector and row highlight are distinct from red actions.
    auto Green=Beveled(0x060012B3),SelectedGreen=Beveled(0x060012B4);
    if(!Green.GetResourceObject())Green=FSlateRoundedBoxBrush(FLinearColor(.015f,.065f,.012f),0.f,Gold,1.f);
    if(!SelectedGreen.GetResourceObject())SelectedGreen=FSlateRoundedBoxBrush(FLinearColor(.04f,.14f,.025f),0.f,Gold,1.f);
    MenuItem=Button;
    MenuItem.SetNormal(Green).SetHovered(SelectedGreen).SetPressed(SelectedGreen)
        .SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
    Combo=FCoreStyle::Get().GetWidgetStyle<FComboButtonStyle>("ComboButton");
    Combo.SetButtonStyle(MenuItem).SetContentPadding(FMargin(ButtonPadding)).SetMenuBorderBrush(Green).SetMenuBorderPadding(FMargin(2));
    if(const auto* Arrow=Image(0x060012B1);Arrow->GetResourceObject())
        Combo.SetDownArrowImage(*Arrow).SetDownArrowPadding(FMargin(6,0,0,0));
    ComboBox=FCoreStyle::Get().GetWidgetStyle<FComboBoxStyle>("ComboBox");
    ComboBox.SetComboButtonStyle(Combo).SetContentPadding(FMargin(ButtonPadding));
    ComboRow=FCoreStyle::Get().GetWidgetStyle<FTableRowStyle>("TableView.Row");
    ComboRow.SetEvenRowBackgroundBrush(Green).SetOddRowBackgroundBrush(Green)
        .SetEvenRowBackgroundHoveredBrush(SelectedGreen).SetOddRowBackgroundHoveredBrush(SelectedGreen)
        .SetActiveBrush(SelectedGreen).SetActiveHoveredBrush(SelectedGreen)
        .SetInactiveBrush(SelectedGreen).SetInactiveHoveredBrush(SelectedGreen)
        .SetSelectorFocusedBrush(SelectedGreen).SetTextColor(Ivory).SetSelectedTextColor(FLinearColor::White);
    Spin=FCoreStyle::Get().GetWidgetStyle<FSpinBoxStyle>("SpinBox");Spin.SetBackgroundBrush(Well).SetHoveredBackgroundBrush(Well)
        .SetActiveFillBrush(Well).SetInactiveFillBrush(Well).SetForegroundColor(Ivory);
    if(Resolver&&Resolver->ResolveFont(0x40000000,Font))Atlas=Resolver->ResolveFontAtlas(Font.ForegroundSurfaceDataID,TintAtlas);
    OriginalFrame=Frame;OriginalBackground=Background;OriginalTitle=TitleBackground;
    OriginalSectionFrame=SectionFrame;OriginalSectionHeader=SectionHeader;
    const float Previous=BackgroundOpacity;BackgroundOpacity=-1;
    if(Previous>=0)SetWindowAppearance(Previous,HideFrame);
}
bool FACEPluginStyle::SetWindowAppearance(float Opacity,bool Frameless)
{
    Opacity=FMath::Clamp(Opacity,0.f,1.f);
    if(BackgroundOpacity==Opacity&&HideFrame==Frameless)return false;
    BackgroundOpacity=Opacity;HideFrame=Frameless;
    auto Fade=[](FSlateBrush& To,const FSlateBrush& From,float Alpha)
    {
        To=From;auto Color=From.TintColor.GetSpecifiedColor();Color.A*=Alpha;To.TintColor=Color;
        Color=From.OutlineSettings.Color.GetSpecifiedColor();Color.A*=Alpha;To.OutlineSettings.Color=Color;
    };
    // Pinning hides chrome, not the backdrop behind the window's contents.
    Fade(Frame,OriginalFrame,Opacity);
    if(Frameless)
    {
        auto Outline=Frame.OutlineSettings.Color.GetSpecifiedColor();Outline.A=0.f;
        Frame.OutlineSettings.Color=Outline;
    }
    Fade(Background,OriginalBackground,Opacity);
    Fade(TitleBackground,OriginalTitle,Opacity);
    Fade(SectionFrame,OriginalSectionFrame,Opacity);
    Fade(SectionHeader,OriginalSectionHeader,Opacity);
    return true;
}
TSharedPtr<FACEPluginFont> FACEPluginStyle::Typeface(int32 Size)
{
    // Resolve the nearest authored size instead of enlarging the smallest atlas.
    const int32 Pixels=FMath::Clamp(FMath::RoundToInt(Size*1.333333f/2)*2,12,24);
    auto& Face=Fonts.FindOrAdd(Pixels);if(!Face)Face=MakeShared<FACEPluginFont>();
    if(!Face->Atlas&&Resolver&&Resolver->ResolveFont(ACEUIFontStyles::FontIdForStyleName(FString::Printf(TEXT("basefont%d"),Pixels)),Face->Font))
        Face->Atlas=Resolver->ResolveFontAtlas(Face->Font.ForegroundSurfaceDataID,Face->Tint);
    return Face;
}
namespace
{
    float ButtonBorderWidth(const FButtonStyle& Style)
    {
        float Width=0;
        for(const auto* Brush:{&Style.Normal,&Style.Hovered,&Style.Pressed,&Style.Disabled})
        {
            if(Brush->DrawAs==ESlateBrushDrawType::Box||Brush->DrawAs==ESlateBrushDrawType::Border)
                Width=FMath::Max(Width,FMath::Max(FMath::Max(Brush->Margin.Left,Brush->Margin.Right)*float(Brush->ImageSize.X),
                    FMath::Max(Brush->Margin.Top,Brush->Margin.Bottom)*float(Brush->ImageSize.Y)));
            else if(Brush->DrawAs==ESlateBrushDrawType::RoundedBox)
                Width=FMath::Max(Width,Brush->OutlineSettings.Width);
        }
        return Width;
    }
    float ButtonInteriorPadding(const FButtonStyle& Style)
    {
        const float Border=ButtonBorderWidth(Style);
        // Keep a clear gap inside the artwork, including compact title controls.
        // Borderless inventory images retain their exact icon dimensions.
        return Border>0?Border+(Border>1?2.f:1.f):0.f;
    }
    void CenterButtonLabels(const TSharedRef<SWidget>& Widget)
    {
        if(Widget->GetType()==TEXT("SACEPluginText"))
            StaticCastSharedRef<SACEPluginText>(Widget)->SetJustification(ETextJustify::Center);
        else if(Widget->GetType()==TEXT("STextBlock"))
            StaticCastSharedRef<STextBlock>(Widget)->SetJustification(ETextJustify::Center);
        // Button content can include a label and icon or a multiline caption.
        // This runs once when constructed, never during paint or plugin ticks.
        auto* Children=Widget->GetChildren();
        for(int32 I=0;I<Children->Num();++I)CenterButtonLabels(Children->GetChildAt(I));
    }
}
void SACEPluginButton::Construct(const FArguments& A)
{
    auto Args=A;ContentStyle=A._ButtonStyle;
    const auto Padding=A._ContentPadding;
    Args.HAlign(HAlign_Center).VAlign(VAlign_Center);
    // Evaluate the style here as DAT artwork may load after the widget is built.
    Args.ContentPadding_Lambda([this,Padding]
    {
        const auto P=Padding.Get();
        return FMargin(FMath::Max(ButtonInteriorPadding(*ContentStyle),FMath::Max(FMath::Max(P.Left,P.Right),FMath::Max(P.Top,P.Bottom))));
    });
    SButton::Construct(Args);
    CenterButtonLabels(GetContent());
}
FVector2D SACEPluginButton::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
    auto Size=SButton::ComputeDesiredSize(LayoutScaleMultiplier);
    // A thick bevel needs a usable interior even for a single '+' or '-' glyph.
    // Thin frame controls and borderless inventory icons stay compact.
    if(ContentStyle&&ButtonBorderWidth(*ContentStyle)>1.f)
    {
        const float Minimum=16.f+2.f*ButtonInteriorPadding(*ContentStyle);
        Size.X=FMath::Max(Size.X,double(Minimum));Size.Y=FMath::Max(Size.Y,double(Minimum));
    }
    return Size;
}
void SACEPluginComboButton::Construct(const FArguments& A)
{
    auto Args=A;
    Args.HAlign(HAlign_Fill).VAlign(VAlign_Center).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding));
    CenterButtonLabels(A._ButtonContent.Widget);
    SComboButton::Construct(Args);
}
void SACEPluginTab::Construct(const FArguments& A)
{
    Retail=FACEPluginStyle::Current();Selected=A._Selected;
    TSharedRef<SWidget> Content=A._Content.Widget;
    if(Content==SNullWidget::NullWidget)
        Content=SNew(SACEPluginText).Text(A._Text).Justification(ETextJustify::Center)
            .Font_Lambda([Size=A._FontSize]{return FCoreStyle::GetDefaultFontStyle("Regular",FMath::Clamp(Size.Get(15),10,36));})
            .ColorAndOpacity(FLinearColor(1.f,.98f,.92f));
    SACEPluginButton::Construct(SButton::FArguments().ButtonStyle(&Retail->Tab).IsFocusable(false)
        .ContentPadding(A._ContentPadding).HAlign(HAlign_Center).VAlign(VAlign_Center).OnClicked(A._OnClicked)[Content]);
    // Slate tracks this attribute and repaints retained tabs when selection
    // changes, without polling, reconstructing controls or shifting their size.
    SetAppearPressed(Selected);
}
int32 SACEPluginTab::OnPaint(const FPaintArgs& A,const FGeometry& G,const FSlateRect& Clip,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& WidgetStyle,bool Enabled) const
{
    const int32 Top=SButton::OnPaint(A,G,Clip,Out,Layer,WidgetStyle,Enabled);
    if(!IsSelected())return Top;
    const auto Size=G.GetLocalSize();
    // Keep an unmistakable shape cue even with untinted retail bitmap fonts,
    // a hovered sibling, transparent window backgrounds or hidden window chrome.
    FSlateDrawElement::MakeBox(Out,Top+1,G.ToPaintGeometry(FVector2D(FMath::Max(0.,Size.X-4),3),FSlateLayoutTransform(FVector2D(2,Size.Y-3))),
        FCoreStyle::Get().GetBrush("WhiteBrush"),ShouldBeEnabled(Enabled)?ESlateDrawEffect::None:ESlateDrawEffect::DisabledEffect,
        FLinearColor(1,.78f,.3f)*WidgetStyle.GetColorAndOpacityTint());
    return Top+1;
}
void SACEPluginText::Construct(const FArguments& A)
{
    Retail=FACEPluginStyle::Current();Wrap=A._AutoWrapText;WrapAt=A._WrapTextAt;Justification=A._Justification;
    FArguments Scaled=A;
    Scaled.Font(FACEPluginStyle::ScaledFont(A._Font.IsSet()?A._Font:TAttribute<FSlateFontInfo>(A._TextStyle->Font)));
    STextBlock::Construct(Scaled);
    // Match the bitmap path when a label uses characters absent from the DAT.
    // Never turn a narrow plugin title, setting or button into stacked fragments.
    SetWrappingPolicy(ETextWrappingPolicy::DefaultWrapping);
    SetClipping(EWidgetClipping::ClipToBounds);
}
void SACEPluginText::SetJustification(TAttribute<ETextJustify::Type> InJustification)
{
    Justification=InJustification;
    STextBlock::SetJustification(InJustification);
    Invalidate(EInvalidateWidgetReason::Paint);
}
void SACEPluginText::SetSingleLineEllipsis(TAttribute<bool> InEllipsis)
{
    SingleLineEllipsis=MoveTemp(InEllipsis);
    CachedWidth=-1;
    Invalidate(EInvalidateWidgetReason::Layout);
}
float SACEPluginText::BitmapScale() const
{
    // Match Slate's point-to-pixel conversion, including sizes between DAT fonts.
    return FMath::Max(1.f,float(GetFont().Size))*1.333333f/FMath::Max(1u,Face->Font.MaxCharHeight);
}
void SACEPluginText::Layout(float Available) const
{
    const float Limit=WrapAt.Get(0)>0?WrapAt.Get():(Wrap.Get(false)||CachedEllipsis)?Available:0;
    const int32 W=Limit>0?FMath::Max(1,FMath::FloorToInt(Limit/BitmapScale())):MAX_int32;
    const FString Text=GetText().ToString();
    if(Text!=CachedText||W!=CachedWidth)
    {
        CachedText=Text;CachedWidth=W;
        DisplayText=CachedEllipsis?ACEDatText::Ellipsize(Face->Font,Text,W):Text;
        // Plugin labels wrap at word boundaries. Oversized words retain their
        // natural width and clip to the widget until the user widens the window.
        Lines=ACEDatText::Layout(Face->Font,DisplayText,W,CachedEllipsis,/*bBreakLongWords=*/false);
    }
}
bool SACEPluginText::CanDrawNative() const
{
    const bool Ellipsis=SingleLineEllipsis.Get(false)&&!Wrap.Get(false)&&WrapAt.Get(0)<=0;
    if(CachedEllipsis!=Ellipsis)
    {
        CachedEllipsis=Ellipsis;CachedWidth=-1;
        const_cast<SACEPluginText*>(this)->SetOverflowPolicy(Ellipsis
            ?TOptional<ETextOverflowPolicy>(ETextOverflowPolicy::Ellipsis):TOptional<ETextOverflowPolicy>());
    }
    // Bound font attributes are evaluated after Construct, and can change while
    // a Lua control keeps its Slate identity. Refresh both glyphs and wrapping.
    const int32 Size=GetFont().Size;
    if(!Face||FaceSize!=Size||!Face->Atlas)
    {
        // A centered button previously allotted only the old text width. Do not
        // keep wrapping into that width after a font change: measure anew first.
        if(FaceSize!=Size)Width=0;
        Face=Retail->Typeface(Size);FaceSize=Size;CachedWidth=-1;CheckedGlyphs.Reset();NativeGlyphs=true;
    }
    if(!Face->Atlas)return false;
    const FString Text=GetText().ToString();
    if(Text!=CheckedGlyphs)
    {
        CheckedGlyphs=Text;NativeGlyphs=true;
        for(TCHAR C:Text)if(C>32&&!Face->Font.IndexByUnicode.Contains(C)){NativeGlyphs=false;break;}
    }
    // Keep international plugin labels and symbols readable when absent from the retail atlas.
    return NativeGlyphs;
}
FVector2D SACEPluginText::ComputeDesiredSize(float S) const
{
    if(!CanDrawNative())return STextBlock::ComputeDesiredSize(S);
    const auto M=GetMargin();Layout(FMath::Max(0.f,Width-M.Left-M.Right));int32 W=0;
    for(const auto& L:Lines)W=FMath::Max(W,L.Width);
    return FVector2D(W,Lines.Num()*Face->Font.MaxCharHeight)*BitmapScale()+FVector2D(M.Left+M.Right,M.Top+M.Bottom);
}
int32 SACEPluginText::OnPaint(const FPaintArgs& A,const FGeometry& G,const FSlateRect& Clip,FSlateWindowElementList& E,int32 Layer,const FWidgetStyle& Style,bool Enabled) const
{
    if(!CanDrawNative())return STextBlock::OnPaint(A,G,Clip,E,Layer,Style,Enabled);
    // STextBlock is non-ticking. Use the allotted width to reserve wrapped line height
    // on the next prepass without enabling per-frame work on every label.
    if((Wrap.Get(false)||CachedEllipsis)&&!FMath::IsNearlyEqual(Width,float(G.GetLocalSize().X)))
    {Width=G.GetLocalSize().X;const_cast<SACEPluginText*>(this)->Invalidate(EInvalidateWidgetReason::Layout);}
    const auto M=GetMargin();const float Available=G.GetLocalSize().X-M.Left-M.Right;Layout(Available);
    const float Scale=BitmapScale();auto* Atlas=Face->Atlas.Get();const auto& DatFont=Face->Font;
    FSlateBrush B;B.DrawAs=ESlateBrushDrawType::Image;B.SetResourceObject(Atlas);
    const FLinearColor Color=(Face->Tint?GetColorAndOpacity().GetColor(Style):FLinearColor::White)*Style.GetColorAndOpacityTint();
    // Rounding each scaled glyph quad independently can squeeze a thin stem
    // out of its sample footprint (notably 'l' at 16pt). Preserve fractional
    // geometry and let the atlas's bilinear sampler retain its coverage.
    // Native/integer pixel sizes still snap for crisp authored bitmap text.
    const float RasterScale=Scale*G.GetAccumulatedLayoutTransform().GetScale();
    const auto Effects=(ShouldBeEnabled(Enabled)?ESlateDrawEffect::None:ESlateDrawEffect::DisabledEffect)
        | (FMath::IsNearlyEqual(RasterScale,FMath::RoundToFloat(RasterScale),1.e-4f)?ESlateDrawEffect::None:ESlateDrawEffect::NoPixelSnapping);
    // Glyphs use the accumulated render transform, so their clipping must too.
    // A layout-space rectangle cuts off labels after plugin scale/offset/rotation.
    E.PushClip(FSlateClippingZone(G));
    for(int32 N=0;N<Lines.Num();++N)
    {
        const auto& L=Lines[N];float X=M.Left;const float Spare=FMath::Max(0.f,Available-L.Width*Scale);
        if(Justification.Get()==ETextJustify::Center)X+=Spare*.5f;else if(Justification.Get()==ETextJustify::Right)X+=Spare;
        const float Y=M.Top+N*DatFont.MaxCharHeight*Scale;
        for(int32 I=L.Begin;I<L.End;++I)if(const auto* C=ACEDatText::FindChar(DatFont,DisplayText[I]))
        {
            X+=int8(C->HorizontalOffsetBefore)*Scale;
            if(C->Width&&C->Height)
            {
                const FVector2D UV(C->OffsetX,C->OffsetY),Size(C->Width,C->Height),AS(Atlas->GetSizeX(),Atlas->GetSizeY());
                B.SetUVRegion(FBox2f(FVector2f(UV/AS),FVector2f((UV+Size)/AS)));
                FSlateDrawElement::MakeBox(E,Layer,G.ToPaintGeometry(Size*Scale,FSlateLayoutTransform(FVector2D(X,Y+int8(C->VerticalOffsetBefore)*Scale))),&B,Effects,Color);
            }
            X+=(C->Width+int8(C->HorizontalOffsetAfter))*Scale;
        }
    }
    E.PopClip();return Layer;
}
