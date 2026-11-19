#include "Mods/ACEPluginPresentation.h"
#include "Mods/ACEPluginColor.h"
#include "Mods/ACEPluginStyle.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEDatSubsystem.h"
#include "UI/ACEUIResourceResolver.h"
#include "Engine/GameInstance.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Colors/SColorWheel.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Text/STextBlock.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"

namespace
{
namespace ACEPluginCompanionPanelPrivate
{
    FString S(const TSharedPtr<FJsonObject>& O,const TCHAR* K,const FString& D=FString())
    {FString V;return O->TryGetStringField(K,V)?V:D;}
    double N(const TSharedPtr<FJsonObject>& O,const TCHAR* K,double D=0)
    {double V;return O->TryGetNumberField(K,V)&&FMath::IsFinite(V)?V:D;}
    bool B(const TSharedPtr<FJsonObject>& O,const TCHAR* K,bool D=true)
    {bool V;return O->TryGetBoolField(K,V)?V:D;}
    FLinearColor Color(const FString& Name)
    {
        FLinearColor Parsed;
        return ACEPluginColor::Parse(Name,Parsed)?Parsed:FLinearColor(1.f,.98f,.92f);
    }
    // Effects advance on Slate's frame clock, independent of Lua's callback rate.
    class SACECompanionVisual : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SACECompanionVisual){} SLATE_ARGUMENT(TSharedPtr<FJsonObject>,Data)
            SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(FString,Plugin)
            SLATE_DEFAULT_SLOT(FArguments,Content) SLATE_END_ARGS()
        void Construct(const FArguments& A)
        {
            Data=A._Data;Host=A._Host;Plugin=A._Plugin;ChildSlot[A._Content.Widget];
            SetRenderTransformPivot(FVector2D(.5,.5));
            SetVisibility(TAttribute<EVisibility>::CreateLambda([this](){return B(Data,TEXT("hide_when_pinned"),false)
                &&Host.IsValid()&&Host->IsPluginWindowChromeHidden(Plugin)?EVisibility::Collapsed:EVisibility::SelfHitTestInvisible;}));
            RefreshVisual();
        }
        void RefreshVisual()
        {
            Effect=S(Data,TEXT("effect"));Period=N(Data,TEXT("period"),1);Opacity=N(Data,TEXT("opacity"),1);
            Minimum=N(Data,TEXT("min_opacity"),.2);Rotation=FMath::DegreesToRadians(float(N(Data,TEXT("rotation"))));
            Scale=N(Data,TEXT("scale"),1);Offset={N(Data,TEXT("offset_x")),N(Data,TEXT("offset_y"))};
            SetRenderOpacity(Opacity);SetRenderTransform(Concatenate(FScale2D(Scale),FQuat2D(Rotation),Offset));
            SetCanTick(!Effect.IsEmpty());
        }
        void Tick(const FGeometry& G,double Time,float Dt) override
        {
            SCompoundWidget::Tick(G,Time,Dt);
            const double Phase=FMath::Fmod(Time,Period)/Period;
            float Alpha=Opacity;
            if(Effect==TEXT("blink"))Alpha*=Phase<.5?1.f:Minimum;
            if(Effect==TEXT("pulse"))Alpha*=FMath::Lerp(Minimum,1.f,float(.5-.5*FMath::Cos(Phase*2*PI)));
            SetRenderOpacity(Alpha);
            const float Angle=Rotation+(Effect==TEXT("rotate")?float(Phase*2*PI):0.f);
            SetRenderTransform(Concatenate(FScale2D(Scale),FQuat2D(Angle),Offset));
        }
    private:
        TSharedPtr<FJsonObject> Data;TWeakObjectPtr<UACEPluginSubsystem> Host;FString Plugin;
        FString Effect;double Period=1;float Opacity=1,Minimum=.2f,Rotation=0,Scale=1;FVector2D Offset=FVector2D::ZeroVector;
    };
    // Stable Lua control IDs are also stable Slate identities. Only structural
    // changes touch slots; ordinary snapshots update the retained row data.
    class SACECompanionRows : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SACECompanionRows){} SLATE_ARGUMENT(UACEPluginSubsystem*,Host) SLATE_ARGUMENT(FString,Id) SLATE_END_ARGS()
        void Construct(const FArguments& Args)
        {Host=Args._Host;Id=Args._Id;FACEPluginStyle::Use(Args._Host,Id);ChildSlot[SAssignNew(Rows,SVerticalBox)];Refresh();}
        void Tick(const FGeometry& G,double Time,float Delta) override
        {
            SCompoundWidget::Tick(G,Time,Delta);Refresh();
            if(auto P=Last.Pin();P&&!P->ScrollPanel())
            {
                const FVector2D Size=G.GetLocalSize();
                if(Size.X>0&&Size.Y>0&&!P->PanelSize.Equals(Size,1))
                {P->PanelSize=Size;P->NextDecision=0;}
            }
        }
    private:
        struct FRow
        {
            FString Type;
            TSharedPtr<FJsonObject> Data;
            TSharedPtr<SWidget> Widget;
            TSharedPtr<SBox> Frame;
            TSharedPtr<SACECompanionVisual> Visual;
            TSharedPtr<SVerticalBox> Vertical;
            TSharedPtr<SHorizontalBox> Horizontal,TabButtons;
            TSharedPtr<SWidgetSwitcher> Switch;
            TSharedPtr<SOverlay> Overlay;
            TArray<TSharedPtr<SWidget>> Children;
            TArray<bool> AutoWidths;
            // False means this control is being edited. Retry just this control,
            // never block other rows because the game viewport has mouse capture.
            TFunction<bool()> Sync;
        };
        TWeakObjectPtr<UACEPluginSubsystem> Host;FString Id;uint32 Revision=MAX_uint32;
        TWeakPtr<FACEClientPlugin> Last;TSharedPtr<SVerticalBox> Rows;
        TStrongObjectPtr<UACEUIResourceResolver> Resolver;
        TMap<FString,TSharedPtr<FRow>> Controls;
        TArray<TSharedPtr<SWidget>> RootChildren;
        TSet<FString> Visited;
        TArray<TSharedPtr<FRow>> Pending;
        TMap<FString,bool> Expanded;TMap<FString,FString> Tabs;
        TSharedRef<STextBlock> Label(const TSharedPtr<FJsonObject>& R,const TCHAR* ColorKey=TEXT("color"))
        {
            auto Text=SNew(SACEPluginText).Text_Lambda([R](){return FText::FromString(S(R,TEXT("text")));})
                .Font_Lambda([R](){auto Font=FCoreStyle::GetDefaultFontStyle(B(R,TEXT("bold"),false)?"Bold":"Regular",FMath::Clamp(int32(N(R,TEXT("size"),15)),10,36));
                    Font.OutlineSettings.OutlineSize=int32(N(R,TEXT("outline"),N(R,TEXT("glow"))));Font.OutlineSettings.OutlineColor=Color(S(R,TEXT("outline_color"),S(R,TEXT("glow_color"),TEXT("#000000"))));return Font;})
                .Justification_Lambda([R](){const auto Align=S(R,TEXT("align"));return Align==TEXT("right")?ETextJustify::Right:Align==TEXT("center")?ETextJustify::Center:ETextJustify::Left;})
                .ColorAndOpacity_Lambda([R,ColorField=FString(ColorKey)](){return FSlateColor(Color(S(R,*ColorField)));})
                .AutoWrapText_Lambda([R](){return B(R,TEXT("wrap"));});
            Text->SetSingleLineEllipsis(TAttribute<bool>::CreateLambda([R](){return B(R,TEXT("ellipsis"),false);}));
            return Text;
        }
        UTexture2D* Image(const FString& Asset)
        {
            auto P=Last.Pin();if(!P)return nullptr;
            if(!P->Images)P->Images=MakeShared<FACEPluginImageCache>();
            return P->Images->Load(P->Directory,Asset);
        }
        void Children(FRow& Node)
        {
            const auto R=Node.Data;const FString Key=S(R,TEXT("id"));
            const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;R->TryGetArrayField(TEXT("children"),Values);
            TArray<TSharedPtr<SWidget>> Next;TArray<FString> Keys;
            TArray<bool> AutoWidths;
            if(Values)for(const auto& V:*Values){Next.Add(Build(V->AsObject()));Keys.Add(S(V->AsObject(),TEXT("id")));AutoWidths.Add(B(R,TEXT("compact"),false)||B(V->AsObject(),TEXT("auto_width"),false)||V->AsObject()->HasField(TEXT("width")));}
            if(Node.Children!=Next||Node.AutoWidths!=AutoWidths)
            {
                // Reattach existing child widgets in their new order. In particular,
                // a dropdown's anchor, text editor and section survive sibling edits.
                if(Node.Vertical){Node.Vertical->ClearChildren();for(const auto& W:Next)Node.Vertical->AddSlot().AutoHeight().Padding(2)[W.ToSharedRef()];}
                if(Node.Overlay){Node.Overlay->ClearChildren();for(const auto& W:Next)Node.Overlay->AddSlot()[W.ToSharedRef()];}
                if(Node.Horizontal)
                {
                    Node.Horizontal->ClearChildren();
                    for(int32 I=0;I<Next.Num();++I)
                    {
                        auto Slot=Node.Horizontal->AddSlot();Slot.Padding(3).VAlign(S(R,TEXT("valign"))==TEXT("top")?VAlign_Top:S(R,TEXT("valign"))==TEXT("bottom")?VAlign_Bottom:VAlign_Center); // personal: dev-dashboard
                        if(AutoWidths[I])Slot.AutoWidth();else Slot.FillWidth(1);
                        Slot[Next[I].ToSharedRef()];
                    }
                }
                if(Node.Switch)
                {
                    for(const auto& Old:Node.Children)Node.Switch->RemoveSlot(Old.ToSharedRef());Node.TabButtons->ClearChildren();
                    for(int32 I=0;I<Next.Num();++I)
                    {
                        Node.Switch->AddSlot()[Next[I].ToSharedRef()];const FString Page=Keys[I];
                        const auto Data=Controls[Page]->Data;
                        Node.TabButtons->AddSlot().FillWidth(1).Padding(2)[SNew(SACEPluginTab).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).Tag(FName(*(TEXT("PluginTab.")+Key+TEXT(".")+Page)))
                            .Text_Lambda([Data](){return FText::FromString(S(Data,TEXT("text")));})
                            .FontSize_Lambda([Data](){return int32(N(Data,TEXT("size"),15));})
                            .Selected_Lambda([this,Key,Page](){return Tabs.FindRef(Key)==Page;})
                            .OnClicked_Lambda([this,Key,Page,Switch=Node.Switch,I](){Tabs.Add(Key,Page);Switch->SetActiveWidgetIndex(I);return FReply::Handled();})];
                    }
                }
                Node.Children=MoveTemp(Next);
                Node.AutoWidths=MoveTemp(AutoWidths);
            }
            if(Node.Switch)
            {
                int32 Page=Keys.IndexOfByKey(Tabs.FindRef(Key));if(Page==INDEX_NONE)Page=0;
                if(Keys.IsValidIndex(Page))Tabs.Add(Key,Keys[Page]);
                Node.Switch->SetActiveWidgetIndex(Page);
            }
        }
        TSharedRef<SWidget> Build(const TSharedPtr<FJsonObject>& Source)
        {
            const FString Key=S(Source,TEXT("id")),Type=S(Source,TEXT("type"),S(Source,TEXT("action")).IsEmpty()?TEXT("text"):TEXT("button"));
            Visited.Add(Key);auto Node=Controls.FindRef(Key);
            const auto Clickable=[](const auto& D){return B(D,TEXT("clickable"),false)||D->HasField(TEXT("action"));};
            if(Node&&Node->Type==Type&&Clickable(Node->Data)==Clickable(Source)
                &&((Type!=TEXT("image")&&Type!=TEXT("icon"))||S(Node->Data,TEXT("text")).IsEmpty()==S(Source,TEXT("text")).IsEmpty()))
            {
                Node->Data->Values=Source->Values;
                Node->Visual->RefreshVisual();
                Node->Frame->SetHAlign(Source->HasField(TEXT("width"))?HAlign_Left:HAlign_Fill);
                Node->Frame->SetVAlign(Source->HasField(TEXT("height"))?VAlign_Top:VAlign_Fill);
                Node->Widget->SetClipping(B(Source,TEXT("wrap"))?EWidgetClipping::Inherit:EWidgetClipping::ClipToBounds);
                if(Node->Vertical||Node->Horizontal||Node->Switch||Node->Overlay)Children(*Node);
                if(Node->Sync&&!Node->Sync())Pending.Add(Node);
                return Node->Widget.ToSharedRef();
            }
            Node=MakeShared<FRow>();Node->Type=Type;Node->Data=MakeShared<FJsonObject>(*Source);Controls.Add(Key,Node);
            const auto R=Node->Data;
            auto Changed=[H=Host,Plugin=Id,Key,R](const TSharedPtr<FJsonValue>& Value){if(H.IsValid()&&H->ChangePluginControl(Plugin,Key,Value))R->SetField(TEXT("value"),Value);};
            TSharedRef<SWidget> Content=SNullWidget::NullWidget;
            if(Type==TEXT("row")||Type==TEXT("column")||Type==TEXT("section")||Type==TEXT("tabs")||Type==TEXT("overlay")||Type==TEXT("frame"))
            {
                if(Type==TEXT("row")){Node->Horizontal=SNew(SHorizontalBox);Content=Node->Horizontal.ToSharedRef();}
                else if(Type==TEXT("overlay")||Type==TEXT("frame"))
                {
                    Node->Overlay=SNew(SOverlay);Content=Node->Overlay.ToSharedRef();
                    if(Type==TEXT("frame"))Content=SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
                        .BorderBackgroundColor_Lambda([R](){return Color(S(R,TEXT("color"),TEXT("#00000080")));}).Padding(4)[Content];
                }
                else if(Type==TEXT("tabs"))
                {
                    Node->Switch=SNew(SWidgetSwitcher);Node->TabButtons=SNew(SHorizontalBox);
                    Content=SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Node->TabButtons.ToSharedRef()]+SVerticalBox::Slot().AutoHeight()[Node->Switch.ToSharedRef()];
                }
                else
                {
                    Node->Vertical=SNew(SVerticalBox);
                    if(Type==TEXT("section"))
                    {
                        if(!Expanded.Contains(Key))Expanded.Add(Key,B(R,TEXT("expanded"),true));
                        const auto Theme=FACEPluginStyle::Current();
                        Content=SNew(SExpandableArea).BorderImage(&Theme->SectionHeader).BodyBorderImage(&Theme->TitleBackground)
                            .InitiallyCollapsed(!Expanded[Key]).OnAreaExpansionChanged_Lambda([this,Key](bool Open){Expanded.Add(Key,Open);}).HeaderContent()[Label(R)].BodyContent()[Node->Vertical.ToSharedRef()];
                    }
                    else Content=Node->Vertical.ToSharedRef();
                }
                Children(*Node);
            }
            else if(Type==TEXT("button"))
                Content=SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&FACEPluginStyle::Current()->Button).Tag(FName(*(TEXT("PluginRow.")+Key))).IsFocusable(false)
                    .OnClicked_Lambda([H=Host,Plugin=Id,Key](){if(H.IsValid())H->ClickPluginRow(Plugin,Key);return FReply::Handled();})[Label(R)];
            else if(Type==TEXT("checkbox"))
                Content=SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsChecked_Lambda([R](){return B(R,TEXT("value"),false)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
                    .OnCheckStateChanged_Lambda([Changed](ECheckBoxState Value){Changed(MakeShared<FJsonValueBoolean>(Value==ECheckBoxState::Checked));})[Label(R)];
            else if(Type==TEXT("input"))
            {
                auto Applying=MakeShared<bool>(false);
                auto Input=SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit).Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(S(R,TEXT("value"))))
                    .HintText_Lambda([R](){return FText::FromString(S(R,TEXT("placeholder")));}).SelectAllTextWhenFocused(false)
                    .OnTextChanged_Lambda([Changed,R,Applying](const FText& Text){if(!*Applying&&B(R,TEXT("live"),false))Changed(MakeShared<FJsonValueString>(Text.ToString().Left(1024)));})
                    .OnTextCommitted_Lambda([Changed,Applying](const FText& Text,ETextCommit::Type){if(!*Applying)Changed(MakeShared<FJsonValueString>(Text.ToString().Left(1024)));});
                Node->Sync=[R,Input,Applying](){if(Input->HasAnyUserFocusOrFocusedDescendants())return false;
                    const FString Value=S(R,TEXT("value"));if(Input->GetText().ToString()!=Value){TGuardValue<bool> Guard(*Applying,true);Input->SetText(FText::FromString(Value));}return true;};
                Content=SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Label(R)]+SVerticalBox::Slot().AutoHeight()[Input];
            }
            else if(Type==TEXT("color"))
            {
                auto Value=[R](){return Color(S(R,TEXT("value")));};
                auto Commit=[Changed](FLinearColor C){Changed(MakeShared<FJsonValueString>(ACEPluginColor::Encode(C)));};
                auto Weak=MakeShared<TWeakPtr<SEditableTextBox>>();
                auto Input=SNew(SACEPluginEditableTextBox).Style(&FACEPluginStyle::Current()->Edit)
                    .Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(S(R,TEXT("value"))))
                    .SelectAllTextWhenFocused(false)
                    .OnTextCommitted_Lambda([Changed,Weak](const FText& Text,ETextCommit::Type)
                {
                    FLinearColor C;const bool Valid=ACEPluginColor::Parse(Text.ToString(),C);
                    if(auto Edit=Weak->Pin())Edit->SetError(Valid?FText::GetEmpty():FText::FromString(TEXT("Use #RRGGBB or #RRGGBBAA")));
                    if(Valid)Changed(MakeShared<FJsonValueString>(ACEPluginColor::Encode(C)));
                });
                *Weak=Input;
                Node->Sync=[R,Input](){if(Input->HasAnyUserFocusOrFocusedDescendants())return false;
                    Input->SetText(FText::FromString(S(R,TEXT("value"))));Input->SetError(FText::GetEmpty());return true;};
                auto Body=SNew(SVerticalBox);
                Body->AddSlot().AutoHeight().HAlign(HAlign_Center)
                    [SNew(SBox).WidthOverride(128).HeightOverride(128)[SNew(SColorWheel)
                        .SelectedColor_Lambda([Value](){return Value().LinearRGBToHSV();})
                        .OnValueChanged_Lambda([Commit](FLinearColor HSV){Commit(HSV.HSVToLinearRGB());})]];
                for(bool Alpha:{false,true})
                    Body->AddSlot().AutoHeight().Padding(2)
                        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()
                            [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",13)).Text(FText::FromString(Alpha?TEXT("Opacity"):TEXT("Brightness")))]
                            +SVerticalBox::Slot().AutoHeight()[SNew(SSlider).Style(&FACEPluginStyle::Current()->Slider)
                                .Value_Lambda([Value,Alpha](){return Alpha?Value().A:Value().LinearRGBToHSV().B;})
                                .OnValueChanged_Lambda([Value,Commit,Alpha](float V){auto C=Value().LinearRGBToHSV();if(Alpha)C.A=V;else C.B=V;Commit(C.HSVToLinearRGB());})]];
                Content=SNew(SExpandableArea).BorderImage(&FACEPluginStyle::Current()->SectionHeader)
                    .BodyBorderImage(&FACEPluginStyle::Current()->TitleBackground).InitiallyCollapsed(!B(R,TEXT("expanded"),false))
                    .HeaderContent()[SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Label(R)]
                        +SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)
                            [SNew(SBox).WidthOverride(32).HeightOverride(28)[SNew(SColorBlock).Color_Lambda(Value).ShowBackgroundForAlpha(true)]]
                            +SHorizontalBox::Slot().FillWidth(1)[Input]]]
                    .BodyContent()[Body];
            }
            else if(Type==TEXT("number"))
            {
                auto Input=SNew(SACEPluginSpinBox<double>).Style(&FACEPluginStyle::Current()->Spin).Font(FACEPluginStyle::NativeFont("Regular",15)).MinValue(N(R,TEXT("min"))).MaxValue(N(R,TEXT("max"),100)).Delta(N(R,TEXT("step"),1)).Value(N(R,TEXT("value")))
                    .OnValueCommitted_Lambda([Changed](double V,ETextCommit::Type){Changed(MakeShared<FJsonValueNumber>(V));})
                    .OnEndSliderMovement_Lambda([Changed](double V){Changed(MakeShared<FJsonValueNumber>(V));});
                Node->Sync=[R,Input](){if(Input->HasAnyUserFocusOrFocusedDescendants()||Input->HasMouseCapture())return false;
                    Input->SetMinValue(N(R,TEXT("min")));Input->SetMaxValue(N(R,TEXT("max"),100));Input->SetDelta(N(R,TEXT("step"),1));Input->SetValue(N(R,TEXT("value")));return true;};
                Content=SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Label(R)]+SVerticalBox::Slot().AutoHeight()[Input];
            }
            else if(Type==TEXT("slider"))
            {
                auto Input=SNew(SSlider).MinValue(N(R,TEXT("min"))).MaxValue(N(R,TEXT("max"),100)).StepSize_Lambda([R](){return float(N(R,TEXT("step"),1));}).Value(N(R,TEXT("value")))
                    .OnValueChanged_Lambda([Changed](float V){Changed(MakeShared<FJsonValueNumber>(V));});
                Node->Sync=[R,Input](){if(Input->HasMouseCapture()||Input->HasAnyUserFocusOrFocusedDescendants())return false;
                    Input->SetMinAndMaxValues(N(R,TEXT("min")),N(R,TEXT("max"),100));Input->SetValue(float(N(R,TEXT("value"))));return true;};
                Content=SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Label(R)]+SHorizontalBox::Slot().AutoWidth()[SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).Text_Lambda([R](){return FText::AsNumber(N(R,TEXT("value")));})]]
                    +SVerticalBox::Slot().AutoHeight()[Input];
            }
            else if(Type==TEXT("select"))
            {
                auto Options=MakeShared<TArray<TSharedPtr<FString>>>();auto Applying=MakeShared<bool>(false);
                auto Combo=SNew(SComboBox<TSharedPtr<FString>>).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).OptionsSource(&Options.Get())
                    .ComboBoxStyle(&FACEPluginStyle::Current()->ComboBox).ItemStyle(&FACEPluginStyle::Current()->ComboRow)
                    .OnGenerateWidget_Lambda([Options](TSharedPtr<FString> V){return SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(V?*V:FString()));})
                    .OnSelectionChanged_Lambda([Options,Changed,Applying](TSharedPtr<FString> V,ESelectInfo::Type){if(V&&!*Applying)Changed(MakeShared<FJsonValueString>(*V));})
                    [SNew(SACEPluginText).Justification(ETextJustify::Center).Font(FACEPluginStyle::NativeFont("Regular",15)).Text_Lambda([R](){return FText::FromString(S(R,TEXT("value")));})];
                Node->Sync=[R,Options,Combo,Applying]()
                {
                    // Keep the open menu's rows/highlight stable until it closes.
                    if(Combo->IsOpen())return false;
                    TGuardValue<bool> Guard(*Applying,true);const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;R->TryGetArrayField(TEXT("options"),Values);
                    const int32 Count=Values?Values->Num():0;bool Different=Options->Num()!=Count;
                    for(int32 I=0;!Different&&I<Count;++I)Different=*(*Options)[I]!=(*Values)[I]->AsString();
                    if(Different){Options->Reset();if(Values)for(const auto& V:*Values)Options->Add(MakeShared<FString>(V->AsString()));Combo->RefreshOptions();}
                    TSharedPtr<FString> Selected;const FString Value=S(R,TEXT("value"));for(const auto& Option:*Options)if(*Option==Value){Selected=Option;break;}
                    if(Combo->GetSelectedItem()!=Selected)Combo->SetSelectedItem(Selected);
                    return true;
                };
                Content=SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[Label(R)]+SVerticalBox::Slot().AutoHeight()[Combo];
            }
            else if(Type==TEXT("progress"))
                Content=SNew(SOverlay)+SOverlay::Slot()[SNew(SBox).MinDesiredHeight(22)[SNew(SProgressBar).Percent_Lambda([R](){return TOptional<float>(FMath::Clamp(N(R,TEXT("value")),0.,1.));}).FillColorAndOpacity_Lambda([R](){return Color(S(R,TEXT("color")));})]]
                    +SOverlay::Slot().VAlign(VAlign_Center)[Label(R,TEXT("text_color"))];
            else if(Type==TEXT("item_icon"))
            {
                if(!Resolver.IsValid()&&Host.IsValid()){Resolver.Reset(NewObject<UACEUIResourceResolver>(Host.Get()));Resolver->Initialize(Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>());}
                auto Background=MakeShared<FSlateBrush>(),Foreground=MakeShared<FSlateBrush>();
                auto Back=SNew(SImage).Image_Lambda([Background]()->const FSlateBrush*{return &Background.Get();});
                auto Front=SNew(SImage).Image_Lambda([Foreground]()->const FSlateBrush*{return &Foreground.Get();});
                Node->Sync=[this,R,Background,Foreground,Back,Front]()
                {
                    if(!Resolver.IsValid()||!Resolver->GetDatSubsystem()||!Resolver->GetDatSubsystem()->IsDatReady())return false;
                    const double Size=FMath::Clamp(N(R,TEXT("size"),32),16.,128.);
                    auto Update=[Size](const auto& Brush,const auto& Widget,UTexture2D* Texture)
                    {
                        if(Brush->GetResourceObject()!=Texture||Brush->ImageSize!=FVector2D(Size,Size))
                        {Brush->SetResourceObject(Texture);Brush->DrawAs=ESlateBrushDrawType::Image;Brush->ImageSize=FVector2D(Size,Size);Widget->Invalidate(EInvalidateWidgetReason::Layout|EInvalidateWidgetReason::Paint);}
                    };
                    // Same compositors as the inventory and Supply Ledger, including
                    // the square type background, underlay, overlay and enchantments.
                    Update(Background,Back,Resolver->ResolveItemBackground(uint32(N(R,TEXT("item_type"))),uint32(N(R,TEXT("underlay")))));
                    Update(Foreground,Front,Resolver->ResolveItemForeground(uint32(N(R,TEXT("icon"))),uint32(N(R,TEXT("overlay"))),uint32(N(R,TEXT("effects")))));
                    return true;
                };
                Content=SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[SNew(SBox)
                    .WidthOverride_Lambda([R](){return FOptionalSize(FMath::Clamp(N(R,TEXT("size"),32),16.,128.));})
                    .HeightOverride_Lambda([R](){return FOptionalSize(FMath::Clamp(N(R,TEXT("size"),32),16.,128.));})
                    [SNew(SOverlay)+SOverlay::Slot()[Back]+SOverlay::Slot()[Front]]]
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(6,0)[Label(R)];
            }
            else if(Type==TEXT("icon")||Type==TEXT("image"))
            {
                if(!Resolver.IsValid()&&Host.IsValid()){Resolver.Reset(NewObject<UACEUIResourceResolver>(Host.Get()));Resolver->Initialize(Host->GetGameInstance()->GetSubsystem<UACEDatSubsystem>());}
                auto Brush=MakeShared<FSlateBrush>();auto Icon=SNew(SImage).Image_Lambda([Brush]()->const FSlateBrush*{return &Brush.Get();})
                    .ColorAndOpacity_Lambda([R](){return Color(S(R,TEXT("color"),TEXT("#FFFFFF")));});
                Node->Sync=[this,R,Type,Brush,Icon]()
                {
                    const double Size=FMath::Clamp(N(R,TEXT("size"),32),16.,128.);
                    auto* Texture=Type==TEXT("image")?Image(S(R,TEXT("asset"))):Resolver.IsValid()?Resolver->ResolveIconTexture(uint32(N(R,TEXT("icon")))):nullptr;
                    const FVector2D Dimensions=Type==TEXT("image")&&Texture?FVector2D(Texture->GetSizeX(),Texture->GetSizeY()):FVector2D(Size,Size);
                    if(Brush->GetResourceObject()!=Texture||Brush->ImageSize!=Dimensions){Brush->SetResourceObject(Texture);Brush->ImageSize=Dimensions;Icon->Invalidate(EInvalidateWidgetReason::Layout|EInvalidateWidgetReason::Paint);}return true;
                };
                TSharedRef<SWidget> Artwork=Icon;
                {
                    auto Layers=SNew(SOverlay);
                    for(float Scale:{1.2f,1.1f})Layers->AddSlot()[SNew(SImage).Image_Lambda([Brush]()->const FSlateBrush*{return &Brush.Get();})
                        .Visibility_Lambda([R](){return N(R,TEXT("glow"))>0?EVisibility::HitTestInvisible:EVisibility::Collapsed;})
                        .RenderTransformPivot(FVector2D(.5,.5)).RenderTransform(FSlateRenderTransform(FScale2D(Scale)))
                        .ColorAndOpacity_Lambda([R](){auto C=Color(S(R,TEXT("glow_color"),TEXT("#FFFFFF")));C.A*=.25f;return C;})];
                    Layers->AddSlot()[Icon];Artwork=Layers;
                }
                Content=SNew(SBox)
                    .WidthOverride_Lambda([R](){return FOptionalSize(N(R,TEXT("width"),N(R,TEXT("size"),32)));})
                    .HeightOverride_Lambda([R](){return FOptionalSize(N(R,TEXT("height"),N(R,TEXT("size"),32)));})
                    [SNew(SScaleBox).Stretch(EStretch::ScaleToFit)[Artwork]];
                if(!S(R,TEXT("text")).IsEmpty())Content=SNew(SHorizontalBox)+SHorizontalBox::Slot().AutoWidth()[Content]
                    +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(6,0)[Label(R)];
            }
            else Content=Label(R);
            if(Type!=TEXT("button")&&(B(R,TEXT("clickable"),false)||R->HasField(TEXT("action"))))
                Content=SNew(SButton).ButtonStyle(&FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("NoBorder")).ContentPadding(0).IsFocusable(false)
                    .OnClicked_Lambda([H=Host,Plugin=Id,Key](){if(H.IsValid())H->ClickPluginRow(Plugin,Key);return FReply::Handled();})[Content];
            Content->SetEnabled(TAttribute<bool>::CreateLambda([R](){return B(R,TEXT("enabled"));}));
            Content->SetToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([R](){return FText::FromString(S(R,TEXT("tooltip")));})));
            Node->Widget=SAssignNew(Node->Frame,SBox)
                .Visibility_Lambda([H=Host,Plugin=Id,R](){return B(R,TEXT("hide_when_pinned"),false)&&H.IsValid()&&H->IsPluginWindowChromeHidden(Plugin)?EVisibility::Collapsed:EVisibility::SelfHitTestInvisible;})
                .HAlign(R->HasField(TEXT("width"))?HAlign_Left:HAlign_Fill)
                .VAlign(R->HasField(TEXT("height"))?VAlign_Top:VAlign_Fill)
                .Clipping(B(R,TEXT("wrap"))?EWidgetClipping::Inherit:EWidgetClipping::ClipToBounds)
                .Padding_Lambda([R](){return FMargin(FMath::Clamp(float(N(R,TEXT("padding"),2)),0.f,24.f));})
                [SNew(SBox).Tag(FName(*(TEXT("PluginControl.")+Key)))
                    .WidthOverride_Lambda([R](){return R->HasField(TEXT("width"))?FOptionalSize(N(R,TEXT("width"))):FOptionalSize();})
                    .HeightOverride_Lambda([R](){return R->HasField(TEXT("height"))?FOptionalSize(N(R,TEXT("height"))):FOptionalSize();})
                    [SAssignNew(Node->Visual,SACECompanionVisual).Data(R).Host(Host.Get()).Plugin(Id)[Content]]];
            if(Node->Sync&&!Node->Sync())Pending.Add(Node);
            return Node->Widget.ToSharedRef();
        }
        void Refresh()
        {
            if(!Host.IsValid())return;TSharedPtr<FACEClientPlugin> P;for(const auto& Candidate:Host->Plugins)if(Candidate->Id==Id){P=Candidate;break;}
            if(!P)
            {
                Rows->ClearChildren();RootChildren.Reset();Controls.Reset();Pending.Reset();Last.Reset();Revision=MAX_uint32;return;
            }
            if(Last.Pin()==P&&Revision==P->PanelRevision)
            {Pending.RemoveAll([](const auto& Node){return !Node->Sync||Node->Sync();});return;}
            FACEPluginStyle::Use(Host.Get(),Id);
            if(Last.Pin()!=P){Controls.Reset();Resolver.Reset();Expanded.Reset();Tabs.Reset();}
            Last=P;Revision=P->PanelRevision;Visited.Reset();Pending.Reset();
            TArray<TSharedPtr<SWidget>> Next;
            for(const auto& V:P->PanelRows)Next.Add(Build(V->AsObject()));
            if(RootChildren!=Next){Rows->ClearChildren();for(const auto& Widget:Next)Rows->AddSlot().AutoHeight().Padding(2)[Widget.ToSharedRef()];RootChildren=MoveTemp(Next);}
            for(auto It=Controls.CreateIterator();It;++It)if(!Visited.Contains(It.Key())){Expanded.Remove(It.Key());Tabs.Remove(It.Key());It.RemoveCurrent();}
            // Tick runs after the normal prepass. Measure changed/new content now
            // so the current frame never paints an empty, unmeasured replacement.
            SlatePrepass(GetCachedGeometry().Scale);
        }
    };
}
} // anonymous namespace
TSharedRef<SWidget> UACEPluginSubsystem::MakeCompanionPanel(const FString& Id)
{
    using namespace ACEPluginCompanionPanelPrivate;return SNew(SACECompanionRows).Host(this).Id(Id);}
