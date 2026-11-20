#include "ACEHoverTooltipWidget.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACERetailTextEntry.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACERetailObjectNames.h"
#include "UI/ACERetailAllegianceTitle.h"
#include "ACESession.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIResourceResolver.h"
#include "Dat/ACEDatTextLayout.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEOpcodes.h"
#include "ACEPlayerController.h"
#include "VR/ACEVRComponent.h"
#include "ACETypes.h"
#include "ACEInventoryRules.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/MultiLineEditableText.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/GameInstance.h"
#include "Styling/CoreStyle.h"

namespace
{
	static const FLinearColor SocialGold(0.95f, 0.82f, 0.35f, 1.f);
	static const FLinearColor SocialWhite(0.92f, 0.92f, 0.88f, 1.f);
	static const FLinearColor SocialDim(0.65f, 0.65f, 0.6f, 1.f);
	/** DAT paint climbs past 20k — overlays must sit above tab/frame chrome. */
	constexpr int32 SocialOverlayZ = 100000;

	TArray<int32> FellowshipExperiencePercentages(const FACEFellowshipInfo& Info, UACEDatSubsystem* Dat)
	{
		TArray<int32> Shares;
		Shares.Init(Info.bShareXP ? -1 : 0, Info.Members.Num());
		if (!Info.bShareXP || Info.Members.IsEmpty()) return Shares;
		// gmFellowshipUI::UpdateFellowStats / FellowshipSystem::GetEvenSplitXPPctg.
		// This is the retail nominal split, not quest XP or a distance-adjusted award.
		if (Info.bEvenShare)
		{
			constexpr int32 RetailPercent[] = {0, 100, 75, 60, 55, 50, 45, 40, 35, 31, 28};
			if (Info.Members.Num() < UE_ARRAY_COUNT(RetailPercent)) Shares.Init(RetailPercent[Info.Members.Num()], Info.Members.Num());
			return Shares;
		}
		if (!Dat) return Shares;
		TArray<uint64> Costs;
		double Total = 0;
		for (const auto& Member : Info.Members)
		{
			uint64 Cost = 0;
			if (!Dat->TryGetLevelExperienceCost(Member.Level, Cost)) return Shares;
			Costs.Add(Cost); Total += double(Cost);
		}
		for (int32 I = 0; I < Shares.Num(); ++I) Shares[I] = FMath::FloorToInt(100.0 * double(Costs[I]) / Total + 1.e-9);
		return Shares;
	}

	FString ContractRepeatTime(double Seconds)
	{
		// Retail ClientUISystem::DeltaTimeToString uses 30-day months, omits
		// zero units and always includes seconds. Clamp before integer conversion.
		uint32 Remaining = static_cast<uint32>(FMath::Clamp(Seconds, 0.0, double(MAX_uint32)));
		FString Text;
		const uint32 Units[] = {2592000, 86400, 3600, 60};
		const TCHAR* Suffixes[] = {TEXT("mo"), TEXT("d"), TEXT("h"), TEXT("m")};
		for (int32 I = 0; I < UE_ARRAY_COUNT(Units); ++I)
		{
			if (Remaining >= Units[I]) Text += FString::Printf(TEXT("%u%s "), Remaining / Units[I], Suffixes[I]);
			Remaining %= Units[I];
		}
		return Text + FString::Printf(TEXT("%us"), Remaining);
	}

	// Retail TinkeringSystem::IsValidMaterialType excludes the material category
	// headings. ACE's salvage table likewise has no output weenie for those IDs.
	bool IsSalvageMaterial(int32 Material)
	{
		return Material >= 1 && Material <= 77 && Material != 3 && Material != 9
			&& Material != 56 && Material != 65 && Material != 72;
	}

	bool IsOwnedSalvageObject(const FACESession& Session, int32 Guid)
	{
		TSet<int32> Seen;
		while (Guid && Guid != Session.GetPlayerGuid())
		{
			if (Seen.Contains(Guid) || Session.GetTradeSelfItems().Contains(Guid)) return false;
			Seen.Add(Guid);
			const FACEWorldObject* Item = Session.GetWorldObjects().Find(Guid);
			if (!Item || Item->WielderId || Item->ParentGuid || Item->CurrentWieldedLocation) return false;
			Guid = Item->ContainerId;
		}
		return Guid != 0 && Guid == Session.GetPlayerGuid();
	}

	bool IsSalvageCandidate(const FACESession& Session, const FACEWorldObject& Item, int32 ToolGuid)
	{
		return Item.Guid != ToolGuid && Item.Guid != Session.GetPlayerGuid() && IsOwnedSalvageObject(Session, Item.Guid)
			&& IsSalvageMaterial(Item.MaterialType) && Item.Structure < 100
			&& !(Item.ObjectDescriptionFlags & ACEObjectDescFlag::Retained);
	}

	// Retail GetContainedItemsList is _itemsList, separate from _containersList.
	// A main-pack drop must not traverse every backpack carried by the player.
	bool GetSalvageChildren(const FACESession& Session, const FACEWorldObject& Item, TArray<int32>& Out)
	{
		Out.Reset();
		if (Item.Guid == Session.GetPlayerGuid())
		{
			Session.GetPackItemGuids(Item.Guid, Out);
			return !Out.IsEmpty();
		}
		bool bKnownNonempty = false;
		if (const auto* Contents = Session.GetContainerContents(Item.Guid))
		{
			// Unresolved child records still make the container nonempty. An older
			// empty ViewContents also must not hide a newly replicated child below.
			bKnownNonempty = !Contents->IsEmpty();
			for (const auto& Ref : *Contents)
			{
				const FACEWorldObject* Child = Session.GetWorldObjects().Find(Ref.ItemGuid);
				if (Child && Child->ContainerId == Item.Guid) Out.AddUnique(Ref.ItemGuid);
			}
		}
		// The main backpack is represented by the player object, whose public
		// descriptor need not contain the Container bit or capacity fields.
		if (Item.Guid != Session.GetPlayerGuid() && !ACEInventoryRules::IsContainer(Item)) return bKnownNonempty;
		for (const auto& Pair : Session.GetWorldObjects())
			if (Pair.Value.ContainerId == Item.Guid) Out.AddUnique(Pair.Key);
		Out.Sort([&](int32 A, int32 B)
		{
			const int32 PA = Session.GetWorldObjects().FindChecked(A).PlacementPosition;
			const int32 PB = Session.GetWorldObjects().FindChecked(B).PlacementPosition;
			return PA != PB ? PA < PB : A < B;
		});
		return bKnownNonempty || !Out.IsEmpty();
	}
}

void UACEUIGameplayBinder::HandleFellowshipChanged()
{
	RefreshFellowshipOverlays();
	RefreshSocialButtonLabels();
}

void UACEUIGameplayBinder::HandleAllegianceChanged()
{
	RefreshAllegianceOverlays();
}

void UACEUIGameplayBinder::HandleFriendsChanged()
{
	RefreshFriendsOverlays();
}

void UACEUIGameplayBinder::HandleContractsChanged()
{
	RefreshQuestOverlays();
}

void UACEUIGameplayBinder::HandleSquelchChanged()
{
	if (Client)
		if (auto S=Client->GetSession();S.IsValid())
			GlobalChatTypeFilter=(GlobalChatTypeFilter&0xffffffff00000000ull)|uint32(~S->GetGlobalSquelchMask());
	RefreshSquelchOverlays();
	if (bChatTargetPopupOpen) RefreshChatTargetPopup();
}

void UACEUIGameplayBinder::HandleSquelchNameCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnEnter)
	{
		return;
	}
	const FString Name = Text.ToString().TrimStartAndEnd();
	if (Name.IsEmpty() || !Client)
	{
		return;
	}
	// Retail: guid 0 + name lets the server resolve offline characters. 1 = AllChannels.
	Client->SendModifyCharacterSquelch(true, 0, Name, 1);
	if (SquelchNameEntry)
	{
		SquelchNameEntry->SetText(FText::GetEmpty());
	}
}

void UACEUIGameplayBinder::EnsureSocialEntryBoxes()
{
	if (!Canvas || !Canvas->WidgetTree)
	{
		return;
	}
	UWidgetTree* Tree = Canvas->WidgetTree;
	auto MakeEntry = [&](TObjectPtr<UACERetailTextEntry>& Box)
	{
		if (Box)
		{
			return;
		}
		Box = Tree->ConstructWidget<UACERetailTextEntry>();
		Box->SetVisibility(ESlateVisibility::Collapsed);
		Box->ContentMargins=FMargin(2.f,0.f);
		Box->TextColor=SocialWhite;
	};
	MakeEntry(FellowshipNameEntry);
	MakeEntry(FriendNameEntry);
	MakeEntry(SquelchNameEntry);
	if (FriendNameEntry && !bSocialEntriesBound)
	{
		FellowshipNameEntry->OnTextChanged.AddDynamic(this, &UACEUIGameplayBinder::HandleFellowshipNameChanged);
		FellowshipNameEntry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleFellowshipNameCommitted);
		FriendNameEntry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleFriendNameCommitted);
		if (SquelchNameEntry)
		{
			SquelchNameEntry->OnTextCommitted.AddDynamic(this, &UACEUIGameplayBinder::HandleSquelchNameCommitted);
		}
		bSocialEntriesBound = true;
	}
}

bool UACEUIGameplayBinder::ScrollFellowship(float WheelDelta,FVector2D CanvasLocalPos)
{
	const bool bFriends = ActiveSocialTab == TEXT("FriendsPage");
	const bool bSquelch = ActiveSocialTab == TEXT("SquelchPage");
	if(ActivePanelPage!=TEXT("SocialPanel_Field") || (!bFriends && !bSquelch && ActiveSocialTab!=TEXT("FellowshipPage")) || !Manager || !Canvas)return false;
	const auto List=Manager->FindElementUnder(ActiveSocialTab,bFriends?TEXT("FriendsListBox"):bSquelch?TEXT("SquelchListBox"):TEXT("FellowsListBox"));
	if(!List || !Canvas->IsElementExposedAt(List,CanvasLocalPos))return false;
	const FVector2D P=Canvas->ViewportToLayout(CanvasLocalPos);const FIntPoint O=List->GetScreenOrigin();
	if(P.X<O.X||P.Y<O.Y||P.X>=O.X+List->Width||P.Y>=O.Y+List->Height)return false;
	if (bFriends) { FriendScrollOffset+=WheelDelta>0?-1:1;RefreshFriendsOverlays(); }
	else if (bSquelch) { SquelchScrollOffset+=WheelDelta>0?-1:1;RefreshSquelchOverlays(); }
	else { FellowScrollOffset+=WheelDelta>0?-1:1;RefreshFellowshipOverlays(); }
	return true;
}

void UACEUIGameplayBinder::HandleFellowshipNameChanged(const FText&)
{
	if (Manager) if (const auto Create=Manager->FindElementUnder(TEXT("FellowshipPage"),TEXT("CreateFellowshipButton")))
	{Create->bActivatable=CanActivateFellowshipControl(Create->ElementName);Create->bGhosted=!Create->bActivatable;}
}
void UACEUIGameplayBinder::HandleFellowshipNameCommitted(const FText&, ETextCommit::Type Method)
{
	if(Method==ETextCommit::OnEnter)HandleNamedClick(TEXT("CreateFellowshipButton"));
}
bool UACEUIGameplayBinder::CanActivateFellowshipControl(const FString& Name) const
{
	if(!Client)return false;
	const auto Info=Client->GetFellowship();const int32 Self=Client->GetPlayerGuid();
	if(Name==TEXT("CreateFellowshipButton"))return !Info.bValid && FellowshipNameEntry && !FellowshipNameEntry->GetText().ToString().TrimStartAndEnd().IsEmpty();
	if(!Info.bValid)return false;
	const bool Leader=Info.LeaderGuid==Self;
	if(Name==TEXT("FellowQuitButton"))return true;
	if(Name==TEXT("FellowOpenButton")||Name==TEXT("FellowDisbandButton"))return Leader;
	if(Name==TEXT("FellowLeaderButton")||Name==TEXT("FellowDismissButton"))
		return Leader && SelectedFellowGuid!=Self && Info.Members.ContainsByPredicate([&](const auto& M){return M.Guid==SelectedFellowGuid;});
	if(Name==TEXT("FellowRecruitButton"))
	{
		FACEWorldObject Target;
		return (Leader||Info.bOpen) && Info.Members.Num()<9 && LastSelection.bValid && LastSelection.Guid!=Self
			&& Client->GetWorldObject(LastSelection.Guid,Target) && Target.bIsPlayer
			&& !Info.Members.ContainsByPredicate([&](const auto& M){return M.Guid==Target.Guid;});
	}
	return false;
}

bool UACEUIGameplayBinder::CanActivateAllegianceControl(const FString& Name, int32 TargetGuid) const
{
	if (!Client) return false;
	const auto Info=Client->GetAllegiance();
	if (Name==TEXT("BreakButton")) return Info.PatronGuid!=0 && (!TargetGuid || TargetGuid==Info.PatronGuid);
	if (Name==TEXT("KickButton"))
		return Info.Vassals.ContainsByPredicate([&](const auto& V){return V.Guid==(TargetGuid ? TargetGuid : SelectedVassalGuid);});
	if (Name!=TEXT("SwearButton") || Info.PatronGuid) return false;
	const int32 Target=TargetGuid ? TargetGuid : LastSelection.bValid ? LastSelection.Guid : 0;
	FACEWorldObject Player;
	return Target && Target!=Client->GetPlayerGuid() && Target!=Info.MonarchGuid
		&& Client->GetWorldObject(Target,Player) && Player.bIsPlayer
		&& !Info.Vassals.ContainsByPredicate([&](const auto& V){return V.Guid==Target;});
}

void UACEUIGameplayBinder::ShowAllegianceConfirmation(const FString& Action)
{
	if (!CanActivateAllegianceControl(Action)) return;
	const auto Info=Client->GetAllegiance();
	PendingAllegianceAction=Action;
	if (Action==TEXT("SwearButton"))
	{
		PendingAllegianceGuid=LastSelection.Guid;
		FACEWorldObject Player; Client->GetWorldObject(PendingAllegianceGuid,Player);
		PendingAllegiancePrompt=FString::Printf(TEXT("Are you sure you want to swear allegiance to %s?"),*Player.Name);
	}
	else if (Action==TEXT("BreakButton"))
	{
		PendingAllegianceGuid=Info.PatronGuid;
		PendingAllegiancePrompt=FString::Printf(TEXT("Are you sure you want to break your allegiance to %s?"),*Info.PatronName);
	}
	else
	{
		PendingAllegianceGuid=SelectedVassalGuid;
		const auto* V=Info.Vassals.FindByPredicate([&](const auto& Member){return Member.Guid==PendingAllegianceGuid;});
		PendingAllegiancePrompt=FString::Printf(TEXT("Are you sure you want to break %s's allegiance to you?"),*V->Name);
	}
	RefreshServerConfirmation();
}

bool UACEUIGameplayBinder::ScrollAllegiance(float WheelDelta,FVector2D CanvasLocalPos)
{
	if (ActivePanelPage!=TEXT("SocialPanel_Field") || ActiveSocialTab!=TEXT("AllegiancePage") || !Manager || !Canvas) return false;
	const auto List=Manager->FindElementUnder(TEXT("AllegiancePage"),TEXT("VassalsListBox"));
	if (!Canvas->IsElementExposedAt(List,CanvasLocalPos)) return false;
	VassalScrollOffset+=WheelDelta>0 ? -1 : 1;
	RefreshAllegianceOverlays(); return true;
}

void UACEUIGameplayBinder::HandleFriendNameCommitted(const FText& Text, ETextCommit::Type CommitMethod)
{
	if (CommitMethod != ETextCommit::OnEnter && CommitMethod != ETextCommit::OnUserMovedFocus)
	{
		return;
	}
	const FString Name = Text.ToString().TrimStartAndEnd();
	if (Name.IsEmpty() || !Client)
	{
		return;
	}
	Client->SendAddFriend(Name);
	if (FriendNameEntry)
	{
		FriendNameEntry->SetText(FText::GetEmpty());
	}
}


void UACEUIGameplayBinder::SyncSocialPanelTab(const FString& PageName)
{
	if (!Manager)
	{
		return;
	}
	if (Client && ActiveSocialTab!=PageName)
	{
		if (ActiveSocialTab==TEXT("AllegiancePage")) Client->SendAllegianceUpdateRequest(false);
		if (ActiveSocialTab==TEXT("FellowshipPage")) Client->SendFellowshipUpdateRequest(false);
	}
	ActiveSocialTab = PageName;
	static const TCHAR* Pages[] = {
		TEXT("AllegiancePage"), TEXT("FellowshipPage"), TEXT("FriendsPage"), TEXT("SquelchPage")
	};
	for (const TCHAR* Page : Pages)
	{
		Manager->SetElementVisibleByName(Page, PageName == Page);
	}
	ApplyPanelTabChrome(TEXT("AllegianceTab"), PageName == TEXT("AllegiancePage"));
	ApplyPanelTabChrome(TEXT("FellowshipTab"), PageName == TEXT("FellowshipPage"));
	ApplyPanelTabChrome(TEXT("FriendsTab"), PageName == TEXT("FriendsPage"));
	ApplyPanelTabChrome(TEXT("SquelchTab"), PageName == TEXT("SquelchPage"));

	if (Client)
	{
		if (PageName == TEXT("AllegiancePage"))
		{
			Client->SendAllegianceUpdateRequest(true);
		}
		else if (PageName == TEXT("FellowshipPage"))
		{
			Client->SendFellowshipUpdateRequest(true);
		}
	}
	RefreshSocialOverlays();
}

void UACEUIGameplayBinder::RefreshSocialOverlays()
{
	if (ActivePanelPage != TEXT("SocialPanel_Field"))
	{
		for (UTextBlock* T : SocialTabLabels)
		{
			if (T) { T->SetVisibility(ESlateVisibility::Collapsed); }
		}
		RefreshAllegianceOverlays();
		RefreshFellowshipOverlays();
		RefreshFriendsOverlays();
		RefreshSquelchOverlays();
		RefreshSocialButtonLabels();
		return;
	}
	EnsureSocialEntryBoxes();
	static const TPair<const TCHAR*, const TCHAR*> Tabs[] = {
		{ TEXT("AllegianceTab"), TEXT("Allegiance") },
		{ TEXT("FellowshipTab"), TEXT("Fellowship") },
		{ TEXT("FriendsTab"), TEXT("Friends") },
		{ TEXT("SquelchTab"), TEXT("Squelch") },
	};
	for (int32 i = 0; i < UE_ARRAY_COUNT(Tabs); ++i)
	{
		UTextBlock* Label = nullptr;
		if (SocialTabLabels.IsValidIndex(i))
		{
			Label = SocialTabLabels[i];
		}
		if (!Label && Canvas && Canvas->WidgetTree)
		{
			Label = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
			Label->SetJustification(ETextJustify::Center);
			if (SocialTabLabels.Num() <= i)
			{
				SocialTabLabels.SetNum(i + 1);
			}
			SocialTabLabels[i] = Label;
		}
		if (!Label)
		{
			continue;
		}
		TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(
			TEXT("SocialPanel_Field"), Tabs[i].Key);
		if (!El.IsValid())
		{
			Label->SetVisibility(ESlateVisibility::Collapsed);
			continue;
		}
		const bool bActive = (i == 0 && ActiveSocialTab == TEXT("AllegiancePage"))
			|| (i == 1 && ActiveSocialTab == TEXT("FellowshipPage"))
			|| (i == 2 && ActiveSocialTab == TEXT("FriendsPage"))
			|| (i == 3 && ActiveSocialTab == TEXT("SquelchPage"));
		Label->SetText(FText::FromString(Tabs[i].Value));
		Label->SetColorAndOpacity(FSlateColor(bActive ? SocialGold : SocialWhite));
		Label->SetFont(FCoreStyle::GetDefaultFontStyle(bActive ? TEXT("Bold") : TEXT("Regular"), 9));
		Label->SetVisibility(ESlateVisibility::HitTestInvisible);
		// Inset like SkillManagement tabs so captions sit in the tab face, not over chrome edges.
		Canvas->PlaceWidgetAtElement(Label, El, SocialOverlayZ, FMargin(10.f, 4.f, 8.f, 4.f));
	}
	RefreshAllegianceOverlays();
	RefreshFellowshipOverlays();
	RefreshFriendsOverlays();
	RefreshSquelchOverlays();
	RefreshSocialButtonLabels();
}

void UACEUIGameplayBinder::RefreshAllegianceOverlays()
{
	auto Hide = [](UTextBlock* T) { if (T) T->SetVisibility(ESlateVisibility::Collapsed); };
	if (ActivePanelPage != TEXT("SocialPanel_Field") || ActiveSocialTab != TEXT("AllegiancePage")
		|| !Client || !Manager || !Canvas)
	{
		Hide(AllegianceNameLabel); Hide(MonarchNameLabel); Hide(PatronNameLabel);
		Hide(AllegianceXPLabel); Hide(AllegiancePatronXPLabel);
		for (UTextBlock* L : AllegianceCaptionLabels) Hide(L);
		for (UTextBlock* L : VassalRows) Hide(L);
		for (UTextBlock* L : VassalXPRows) Hide(L);
		for (UTextBlock* L : VassalStatusRows) Hide(L);
		for (const auto& Row : VassalRowElements) if (Row) Row->bVisible = false;
		return;
	}
	EnsureOverlays();
	if (!bLoadedSocialStrings && Canvas->GetResourceResolver())
		if (const auto* Dat = Canvas->GetResourceResolver()->GetDatSubsystem())
			bLoadedSocialStrings = SocialStrings.LoadStrings(Dat->GetDatDirectory(), 0x23000001);
	const FACEAllegianceInfo& Info = Client->GetAllegiance();
	if (!Info.Vassals.ContainsByPredicate([&](const auto& V) { return V.Guid == SelectedVassalGuid; })) SelectedVassalGuid = 0;
	for (const TCHAR* Name : {TEXT("SwearButton"), TEXT("BreakButton"), TEXT("KickButton")})
		if (auto Button = Manager->FindElementUnder(TEXT("AllegiancePage"), Name))
		{ Button->bActivatable = CanActivateAllegianceControl(Name); Button->bGhosted = !Button->bActivatable; }

	// gmAllegianceUI shows a combined Patron/Monarch field when directly sworn to
	// the monarch, and no upstream fields for the monarch themself.
	const bool bMonarch = Info.MonarchGuid != 0 && Info.MonarchGuid != Client->GetPlayerGuid();
	const bool bPatron = Info.PatronGuid != 0 && Info.PatronGuid != Info.MonarchGuid;
	const bool bDirectMonarch = bMonarch && Info.PatronGuid == Info.MonarchGuid;
	for (const auto& Field : {TPair<const TCHAR*, bool>(TEXT("MonarchField"), bMonarch),
		TPair<const TCHAR*, bool>(TEXT("PatronField"), bPatron)})
		if (auto El = Manager->FindElementUnder(TEXT("AllegiancePage"), Field.Key))
		{
			El->bVisible = Field.Value;
			El->bUseExplicitState = true;
			El->DefaultState = (FString(Field.Key) == TEXT("MonarchField") ? Info.Monarch.bOnline : Info.Patron.bOnline) ? 1 : 13;
		}
	if (auto XP = Manager->FindElementUnder(TEXT("MonarchField"), TEXT("XPProducedFrame"))) XP->bVisible = bDirectMonarch;

	auto Place = [&](TObjectPtr<UTextBlock>& Label, const TSharedPtr<FACEUIElement>& El, const FString& Text)
	{
		if (!Label && Canvas->WidgetTree) Label = Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>();
		PlaceTextOnElement(Label, El, Text, 9, SocialWhite, SocialOverlayZ + 10);
	};
	auto FullName = [](const FACEAllegianceMember& Member, const FString& Name)
	{
		const FString Title = ACERetailAllegiance::Title(Member.Rank, Member.HeritageGroup, Member.Gender);
		return Title.IsEmpty() ? Name : Title + TEXT(" ") + Name;
	};
	auto XPText = [&](int32 Value)
	{
		return SocialStrings.FormatText(TEXT("ID_Allegiance_VassalExperiencePassedUp"),
			{{TEXT("VALUE"), FText::AsNumber(Value).ToString()}});
	};
	Place(AllegianceNameLabel, Manager->FindElementUnder(TEXT("AllegiancePage"), TEXT("PlayerName")),
		SocialStrings.FormatText(TEXT("ID_Allegiance_CharacterName"), {{TEXT("NAME"), Info.AllegianceName}}));
	Place(MonarchNameLabel, Manager->FindElementUnder(TEXT("AllegiancePage"), TEXT("MonarchName")), FullName(Info.Monarch, Info.MonarchName));
	Place(PatronNameLabel, Manager->FindElementUnder(TEXT("AllegiancePage"), TEXT("PatronName")), FullName(Info.Patron, Info.PatronName));
	Place(AllegianceXPLabel, Manager->FindElementUnder(TEXT("MonarchField"), TEXT("XPProduced")), XPText(Info.SelfCPTithed));
	Place(AllegiancePatronXPLabel, Manager->FindElementUnder(TEXT("PatronField"), TEXT("XPProduced")), XPText(Info.SelfCPTithed));

	const auto& Vitals = Client->GetPlayerVitals();
	const int32* QualityRank = Vitals.StatQualityInts.Find(30);
	const int32 DisplayRank = Vitals.EffectiveAllegianceRank >= 0 ? Vitals.EffectiveAllegianceRank
		: QualityRank && *QualityRank >= 0 ? *QualityRank : Info.Rank;
	const bool bBuffed = DisplayRank != Info.Rank;
	const FString Title = ACERetailAllegiance::Title(Info.Rank,
		Info.Self.HeritageGroup ? Info.Self.HeritageGroup : Vitals.HeritageGroup,
		Info.Self.Gender ? Info.Self.Gender : Vitals.Gender);
	if (auto El = Manager->FindElementUnder(TEXT("AllegiancePage"), TEXT("PlayerRank")))
	{ El->bUseExplicitState = true; El->DefaultState = bBuffed ? 0x10000014 : 1; }
	const TPair<const TCHAR*, FString> Captions[] = {
		{TEXT("PlayerRank"), SocialStrings.FormatText(bBuffed ? TEXT("ID_Allegiance_RankBuffed") : TEXT("ID_Allegiance_Rank"),
			{{TEXT("TITLE"), Title}, {TEXT("RANK"), FString::FromInt(DisplayRank)}, {TEXT("RANKBUFF"), FString::FromInt(DisplayRank - Info.Rank)}}).Replace(TEXT("\\["), TEXT("[")).Replace(TEXT("\\]"), TEXT("]"))},
		{TEXT("PlayerFollowers"), SocialStrings.FormatText(TEXT("ID_Allegiance_Followers"), {{TEXT("FOLLOWERS"), FText::AsNumber(Info.TotalVassals).ToString()}})},
		{TEXT("MonarchFollowers"), SocialStrings.FormatText(TEXT("ID_Allegiance_Followers"), {{TEXT("FOLLOWERS"), FText::AsNumber(FMath::Max(0, Info.TotalMembers - 1)).ToString()}})},
		{TEXT("VassalsListBoxLabel"), SocialStrings.Text(TEXT("ID_Allegiance_VassalsLabel"))},
		{TEXT("VassalsXPProducedLabel"), SocialStrings.Strings.FindRef(0x0B1AF37C)}
	};
	while (AllegianceCaptionLabels.Num() < UE_ARRAY_COUNT(Captions) + 2) AllegianceCaptionLabels.Add(nullptr);
	for (int32 I = 0; I < UE_ARRAY_COUNT(Captions); ++I)
	{
		const auto El = Manager->FindElementUnder(TEXT("AllegiancePage"), Captions[I].Key);
		const FString Text = El && El->TextEntryId ? SocialStrings.Strings.FindRef(El->TextEntryId) : Captions[I].Value;
		Place(AllegianceCaptionLabels[I], El, Text);
	}

	for (int32 I = 0; I < 2; ++I)
	{
		const auto El = Manager->FindElementUnder(I == 0 ? TEXT("MonarchField") : TEXT("PatronField"), TEXT("XPProducedLabel"));
		Place(AllegianceCaptionLabels[UE_ARRAY_COUNT(Captions) + I], El, El ? SocialStrings.Strings.FindRef(El->TextEntryId) : FString());
	}

	const auto List = Manager->FindElementUnder(TEXT("AllegiancePage"), TEXT("VassalsListBox"));
	// Retail's template is two lines: titled name, then offline status and XP.
	constexpr int32 RowHeight = 32;
	VassalVisibleRows = List ? FMath::Max(1, List->Height / RowHeight) : 1;
	const int32 MaxOffset = FMath::Max(0, Info.Vassals.Num() - VassalVisibleRows);
	VassalScrollOffset = FMath::Clamp(VassalScrollOffset, 0, MaxOffset);
	SyncDatScrollbar(Manager->FindElementUnder(TEXT("AllegiancePage"), TEXT("VassalsListBoxScrollbar")),
		MaxOffset ? float(VassalScrollOffset) / MaxOffset : 0.f,
		Info.Vassals.IsEmpty() ? 1.f : FMath::Min(1.f, float(VassalVisibleRows) / Info.Vassals.Num()));
	const int32 Count = FMath::Min(VassalVisibleRows, Info.Vassals.Num());
	while (VassalRows.Num() < Count && Canvas->WidgetTree)
	{
		VassalRows.Add(nullptr); VassalXPRows.Add(nullptr); VassalStatusRows.Add(nullptr);
		VassalRowElements.Add(UACEUILayoutResolver::LoadTemplate(0x2100002F, 0x10000266));
	}
	VassalRowGuids.SetNumZeroed(VassalRows.Num());
	for (int32 I = 0; I < VassalRows.Num(); ++I)
	{
		const auto Entry = VassalRowElements[I];
		if (Entry && List && Entry->Parent.Pin() != List) List->AddChild(Entry);
		if (Entry) Entry->bVisible = List && I < Count;
		if (!Entry || !Entry->bVisible)
		{ Hide(VassalRows[I]); Hide(VassalXPRows[I]); Hide(VassalStatusRows[I]); VassalRowGuids[I] = 0; continue; }
		const auto& V = Info.Vassals[I + VassalScrollOffset];
		VassalRowGuids[I] = V.Guid;
		Entry->Y = I * RowHeight; Entry->Width = List->Width;
		Entry->bUseExplicitState = true; Entry->DefaultState = V.Guid == SelectedVassalGuid ? 6 : 1;
		for (const auto& Child : Entry->Children)
		{
			if (Child->ElementId == 0x10000267)
			{
				Child->Width = Entry->Width;
				for (const auto& Name : Child->Children)
				{ Name->Width = Child->Width; Place(VassalRows[I], Name, FullName(V, V.Name)); }
			}
			else if (Child->ElementId == 0x10000269)
			{ Child->Width = Entry->Width - Child->X; Place(VassalXPRows[I], Child, XPText(V.CPTithed)); }
			else if (Child->ElementId == 0x100004AA)
			{ Child->bVisible = !V.bOnline; Place(VassalStatusRows[I], Child, SocialStrings.Strings.FindRef(Child->TextEntryId)); }
		}
	}
}

void UACEUIGameplayBinder::RefreshFellowshipOverlays()
{
	if (ActivePanelPage != TEXT("SocialPanel_Field") || ActiveSocialTab != TEXT("FellowshipPage")
		|| !Client || !Manager || !Canvas)
	{
		for (UTextBlock* R : FellowRows) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* R : FellowDetailLabels) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		if (FellowshipNameEntry) { FellowshipNameEntry->SetVisibility(ESlateVisibility::Collapsed); }
		if (FellowshipTitleLabel) { FellowshipTitleLabel->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	EnsureOverlays();
	EnsureSocialEntryBoxes();
	const FACEFellowshipInfo Info = Client->GetFellowship();
	const auto XpShares = FellowshipExperiencePercentages(Info, Client->GetGameInstance()->GetSubsystem<UACEDatSubsystem>());
	if(!Info.Members.ContainsByPredicate([&](const auto& M){return M.Guid==SelectedFellowGuid;}))SelectedFellowGuid=0;
	for(const TCHAR* Name:{TEXT("CreateFellowshipButton"),TEXT("FellowQuitButton"),TEXT("FellowOpenButton"),TEXT("FellowDisbandButton"),TEXT("FellowLeaderButton"),TEXT("FellowDismissButton"),TEXT("FellowRecruitButton")})
		if(const auto Button=Manager->FindElementUnder(TEXT("FellowshipPage"),Name))
		{Button->bActivatable=CanActivateFellowshipControl(Name);Button->bGhosted=!Button->bActivatable;}

	Manager->SetElementVisibleByName(TEXT("NotInAFellowshipFrame"), !Info.bValid);
	Manager->SetElementVisibleByName(TEXT("FellowshipFrame"), Info.bValid);

	auto PlaceSocialEntry = [&](UACERetailTextEntry* Box, const FString& ElementName)
	{
		if (!Box)
		{
			return;
		}
		TSharedPtr<FACEUIElement> EntryEl = Manager->FindElementUnder(
			TEXT("FellowshipPage"), ElementName);
		if (!EntryEl.IsValid())
		{
			EntryEl = Manager->FindElementByName(ElementName);
		}
		if (!EntryEl.IsValid())
		{
			Box->SetVisibility(ESlateVisibility::Collapsed);
			return;
		}
		Box->SetRetailElement(Canvas->GetResourceResolver(),EntryEl);
		Box->SetVisibility(ESlateVisibility::Visible);
		Canvas->PlaceWidgetAtElement(Box, EntryEl, SocialOverlayZ + 5, FMargin(3.f, 1.f));
	};

	if (!Info.bValid)
	{
		FellowScrollOffset=0;
		for(auto Entry:FellowRowElements)if(Entry)Entry->bVisible=false;
		for(UTextBlock* Label:FellowDetailLabels)if(Label)Label->SetVisibility(ESlateVisibility::Collapsed);
		PlaceSocialEntry(FellowshipNameEntry, TEXT("FellowshipNameEntryBox"));
		if (!FellowshipTitleLabel && Canvas->WidgetTree)
		{
			FellowshipTitleLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		}
		if (FellowshipTitleLabel)
		{
			PlaceTextOnElement(FellowshipTitleLabel,
				Manager->FindElementUnder(TEXT("FellowshipPage"), TEXT("FellowshipNameLabel")),
				TEXT("Fellowship Name:"), 9, SocialGold, SocialOverlayZ + 4);
		}
		for (UTextBlock* R : FellowRows) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}

	if (FellowshipNameEntry)
	{
		FellowshipNameEntry->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (!FellowshipTitleLabel && Canvas->WidgetTree)
	{
		FellowshipTitleLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
	}
	if (FellowshipTitleLabel)
	{
		const FString Title = FString::Printf(TEXT("%s%s%s"),
			*Info.Name,
			Info.bOpen ? TEXT("  [Open]") : TEXT("  [Closed]"),
			Info.bShareXP ? TEXT("  XP") : TEXT(""));
		TSharedPtr<FACEUIElement> TitleEl = Manager->FindElementUnder(
			TEXT("FellowshipPage"), TEXT("FellowshipName"));
		PlaceTextOnElement(FellowshipTitleLabel, TitleEl, Title, 10, SocialGold, SocialOverlayZ + 4);
	}

	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("FellowshipPage"), TEXT("FellowsListBox"));
	constexpr int32 RowH = 32; // classic_fellowship: name/level, then H/S/M meters
	// Retail gmFellowshipUI::Update lists every server-supplied fellow. Nine is
	// a server recruitment rule, not a limit on the roster the UI can display.
	FellowVisibleRows=ListEl ? FMath::Max(1,ListEl->Height/RowH) : 1;
	const int32 MaxRows=FMath::Min(FellowVisibleRows,Info.Members.Num());
	while (FellowRows.Num() < MaxRows && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		FellowRows.Add(Row);
		FellowRowElements.Add(UACEUILayoutResolver::LoadTemplate(0x21000030,0x10000281));
		for(int32 J=0;J<4;++J) FellowDetailLabels.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	}
	FellowRowGuids.SetNum(FellowRows.Num());
	const int32 MaxOffset=FMath::Max(0,Info.Members.Num()-FellowVisibleRows);
	FellowScrollOffset=FMath::Clamp(FellowScrollOffset,0,MaxOffset);
	SyncDatScrollbar(Manager->FindElementUnder(TEXT("FellowshipPage"),TEXT("FellowsListBoxScrollbar")),MaxOffset?float(FellowScrollOffset)/MaxOffset:0, Info.Members.IsEmpty()?1.f:FMath::Min(1.f,float(FellowVisibleRows)/Info.Members.Num()));
	for (int32 i = 0; i < FellowRows.Num(); ++i)
	{
		UTextBlock* Row = FellowRows[i];
		const auto Entry=FellowRowElements[i];
		const int32 Index=i+FellowScrollOffset;
		if (!Row || !Entry) continue;
		if(ListEl && Entry->Parent.Pin()!=ListEl)ListEl->AddChild(Entry);
		Entry->bVisible=ListEl && Info.Members.IsValidIndex(Index) && i<FellowVisibleRows;
		if (!Entry->bVisible)
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			for(int32 J=0;J<4;++J)FellowDetailLabels[i*4+J]->SetVisibility(ESlateVisibility::Collapsed);
			FellowRowGuids[i] = 0;
			continue;
		}
		const FACEFellowshipMember& M = Info.Members[Index];
		FellowRowGuids[i] = M.Guid;
		const bool bSel = (M.Guid == SelectedFellowGuid);
		Entry->Y=i*RowH;Entry->Width=ListEl->Width;Entry->DefaultState=bSel?6:1;
		TFunction<TSharedPtr<FACEUIElement>(TSharedPtr<FACEUIElement>,const TCHAR*)> Find;
		Find=[&](TSharedPtr<FACEUIElement> N,const TCHAR* Name)->TSharedPtr<FACEUIElement>
		{if(N->ElementName==Name)return N;for(const auto& C:N->Children)if(auto Found=Find(C,Name))return Found;return nullptr;};
		auto NameEl=Find(Entry,TEXT("FellowName"));auto StatsEl=Find(Entry,TEXT("FellowStats"));
		const int32 StatsWidth=FMath::Min(128,ListEl->Width);
		if(NameEl){NameEl->Width=FMath::Max(1,ListEl->Width-StatsWidth-2);NameEl->bTextOneLine=true;}
		if(StatsEl){StatsEl->X=FMath::Max(1,ListEl->Width-StatsWidth);StatsEl->Width=StatsWidth;StatsEl->bTextOneLine=true;}
		PlaceTextOnElement(Row,NameEl,M.Name,10,bSel?SocialGold:SocialWhite,SocialOverlayZ+10);
		Row->SetClipping(EWidgetClipping::ClipToBounds);
		Row->SetToolTipText(FText::FromString(M.Name));
		const FString Share=XpShares[Index]>=0?FString::Printf(TEXT("%d%%"),XpShares[Index]):TEXT("--");
		PlaceTextOnElement(FellowDetailLabels[i*4],StatsEl,FString::Printf(TEXT("Lv %d  XP %s"),M.Level,*Share),10,SocialWhite,SocialOverlayZ+10);
		FellowDetailLabels[i*4]->SetAutoWrapText(false);
		FellowDetailLabels[i*4]->SetToolTipText(FText::FromString(TEXT("Nominal retail XP share. Server bonuses, distance and quest rewards can change the actual award.")));
		const TCHAR* Meters[]={TEXT("FellowHealth"),TEXT("FellowStamina"),TEXT("FellowMana")};
		const TCHAR* Labels[]={TEXT("FellowHealthStats"),TEXT("FellowStaminaStats"),TEXT("FellowManaStats")};
		const int32 Cur[]={M.HealthCur,M.StaminaCur,M.ManaCur},Max[]={M.HealthMax,M.StaminaMax,M.ManaMax};
		for(int32 J=0;J<3;++J)
		{
			auto Meter=Find(Entry,Meters[J]);auto Label=Find(Entry,Labels[J]);
			if(Meter){Meter->X=ListEl->Width*J/3;Meter->Width=ListEl->Width*(J+1)/3-Meter->X;Meter->MeterFillFraction=Max[J]>0?FMath::Clamp(float(Cur[J])/Max[J],0.f,1.f):0.f;for(auto C:Meter->Children)C->Width=Meter->Width;}
			PlaceTextOnElement(FellowDetailLabels[i*4+J+1],Label,FString::Printf(TEXT("%d/%d"),Cur[J],Max[J]),9,SocialWhite,SocialOverlayZ+10);
		}
	}
}

void UACEUIGameplayBinder::RefreshFriendsOverlays()
{
	if (ActivePanelPage != TEXT("SocialPanel_Field") || ActiveSocialTab != TEXT("FriendsPage")
		|| !Client || !Manager || !Canvas)
	{
		for (UTextBlock* R : FriendRows) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* R : FriendStatusRows) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		for (const auto& R : FriendRowElements) { if (R) R->bVisible = false; }
		if (FriendNameEntry) { FriendNameEntry->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	EnsureOverlays();
	EnsureSocialEntryBoxes();
	if (FriendNameEntry)
	{
		TSharedPtr<FACEUIElement> EntryEl = Manager->FindElementUnder(
			TEXT("FriendsPage"), TEXT("FriendNameEntryBox"));
		if (!EntryEl.IsValid())
		{
			EntryEl = Manager->FindElementByName(TEXT("FriendNameEntryBox"));
		}
		if (EntryEl.IsValid())
		{
			FriendNameEntry->SetVisibility(ESlateVisibility::Visible);
			FriendNameEntry->SetRetailElement(Canvas->GetResourceResolver(),EntryEl);
			Canvas->PlaceWidgetAtElement(FriendNameEntry, EntryEl, SocialOverlayZ + 5, FMargin(3.f, 1.f));
		}
	}
	TArray<FACEFriendInfo> List = Client->GetFriends();
	// gmFriendsUI::FindSortedInsertPosition: online first, then name.
	List.Sort([](const FACEFriendInfo& A, const FACEFriendInfo& B)
	{ return A.bOnline != B.bOnline ? A.bOnline : A.Name < B.Name; });
	const auto* Selected=List.FindByPredicate([&](const auto& F){return F.Guid==SelectedFriendGuid;});
	if (!Selected) SelectedFriendGuid=0;
	for (const TCHAR* Name:{TEXT("TellButton"),TEXT("RemoveButton"),TEXT("AddButton")})
		if (auto Button=Manager->FindElementUnder(TEXT("FriendsPage"),Name))
		{
			Button->bActivatable=FString(Name)==TEXT("AddButton") ? List.Num()<50
				: Selected && (FString(Name)!=TEXT("TellButton") || Selected->bOnline);
			Button->bGhosted=!Button->bActivatable;
		}
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("FriendsPage"), TEXT("FriendsListBox"));
	constexpr int32 RowH = 24;
	FriendVisibleRows = ListEl ? FMath::Max(1, ListEl->Height / RowH) : 1;
	const int32 MaxRows = FMath::Min(List.Num(), FriendVisibleRows);
	const int32 MaxOffset = FMath::Max(0, List.Num() - FriendVisibleRows);
	FriendScrollOffset = FMath::Clamp(FriendScrollOffset, 0, MaxOffset);
	SyncDatScrollbar(Manager->FindElementUnder(TEXT("FriendsPage"), TEXT("FreindsListBoxScrollbar")),
		MaxOffset ? float(FriendScrollOffset) / MaxOffset : 0.f,
		List.IsEmpty() ? 1.f : FMath::Min(1.f, float(FriendVisibleRows) / List.Num()));
	while (FriendRows.Num() < MaxRows && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		FriendRows.Add(Row);
		FriendStatusRows.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
		FriendRowElements.Add(UACEUILayoutResolver::LoadTemplate(0x2100005D, 0x10000519));
	}
	FriendRowGuids.SetNum(FriendRows.Num());
	for (int32 i = 0; i < FriendRows.Num(); ++i)
	{
		UTextBlock* Row = FriendRows[i];
		if (!Row)
		{
			continue;
		}
		const auto Entry = FriendRowElements[i];
		const int32 Index = i + FriendScrollOffset;
		if (ListEl && Entry && Entry->Parent.Pin() != ListEl) ListEl->AddChild(Entry);
		if (Entry) Entry->bVisible = ListEl && i < FriendVisibleRows && List.IsValidIndex(Index);
		if (!Entry || !Entry->bVisible)
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			FriendStatusRows[i]->SetVisibility(ESlateVisibility::Collapsed);
			FriendRowGuids[i] = 0;
			continue;
		}
		const FACEFriendInfo& F = List[Index];
		FriendRowGuids[i] = F.Guid;
		const bool bSel = (F.Guid == SelectedFriendGuid);
		Entry->Y = i * RowH; Entry->Width = ListEl->Width;
		Entry->bUseExplicitState = true; Entry->DefaultState = bSel ? 6 : 1;
		const auto NameEl = Entry->Children.IsEmpty() ? nullptr : Entry->Children[0];
		if (!NameEl) continue;
		NameEl->Width = Entry->Width;
		NameEl->bUseExplicitState = true; NameEl->DefaultState = F.bOnline ? 0x10000054 : 0x10000055;
		for (const auto& Status : NameEl->Children)
		{
			Status->Width = Entry->Width;
			Status->bUseExplicitState = true; Status->DefaultState = NameEl->DefaultState;
			FriendStatusRows[i]->SetText(FText::FromString(F.bOnline ? TEXT("Online") : TEXT("Offline")));
			FriendStatusRows[i]->SetVisibility(ESlateVisibility::HitTestInvisible);
			FriendStatusRows[i]->SetJustification(ETextJustify::Right);
			Canvas->PlaceWidgetAtElement(FriendStatusRows[i], Status, SocialOverlayZ + 11);
		}
		Row->SetText(FText::FromString(F.Name));
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		Row->SetClipping(EWidgetClipping::ClipToBounds);
		Canvas->PlaceWidgetAtElement(Row, NameEl, SocialOverlayZ + 10, FMargin(0, 0, 70, 0));
	}
}

void UACEUIGameplayBinder::RefreshSquelchOverlays()
{
	if (ActivePanelPage != TEXT("SocialPanel_Field") || ActiveSocialTab != TEXT("SquelchPage")
		|| !Client || !Manager || !Canvas)
	{
		for (UTextBlock* R : SquelchRows) { if (R) { R->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UTextBlock* R : SquelchStatusRows) if (R) R->SetVisibility(ESlateVisibility::Collapsed);
		for (const auto& R : SquelchRowElements) if (R) R->bVisible=false;
		if (SquelchNameEntry) { SquelchNameEntry->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	EnsureOverlays();
	EnsureSocialEntryBoxes();
	if (SquelchNameEntry)
	{
		TSharedPtr<FACEUIElement> EntryEl = Manager->FindElementUnder(
			TEXT("SquelchPage"), TEXT("SquelchNameEntryBox"));
		if (!EntryEl.IsValid())
		{
			EntryEl = Manager->FindElementByName(TEXT("SquelchNameEntryBox"));
		}
		if (EntryEl.IsValid())
		{
			SquelchNameEntry->SetVisibility(ESlateVisibility::Visible);
			SquelchNameEntry->SetRetailElement(Canvas->GetResourceResolver(),EntryEl);
			Canvas->PlaceWidgetAtElement(SquelchNameEntry, EntryEl, SocialOverlayZ + 5, FMargin(3.f, 1.f));
		}
	}
	TArray<FACESquelchEntry> List = Client->GetSquelches();
	List.Sort([](const auto& A,const auto& B){return A.Name<B.Name;});
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(
		TEXT("SquelchPage"), TEXT("SquelchListBox"));
	constexpr int32 RowH = 24; // classic_squelch/SquelchEntryTemplate
	SquelchVisibleRows=ListEl ? FMath::Max(1,ListEl->Height/RowH) : 1;
	const int32 MaxRows=FMath::Min(List.Num(),SquelchVisibleRows);
	const int32 MaxOffset=FMath::Max(0,List.Num()-SquelchVisibleRows);
	SquelchScrollOffset=FMath::Clamp(SquelchScrollOffset,0,MaxOffset);
	SyncDatScrollbar(Manager->FindElementUnder(TEXT("SquelchPage"),TEXT("SquelchListBoxScrollbar")),
		MaxOffset ? float(SquelchScrollOffset)/MaxOffset : 0.f,
		List.IsEmpty() ? 1.f : FMath::Min(1.f,float(SquelchVisibleRows)/List.Num()));
	if (!List.ContainsByPredicate([&](const auto& E){return E.Guid==SelectedSquelchGuid && E.Name==SelectedSquelchName;}))
	{SelectedSquelchGuid=0;SelectedSquelchName.Reset();}
	for (const TCHAR* Name:{TEXT("SquelchRemoveButton"),TEXT("SquelchCharacterButton"),TEXT("SquelchAccountButton")})
		if (auto Button=Manager->FindElementUnder(TEXT("SquelchPage"),Name))
		{
			Button->bActivatable=FString(Name)==TEXT("SquelchRemoveButton") ? !SelectedSquelchName.IsEmpty() : List.Num()<50;
			Button->bGhosted=!Button->bActivatable;
		}
	while (SquelchRows.Num() < MaxRows && Canvas->WidgetTree)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		SquelchRows.Add(Row);
		SquelchStatusRows.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
		SquelchRowElements.Add(UACEUILayoutResolver::LoadTemplate(0x21000060,0x10000541));
	}
	SquelchRowGuids.SetNum(SquelchRows.Num());
	SquelchRowNames.SetNum(SquelchRows.Num());
	for (int32 i = 0; i < SquelchRows.Num(); ++i)
	{
		UTextBlock* Row = SquelchRows[i];
		if (!Row)
		{
			continue;
		}
		const int32 Index=i+SquelchScrollOffset;
		const auto Entry=SquelchRowElements[i];
		if (ListEl && Entry && Entry->Parent.Pin()!=ListEl) ListEl->AddChild(Entry);
		if (Entry) Entry->bVisible=ListEl && i<SquelchVisibleRows && List.IsValidIndex(Index);
		if (!Entry || !Entry->bVisible)
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			SquelchStatusRows[i]->SetVisibility(ESlateVisibility::Collapsed);
			SquelchRowGuids[i] = 0;
			SquelchRowNames[i].Reset();
			continue;
		}
		const FACESquelchEntry& E = List[Index];
		SquelchRowGuids[i] = E.Guid;
		SquelchRowNames[i] = E.Name;
		const bool bSel = E.Guid==SelectedSquelchGuid && E.Name==SelectedSquelchName;
		Entry->Y=i*RowH;Entry->Width=ListEl->Width;
		Entry->bUseExplicitState=true;Entry->DefaultState=bSel ? 6 : 1;
		const auto NameEl=Entry->Children.IsEmpty() ? nullptr : Entry->Children[0];
		if (!NameEl) continue;
		NameEl->Width=Entry->Width;NameEl->bUseExplicitState=true;
		NameEl->DefaultState=E.bAccount ? 0x10000057 : 0x10000056;
		for (const auto& Status:NameEl->Children)
		{
			Status->Width=Entry->Width;Status->bUseExplicitState=true;Status->DefaultState=NameEl->DefaultState;
			FString Caption=SocialStrings.Strings.FindRef(E.bAccount ? 0x0CD7089Cu : 0x0C3DF83Cu);
			if (Caption.IsEmpty()) Caption=E.bAccount ? TEXT("Account") : TEXT("Character");
			SquelchStatusRows[i]->SetText(FText::FromString(Caption));
			SquelchStatusRows[i]->SetVisibility(ESlateVisibility::HitTestInvisible);
			SquelchStatusRows[i]->SetJustification(ETextJustify::Right);
			Canvas->PlaceWidgetAtElement(SquelchStatusRows[i],Status,SocialOverlayZ+11);
		}
		Row->SetText(FText::FromString(E.Name));
		Row->SetVisibility(ESlateVisibility::HitTestInvisible);
		Row->SetClipping(EWidgetClipping::ClipToBounds);
		Canvas->PlaceWidgetAtElement(Row,NameEl,SocialOverlayZ+10,FMargin(0,0,90,0));
	}
}

void UACEUIGameplayBinder::RefreshSocialButtonLabels()
{
	if (ActivePanelPage != TEXT("SocialPanel_Field") || !Manager || !Canvas || !Canvas->WidgetTree)
	{
		for (UTextBlock* T : SocialButtonLabels) { if (T) { T->SetVisibility(ESlateVisibility::Collapsed); } }
		for (UBorder* B : SocialCheckboxIcons) { if (B) { B->SetVisibility(ESlateVisibility::Collapsed); } }
		return;
	}
	// Element name → caption. Only elements on the visible page resolve; the rest collapse.
	// bCentered separates DAT buttons (centered caption) from static text labels.
	struct FSocialCaption { const TCHAR* Element; const TCHAR* Text; bool bCentered; };
	if (!bLoadedSocialStrings && Canvas->GetResourceResolver())
		if (const auto* Dat=Canvas->GetResourceResolver()->GetDatSubsystem())
		{
			bLoadedSocialStrings = true;
			SocialStrings.LoadStrings(Dat->GetDatDirectory(), 0x23000001);
		}
	static const FSocialCaption Labels[] = {
		{ TEXT("SwearButton"), TEXT("Swear Allegiance"), true },
		{ TEXT("BreakButton"), TEXT("Break Allegiance"), true },
		{ TEXT("KickButton"), TEXT("Boot Vassal"), true },
		{ TEXT("FellowLeaderButton"), TEXT("Make Leader"), true },
		{ TEXT("FellowQuitButton"), TEXT("Quit"), true },
		{ TEXT("FellowOpenButton"), TEXT("Open"), true },
		{ TEXT("FellowRecruitButton"), TEXT("Recruit"), true },
		{ TEXT("FellowDismissButton"), TEXT("Dismiss"), true },
		{ TEXT("FellowDisbandButton"), TEXT("Disband"), true },
		{ TEXT("CreateFellowshipButton"), TEXT("Create Fellowship"), true },
		{ TEXT("TellButton"), TEXT("Tell"), true },
		{ TEXT("RemoveButton"), TEXT("Remove"), true },
		{ TEXT("AddButton"), TEXT("Add"), true },
		{ TEXT("FriendNameLabel"), TEXT("Friend Name:"), false },
		{ TEXT("FriendsLabel"), TEXT("Friend"), false },
		{ TEXT("OnlineLabel"), TEXT("Status"), false },
		{ TEXT("SquelchRemoveButton"), TEXT("Remove"), true },
		{ TEXT("SquelchCharacterButton"), TEXT("Squelch Character"), true },
		{ TEXT("SquelchAccountButton"), TEXT("Squelch Account"), true },
		{ TEXT("SquelchLabel"), TEXT("Squelched Characters"), false },
		{ TEXT("SquelchNameLabel"), TEXT("Name:"), false },
		{ TEXT("FellowNameLabel"), TEXT("Name"), false },
		{ TEXT("FellowStatsLabel"), TEXT("Stats"), false },
		{ TEXT("MonarchLabel"), TEXT("Monarch:"), false },
		{ TEXT("PatronLabel"), TEXT("Patron:"), false },
	};
	while (SocialButtonLabels.Num() < UE_ARRAY_COUNT(Labels))
	{
		SocialButtonLabels.Add(
			Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	}
	for (int32 i = 0; i < UE_ARRAY_COUNT(Labels); ++i)
	{
		if (UTextBlock* L = SocialButtonLabels[i])
		{
			L->SetJustification(Labels[i].bCentered ? ETextJustify::Center : ETextJustify::Left);
			const bool FriendCaption = FCString::Strcmp(Labels[i].Element, TEXT("FriendsLabel")) == 0
				|| FCString::Strcmp(Labels[i].Element, TEXT("OnlineLabel")) == 0
				|| FCString::Strcmp(Labels[i].Element, TEXT("FriendNameLabel")) == 0;
			// OnlineLabel is also present on the allegiance page. Resolve the
			// friends heading under its own page so it does not inherit that visibility.
			TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(
				FriendCaption ? TEXT("FriendsPage") : TEXT("SocialPanel_Field"), Labels[i].Element);
			if (!El.IsValid())
			{
				El = Manager->FindElementByName(Labels[i].Element);
			}
			FString Caption=El ? SocialStrings.Strings.FindRef(El->TextEntryId) : FString();
			if (Caption.IsEmpty()) Caption=Labels[i].Text;
			if (FString(Labels[i].Element) == TEXT("MonarchLabel") && Client)
			{
				const auto& A = Client->GetAllegiance();
				Caption = SocialStrings.Text(A.PatronGuid != 0 && A.PatronGuid == A.MonarchGuid
					? TEXT("ID_Allegiance_PatronSlashMonarchLabel") : TEXT("ID_Allegiance_MonarchLabel"));
			}
			if (FString(Labels[i].Element)==TEXT("FellowOpenButton") && Client && Client->GetFellowship().bOpen) Caption=TEXT("Close");
			PlaceTextOnElement(L, El, Caption, 8,
				Labels[i].bCentered ? SocialWhite : SocialGold,
				SocialOverlayZ + 20, Labels[i].bCentered);
		}
	}

	// Retail checkbox rows on these pages are character options — draw glyph + text.
	struct FSocialCheck { const TCHAR* Element; const TCHAR* Text; int32 Option; };
	static const FSocialCheck Checks[] = {
		{ TEXT("IgnoreAllegianceRequests"), TEXT("Ignore allegiance requests"), 0x01 },
		{ TEXT("IgnoreFellowshipRequests"), TEXT("Ignore fellowship requests"), 0x02 },
		{ TEXT("FellowshipAutoAcceptRequests"), TEXT("Auto-accept fellowship requests"), 0x12 },
		{ TEXT("FellowshipShareXP"), TEXT("Share experience"), 0x0F },
		{ TEXT("FellowshipShareLoot"), TEXT("Share loot"), 0x11 },
		{ TEXT("AppearOffline_Checkbox"), TEXT("Appear offline"), 0x27 },
	};
	constexpr int32 DidSocialCheckOff = 0x06004D15;
	constexpr int32 DidSocialCheckOn = 0x06004D18;
	const int32 LabelBase = UE_ARRAY_COUNT(Labels);
	while (SocialButtonLabels.Num() < LabelBase + UE_ARRAY_COUNT(Checks))
	{
		UTextBlock* L = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		L->SetJustification(ETextJustify::Left);
		SocialButtonLabels.Add(L);
	}
	for (int32 i = 0; i < UE_ARRAY_COUNT(Checks); ++i)
	{
		TSharedPtr<FACEUIElement> El = Manager->FindElementUnder(
			TEXT("SocialPanel_Field"), Checks[i].Element);
		if (!El.IsValid())
		{
			El = Manager->FindElementByName(Checks[i].Element);
		}
		bool bVisible = El.IsValid() && El->bVisible;
		for (TSharedPtr<FACEUIElement> P = El.IsValid() ? El->Parent.Pin() : nullptr;
			bVisible && P.IsValid(); P = P->Parent.Pin())
		{
			if (!P->bVisible) { bVisible = false; }
		}
		UBorder* Icon = EnsureIconBorder(SocialCheckboxIcons, i);
		UTextBlock* Text = SocialButtonLabels.IsValidIndex(LabelBase + i)
			? SocialButtonLabels[LabelBase + i] : nullptr;
		if (!bVisible || !Icon)
		{
			if (Icon) { Icon->SetVisibility(ESlateVisibility::Collapsed); }
			if (Text) { Text->SetVisibility(ESlateVisibility::Collapsed); }
			continue;
		}
		const bool bOn = Client && Client->IsCharacterOptionSet(Checks[i].Option);
		SetIconDid(Icon, bOn ? DidSocialCheckOn : DidSocialCheckOff);
		Icon->SetVisibility(ESlateVisibility::HitTestInvisible);
		if (Icon->GetParent() != Canvas->GetElementLayer())
		{
			Canvas->GetElementLayer()->AddChild(Icon);
		}
		const FIntPoint O = El->GetScreenOrigin();
		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Icon->Slot))
		{
			Slot->SetAnchors(FAnchors(0.f, 0.f));
			Slot->SetPosition(Canvas->LayoutToViewport(FVector2D(O.X, O.Y + 1)));
			Slot->SetSize(FVector2D(13.f, 13.f)*Canvas->GetLastScale2D());
			Canvas->SetOverlayOrder(Icon, Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")), SocialOverlayZ + 21);
		}
		if (Text)
		{
			Text->SetText(FText::FromString(Checks[i].Text));
			Text->SetColorAndOpacity(FSlateColor(bOn ? SocialGold : SocialWhite));
			Text->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 8));
			Text->SetVisibility(ESlateVisibility::HitTestInvisible);
			Canvas->PlaceWidgetAtElement(Text, El, SocialOverlayZ + 22, FMargin(16.f, 0.f, 2.f, 0.f));
		}
	}
}

void UACEUIGameplayBinder::RefreshQuestOverlays()
{
	for (UTextBlock* L : QuestTabLabels) if (L) L->SetVisibility(ESlateVisibility::Collapsed);
	for (UTextBlock* L : JournalLabels) if (L) L->SetVisibility(ESlateVisibility::Collapsed);
	for (const auto& Row : JournalRowElements) if (Row) Row->bVisible=false;
	for (const auto& Row : ContractRowElements) if (Row) Row->bVisible=false;
	for (UACERetailTextEntry* E : JournalEntries) if (E) E->SetVisibility(ESlateVisibility::Collapsed);
	if (JournalNotes) JournalNotes->SetVisibility(ESlateVisibility::Collapsed);
	if (ActivePanelPage != TEXT("QuestManagementPanel_Field") || !Client || !Manager || !Canvas
		|| !Canvas->WidgetTree)
	{
		for (UTextBlock* R : QuestRows) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* R : QuestStatusRows) { if (R) R->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* T : QuestDetailLabels) { if (T) T->SetVisibility(ESlateVisibility::Collapsed); }
		QuestRowIds.Reset();
		return;
	}
	EnsureOverlays();
	if (const auto Panel = Manager->FindElementByName(TEXT("QuestManagementPanel_Field")))
	{
		const TCHAR* Tabs[] = {TEXT("Contracts"), TEXT("Journal"), TEXT("PageList")};
		const TCHAR* Captions[] = {TEXT("Contracts"), TEXT("Journal"), TEXT("Page List")};
		while (QuestTabLabels.Num()<3) QuestTabLabels.Add(Canvas->WidgetTree->ConstructWidget<UACERetailTextBlock>());
		for (int32 I=0; I<3; ++I)
		{
			const FString Page=FString(Tabs[I])+TEXT("Page"), Tab=FString(Tabs[I])+TEXT("Tab");
			Manager->SetElementVisibleByName(Page,ActiveQuestTab==Page);
			Manager->SetElementVisibleByName(Tab,true);
			ApplyPanelTabChrome(Tab,ActiveQuestTab==Page);
			PlaceTextOnElement(QuestTabLabels[I],Tab,Captions[I],9,SocialWhite,660,true);
		}
		if (ActiveQuestTab != TEXT("ContractsPage"))
		{
			for (UTextBlock* R:QuestRows) if (R) R->SetVisibility(ESlateVisibility::Collapsed);
			for (UTextBlock* R:QuestStatusRows) if (R) R->SetVisibility(ESlateVisibility::Collapsed);
			for (UTextBlock* R:QuestDetailLabels) if (R) R->SetVisibility(ESlateVisibility::Collapsed);
			RefreshJournalOverlays(); return;
		}

	}
	UACEDatSubsystem* Dat = nullptr;
	if (PlayerController)
	{
		if (UGameInstance* GI = PlayerController->GetGameInstance())
		{
			Dat = GI->GetSubsystem<UACEDatSubsystem>();
		}
	}
	TArray<FACEContractEntry> Contracts = Client->GetContracts();
	for (UTextBlock* R:QuestStatusRows) if (R) R->SetVisibility(ESlateVisibility::Collapsed);
	const double Now = FPlatformTime::Seconds();
	auto RepeatRemaining = [Now](const FACEContractEntry& C)
	{
		const double Elapsed = C.ReceivedAt > 0.0 ? FMath::Max(0.0, Now - C.ReceivedAt) : 0.0;
		return FMath::Max(0.0, C.TimeWhenRepeats - Elapsed);
	};
	auto Status=[Dat, &RepeatRemaining](const FACEContractEntry& C)->FString
	{
		if(C.Stage==1)return TEXT("Available");
		if (C.Stage == 3)
		{
			if (C.TimeWhenRepeats > 0.0)
			{
				const double Remaining = RepeatRemaining(C);
				return Remaining > 0.0
					? FString::Printf(TEXT("Done (%s to Repeat)"), *ContractRepeatTime(Remaining))
					: FString(TEXT("Available"));
			}
			const auto* Info = Dat ? Dat->GetContractInfo(C.ContractId) : nullptr;
			return Info && !Info->QuestFlagRepeatTime.IsEmpty() ? TEXT("Available") : TEXT("Done");
		}
		// Retail gmContractsUI::FillProgressString substitutes stage - 4 into
		// the DAT progress description (for example, remaining kills).
		if(C.Stage>=4 && Dat)
			if(const auto* Info=Dat->GetContractInfo(C.ContractId);Info && !Info->DescriptionProgress.IsEmpty())
			{
				FString Text;
				const FString& Format=Info->DescriptionProgress;
				for(int32 I=0;I<Format.Len();++I)
				{
					if(Format[I]=='%' && I+1<Format.Len())
					{
						if(Format[I+1]=='%'){Text.AppendChar('%');++I;continue;}
						if(Format[I+1]=='d'||Format[I+1]=='u'||Format[I+1]=='i')
						{Text+=FString::FromInt(C.Stage-4);++I;continue;}
					}
					Text.AppendChar(Format[I]);
				}
				return Text;
			}
		return C.Stage >= 2 ? TEXT("In Progress") : TEXT("");
	};
	auto ContractName = [Dat](int32 ContractId) -> FString
	{
		if (Dat)
		{
			if (const FACEDatContractInfo* Info = Dat->GetContractInfo(static_cast<uint32>(ContractId)))
			{
				if (!Info->Name.IsEmpty())
				{
					return Info->Name;
				}
			}
		}
		return FString::Printf(TEXT("Contract %d"), ContractId);
	};
	// Retail sorts visible status text, with numeric repeat timers and names as
	// tie-breakers. Use one time snapshot so comparisons remain consistent.
	Contracts.Sort([&](const FACEContractEntry& A, const FACEContractEntry& B)
	{
		int32 Order = 0;
		if (bQuestSortByStatus)
		{
			const double ARepeat = A.Stage == 3 ? RepeatRemaining(A) : 0.0;
			const double BRepeat = B.Stage == 3 ? RepeatRemaining(B) : 0.0;
			const FString AStatus = ARepeat > 0.0 ? FString(TEXT("Done")) : Status(A);
			const FString BStatus = BRepeat > 0.0 ? FString(TEXT("Done")) : Status(B);
			Order = AStatus.Compare(BStatus, ESearchCase::IgnoreCase);
			if (Order == 0 && ARepeat != BRepeat) Order = ARepeat < BRepeat ? -1 : 1;
		}
		if (Order == 0) Order = ContractName(A.ContractId).Compare(ContractName(B.ContractId), ESearchCase::IgnoreCase);
		if (Order == 0 && A.ContractId != B.ContractId) Order = A.ContractId < B.ContractId ? -1 : 1;
		return bQuestReverseSort ? Order > 0 : Order < 0;
	});

	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementByName(TEXT("ContractsBox"));
	// Keep the retail columns and font. Long progress descriptions wrap instead
	// of clipping the count or taking all the space reserved for contract names.
	FACEDatFont ContractFont;
	const bool HasContractFont=Canvas->GetResourceResolver()->ResolveFont(0x40000001,ContractFont);
	const int32 StatusWidth=ListEl?FMath::Max(1,ListEl->Width-152):118;
	int32 QuestRowHeight=20;
	if(HasContractFont)for(const auto& C:Contracts)
		QuestRowHeight=FMath::Max(QuestRowHeight,
			ACEDatText::Layout(ContractFont,Status(C),FMath::Max(1,StatusWidth-12),false).Num()*int32(ContractFont.MaxCharHeight)+2);
	const int32 VisibleRows = ListEl.IsValid()
		? FMath::Max(1, ListEl->Height / QuestRowHeight) : 1;
	QuestScrollOffset = FMath::Clamp(QuestScrollOffset, 0,
		FMath::Max(0, Contracts.Num() - VisibleRows));
	while (QuestRows.Num() < VisibleRows)
	{
		UTextBlock* Row = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		Row->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9));
		QuestRows.Add(Row);
	}
	QuestRowIds.SetNum(QuestRows.Num());
	while (QuestStatusRows.Num()<QuestRows.Num())
		QuestStatusRows.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	while (ContractRowElements.Num()<VisibleRows)
		ContractRowElements.Add(UACEUILayoutResolver::LoadTemplate(0x21000069,0x100005D7));
	if (!Contracts.ContainsByPredicate([this](const FACEContractEntry& C){return C.ContractId==SelectedContractId;}))
		SelectedContractId=Contracts.IsEmpty()?0:Contracts[0].ContractId;
	if (Contracts.Num() == 0 && ListEl.IsValid() && QuestRows.Num() > 0 && QuestRows[0])
	{
		QuestRows[0]->SetText(FText::FromString(TEXT("No active contracts.")));
		QuestRows[0]->SetColorAndOpacity(FSlateColor(SocialDim));
		QuestRows[0]->SetVisibility(ESlateVisibility::HitTestInvisible);
		Canvas->PlaceWidgetAtElement(QuestRows[0], ListEl, 640, FMargin(6.f, 4.f, 6.f, 4.f));
		QuestRowIds[0] = 0;
	}
	for (int32 i = (Contracts.Num() == 0 ? 1 : 0); i < QuestRows.Num(); ++i)
	{
		UTextBlock* Row = QuestRows[i];
		if (!Row)
		{
			continue;
		}
		const int32 Index = QuestScrollOffset + i;
		if (!ListEl.IsValid() || i >= VisibleRows || !Contracts.IsValidIndex(Index))
		{
			Row->SetVisibility(ESlateVisibility::Collapsed);
			if (QuestRowIds.IsValidIndex(i)) { QuestRowIds[i] = 0; }
			continue;
		}
		const FACEContractEntry& C = Contracts[Index];
		QuestRowIds[i] = C.ContractId;
		const auto Entry=ContractRowElements[i];
		if(!Entry || Entry->Children.Num()<2)continue;
		if(Entry->Parent.Pin()!=ListEl)ListEl->AddChild(Entry);
		Entry->bVisible=true;Entry->Y=i*QuestRowHeight;Entry->Width=ListEl->Width;Entry->Height=QuestRowHeight;
		Entry->bUseExplicitState=true;Entry->DefaultState=C.ContractId==SelectedContractId?6:1;
		const auto NameCell=Entry->Children[0],StatusCell=Entry->Children[1];
		NameCell->Width=ListEl->Width-StatusWidth;
		StatusCell->X=NameCell->Width;StatusCell->Width=StatusWidth;
		NameCell->Height=StatusCell->Height=QuestRowHeight;
		StatusCell->bTextOneLine=false;
		const FString Name=ContractName(C.ContractId);
		const int32 NameWidth=FMath::Max(0,NameCell->Width-int32(NameCell->TextMargins.Left+NameCell->TextMargins.Right));
		PlaceTextOnElement(Row,NameCell,HasContractFont?ACEDatText::Ellipsize(ContractFont,Name,NameWidth):Name,
			9,SocialWhite,640+i);
		UTextBlock* State=QuestStatusRows[i];
		PlaceTextOnElement(State,StatusCell,Status(C),9,SocialWhite,640+i);
	}
	if (TSharedPtr<FACEUIElement> Bar = Manager->FindElementByName(TEXT("ContractsBoxScrollbar")))
	{
		const int32 MaxOffset = FMath::Max(0, Contracts.Num() - VisibleRows);
		const float Frac = MaxOffset > 0
			? static_cast<float>(QuestScrollOffset) / static_cast<float>(MaxOffset) : 0.f;
		const float VisibleFrac = Contracts.Num() > 0
			? FMath::Clamp(static_cast<float>(VisibleRows) / static_cast<float>(Contracts.Num()), 0.05f, 1.f)
			: 1.f;
		SyncDatScrollbar(Bar, Frac, VisibleFrac);
	}

	// Detail pane + static captions. Retail fills all of these from the ContractTable.
	const FACEContractEntry* Sel = Contracts.FindByPredicate([this](const FACEContractEntry& C)
	{
		return C.ContractId == SelectedContractId;
	});
	if (!Sel && Contracts.Num() > 0)
	{
		Sel = &Contracts[0];
		SelectedContractId = Sel->ContractId;
	}
	if (!Sel) SelectedContractId = 0;
	if (auto Abandon = Manager->FindElementByName(TEXT("ContractAbandonButton")))
	{
		// The DAT starts this button ghosted. Any selected tracked contract can
		// be abandoned, including an available or completed contract.
		Abandon->bGhosted = Sel == nullptr;
		Abandon->bActivatable = Sel != nullptr;
	}
	const FACEDatContractInfo* Info = (Sel && Dat)
		? Dat->GetContractInfo(static_cast<uint32>(Sel->ContractId)) : nullptr;
	auto FormatCell = [](uint32 Cell, const FVector3f& Origin) -> FString
	{
		if (Cell == 0)
		{
			return TEXT("-");
		}
		// Landblock coordinates: retail shows N/S, E/W from the global landblock grid.
		const int32 BlockX = static_cast<int32>((Cell >> 24) & 0xFF);
		const int32 BlockY = static_cast<int32>((Cell >> 16) & 0xFF);
		const float GlobalX = BlockX * 192.f + Origin.X;
		const float GlobalY = BlockY * 192.f + Origin.Y;
		const float NS = GlobalY / 240.f - 102.f;
		const float EW = GlobalX / 240.f - 102.f;
		return FString::Printf(TEXT("%.1f%s, %.1f%s"), FMath::Abs(NS), NS >= 0.f ? TEXT("N") : TEXT("S"),
			FMath::Abs(EW), EW >= 0.f ? TEXT("E") : TEXT("W"));
	};
	const FString StatusText = Sel?Status(*Sel):FString(TEXT("-"));
	auto Remaining=[Sel, Now](double Seconds)
	{
		const double Elapsed=Sel&&Sel->ReceivedAt>0?FMath::Max(0.0,Now-Sel->ReceivedAt):0;
		const int32 S=FMath::CeilToInt(FMath::Clamp(Seconds-Elapsed,0.0,double(MAX_int32)));
		return FString::Printf(TEXT("%dd %02d:%02d:%02d"),S/86400,(S/3600)%24,(S/60)%60,S%60);
	};
	FString TimedText = TEXT("Not timed");
	if (Sel && Sel->TimeWhenDone > 0.f)
	{
		TimedText = FString(TEXT("Time left: "))+Remaining(Sel->TimeWhenDone);
	}
	else if (Sel && Sel->TimeWhenRepeats > 0.f)
	{
		TimedText = FString(TEXT("Available in: "))+Remaining(Sel->TimeWhenRepeats);
	}
	const FString Notes = Info ? Info->Description : FString();
	struct FQuestCaption { const TCHAR* Element; FString Text; bool bGold; };
	const FQuestCaption Captions[] = {
		{ TEXT("ContractNameSortButton"), FString(TEXT("Contract")), false },
		{ TEXT("ContractStatusSortButton"), FString(TEXT("Status")), false },
		{ TEXT("ContractStatusLabel"), FString(TEXT("Status:")), true },
		{ TEXT("ContractStatusText"), StatusText, false },
		{ TEXT("ContractContactLabel"), FString(TEXT("Contact:")), true },
		{ TEXT("ContractContactText"), Info && !Info->NameNPCStart.IsEmpty()
			? Info->NameNPCStart : FString(TEXT("-")), false },
		{ TEXT("ContractContactLocLabel"), FString(TEXT("Contact location:")), true },
		{ TEXT("ContractContactLocText"), Info
			? FormatCell(Info->CellNPCStart, Info->OriginNPCStart) : FString(TEXT("-")), false },
		{ TEXT("ContractAreaLabel"), FString(TEXT("Quest area:")), true },
		{ TEXT("ContractAreaText"), Info
			? FormatCell(Info->CellQuestArea, Info->OriginQuestArea) : FString(TEXT("-")), false },
		{ TEXT("ContractNotesLabel"), Notes, false },
		{ TEXT("ContractTimedLabel"), FString(TEXT("Timing:")), true },
		{ TEXT("ContractTimedText"), TimedText, false },
		{ TEXT("ContractAbandonButton"), FString(TEXT("Abandon")), false },
	};
	while (QuestDetailLabels.Num() < UE_ARRAY_COUNT(Captions))
	{
		QuestDetailLabels.Add(
			Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
	}
	for (int32 i = 0; i < UE_ARRAY_COUNT(Captions); ++i)
	{
		UTextBlock* Label = QuestDetailLabels[i];
		if (!Label)
		{
			continue;
		}
		const bool bButton = FCString::Strstr(Captions[i].Element, TEXT("Button")) != nullptr;
		const bool bWrap = FCString::Strcmp(Captions[i].Element, TEXT("ContractNotesLabel")) == 0;
		Label->SetJustification(bButton ? ETextJustify::Center : ETextJustify::Left);
		Label->SetAutoWrapText(bWrap);
		PlaceTextOnElement(Label, Captions[i].Element, Captions[i].Text, bWrap ? 8 : 9,
			Captions[i].bGold ? SocialGold : SocialWhite, 660 + i, bButton);
	}
}

void UACEUIGameplayBinder::ShowSalvagePanel(int32 ToolGuid)
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	FACEWorldObject Tool;
	if (!Session || !Client->GetWorldObject(ToolGuid, Tool)
		|| !(uint32(Tool.ItemType) & 0x20000000u) || !IsOwnedSalvageObject(*Session, ToolGuid))
	{
		return;
	}
	OpenSalvageToolGuid = ToolGuid;
	SalvageMaterialType = 0;
	SalvageQueueGuids.Reset();
	SalvageScrollOffset = 0;
	OpenLootContainerGuid = 0;
	OpenVendorGuid = 0;
	bTradeOpen = false;
	ExpandEnvFloatyForWideContent();
	SyncEnvPanelMode();
	if (Manager)
	{
		if (TSharedPtr<FACEUIElement> Env = Manager->FindElementByName(TEXT("RootGameplay_FloatyEnvPanel_Field")))
		{
			Manager->BringFloatyToFront(Env);
		}
		Manager->SetElementVisibleByName(TEXT("SalvagePanel"), true);
	}
	RefreshSalvageOverlays();
	if (PlayerController && PlayerController->IsVRActive())
		if (auto* Rig = PlayerController->GetVRComponent()) Rig->RevealSalvagePanel();
	PostInventorySystemMessage(TEXT("Drag salvageable items onto the Ust panel, then click Salvage."));
}

void UACEUIGameplayBinder::HideSalvagePanel()
{
	OpenSalvageToolGuid = 0;
	SalvageMaterialType = 0;
	SalvageQueueGuids.Reset();
	SalvageScrollOffset = 0;
	SalvageItemSlotGuids.Reset();
	for (UBorder* B : SalvageItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : SalvageItemBackgrounds) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UBorder* B : SalvageItemSelections) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
	for (UTextBlock* L : SalvageLabels) { if (L) L->SetVisibility(ESlateVisibility::Collapsed); }
	if (SalvageWarningLabel) { SalvageWarningLabel->SetVisibility(ESlateVisibility::Collapsed); }
	SyncEnvPanelMode();
}

bool UACEUIGameplayBinder::CanAddItemToSalvageQueue(int32 Guid) const
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!Session || !OpenSalvageToolGuid || !IsOwnedSalvageObject(*Session, OpenSalvageToolGuid)) return false;
	TSet<int32> Seen;
	TFunction<bool(int32)> CanAdd = [&](int32 ItemGuid)
	{
		if (!ItemGuid || ItemGuid == OpenSalvageToolGuid || Seen.Contains(ItemGuid)
			|| SalvageQueueGuids.Contains(ItemGuid) || !IsOwnedSalvageObject(*Session, ItemGuid)) return false;
		Seen.Add(ItemGuid);
		const FACEWorldObject* Item = Session->GetWorldObjects().Find(ItemGuid);
		if (!Item) return false;
		TArray<int32> Children;
		if (GetSalvageChildren(*Session, *Item, Children))
		{
			for (int32 Child : Children) if (CanAdd(Child)) return true;
			return false;
		}
		return IsSalvageCandidate(*Session, *Item, OpenSalvageToolGuid)
			&& (Client->IsCharacterOptionSet(0x22) || !SalvageMaterialType || Item->MaterialType == SalvageMaterialType);
	};
	return CanAdd(Guid);
}

bool UACEUIGameplayBinder::AddItemToSalvageQueue(int32 Guid)
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	if (!Session || !CanAddItemToSalvageQueue(Guid))
	{
		PostInventorySystemMessage(TEXT("That item cannot be added to salvage. Check its material, retained status, and ownership."));
		return false;
	}
	TSet<int32> Seen;
	const int32 Before = SalvageQueueGuids.Num();
	TFunction<void(int32)> Add = [&](int32 ItemGuid)
	{
		if (Seen.Contains(ItemGuid) || ItemGuid == OpenSalvageToolGuid
			|| SalvageQueueGuids.Contains(ItemGuid) || !IsOwnedSalvageObject(*Session, ItemGuid)) return;
		Seen.Add(ItemGuid);
		const FACEWorldObject* Item = Session->GetWorldObjects().Find(ItemGuid);
		if (!Item) return;
		TArray<int32> Children;
		if (GetSalvageChildren(*Session, *Item, Children))
		{
			PostInventorySystemMessage(FString::Printf(TEXT("Adding contents of %s."), *Item->Name));
			for (int32 Child : Children) Add(Child);
			return;
		}
		if (!IsSalvageCandidate(*Session, *Item, OpenSalvageToolGuid)
			|| (!Client->IsCharacterOptionSet(0x22) && SalvageMaterialType && Item->MaterialType != SalvageMaterialType)) return;
		if (!SalvageMaterialType) SalvageMaterialType = Item->MaterialType;
		SalvageQueueGuids.Add(ItemGuid);
	};
	Add(Guid);
	RefreshSalvageOverlays();
	return SalvageQueueGuids.Num() > Before;
}

void UACEUIGameplayBinder::RemoveItemFromSalvageQueue(int32 Guid)
{
	SalvageQueueGuids.Remove(Guid);
	if (SalvageQueueGuids.Num() == 0)
	{
		SalvageMaterialType = 0;
	}
	RefreshSalvageOverlays();
}

void UACEUIGameplayBinder::SubmitSalvageQueue()
{
	const auto Session = Client ? Client->GetSession() : nullptr;
	FACEWorldObject Tool;
	if (!Session || !Client->GetWorldObject(OpenSalvageToolGuid, Tool)
		|| !(uint32(Tool.ItemType) & 0x20000000u) || !IsOwnedSalvageObject(*Session, OpenSalvageToolGuid))
	{
		HideSalvagePanel();
		return;
	}
	// Retail submits last-to-first, then clears immediately. Item creation/removal
	// is authoritative inventory replication; SalvageOperationsResult is chat only.
	TArray<int32> Items;
	for (int32 I = SalvageQueueGuids.Num() - 1; I >= 0; --I)
	{
		const FACEWorldObject* Item = Session->GetWorldObjects().Find(SalvageQueueGuids[I]);
		TArray<int32> Children;
		if (Item && IsSalvageCandidate(*Session, *Item, OpenSalvageToolGuid)
			&& !GetSalvageChildren(*Session, *Item, Children)) Items.AddUnique(Item->Guid);
	}
	if (!Items.IsEmpty()) Client->SendCreateTinkeringTool(OpenSalvageToolGuid, Items);
	SalvageQueueGuids.Reset();
	SalvageMaterialType = 0;
	SalvageScrollOffset = 0;
	RefreshSalvageOverlays();
}

bool UACEUIGameplayBinder::ScrollSalvage(float WheelDelta, FVector2D CanvasLocalPos)
{
	if (!OpenSalvageToolGuid || !Manager || !Canvas || FMath::IsNearlyZero(WheelDelta)) return false;
	const auto List = Manager->FindElementUnder(TEXT("SalvagePanel"), TEXT("SalvageItemsList"));
	if (!Canvas->IsElementExposedAt(List, CanvasLocalPos)) return false;
	SetSalvageScrollOffset(SalvageScrollOffset + (WheelDelta > 0 ? -1 : 1));
	return true;
}

void UACEUIGameplayBinder::SetSalvageScrollOffset(int32 Offset)
{
	SalvageScrollOffset = FMath::Clamp(Offset, 0, FMath::Max(0, SalvageQueueGuids.Num() + 1 - SalvageVisibleSlots));
	RefreshSalvageOverlays();
}

void UACEUIGameplayBinder::RefreshSalvageOverlays()
{
	// Queued items can be sold, moved, retained, or removed by inventory updates.
	// Never leave stale invisible entries for a later destructive submit.
	if (Client && OpenSalvageToolGuid)
	{
		const auto Session = Client->GetSession();
		if (!Session || !IsOwnedSalvageObject(*Session, OpenSalvageToolGuid))
		{
			HideSalvagePanel();
			return;
		}
		SalvageQueueGuids.RemoveAll([&](int32 Guid)
		{
			const FACEWorldObject* Item = Session->GetWorldObjects().Find(Guid);
			TArray<int32> Children;
			return !Item || !IsSalvageCandidate(*Session, *Item, OpenSalvageToolGuid)
				|| GetSalvageChildren(*Session, *Item, Children);
		});
		if (SalvageQueueGuids.IsEmpty()) SalvageMaterialType = 0;
	}
	if (!Client || !Manager || !Canvas || OpenSalvageToolGuid == 0)
	{
		for (UBorder* B : SalvageItemSlots) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : SalvageItemBackgrounds) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UBorder* B : SalvageItemSelections) { if (B) B->SetVisibility(ESlateVisibility::Collapsed); }
		for (UTextBlock* L : SalvageLabels) { if (L) L->SetVisibility(ESlateVisibility::Collapsed); }
		if (SalvageWarningLabel) { SalvageWarningLabel->SetVisibility(ESlateVisibility::Collapsed); }
		return;
	}
	SyncEnvPanelMode();
	EnsureOverlays();
	TSharedPtr<FACEUIElement> ListEl = Manager->FindElementUnder(TEXT("SalvagePanel"), TEXT("SalvageItemsList"));
	if (!ListEl.IsValid())
	{
		return;
	}
	constexpr int32 Cell = 32;
	constexpr int32 OverlayZ = 120000;
	// The authored salvage list is a single horizontal row, with its own scrollbar.
	SalvageVisibleSlots = FMath::Max(1, ListEl->Width / Cell);
	// UI_ItemList_AtLeastOneEmptySlot reserves a drop slot after the final item,
	// including when the last item exactly fills the visible row.
	const int32 Capacity = SalvageQueueGuids.Num() + 1;
	const int32 MaxOffset = FMath::Max(0, Capacity - SalvageVisibleSlots);
	SalvageScrollOffset = FMath::Clamp(SalvageScrollOffset, 0, MaxOffset);
	if (const auto Bar = Manager->FindElementUnder(TEXT("SalvagePanel"), TEXT("Salvage_ItemListScroll")))
	{
		// DAT UICore_Scrollbar_hide_when_disabled: no track, arrows, or thumb
		// while the complete list and trailing drop slot fit in the row.
		Bar->bVisible = MaxOffset > 0;
		SyncDatScrollbar(Bar, MaxOffset ? float(SalvageScrollOffset) / MaxOffset : 0.f,
			FMath::Min(1.f, float(SalvageVisibleSlots) / Capacity));
	}
	for (UBorder* B : SalvageItemSlots) if (B) B->SetVisibility(ESlateVisibility::Collapsed);
	for (UBorder* B : SalvageItemBackgrounds) if (B) B->SetVisibility(ESlateVisibility::Collapsed);
	for (UBorder* B : SalvageItemSelections) if (B) B->SetVisibility(ESlateVisibility::Collapsed);
	for (UTextBlock* L : SalvageLabels) if (L) L->SetVisibility(ESlateVisibility::Collapsed);
	SalvageItemSlotGuids.SetNumZeroed(SalvageVisibleSlots);
	for (int32 i = 0; i < SalvageVisibleSlots; ++i)
	{
		const FMargin Insets(i * Cell, 0, ListEl->Width - (i + 1) * Cell, ListEl->Height - Cell);
		// UIElement_ItemList keeps default slot art across the visible row, even
		// with no offers. A trailing empty cell also remains a visible drop target.
		UBorder* Bg = EnsureIconBorder(SalvageItemBackgrounds, i);
		if (Bg)
		{
			SetItemSlotBackground(Bg, nullptr);
			Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
			Canvas->PlaceWidgetAtElement(Bg, ListEl, OverlayZ, Insets);
		}
		UBorder* Icon = EnsureIconBorder(SalvageItemSlots, i);
		if (!Icon)
		{
			continue;
		}
		if (!SalvageQueueGuids.IsValidIndex(i + SalvageScrollOffset))
		{
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			SalvageItemSlotGuids[i] = 0;
			continue;
		}
		const int32 Guid = SalvageQueueGuids[i + SalvageScrollOffset];
		FACEWorldObject Obj;
		if (!Client->GetWorldObject(Guid, Obj))
		{
			Icon->SetVisibility(ESlateVisibility::Collapsed);
			SalvageItemSlotGuids[i] = 0;
			continue;
		}
		SalvageItemSlotGuids[i] = Guid;
		SetItemSlotForeground(Icon, &Obj);
		Icon->SetVisibility(ESlateVisibility::Visible);
		UACEHoverTooltipWidget::SetWidgetTooltip(Icon, FText::FromString(ACERetailObjectNames::Name(Obj)));
		Canvas->PlaceWidgetAtElement(Icon, ListEl, OverlayZ + 1, Insets);
		if (Bg)
		{
			SetItemSlotBackground(Bg, &Obj);
			Bg->SetVisibility(ESlateVisibility::HitTestInvisible);
			Canvas->PlaceWidgetAtElement(Bg, ListEl, OverlayZ, Insets);
		}
		if (Guid == LastSelection.Guid)
			if (UBorder* Selection = EnsureIconBorder(SalvageItemSelections, i))
			{
				SetIconDid(Selection, 0x06004D09);
				Selection->SetVisibility(ESlateVisibility::HitTestInvisible);
				Canvas->PlaceWidgetAtElement(Selection, ListEl, OverlayZ + 2, Insets);
			}
	}
	if (!SalvageWarningLabel && Canvas->WidgetTree)
	{
		SalvageWarningLabel = Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass());
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 9);
		SalvageWarningLabel->SetFont(Font);
	}
	if (SalvageWarningLabel)
	{
		// client_local_English.dat 0x23000001:0x0A96DC87. The retail action
		// is immediate; keep this warning visible even while the queue is empty.
		const FString Warn = TEXT("WARNING: Items in this panel will be destroyed!");
		SalvageWarningLabel->SetText(FText::FromString(Warn));
		SalvageWarningLabel->SetVisibility(ESlateVisibility::HitTestInvisible);
		SalvageWarningLabel->SetColorAndOpacity(FSlateColor(SocialGold));
		PlaceTextOnElement(SalvageWarningLabel, TEXT("SalvageWarning_Text"), Warn, 9, SocialWhite, OverlayZ);
	}
	if (auto Button = Manager->FindElementUnder(TEXT("SalvagePanel"), TEXT("Salvage_Button")))
	{
		Button->bVisible = true;
		Button->bActivatable = !SalvageQueueGuids.IsEmpty();
		Button->bGhosted = !Button->bActivatable;
		if (SalvageLabels.IsEmpty() && Canvas->WidgetTree)
			SalvageLabels.Add(Canvas->WidgetTree->ConstructWidget<UTextBlock>(UACERetailTextBlock::StaticClass()));
		if (!SalvageLabels.IsEmpty()) PlaceTextOnElement(SalvageLabels[0], Button, TEXT("Salvage"), 9,
			Button->bActivatable ? SocialWhite : SocialDim, OverlayZ, true);
		if (!SalvageLabels.IsEmpty() && !Button->bActivatable)
			SalvageLabels[0]->SetColorAndOpacity(FSlateColor(SocialDim));
	}
}
