#pragma once
#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Dat/ACEDatFileTypes.h"
#include "Dat/ACEDatTextLayout.h"

class UACEPluginSubsystem;
class UACEUIResourceResolver;
class UTexture2D;
struct FACESupplyEntry;

struct FACEPluginFont
{
    FACEDatFont Font;
    TObjectPtr<UTexture2D> Atlas=nullptr;
    bool Tint=false;
};

/** Shared retail artwork and atlas, retained as long as any plugin widget uses them. */
class FACEPluginStyle : public FGCObject
{
public:
    explicit FACEPluginStyle(UACEUIResourceResolver* InResolver);
    static TSharedRef<FACEPluginStyle> Use(UACEPluginSubsystem* Host,const FString& WindowId=FString());
    static TSharedRef<FACEPluginStyle> Current();
    /** Inventory's DAT tooltip, with live text and resources created only on hover. */
    static TSharedRef<IToolTip> Tooltip(TAttribute<FText> Text);
    // Native panels share authored 16/20/24px body and heading sizes.
    // Lua's explicit font sizes continue to use their requested values.
    static FSlateFontInfo NativeFont(FName Typeface,int32 Size=15);
    static FSlateFontInfo ScaleFont(FSlateFontInfo Font);
    static TAttribute<FSlateFontInfo> ScaledFont(TAttribute<FSlateFontInfo> Font);
    static constexpr float ButtonPadding=6.f, CompactButtonPadding=4.f, IconButtonPadding=2.f, ChromeButtonPadding=1.f;
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override {return TEXT("ACEPluginStyle");}
    const FSlateBrush* Image(uint32 Id);
    const FSlateBrush* SpellImage(uint32 SpellId);
    const FSlateBrush* ItemImage(const FACESupplyEntry& Item,bool Foreground);
    void Refresh();
    bool HasDatArtwork() const {return DatArtworkLoaded;}
    bool SetWindowAppearance(float Opacity,bool Frameless);
    TSharedPtr<FACEPluginFont> Typeface(int32 Size);
    FButtonStyle Button,DockButton,Tab,MenuItem;
    FSliderStyle Slider;
    FCheckBoxStyle Check;
    FEditableTextBoxStyle Edit;
    FComboButtonStyle Combo;
    FComboBoxStyle ComboBox;
    FTableRowStyle ComboRow;
    FSpinBoxStyle Spin;
    FSlateBrush Frame, Background, TitleBackground, SectionFrame, SectionHeader;
    FACEDatFont Font;
    TObjectPtr<UTexture2D> Atlas=nullptr;
    bool TintAtlas=false;
private:
    void InitializeArtwork();
    TObjectPtr<UACEUIResourceResolver> Resolver=nullptr;
    TMap<uint32,TSharedPtr<FSlateBrush>> Images;
    TMap<FString,TSharedPtr<FSlateBrush>> ItemImages;
    TMap<int32,TSharedPtr<FACEPluginFont>> Fonts;
    TArray<TObjectPtr<UTexture2D>> ButtonTextures;
    FSlateBrush OriginalFrame,OriginalBackground,OriginalTitle,OriginalSectionFrame,OriginalSectionHeader;
    float BackgroundOpacity=-1;
    bool HideFrame=false;
    bool DatArtworkLoaded=false;
};

/** Centered content and equal insets on every edge, including pressed states. */
class SACEPluginButton : public SButton
{
public:
    using FArguments=SButton::FArguments;
    void Construct(const FArguments& Args);
    virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
private:
    const FButtonStyle* ContentStyle=nullptr;
};

/** Menu selectors share action-button spacing without inheriting red artwork. */
class SACEPluginComboButton : public SComboButton
{
public:
    using FArguments=SComboButton::FArguments;
    void Construct(const FArguments& Args);
};

/** Persistent navigation selection, distinct from action buttons and hover. */
class SACEPluginTab : public SACEPluginButton
{
public:
    SLATE_BEGIN_ARGS(SACEPluginTab):_Selected(false),_FontSize(15),_ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)){}
        SLATE_ATTRIBUTE(bool,Selected)
        SLATE_ATTRIBUTE(FText,Text)
        SLATE_ATTRIBUTE(int32,FontSize)
        SLATE_ARGUMENT(FMargin,ContentPadding)
        SLATE_EVENT(FOnClicked,OnClicked)
        SLATE_DEFAULT_SLOT(FArguments,Content)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    bool IsSelected() const {return Selected.Get(false);}
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
private:
    TSharedPtr<FACEPluginStyle> Retail;
    TAttribute<bool> Selected;
};

/** Native DAT bitmap text with cached word-boundary wrapping; no generated text textures. */
class SACEPluginText : public STextBlock
{
public:
    using FArguments=STextBlock::FArguments;
    void Construct(const FArguments& Args);
    void SetJustification(TAttribute<ETextJustify::Type> InJustification);
    void SetSingleLineEllipsis(TAttribute<bool> InEllipsis);
    virtual FVector2D ComputeDesiredSize(float) const override;
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool) const override;
private:
    TSharedPtr<FACEPluginStyle> Retail;
    mutable TSharedPtr<FACEPluginFont> Face;
    mutable int32 FaceSize=INDEX_NONE;
    TAttribute<bool> Wrap;
    TAttribute<float> WrapAt;
    TAttribute<ETextJustify::Type> Justification;
    TAttribute<bool> SingleLineEllipsis;
    mutable bool CachedEllipsis=false;
    mutable FString CachedText;
    mutable FString DisplayText;
    mutable TArray<FACEBitmapTextLine> Lines;
    mutable int32 CachedWidth=-1;
    mutable float Width=0;
    mutable bool NativeGlyphs=true;
    mutable FString CheckedGlyphs;
    bool CanDrawNative() const;
    float BitmapScale() const;
    void Layout(float Available) const;
};

/** Keep existing controls, focus and unsaved values when the user changes text scale. */
class SACEPluginEditableTextBox : public SEditableTextBox
{
public:
    using FArguments=SEditableTextBox::FArguments;
    void Construct(const FArguments& Args)
    {
        FArguments Scaled=Args;
        Scaled.Font(FACEPluginStyle::ScaledFont(Args._Font.IsSet()?Args._Font:TAttribute<FSlateFontInfo>(Args._Style->TextStyle.Font)));
        SEditableTextBox::Construct(Scaled);
    }
};
template<typename NumericType>
class SACEPluginSpinBox : public SSpinBox<NumericType>
{
public:
    using FArguments=typename SSpinBox<NumericType>::FArguments;
    void Construct(const FArguments& Args)
    {
        FArguments Scaled=Args;
        Scaled.Font(FACEPluginStyle::ScaledFont(Args._Font.IsSet()?Args._Font:TAttribute<FSlateFontInfo>(FACEPluginStyle::NativeFont("Regular",15))));
        SSpinBox<NumericType>::Construct(Scaled);
    }
};
class SACEPluginMultiLineEditableTextBox : public SMultiLineEditableTextBox
{
public:
    using FArguments=SMultiLineEditableTextBox::FArguments;
    void Construct(const FArguments& Args)
    {
        FArguments Scaled=Args;
        Scaled.Font(FACEPluginStyle::ScaledFont(Args._Font.IsSet()?Args._Font:TAttribute<FSlateFontInfo>(Args._Style->TextStyle.Font)));
        SMultiLineEditableTextBox::Construct(Scaled);
    }
};
