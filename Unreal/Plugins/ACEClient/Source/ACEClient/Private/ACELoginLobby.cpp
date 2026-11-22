#include "ACELoginWidget.h"
#include "Mobile/ACEMobileSettings.h"
#include "Components/SafeZone.h"
#include "ACEDesktopDisplay.h"
#include "ACEPlayerController.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACEUIFontStyles.h"
#include "UI/ACEUIResourceResolver.h"
#include "Mods/ACEPluginPanel.h"
#include "ACELoginSettings.h"
#include "ACEClientBuild.h"
#include "ACEUpdateSubsystem.h"
#include "Components/ProgressBar.h"
#include "Components/CheckBox.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACESession.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/NativeWidgetHost.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/StackBox.h"
#include "Components/StackBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WrapBox.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Engine/GameInstance.h"
#include "Kismet/KismetSystemLibrary.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include <shobjidl.h>
#endif

namespace
{
	const FLinearColor Ink(.008f, .006f, .003f, 1);
	const FLinearColor Surface(.018f, .014f, .009f, 1);
	const FLinearColor Tile(.045f, .036f, .025f, 1);
	const FLinearColor Gold(.36f, .27f, .14f, 1);
	const FLinearColor Muted(.58f, .54f, .46f, 1);
	const FLinearColor LobbyTextColor(.88f, .85f, .77f, 1);
	// Draw with Slate primitives so the eye works before DAT loads and in packages.
	class SPasswordVisibilityIcon : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SPasswordVisibilityIcon) {} SLATE_ATTRIBUTE(bool, Revealed) SLATE_END_ARGS()
		void Construct(const FArguments& Args)
		{
			Revealed=Args._Revealed;
			for(int32 I=0;I<=32;++I){const float T=I/32.f;Outline.Add(FVector2D(2+20*T,10-6*FMath::Sin(PI*T)));}
			for(int32 I=32;I>=0;--I){const float T=I/32.f;Outline.Add(FVector2D(2+20*T,10+6*FMath::Sin(PI*T)));}
			for(int32 I=0;I<=24;++I){const float A=2*PI*I/24.f;Pupil.Add(FVector2D(12+3*FMath::Cos(A),10+3*FMath::Sin(A)));}
		}
		FVector2D ComputeDesiredSize(float) const override {return FVector2D(24,20);}
		int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
		{
			const double Scale=FMath::Min(G.GetLocalSize().X/24.,G.GetLocalSize().Y/20.);
			const auto Art=G.MakeChild(FVector2D(24,20),FSlateLayoutTransform(Scale,(G.GetLocalSize()-FVector2D(24,20)*Scale)*.5));
			const auto Effect=ShouldBeEnabled(Enabled)?ESlateDrawEffect::None:ESlateDrawEffect::DisabledEffect;
			const auto Color=LobbyTextColor*Style.GetColorAndOpacityTint();
			for(const auto* Path:{&Outline,&Pupil})FSlateDrawElement::MakeLines(Out,Layer,Art.ToPaintGeometry(),*Path,Effect,Color,true,1.6f);
			if(Revealed.Get())FSlateDrawElement::MakeLines(Out,Layer,Art.ToPaintGeometry(),{FVector2D(3,2),FVector2D(21,18)},Effect,Color,true,1.8f);
			return Layer;
		}
	private:
		TAttribute<bool> Revealed;
		TArray<FVector2D> Outline,Pupil;
	};
	void ButtonStyle(UButton* Button, bool Accent)
	{
		const auto* Action=Cast<UACELoginActionButton>(Button);
		const bool Primary=Action && (Action->Action==TEXT("launch") || Action->Action==TEXT("downloadupdate") || Action->Action==TEXT("installupdate"));
		const FLinearColor Active=Primary?FLinearColor(.055f,.085f,.045f,1):FLinearColor(.105f,.079f,.041f,1);
		FButtonStyle Style;
		Style.SetNormal(FSlateRoundedBoxBrush(Accent ? Active : Tile, 0.f, Accent ? Gold : FLinearColor(.14f,.115f,.075f,1), 1.f));
		Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(.12f,.10f,.065f,1), 0.f, FLinearColor(.6f,.49f,.29f,1), 1.f));
		Style.SetPressed(FSlateRoundedBoxBrush(Ink, 0.f, Gold, 1.f));
		Style.SetDisabled(FSlateRoundedBoxBrush(Surface, 0.f, FLinearColor(.07f,.06f,.04f,1), 1.f));
		Style.SetNormalPadding(FMargin(14,7)); Style.SetPressedPadding(FMargin(14,7));
		Button->SetStyle(Style);
		if (auto* Size=Cast<USizeBox>(Button->GetContent()))
			if (auto* Caption=Cast<UTextBlock>(Size->GetContent())) Caption->SetColorAndOpacity(LobbyTextColor);
	}
	void AddLine(UVerticalBox* Box, UWidget* Child, float Bottom = 12)
	{
		Box->AddChildToVerticalBox(Child)->SetPadding(FMargin(0,0,0,Bottom));
	}
}

void UACELoginActionButton::HandleAction()
{
	// The callback can rebuild its list, including this button.
	const FString Command = Action, Argument = Value;
	if (Owner.IsValid()) Owner->RunAction(Command, Argument);
}

UTextBlock* UACELoginWidget::Label(const FString& String, int32 Size, bool bMuted)
{
	auto* T = WidgetTree->ConstructWidget<UACERetailTextBlock>();
	T->SetText(FText::FromString(String));
	T->SetFont(FCoreStyle::GetDefaultFontStyle(Size >= 26 ? TEXT("Bold") : TEXT("Regular"), Size));
	// The authored serif glyphs match the rest of the client. Until DAT is
	// available, UACERetailTextBlock keeps its readable engine-font fallback.
	const int32 Pixels=Size>=26?30:FMath::Clamp(Size,12,24);
	T->SetFontOverride(ACEUIFontStyles::FontIdForStyleName((Size>=26?FString(TEXT("fancyfont")):FString(TEXT("basefont")))+FString::FromInt(Pixels)));
	if(auto* GI=GetGameInstance())if(auto* Dat=GI->GetSubsystem<UACEDatSubsystem>())
		T->SetRetailElement(Dat->GetUiResources(),nullptr,FVector2D(1.25),0,false);
	T->SetColorAndOpacity(bMuted ? Muted : LobbyTextColor);
	T->SetAutoWrapText(true);
	T->SetVisibility(ESlateVisibility::HitTestInvisible);
	return T;
}

UACELoginActionButton* UACELoginWidget::ActionButton(const FString& Title, const FString& Action, const FString& Value, bool bAccent)
{
	auto* B = WidgetTree->ConstructWidget<UACELoginActionButton>(UACELoginActionButton::StaticClass(),Action==TEXT("launch")?FName(TEXT("LoginButton")):NAME_None);
	B->Action = Action; B->Value = Value; B->Owner = this;
	B->OnClicked.AddDynamic(B, &UACELoginActionButton::HandleAction);
	ButtonStyle(B, bAccent);
	auto* T = Label(Title, UACEMobileSettings::IsEnabled()?22:18); T->SetAutoWrapText(false); T->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
	T->SetJustification(ETextJustify::Center);
	auto* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetMinDesiredHeight(UACEMobileSettings::IsEnabled()?48:26);
	Size->SetContent(T); Cast<USizeBoxSlot>(T->Slot)->SetVerticalAlignment(VAlign_Center);
	B->SetContent(Size);
	return B;
}

UEditableTextBox* UACELoginWidget::Field(UVerticalBox* Parent, const FString& Title, const FName Name, bool bSecret)
{
	AddLine(Parent, Label(Title, 16, true), 5);
	auto* Box = WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), Name);
	FEditableTextBoxStyle Style = Box->GetWidgetStyle();
	Style.SetBackgroundImageNormal(FSlateRoundedBoxBrush(Ink, 0.f, Muted, 1.f));
	Style.SetBackgroundImageHovered(FSlateRoundedBoxBrush(Surface, 0.f, Gold, 1.f));
	Style.SetBackgroundImageFocused(FSlateRoundedBoxBrush(Ink, 0.f, Gold, 2.f));
	Style.SetForegroundColor(LobbyTextColor); Style.SetPadding(FMargin(12,7));
	Style.SetTextStyle(FTextBlockStyle().SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),20)).SetColorAndOpacity(LobbyTextColor));
	Box->SetWidgetStyle(Style); Box->SetIsPassword(bSecret); Box->SetClearKeyboardFocusOnCommit(true);
	Box->SetSelectAllTextWhenFocused(true);
	AddLine(Parent, Box, 12);
	return Box;
}

TSharedRef<SWidget> UACELoginWidget::RebuildWidget()
{
	EnsureDefaultLayout(); // Build before Super captures the root Slate widget.
	return Super::RebuildWidget();
}

void UACELoginWidget::EnsureDefaultLayout()
{
	if (HostBox || !WidgetTree) return;
	SetIsFocusable(true); SetVisibility(ESlateVisibility::Visible);
	ResponsiveScale=WidgetTree->ConstructWidget<UScaleBox>(); ResponsiveScale->SetStretch(EStretch::UserSpecified);
	WidgetTree->RootWidget=ResponsiveScale;
    if(UACEMobileSettings::IsEnabled())
    {auto* Safe=WidgetTree->ConstructWidget<USafeZone>();Safe->SetContent(ResponsiveScale);WidgetTree->RootWidget=Safe;}
	auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(); ResponsiveScale->SetContent(Canvas);
	// UScaleBoxSlot defaults to Center, which would allocate only the canvas's
	// tiny desired size instead of the viewport and clip all anchored controls.
	Cast<UScaleBoxSlot>(Canvas->Slot)->SetHorizontalAlignment(HAlign_Fill);
	Cast<UScaleBoxSlot>(Canvas->Slot)->SetVerticalAlignment(VAlign_Fill);
	BackdropBorder = WidgetTree->ConstructWidget<UBorder>();
	BackdropBorder->SetBrush(FSlateRoundedBoxBrush(Ink,0.f)); BackdropBorder->SetPadding(FMargin(0));
	auto* BackSlot = Canvas->AddChildToCanvas(BackdropBorder); BackSlot->SetAnchors(FAnchors(0,0,1,1)); BackSlot->SetOffsets(FMargin(0));
	SetUseDatIntroBackdrop(bUseDatIntroBackdrop);
	DashboardSize = WidgetTree->ConstructWidget<USizeBox>(); DashboardSize->SetWidthOverride(1052);
	// The shell never scrolls. Only the bounded server/account/directory lists do.
	auto* DashboardSlot = Canvas->AddChildToCanvas(DashboardSize); DashboardSlot->SetAnchors(FAnchors(.5f,0,.5f,1));
	DashboardSlot->SetOffsets(FMargin(-526,24,1052,64));
	auto* Root = WidgetTree->ConstructWidget<UVerticalBox>(); DashboardSize->SetContent(Root);
    if(UACEMobileSettings::IsEnabled())
    {
        // Short landscape screens scroll the readable launcher instead of
        // scaling all text/buttons down to fit a desktop-height dashboard.
        auto* TouchScroll=WidgetTree->ConstructWidget<UScrollBox>();
        DashboardSize->SetContent(TouchScroll);TouchScroll->AddChild(Root);
    }
	auto* Heading = WidgetTree->ConstructWidget<UHorizontalBox>(); AddLine(Root,Heading,16);
#if PLATFORM_ANDROID
    auto* Brand = Label(UACEMobileSettings::IsEnabled()?TEXT("AC:Unreal Mobile"):TEXT("AC:VR"), 34);
#else
	auto* Brand = Label(TEXT("AC:Unreal"), 34);
#endif
	Heading->AddChildToHorizontalBox(Brand)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
#if !PLATFORM_ANDROID
	if (!UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled())
	{
		auto* ResetSlot=Heading->AddChildToHorizontalBox(ActionButton(TEXT("Reset graphics"),TEXT("resetgraphics")));
		ResetSlot->SetPadding(FMargin(0,0,8,0)); ResetSlot->SetVerticalAlignment(VAlign_Top);
	}
#endif
	auto* QuitSlot=Heading->AddChildToHorizontalBox(ActionButton(TEXT("Quit"),TEXT("quit")));
	QuitSlot->SetHorizontalAlignment(HAlign_Right); QuitSlot->SetVerticalAlignment(VAlign_Top);
	auto* Header = WidgetTree->ConstructWidget<UWrapBox>(); Header->SetInnerSlotPadding(FVector2D(8,8)); AddLine(Root, Header, 18);
	for (const auto& Item : TArray<TPair<FString,FString>>{{TEXT("Play"),TEXT("play")},{TEXT("Accounts"),TEXT("accounts")},{TEXT("Browse servers"),TEXT("browser")},{TEXT("Game files"),TEXT("files")}})
	{
		auto* Button=ActionButton(Item.Key,Item.Value); Header->AddChildToWrapBox(Button); NavigationButtons.Add(Button);
	}
	UpdateNavButton=ActionButton(TEXT("Updates"),TEXT("updates")); Header->AddChildToWrapBox(UpdateNavButton); NavigationButtons.Add(UpdateNavButton);
	auto* PluginsButton=ActionButton(TEXT("Plugins"),TEXT("plugins")); Header->AddChildToWrapBox(PluginsButton); NavigationButtons.Add(PluginsButton);
	Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>(); Root->AddChildToVerticalBox(Pages)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	auto Card = [&](UVerticalBox*& Content) -> UBorder*
	{
		auto* B = WidgetTree->ConstructWidget<UBorder>(); B->SetBrush(FSlateRoundedBoxBrush(Surface,0.f,Gold,1.f)); B->SetPadding(FMargin(0));
		auto* Layers=WidgetTree->ConstructWidget<UOverlay>(); B->SetContent(Layers);
		auto* Frame=WidgetTree->ConstructWidget<UCanvasPanel>(); auto* FrameSlot=Layers->AddChildToOverlay(Frame);
		FrameSlot->SetHorizontalAlignment(HAlign_Fill); FrameSlot->SetVerticalAlignment(VAlign_Fill); Frame->SetVisibility(ESlateVisibility::HitTestInvisible);
		// The character creator's authored bronze frame, with corners at native
		// proportions and only the straight edge strips stretched.
		auto Art=[&](uint32 Id,FAnchors Anchors,FMargin Offsets)
		{
			auto* Image=WidgetTree->ConstructWidget<UImage>(); LobbyArtwork.Add(Image,Id);
			Image->SetVisibility(ESlateVisibility::HitTestInvisible);
			auto* ArtSlot=Frame->AddChildToCanvas(Image); ArtSlot->SetAnchors(Anchors); ArtSlot->SetOffsets(Offsets);
			Image->SetOpacity(0); // The flat border remains usable before DAT loads.
		};
		Art(0x06004CC7,FAnchors(0,0,1,0),FMargin(34,0,34,7));
		Art(0x06004CC7,FAnchors(0,1,1,1),FMargin(34,-7,34,7));
		Art(0x06004CB8,FAnchors(0,0,0,1),FMargin(0,34,5,34));
		Art(0x06004CB8,FAnchors(1,0,1,1),FMargin(-5,34,5,34));
		Art(0x06004CC3,FAnchors(0,0),FMargin(0,0,34,34));
		Art(0x06004CC4,FAnchors(1,0),FMargin(-34,0,34,34));
		Art(0x06004CC5,FAnchors(0,1),FMargin(0,-34,34,34));
		Art(0x06004CC6,FAnchors(1,1),FMargin(-34,-34,34,34));
		Content = WidgetTree->ConstructWidget<UVerticalBox>(); auto* ContentSlot=Layers->AddChildToOverlay(Content);
		ContentSlot->SetPadding(FMargin(22)); ContentSlot->SetHorizontalAlignment(HAlign_Fill); ContentSlot->SetVerticalAlignment(VAlign_Fill); return B;
	};
	auto Row = [&](UVerticalBox* Parent, const TArray<TPair<FString,FString>>& Buttons)
	{
		auto* R = WidgetTree->ConstructWidget<UWrapBox>(); R->SetInnerSlotPadding(FVector2D(8,8)); AddLine(Parent,R);
		for (const auto& Item : Buttons) R->AddChildToWrapBox(ActionButton(Item.Key, Item.Value));
	};
	// Play: two cards on desktop/headset; a stacked layout in narrow windows.
	auto* Play = WidgetTree->ConstructWidget<UStackBox>(); PlayLayout=Play; Play->SetOrientation(Orient_Horizontal); Pages->AddChild(Play);
	SidebarSize = WidgetTree->ConstructWidget<USizeBox>(); SidebarSize->SetWidthOverride(280); Play->AddChildToStackBox(SidebarSize)->SetPadding(FMargin(0,0,20,0));
	UVerticalBox* Sidebar; SidebarSize->SetContent(Card(Sidebar));
	AddLine(Sidebar,Label(TEXT("YOUR SERVERS"),16,true),16);
	ServerList = WidgetTree->ConstructWidget<UScrollBox>(); ServerList->SetScrollbarThickness(FVector2D(8,8));
	auto* ServerSlot=Sidebar->AddChildToVerticalBox(ServerList); ServerSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); ServerSlot->SetPadding(FMargin(0,0,0,16));
	AddLine(Sidebar,ActionButton(TEXT("+ Custom server"),TEXT("newserver")),8);
	AddLine(Sidebar,ActionButton(TEXT("Browse directory"),TEXT("browser")),0);
	DetailSize = WidgetTree->ConstructWidget<USizeBox>(); Play->AddChildToStackBox(DetailSize)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	UVerticalBox* Details; DetailSize->SetContent(Card(Details));
	ServerTitle=Label(TEXT("Choose your world"),22); AddLine(Details,ServerTitle,5);
	ServerDetail=Label(TEXT("Save a server or discover one in the directory."),16,true); AddLine(Details,ServerDetail,14);
	ServerDescription=Label(TEXT(""),18); AddLine(Details,ServerDescription,14);
	auto* Links=WidgetTree->ConstructWidget<UWrapBox>(); Links->SetInnerSlotPadding(FVector2D(8,8)); AddLine(Details,Links,16);
	WebsiteButton=ActionButton(TEXT("Website"),TEXT("website")); Links->AddChildToWrapBox(WebsiteButton);
	DiscordButton=ActionButton(TEXT("Discord"),TEXT("discord")); Links->AddChildToWrapBox(DiscordButton);
	Links->AddChildToWrapBox(ActionButton(TEXT("Edit server"),TEXT("editserver")));
	Links->AddChildToWrapBox(ActionButton(TEXT("Remove server"),TEXT("removeserver")));
	AddLine(Details,Label(TEXT("Accounts"),16,true),8);
	AccountList=WidgetTree->ConstructWidget<UScrollBox>(); AccountList->SetScrollbarThickness(FVector2D(8,8));
	auto* AccountSlot=Details->AddChildToVerticalBox(AccountList); AccountSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); AccountSlot->SetPadding(FMargin(0,0,0,12));
	LoginButton=ActionButton(TEXT("Launch"),TEXT("launch"),FString(),true); AddLine(Details,LoginButton,8);
	AddLine(Details,Label((PLATFORM_LINUX || PLATFORM_MAC) ? TEXT("Manage saved accounts and optional password saving on the Accounts tab.") : TEXT("Accounts are saved securely on this device."),14,true),0);
	// Server editor: explicit save/cancel avoids sending credentials to an edited endpoint by accident.
	UVerticalBox* Edit; Pages->AddChild(Card(Edit));
	AddLine(Edit,Label(TEXT("Server details"),30),18);
	NameBox=Field(Edit,TEXT("Server name"),TEXT("ServerName"));
	auto* Address=WidgetTree->ConstructWidget<UHorizontalBox>(); AddLine(Edit,Address,0);
	auto* HostCol=WidgetTree->ConstructWidget<UVerticalBox>(); Address->AddChildToHorizontalBox(HostCol)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	HostBox=Field(HostCol,TEXT("Hostname or IP address"),TEXT("HostBox"));
	auto* PortWidth=WidgetTree->ConstructWidget<USizeBox>(); PortWidth->SetWidthOverride(140); Address->AddChildToHorizontalBox(PortWidth)->SetPadding(FMargin(16,0,0,0));
	auto* PortCol=WidgetTree->ConstructWidget<UVerticalBox>(); PortWidth->SetContent(PortCol); PortBox=Field(PortCol,TEXT("Port"),TEXT("PortBox"));
	AddLine(Edit,Label(TEXT("Server type"),16,true),5);
	auto* Types=WidgetTree->ConstructWidget<UHorizontalBox>(); AddLine(Edit,Types,12);
	ACETypeButton=ActionButton(TEXT("ACE"),TEXT("ace")); GDLETypeButton=ActionButton(TEXT("GDLE"),TEXT("gdle"));
	Types->AddChildToHorizontalBox(ACETypeButton)->SetPadding(FMargin(0,0,8,0)); Types->AddChildToHorizontalBox(GDLETypeButton);
	DescriptionBox=Field(Edit,TEXT("Description (optional)"),TEXT("ServerDescription"));
	WebsiteBox=Field(Edit,TEXT("Website (optional)"),TEXT("ServerWebsite")); DiscordBox=Field(Edit,TEXT("Discord invite (optional)"),TEXT("ServerDiscord"));
	Row(Edit,{{TEXT("Save server"),TEXT("saveserver")},{TEXT("Cancel"),TEXT("play")}});
	// Directory stays inside the same widget surface: no OS dropdowns in stereo.
	UVerticalBox* Browser; Pages->AddChild(Card(Browser));
	AddLine(Browser,Label(TEXT("Find your next world"),30),6);
	AddLine(Browser,Label(TEXT("Community server directory  /  ACE + GDLE"),18,true),18);
	SearchBox=Field(Browser,TEXT("Search names, descriptions or addresses"),TEXT("ServerSearch"));
	SearchBox->OnTextChanged.AddDynamic(this,&UACELoginWidget::OnSearchChanged);
	BrowserList=WidgetTree->ConstructWidget<UScrollBox>(); BrowserList->SetScrollbarThickness(FVector2D(10,10));
	auto* BrowserSlot=Browser->AddChildToVerticalBox(BrowserList); BrowserSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); BrowserSlot->SetPadding(FMargin(0,0,0,14));
	BrowserDetail=Label(TEXT("Refresh to load the public server directory."),18); AddLine(Browser,BrowserDetail,16);
	Row(Browser,{{TEXT("Add to my servers"),TEXT("import")},{TEXT("Refresh"),TEXT("refresh")},{TEXT("Website"),TEXT("browserwebsite")},{TEXT("Discord"),TEXT("browserdiscord")}});
	AddLine(Browser,Label(TEXT("Directory maintained by acresources / Servers.xml. Adding a server does not connect to it."),14,true),0);
	UVerticalBox* Files; Pages->AddChild(Card(Files));
	AddLine(Files,Label(TEXT("Your game files"),30),12);
	FileProblem=Label(TEXT(""),20);
	FileProblem->SetColorAndOpacity(FLinearColor(1.f,.65f,.3f,1.f));
	FileProblem->SetVisibility(ESlateVisibility::Collapsed); AddLine(Files,FileProblem,16);
	AddLine(Files,Label(TEXT("Choose the folder containing your Asheron's Call .dat files. The client reads these files directly."),20),24);
	DatBox=Field(Files,TEXT("DAT folder"),TEXT("DatDirectory")); DatBox->OnTextChanged.AddDynamic(this,&UACELoginWidget::OnDatChanged);
#if PLATFORM_WINDOWS
	if (!UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled()) AddLine(Files,ActionButton(TEXT("Choose folder"),TEXT("browsefolder")));
	Row(Files,{{TEXT("Save location"),TEXT("savedat")},{TEXT("Use default"),TEXT("defaultdat")}});
#else
	Row(Files,{{TEXT("Save location"),TEXT("savedat")},{TEXT("Use app DAT folder"),TEXT("defaultdat")}});
	AddLine(Files,Label((PLATFORM_LINUX || PLATFORM_MAC) ? TEXT("Enter an absolute folder path. Match the DAT filenames exactly.") : TEXT("The folder must be accessible to this app. The Android installer uses the app's DAT folder."),18,true),20);
#endif
	FileStatus=Label(TEXT(""),20); AddLine(Files,FileStatus,24);
	AddLine(Files,Label(TEXT("Changing an already-loaded installation takes effect after restarting. No game files are moved or copied."),18,true));
	UVerticalBox* Updates; Pages->AddChild(Card(Updates));
	AddLine(Updates,Label(TEXT("Client updates"),28));
	AddLine(Updates,Label(TEXT("New versions are checked automatically when the launcher opens. Updates are optional and keep your accounts, settings, and DAT files."),18,true));
	AutoUpdateCheckBox=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("AutoUpdateCheckBox"));
	// Draw the indicator with Slate primitives and text so it stays legible in
	// packaged clients without relying on the editor's checkbox image resources.
	FCheckBoxStyle AutoUpdateStyle;
	AutoUpdateStyle.SetCheckBoxType(ESlateCheckBoxType::ToggleButton)
		.SetUncheckedImage(FSlateNoResource()).SetCheckedImage(FSlateNoResource())
		.SetUncheckedHoveredImage(FSlateRoundedBoxBrush(Tile,4.f)).SetCheckedHoveredImage(FSlateRoundedBoxBrush(Tile,4.f))
		.SetUncheckedPressedImage(FSlateRoundedBoxBrush(Ink,4.f)).SetCheckedPressedImage(FSlateRoundedBoxBrush(Ink,4.f));
	AutoUpdateStyle.SetForegroundColor(LobbyTextColor).SetHoveredForegroundColor(LobbyTextColor).SetPressedForegroundColor(LobbyTextColor)
		.SetCheckedForegroundColor(LobbyTextColor).SetCheckedHoveredForegroundColor(LobbyTextColor).SetCheckedPressedForegroundColor(LobbyTextColor);
	AutoUpdateStyle.SetPadding(FMargin(0,6)); AutoUpdateCheckBox->SetWidgetStyle(AutoUpdateStyle);
	auto* AutoUpdateRow=WidgetTree->ConstructWidget<UHorizontalBox>(); AutoUpdateCheckBox->SetContent(AutoUpdateRow);
	auto* IndicatorSize=WidgetTree->ConstructWidget<USizeBox>(); IndicatorSize->SetWidthOverride(28); IndicatorSize->SetHeightOverride(28);
	AutoUpdateRow->AddChildToHorizontalBox(IndicatorSize)->SetVerticalAlignment(VAlign_Center);
	auto* Indicator=WidgetTree->ConstructWidget<UBorder>(); Indicator->SetBrush(FSlateRoundedBoxBrush(Ink,3.f,Gold,2.f));
	Indicator->SetPadding(FMargin(0)); Indicator->SetVerticalAlignment(VAlign_Center); IndicatorSize->SetContent(Indicator);
	AutoUpdateMark=WidgetTree->ConstructWidget<UTextBlock>(); AutoUpdateMark->SetText(FText::FromString(TEXT("\u2713")));
	AutoUpdateMark->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"),22));
	AutoUpdateMark->SetAutoWrapText(false); AutoUpdateMark->SetJustification(ETextJustify::Center);
	AutoUpdateMark->SetColorAndOpacity(LobbyTextColor); AutoUpdateMark->SetRenderOpacity(0); Indicator->SetContent(AutoUpdateMark);
	auto* AutoUpdateCaption=AutoUpdateRow->AddChildToHorizontalBox(Label(TEXT("Automatically download and install updates"),18));
	AutoUpdateCaption->SetPadding(FMargin(10,0,0,0)); AutoUpdateCaption->SetVerticalAlignment(VAlign_Center);
	AutoUpdateCaption->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	AutoUpdateCheckBox->SetIsChecked(false);
	AutoUpdateCheckBox->OnCheckStateChanged.AddDynamic(this,&UACELoginWidget::OnAutoUpdateChanged);
	AddLine(Updates,AutoUpdateCheckBox,8);
	AddLine(Updates,Label(TEXT("Off by default. When enabled, checks every 15 minutes in the launcher and updates before you log in. Never interrupts gameplay. Cancel pauses automatic updates until the next app launch or Check now."),16,true));
	UpdateText=Label(TEXT("Checking for updates..."),20); AddLine(Updates,UpdateText,20);
	UpdateProgress=WidgetTree->ConstructWidget<UProgressBar>(); UpdateProgress->SetFillColorAndOpacity(Gold);
	auto* ProgressSize=WidgetTree->ConstructWidget<USizeBox>(); ProgressSize->SetHeightOverride(16); ProgressSize->SetContent(UpdateProgress); AddLine(Updates,ProgressSize,20);
	auto* UpdateActions=WidgetTree->ConstructWidget<UWrapBox>(); UpdateActions->SetInnerSlotPadding(FVector2D(10,10)); AddLine(Updates,UpdateActions,20);
	CheckUpdateButton=ActionButton(TEXT("Check now"),TEXT("checkupdate")); UpdateActions->AddChildToWrapBox(CheckUpdateButton);
	DownloadUpdateButton=ActionButton(TEXT("Download update"),TEXT("downloadupdate"),FString(),true); UpdateActions->AddChildToWrapBox(DownloadUpdateButton);
	InstallUpdateButton=ActionButton(PLATFORM_ANDROID?TEXT("Install"):TEXT("Install and restart"),TEXT("installupdate"),FString(),true); UpdateActions->AddChildToWrapBox(InstallUpdateButton);
	CancelUpdateButton=ActionButton(TEXT("Cancel update"),TEXT("cancelupdate")); UpdateActions->AddChildToWrapBox(CancelUpdateButton);
	UpdateInstallHelp=Label(ACEUpdates::InstallationNotice(PLATFORM_ANDROID,UACEMobileSettings::IsEnabled()),18,true); AddLine(Updates,UpdateInstallHelp);
	Row(Updates,{{TEXT("Release notes"),TEXT("updatenotes")},{TEXT("Back to play"),TEXT("play")}});
	auto* PluginPanel=WidgetTree->ConstructWidget<UACEPluginPanel>();
	Pages->AddChild(PluginPanel);
	UVerticalBox* Accounts; Pages->AddChild(Card(Accounts));
	AddLine(Accounts,Label(TEXT("Manage accounts"),30));
	AddLine(Accounts,Label(TEXT("Drag the grip beside an account to reorder. Use New account to add another login, even with the same username."),18,true));
	ManagedAccountList=WidgetTree->ConstructWidget<UScrollBox>(); ManagedAccountList->SetScrollbarThickness(FVector2D(8,8));
	auto* ManagedSlot=Accounts->AddChildToVerticalBox(ManagedAccountList); ManagedSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); ManagedSlot->SetPadding(FMargin(0,0,0,12));
	AccountAliasBox=Field(Accounts,TEXT("Alias (optional)"),TEXT("AccountAliasBox"));
	auto* Credentials=WidgetTree->ConstructWidget<UHorizontalBox>(); AddLine(Accounts,Credentials,0);
	auto* UserColumn=WidgetTree->ConstructWidget<UVerticalBox>(); auto* UserSlot=Credentials->AddChildToHorizontalBox(UserColumn);
	UserSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); UserSlot->SetPadding(FMargin(0,0,16,0));
	auto* PasswordColumn=WidgetTree->ConstructWidget<UVerticalBox>(); Credentials->AddChildToHorizontalBox(PasswordColumn)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	AccountBox=Field(UserColumn,TEXT("Username"),TEXT("AccountBox"));
	PasswordBox=Field(PasswordColumn,TEXT("Password"),TEXT("PasswordBox"),true);
	PasswordBox->RemoveFromParent();
	auto* PasswordRow=WidgetTree->ConstructWidget<UHorizontalBox>(); AddLine(PasswordColumn,PasswordRow);
	PasswordRow->AddChildToHorizontalBox(PasswordBox)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	PasswordVisibilityButton=ActionButton(TEXT(""),TEXT("togglepassword"));
	auto EyeStyle=PasswordVisibilityButton->GetStyle(); EyeStyle.SetNormalPadding(FMargin(8)); EyeStyle.SetPressedPadding(FMargin(8)); PasswordVisibilityButton->SetStyle(EyeStyle);
	auto* Eye=WidgetTree->ConstructWidget<UNativeWidgetHost>(); Eye->SetVisibility(ESlateVisibility::HitTestInvisible);
	Eye->SetContent(SNew(SPasswordVisibilityIcon).Revealed_Lambda([Box=TWeakObjectPtr<UEditableTextBox>(PasswordBox)](){return Box.IsValid()&&!Box->GetIsPassword();}));
	PasswordVisibilityButton->SetContent(Eye);
	auto* EyeWidth=WidgetTree->ConstructWidget<USizeBox>(); EyeWidth->SetWidthOverride(44); EyeWidth->SetContent(PasswordVisibilityButton);
	PasswordRow->AddChildToHorizontalBox(EyeWidth)->SetPadding(FMargin(8,0,0,0));
	SetPasswordVisible(false);
#if PLATFORM_LINUX || PLATFORM_MAC
	RememberPasswordsCheckBox=WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(),TEXT("RememberPasswordsCheckBox"));
	RememberPasswordsCheckBox->SetWidgetStyle(AutoUpdateStyle);
	auto* RememberRow=WidgetTree->ConstructWidget<UHorizontalBox>(); RememberPasswordsCheckBox->SetContent(RememberRow);
	auto* RememberIndicator=WidgetTree->ConstructWidget<UBorder>(); RememberIndicator->SetBrush(FSlateRoundedBoxBrush(Ink,4.f,Gold,1.f));
	auto* RememberSize=WidgetTree->ConstructWidget<USizeBox>(); RememberSize->SetWidthOverride(28); RememberSize->SetHeightOverride(28); RememberSize->SetContent(RememberIndicator);
	RememberRow->AddChildToHorizontalBox(RememberSize)->SetVerticalAlignment(VAlign_Center);
	RememberPasswordsMark=Label(TEXT("\u2713"),18); RememberPasswordsMark->SetRenderOpacity(0); RememberIndicator->SetContent(RememberPasswordsMark);
	auto* RememberCaption=RememberRow->AddChildToHorizontalBox(Label(TEXT("Remember passwords on this device (unencrypted)"),18,true));
	RememberCaption->SetPadding(FMargin(10,0,0,0)); RememberCaption->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	RememberPasswordsCheckBox->OnCheckStateChanged.AddDynamic(this,&UACELoginWidget::OnRememberPasswordsChanged);
	AddLine(Accounts,RememberPasswordsCheckBox,8);
#endif
	Row(Accounts,{{TEXT("Save account"),TEXT("saveaccount")},{TEXT("New account"),TEXT("newaccount")},{TEXT("Remove account"),TEXT("removeaccount")}});
	AddLine(Accounts,Label((PLATFORM_LINUX || PLATFORM_MAC) ? TEXT("Password saving is off by default. The local file is restricted to your computer's user account. Turning this off removes saved passwords. Passwords remain available for this session.") : TEXT("Accounts are saved securely on this device."),14,true));
	Row(Accounts,{{TEXT("Back to play"),TEXT("play")}});
	// Inline destructive-action confirmation is also fully usable with VR pointers.
	FooterSize=WidgetTree->ConstructWidget<USizeBox>(); FooterSize->SetWidthOverride(1028);
	auto* FooterSlot=Canvas->AddChildToCanvas(FooterSize); FooterSlot->SetAnchors(FAnchors(.5f,1)); FooterSlot->SetAlignment(FVector2D(.5f,1)); FooterSlot->SetPosition(FVector2D(0,-18)); FooterSlot->SetAutoSize(true);
	auto* Footer=WidgetTree->ConstructWidget<UVerticalBox>(); FooterSize->SetContent(Footer);
	UVerticalBox* Confirmation; ConfirmPanel=Card(Confirmation); AddLine(Footer,ConfirmPanel);
	ConfirmText=Label(TEXT(""),18); AddLine(Confirmation,ConfirmText);
	Row(Confirmation,{{TEXT("Remove"),TEXT("confirmremove")},{TEXT("Keep"),TEXT("cancelremove")}}); ConfirmPanel->SetVisibility(ESlateVisibility::Collapsed);
	auto* StatusBorder=WidgetTree->ConstructWidget<UBorder>(); StatusBorder->SetBrush(FSlateRoundedBoxBrush(Ink,0.f)); StatusBorder->SetPadding(FMargin(12,10));
	StatusText=Label(TEXT(""),18); StatusBorder->SetContent(StatusText); AddLine(Footer,StatusBorder,0);
	StatusBorder->SetVisibility(ESlateVisibility::Collapsed);
	auto* BuildLabel=Label(FString::Printf(TEXT("Version %s"),ACEClientBuild::Version),16);
	BuildLabel->SetJustification(ETextJustify::Right); AddLine(Footer,BuildLabel,0);
	// Retained for Blueprint/test compatibility; retail DAT owns character selection.
	EnterWorldButton=ActionButton(TEXT("Enter world"),TEXT("enter")); EnterWorldButton->SetVisibility(ESlateVisibility::Collapsed);
	CharacterListLabel=Label(TEXT("")); CharacterListBox=WidgetTree->ConstructWidget<UScrollBox>();
}

void UACELoginWidget::NativeConstruct()
{
	EnsureDefaultLayout(); Super::NativeConstruct();
	if (auto* GI=GetGameInstance())
	{
		Updater=GI->GetSubsystem<UACEUpdateSubsystem>();
		Client=GI->GetSubsystem<UACEClientSubsystem>();
		if (Client)
		{
			Client->OnSessionStateChanged.AddUniqueDynamic(this,&UACELoginWidget::HandleState);
			Client->OnCharacterList.AddUniqueDynamic(this,&UACELoginWidget::HandleCharacters);
			Client->OnLogMessage.AddUniqueDynamic(this,&UACELoginWidget::HandleLog);
		}
	}
	if (!bLoginSettingsLoaded)
	{
		Profile.Host=DefaultHost; Profile.Port=FString::FromInt(DefaultPort); Profile.Account=DefaultAccount; Profile.Password=DefaultPassword;
		ACELoginSettings::Load(Profile, ProfilePath.IsEmpty()?ACELoginSettings::GetPath():ProfilePath); Profile.MigrateLegacy(); bLoginSettingsLoaded=true;
		SelectServer(Profile.SelectedServerId);
		if (Profile.DatDirectory.IsEmpty())
			if (auto* GI=GetGameInstance()) if (auto* Dat=GI->GetSubsystem<UACEDatSubsystem>()) Profile.DatDirectory=Dat->GetDatDirectory();
		DatBox->SetText(FText::FromString(Profile.DatDirectory));
		FString Cached, Error;
		if (IFileManager::Get().FileSize(*ACELoginProfile::DirectoryCachePath()) <= 2*1024*1024
			&& FFileHelper::LoadFileToString(Cached,*ACELoginProfile::DirectoryCachePath())) ACELoginProfile::ParseDirectory(Cached,Directory,Error);
		SetStatus(FString());
		ValidateGameFiles();
	}
	AutoUpdateCheckBox->SetIsChecked(Profile.bAutoUpdate);
	if (RememberPasswordsCheckBox) RememberPasswordsCheckBox->SetIsChecked(Profile.bSavePasswordsLocally);
	if (RememberPasswordsMark) RememberPasswordsMark->SetRenderOpacity(Profile.bSavePasswordsLocally?1.f:0.f);
	if (Updater) { Updater->SetAutoUpdateEnabled(Profile.bAutoUpdate); Updater->Check(true); }
	RefreshUpdateControls();
	ForceLayoutPrepass();
}

void UACELoginWidget::NativeDestruct()
{
	SaveLoginEntries();
	if (DirectoryRequest) { DirectoryRequest->OnProcessRequestComplete().Unbind(); DirectoryRequest->CancelRequest(); DirectoryRequest.Reset(); bRefreshingDirectory=false; }
	if (Client)
	{
		Client->OnSessionStateChanged.RemoveDynamic(this,&UACELoginWidget::HandleState);
		Client->OnCharacterList.RemoveDynamic(this,&UACELoginWidget::HandleCharacters);
		Client->OnLogMessage.RemoveDynamic(this,&UACELoginWidget::HandleLog);
	}
	Super::NativeDestruct();
}

void UACELoginWidget::NativeTick(const FGeometry& Geometry, float Dt)
{
	Super::NativeTick(Geometry,Dt);
	if (bAccountOrderDirty) RefreshAccounts();
	TickAccountDrag(Dt);
	UpdatePollTime-=Dt;
	if (UpdatePollTime<=0)
	{
		UpdatePollTime=.25f;
		// The launcher can open before the asynchronous DAT load completes.
		// Install the retail fonts once ready without rebuilding the form.
		if(!bRetailFontsReady)if(auto* GI=GetGameInstance())if(auto* Dat=GI->GetSubsystem<UACEDatSubsystem>();Dat&&Dat->IsDatReady())
		{
			WidgetTree->ForEachWidget([Dat](UWidget* Widget)
			{
				if(auto* Text=Cast<UACERetailTextBlock>(Widget))
					Text->SetRetailElement(Dat->GetUiResources(),nullptr,FVector2D(1.25),0,false);
			});
			for(const auto& Art:LobbyArtwork) if(auto* Texture=Dat->GetUiResources()->ResolveTexture(Art.Value))
			{
				Art.Key->SetBrushFromTexture(Texture); Art.Key->SetOpacity(.65f);
			}
			bRetailFontsReady=true;
		}
		if (Updater)
		{
			Updater->PollInstall();
			if (Updater->IsAutoUpdateEnabled()) SaveLoginEntries(); // Persist edits before an automatic restart.
			const auto Action=Updater->PollAutoUpdate(UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled());
			if (Action==EACEAutoUpdateAction::Download || Action==EACEAutoUpdateAction::Install
				|| Updater->State==EACEUpdateState::InstallNotice) Pages->SetActiveWidgetIndex(4);
		}
		RefreshUpdateControls();
	}
	if(StyledPage!=Pages->GetActiveWidgetIndex())
	{
		StyledPage=Pages->GetActiveWidgetIndex();
		const int32 PageForTab[]={0,6,2,3,4,5};
		for(int32 I=0;I<NavigationButtons.Num();++I) ButtonStyle(NavigationButtons[I],PageForTab[I]==StyledPage);
	}
	// Scale the whole layout together, allowing a narrower stacked Play page.
	// Do not impose a minimum scale that would push controls off small screens.
	const FVector2D View=Geometry.GetLocalSize();
	if(View.X<=0 || View.Y<=0) return;
	const float Scale=FMath::Clamp(float(UACEMobileSettings::IsEnabled()?FMath::Min(View.X/1052.,FMath::Max(View.Y,540.)/540.):FMath::Min(View.X/800.,View.Y/960.)),.1f,2.5f);
	if (ResponsiveScale && !FMath::IsNearlyEqual(ResponsiveScale->GetUserSpecifiedScale(),Scale)) ResponsiveScale->SetUserSpecifiedScale(Scale);
	const float Width=FMath::Clamp(float(View.X)/Scale-48.f,280.f,1160.f);
	if (DashboardSize && !FMath::IsNearlyEqual(Width,LayoutWidth,1.f))
	{
		LayoutWidth=Width; DashboardSize->SetWidthOverride(Width); FooterSize->SetWidthOverride(Width);
		const bool Narrow=Width<900.f;
		PlayLayout->SetOrientation(Narrow?Orient_Vertical:Orient_Horizontal);
		SidebarSize->SetWidthOverride(Narrow?Width:280.f);
		if(Narrow) SidebarSize->SetHeightOverride(320); else SidebarSize->ClearHeightOverride();
		Cast<UStackBoxSlot>(SidebarSize->Slot)->SetPadding(Narrow?FMargin(0,0,0,16):FMargin(0,0,20,0));
	}
	// Reserve the actual footer height; errors and confirmation buttons must
	// never cover Launch or require scrolling the main page to reach it.
	if (DashboardSize && FooterSize)
		if (auto* DashboardSlot=Cast<UCanvasPanelSlot>(DashboardSize->Slot))
		{
			const FMargin Offsets(-Width*.5f,24,Width,FooterSize->GetDesiredSize().Y+34.f);
			if(DashboardSlot->GetOffsets()!=Offsets) DashboardSlot->SetOffsets(Offsets);
		}
}

void UACELoginWidget::OnRememberPasswordsChanged(bool bChecked)
{
	if (RememberPasswordsMark) RememberPasswordsMark->SetRenderOpacity(bChecked?1.f:0.f);
	if (Profile.bSavePasswordsLocally==bChecked) return;
	Profile.bSavePasswordsLocally=bChecked;
	bLoginEntriesDirty=true;
	SaveLoginEntries();
}

void UACELoginWidget::OnAutoUpdateChanged(bool bChecked)
{
	Profile.bAutoUpdate=bChecked;
	bLoginEntriesDirty=true;
	SaveLoginEntries();
	if (Updater) Updater->SetAutoUpdateEnabled(bChecked);
	RefreshUpdateControls();
}

void UACELoginWidget::RefreshUpdateControls()
{
	if (AutoUpdateMark) AutoUpdateMark->SetRenderOpacity(Profile.bAutoUpdate?1.f:0.f);
	if (!Updater || !UpdateText) return;
	const auto State=Updater->State;
	const bool Busy=Updater->IsBusy();
	const bool Available=Updater->Release.Number>ACEClientBuild::ReleaseNumber;
	FString Text=Updater->Message;
	if (State==EACEUpdateState::Downloading) Text+=FString::Printf(TEXT(" %.0f%%"),Updater->Progress()*100);
	if (UpdateText->GetText().ToString()!=Text) UpdateText->SetText(FText::FromString(Text));
	UpdateText->SetColorAndOpacity(LobbyTextColor);
	UpdateInstallHelp->SetVisibility(State==EACEUpdateState::Ready || State==EACEUpdateState::InstallNotice || State==EACEUpdateState::Installing?ESlateVisibility::Collapsed:ESlateVisibility::HitTestInvisible);
	UpdateProgress->SetPercent(State==EACEUpdateState::Ready || State==EACEUpdateState::InstallNotice || State==EACEUpdateState::Installing?1.f:Updater->Progress());
	CheckUpdateButton->SetIsEnabled(!Busy && State!=EACEUpdateState::Checking);
	DownloadUpdateButton->SetVisibility(Available && !Busy && State!=EACEUpdateState::Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
	InstallUpdateButton->SetVisibility(State==EACEUpdateState::Ready?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
	CancelUpdateButton->SetVisibility(State==EACEUpdateState::Downloading || State==EACEUpdateState::Verifying || State==EACEUpdateState::InstallNotice?ESlateVisibility::Visible:ESlateVisibility::Collapsed);
	AutoUpdateCheckBox->SetIsEnabled(State!=EACEUpdateState::Installing);
	LoginButton->SetIsEnabled(!Busy && Profile.SelectedServer()!=nullptr);
	if (auto* Size=Cast<USizeBox>(UpdateNavButton->GetContent())) if(auto* Label=Cast<UTextBlock>(Size->GetContent()))
	{ const FString Title=Available?TEXT("Update available"):TEXT("Updates"); if(Label->GetText().ToString()!=Title)Label->SetText(FText::FromString(Title)); }
}

void UACELoginWidget::OnLoginEntryChanged(const FText&) { bLoginEntriesDirty=true; }
void UACELoginWidget::OnLoginEntryCommitted(const FText&, ETextCommit::Type) { SaveLoginEntries(); }
void UACELoginWidget::SaveLoginEntries()
{
	if (!bLoginSettingsLoaded || !bLoginEntriesDirty) return;
	if (ACELoginSettings::Save(Profile, ProfilePath.IsEmpty()?ACELoginSettings::GetPath():ProfilePath)) bLoginEntriesDirty=false;
	else SetStatus(TEXT("Could not save settings on this device. Check available storage."));
}
void UACELoginWidget::OnLoginClicked() { DoLogin(); }
void UACELoginWidget::OnEnterWorldClicked() { DoEnterSelectedCharacter(); }

void UACELoginWidget::RefreshServers()
{
	ServerList->ClearChildren();
	if (Profile.Servers.IsEmpty()) ServerList->AddChild(Label(TEXT("No saved servers yet.\n\nBrowse the directory or add your own."),18,true));
	for (const auto& S:Profile.Servers)
	{
		auto* B=ActionButton(S.Name,TEXT("selectserver"),S.Id,S.Id==Profile.SelectedServerId);
		Cast<UScrollBoxSlot>(ServerList->AddChild(B))->SetPadding(FMargin(0,0,0,8));
	}
}

void UACELoginWidget::SelectServer(const FString& Id)
{
	SetPasswordVisible(false);
	const FString AccountId = Profile.SelectedAccountId;
	Profile.SelectedServerId=Id; Profile.SelectedAccountId=AccountId;
	const auto* S=Profile.SelectedServer();
	HostBox->SetText(FText::FromString(S?S->Host:TEXT(""))); PortBox->SetText(FText::FromString(FString::FromInt(S?S->Port:9000)));
	ServerTitle->SetText(FText::FromString(S?S->Name:TEXT("Choose your world")));
	ServerDetail->SetText(FText::FromString(S?FString::Printf(TEXT("%s  /  %s:%d"),S->bGDLE?TEXT("GDLE"):TEXT("ACE"),*S->Host,S->Port):TEXT("Add a server to get started.")));
	ServerDescription->SetText(FText::FromString(S?S->Description.Left(240):TEXT("")));
	ServerDescription->SetVisibility(S && !S->Description.IsEmpty()?ESlateVisibility::HitTestInvisible:ESlateVisibility::Collapsed);
	WebsiteButton->SetIsEnabled(S && ACELoginProfile::IsWebLink(S->Website)); DiscordButton->SetIsEnabled(S && ACELoginProfile::IsWebLink(S->Discord));
	LoginButton->SetIsEnabled(S!=nullptr);
	const auto* A=Profile.Accounts.FindByPredicate([&](const auto& E){return E.Id==AccountId;});
	if (!A && !Profile.Accounts.IsEmpty()) A=&Profile.Accounts[0];
	Profile.SelectedAccountId=A?A->Id:FString();
	AccountBox->SetText(FText::FromString(A?A->Username:TEXT(""))); PasswordBox->SetText(FText::FromString(A?A->Password:TEXT("")));
	AccountAliasBox->SetText(FText::FromString(A?A->Alias:TEXT("")));
	Profile.Host=S?S->Host:FString(); Profile.Port=S?FString::FromInt(S->Port):TEXT("9000");
	Profile.Account=A?A->Username:FString(); Profile.Password=A?A->Password:FString();
	RefreshServers(); RefreshAccounts(); bLoginEntriesDirty=true;
}

void UACELoginWidget::RefreshAccounts()
{
	bAccountOrderDirty=false;
	for(auto* List:{AccountList.Get(),ManagedAccountList.Get()})
	{
		const float ScrollOffset=List->GetScrollOffset();
		List->ClearChildren();
		for (const auto& A:Profile.Accounts)
		{
			auto* Button=ActionButton(A.DisplayName(),TEXT("selectaccount"),A.Id,A.Id==Profile.SelectedAccountId);
			Button->bAccountDragHandle=List==ManagedAccountList;
			Button->SetToolTipText(FText::FromString(A.DisplayName()));
			Cast<UScrollBoxSlot>(List->AddChild(Button))->SetPadding(FMargin(0,0,0,6));
		}
		if (!List->GetChildrenCount()) List->AddChild(Label(TEXT("Add a login on the Accounts tab."),18,true));
		List->SetScrollOffset(ScrollOffset);
	}
}

bool UACELoginWidget::CanReorderAccounts() const
{
	return Pages && Pages->GetActiveWidgetIndex()==6 && PendingRemoval.IsEmpty();
}

bool UACELoginWidget::MoveAccountInOrder(const FString& Id, const FString& TargetId, bool bAfter)
{
	if (!CanReorderAccounts() || Id==TargetId) return false;
	const int32 From=Profile.Accounts.IndexOfByPredicate([&](const auto& A){return A.Id==Id;});
	const int32 Target=Profile.Accounts.IndexOfByPredicate([&](const auto& A){return A.Id==TargetId;});
	if (From==INDEX_NONE || Target==INDEX_NONE) return false;
	int32 To=Target+(bAfter?1:0);
	if (From<To) --To;
	if (From==To) return false;
	// Move the entire identity; usernames need not be unique. Leave selection
	// and any unsaved form edits alone, and use the existing profile format.
	FACELoginAccount Account=MoveTemp(Profile.Accounts[From]);
	Profile.Accounts.RemoveAt(From);
	Profile.Accounts.Insert(MoveTemp(Account),To);
	bLoginEntriesDirty=true;
	SaveLoginEntries();
	// Rebuild both lists after the drop callback has finished using the row.
	bAccountOrderDirty=true;
	return true;
}

bool UACELoginWidget::StoreAccount()
{
	const FString User=AccountBox->GetText().ToString().TrimStartAndEnd(), Secret=PasswordBox->GetText().ToString();
	const FString Alias=AccountAliasBox->GetText().ToString().TrimStartAndEnd();
	if (User.IsEmpty() || User.Len()>128 || Secret.IsEmpty() || Secret.Len()>256 || User.Contains(TEXT(":")))
	{ SetStatus(TEXT("Enter a username and password. Usernames cannot contain a colon.")); return false; }
	if (Alias.Len()>100 || Alias.Contains(TEXT("\n")) || Alias.Contains(TEXT("\r")))
	{ SetStatus(TEXT("Enter an alias of up to 100 characters on one line.")); return false; }
	auto* A=Profile.Accounts.FindByPredicate([&](const auto& E){return E.Id==Profile.SelectedAccountId;});
	// New account always creates an independent entry. Matching credentials can
	// belong to different aliases; only selection by ID is an edit operation.
	if (!A)
	{
		if (Profile.Accounts.Num()>=512) { SetStatus(TEXT("Remove an unused account before adding another.")); return false; }
		A=&Profile.Accounts.AddDefaulted_GetRef(); A->Id=FGuid::NewGuid().ToString();
	}
	A->Username=User; A->Password=Secret; A->Alias=Alias; Profile.SelectedAccountId=A->Id;
	Profile.Account=User; Profile.Password=Secret; bLoginEntriesDirty=true; SaveLoginEntries(); RefreshAccounts(); return !bLoginEntriesDirty;
}

void UACELoginWidget::EditServer(bool bNew)
{
	const auto* S=bNew?nullptr:Profile.SelectedServer();
	if (!bNew && !S) { SetStatus(TEXT("Select a server to edit.")); return; }
	EditingServerId=S?S->Id:FString(); bEditingGDLE=S && S->bGDLE;
	NameBox->SetText(FText::FromString(S?S->Name:TEXT(""))); HostBox->SetText(FText::FromString(S?S->Host:TEXT("")));
	PortBox->SetText(FText::FromString(S?FString::FromInt(S->Port):TEXT("9000")));
	DescriptionBox->SetText(FText::FromString(S?S->Description:TEXT("")));
	WebsiteBox->SetText(FText::FromString(S?S->Website:TEXT(""))); DiscordBox->SetText(FText::FromString(S?S->Discord:TEXT("")));
	ButtonStyle(ACETypeButton,!bEditingGDLE); ButtonStyle(GDLETypeButton,bEditingGDLE); Pages->SetActiveWidgetIndex(1);
	SetStatus(TEXT("Choose the login type used by this server."));
}

void UACELoginWidget::DoLogin()
{
	SetPasswordVisible(false);
	if (Updater && Updater->IsBusy()) { SetStatus(TEXT("Finish or cancel the update before logging in.")); return; }
	// Check both files before authenticating: the cell index is deliberately
	// deferred, so portal readiness alone can still lead to an empty world.
	if (!ValidateGameFiles()) return;
	if (!Profile.SelectedServer()) { SetStatus(TEXT("Choose a server first.")); return; }
	if (!StoreAccount()) { Pages->SetActiveWidgetIndex(6); return; }
	const auto* S=Profile.SelectedServer();
	if (Client && S)
	{
		if (auto* GI=GetGameInstance()) if (auto* Dat=GI->GetSubsystem<UACEDatSubsystem>()) Dat->BeginBackgroundLoad(true);
		SetStatus(TEXT("Connecting..."));
		if (!Client->Login(S->Host,S->Port,Profile.Account,Profile.Password,S->bGDLE)) SetStatus(TEXT("Could not start the connection. Check the server address."));
	}
}

void UACELoginWidget::OnSearchChanged(const FText&) { RefreshBrowser(); }
void UACELoginWidget::OnDatChanged(const FText&) { UpdateDatStatus(); }

void UACELoginWidget::RefreshBrowser()
{
	BrowserList->ClearChildren(); const FString Search=SearchBox->GetText().ToString().TrimStartAndEnd();
	for (const auto& S:Directory)
	{
		if (!Search.IsEmpty() && !S.Name.Contains(Search) && !S.Description.Contains(Search) && !S.Host.Contains(Search)) continue;
		const bool Saved=Profile.Servers.ContainsByPredicate([&](const auto& E){return E.Host.Equals(S.Host,ESearchCase::IgnoreCase) && E.Port==S.Port;});
		const FString Name=FString::Printf(TEXT("%s  /  %s%s"),*S.Name,S.bGDLE?TEXT("GDLE"):TEXT("ACE"),Saved?TEXT("  /  Saved"):TEXT(""));
		Cast<UScrollBoxSlot>(BrowserList->AddChild(ActionButton(Name,TEXT("directoryselect"),S.Id,S.Id==BrowserServerId)))->SetPadding(FMargin(0,0,0,6));
	}
	if (!BrowserList->GetChildrenCount()) BrowserList->AddChild(Label(Directory.IsEmpty()?TEXT("No cached directory. Refresh when connected to the internet."):TEXT("No servers match your search."),18,true));
	const auto* S=Directory.FindByPredicate([&](const auto& E){return E.Id==BrowserServerId;});
	BrowserDetail->SetText(FText::FromString(S?FString::Printf(TEXT("%s:%d\n%s"),*S->Host,S->Port,*S->Description.Left(600)):TEXT("Select a server to see its details and community links.")));
}

void UACELoginWidget::FetchDirectory()
{
	if (bRefreshingDirectory) return;
	bRefreshingDirectory=true; SetStatus(TEXT("Refreshing the community directory..."));
	DirectoryRequest=FHttpModule::Get().CreateRequest(); DirectoryRequest->SetURL(ACELoginProfile::DirectoryUrl);
	DirectoryRequest->SetVerb(TEXT("GET")); DirectoryRequest->SetTimeout(20.f);
	DirectoryRequest->SetHeader(TEXT("Accept"),TEXT("application/xml,text/xml,text/plain"));
	TWeakObjectPtr<UACELoginWidget> Weak(this);
	DirectoryRequest->OnProcessRequestComplete().BindLambda([Weak](FHttpRequestPtr, FHttpResponsePtr Response, bool Success)
	{
		if (!Weak.IsValid()) return; auto* W=Weak.Get(); W->bRefreshingDirectory=false;
		FString Error; TArray<FACELoginServer> Parsed;
		if (!Success || !Response || Response->GetResponseCode()!=200 || Response->GetContent().Num()>2*1024*1024)
			Error=TEXT("Directory unavailable. Your saved servers and cached directory are still available.");
		else if (ACELoginProfile::ParseDirectory(Response->GetContentAsString(),Parsed,Error))
		{
			W->Directory=MoveTemp(Parsed);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ACELoginProfile::DirectoryCachePath()),true);
			FFileHelper::SaveStringToFile(Response->GetContentAsString(),*ACELoginProfile::DirectoryCachePath(),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			W->SetStatus(FString::Printf(TEXT("%d community servers. Select one to add it to your list."),W->Directory.Num()));
		}
		if (!Error.IsEmpty()) W->SetStatus(Error);
		W->RefreshBrowser();
	});
	if (!DirectoryRequest->ProcessRequest()) { bRefreshingDirectory=false; SetStatus(TEXT("Could not refresh the directory. You can still use saved servers.")); }
}

void UACELoginWidget::UpdateDatStatus()
{
	if (!FileStatus || !DatBox) return;
	const FString Dir=DatBox->GetText().ToString().TrimStartAndEnd();
	FString Report;
	for (const TCHAR* File:{TEXT("client_portal.dat"),TEXT("client_cell_1.dat"),TEXT("client_highres.dat")})
		Report+=FString::Printf(TEXT("%s   %s\n"),IFileManager::Get().FileSize(*(Dir/File))>0?TEXT("Found"):TEXT("Missing"),File);
	Report+=TEXT("Portal and cell files are required. High-resolution textures are used when available.");
	FileStatus->SetText(FText::FromString(Report));
}

bool UACELoginWidget::ValidateGameFiles()
{
	// Validate the active installation, not an unsaved edit in the folder field.
	FString Dir=Profile.DatDirectory;
	if (auto* GI=GetGameInstance()) if (auto* Dat=GI->GetSubsystem<UACEDatSubsystem>()) Dir=Dat->GetDatDirectory();
	TArray<FString> Missing;
	for (const TCHAR* File:{TEXT("client_portal.dat"),TEXT("client_cell_1.dat")})
		if (Dir.IsEmpty() || IFileManager::Get().FileSize(*(Dir/File))<=0) Missing.Add(File);
	if (Missing.IsEmpty())
	{
		FileProblem->SetVisibility(ESlateVisibility::Collapsed);
		return true;
	}
	FString Message=FString::Printf(TEXT("Game files are missing or empty: %s.\nSearched folder: %s\n"),
		*FString::Join(Missing,TEXT(", ")),Dir.IsEmpty()?TEXT("No folder selected"):*Dir);
#if PLATFORM_ANDROID
	Message+=UACEMobileSettings::IsEnabled()?TEXT("Install the game data with Mobile/Install-Android.ps1 from your computer, then reopen the app. Your phone and Quest installations are separate."):TEXT("Close the game, connect your Quest to your computer, and run Repair-Quest.cmd from the extracted Quest installer to install the DAT files. Then reopen the game.");
#else
	Message+=TEXT("Select the folder containing your Asheron's Call DAT files below, then click Save location and return to Play. If you have not installed the game data yet, install it first.");
#endif
	// This text uses the packaged engine font; it must work without any DAT UI assets.
	FileProblem->SetText(FText::FromString(Message));
	FileProblem->SetVisibility(ESlateVisibility::HitTestInvisible);
	DatBox->SetText(FText::FromString(Dir));
	Pages->SetActiveWidgetIndex(3);
	SetVisibility(ESlateVisibility::Visible);
	SetStatus(TEXT("Game files required. See Game files for the missing files and repair instructions."));
	return false;
}

void UACELoginWidget::SetPasswordVisible(bool bVisible)
{
	if(PasswordBox)PasswordBox->SetIsPassword(!bVisible);
	if(PasswordVisibilityButton)
	{
		PasswordVisibilityButton->SetToolTipText(FText::FromString(bVisible?TEXT("Hide password"):TEXT("Show password")));
		if(auto* Eye=Cast<UNativeWidgetHost>(PasswordVisibilityButton->GetContent()))
			if(auto Icon=Eye->GetContent())Icon->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void UACELoginWidget::RunAction(const FString& Action, const FString& Value)
{
	if(Action==TEXT("resetgraphics"))
	{
		FACEDesktopDisplay::ResetGraphics(Cast<AACEPlayerController>(GetOwningPlayer()));
		SetStatus(TEXT("Graphics reset. Windowed mode and default graphics restored."));return;
	}
	if(Action==TEXT("togglepassword")){SetPasswordVisible(PasswordBox&&PasswordBox->GetIsPassword());return;}
	// Reveal is temporary UI state, never a saved account preference.
	if(Action==TEXT("play")||Action==TEXT("plugins")||Action==TEXT("updates")||Action==TEXT("files")||Action==TEXT("browser")||Action==TEXT("launch")||Action==TEXT("newaccount"))SetPasswordVisible(false);
	if (Action==TEXT("quit"))
	{
		SaveLoginEntries();
		UKismetSystemLibrary::QuitGame(this,GetOwningPlayer(),EQuitPreference::Quit,false);
		return;
	}
	if (Action==TEXT("plugins")) { Pages->SetActiveWidgetIndex(5); return; }
	if (Action==TEXT("accounts")) { Pages->SetActiveWidgetIndex(6); return; }
	if (Action==TEXT("updates")) { Pages->SetActiveWidgetIndex(4); RefreshUpdateControls(); return; }
	if (Updater)
	{
		if (Action==TEXT("checkupdate")) { Updater->Check(); RefreshUpdateControls(); return; }
		if (Action==TEXT("downloadupdate")) { Updater->Download(); RefreshUpdateControls(); return; }
		if (Action==TEXT("cancelupdate")) { Updater->Cancel(); RefreshUpdateControls(); return; }
		if (Action==TEXT("installupdate")) { SaveLoginEntries(); Updater->Install(UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled()); RefreshUpdateControls(); return; }
		if (Action==TEXT("updatenotes")) { FPlatformProcess::LaunchURL(*(Updater->Release.Number>0?FString::Printf(TEXT("https://thwargle.com/downloads/ac/v%d/RELEASE-NOTES.md"),Updater->Release.Number):FString(TEXT("https://thwargle.com/unreal/#download"))),nullptr,nullptr); return; }
	}
	if (Action==TEXT("launch")) { DoLogin(); return; }
	if (Action==TEXT("enter")) { DoEnterSelectedCharacter(); return; }
	if (Action==TEXT("play")) { Pages->SetActiveWidgetIndex(0); SelectServer(Profile.SelectedServerId); }
	else if (Action==TEXT("selectserver")) { SelectServer(Value); SetStatus(TEXT("Server selected. Choose an account and launch.")); }
	else if (Action==TEXT("selectaccount")) { Profile.SelectedAccountId=Value; SelectServer(Profile.SelectedServerId); }
	else if (Action==TEXT("newserver") || Action==TEXT("editserver")) EditServer(Action==TEXT("newserver"));
	else if (Action==TEXT("ace") || Action==TEXT("gdle"))
	{ bEditingGDLE=Action==TEXT("gdle"); ButtonStyle(ACETypeButton,!bEditingGDLE); ButtonStyle(GDLETypeButton,bEditingGDLE); }
	else if (Action==TEXT("saveserver"))
	{
		FACELoginServer S; S.Id=EditingServerId.IsEmpty()?FGuid::NewGuid().ToString():EditingServerId;
		S.Name=NameBox->GetText().ToString().TrimStartAndEnd(); S.Host=HostBox->GetText().ToString().TrimStartAndEnd();
		const FString Port=PortBox->GetText().ToString().TrimStartAndEnd(); S.Port=Port.IsNumeric()?FCString::Atoi(*Port):0;
		S.bGDLE=bEditingGDLE; S.Description=DescriptionBox->GetText().ToString().Left(4096);
		S.Website=WebsiteBox->GetText().ToString().TrimStartAndEnd(); S.Discord=DiscordBox->GetText().ToString().TrimStartAndEnd();
		FString Error; if (!ACELoginProfile::ValidateServer(S,Error)) { SetStatus(Error); return; }
		if (Profile.Servers.ContainsByPredicate([&](const auto& E){return E.Id!=S.Id && E.Host.Equals(S.Host,ESearchCase::IgnoreCase) && E.Port==S.Port;}))
		{ SetStatus(TEXT("This address and port are already in your server list.")); return; }
		auto* Existing=Profile.Servers.FindByPredicate([&](const auto& E){return E.Id==S.Id;});
		if (Existing) *Existing=S;
		else { if (Profile.Servers.Num()>=128) { SetStatus(TEXT("Remove an unused server before adding another.")); return; } Profile.Servers.Add(S); }
		SelectServer(S.Id); Pages->SetActiveWidgetIndex(0); SetStatus(TEXT("Server saved."));
	}
	else if (Action==TEXT("saveaccount")) { if (StoreAccount()) SetStatus((PLATFORM_LINUX || PLATFORM_MAC) ? TEXT("Account saved on this device.") : TEXT("Account saved securely on this device.")); }
	else if (Action==TEXT("newaccount"))
	{
		Profile.SelectedAccountId.Reset(); Profile.Account.Reset(); Profile.Password.Reset();
		AccountAliasBox->SetText(FText::GetEmpty());
		AccountBox->SetText(FText::GetEmpty()); PasswordBox->SetText(FText::GetEmpty()); RefreshAccounts(); bLoginEntriesDirty=true;
	}
	else if (Action==TEXT("removeserver") || Action==TEXT("removeaccount"))
	{
		if ((Action==TEXT("removeserver") && !Profile.SelectedServer()) || (Action==TEXT("removeaccount") && Profile.SelectedAccountId.IsEmpty())) return;
		PendingRemoval=Action==TEXT("removeserver")?TEXT("s:")+Profile.SelectedServerId:TEXT("a:")+Profile.SelectedAccountId;
		ConfirmText->SetText(FText::FromString(Action==TEXT("removeserver")?TEXT("Remove this server from your saved list? Your accounts will remain available."):TEXT("Remove this saved account from this device?")));
		ConfirmPanel->SetVisibility(ESlateVisibility::Visible);
	}
	else if (Action==TEXT("confirmremove"))
	{
		if (PendingRemoval.StartsWith(TEXT("s:"))) Profile.RemoveServer(PendingRemoval.Mid(2));
		else if (PendingRemoval.StartsWith(TEXT("a:"))) Profile.Accounts.RemoveAll([&](const auto& A){return A.Id==PendingRemoval.Mid(2);});
		PendingRemoval.Reset(); ConfirmPanel->SetVisibility(ESlateVisibility::Collapsed); SelectServer(Profile.SelectedServerId); SetStatus(TEXT("Removed from this device."));
	}
	else if (Action==TEXT("cancelremove")) { PendingRemoval.Reset(); ConfirmPanel->SetVisibility(ESlateVisibility::Collapsed); }
	else if (Action==TEXT("browser")) { Pages->SetActiveWidgetIndex(2); RefreshBrowser(); if (Directory.IsEmpty()) FetchDirectory(); }
	else if (Action==TEXT("refresh")) FetchDirectory();
	else if (Action==TEXT("directoryselect")) { BrowserServerId=Value; RefreshBrowser(); }
	else if (Action==TEXT("import"))
	{
		const auto* S=Directory.FindByPredicate([&](const auto& E){return E.Id==BrowserServerId;});
		if (!S) { SetStatus(TEXT("Select a server in the directory first.")); return; }
		const auto* Existing=Profile.Servers.FindByPredicate([&](const auto& E){return E.Host.Equals(S->Host,ESearchCase::IgnoreCase) && E.Port==S->Port;});
		if (Existing) SelectServer(Existing->Id);
		else
		{
			if (Profile.Servers.Num()>=128) { SetStatus(TEXT("Remove an unused server before adding another.")); return; }
			FACELoginServer Copy=*S; Copy.Id=FGuid::NewGuid().ToString(); Profile.Servers.Add(Copy); SelectServer(Copy.Id);
		}
		Pages->SetActiveWidgetIndex(0); SetStatus(TEXT("Server added. Choose an account to launch."));
	}
	else if (Action==TEXT("website") || Action==TEXT("discord") || Action==TEXT("browserwebsite") || Action==TEXT("browserdiscord"))
	{
		const auto* S=Action.StartsWith(TEXT("browser"))?Directory.FindByPredicate([&](const auto& E){return E.Id==BrowserServerId;}):Profile.SelectedServer();
		if (!S) return; const FString Url=Action.EndsWith(TEXT("discord"))?S->Discord:S->Website;
		if (ACELoginProfile::IsWebLink(Url)) FPlatformProcess::LaunchURL(*Url,nullptr,nullptr);
		else SetStatus(TEXT("This server has not provided that link."));
	}
	else if (Action==TEXT("files")) { Pages->SetActiveWidgetIndex(3); UpdateDatStatus(); }
	else if (Action==TEXT("defaultdat"))
	{
#if PLATFORM_ANDROID || PLATFORM_LINUX || PLATFORM_MAC
		DatBox->SetText(FText::FromString(FPaths::ProjectSavedDir()/TEXT("DAT")));
#else
		DatBox->SetText(FText::FromString(TEXT("C:/Turbine/Asheron's Call")));
#endif
	}
	else if (Action==TEXT("savedat"))
	{
		FString Dir=DatBox->GetText().ToString().TrimStartAndEnd(); FPaths::NormalizeDirectoryName(Dir);
		if (Dir.IsEmpty() || IFileManager::Get().FileSize(*(Dir/TEXT("client_portal.dat")))<=0 || IFileManager::Get().FileSize(*(Dir/TEXT("client_cell_1.dat")))<=0)
		{ SetStatus(TEXT("Choose a folder containing client_portal.dat and client_cell_1.dat.")); return; }
		Profile.DatDirectory=Dir; bLoginEntriesDirty=true;
		bool Restart=false;
		if (auto* GI=GetGameInstance()) if (auto* Dat=GI->GetSubsystem<UACEDatSubsystem>())
		{
			Restart=(Dat->IsDatReady() || Dat->IsDatLoading()) && Dat->GetDatDirectory()!=Dir;
			if (!Restart) Dat->SetDatDirectory(Dir);
		}
		SetStatus(Restart?TEXT("Location saved. Restart the client to use these game files."):TEXT("Game-file location saved."));
		FileProblem->SetVisibility(ESlateVisibility::Collapsed);
	}
	else if (Action==TEXT("browsefolder"))
	{
#if PLATFORM_WINDOWS
		// Runtime Windows picker; no dependency on editor-only DesktopPlatform.
		const HRESULT Init=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
		IFileOpenDialog* Dialog=nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&Dialog))))
		{
			DWORD Options=0; Dialog->GetOptions(&Options); Dialog->SetOptions(Options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR);
			Dialog->SetTitle(L"Choose your Asheron's Call DAT folder");
			if (SUCCEEDED(Dialog->Show(nullptr)))
			{
				IShellItem* Item=nullptr;
				if (SUCCEEDED(Dialog->GetResult(&Item)))
				{
					PWSTR Path=nullptr; if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH,&Path))) { DatBox->SetText(FText::FromString(Path)); CoTaskMemFree(Path); } Item->Release();
				}
			}
			Dialog->Release();
		}
		if (SUCCEEDED(Init)) CoUninitialize();
#endif
	}
	SaveLoginEntries();
}
