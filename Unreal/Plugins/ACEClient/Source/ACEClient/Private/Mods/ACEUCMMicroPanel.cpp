#include "Mods/ACEPluginStyle.h"
#include "ACERuntimeOptions.h"
#include "Mods/ACEPluginSubsystem.h"
#include "ACEClientSubsystem.h"
#include "ACEDatSubsystem.h"
#include "ACEPlayerController.h"
#include "ACESession.h"
#include "Engine/GameInstance.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"

TSharedPtr<FACEClientPlugin> UACEPluginSubsystem::CompactPolicy(const FString& Owner) const
{
    if(Owner==TEXT("ucmmicro"))
        if(auto Full=Find(TEXT("ucm"));Full&&Full->Enabled)return Full;
    return Find(Owner);
}
bool UACEPluginSubsystem::IsUCMFollowEnabled(const FString& Owner) const
{
    auto P=CompactPolicy(Owner);if(!P||!P->Profile)return false;
    bool Navigation=false;FString Name;double Id=0;
    P->Profile->TryGetBoolField(TEXT("navigation"),Navigation);
    P->Profile->TryGetStringField(TEXT("follow_name"),Name);P->Profile->TryGetNumberField(TEXT("follow_id"),Id);
    return Navigation&&(!Name.IsEmpty()||Id!=0);
}
bool UACEPluginSubsystem::SetUCMFollowSelected(bool Enabled,const FString& Owner)
{
    auto P=CompactPolicy(Owner);if(!P||!P->Profile)return false;
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Target;
    if(Enabled&&(!C->GetWorldObject(C->GetSelectedObject().Guid,Target)||!Target.bIsPlayer
        ||Target.Guid==C->GetPlayerGuid()||!Target.bHasPosition))
    {P->Status=TEXT("Select another player to follow");return false;}
    const auto Previous=MakeShared<FJsonObject>(*P->Profile);
    P->Profile->SetStringField(TEXT("follow_name"),Enabled?Target.Name:FString());
    P->Profile->SetNumberField(TEXT("follow_id"),Enabled?uint32(Target.Guid):0);
    // Stopping Follow must not unexpectedly begin walking an old recorded route.
    P->Profile->SetBoolField(TEXT("navigation"),Enabled);
    if(!SaveCurrentProfile(P->Id,false))
    {P->Profile=Previous;P->Status=TEXT("Could not save follow settings: ")+Notice;return false;}
    FollowMovementSamples.Reset();FollowMovementTarget=0;bFollowMovementOverflow=false;
    if(MovementOwner==P->Id){MovementOwner.Empty();MoveExpires=0;FaceHeading.Reset();}
    P->NextDecision=0;
    P->Status=Enabled?(P->Running?TEXT("Following: "):TEXT("Start UCM to follow: "))+Target.Name:TEXT("Follow stopped; navigation disabled");
    ScheduleDecisionWake(FPlatformTime::Seconds());
    return true;
}
void UACEPluginSubsystem::ObserveFollowPosition(int32 Guid,const FACEPosition& Position)
{
    auto P=PolicyPlugin();bool Corners=false;
    if(!P||!P->Running||!IsUCMFollowEnabled(P->Id)||!P->Profile->TryGetBoolField(TEXT("follow_corners"),Corners)||!Corners)return;
    FString Name;double Id=0;P->Profile->TryGetStringField(TEXT("follow_name"),Name);P->Profile->TryGetNumberField(TEXT("follow_id"),Id);
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();FACEWorldObject Target;
    if(!C->GetWorldObject(Guid,Target)||!Target.bIsPlayer||(!Name.IsEmpty()?Target.Name!=Name:uint32(Guid)!=uint32(Id)))return;
    if(FollowMovementTarget!=Guid||FollowMovementTeleport!=C->GetTeleportSeq())
    {FollowMovementSamples.Reset();bFollowMovementOverflow=false;}
    FollowMovementTarget=Guid;FollowMovementTeleport=C->GetTeleportSeq();
    if(!FollowMovementSamples.IsEmpty()&&FVector::DistSquared(FollowMovementSamples.Last().ToUnrealLocation(),Position.ToUnrealLocation())<FMath::Square(9.6))return;
    if(FollowMovementSamples.Num()>=512){bFollowMovementOverflow=true;return;}
    FollowMovementSamples.Add(Position);
}
FString UACEPluginSubsystem::UCMCurrentStatus(const FString& Owner) const
{
    auto P=CompactPolicy(Owner);if(!P)return TEXT("UCM unavailable");
    if(!P->Running)return P->Status;
    for(const TCHAR* Id:{TEXT("itemtools"),TEXT("autocraft")})if(auto Other=Find(Id);Other&&Other->Running)
        return TEXT("Paused while ")+Other->Name+TEXT(" runs");
    auto* C=GetGameInstance()->GetSubsystem<UACEClientSubsystem>();auto S=C->GetSession();
    const auto Elapsed=[](double Start){return FString::Printf(TEXT(" (%ds)"),FMath::Max(0,int32(FPlatformTime::Seconds()-Start)));};
    if(S&&S->GetPendingEquipmentGuid())
    {
        FACEWorldObject Item;C->GetWorldObject(S->GetPendingEquipmentGuid(),Item);
        return TEXT("Switching equipment: ")+Item.Name;
    }
    if(InventoryInteractionPending())return TEXT("Waiting for inventory transfer");
    if(PendingSpell)
    {
        FString Name;uint32 Icon=0;GetGameInstance()->GetSubsystem<UACEDatSubsystem>()->TryGetSpellInfo(PendingSpell,Name,Icon);
        if(Name.IsEmpty())Name=FString::Printf(TEXT("spell %d"),PendingSpell);
        return (PendingSpellActivity==TEXT("buffs")||PendingSpellActivity==TEXT("buff_others")?FString(TEXT("Buffing: ")):FString(TEXT("Casting: ")))
            +Name+TEXT("; awaiting result")+Elapsed(PendingSpellAt);
    }
    if(ConfirmedBuffAckAt&&DeferredBuffIntent)return TEXT("Waiting for final buff acknowledgment")+Elapsed(ConfirmedBuffAckAt);
    if(P->CombatTransitionIntent)return FString(P->WaitingAttackCancel?TEXT("Waiting for attack to finish"):TEXT("Waiting for combat stance"))+Elapsed(P->CombatTransitionStarted);
    if(C->IsUseBusy())return TEXT("Waiting for game action acknowledgment");
    if(auto* PC=Cast<AACEPlayerController>(GetGameInstance()->GetFirstLocalPlayerController()))
    {
        if(PC->IsWorldTransitionActive())return TEXT("Waiting for world transition");
        if(PC->IsUseApproachActive())return TEXT("Approaching interaction target");
    }
    return P->Status;
}

TSharedRef<SWidget> UACEPluginSubsystem::MakeUCMMicroPanel(const FString& Owner)
{
    FACEPluginStyle::Use(this,Owner);
    const TWeakObjectPtr<UACEPluginSubsystem> Host(this);
    const auto UCM=[Host,Owner]() -> TSharedPtr<FACEClientPlugin>
    {return Host.IsValid()?Host->CompactPolicy(Owner):nullptr;};
    auto Rows=SNew(SVerticalBox);
    const auto Row=[Rows](const TCHAR* Label,TFunction<bool()> Read,TFunction<void(bool)> Write)
    {
        Rows->AddSlot().AutoHeight().Padding(2,4)
        [SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsFocusable(false).ClickMethod(EButtonClickMethod::MouseDown)
            .IsChecked_Lambda([Read](){return Read()?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
            .OnCheckStateChanged_Lambda([Write](ECheckBoxState State){Write(State==ECheckBoxState::Checked);})
            [SNew(SACEPluginText).Text(FText::FromString(Label)).Font(FACEPluginStyle::NativeFont("Regular",15))]];
    };
    Rows->AddSlot().AutoHeight().Padding(2,4)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&FACEPluginStyle::Current()->Button)
        .IsFocusable(false)
        .OnClicked_Lambda([Host,UCM]()
        {
            if(Host.IsValid())
            {
                if(auto P=UCM())
                {
                    if(P->Running)Host->Stop(P->Id,TEXT("Stopped"),true);
                    else Host->Start(P->Id);
                }
            }
            return FReply::Handled();
        })
        [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",18))
            .Text_Lambda([UCM](){auto P=UCM();return FText::FromString(P&&P->Running?TEXT("Stop UCM"):TEXT("Start UCM"));})]];
    auto Quick=SNew(SHorizontalBox);
    const auto QuickButton=[&](const TCHAR* Label,const TCHAR* Help,TFunction<void()> Click,TAttribute<bool> Enabled)
    {
        Quick->AddSlot().AutoWidth().Padding(1)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::CompactButtonPadding)).ButtonStyle(&FACEPluginStyle::Current()->Button)
            .IsFocusable(false).ToolTip(FACEPluginStyle::Tooltip(FText::FromString(Help))).IsEnabled(Enabled)
            .OnClicked_Lambda([Click](){Click();return FReply::Handled();})
            [SNew(SACEPluginText).Text(FText::FromString(Label)).Font(FACEPluginStyle::NativeFont("Regular",14))]];
    };
    QuickButton(TEXT("F"),TEXT("Refresh enabled buffs now, even with UCM stopped."),[Host,Owner](){if(Host.IsValid())Host->RequestForceBuff(Owner);},true);
    QuickButton(TEXT("CF"),TEXT("Cancel Force Buff after the current cast."),[Host,Owner](){if(Host.IsValid())Host->CancelForceBuff(Owner);},
        TAttribute<bool>::CreateLambda([Host,UCM](){return Host.IsValid()&&Host->IsForceBuffRequested()&&Host->PolicyPlugin()==UCM();}));
    Quick->AddSlot().AutoWidth().Padding(3,0)[SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsFocusable(false)
        .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Follow the selected player while UCM runs."))))
        .IsChecked_Lambda([Host,Owner](){return Host.IsValid()&&Host->IsUCMFollowEnabled(Owner)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([Host,Owner](ECheckBoxState V){if(Host.IsValid())Host->SetUCMFollowSelected(V==ECheckBoxState::Checked,Owner);})
        [SNew(SACEPluginText).Text(FText::FromString(TEXT("FC"))).Font(FACEPluginStyle::NativeFont("Regular",14))]];
    Quick->AddSlot().AutoWidth().Padding(3,0)[SNew(SCheckBox).Style(&FACEPluginStyle::Current()->Check).IsFocusable(false)
        .ToolTip(FACEPluginStyle::Tooltip(FText::FromString(TEXT("Follow the player's footsteps around corners."))))
        .IsChecked_Lambda([UCM](){auto P=UCM();bool On=false;if(P)P->Profile->TryGetBoolField(TEXT("follow_corners"),On);return On?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
        .OnCheckStateChanged_Lambda([Host,UCM](ECheckBoxState V){if(auto P=UCM();P&&Host.IsValid()){P->Profile->SetBoolField(TEXT("follow_corners"),V==ECheckBoxState::Checked);Host->SaveCurrentProfile(P->Id,false);}})
        [SNew(SACEPluginText).Text(FText::FromString(TEXT("C"))).Font(FACEPluginStyle::NativeFont("Regular",14))]];
    Rows->AddSlot().AutoHeight().Padding(2,4)[Quick];
    // Text length must never change the Micro window/drag-bar dimensions.
    Rows->AddSlot().AutoHeight().Padding(2,4)[SNew(SBox).WidthOverride(1).HeightOverride_Lambda([](){return FOptionalSize(40.f*ACERuntimeOptions::PluginTextScale());}).Clipping(EWidgetClipping::ClipToBounds)
        .ToolTip(FACEPluginStyle::Tooltip(TAttribute<FText>::CreateLambda([Host,Owner](){return FText::FromString(Host.IsValid()?Host->UCMCurrentStatus(Owner):FString());})))
        [SNew(SVerticalBox)+SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).Text(FText::FromString(TEXT("Current status"))).Font(FACEPluginStyle::NativeFont("Regular",13))]
        +SVerticalBox::Slot().AutoHeight()[SNew(SACEPluginText).AutoWrapText(false).Font(FACEPluginStyle::NativeFont("Regular",14))
            .Text_Lambda([Host,Owner](){return FText::FromString(Host.IsValid()?Host->UCMCurrentStatus(Owner):FString());})]]];
    for(const auto& Entry:TArray<TPair<FString,FString>>{
        {TEXT("buffing"),TEXT("Buffing")},{TEXT("combat"),TEXT("Combat")},
        {TEXT("looting"),TEXT("Looting")},{TEXT("navigation"),TEXT("Navigation")},
        {TEXT("recovery"),TEXT("Vital recovery")},{TEXT("buff_others"),TEXT("Buff others")},
        {TEXT("summon_pets"),TEXT("Summoning")},
        {TEXT("vendor_restock"),TEXT("Vendor restocking")},{TEXT("meta_enabled"),TEXT("Metas")},{TEXT("idle_peace"),TEXT("Peace when idle")}})
    {
        const FString Key=Entry.Key;
        Row(*Entry.Value,[UCM,Key]()
        {
            auto P=UCM();if(!P)return false;
            if(Key==TEXT("combat")){FString Mode;P->Profile->TryGetStringField(Key,Mode);return !Mode.IsEmpty()&&Mode!=TEXT("off");}
            bool Value=Key==TEXT("recovery")||Key==TEXT("meta_enabled");P->Profile->TryGetBoolField(Key,Value);return Value;
        },[Host,UCM,Key](bool On)
        {
            if(!Host.IsValid())return;auto P=UCM();if(!P)return;
            if(Key==TEXT("combat")){P->Profile->SetStringField(Key,On?TEXT("auto"):TEXT("off"));P->Profile->SetBoolField(TEXT("manual_combat"),false);}
            else P->Profile->SetBoolField(Key,On);
            Host->SaveCurrentProfile(P->Id,false);
        });
    }
    Rows->AddSlot().AutoHeight().Padding(2,4)[SNew(SACEPluginButton).ContentPadding(FMargin(FACEPluginStyle::ButtonPadding)).ButtonStyle(&FACEPluginStyle::Current()->Button).IsFocusable(false)

        .OnClicked_Lambda([Host](){if(Host.IsValid())Host->TogglePluginWindow(TEXT("ucm.log"));return FReply::Handled();})
        [SNew(SACEPluginText).Font(FACEPluginStyle::NativeFont("Regular",15)).Text(FText::FromString(TEXT("Open UCM Log")))]];
    // The shared plugin window owns the background and its opacity.
    return SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("NoBrush")).Padding(10)
        [SNew(SScrollBox)+SScrollBox::Slot()[Rows]];
}
