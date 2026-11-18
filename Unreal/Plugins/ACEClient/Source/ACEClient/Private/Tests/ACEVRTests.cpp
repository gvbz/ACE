#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"
#include "VR/ACEVRMath.h"
#include "VR/ACEVRPointerVisuals.h"
#include "ACEEquipmentRules.h"
#include "ACERuntimeOptions.h"
#include "UI/ACEFrameRateWidget.h"
#include "Dat/ACEPortalViewMask.h"
#include "VR/ACEVRComponent.h"
#include "../VR/ACEEnemyHealthBar.h"
#include "../VR/ACEVRNativeHUD.h"
#include "../VR/ACEVRPlatformTextEntry.h"
#include "VR/ACEVRSettings.h"
#include "VR/ACEVRWidget.h"
#include "VR/ACEVRMenu.h"
#include "VR/ACEVRRetailSurface.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUICharGenBinder.h"
#include "UI/ACEUIFlow.h"
#include "UI/ACEUIGameplayBinder.h"
#include "UI/ACEUIElementManager.h"
#include "UI/ACEUILayoutResolver.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACERetailTextBlock.h"
#include "UI/ACEAppraisalFormatting.h"
#include "UObject/UObjectIterator.h"
#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/BoxComponent.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "ACEPlayerController.h"
#include "ACEWorldEntityActor.h"
#include "ACEClientSubsystem.h"
#include "ACECharacterAppearanceComponent.h"
#include "ACECombatStance.h"
#include "ACEDatSubsystem.h"
#include "ACELoginWidget.h"
#include "ACELoginSettings.h"
#include "ACELoadingScreenActor.h"
#include "ACESession.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/WidgetInteractionComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/EditableTextBox.h"
#include "Blueprint/WidgetTree.h"
#include "Features/IModularFeatures.h"
#include "XRMotionControllerBase.h"
#include "MotionControllerComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/InputSettings.h"
#include "Slate/WidgetRenderer.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/IConsoleManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include <limits>

namespace
{
	TSharedPtr<SWidget> FindVRMenuControl(const TSharedRef<SWidget>& Widget,FName Tag)
	{
		if(Widget->GetTag()==Tag)return Widget;
		FChildren* Children=Widget->GetChildren();
		for(int32 I=0;I<Children->Num();++I)
			if(auto Found=FindVRMenuControl(Children->GetChildAt(I),Tag))return Found;
		return nullptr;
	}
	class FVRTestControllers : public FXRMotionControllerBase
	{
	public:
		bool Connected = true;
		bool LeftTracked = true;
		FVRTestControllers() { IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), this); }
		~FVRTestControllers() { IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), this); }
		FName GetMotionControllerDeviceTypeName() const override { return TEXT("ACEVRTest"); }
		bool GetControllerOrientationAndPosition(int32 Index, FName Source, FRotator& Rotation, FVector& Position, float Scale) const override
		{
			if (GetControllerTrackingStatus(Index, Source) != ETrackingStatus::Tracked) return false;
			Rotation = FRotator::ZeroRotator;
			Position = FVector(20, Source.ToString().StartsWith(TEXT("Left")) ? -20 : 20, 130);
			return true;
		}
		ETrackingStatus GetControllerTrackingStatus(int32 Index, FName Source) const override
		{
			return Connected && Index == 0 && (LeftTracked || !Source.ToString().StartsWith(TEXT("Left"))) ? ETrackingStatus::Tracked : ETrackingStatus::NotTracked;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVRMathTest, "ACE.VR.GesturesAndSettings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVRMathTest::RunTest(const FString& Parameters)
{
	for(float Yaw:{-70.f,0.f,70.f})for(float Pitch:{-35.f,0.f,40.f})
	{
		const FTransform Pose(FRotator(Pitch,Yaw,23),FVector(25,-18,150),FVector(.7,.8,.9));
		const FTransform Mirror=ACEVRMath::MirrorPartPose(Pose);
		for(FVector Vertex:{FVector::ZeroVector,FVector(7,0,0),FVector(0,9,0),FVector(0,0,11)})
		{
			FVector Expected=Pose.TransformPosition(Vertex);Expected.X=-Expected.X;
			TestTrue(TEXT("Mirror reflects face/limb vertices, preserving side and height at every angle"),Mirror.TransformPosition(Vertex).Equals(Expected,.001));
		}
	}
	TStrongObjectPtr<UACEVRSettings> SavedPreferences(NewObject<UACEVRSettings>());
	ON_SCOPE_EXIT { SavedPreferences->Persist(); };
	if (auto* Scale=IConsoleManager::Get().FindConsoleVariable(TEXT("xr.SecondaryScreenPercentage.HMDRenderTarget")))
	{
		const float Saved=Scale->GetFloat(); const uint32 Priority=Scale->GetFlags() & ECVF_SetByMask;
		Scale->ClearFlags(ECVF_SetByMask);
		auto* Settings=NewObject<UACEVRSettings>(); Settings->RenderScale=80.f; Settings->ApplyRenderScale();
		TestEqual(TEXT("VR resolution applies to the actual OpenXR eye target"),Scale->GetFloat(),80.f);
		Settings->RenderScale=std::numeric_limits<float>::quiet_NaN(); Settings->ApplyRenderScale();
		TestEqual(TEXT("Invalid VR resolution restores a finite eye target"),Scale->GetFloat(),100.f);
		Scale->ClearFlags(ECVF_SetByMask); Scale->Set(Saved,static_cast<EConsoleVariableFlags>(Priority));
	}
	else AddError(TEXT("OpenXR HMD render-target resolution control is missing"));
	if (auto* Samples=IConsoleManager::Get().FindConsoleVariable(TEXT("r.MSAACount")))
	{
		const int32 Saved=Samples->GetInt(); const uint32 Priority=Samples->GetFlags() & ECVF_SetByMask;
		Samples->ClearFlags(ECVF_SetByMask);
		auto* S=NewObject<UACEVRSettings>(); S->MSAASamples=2; S->ApplyEdgeSmoothing();
		TestEqual(TEXT("VR sample setting only affects an MSAA renderer"), Samples->GetInt(), UACEVRSettings::SupportsMSAASettings() ? 2 : Saved);
		S->MSAASamples=8; S->Sanitize(); S->ApplyEdgeSmoothing();
		TestEqual(TEXT("Unsupported sample counts return to the Quest quality default"), S->MSAASamples, 4);
		Samples->ClearFlags(ECVF_SetByMask); Samples->Set(Saved,static_cast<EConsoleVariableFlags>(Priority));
	}
	ACEOutdoorPortalPlan::FAdmittedAperture Door;
	Door.WorldNormal=FVector::ForwardVector;
	Door.WorldVerts={FVector(100,-50,0),FVector(100,50,0),FVector(100,50,200),FVector(100,-50,200)};
	const auto Mask=FACEPortalViewMask::Build(FVector(0,0,100),{Door});
	TestTrue(TEXT("Left eye sees the terrain ray through its doorway edge"),Mask.ContainsFromEye(FVector(300,151,100),FVector(0,-3.2,100)));
	TestFalse(TEXT("Right eye keeps terrain behind its own doorway wall"),Mask.ContainsFromEye(FVector(300,151,100),FVector(0,3.2,100)));
	TestFalse(TEXT("Portal terrain never covers the room floor before the door"),Mask.ContainsFromEye(FVector(50,0,100),FVector(0,-3.2,100)));
	TestTrue(TEXT("A Touch stick at its measured gate reaches full movement speed"),FMath::IsNearlyEqual(ACEVRMath::DeadZone(FVector2D(0,.93),.2f,.9f).Y,1.));
	for (double X : {-.12,.12})
	{
		const FVector2D Input(X,.99), Output=ACEVRMath::AssistForward(Input,10);
		TestTrue(TEXT("Small forward deflection has no directional strafe bias"),FMath::IsNearlyZero(Output.X));
		TestTrue(TEXT("Forward assistance preserves stick magnitude"),FMath::IsNearlyEqual(Input.Size(),Output.Size()));
		TestTrue(TEXT("Forward assistance can be disabled"),ACEVRMath::AssistForward(Input,0).Equals(Input));
	}
	TestTrue(TEXT("Intentional diagonals remain diagonal"),ACEVRMath::AssistForward(FVector2D(.7,.7),10).Equals(FVector2D(.7,.7)));
	FACEWorldObject OneHand; OneHand.ValidLocations = ACEEquipMask::MeleeWeapon;
	TestTrue(TEXT("Retail one-handed weapon can use shield slot"),ACEEquipmentRules::CanWieldInSlot(OneHand,ACEEquipMask::Shield));
	OneHand.ValidLocations = ACEEquipMask::TwoHanded;
	TestFalse(TEXT("Two-handed weapon cannot become an offhand weapon"),ACEEquipmentRules::CanWieldInSlot(OneHand,ACEEquipMask::Shield));
	OneHand.ValidLocations = ACEEquipMask::Shield;
	TestTrue(TEXT("A shield can use its authored slot"),ACEEquipmentRules::CanWieldInSlot(OneHand,ACEEquipMask::Shield));
	for (const FRotator& Look : {FRotator(60,30,0),FRotator(-55,170,20),FRotator(0,0,45)})
	{
		const FTransform Head(Look,FVector(30,40,175)), Bind(FRotator(0,0,0),FVector(0,0,158));
		const FVector Eye(0,-8,17);
		const FTransform Body=ACEVRMath::BodyFromHead(Head,Bind,Eye,1.f);
		const FQuat Rotation=Head.GetRotation()*FRotator(0,-90,0).Quaternion()*Bind.GetRotation();
		TestTrue(TEXT("Looking up, down or rolling keeps the neck attached to the torso"),
			Body.TransformPosition(Bind.GetLocation()).Equals(Head.GetLocation()-Rotation.RotateVector(Eye),.001));
	}
	FACEWorldObject Arrow; Arrow.ItemType = ACEItemType::MissileWeapon; Arrow.CurrentWieldedLocation = ACEEquipMask::MissileAmmo;
	FACEWorldObject Sword; Sword.ItemType = ACEItemType::MeleeWeapon; Sword.CurrentWieldedLocation = ACEEquipMask::MeleeWeapon;
	FACEWorldObject Bow; Bow.ItemType = ACEItemType::MissileWeapon; Bow.CurrentWieldedLocation = ACEEquipMask::MissileWeapon;
	FACEWorldObject Wand; Wand.ItemType = ACEItemType::Caster; Wand.CurrentWieldedLocation = ACEEquipMask::Held;
	TestEqual(TEXT("Equipped retail arrows do not force missile mode with a sword"),
		ACECombatStance::ResolveEquippedMode({Arrow, Sword}), ACECombatMode::Melee);
	TestEqual(TEXT("Ammunition alone retains unarmed melee"), ACECombatStance::ResolveEquippedMode({Arrow}), ACECombatMode::Melee);
	TestEqual(TEXT("A bow and ammunition select missile mode"), ACECombatStance::ResolveEquippedMode({Arrow, Bow}), ACECombatMode::Missile);
	TestEqual(TEXT("A wand and remaining ammunition select magic"), ACECombatStance::ResolveEquippedMode({Arrow, Wand}), ACECombatMode::Magic);
	TestTrue(TEXT("Stick noise stays still"), ACEVRMath::DeadZone(FVector2D(.1, .1), .2f).IsNearlyZero());
	TestTrue(TEXT("Diagonal movement has unit maximum speed"), FMath::IsNearlyEqual(ACEVRMath::DeadZone(FVector2D(1, 1), .2f).Size(), 1.));
	TestTrue(TEXT("Nonfinite input cannot move"), ACEVRMath::DeadZone(FVector2D(std::numeric_limits<float>::quiet_NaN(), 1), .2f).IsNearlyZero());
	for (const FVector Hand : {FVector(20, 0, 0), FVector(200, 0, 0), FVector::ZeroVector, FVector(0, 0, 20)})
	{
		FVector Elbow, Wrist; ACEVRMath::SolveArm(FVector::ZeroVector, Hand, FVector(0, 0, -40), 30, 27, Elbow, Wrist);
		TestFalse(TEXT("Arm solve is finite"), Elbow.ContainsNaN() || Wrist.ContainsNaN());
		TestTrue(TEXT("Upper arm length preserved"), FMath::IsNearlyEqual(Elbow.Size(), 30., .001));
		TestTrue(TEXT("Forearm length preserved"), FMath::IsNearlyEqual(FVector::Distance(Elbow, Wrist), 27., .001));
	}
	ACEVRMath::FSwing Swing; float Speed = 0;
	{
		ACEVRMath::FSwing Pointing;
		Pointing.Elapsed = .12f; Pointing.Travel = 60.f;
		TestFalse(TEXT("Fast long-blade pointing with a still hand is not a strike"), Pointing.HasDeliberateHandMotion(FVector::ZeroVector, FVector(2, 0, 0)));
		Pointing.Elapsed = .25f;
		TestFalse(TEXT("Slowly reaching the sword toward a target is not a strike"), Pointing.HasDeliberateHandMotion(FVector::ZeroVector, FVector(20, 0, 0)));
		Pointing.Elapsed = .02f;
		TestFalse(TEXT("Brief tracking spikes cannot count as a sustained strike"), Pointing.HasDeliberateHandMotion(FVector::ZeroVector, FVector(30, 0, 0)));
		Pointing.Elapsed = .12f;
		TestTrue(TEXT("A sustained arm swing qualifies"), Pointing.HasDeliberateHandMotion(FVector::ZeroVector, FVector(30, 0, 0)));
		Pointing.Elapsed=.06f; Pointing.Travel=20.f;
		TestTrue(TEXT("A compact deliberate mace strike qualifies"),Pointing.HasDeliberateHandMotion(FVector::ZeroVector,FVector(12,0,0)));
	}
	TestFalse(TEXT("First tracked frame cannot hit"), Swing.Sample(FVector::ZeroVector, .02f, 140, true, Speed));
	int32 Hits = 0;
	for (int32 I = 1; I <= 30; ++I) if (Swing.Sample(FVector(I * 4, 0, 0), .02f, 140, true, Speed)) { ++Hits; Swing.Commit(); }
	TestEqual(TEXT("Continuous fast motion creates one swing"), Hits, 1);
	Swing.Sample(FVector(120, 0, 0), .02f, 140, true, Speed);
	TestFalse(TEXT("A single still frame cannot re-arm a strike"), Swing.bArmed);
	for (int32 I = 0; I < 8; ++I) Swing.Sample(FVector(120, 0, 0), .02f, 140, true, Speed);
	TestTrue(TEXT("Resting after a strike re-arms the next swing"), Swing.bArmed);
	Swing.Reset();
	for (int32 I = 0; I < 100; ++I)
		TestFalse(TEXT("Fast oscillating tracking jitter cannot accumulate a swing"), Swing.Sample(FVector(I % 2 ? 3 : 0, 0, 0), .01f, 140, true, Speed));
	TestFalse(TEXT("Tracking loss cancels swing"), Swing.Sample(FVector(120, 0, 0), .02f, 140, false, Speed));
	TestFalse(TEXT("Reacquired tracking cannot strike"), Swing.Sample(FVector(120, 0, 0), .02f, 140, true, Speed));
	TestFalse(TEXT("Tracking discontinuity cannot strike"), Swing.Sample(FVector(400, 0, 0), .02f, 140, true, Speed));
	float Draw = 0;
	TestTrue(TEXT("Pulling backward draws bow"), ACEVRMath::BowDraw(FVector::ZeroVector, FVector(-60, 0, 0), FVector::ForwardVector, 60, Draw));
	TestEqual(TEXT("Full draw fraction"), Draw, 1.f);
	TestTrue(TEXT("Support controller ray does not restrict two-hand bow aiming"), ACEVRMath::BowDraw(FVector::ZeroVector, FVector(60, 0, 0), FVector::UpVector, 60, Draw));
	TestTrue(TEXT("Diagonal physical draw is valid"), ACEVRMath::BowDraw(FVector::ZeroVector, FVector(-30, 40, 0), FVector::ForwardVector, 60, Draw));
	TestFalse(TEXT("An undrawn nock cannot fire"), ACEVRMath::BowDraw(FVector::ZeroVector, FVector(-5, 0, 0), FVector::ForwardVector, 60, Draw));
	TestFalse(TEXT("Unreachable tracking jump cannot draw"), ACEVRMath::BowDraw(FVector::ZeroVector, FVector(-200, 0, 0), FVector::ForwardVector, 60, Draw));
	TestTrue(TEXT("Small draw setting still permits a full physical arm span"),ACEVRMath::BowDraw(FVector::ZeroVector,FVector(-75,0,0),FVector::ForwardVector,30,Draw));
	TestEqual(TEXT("Overdraw stays at full power"),Draw,1.f);
	TestEqual(TEXT("Shift supports password symbols"), ACEVRMath::KeyboardCharacter('1', true), FString(TEXT("!")));
	TestEqual(TEXT("Shift supports backslash key"), ACEVRMath::KeyboardCharacter('\\', true), FString(TEXT("|")));
	TestEqual(TEXT("Shift supports capital letters"), ACEVRMath::KeyboardCharacter('a', true), FString(TEXT("A")));
	auto* Settings = NewObject<UACEVRSettings>(); Settings->SnapDegrees = std::numeric_limits<float>::quiet_NaN(); Settings->BowFullDraw = 0;
	Settings->MenuAnchorMode=-1; Settings->bPinMenuToView=true; Settings->Sanitize();
	TestEqual(TEXT("Existing head-pinned inventory migrates to Head"),Settings->MenuAnchorMode,0);
	Settings->MenuAnchorMode=-1; Settings->bPinMenuToView=false;
	Settings->PanelLayouts.Add("Menu",FTransform(FRotator(12,170,3),FVector(120,25,-15)));
	Settings->Sanitize();
	TestEqual(TEXT("Existing unpinned inventory migrates to World"),Settings->MenuAnchorMode,2);
	TestTrue(TEXT("Legacy saved inventory position survives migration"),Settings->MenuViewOffset.Equals(FVector(120,25,-15)));
	Settings->PanelLayouts.Reset(); Settings->MenuViewOffset=FVector(90,0,-10); Settings->MenuViewRotation=FRotator::ZeroRotator;
	Settings->Sanitize(); TestEqual(TEXT("Invalid snap resets safely"), Settings->SnapDegrees, 30.f); TestEqual(TEXT("Draw range bounded"), Settings->BowFullDraw, 30.f);
	Settings->MenuRecenterDegrees=std::numeric_limits<float>::quiet_NaN();Settings->Sanitize();
	TestEqual(TEXT("Invalid inventory recenter radius restores the default"),Settings->MenuRecenterDegrees,55.f);
	Settings->MenuRecenterDegrees=-1;Settings->Sanitize();
	TestEqual(TEXT("Inventory recenter radius retains a usable lower bound"),Settings->MenuRecenterDegrees,30.f);
	Settings->MenuRecenterDegrees=999;Settings->Sanitize();
	TestEqual(TEXT("Inventory recenter radius cannot prevent all body follow"),Settings->MenuRecenterDegrees,120.f);
	Settings->MovementDirection=-1;Settings->bHeadRelativeMovement=false;Settings->Sanitize();
	TestEqual(TEXT("Existing hand-relative preference migrates"),Settings->MovementDirection,1);
	Settings->MovementDirection=2;Settings->Sanitize();TestEqual(TEXT("Stick-only direction survives settings sanitation"),Settings->MovementDirection,2);
	Settings->SettingsVersion = 0; Settings->WristScale = .045f; Settings->Sanitize();
	TestEqual(TEXT("Existing default wrist size upgrades once"), Settings->WristScale, .06f);
	Settings->SettingsVersion = 0; Settings->WristScale = .08f; Settings->Sanitize();
	TestEqual(TEXT("Customized wrist size survives migration"), Settings->WristScale, .08f);
	Settings->SettingsVersion = 1; Settings->MovementScale = .65f; Settings->Sanitize();
	TestEqual(TEXT("Old reduced default upgrades to full character speed"), Settings->MovementScale, 1.f);
	Settings->SettingsVersion = 1; Settings->MovementScale = .4f; Settings->Sanitize();
	TestEqual(TEXT("Customized comfort speed survives migration"), Settings->MovementScale, .4f);
	Settings->VitalsViewOffset = FVector(std::numeric_limits<double>::infinity(), 0, 0); Settings->Sanitize();
	TestFalse(TEXT("Invalid saved vitals placement cannot break the view transform"), Settings->VitalsViewOffset.ContainsNaN());
	Settings->CompassViewOffset=FVector(std::numeric_limits<double>::infinity(),0,0); Settings->CompassScale=std::numeric_limits<float>::quiet_NaN();
	Settings->MenuViewOffset=Settings->CompassViewOffset; Settings->OptionsScale=100; Settings->CompassAnchorMode=99; Settings->Sanitize();
	TestTrue(TEXT("Invalid compass/menu preferences recover safely"),!Settings->CompassViewOffset.ContainsNaN() && !Settings->MenuViewOffset.ContainsNaN() && Settings->CompassScale==.065f && Settings->OptionsScale==.15f && Settings->CompassAnchorMode==2);
	const auto* Inputs = GetDefault<UInputSettings>();
	Settings->ButtonBindings.Reset();Settings->SetButtonAction("VRSelect","VRJump");
	TestEqual(TEXT("VR actions can move to another button"),Settings->GetButtonAction("VRSelect"),FName("VRJump"));
	TestEqual(TEXT("Rebinding preserves the displaced action on the old button"),Settings->GetButtonAction("VRJump"),FName("VRSelect"));
	Settings->SetButtonAction("VRSelect","Invalid");
	TestEqual(TEXT("Unknown bindings cannot discard an action"),Settings->GetButtonAction("VRSelect"),FName("VRJump"));
	Settings->ButtonBindings.Reset();Settings->Persist();
	Settings->MenuButtonBindings.Reset();
	TestEqual(TEXT("Native menu A defaults to use/equip"),Settings->GetMenuButtonAction("VRSelect"),FName("VRMenuUse"));
	TestEqual(TEXT("Native menu B defaults to inspect"),Settings->GetMenuButtonAction("VRPrevious"),FName("VRMenuInspect"));
	Settings->SetMenuButtonAction("VRSelect","VRMenuInspect");Settings->SetMenuButtonAction("VRPrevious",NAME_None);
	auto* ReloadedMenuSettings=NewObject<UACEVRSettings>();ReloadedMenuSettings->LoadConfig();
	TestEqual(TEXT("Native menu binding survives reload"),ReloadedMenuSettings->GetMenuButtonAction("VRSelect"),FName("VRMenuInspect"));
	TestEqual(TEXT("Normal-action override survives reload"),ReloadedMenuSettings->GetMenuButtonAction("VRPrevious"),NAME_None);
	Settings->MenuButtonBindings.Reset();Settings->Persist();
	const FVector Half=ACEVRMath::BallisticOffset(FVector::ForwardVector,24.f,.5f,1.f,100.f);
	const FVector Full=ACEVRMath::BallisticOffset(FVector::ForwardVector,24.f,1.f,1.f,100.f);
	TestTrue(TEXT("A full draw extends the trajectory with the server launch-speed curve"),FMath::IsNearlyEqual(Half.X,1620.,.01) && FMath::IsNearlyEqual(Full.X,2400.,.01));
	TestTrue(TEXT("Arrow preview follows gravity rather than a straight ray"),FMath::IsNearlyEqual(Full.Z,-490.,.01));
	for (const FName Name : {FName("VRInventory"), FName("VRCombat"), FName("VRSettings"), FName("VRSelect"), FName("VRLeftTrigger"), FName("VRRightTrigger"), FName("VRLeftGrip"), FName("VRRightGrip"), FName("VRJump")})
	{
		TArray<FInputActionKeyMapping> Keys; Inputs->GetActionMappingByName(Name, Keys);
		TestTrue(FString::Printf(TEXT("Touch mapping exists at XR startup: %s"), *Name.ToString()), Keys.Num() > 0 && Keys[0].Key.IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVRProtocolTest, "ACE.VR.Protocol", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVRProtocolTest::RunTest(const FString& Parameters)
{
	FACESession Session;
	Session.State = EACESessionState::InWorld; Session.PlayerGuid = 100;
	FACEWorldObject Wielder; Wielder.Guid=100;Session.WorldObjects.Add(100,Wielder);
	TestFalse(TEXT("No VR combat without capability ACK"), Session.SendVRCombat(1, 0x12340001, 200, 1, 0, FVector(0, 0, 1.5), FVector(0, 1, 0), 1, 0));
	auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	auto Address = Sockets->CreateInternetAddr(); bool Valid = false; Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
	auto* Receiver = Sockets->CreateSocket(NAME_DGram, TEXT("VR protocol fixture"), false);
	if (!Receiver || !Receiver->Bind(*Address)) { if (Receiver) Sockets->DestroySocket(Receiver); AddError(TEXT("Loopback socket unavailable")); return false; }
	Receiver->GetAddress(*Address); Session.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("VR protocol sender"), false);
	Session.ServerC2SAddr = Address; Session.IssacClient = MakeUnique<FACEIsaac>(123);
	FACEBinaryWriter Ack; Ack.WriteUInt32(100); Ack.WriteUInt32(1); Ack.WriteUInt32(0xF7D0); Ack.WriteUInt32(1); Ack.WriteUInt32(7);
	FACEBinaryReader AR(Ack.GetData()); Session.HandleGameEvent(AR);
	TestTrue(TEXT("v1 feature ACK enables combat"), Session.SupportsVRCombat());
	TestFalse(TEXT("Older servers do not receive unarmed extension attacks"), Session.SendVRCombat(2,0x12340001,0,0,300,FVector(0,0,1),FVector(.3,0,1),1,.1f));
	// The current server advertises world membership as an additional bit.
	// Capability negotiation must remain composable when optional features grow.
	FACEBinaryWriter CurrentAck; CurrentAck.WriteUInt32(100); CurrentAck.WriteUInt32(2); CurrentAck.WriteUInt32(0xF7D0);
	CurrentAck.WriteUInt32(1); CurrentAck.WriteUInt32(15); CurrentAck.WriteUInt32(12); CurrentAck.WriteUInt32(0);
	FACEBinaryReader CurrentAR(CurrentAck.GetData()); Session.HandleGameEvent(CurrentAR);
	TestTrue(TEXT("Current server combat plus snapshot ACK still enables combat"), Session.SupportsVRCombat());
	FACEBinaryWriter MissileAck;MissileAck.WriteUInt32(100);MissileAck.WriteUInt32(3);MissileAck.WriteUInt32(0xF7D0);
	MissileAck.WriteUInt32(1);MissileAck.WriteUInt32(55);MissileAck.WriteUInt32(0x80000EB2u);MissileAck.WriteFloat(24.f);
	FACEBinaryReader MissileAR(MissileAck.GetData());Session.HandleGameEvent(MissileAR);
	TestEqual(TEXT("Trajectory uses the speed negotiated for this launcher"),Session.GetVRMissileSpeed(static_cast<int32>(0x80000EB2u)),24.f);
	TestEqual(TEXT("A previous weapon's speed cannot be reused after swapping launchers"),Session.GetVRMissileSpeed(300),0.f);
	Session.CachedC2SPackets.Reset(); Session.TeleportSeq = 12;
	// Public child descriptions omit the wield mask. Repeated world snapshots
	// must preserve the private equipment profile for real high-bit GUIDs.
	FACEWorldObject Profile; Profile.Guid = static_cast<int32>(0x80000EB2u); Profile.WielderId = 100;
	Profile.CurrentWieldedLocation = ACEEquipMask::MissileWeapon;
	Session.UpsertWorldObject(Profile);
	FACEWorldObject PublicBow; PublicBow.Guid = Profile.Guid; PublicBow.ParentGuid = 100; PublicBow.ParentLocation = 1;
	PublicBow.ItemType = ACEItemType::MissileWeapon; PublicBow.SetupId = 0x0200158D;
	Session.UpsertWorldObject(PublicBow);
	TArray<FACEWorldObject> Equipped; Session.GetEquippedItems(Equipped);
	TestTrue(TEXT("Public attached-object refresh retains bow ownership and wield slot"), Equipped.ContainsByPredicate([&](const auto& O)
		{ return O.Guid == Profile.Guid && O.CurrentWieldedLocation == ACEEquipMask::MissileWeapon; }));
	PublicBow.ContainerId = 100; PublicBow.ParentGuid = PublicBow.ParentLocation = 0;
	Session.UpsertWorldObject(PublicBow); Session.GetEquippedItems(Equipped);
	TestFalse(TEXT("A real dequip still clears the wield slot"), Equipped.ContainsByPredicate([&](const auto& O) { return O.Guid == Profile.Guid; }));
	Session.WorldObjects.Remove(Profile.Guid);
	for(int64 WieldSlot:{ACEEquipMask::MissileAmmo,ACEEquipMask::MissileWeapon})
	{
	FACEWorldObject Arrows; Arrows.Guid = static_cast<int32>(0x80000EB4u); Arrows.WielderId = Arrows.ParentGuid = 100;
	Arrows.CurrentWieldedLocation = WieldSlot; Arrows.StackSize = 3; Arrows.MaxStackSize = 2500;
	Arrows.ParentGuid = 0;
	Session.WorldObjects.Add(Arrows.Guid, Arrows);
	FACEBinaryWriter EquipArrows; EquipArrows.WriteUInt32(uint32(Arrows.Guid)); EquipArrows.WriteUInt32(uint32(WieldSlot));
	FACEBinaryReader EquipArrowInput(EquipArrows.GetData()); Session.HandleWieldItem(EquipArrowInput);
	if(WieldSlot==ACEEquipMask::MissileAmmo) TestEqual(TEXT("Equipping arrows does not invent a quiver attachment before server reload"), Session.WorldObjects[Arrows.Guid].ParentGuid, 0);
	TestEqual(TEXT("Unloaded ammunition or thrown stack remains equipped"), Session.WorldObjects[Arrows.Guid].CurrentWieldedLocation, WieldSlot);
	for (int32 Remaining : {2, 1, 0})
	{
		FACEBinaryWriter Pickup; Pickup.WriteUInt32(uint32(Arrows.Guid)); Pickup.WriteUInt16(1); Pickup.WriteUInt16(7 - Remaining * 2);
		FACEBinaryReader PickupInput(Pickup.GetData()); Session.HandlePickupEvent(PickupInput);
		TestEqual(TEXT("Launch detaches only the missile visual, preserving the equipped stack"), Session.WorldObjects[Arrows.Guid].CurrentWieldedLocation, WieldSlot);
		TestEqual(TEXT("Launched stack is never falsely moved into the backpack"), Session.WorldObjects[Arrows.Guid].ContainerId, 0);
		if (Remaining)
		{
			FACEBinaryWriter Count; Count.WriteUInt8(1); Count.WriteUInt32(uint32(Arrows.Guid)); Count.WriteUInt32(Remaining); Count.WriteUInt32(Remaining);
			FACEBinaryReader CountInput(Count.GetData()); Session.HandleSetStackSize(CountInput);
			FACEBinaryWriter Parent; Parent.WriteUInt32(100); Parent.WriteUInt32(uint32(Arrows.Guid)); Parent.WriteUInt32(1); Parent.WriteUInt32(1);
			Parent.WriteUInt16(1); Parent.WriteUInt16(8 - Remaining * 2);
			TestEqual(TEXT("Server reload attachment body is exactly twenty bytes"), Parent.GetData().Num(), 20);
			FACEBinaryReader ParentInput(Parent.GetData()); Session.HandleParentEvent(ParentInput);
			TestEqual(TEXT("Reload reattaches the remaining arrows"), Session.WorldObjects[Arrows.Guid].ParentGuid, 100);
			Session.GetEquippedItems(Equipped);
			TestTrue(TEXT("A second and third draw can resolve the remaining stack"), Equipped.ContainsByPredicate([&](const auto& O) { return O.Guid == Arrows.Guid && O.StackSize == Remaining; }));
		}
		else
		{
			// ACE's actual ConsumeItem path, shared by desktop and VR.
			FACEBinaryWriter Owner; Owner.WriteUInt8(1); Owner.WriteUInt32(uint32(Arrows.Guid)); Owner.WriteUInt32(3); Owner.WriteUInt32(0);
			FACEBinaryReader OR(Owner.GetData()); Session.HandlePublicUpdateInstanceId(OR);
			FACEBinaryWriter Location; Location.WriteUInt8(1); Location.WriteUInt32(uint32(Arrows.Guid)); Location.WriteUInt32(10); Location.WriteUInt32(0);
			FACEBinaryReader LR(Location.GetData()); Session.HandlePublicUpdatePropertyInt(LR);
			FACEBinaryWriter Remove; Remove.WriteUInt32(uint32(Arrows.Guid)); Remove.WriteUInt16(1); Remove.Align();
			FACEBinaryReader RemoveInput(Remove.GetData()); Session.HandleObjectDelete(RemoveInput);
			TestFalse(TEXT("Only consuming the final arrow removes its stack"), Session.WorldObjects.Contains(Arrows.Guid));
		}
	}
	}
	TestTrue(TEXT("Aimed spell sends reliable action"), Session.SendVRCombat(1, 0x12340001, 200, 42, 300, FVector(.2, .3, 1.5), FVector(0, 1, 0), 1, .25));
	uint32 Sequence = 0; for (const auto& Pair : Session.CachedC2SPackets) Sequence = FMath::Max(Sequence, Pair.Key);
	if (TestTrue(TEXT("Action has packet"), Sequence != 0))
	{
		FACEBinaryReader Wire(Session.CachedC2SPackets[Sequence].Payload); Wire.Skip(16);
		TestEqual(TEXT("Authenticated game-action envelope"), Wire.ReadUInt32(), ACEOpcode::GameAction); Wire.ReadUInt32();
		TestEqual(TEXT("Extension opcode"), Wire.ReadUInt32(), 0xF7D0u);
		TestEqual(TEXT("Server v1 payload byte count"), Wire.Remaining(), 64);
		TestEqual(TEXT("Version"), Wire.ReadUInt32(), 1u); TestEqual(TEXT("Kind"), Wire.ReadUInt32(), 1u);
		TestEqual(TEXT("Replay sequence"), Wire.ReadUInt32(), 1u); TestEqual(TEXT("Cell"), Wire.ReadUInt32(), 0x12340001u);
		TestEqual(TEXT("Teleport epoch"), Wire.ReadUInt32(), 12u); TestEqual(TEXT("Weapon"), Wire.ReadUInt32(), 200u);
		TestEqual(TEXT("Spell"), Wire.ReadUInt32(), 42u); TestEqual(TEXT("Target"), Wire.ReadUInt32(), 300u);
		for (float Expected : {.2f, .3f, 1.5f, 0.f, 1.f, 0.f, 1.f, .25f}) TestEqual(TEXT("Aim/cost payload float"), Wire.ReadFloat(), Expected);
	}
	FACEBinaryWriter PunchAck; PunchAck.WriteUInt32(100); PunchAck.WriteUInt32(4); PunchAck.WriteUInt32(0xF7D0);
	for (bool Contact : {false,true})
	{
		Session.SetReportedContact(Contact);Session.CachedC2SPackets.Reset();
		Session.PlayerPosition.CellId=0x12340001;Session.PlayerPosition.Location=FVector(10,20,Contact?0:2);
		Session.SendVRCombat(1,0x12340001,200,42,300,FVector(0,0,1),FVector(0,1,0),1,0);
		bool FoundPosition=false;uint32 PositionSequence=0,ActionSequence=0;
		for(const auto& Packet:Session.CachedC2SPackets)
		{
			FACEBinaryReader R(Packet.Value.Payload);R.Skip(24);const uint32 Action=R.ReadUInt32();
			if(Action==ACEGameAction::AutonomousPosition)
			{
				FoundPosition=true;PositionSequence=Packet.Key;R.Skip(12);
				TestEqual(TEXT("Cast carries current feet height"),R.ReadFloat(),Contact?0.f:2.f);
				R.Skip(24);TestEqual(TEXT("Cast preserves real grounded/airborne contact"),R.ReadUInt8(),uint8(Contact?1:0));
			}
			else if(Action==0xF7D0)ActionSequence=Packet.Key;
		}
		TestTrue(TEXT("Position is sent before the trigger action"),FoundPosition && PositionSequence<ActionSequence);
	}
	PunchAck.WriteUInt32(1); PunchAck.WriteUInt32(135);
	FACEBinaryReader PunchAR(PunchAck.GetData()); Session.HandleGameEvent(PunchAR);
	TestTrue(TEXT("Optional unarmed capability enables empty-hand strikes"), Session.SupportsVRUnarmed());
	const uint32 BeforePowerCaps=Session.VRCapabilities;
	for (bool Supported : {false,true})
	{
		Session.VRCapabilities=BeforePowerCaps | (Supported ? 65536u : 0u);
		TestTrue(TEXT("Tracked missile accepts a separate accuracy setting"),Session.SendVRCombat(3,0x12340001,200,0,300,FVector(0,0,1),FVector(0,1,0),1,0,.25f));
		uint32 Last=0;for(const auto& Pair:Session.CachedC2SPackets)Last=FMath::Max(Last,Pair.Key);
		FACEBinaryReader Packet(Session.CachedC2SPackets[Last].Payload);Packet.Skip(28);
		TestEqual(TEXT("Power trailer is sent only to capable servers"),Packet.Remaining(),Supported?68:64);
		Packet.Skip(56);TestEqual(TEXT("Physical release strength remains independent"),Packet.ReadFloat(),1.f);Packet.ReadFloat();
		if(Supported)TestEqual(TEXT("Selected accuracy arrives in the trailer"),Packet.ReadFloat(),.25f);
	}
	Session.VRCapabilities=BeforePowerCaps;
	FACEPosition InteriorCoords;InteriorCoords.CellId=0xDA740102;InteriorCoords.Location=FVector(179,59,6);
	FACEPosition ExteriorCoords=InteriorCoords;ExteriorCoords.CellId=0xDA740001;
	TestEqual(TEXT("A building doorway keeps geographic coordinates"),UACEUIGameplayBinder::FormatMapCoords(InteriorCoords,true),UACEUIGameplayBinder::FormatMapCoords(ExteriorCoords));
	TestFalse(TEXT("VR indoor coordinates are not a cell identifier"),UACEUIGameplayBinder::FormatMapCoords(InteriorCoords,true).Contains(TEXT("Cell")));
	InteriorCoords.Location.X+=24;
	TestNotEqual(TEXT("Indoor coordinates track movement within a cell"),UACEUIGameplayBinder::FormatMapCoords(InteriorCoords,true),UACEUIGameplayBinder::FormatMapCoords(ExteriorCoords));
	TestTrue(TEXT("Invalid position has no fabricated coordinates"),UACEUIGameplayBinder::FormatMapCoords(FACEPosition(),true).IsEmpty());
	TestTrue(TEXT("Empty offhand can send a tracked punch"),Session.SendVRCombat(2,0x12340001,0,1,300,FVector(0,0,1),FVector(.3,0,1),1,.1f));
	TestFalse(TEXT("Unarmed capability never permits a weaponless spell"),Session.SendVRCombat(1,0x12340001,0,1,300,FVector(0,0,1),FVector(0,1,0),1,0));
	TestFalse(TEXT("Unknown punch hand is rejected"),Session.SendVRCombat(2,0x12340001,0,2,300,FVector(0,0,1),FVector(.3,0,1),1,.1f));
	FACEWorldObject Self; Self.Guid = 100; Session.WorldObjects.Add(100, Self);
	FACEWorldObject Bag; Bag.Guid = 201; Bag.ContainerId = 100; Session.WorldObjects.Add(201, Bag);
	FACEWorldObject Item; Item.Guid = 202; Item.ContainerId = 201; Session.WorldObjects.Add(202, Item);
	FACEWorldObject Worn; Worn.Guid = 203; Worn.WielderId = 100; Session.WorldObjects.Add(203, Worn);
	FACEWorldObject Stock; Stock.Guid = 204; Stock.ContainerId = 900; Session.WorldObjects.Add(204, Stock);
	FACEWorldObject Live; Live.Guid = 300; Session.WorldObjects.Add(300, Live);
	FACEWorldObject Old; Old.Guid = 301; Session.WorldObjects.Add(301, Old);
	Session.SelectObject(301);
	TArray<int32> Deleted; Session.OnObjectDeleted.AddLambda([&](int32 Guid) { Deleted.Add(Guid); });
	for (uint32 Epoch : {11u, 12u})
	{
		FACEBinaryWriter Snapshot; Snapshot.WriteUInt32(Epoch); Snapshot.WriteUInt32(1); Snapshot.WriteUInt32(300);
		FACEBinaryReader Input(Snapshot.GetData()); Session.ApplyVRWorldSnapshot(Input);
		TestEqual(TEXT("Only the current teleport epoch can remove a stale NPC"), Session.WorldObjects.Contains(301), Epoch != 12u);
	}
	TestTrue(TEXT("Authoritative membership preserves the live NPC"), Session.WorldObjects.Contains(300));
	TestEqual(TEXT("Snapshot removal clears an old area's selected enemy"),Session.GetSelectedObject().Guid,0);
	const auto SavedPlayerPosition=Session.PlayerPosition;
	Session.PlayerPosition.CellId=0x7D640001;Session.PlayerPosition.Location=FVector(44,94,12);
	Session.WorldObjects[300].bHasPosition=true;Session.WorldObjects[300].Position=Session.PlayerPosition;
	TestTrue(TEXT("A nearby creature can send health feedback"),Session.IsNearbyHealthObject(300));
	Session.PlayerPosition.CellId=0x00640001;
	TestFalse(TEXT("Retaining a cached creature after distant teleport cannot show its health"),Session.IsNearbyHealthObject(300));
	Session.PlayerPosition=SavedPlayerPosition;
	for (int32 Guid : {100, 201, 202, 203, 204}) TestTrue(TEXT("Snapshot preserves self, nested inventory, equipment, and vendor stock"), Session.WorldObjects.Contains(Guid));
	TestEqual(TEXT("Stale NPC deletion reaches the world presenter once"), Deleted.Num(), 1);
	FACEBinaryWriter Truncated; Truncated.WriteUInt32(12); Truncated.WriteUInt32(2); Truncated.WriteUInt32(100);
	FACEBinaryReader BadSnapshot(Truncated.GetData()); Session.ApplyVRWorldSnapshot(BadSnapshot);
	TestTrue(TEXT("Truncated membership cannot erase live NPCs"), Session.WorldObjects.Contains(300));
	Session.WorldObjects.Add(301,Old);
	FACEBinaryWriter FullAck;for(uint32 V:{100u,5u,0xF7D0u,1u,511u,12u,1u,300u,200u})FullAck.WriteUInt32(V);FullAck.WriteFloat(24.f);
	FACEBinaryReader FullAR(FullAck.GetData());Session.HandleGameEvent(FullAR);
	TestFalse(TEXT("Membership consumes exactly its GUIDs before the missile profile trailer"),Session.WorldObjects.Contains(301));
	TestEqual(TEXT("Membership leaves the missile speed readable"),Session.GetVRMissileSpeed(200),24.f);
	FACEBinaryWriter ProfilePacket;for(uint32 V:{100u,6u,0xF7D3u,2717u})ProfilePacket.WriteUInt32(V);ProfilePacket.WriteFloat(30.f);ProfilePacket.WriteUInt32(1);
	FACEBinaryReader ProfileReader(ProfilePacket.GetData());Session.HandleGameEvent(ProfileReader);
	float ArcSpeed=0;bool Gravity=false;
	TestTrue(TEXT("Negotiated arc preview reads exact authoritative speed and gravity"),Session.GetVRSpellProfile(2717,ArcSpeed,Gravity) && ArcSpeed==30.f && Gravity);
	FACEBinaryWriter RadiusPacket;for(uint32 V:{100u,7u,0xF7D3u,2717u})RadiusPacket.WriteUInt32(V);
	RadiusPacket.WriteFloat(30.f);RadiusPacket.WriteUInt32(1);RadiusPacket.WriteFloat(.12f);
	FACEBinaryReader RadiusReader(RadiusPacket.GetData());Session.HandleGameEvent(RadiusReader);float ArcRadius=0;
	TestTrue(TEXT("Arc preview reads authoritative projectile radius when supplied"),Session.GetVRSpellProfile(2717,ArcSpeed,Gravity,&ArcRadius) && FMath::IsNearlyEqual(ArcRadius,.12f));
	Session.VRCapabilities |= 2048u;
	auto Recovery = [&](uint32 Seq, uint32 Epoch, float Remaining, float Duration)
	{
		FACEBinaryWriter W; W.WriteUInt32(Seq); W.WriteUInt32(Epoch); W.WriteFloat(Remaining); W.WriteFloat(Duration);
		FACEBinaryReader R(W.GetData()); Session.HandleVRRecovery(R);
	};
	Recovery(Session.VRSequence,12,1.f,2.f);
	TestTrue(TEXT("Server recovery gives a live countdown and progress"),Session.GetVRRecoveryRemaining()>.9f && FMath::IsNearlyEqual(Session.GetVRRecoveryProgress(),.5f,.05f));
	const double ReadyAt=Session.VRRecoveryReadyAt;
	Recovery(Session.VRSequence,12,10.f,10.f);
	TestEqual(TEXT("Duplicate recovery cannot restart the countdown"),Session.VRRecoveryReadyAt,ReadyAt);
	++Session.VRSequence;
	Recovery(Session.VRSequence,11,1.f,2.f);
	Recovery(Session.VRSequence,12,3.f,2.f);
	Recovery(Session.VRSequence,12,NAN,2.f);
	Recovery(Session.VRSequence+1,12,1.f,2.f);
	TestEqual(TEXT("Wrong epoch, invalid times and unsolicited sequence cannot change recovery"),Session.VRRecoveryReadyAt,ReadyAt);
	++Session.TeleportSeq;
	TestEqual(TEXT("A portal immediately invalidates recovery"),Session.GetVRRecoveryRemaining(),0.f);
	--Session.TeleportSeq;
	Session.VRRecoveryReadyAt=FPlatformTime::Seconds()-.01;
	TestEqual(TEXT("Expired recovery is ready"),Session.GetVRRecoveryProgress(),1.f);
	Session.VRCapabilities |= 4096u | 16384u;
	const uint32 CastSequence=++Session.VRSequence;
	auto Casting=[&](uint32 Seq,uint32 Epoch,uint32 Phase,float Remaining,float Duration) {
		FACEBinaryWriter W;for(uint32 V:{Seq,Epoch,42u,Phase}) W.WriteUInt32(V);W.WriteFloat(Remaining);W.WriteFloat(Duration);
		FACEBinaryReader R(W.GetData());Session.HandleVRCasting(R);
	};
	Casting(CastSequence,12,1,1,2);
	TestTrue(TEXT("Cast windup reports server remaining time and progress"),Session.GetVRCastPhase()==1 && Session.GetVRCastRemaining()>.9f && FMath::IsNearlyEqual(Session.GetVRCastProgress(),.5f,.05f));
	const double CastReady=Session.VRCastReadyAt;
	Casting(CastSequence,12,1,2,2);Casting(CastSequence+1,12,1,2,2);Casting(CastSequence,11,0,0,0);
	TestEqual(TEXT("Duplicate, future and old-world cast messages cannot restart or cancel it"),Session.VRCastReadyAt,CastReady);
	Casting(CastSequence,12,2,0,0);TestEqual(TEXT("Release moves timer into recovery"),Session.GetVRCastPhase(),2u);
	Casting(CastSequence,12,0,0,0);TestEqual(TEXT("Completion makes casting ready"),Session.GetVRCastPhase(),0u);
	Casting(CastSequence,12,1,1,2);TestEqual(TEXT("A late windup cannot resurrect a completed cast"),Session.GetVRCastPhase(),0u);
	Session.ClearWorldState(); TestFalse(TEXT("Logout clears capabilities"), Session.SupportsVRCombat());
	TestEqual(TEXT("Logout clears the cast state"),Session.VRCastSequence,0u);
	TestEqual(TEXT("Logout clears the old countdown sequence"),Session.VRRecoverySequence,0u);
	TestFalse(TEXT("Logout clears the previous server's spell profiles"),Session.GetVRSpellProfile(2717,ArcSpeed,Gravity));
	Session.State = EACESessionState::Disconnected; Session.Disconnect(); Sockets->DestroySocket(Receiver);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVRRigTest, "ACE.VR.RigAndMenus", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVRRigTest::RunTest(const FString& Parameters)
{
	FString RetailDatDirectory=TEXT("C:/Turbine/Asheron's Call");
	FParse::Value(FCommandLine::Get(),TEXT("RetailDatDir="),RetailDatDirectory);
	TStrongObjectPtr<UACEVRSettings> SavedPreferences(NewObject<UACEVRSettings>());
	ON_SCOPE_EXIT { SavedPreferences->Persist(); };
	const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
		.CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
	auto* GI = NewObject<UGameInstance>(GEngine); GI->InitializeStandalone();
	auto* DummyWorld = GI->GetWorld();
	GI->GetWorldContext()->SetCurrentWorld(World); World->SetGameInstance(GI); DummyWorld->DestroyWorld(false);
	auto* PC = World->SpawnActor<AACEPlayerController>(); auto* Pawn = World->SpawnActor<APawn>();
	PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));
	World->AddController(PC);
	auto* Capsule = NewObject<UCapsuleComponent>(Pawn); Capsule->InitCapsuleSize(34, 96); Pawn->SetRootComponent(Capsule); Capsule->RegisterComponent();
	auto* Camera = NewObject<UCameraComponent>(Pawn); Camera->SetupAttachment(Capsule); Camera->RegisterComponent();
	PC->Possess(Pawn);
	auto* VR = NewObject<UACEVRComponent>(Pawn); Pawn->AddInstanceComponent(VR); VR->RegisterComponent();
	VR->PC = PC; VR->Client = GI->GetSubsystem<UACEClientSubsystem>(); PC->Client = VR->Client;
	VR->Settings = NewObject<UACEVRSettings>(); VR->ActivateRig();
	TestTrue(TEXT("Rig activated"), VR->IsActive()); TestFalse(TEXT("Desktop camera disabled"), Camera->IsActive());
	// This fixture drives render ticks without advancing World time.
	for (auto* Panel : {VR->RetailPanel.Get(), VR->SettingsPanel.Get(), VR->WristPanel.Get(), VR->VitalsPanel.Get(),
		VR->ChatPanel.Get(), VR->JumpPanel.Get(), VR->KeyboardPanel.Get(), VR->FocusPanel.Get()}) Panel->SetRedrawTime(0);
	TestTrue(TEXT("Head tracking owns camera"), VR->Head->bLockToHmd && VR->Head->IsActive());
	TestTrue(TEXT("Tracking origin ignores body rotation and translation"), VR->TrackingOrigin->IsUsingAbsoluteRotation() && VR->TrackingOrigin->IsUsingAbsoluteLocation());
	TestTrue(TEXT("Wrist smoothing is outside the controller late-update hierarchy"), VR->WristPanel->GetAttachParent() != VR->LeftGrip);
	TestTrue(TEXT("Rig updates before camera evaluation"), VR->PrimaryComponentTick.TickGroup == TG_PostPhysics);
	// Reproduce reconstruction of the same login widget during the viewport-to-VR handoff.
	auto* Login = CreateWidget<UACELoginWidget>(PC);
	Login->ProfilePath=FPaths::ProjectSavedDir()/TEXT("Automation/VR/Login-")+FGuid::NewGuid().ToString()+TEXT(".dat");
	FACELoginSettings LoginFixture; LoginFixture.Host=TEXT("localhost"); LoginFixture.Account=TEXT("VR fixture"); LoginFixture.Password=TEXT("fixture-only");
	ACELoginSettings::Save(LoginFixture,Login->ProfilePath);
	{
		auto SlateLogin = Login->TakeWidget();
		Login->NativeConstruct();
		auto LoginBindings = [&](const auto& Delegate)
		{
			return Delegate.GetAllObjects().FilterByPredicate([&](UObject* Object) { return Object == Login; }).Num();
		};
		TestEqual(TEXT("Reconstructed login has one session listener"), LoginBindings(VR->Client->OnSessionStateChanged), 1);
		TestEqual(TEXT("Reconstructed login has one character listener"), LoginBindings(VR->Client->OnCharacterList), 1);
		TestEqual(TEXT("Reconstructed login has one log listener"), LoginBindings(VR->Client->OnLogMessage), 1);
		Login->NativeDestruct();
		TestEqual(TEXT("Detached login releases its session listener"), LoginBindings(VR->Client->OnSessionStateChanged), 0);
		TestEqual(TEXT("Detached login releases its character listener"), LoginBindings(VR->Client->OnCharacterList), 0);
		TestEqual(TEXT("Detached login releases its log listener"), LoginBindings(VR->Client->OnLogMessage), 0);
		Login->NativeConstruct();
		TestEqual(TEXT("Login reattaches after moving into VR"), LoginBindings(VR->Client->OnSessionStateChanged), 1);
	}
	PC->LoginWidget = Login;
	Pawn->SetActorEnableCollision(false); Pawn->SetActorHiddenInGame(true);
	{
		FVRTestControllers Controllers;
		auto PollHands = [&]()
		{
			for (auto* Hand : {VR->LeftGrip.Get(), VR->RightGrip.Get(), VR->LeftAim.Get(), VR->RightAim.Get()})
				Hand->TickComponent(.016f, LEVELTICK_All, nullptr);
		};
		// Start with no head pose, then put on the headset several frames later.
		VR->UpdatePanels();
		const FVector StartupPanel = VR->RetailPanel->GetComponentLocation();
		VR->Head->SetRelativeLocationAndRotation(FVector(35, -12, 165), FRotator(0, 35, 0));
		PollHands(); VR->UpdateTrackingState(true); VR->UpdatePanels(); VR->UpdateArms();
		VR->Settings->ButtonBindings.Reset();
		// Explicit grab binding: the default left grip now toggles native chat.
		VR->Settings->ButtonBindings.Add("VRLeftGrip","VRLeftGrip");
		VR->RouteControllerButton("VRLeftGrip",true);
		TestTrue(TEXT("Physical button dispatch reaches its held action"),VR->bLeftGripHeld);
		VR->ConfigureButton("VRLeftGrip","VRRightGrip");VR->RouteControllerButton("VRLeftGrip",false);
		TestFalse(TEXT("Rebinding while held still releases the original action"),VR->bLeftGripHeld || VR->bRightGripHeld);
		VR->RouteControllerButton("VRLeftGrip",true);
		TestTrue(TEXT("The next press uses the new action immediately"),VR->bRightGripHeld && !VR->bLeftGripHeld);
		VR->UpdateTrackingState(false);
		TestTrue(TEXT("Tracking loss clears held button routes"),VR->HeldControllerActions.IsEmpty());
		VR->ResetButtonBindings();VR->UpdateTrackingState(true);VR->UpdatePanels();
		const float SavedFPS=ACERuntimeOptions::Get(TEXT("ShowFrameRate"));
		const auto SavedFPSState=VR->Client->Session->State;
		ACERuntimeOptions::Set(TEXT("ShowFrameRate"),1);
		for (auto State : {EACESessionState::Disconnected,EACESessionState::Connecting,
			EACESessionState::AwaitConnectRequest,EACESessionState::AwaitCharacterList,
			EACESessionState::CharacterSelect,EACESessionState::EnteringWorld,EACESessionState::Failed})
		{
			VR->Client->Session->State=State;PC->UpdateFrameRateOverlay();VR->UpdatePanels();
			TestNull(TEXT("Saved FPS toggle does not create a counter outside gameplay"),PC->FrameRateWidget.Get());
			TestNull(TEXT("Login and character screens do not create a VR FPS surface"),VR->FrameRatePanel.Get());
		}
		VR->Client->Session->State=EACESessionState::InWorld;PC->UpdateFrameRateOverlay();VR->UpdatePanels();
		TestNotNull(TEXT("Entering gameplay with FPS enabled creates its VR surface"),VR->FrameRatePanel.Get());
		if(VR->FrameRatePanel)
		{
			TestTrue(TEXT("FPS surface is visible and follows tracked head pose"),VR->FrameRatePanel->IsVisible() && VR->FrameRatePanel->GetAttachParent()==VR->Head);
			TestEqual(TEXT("FPS surface cannot intercept a pointer"),VR->FrameRatePanel->GetCollisionEnabled(),ECollisionEnabled::NoCollision);
			auto* SavedCounter=PC->FrameRateWidget.Get();
			VR->Client->Session->State=EACESessionState::CharacterSelect;VR->UpdatePanels();
			TestFalse(TEXT("Logout hides and stops the VR surface even before the controller updates"),VR->FrameRatePanel->IsVisible() || VR->FrameRatePanel->IsComponentTickEnabled());
			PC->UpdateFrameRateOverlay();
			TestTrue(TEXT("Returning to character selection collapses the shared counter"),SavedCounter->GetVisibility()==ESlateVisibility::Collapsed);
			TestEqual(TEXT("Leaving gameplay resets the frame sample"),SavedCounter->GetCounterText(),FString(TEXT("FPS --")));
			TestEqual(TEXT("Logout preserves the user's FPS preference"),ACERuntimeOptions::Get(TEXT("ShowFrameRate")),1.f);
			VR->Client->Session->State=EACESessionState::InWorld;PC->UpdateFrameRateOverlay();VR->UpdatePanels();
			TestTrue(TEXT("Reentering gameplay reuses the counter and its saved placement"),PC->FrameRateWidget==SavedCounter && VR->FrameRatePanel->IsVisible());
			ACERuntimeOptions::Set(TEXT("ShowFrameRate"),0);PC->UpdateFrameRateOverlay();VR->UpdatePanels();
			TestFalse(TEXT("Disabling FPS stops drawing and ticking its surface"),VR->FrameRatePanel->IsVisible() || VR->FrameRatePanel->IsComponentTickEnabled());
		}
		VR->Client->Session->State=SavedFPSState;
		ACERuntimeOptions::Set(TEXT("ShowFrameRate"),SavedFPS);PC->UpdateFrameRateOverlay();VR->UpdatePanels();
		TestFalse(TEXT("First tracked pose replaces the startup menu location"), StartupPanel.Equals(VR->RetailPanel->GetComponentLocation(), 1));
		TestTrue(TEXT("Login card is near tracked eye height"), FMath::Abs(VR->RetailPanel->GetComponentLocation().Z - VR->Head->GetComponentLocation().Z) < 25);
		const FVector AnchoredPanel = VR->RetailPanel->GetComponentLocation();
		VR->Head->AddLocalRotation(FRotator(0, 10, 0)); VR->UpdateTrackingState(true);
		TestTrue(TEXT("A menu stays anchored while the user turns to read it"), AnchoredPanel.Equals(VR->RetailPanel->GetComponentLocation()));
		VR->Head->AddLocalRotation(FRotator(0, -10, 0));
		VR->bTracking = false; VR->UpdatePanels();
		TestTrue(TEXT("An unworn headset does not restart the login flow"), PC->IsLoginUIVisible());
		VR->bTracking = true; VR->UpdatePanels();
		VR->InventoryPressed(); VR->UpdateInventoryHold(.2f); VR->InventoryReleased();
		TestFalse(TEXT("Short inventory tap does not open VR options"),VR->bSettingsOpen);
		VR->InventoryPressed(); VR->UpdateInventoryHold(.7f);
		TestTrue(TEXT("Holding X opens options without a runtime Menu event, including in lobby"),VR->bSettingsOpen && VR->SettingsPanel->IsVisible());
		VR->UpdateInventoryHold(2.f); VR->InventoryReleased();
		TestTrue(TEXT("Held X opens options only once and release keeps them open"),VR->bSettingsOpen);
		VR->ToggleSettings();
		VR->InventoryPressed(); VR->UpdateInventoryHold(.3f); VR->UpdateTrackingState(false); VR->UpdateTrackingState(true); VR->UpdateInventoryHold(1.f);
		TestFalse(TEXT("Runtime focus loss cancels a pending menu hold"),VR->bSettingsOpen);
		TestTrue(TEXT("Both controller poses work while the login pawn is hidden"), VR->LeftGrip->IsTracked() && VR->RightAim->IsTracked());
		TestTrue(TEXT("Hand markers remain visible in the unlit lobby"), VR->FallbackArms[2]->IsVisible() && VR->FallbackArms[5]->IsVisible() && !VR->PresentationActor->IsHidden());
		TestTrue(TEXT("Menu beams appear before a widget is hit"), VR->PointerBeams[0]->IsVisible() && VR->PointerBeams[1]->IsVisible());
		TestEqual(TEXT("Disabled player collision does not disable the VR login panel"), VR->RetailPanel->GetCollisionEnabled(), ECollisionEnabled::QueryOnly);
		FHitResult Hit;
		const FVector PanelCenter = VR->RetailPanel->GetComponentLocation(), Normal = VR->RetailPanel->GetForwardVector();
		TestTrue(TEXT("Login panel can be hit by an actual world trace"), World->LineTraceSingleByChannel(Hit, PanelCenter + Normal * 50, PanelCenter - Normal * 50, ECC_Visibility));
		TestTrue(TEXT("Trace hits the login widget"), Hit.GetComponent() == VR->RetailPanel);
		TestFalse(TEXT("Keyboard stays hidden until a login field is selected"),VR->KeyboardPanel->IsVisible());
		VR->FocusTextEntry(Login->AccountBox); VR->UpdatePanels();
		TestTrue(TEXT("Selecting a field shows the world-space keyboard"), VR->KeyboardPanel->IsVisible() && VR->KeyboardPanel->GetWidgetSpace() == EWidgetSpace::World);
		TestTrue(TEXT("Login form stays visible while editing"),VR->RetailPanel->IsVisible());
		if (FApp::CanEverRender())
		{
			if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
			for (int32 Pass = 0; Pass < 3; ++Pass)
			{
				VR->RetailPanel->TickComponent(.016f, LEVELTICK_All, nullptr);
				VR->KeyboardPanel->TickComponent(.016f, LEVELTICK_All, nullptr);
				FlushRenderingCommands();
			}
			// Screenshots contain only fixture text; never save or expose local credentials.
			for (const FName Field : {FName("HostBox"), FName("PortBox"), FName("AccountBox"), FName("PasswordBox")})
				if (auto* Box = Cast<UEditableTextBox>(Login->WidgetTree->FindWidget(Field)))
				{
					Box->OnTextChanged.RemoveAll(Login); Box->OnTextCommitted.RemoveAll(Login);
					Box->SetText(FText::FromString(Field == TEXT("HostBox") ? TEXT("localhost") : Field == TEXT("PortBox") ? TEXT("9000") : TEXT("VR test")));
				}
			VR->RetailPanel->TickComponent(.016f, LEVELTICK_All, nullptr); FlushRenderingCommands();
			auto* LoginButton = Login->WidgetTree->FindWidget(TEXT("LoginButton"));
			if (TestNotNull(TEXT("The rendered card contains a Login button"), LoginButton))
			{
				const FGeometry Geometry = LoginButton->GetCachedGeometry();
				const FVector2D ButtonCenter = Login->GetCachedGeometry().AbsoluteToLocal(Geometry.LocalToAbsolute(Geometry.GetLocalSize() * .5));
				TestTrue(TEXT("Login button fits inside the launcher surface"), Geometry.GetLocalSize().X > 1 && ButtonCenter.Y > 0 && ButtonCenter.Y < VR->RetailPanel->GetDrawSize().Y);
				auto TraceControl = [&](UWidgetComponent* Panel, FVector2D Pixel)
				{
					const FVector2D Size = Panel->GetDrawSize();
					const FVector Control = Panel->GetComponentTransform().TransformPosition(FVector(0, Size.X * .5 - Pixel.X, Size.Y * .5 - Pixel.Y));
					const FVector Eye = VR->Head->GetComponentLocation();
					FVector FromEye = Control - Eye;
					const double DownAngle = FMath::RadiansToDegrees(FMath::Atan2(FromEye.Z, FromEye.Size2D()));
					TestTrue(TEXT("Login and keys are within 35 degrees of eye level"), FMath::Abs(DownAngle) < 35.);
					VR->RightAim->SetWorldLocationAndRotation(Eye + FVector(0, 12, -12), (Control - Eye - FVector(0, 12, -12)).Rotation());
					VR->RightPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
					TestTrue(TEXT("Controller ray reaches the intended panel without another panel covering it"), VR->RightPointer->GetHoveredWidgetComponent() == Panel);
					AddInfo(FString::Printf(TEXT("%s control pixel=%s pointer=%s"), *Panel->GetName(), *Pixel.ToString(), *VR->RightPointer->Get2DHitLocation().ToString()));
					TestTrue(Panel->GetName() + TEXT(" control accepts pointer interaction"), VR->RightPointer->IsOverInteractableWidget());
				};
				TraceControl(VR->RetailPanel, ButtonCenter);
				TraceControl(VR->KeyboardPanel, FVector2D(50, 70));
			}
			TestTrue(TEXT("Keyboard stays above the template floor in the lobby"), VR->KeyboardPanel->Bounds.GetBox().Min.Z > 0);
			const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/VR"); IFileManager::Get().MakeDirectory(*Directory, true);
			for (auto* Panel : {VR->RetailPanel.Get(), VR->KeyboardPanel.Get()})
			{
				auto* Surface = Panel->GetRenderTarget();
				TArray<FColor> Pixels; Surface->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
				TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Surface->SizeX, Surface->SizeY, Pixels, PNG);
				FFileHelper::SaveArrayToFile(PNG, *(Directory / (Panel->GetName() + TEXT(".png"))));
				TestTrue(Panel->GetName() + TEXT(" renders visible controls"), Pixels.ContainsByPredicate([](FColor Pixel) { return Pixel.R > 100 || Pixel.G > 100 || Pixel.B > 100; }));
			}
			PollHands(); VR->RightPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
		}
		// Character creation reuses the retail canvas after character selection.
		// Exercise real controller rays, capture, keyboard keys and final submission.
		if (FApp::CanEverRender())
		{
			VR->DismissTextEntry();
			TestTrue(TEXT("Creator loads retail DAT"),GI->GetSubsystem<UACEDatSubsystem>()->LoadDatDirectory(RetailDatDirectory));
			VR->Client->Session->State=EACESessionState::CharacterSelect;
			auto* CreationCanvas=CreateWidget<UACEUICanvasWidget>(PC);
			CreationCanvas->InitializeCanvas(VR->Client->GetUIElementManager());
			CreationCanvas->SetResourceResolver(VR->Client->GetUIResourceResolver());
			PC->DatCanvasWidget=CreationCanvas;
			PC->ShowCharacterCreationUI();
			auto* Creator=PC->DatCharGenBinder.Get();
			if (TestNotNull(TEXT("VR opens the real character creator"),Creator))
			{
				TestFalse(TEXT("Opening creation keeps controller input in game mode"),PC->bShowMouseCursor);
				auto PaintCreator=[&]
				{
					for(int Pass=0;Pass<3;++Pass)
					{
						VR->UpdatePanels();VR->RetailPanel->TickComponent(.016f,LEVELTICK_All,nullptr);
						VR->KeyboardPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();
					}
				};
				auto AimControl=[&](bool Left,UWidgetComponent* Panel,FVector2D Pixel)
				{
					const FVector2D Size=Panel->GetDrawSize();
					const FVector Control=Panel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
					const FVector From=VR->Head->GetComponentLocation()+FVector(0,Left?-15:15,-20);
					(Left?VR->LeftAim:VR->RightAim)->SetWorldLocationAndRotation(From,(Control-From).Rotation());
					auto* Pointer=Left?VR->LeftPointer.Get():VR->RightPointer.Get();
					Pointer->TickComponent(.016f,LEVELTICK_All,nullptr);
					TestTrue(TEXT("Creator controller ray reaches its visible panel"),Pointer->GetHoveredWidgetComponent()==Panel);
				};
				auto AimElement=[&](bool Left,const TCHAR* Name)
				{
					auto Element=VR->Client->GetUIElementManager()->FindElementByName(Name);
					if(!TestTrue(FString(TEXT("Creator control exists: "))+Name,Element.IsValid()))return;
					AimControl(Left,VR->RetailPanel,CreationCanvas->LayoutToViewport(FVector2D(Element->GetScreenOrigin())+FVector2D(Element->Width*.5,Element->Height*.5)));
				};
				auto ClickElement=[&](bool Left,const TCHAR* Name)
				{
					AimElement(Left,Name);VR->Trigger(Left,true);PaintCreator();VR->Trigger(Left,false);PaintCreator();
				};
				PaintCreator();
				TestFalse(TEXT("No keyboard covers creator navigation before name entry"),VR->KeyboardPanel->IsVisible());
				Creator->Model.SelectHeritage(1);
				ClickElement(true,TEXT("RadioSho"));TestEqual(TEXT("Left trigger changes heritage"),Creator->Model.Selection.Heritage,3u);
				ClickElement(false,TEXT("CGProfessionButton"));TestEqual(TEXT("Right trigger navigates to profession"),Creator->GetPage(),2);
				ClickElement(true,TEXT("WarButton"));TestEqual(TEXT("Left trigger chooses profession"),Creator->Model.Selection.Profession,4);
				ClickElement(false,TEXT("CGSkillsButton"));TestEqual(TEXT("Controller opens skills"),Creator->GetPage(),3);
				AimElement(true,TEXT("CGAppearanceButton"));VR->Trigger(true,true);
				VR->CancelGestures();PaintCreator();
				TestEqual(TEXT("Lost tracking cancels the pending navigation click"),Creator->GetPage(),3);
				ClickElement(false,TEXT("CGAppearanceButton"));TestEqual(TEXT("Other hand works after capture cancellation"),Creator->GetPage(),4);
				ClickElement(true,TEXT("CGTownButton"));TestEqual(TEXT("Controller opens starting town"),Creator->GetPage(),5);
				ClickElement(false,TEXT("CGSummaryButton"));TestEqual(TEXT("Controller opens summary"),Creator->GetPage(),6);
				ClickElement(true,TEXT("NameTextBox"));
				TestTrue(TEXT("Creator name is an explicit VR text session"),VR->bTextKeyboardOpen && VR->FocusedTextEntry.Get()==CreationCanvas);
				TestTrue(TEXT("Summary remains visible beside its keyboard"),VR->RetailPanel->bRenderInMainPass);
				const FVector2D KeySize=VR->KeyboardPanel->GetDrawSize();
				const FVector KeyboardTop=VR->KeyboardPanel->GetComponentTransform().TransformPosition(FVector(0,0,KeySize.Y*.5));
				const FVector CreatorBottom=VR->RetailPanel->GetComponentTransform().TransformPosition(FVector(0,0,-VR->RetailPanel->GetDrawSize().Y*.5));
				TestTrue(TEXT("Creator keyboard cannot overlap Finish/name controls"),KeyboardTop.Z<CreatorBottom.Z-3.f);
				VR->TypeText(TEXT("VR Hero"));
				TestEqual(TEXT("PC VR keyboard types into the actual name"),Creator->Model.Selection.Name,FString(TEXT("VR Hero")));
				AimControl(false,VR->KeyboardPanel,FVector2D(50,120));VR->Trigger(false,true);VR->Trigger(false,false);PaintCreator();
				TestTrue(TEXT("Clicking an actual keyboard key retains canvas focus"),Creator->Model.Selection.Name.StartsWith(TEXT("VR Hero")) && Creator->Model.Selection.Name.Len()==8);
				ClickElement(false,TEXT("CGTownButton"));TestEqual(TEXT("One click dismisses keyboard and navigates"),Creator->GetPage(),5);
				TestFalse(TEXT("Leaving name entry hides its keyboard"),VR->bTextKeyboardOpen || VR->KeyboardPanel->IsVisible());
				ClickElement(true,TEXT("CGSummaryButton"));ClickElement(false,TEXT("NameTextBox"));
				TestTrue(TEXT("Name keyboard reopens with the other hand"),VR->bTextKeyboardOpen);
				Creator->SetNameFromKeyboard(TEXT(""));VR->TypeKey(EKeys::Enter);PaintCreator();
				TestFalse(TEXT("Enter closes editing and displays retail name validation"),Creator->CanEditName());
				TestFalse(TEXT("Validation never leaves an inert keyboard covering the dialog"),VR->KeyboardPanel->IsVisible());
				Creator->KeyDown(FKeyEvent(EKeys::Enter,FModifierKeysState(),0,false,0,0));PaintCreator();
				ClickElement(true,TEXT("NameTextBox"));VR->TypeText(TEXT("Creation Hero"));
				VR->TypeKey(EKeys::Enter);PaintCreator();
				// This fixture is deliberately not connected; reaching the normal
				// submission/error dialog proves Enter was delivered, without creating
				// an account character. The creation wire tests use a loopback socket.
				TestFalse(TEXT("VR Enter reaches the existing submission path"),Creator->CanEditName());
				VR->DismissTextEntry();Creator->Shutdown();CreationCanvas->SetCharGenBinder(nullptr);
			}
			PC->DatCharGenBinder=nullptr;PC->DatCanvasWidget=nullptr;
			VR->Client->GetUIFlow()->SetMode(ACEUI::EACEUIFlowMode::CharacterManagement);
			VR->UpdatePanels();
		}
		VR->Client->Session->State = EACESessionState::InWorld;
		VR->ResetTrackingOrigin();
		TestTrue(TEXT("World entry removes the separate lobby elevation"), VR->TrackingOrigin->GetComponentLocation().Z < 100.f);
		const bool OriginalSeated = VR->Settings->bSeated;
		for (int32 I=0;I<6;++I)
		{
			VR->Settings->bSeated=(I%2)==0; VR->ResetTrackingOrigin();
			const float EyeHeight = VR->Head->GetComponentLocation().Z - (Pawn->GetActorLocation().Z-Capsule->GetScaledCapsuleHalfHeight());
			TestTrue(TEXT("Repeated posture changes never accumulate floor/eye offsets"),EyeHeight>=100.f && EyeHeight<=240.f);
		}
		VR->Settings->bSeated=OriginalSeated;VR->ResetTrackingOrigin();
		VR->Client->Session->PlayerGuid = 100;
		VR->Client->Session->PlayerVitals.CombatMode = ACECombatMode::Magic;
		VR->Client->Session->KnownSpells = {1, 2, 3};
		FACEWorldObject Wand; Wand.Guid = 200; Wand.WielderId = 100; Wand.CurrentWieldedLocation = ACEEquipMask::Held;
		VR->Client->Session->WorldObjects.Add(Wand.Guid, Wand);
		auto* Dat = GI->GetSubsystem<UACEDatSubsystem>();
		// The creator has already used the resolver's font/texture caches. Do not
		// replace their underlying DAT database halfway through this fixture.
		TestTrue(TEXT("Retail UI and avatar fixture loads DAT"), Dat->GetPortalDat() || Dat->LoadDatDirectory(RetailDatDirectory));
		auto* Manager = NewObject<UACEUIElementManager>(PC); Manager->Initialize();
		auto* Resources = NewObject<UACEUIResourceResolver>(PC); Resources->Initialize(Dat);
		auto* Layout = NewObject<UACEUILayoutResolver>(PC); Layout->Initialize(Dat, Manager);
		TestTrue(TEXT("Retail gameplay layout loads"), Layout->LoadLayout(0x21000005));
		auto* GameCanvas = CreateWidget<UACEUICanvasWidget>(PC);
		GameCanvas->InitializeCanvas(Manager); GameCanvas->SetResourceResolver(Resources); GameCanvas->TakeWidget();
		auto* Gameplay = NewObject<UACEUIGameplayBinder>(PC);
		Gameplay->Initialize(VR->Client, Manager, GameCanvas, PC); GameCanvas->SetGameplayBinder(Gameplay);
		PC->DatCanvasWidget = GameCanvas; PC->DatGameplayBinder = Gameplay;
		FACEWorldObject SelfRecord; SelfRecord.Guid = 100; SelfRecord.ItemsCapacity = 102;
		VR->Client->Session->WorldObjects.Add(100, SelfRecord);
		Gameplay->ApplyCombatMode(ACECombatMode::Magic);
		VR->Head->SetRelativeLocation(FVector(0, 0, 165)); PollHands();
		VR->Head->SetWorldRotation((VR->LeftGrip->GetComponentLocation() + FVector(0, 0, 12) - VR->Head->GetComponentLocation()).Rotation());
		auto PaintRetail = [&]()
		{
			// These assertions exercise the retained retail surface explicitly.
			VR->bUseDesktopMenu=true;
			for (int32 Pass = 0; Pass < 5; ++Pass)
			{
				VR->UpdatePanels(); VR->RetailPanel->TickComponent(.016f, LEVELTICK_All, nullptr);
				VR->WristPanel->TickComponent(.016f, LEVELTICK_All, nullptr);
				VR->VitalsPanel->TickComponent(.016f, LEVELTICK_All, nullptr);
				VR->KeyboardPanel->TickComponent(.016f, LEVELTICK_All, nullptr); FlushRenderingCommands();
			}
		};
		VR->bInventoryOpen = true;
		PaintRetail();
		{
			const bool ValidBefore = VR->Client->Session->PlayerVitals.bValid;
			VR->Client->Session->PlayerVitals.bValid = true;
			VR->Client->Session->NotifyVitalsChanged();
			VR->ToggleCombat(); PaintRetail();
			TestTrue(TEXT("Y preserves the open inventory interface"),VR->bInventoryOpen && VR->RetailPanel->IsVisible());
			TestEqual(TEXT("Y switches the retail binder to peace immediately"),Gameplay->CombatMode,int32(ACECombatMode::NonCombat));
			TestTrue(TEXT("Peace icon changes without waiting for server stat traffic"),Manager->FindElementByName(TEXT("PeaceModeButton"))->bVisible);
			TestFalse(TEXT("Y leaving combat hides the wrist spellbar"),VR->WristPanel->IsVisible());
			VR->ToggleCombat(); PaintRetail();
			TestEqual(TEXT("Y with a wand updates both session and retail to magic"),Gameplay->CombatMode,int32(ACECombatMode::Magic));
			TestTrue(TEXT("Y exposes retail magic art and the wrist surface"),Manager->FindElementByName(TEXT("MagicModeButton"))->bVisible && VR->WristPanel->IsVisible());
			VR->Client->Session->ApplyServerCombatMode(ACECombatMode::NonCombat); PaintRetail();
			TestTrue(TEXT("Transient server peace motion cannot flicker an armed wristbar"),Gameplay->CombatMode==int32(ACECombatMode::Magic) && VR->WristPanel->IsVisible());
			VR->Client->Session->PlayerVitals.bValid = ValidBefore;
			VR->bInventoryOpen = true; PaintRetail();
		}
		TestEqual(TEXT("A newly opened empty spell bank creates every visible tile"), Gameplay->SpellBarSlotBgs.Num(), 13);
		for (auto* Panel : {VR->RetailPanel.Get(), VR->WristPanel.Get()})
		{
			auto* Target = Panel->GetRenderTarget(); TArray<FColor> Pixels;
			Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
			for (int32 I = 0; I < 9; ++I)
			{
				const auto Geometry = Gameplay->SpellBarSlotBgs[I]->GetCachedGeometry();
				FVector2D Corner = GameCanvas->GetCachedGeometry().AbsoluteToLocal(Geometry.LocalToAbsolute(FVector2D::ZeroVector));
				if (Panel == VR->WristPanel) Corner -= VR->WristRetail->ToCanvas(FVector2D::ZeroVector);
				int32 Blue = 0;
				for (int32 Y = 2; Y < 30; ++Y) for (int32 X = 2; X < 30; ++X)
				{
					const int32 Pixel = (FMath::RoundToInt(Corner.Y) + Y) * Target->SizeX + FMath::RoundToInt(Corner.X) + X;
					if (Pixels.IsValidIndex(Pixel)) { const auto P = Pixels[Pixel]; Blue += P.B > 35 && P.B > P.R * 2 && P.B > P.G; }
				}
				TestTrue(FString::Printf(TEXT("%s empty slot %d visibly paints blue artwork before any spell is added"), *Panel->GetName(), I + 1), Blue > 20);
			}
			if (Panel == VR->WristPanel)
			{
				TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Target->SizeX, Target->SizeY, Pixels, PNG);
				FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectSavedDir() / TEXT("Automation/VR/EmptyWristHotbar.png")));
			}
		}
		VR->Client->Session->SendAddSpellToBar(1, 0, 0); VR->Client->Session->SendAddSpellToBar(2, 1, 0); PaintRetail();
		TestTrue(TEXT("Retail spell selection routes to VR"), VR->Client->SendCastSpell(2));
		TestEqual(TEXT("Retail and wrist selection agree"), VR->SelectedSpell, 2);
		TestTrue(TEXT("Selecting a spell preserves the open inventory menu"), VR->bInventoryOpen);
		VR->UpdatePanels();
		TestTrue(TEXT("Wrist spell menu is visible with a wand equipped"), VR->WristPanel->IsVisible());
		VR->Settings->bShowWristSpellBar=false;VR->UpdatePanels();
		TestFalse(TEXT("Wrist visibility setting hides its panel"),VR->WristPanel->IsVisible());
		TestEqual(TEXT("Hiding the wrist bar preserves magic stance"),VR->GetCombatMode(),int32(ACECombatMode::Magic));
		VR->Settings->bShowWristSpellBar=true;VR->UpdatePanels();
		TestEqual(TEXT("Wrist uses the retail surface instead of a custom list"), VR->WristPanel->GetWidget(), static_cast<UUserWidget*>(VR->WristRetail.Get()));
		{
			const FTransform Rig=VR->TrackingOrigin->GetComponentTransform();
			VR->UpdatePanels(.2f);
			const FTransform Wrist=VR->WristPanel->GetComponentTransform().GetRelativeTransform(Rig);
			VR->TrackingOrigin->AddWorldOffset(FVector(300,400,0)); VR->UpdatePanels(.011f);
			TestTrue(TEXT("Travel moves the smoothed wrist menu with the controllers without lag"),
				VR->WristPanel->GetComponentTransform().GetRelativeTransform(VR->TrackingOrigin->GetComponentTransform()).GetLocation().Equals(Wrist.GetLocation(),.1f));
			VR->TrackingOrigin->SetWorldTransform(Rig); VR->UpdatePanels(.2f);
			const float MainScale=VR->Settings->PanelScale;
			VR->Settings->VitalsScale=.13f; VR->Settings->ChatScale=.09f; VR->Settings->PanelScale=.08f; VR->UpdatePanels();
			TestTrue(TEXT("Pinned vitals and chat scales are independent of central canvas"),
				FMath::IsNearlyEqual(VR->VitalsPanel->GetComponentScale().X,.13) && FMath::IsNearlyEqual(VR->ChatPanel->GetComponentScale().X,.09));
			VR->Settings->PanelScale=MainScale; VR->UpdatePanels();
		}
		auto LocalCenter = [&](UWidget* W)
		{
			const FGeometry G = W->GetCachedGeometry();
			return GameCanvas->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize() * .5));
		};
		auto PointAt = [&](UWidgetComponent* Panel, FVector2D Pixel, bool ExpectHit=true)
		{
			const FVector2D Size = Panel->GetDrawSize();
			const FVector Control = Panel->GetComponentTransform().TransformPosition(FVector(0, Size.X * .5 - Pixel.X, Size.Y * .5 - Pixel.Y));
			const FVector From = Control + Panel->GetForwardVector() * 35.f;
			VR->RightAim->SetWorldLocationAndRotation(From, (Control - From).Rotation());
			VR->RightPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
			TestEqual(*FString::Printf(TEXT("Controller trace at %s on %s matches %s (actual %s)"),*Pixel.ToString(),*GetNameSafe(Panel),*GetNameSafe(ExpectHit?Panel:nullptr),*GetNameSafe(VR->RightPointer->GetHoveredWidgetComponent())), VR->RightPointer->GetHoveredWidgetComponent(), ExpectHit?Panel:nullptr);
		};
		{
			TestTrue(TEXT("Vitals controls show while the inventory menu is open"),VR->ShouldShowVitalsControls());
			VR->bInventoryOpen = false; VR->UpdatePanels();
			VR->LeftAim->SetWorldLocationAndRotation(FVector(0,0,200),FRotator(0,180,0));
			VR->RightAim->SetWorldLocationAndRotation(FVector(0,0,200),FRotator(0,180,0));
			VR->LeftPointer->TickComponent(.016f,LEVELTICK_All,nullptr); VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);
			TestFalse(TEXT("Vitals lock label hides when neither menu nor hover is active"),VR->ShouldShowVitalsControls());
			const FTransform VitalsBefore = VR->VitalsPanel->GetComponentTransform();
			const FVector2D SizeBefore = VR->VitalsPanel->GetDrawSize();
			PointAt(VR->VitalsPanel, SizeBefore * FVector2D(.5,.25));
			TestTrue(TEXT("Hovering the vitals reveals lock controls"),VR->ShouldShowVitalsControls());
			TestTrue(TEXT("Revealing lock controls cannot move or resize the vitals"),VitalsBefore.Equals(VR->VitalsPanel->GetComponentTransform()) && SizeBefore == VR->VitalsPanel->GetDrawSize());
			TestTrue(TEXT("Hover information sorts above the central interface"),VR->FocusPanel->TranslucencySortPriority > VR->RetailPanel->TranslucencySortPriority);
			VR->PlaceFocusPopup(VR->Head->GetComponentLocation() + VR->Head->GetForwardVector()*5000.f, false);
			const double FarScale = VR->FocusPanel->GetComponentScale().X;
			TestTrue(TEXT("Distant target label remains within 1.8m reading distance"), FVector::Distance(VR->FocusPanel->GetComponentLocation(),VR->Head->GetComponentLocation()) < 180.f);
			VR->PlaceFocusPopup(VR->Head->GetComponentLocation() + VR->Head->GetForwardVector()*50000.f, false);
			TestEqual(TEXT("Further targets cannot shrink the hover text"),VR->FocusPanel->GetComponentScale().X,FarScale);
			VR->bInventoryOpen = true; PollHands(); PaintRetail();
		}
		{
			FACEWorldObject NPC; NPC.Guid=602; NPC.Name=TEXT("Bookie"); NPC.ItemType=ACEItemType::Creature; NPC.bHasPosition=true; NPC.Position.CellId=1;
			VR->Client->Session->WorldObjects.Add(NPC.Guid,NPC);
			auto* Actor=World->SpawnActor<AACEWorldEntityActor>(); Actor->InitializeFromObject(NPC,100,false);
			Actor->SetActorLocation(VR->Head->GetComponentLocation()+VR->Head->GetForwardVector()*220.f-FVector(0,0,180));
            const uint32 OldCapabilities=VR->Client->Session->VRCapabilities;
            VR->Client->Session->VRCapabilities=127;
            const int32 BeforeUnattributed=VR->WorldNotices.Num();
            VR->HealthFeedback(602,-42,1);VR->HealthFeedback(602,21,1);
            TestEqual(TEXT("Other players' unattributed damage and healing do not create popups"),VR->WorldNotices.Num(),BeforeUnattributed);
            VR->CombatMessage(TEXT("You scorch Bookie for 42 points with Flame Bolt VII."),TEXT(""),ACEChatMessageType::Magic); auto& Magic=VR->WorldNotices.Last();
            TestTrue(TEXT("Outgoing magic damage uses the target lane and gold without a minus sign"),Magic.Actor.Get()==Actor && Magic.NumberLane==1 && Magic.Text==TEXT("42 DAMAGE") && Magic.Color.Equals(FLinearColor(1.f,.8f,.25f)));
            VR->HealthFeedback(VR->Client->GetPlayerGuid(),21,1);TestTrue(TEXT("Self healing uses green and a plus sign"),VR->WorldNotices.Last().Text==TEXT("+21 HEALTH") && VR->WorldNotices.Last().Color.Equals(FLinearColor(.5f,1.f,.498f)));
            VR->CombatFeedback(TEXT("Bookie"),33,false,true);TestTrue(TEXT("Critical marker precedes our damage"),VR->WorldNotices.Last().Text==TEXT("Crit! 33 DAMAGE") && VR->WorldNotices.Last().Color.Equals(FLinearColor(1.f,.8f,.25f)));
            VR->HealthFeedback(VR->Client->GetPlayerGuid(),-17,1);TestTrue(TEXT("Incoming damage uses the YOU lane and red, without a minus sign"),VR->WorldNotices.Last().NumberLane==-1 && VR->WorldNotices.Last().Text==TEXT("17 DAMAGE") && VR->WorldNotices.Last().Color.Equals(FLinearColor(1.f,.247f,.247f)));
            Actor->SetActorLocation(Actor->GetActorLocation()+FVector(10000,0,0)); VR->UpdateWorldNotices();
            for (const auto& N:VR->WorldNotices) if(N.Kind==2)
                TestTrue(TEXT("Distant combat stays at a fixed readable distance"),FVector::Distance(N.Panel->GetComponentLocation(),VR->Head->GetComponentLocation())<170.f);
            Actor->SetActorLocation(VR->Head->GetComponentLocation()+VR->Head->GetForwardVector()*220.f-FVector(0,0,180));
            const int32 Count=VR->WorldNotices.Num();VR->CombatFeedback(TEXT("Bookie"),17,true,false);
            FACEPlayerVitals V;V.bValid=true;V.Health=10;VR->VitalsFeedback(V);V.Health=5;VR->VitalsFeedback(V);
            TestEqual(TEXT("Retail combat and vital packets cannot duplicate confirmed numbers"),VR->WorldNotices.Num(),Count);
            VR->Client->Session->VRCapabilities=OldCapabilities;
            for(auto& N:VR->WorldNotices) N.Panel->DestroyComponent(); VR->WorldNotices.Empty();
			VR->CombatMessage(TEXT("Other Player scorches you for 42 points with Flame Bolt VII."),TEXT(""),ACEChatMessageType::Magic);
			VR->CombatMessage(TEXT("You scorch Bookie for 42 points with Flame Bolt VII."),TEXT("Someone"),ACEChatMessageType::Magic);
			VR->CombatMessage(TEXT("You scorch Bookie for 42 points of fire damage!"),TEXT(""),ACEChatMessageType::CombatSelf);
			TestTrue(TEXT("Other casters, user chat and physical-hit chat cannot duplicate outgoing spell popups"),VR->WorldNotices.IsEmpty());
			VR->CombatMessage(TEXT("Critical hit! Sneak Attack! You scorch Bookie for 1234 points with Flame Bolt VII."),TEXT(""),ACEChatMessageType::Magic);
			TestEqual(TEXT("Critical spell chat preserves the amount"),VR->WorldNotices.Last().Text,FString(TEXT("Crit! 1234 DAMAGE")));
			VR->CombatFeedback(TEXT("Bookie"),0,true,false);
			const auto& Evaded=VR->WorldNotices.Last();
			Evaded.Panel->GetSlateWidget()->SlatePrepass(1.f);
			const FVector2D Required=Evaded.Panel->GetSlateWidget()->GetDesiredSize();
			TestTrue(TEXT("Evaded card fits both font lines and outlines"),Evaded.Panel->GetDrawSize().X>=Required.X && Evaded.Panel->GetDrawSize().Y>=Required.Y);
			for(auto& N:VR->WorldNotices) N.Panel->DestroyComponent(); VR->WorldNotices.Empty();
			VR->Client->SelectObject(602);
			VR->NPCSpeech(602,TEXT("Welcome to the monster fight."));
			TestEqual(TEXT("Speaking NPC is deselected"),VR->Client->GetSelectedObject().Guid,0);
			TestEqual(TEXT("Speaking NPC hover remains suppressed until looking away"),VR->ConversationHoverGuid,602);
			FACEWorldObject Corpse; Corpse.Guid=603;Corpse.ObjectDescriptionFlags=ACEObjectDescFlag::Corpse;
			VR->Client->Session->WorldObjects.Add(603,Corpse);VR->Client->SelectObject(603);VR->RevealRetailDialog(603);
			TestEqual(TEXT("Opening loot retains corpse selection"),VR->Client->GetSelectedObject().Guid,603);
			VR->Client->SelectObject(0);
			TestEqual(TEXT("NPC speech creates a single card"),VR->WorldNotices.Num(),1);
			auto& Short=VR->WorldNotices[0];
			TestTrue(TEXT("Short dialogue is smaller than the old maximum"),Short.Panel->GetDrawSize().Y<360.f && Short.Panel->GetDrawSize().Y>=240.f);
			NPC.SetupId=0x02000001; Actor->ApplyDatAppearanceFromObject(NPC);
			for (int32 P=0;P<Actor->Appearance->GetPartCount();++P)
				if (auto* Part=Actor->Appearance->GetPartMesh(P))
				{ Part->SetRelativeLocation(Part->GetRelativeLocation()*3); Part->SetRelativeScale3D(FVector(3)); }
			VR->UpdateWorldNotices(); FBox NPCBounds(ForceInit);
			TestTrue(TEXT("Tall NPC has visible model bounds"),Actor->Appearance->GetVisualWorldBounds(NPCBounds));
			TestTrue(TEXT("Entire dialogue panel clears a tall NPC regardless of collision pill height"),
				Short.Panel->GetComponentLocation().Z-Short.Panel->GetDrawSize().Y*.05f>NPCBounds.Max.Z);
			FString LongText; for(int32 I=0;I<30;++I) LongText+=TEXT("Bring your winning tickets here after the fight and I will exchange them for a prize. ");
			VR->NPCSpeech(602,LongText);
			auto& Long=VR->WorldNotices[0];
			TestTrue(TEXT("Long dialogue preserves its opening and the full new text"),Long.Text.StartsWith(TEXT("Welcome")) && Long.Text.Contains(LongText));
			TestTrue(TEXT("Long dialogue has time to scroll and read"),Long.Expires-Long.Started>18. && Long.ScrollEnd>0.f);
			TestTrue(TEXT("Long card keeps the old size ceiling"),Long.Panel->GetDrawSize().X<=720.f && Long.Panel->GetDrawSize().Y<=360.f);
			Long.ScrollStarts=FPlatformTime::Seconds()-2.; Long.LastRedraw=0.; VR->UpdateWorldNotices();
			TestTrue(TEXT("Dialogue scrolls slowly after its initial reading hold"),Long.Scroll->GetScrollOffset()>=24.f && Long.Scroll->GetScrollOffset()<30.f);
			VR->ShowWorldNotice(TEXT("Bookie gives you a Monster Fight Ticket."),0,1,FLinearColor::White);
			TestEqual(TEXT("Receipt resolves the speaking NPC"),VR->WorldNotices[1].Actor.Get(),Actor);
			const auto* Speech=VR->WorldNotices[0].Panel.Get(); const auto* Receipt=VR->WorldNotices[1].Panel.Get();
			TestTrue(TEXT("NPC receipt is beside the dialogue, not over its text"),FVector::Distance(Speech->GetComponentLocation(),Receipt->GetComponentLocation()) > (Speech->GetDrawSize().X+Receipt->GetDrawSize().X)*.05f);
			for(int32 I=0;I<15;++I) VR->ShowWorldNotice(TEXT("123"),602,2,FLinearColor::Yellow);
			TestEqual(TEXT("Each damage lane is capped at three notices, preserving dialogue and receipt"),VR->WorldNotices.Num(),5);
			TestTrue(TEXT("Combat bursts cannot replace unread NPC dialogue"),VR->WorldNotices[0].Kind==0 && VR->WorldNotices[0].Text.Contains(LongText));
			for(auto& N:VR->WorldNotices)
			{
				if(N.Kind==2) TestTrue(TEXT("Combat numbers have extended reading time"),N.Expires-N.Started>=2.7);
				N.Panel->SetRedrawTime(0); N.Panel->TickComponent(.016f,LEVELTICK_All,nullptr);
			}
			FlushRenderingCommands();
			for(int32 I=0;I<3;++I)
			{
				auto* Target=VR->WorldNotices[I].Panel->GetRenderTarget();
				if(TestNotNull(TEXT("Notification paints an actual render target"),Target))
				{
					TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
					TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,Pixels,PNG);
					FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("Automation/VR/Notice%d.png"),I)));
				}
			}
			VR->bInventoryOpen=false;
			VR->CombatMessage(TEXT("You are too busy!"),TEXT(""),ACEChatMessageType::TransientInfo);
			auto* Alert=VR->WorldNotices.FindByPredicate([](const auto& N){return N.Kind==3;});
			TestTrue(TEXT("Yellow server warning appears outside the closed retail UI"),Alert && Alert->Panel->IsVisible() && Alert->Text==TEXT("You are too busy!"));
			if(Alert) TestTrue(TEXT("Warning remains below personal menus and cannot intercept clicks"),Alert->Panel->TranslucencySortPriority<VR->SettingsPanel->TranslucencySortPriority && Alert->Panel->GetCollisionEnabled()==ECollisionEnabled::NoCollision);
			VR->CombatMessage(TEXT("Your skill is too low."), TEXT(""), ACEChatMessageType::ChatError);
			TestTrue(TEXT("Retail wield failure also reaches the headset notice area"), VR->WorldNotices.ContainsByPredicate([](const auto& N)
				{return N.Kind == 3 && N.Panel->IsVisible() && N.Text.Contains(TEXT("Your skill is too low."));}));
			VR->bInventoryOpen=true;
			for(auto& N:VR->WorldNotices) N.Panel->DestroyComponent(); VR->WorldNotices.Empty();
			Actor->Destroy(); VR->Client->Session->WorldObjects.Remove(NPC.Guid);
		}
		VR->bInventoryOpen=false; VR->bUseDesktopMenu=false;
		VR->Client->OnSpellLearned.Broadcast(2);
		TestTrue(TEXT("Learning a spell reveals the retail spellbook in VR"),VR->bInventoryOpen && VR->bUseDesktopMenu
			&& Gameplay->ActivePanelPage==TEXT("SpellManagementPanel_Field") && Gameplay->ActiveSpellPanelTab==TEXT("SpellbookPage"));
		TestTrue(TEXT("VR learned spell is selected and visible"),Gameplay->SelectedSpellbookId==2 && Gameplay->SpellbookRowIds.Contains(2));
		Gameplay->ShowPanelPage(TEXT("SpellManagementPanel_Field")); Gameplay->HandleNamedClick(TEXT("SpellbookTab"));
		Gameplay->TickRefresh(); PaintRetail();
		for (int32 TabIndex=1;TabIndex<=8;++TabIndex)
			if (auto Tab=Manager->FindElementByName(FString::Printf(TEXT("Spellcast_Tab%d"),TabIndex)))
				TestEqual(TEXT("Retail hotbar selected tab uses state 12; idle uses 11"),Tab->DefaultState, TabIndex-1==VR->Client->GetActiveSpellBar()?12u:11u);
		FACEBinaryWriter Confirmation;Confirmation.WriteUInt32(100);Confirmation.WriteUInt32(1);Confirmation.WriteUInt32(0x274);
		Confirmation.WriteUInt32(7);Confirmation.WriteUInt32(123);Confirmation.WriteString16L(TEXT("Would you like to start a monster fight?"));
		FACEBinaryReader ConfirmationReader(Confirmation.GetData());VR->Client->Session->HandleGameEvent(ConfirmationReader);
		Gameplay->TickRefresh();PaintRetail();
		TestTrue(TEXT("Server confirmation reveals the retail canvas in VR"),VR->bInventoryOpen && Gameplay->ServerConfirmRoot && Gameplay->ServerConfirmRoot->bVisible);
		if (auto Yes=Manager->FindElementByName(TEXT("ServerConfirmationYes")))
		{
			PointAt(VR->RetailPanel,GameCanvas->LayoutToViewport(FVector2D(Yes->GetScreenOrigin())+FVector2D(Yes->Width,Yes->Height)*.5));
			VR->Trigger(false,true);VR->Trigger(false,false);PaintRetail();
			TestTrue(TEXT("Controller accepts the actual bookie confirmation"),VR->Client->Session->GetConfirmations().IsEmpty());
		}
		else AddError(TEXT("Confirmation Yes button is missing"));
		{
			FACEBinaryWriter Popup;Popup.WriteUInt32(100);Popup.WriteUInt32(2);Popup.WriteUInt32(ACEGameEvent::PopupString);
			Popup.WriteString16L(TEXT("Rescue the sleeping pets and return them to their cages."));
			FACEBinaryReader R(Popup.GetData());VR->Client->Session->HandleGameEvent(R);
			Gameplay->TickRefresh();PaintRetail();
			TestTrue(TEXT("Quest popup is shown on the retail VR canvas"),VR->bInventoryOpen && Gameplay->ServerPopupRoot && Gameplay->ServerPopupRoot->bVisible);
			if(auto OK=Manager->FindElementByName(TEXT("ServerMessageOK")))
			{
				const uint32 Sequence=VR->Client->Session->NextGameActionSequence;
				PointAt(VR->RetailPanel,GameCanvas->LayoutToViewport(FVector2D(OK->GetScreenOrigin())+FVector2D(OK->Width,OK->Height)*.5));
				VR->Trigger(false,true);VR->Trigger(false,false);PaintRetail();
				TestTrue(TEXT("VR trigger dismisses informational messages without a server action"),VR->Client->Session->GetServerPopups().IsEmpty()
					&& VR->Client->Session->NextGameActionSequence==Sequence);
			}
			else AddError(TEXT("Message OK button is missing"));
		}
		if (TestTrue(TEXT("Retail spellbook provides a draggable row"), !Gameplay->SpellbookRowIds.IsEmpty()))
		{
			int32 Row = Gameplay->SpellbookRowIds.IndexOfByPredicate([&](int32 Id) { return Id != 0 && !VR->Client->GetSpellBar(0).Contains(Id); });
			if (Row == INDEX_NONE) Row = 0;
			const int32 Spell = Gameplay->SpellbookRowIds[Row];
			const FVector2D Start = LocalCenter(Gameplay->SpellbookRowBackgrounds[Row]);
			TestTrue(TEXT("Spells can be inspected from their actual visible row"),Gameplay->InspectSpellAt(Start));
			TestEqual(TEXT("Inspection uses the hovered spell"),Gameplay->ExaminedSpellId,Spell);
			TestTrue(TEXT("Spell description uses the retail spell examination body"),Manager->FindElementByName(TEXT("SpellExamineUI"))->bVisible);
			Gameplay->ShowExamination(false);Gameplay->RefreshExaminationOverlay();PaintRetail();
			PointAt(VR->RetailPanel, Start); VR->Trigger(false, true);
			TestTrue(TEXT("Controller press starts a pending spell drag without closing the menu"), Gameplay->bSpellDragPending && VR->bInventoryOpen);
			const FVector2D Other = GameCanvas->GetCachedGeometry().LocalToAbsolute(Start + FVector2D(100, 0));
			const int32 VirtualUser = FSlateApplication::Get().FindOrCreateVirtualUser(VR->RightPointer->VirtualUserIndex)->GetUserIndex();
			FPointerEvent OtherHand(VirtualUser, 0, Other, Other, TSet<FKey>{}, EKeys::Invalid, 0, FModifierKeysState());
			GameCanvas->NativeOnMouseMove(GameCanvas->GetCachedGeometry(), OtherHand);
			TestFalse(TEXT("The other hand cannot move a captured spell drag"), Gameplay->bSpellDragActive);
			PointAt(VR->RetailPanel, Start + FVector2D(30, 0));
			TestTrue(TEXT("Tracked controller movement activates the spell drag"), Gameplay->bSpellDragActive);
			const FVector2D Destination = LocalCenter(Gameplay->SpellBarSlotBgs[2]);
			PointAt(VR->WristPanel, Destination - VR->WristRetail->ToCanvas(FVector2D::ZeroVector));
			VR->Trigger(false, false);
			TestFalse(TEXT("Wrist rejects a dragged spell so its bank cannot be reordered"), VR->Client->GetSpellBar(0).Contains(Spell));
			TestFalse(TEXT("Cross-surface drop releases the drag"), Gameplay->bSpellDragPending);
			PointAt(VR->RetailPanel, Start); VR->Trigger(false, true);
			PointAt(VR->RetailPanel, Destination); VR->Trigger(false, false);
			TestTrue(TEXT("Spellbook drag still adds a spell on the central hotbar"), VR->Client->GetSpellBar(0).Contains(Spell));
			TestTrue(TEXT("Dragging preserves the spellbook window"), VR->bInventoryOpen && Gameplay->ActivePanelPage == TEXT("SpellManagementPanel_Field"));
			VR->Client->SendRemoveSpellFromBar(Spell,0); Gameplay->RefreshSpellHotbarOverlays(); PaintRetail();
			const uint32 BeforeBookClick=VR->Client->Session->NextGameActionSequence;
			PointAt(VR->RetailPanel,Start); VR->Trigger(false,true); VR->Trigger(false,false);
			TestEqual(TEXT("VR spellbook single click does not cast"),VR->Client->Session->NextGameActionSequence,BeforeBookClick);
			VR->Trigger(false,true); VR->Trigger(false,false);
			TestTrue(TEXT("VR double-click adds the spell to the open bank"),VR->Client->GetSpellBar(0).Contains(Spell));
			TestEqual(TEXT("VR double-click only sends the favorite action"),VR->Client->Session->NextGameActionSequence,BeforeBookClick+1);
			PaintRetail();
			PointAt(VR->WristPanel, LocalCenter(Gameplay->SpellBarSlotBgs[0]) - VR->WristRetail->ToCanvas(FVector2D::ZeroVector));
			const auto BeforeWristDrag = VR->Client->GetSpellBar(0);
			VR->Trigger(false, true);
			TestFalse(TEXT("Wrist selection never leaves a reorder pending"), Gameplay->bSpellDragPending);
			PointAt(VR->WristPanel, LocalCenter(Gameplay->SpellBarSlotBgs[1]) - VR->WristRetail->ToCanvas(FVector2D::ZeroVector));
			VR->Trigger(false, false);
			TestTrue(TEXT("Dragging across wrist icons cannot change spell order"), VR->Client->GetSpellBar(0) == BeforeWristDrag);
			TestEqual(TEXT("The retail wrist icon selects the same spell as the original bank"), VR->SelectedSpell, VR->Client->GetSpellBar(0)[0]);
		}
		// Focus uses the same virtual user as both controller pointers. Typing is
		// exercised locally without Enter, so this fixture never sends a chat message.
		if (TestNotNull(TEXT("Gameplay chat entry exists"), Gameplay->ChatEntry.Get()))
		{
			PointAt(VR->RetailPanel, LocalCenter(Gameplay->ChatEntry));
			VR->Trigger(false, true); VR->Trigger(false, false); PaintRetail();
			TestTrue(TEXT("Focusing chat automatically opens the headset keyboard"), VR->bTextKeyboardOpen && VR->KeyboardPanel->IsVisible());
			TestFalse(TEXT("The central quad cannot occlude text entry"), VR->RetailPanel->bRenderInMainPass);
			Gameplay->ChatEntry->SetText(FText::GetEmpty());
			VR->TypeText(TEXT("vr draft"));
			TestEqual(TEXT("Controller keyboard types into focused chat"), Gameplay->ChatEntry->GetText().ToString(), FString(TEXT("vr draft")));
			PointAt(VR->KeyboardPanel, FVector2D(50, 70)); VR->Trigger(false, true); VR->Trigger(false, false);
			TestTrue(TEXT("Clicking a virtual keyboard key retains chat focus and inserts text"), Gameplay->ChatEntry->GetText().ToString().Contains(TEXT("`")));
			Gameplay->ChatEntry->SetText(FText::FromString(TEXT("vr draft")));
			VR->ToggleInventory(); PaintRetail();
			TestFalse(TEXT("X dismisses chat and its keyboard"), VR->bTextKeyboardOpen || VR->bKeyboardOpen);
			TestTrue(TEXT("X restores the open central interface"), VR->RetailPanel->bRenderInMainPass);
			TestEqual(TEXT("Dismissing preserves the unsent draft"), Gameplay->ChatEntry->GetText().ToString(), FString(TEXT("vr draft")));
			Gameplay->FocusChatEntryWindow(0); PaintRetail();
			TestTrue(TEXT("Retail chat focus helper uses the VR keyboard too"), VR->bTextKeyboardOpen);
			VR->RightAim->SetWorldLocationAndRotation(FVector(0, 0, 200), FRotator(0, 180, 0));
			VR->RightPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
			VR->Trigger(false, true); VR->Trigger(false, false); PaintRetail();
			TestFalse(TEXT("Clicking outside keyboard releases chat without trapping input"), VR->bTextKeyboardOpen);
			Gameplay->ChatEntry->SetText(FText::GetEmpty());
			Gameplay->FocusChatEntryWindow(0); PaintRetail();
			VR->TypeKey(EKeys::Enter); PaintRetail();
			TestFalse(TEXT("Committing empty chat also closes its keyboard without leaving inert keys"), VR->bTextKeyboardOpen || VR->bKeyboardOpen);
		}
		VR->Settings->VitalsAnchorMode=0;VR->UpdatePanels();
		const FVector OriginalVitals = VR->Settings->VitalsViewOffset;
		// Slate can restore text focus when clicking non-focusable window chrome.
		VR->RightPointer->SetFocus(Gameplay->ChatEntry); PaintRetail();
		TestFalse(TEXT("Restored Slate focus alone cannot open the keyboard"), VR->bTextKeyboardOpen);
		Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field")); Manager->SetUiLocked(true); PaintRetail();
		const bool PlacementLocked=VR->Settings->bMenuLocked;
		auto ClickRetailLock=[&]()
		{
			VR->UpdatePanels();VR->MenuControlsPanel->SetRedrawTime(0);VR->MenuControlsPanel->RequestRedraw();
			VR->MenuControlsPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();
			PointAt(VR->MenuControlsPanel,FVector2D(600,24));VR->Trigger(false,true);VR->Trigger(false,false);PaintRetail();
		};
		ClickRetailLock();
		TestFalse(TEXT("VR toolbar unlocks the individual retail windows"),Manager->IsUiLocked());
		TestEqual(TEXT("Retail window unlock preserves floating panel placement lock"),VR->Settings->bMenuLocked,PlacementLocked);
		if (const auto Window = Manager->FindElementByName(TEXT("RootGameplay_FloatyPanel_Field")))
		{
			Manager->BringFloatyToFront(Window); PaintRetail();
			TSharedPtr<FACEUIElement> Handle;
			TFunction<void(TSharedPtr<FACEUIElement>)> FindHandle = [&](TSharedPtr<FACEUIElement> Node)
			{
				if (!Node->bVisible) return;
				if (Manager->IsFloatyDragHandle(Node) && Node->Width > 30 && Node->Height > 0) Handle = Node;
				for (const auto& Child : Node->Children) if (!Handle) FindHandle(Child);
			};
			FindHandle(Window);
			if (TestTrue(TEXT("Unlocked inventory has a real drag handle"), Handle.IsValid()))
			{
				const FIntPoint Old(Window->UserDragX, Window->UserDragY);
				const FVector2D Start = FVector2D(Handle->GetScreenOrigin()) + FVector2D(Handle->Width, Handle->Height) * .5;
				PointAt(VR->RetailPanel, Start); VR->Trigger(false, true);
				PointAt(VR->RetailPanel, Start + FVector2D(-35, 20)); VR->Trigger(false, false); PaintRetail();
				TestFalse(TEXT("Dragging window chrome never opens the keyboard"), VR->bTextKeyboardOpen);
				TestTrue(TEXT("Controller drag moves the original inventory window"), Window->UserDragX != Old.X || Window->UserDragY != Old.Y);
				Window->UserDragX = Old.X; Window->UserDragY = Old.Y; PaintRetail();
			}
			if (const auto Close = Manager->FindElementUnder(TEXT("RootGameplay_FloatyPanel_Field"), TEXT("CloseInvPanelButton")))
			{
				PointAt(VR->RetailPanel, FVector2D(Close->GetScreenOrigin()) + FVector2D(Close->Width, Close->Height) * .5);
				VR->Trigger(false, true); VR->Trigger(false, false); PaintRetail();
				TestFalse(TEXT("Closing inventory never opens the keyboard"), VR->bTextKeyboardOpen);
				TestTrue(TEXT("Controller close button closes the original panel"), Gameplay->ActivePanelPage.IsEmpty());
			}
		}
		ClickRetailLock();
		TestTrue(TEXT("VR toolbar can relock the individual retail windows"),Manager->IsUiLocked());
		VR->Settings->bVitalsLocked = false;
		PointAt(VR->VitalsPanel, VR->VitalsPanel->GetDrawSize() * FVector2D(.5, .25));
		VR->BeginVitalsDrag(false); VR->UpdateVitalsDrag();
		TestTrue(TEXT("Grabbing the vitals preserves their initial position"), VR->Settings->VitalsViewOffset.Equals(OriginalVitals, .01));
		VR->RightAim->AddWorldOffset(VR->Head->GetRightVector() * 10.f); VR->UpdatePanels();
		TestTrue(TEXT("Dragging vitals moves their position in view space"), VR->Settings->VitalsViewOffset.Y > OriginalVitals.Y + 9.f);
		VR->EndVitalsDrag(false); VR->Settings->bVitalsLocked = true;
		const FVector LockedVitals = VR->Settings->VitalsViewOffset;
		VR->BeginVitalsDrag(false); VR->RightAim->AddWorldOffset(VR->Head->GetRightVector() * 10.f); VR->UpdateVitalsDrag();
		TestTrue(TEXT("Locked vitals cannot be dragged"), VR->Settings->VitalsViewOffset.Equals(LockedVitals));
		VR->Settings->VitalsViewOffset = OriginalVitals;
		const FTransform OldHead=VR->Head->GetComponentTransform();
		VR->Settings->VitalsAnchorMode=2;VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		const FTransform WorldVitals=VR->VitalsPanel->GetComponentTransform();
		const FTransform TrackingForWorldVitals=VR->TrackingOrigin->GetComponentTransform();
		VR->TrackingOrigin->AddWorldOffset(FVector(1,2,3));VR->TrackingOrigin->AddWorldRotation(FRotator(0,30,0));
		TestTrue(TEXT("World vitals stay fixed between tracking and panel ticks"),VR->VitalsPanel->GetComponentTransform().Equals(WorldVitals,.001));
		VR->TrackingOrigin->SetWorldTransform(TrackingForWorldVitals);
		VR->Head->AddWorldOffset(FVector(20,5,3));VR->Head->AddWorldRotation(FRotator(15,10,7));VR->UpdatePanels();
		TestTrue(TEXT("World-pinned vitals stay still during head movement"),VR->VitalsPanel->GetComponentTransform().Equals(WorldVitals,.01));
		VR->Head->SetWorldTransform(OldHead);VR->Settings->VitalsAnchorMode=1;VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		const FVector OwnerBeforeRun = Pawn->GetActorLocation();
		const FTransform TrackingBeforeRun = VR->TrackingOrigin->GetComponentTransform();
		const FVector VitalsBeforeRun = VR->VitalsPanel->GetComponentLocation();
		for(int32 I=0;I<90;++I)
		{
			const FVector Step = VR->Head->GetForwardVector()*12.f;
			Pawn->AddActorWorldOffset(Step);VR->TrackingOrigin->AddWorldOffset(Step);VR->UpdatePanels(1.f/90.f);
		}
		TestTrue(TEXT("Vitals retain their relative position at full running speed"),
			(VR->VitalsPanel->GetComponentLocation()-VitalsBeforeRun).Equals(Pawn->GetActorLocation()-OwnerBeforeRun,.1));
		Pawn->SetActorLocation(OwnerBeforeRun);VR->TrackingOrigin->SetWorldTransform(TrackingBeforeRun);VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		const FVector BeforeTurn = VR->Head->GetComponentTransform().InverseTransformPosition(VR->VitalsPanel->GetComponentLocation());
		for(int32 I=0;I<4;++I) { VR->RotateTracking(45.f);VR->UpdatePanels(); }
		TestTrue(TEXT("Repeated stick turns keep stabilized vitals in front of the player"),
			VR->Head->GetComponentTransform().InverseTransformPosition(VR->VitalsPanel->GetComponentLocation()).Equals(BeforeTurn,.1));
		VR->TrackingOrigin->SetWorldTransform(TrackingBeforeRun);VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		// Head tracking noise straddling the follow threshold must never pull the
		// anchor back toward its original heading on alternating frames.
		for(float Direction:{-1.f,1.f})
		{
			VR->Head->SetWorldTransform(OldHead);VR->bVitalsAnchorReady=false;VR->UpdatePanels();
			const float StartYaw=VR->VitalsAnchorFrame.Rotator().Yaw;
			float Previous=0;
			for(int32 I=0;I<90;++I)
			{
				VR->Head->SetWorldRotation(FRotator(0,StartYaw+Direction*(28.f+(I%2==0?.4f:-.4f)),0));
				VR->UpdatePanels(1.f/30.f);
				const float Progress=Direction*FMath::FindDeltaAngleDegrees(StartYaw,VR->VitalsAnchorFrame.Rotator().Yaw);
				TestTrue(TEXT("Follow boundary cannot snap backward or jump ahead"),Progress>=Previous-.001f && Progress-Previous<1.5f);
				Previous=Progress;
			}
			TestTrue(TEXT("Follow settles inside the viewing cone"),Previous>9.f && Previous<13.f);
		}
		VR->Head->SetWorldTransform(OldHead);VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		// Compass anchors share the same locomotion transport and late-update rules as vitals.
		const bool MenuBeforePlacement=VR->bInventoryOpen; VR->bInventoryOpen=false;
		VR->Settings->bShowCompass=true; VR->Settings->CompassAnchorMode=2; VR->bCompassAnchorReady=false; VR->UpdatePanels();
		const FTransform WorldCompass=VR->CompassPanel->GetComponentTransform();
		const FTransform TrackingForWorldCompass=VR->TrackingOrigin->GetComponentTransform();
		VR->TrackingOrigin->AddWorldOffset(FVector(1,2,3));VR->TrackingOrigin->AddWorldRotation(FRotator(0,-30,0));
		TestTrue(TEXT("World compass stays fixed between tracking and panel ticks"),VR->CompassPanel->GetComponentTransform().Equals(WorldCompass,.001));
		VR->TrackingOrigin->SetWorldTransform(TrackingForWorldCompass);
		VR->Head->AddWorldOffset(FVector(20,5,3)); VR->Head->AddWorldRotation(FRotator(15,10,7)); VR->UpdatePanels();
		TestTrue(TEXT("World compass ignores physical head movement"),VR->CompassPanel->GetComponentTransform().Equals(WorldCompass,.01));
		VR->Head->SetWorldTransform(OldHead);
		VR->Settings->VitalsAnchorMode=2;VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		const FTransform FixedVitals=VR->VitalsPanel->GetComponentTransform();
		for(int32 I=0;I<180;++I)
		{
			VR->Head->SetWorldLocation(OldHead.GetLocation()+FVector(.2*FMath::Sin(double(I)),.2*FMath::Cos(double(I)),.1*FMath::Sin(I*2.)));
			VR->Head->SetWorldRotation(OldHead.Rotator()+FRotator(.2*FMath::Cos(double(I)),.3*FMath::Sin(double(I)),.1*FMath::Sin(double(I))));
			VR->UpdatePanels(1.f/90.f);
			TestTrue(TEXT("World HUD has no transform jitter under tiny tracking noise"),
				VR->VitalsPanel->GetComponentTransform().Equals(FixedVitals,.0001)
				&& VR->CompassPanel->GetComponentTransform().Equals(WorldCompass,.0001));
		}
		VR->Head->SetWorldTransform(OldHead);VR->Settings->VitalsAnchorMode=1;VR->bVitalsAnchorReady=false;VR->UpdatePanels();
		const FVector StillBody=VR->VitalsPanel->GetComponentLocation();
		for(int32 I=0;I<180;++I)
		{
			VR->Head->SetWorldLocation(OldHead.GetLocation()+FVector(.2*FMath::Sin(double(I)),.2*FMath::Cos(double(I)),.1*FMath::Sin(I*2.)));
			VR->UpdatePanels(1.f/90.f);
			TestTrue(TEXT("Body HUD rejects sub-centimetre resting-head noise"),VR->VitalsPanel->GetComponentLocation().Equals(StillBody,.001));
		}
		VR->Head->SetWorldTransform(OldHead); VR->Settings->CompassAnchorMode=0; VR->UpdatePanels();
		TestEqual(TEXT("Head compass participates in HMD late update"),VR->CompassPanel->GetAttachParent(),static_cast<USceneComponent*>(VR->Head.Get()));
		VR->Settings->CompassAnchorMode=1; VR->bCompassAnchorReady=false; VR->UpdatePanels();
		const FVector CompassBeforeWalk=VR->CompassPanel->GetComponentLocation();
		for(int32 I=0;I<90;++I)
		{
			const FVector Step=VR->Head->GetForwardVector()*12.f;
			Pawn->AddActorWorldOffset(Step); VR->TrackingOrigin->AddWorldOffset(Step); VR->UpdatePanels(1.f/90.f);
		}
		TestTrue(TEXT("Body compass follows running without trailing"),(VR->CompassPanel->GetComponentLocation()-CompassBeforeWalk).Equals(Pawn->GetActorLocation()-OwnerBeforeRun,.1));
		Pawn->SetActorLocation(OwnerBeforeRun); VR->TrackingOrigin->SetWorldTransform(TrackingBeforeRun); VR->bCompassAnchorReady=false; VR->UpdatePanels();
		const FVector CompassBeforeTurn=VR->Head->GetComponentTransform().InverseTransformPosition(VR->CompassPanel->GetComponentLocation());
		for(int32 I=0;I<4;++I){VR->RotateTracking(45.f);VR->UpdatePanels();}
		TestTrue(TEXT("Body compass follows stick turns"),VR->Head->GetComponentTransform().InverseTransformPosition(VR->CompassPanel->GetComponentLocation()).Equals(CompassBeforeTurn,.1));
		VR->TrackingOrigin->SetWorldTransform(TrackingBeforeRun); VR->bCompassAnchorReady=false; VR->UpdatePanels();
		VR->Settings->bCompassLocked=false;
		const FVector OriginalCompass=VR->Settings->CompassViewOffset;
		VR->NativeCompass->UpdateHeading(VR->Head->GetComponentRotation().Yaw,.016f);
		VR->CompassPanel->SetRedrawTime(0); VR->CompassPanel->RequestRedraw();
		VR->CompassPanel->TickComponent(.016f,LEVELTICK_All,nullptr); FlushRenderingCommands();
		PointAt(VR->CompassPanel,FVector2D(60,474)); VR->Trigger(false,true); VR->UpdatePanelEdit();
		TestEqual(TEXT("Compass move captures the initiating hand"),VR->PanelEditHand,1);
		TestTrue(TEXT("Compass grab does not jump"),VR->Settings->CompassViewOffset.Equals(OriginalCompass,.01));
		VR->RightAim->AddWorldOffset(VR->Head->GetRightVector()*10.f); VR->UpdatePanels();
		TestTrue(TEXT("Compass can be moved with a controller"),VR->Settings->CompassViewOffset.Y>OriginalCompass.Y+9.f);
		const FTransform Before3D=VR->CompassPanel->GetComponentTransform();
		VR->RightAim->AddWorldOffset(VR->Head->GetForwardVector()*25.f);VR->RightAim->AddWorldRotation(FRotator(12,15,8));VR->UpdatePanels();
		TestFalse(TEXT("Compass placement supports depth and wrist rotation"),VR->CompassPanel->GetComponentQuat().Equals(Before3D.GetRotation(),.01));
		TestTrue(TEXT("Full 3D placement changes depth"),FMath::Abs(VR->Settings->CompassViewOffset.X-OriginalCompass.X)>5);
		const FVector GrabBefore=VR->PanelGrabInHand.GetLocation();
		const FQuat RotationBefore=VR->CompassPanel->GetComponentQuat();
		VR->TurnStick=FVector2D(0,1);
		for(int32 I=0;I<30;++I) VR->UpdatePanels(1.f/30.f);
		TestTrue(TEXT("Right thumbstick pushes held panel away at a frame independent rate"),FMath::IsNearlyEqual(VR->PanelGrabInHand.GetLocation().X-GrabBefore.X,90.,.01));
		TestTrue(TEXT("Depth control preserves panel angle"),VR->CompassPanel->GetComponentQuat().Equals(RotationBefore,.001));
		VR->TurnStick=FVector2D(0,-1);
		for(int32 I=0;I<90;++I) VR->UpdatePanels(1.f/90.f);
		TestTrue(TEXT("Pulling panel back restores grab distance"),VR->PanelGrabInHand.GetLocation().Equals(GrabBefore,.01));
		VR->TurnStick=FVector2D::ZeroVector;
		VR->EndPanelEdit(false,0); TestEqual(TEXT("Other hand cannot end placement"),VR->PanelEditHand,1); VR->EndPanelEdit(false,1); VR->Trigger(false,false);
		VR->Settings->CompassViewOffset=OriginalCompass;VR->Settings->CompassViewRotation=FRotator::ZeroRotator; VR->UpdatePanels();
		// Resize every editable surface on its initial plane, preserving center and aspect ratio.
		VR->Settings->bVitalsLocked=false; VR->Settings->bMenuLocked=false; VR->Settings->bOptionsLocked=false;
		VR->SettingsPanel->SetVisibility(true);
		for(FName Name:{FName("Compass"),FName("Vitals"),FName("Menu"),FName("Options")})
		{
			auto* Surface=VR->GetEditablePanel(Name);
			// Isolate each ray-plane fixture; menu/HUD visual overlap has separate priority coverage below.
			for(FName Other:{FName("Compass"),FName("Vitals"),FName("Menu"),FName("Options")})
				VR->GetEditablePanel(Other)->SetCollisionEnabled(Other==Name?ECollisionEnabled::QueryOnly:ECollisionEnabled::NoCollision);
			const FVector Center=Surface->GetComponentLocation(); const float OriginalScale=VR->GetPanelScale(Name);
			const FVector2D Pixel=Surface->GetDrawSize()*FVector2D(.85,.85);
			// The menu is closed here; its retained retail canvas only renders
			// the wrist source. Exercise the resize plane without making that
			// hidden backing texture intercept a controller ray.
			if(Name=="Menu")TestFalse(TEXT("Closed retail backing canvas is not a visible interaction surface"),Surface->bRenderInMainPass);
			PointAt(Surface,Pixel,Surface->bRenderInMainPass); VR->BeginPanelEdit(Name,false,true);
			TestEqual(*FString::Printf(TEXT("%s resize captures"),*Name.ToString()),VR->PanelEditHand,1);
			const FVector2D Start=VR->PanelEditStart;
			const FVector Target=VR->PanelEditFrame.TransformPosition(FVector(0,Start.X*1.2,Start.Y*1.2));
			VR->RightAim->SetWorldRotation((Target-VR->RightAim->GetComponentLocation()).Rotation()); VR->UpdatePanelEdit();
			const float ExpectedScale=FMath::Min(OriginalScale*1.2f,Name=="Vitals"?.18f:.15f);
			TestTrue(*FString::Printf(TEXT("%s grows proportionally within supported limits"),*Name.ToString()),FMath::IsNearlyEqual(VR->GetPanelScale(Name),ExpectedScale,.001f));
			for(int32 I=0;I<10;++I)VR->UpdatePanelEdit();
			TestTrue(TEXT("Holding still cannot accumulate resize drift"),FMath::IsNearlyEqual(VR->GetPanelScale(Name),ExpectedScale,.001f));
			TestTrue(TEXT("Resizing preserves panel center"),Surface->GetComponentLocation().Equals(Center,.01));
			VR->EndPanelEdit(false); VR->GetPanelScale(Name)=OriginalScale;
		}
		VR->Settings->bCompassLocked=true; VR->BeginPanelEdit("Compass",false,true);
		TestEqual(TEXT("Locked compass rejects resize"),VR->PanelEditHand,INDEX_NONE);
		VR->Settings->bVitalsLocked=true; VR->Settings->bMenuLocked=true; VR->Settings->bOptionsLocked=true;
		VR->bInventoryOpen=MenuBeforePlacement; VR->UpdatePanels();
		// The menu toolbar is clickable independently of the retail canvas.
		VR->MenuControlsPanel->SetRedrawTime(0); VR->MenuControlsPanel->RequestRedraw();
		VR->MenuControlsPanel->TickComponent(.016f,LEVELTICK_All,nullptr); FlushRenderingCommands();
		PointAt(VR->MenuControlsPanel,FVector2D(240,24)); VR->Trigger(false,true); VR->Trigger(false,false);
		TestFalse(TEXT("Menu toolbar unlock works through the controller"),VR->Settings->bMenuLocked);
		PointAt(VR->MenuControlsPanel,FVector2D(400,24)); VR->Trigger(false,true);
		TestTrue(TEXT("Unlocked menu resize button begins placement"),VR->EditingPanel=="Menu" && VR->bPanelResize);
		VR->Trigger(false,false); TestEqual(TEXT("Trigger release ends resizing"),VR->PanelEditHand,INDEX_NONE);
		VR->Settings->MenuAnchorMode=0; VR->UpdatePanels();
		const FVector OriginalMenuOffset=VR->Settings->MenuViewOffset;
		PointAt(VR->MenuControlsPanel,FVector2D(60,24)); VR->Trigger(false,true);
		VR->RightAim->AddWorldOffset(VR->Head->GetRightVector()*8); VR->UpdatePanels();
		TestTrue(TEXT("Head anchored menu can be repositioned"),VR->Settings->MenuViewOffset.Y>OriginalMenuOffset.Y+7);
		VR->Trigger(false,false); VR->Settings->MenuViewOffset=OriginalMenuOffset;
		VR->Settings->MenuAnchorMode=2; VR->PositionPanel(VR->RetailPanel); VR->UpdatePanels();
		const FTransform SavedMenuPose=VR->RetailPanel->GetComponentTransform();
		const FTransform HeadBeforeMenuPin=VR->Head->GetComponentTransform();
		const bool DesktopMenuBeforeReopen=VR->bUseDesktopMenu;
		VR->ToggleInventory();
		VR->Head->AddWorldOffset(FVector(50,30,0)); VR->Head->AddWorldRotation(FRotator(0,35,0));
		VR->ToggleInventory();
		const FTransform ReopenedMenuPose=VR->RetailPanel->GetComponentTransform();
		TestFalse(TEXT("Reopening inventory leaves the old world position"),ReopenedMenuPose.Equals(SavedMenuPose,.01f));
		TestTrue(TEXT("Reopening inventory captures the current head position and yaw"),VR->MenuAnchorFrame.Equals(
			FTransform(FRotator(0,VR->Head->GetComponentRotation().Yaw,0),VR->Head->GetComponentLocation()),.01f));
		TestTrue(TEXT("Reopening inventory preserves the user's placement offset"),VR->Settings->MenuViewOffset.Equals(OriginalMenuOffset,.01f));
		VR->Head->AddWorldOffset(FVector(20,10,0)); VR->Head->AddWorldRotation(FRotator(0,15,0)); VR->UpdatePanels();
		TestTrue(TEXT("World inventory stays pinned while open"),VR->RetailPanel->GetComponentTransform().Equals(ReopenedMenuPose,.01f));
		VR->RevealRetailDialog(200);
		TestTrue(TEXT("Refreshing an open dialog does not recapture its world anchor"),VR->RetailPanel->GetComponentTransform().Equals(ReopenedMenuPose,.01f));
		VR->bUseDesktopMenu=DesktopMenuBeforeReopen;
		TestNull(TEXT("World inventory does not inherit camera late-update motion"),VR->RetailPanel->GetAttachParent());
		VR->Settings->MenuAnchorMode=1; VR->bMenuAnchorReady=false; VR->UpdateMenuAnchor(.016f);
		const FTransform BodyMenuAnchor=VR->MenuAnchorFrame;
		VR->Head->AddWorldRotation(FRotator(0,.1f,0)); VR->UpdateMenuAnchor(.016f);
		TestTrue(TEXT("Body inventory ignores tiny head rotations while reading"),VR->MenuAnchorFrame.Equals(BodyMenuAnchor,.01f));

		VR->Head->SetWorldTransform(HeadBeforeMenuPin); VR->Settings->MenuAnchorMode=2; VR->bMenuAnchorReady=false; VR->UpdatePanels();
		PointAt(VR->MenuControlsPanel,FVector2D(400,24)); VR->Trigger(false,true);
		VR->bInventoryOpen=false; VR->UpdatePanels();
		TestEqual(TEXT("Hiding a menu ends its active resize"),VR->PanelEditHand,INDEX_NONE);
		VR->Trigger(false,false); VR->bInventoryOpen=MenuBeforePlacement; VR->UpdatePanels();
		PointAt(VR->MenuControlsPanel,FVector2D(240,24)); VR->Trigger(false,true); VR->Trigger(false,false);
		TestTrue(TEXT("Menu toolbar relocks placement"),VR->Settings->bMenuLocked);
		VR->NativeCompass->UpdateHeading(179,1.f,true);
		const float OldAngle=VR->NativeCompass->NorthAngle;
		VR->NativeCompass->UpdateHeading(-179,1.f/90.f);
		TestTrue(TEXT("Compass crosses north using shortest rotation"),FMath::Abs(FMath::FindDeltaAngleRadians(OldAngle,VR->NativeCompass->NorthAngle))<FMath::DegreesToRadians(2.f));
		VR->NextNativeHUDUpdate=FPlatformTime::Seconds()+60.;
		const float BeforeHeading=VR->NativeCompass->NorthAngle; VR->Head->AddWorldRotation(FRotator(0,20,0)); VR->UpdateNativeHUD(true);
		TestFalse(TEXT("Compass heading updates between object scans"),FMath::IsNearlyEqual(BeforeHeading,VR->NativeCompass->NorthAngle));
		FACEVRRadarMarker NorthMarker; NorthMarker.Point=FVector2f(300,200);
		VR->NativeCompass->UpdateHeading(90,1.f,true);
		TestTrue(TEXT("North radar blip rotates with labels and click targets"),VR->NativeCompass->MarkerPoint(NorthMarker).Equals(FVector2f(200,100),.01f));
		VR->Head->SetWorldTransform(OldHead); VR->NextNativeHUDUpdate=0; VR->bCompassAnchorReady=false; VR->bVitalsAnchorReady=false; VR->UpdatePanels();
		// Put settings directly behind the retail inventory and verify the top
		// visual layer also receives the controller, independent of distance.
		const FTransform SettingsBefore = VR->SettingsPanel->GetComponentTransform();
		VR->SettingsPanel->SetWorldTransform(VR->RetailPanel->GetComponentTransform());
		VR->SettingsPanel->AddWorldOffset(-VR->RetailPanel->GetForwardVector()*2.f);
		VR->SettingsPanel->SetVisibility(true);
		VR->SettingsPanel->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		VR->FocusPanelSurface(VR->SettingsPanel);
		PointAt(VR->SettingsPanel,FVector2D(300,100));
		TestEqual(TEXT("Overlapping VR settings take hit priority over main UI"),VR->RightPointer->GetHoveredWidgetComponent(),VR->SettingsPanel.Get());
		VR->SettingsPanel->SetVisibility(false);VR->SettingsPanel->SetWorldTransform(SettingsBefore);VR->UpdatePanels();

		VR->UpdatePanels();
		{
			auto& Session=*VR->Client->Session;
			Session.Book.bOpen=true; Session.Book.BookGuid=70001;Session.Book.CurrentPage=0;Session.Book.NumPages=2;
			FString LongPage;for(int32 I=0;I<100;++I)LongPage+=TEXT("A long page must remain readable and scroll within the parchment.\n");
			Session.Book.Pages={LongPage,TEXT("Second page.")};
			Session.Book.PageTextIncluded={true,true};
			Gameplay->ShowPanelPage(TEXT("BookPanel_Field"));Gameplay->RefreshBookOverlays();PaintRetail();Gameplay->RefreshBookOverlays();PaintRetail();
			TestNotNull(TEXT("Book uses a bounded scrolling text container"),Gameplay->BookScroll.Get());
			const auto Bar=Gameplay->GetBookScrollbar();
			TestTrue(TEXT("Book resolves its authored parchment scrollbar"),Bar.IsValid());
			if(Bar && Gameplay->BookScroll)
			{
				AddInfo(FString::Printf(TEXT("Book bar %s, %dx%d, scrollmax=%.1f"),*Bar->ElementName,Bar->Width,Bar->Height,Gameplay->BookScroll->GetScrollOffsetOfEnd()));
				TestTrue(TEXT("Long book page has real overflow"),Gameplay->BookScroll->GetScrollOffsetOfEnd()>500);
				const FVector2D Click=FVector2D(Bar->GetScreenOrigin())+FVector2D(Bar->Width*.5,Bar->Height*.65);
				TestTrue(TEXT("Book thumb can be grabbed"),Gameplay->TryBeginScrollbarDrag(Click));
				Gameplay->UpdateScrollbarDrag(Click);Gameplay->CancelPointerGestures();PaintRetail();
				TestTrue(TEXT("Dragging book thumb scrolls text"),Gameplay->BookScroll->GetScrollOffset()>100);
				if(auto* RT=VR->RetailPanel->GetRenderTarget())
				{
					TArray<FColor> Pixels;RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> PNG;
					FImageUtils::PNGCompressImageArray(RT->SizeX,RT->SizeY,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/VR/BookScroll.png")));
				}
				Session.Book.CurrentPage=1;Gameplay->RefreshBookOverlays();PaintRetail();
				TestEqual(TEXT("Changing book pages resets scroll to the top"),Gameplay->BookScroll->GetScrollOffset(),0.f);
			}
			Session.Book.Pages[1].Reset();Session.Book.PageTextIncluded[1]=true;
			Gameplay->RefreshBookOverlays();
			TestTrue(TEXT("Received blank page renders blank instead of an endless loading label"),Gameplay->BookPageTextLabel->GetText().IsEmpty());
			Session.Book.PageTextIncluded[1]=false;Gameplay->RefreshBookOverlays();
			TestTrue(TEXT("Unreceived page still reports loading"),Gameplay->BookPageTextLabel->GetText().ToString().Contains(TEXT("Loading")));
			Session.Book={};Gameplay->HidePanel();
		}
		FACEWorldObject Bread; Bread.Guid = 301; Bread.ContainerId = 100; Bread.Name = TEXT("VR vendor test bread");
		Bread.ItemType = ACEItemType::Food; Bread.StackSize = 3; Bread.Value = 30; Bread.IconId = 0x060010FA;
		VR->Client->Session->WorldObjects.Add(Bread.Guid, Bread);
		VR->Client->Session->VendorItemTypes = ACEItemType::Food;
		VR->Client->Session->VendorMaxValue = -1;
		Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field")); Gameplay->ShowVendorPanel(400);
		{
			FACEWorldObject Stock;Stock.Guid=75002;Stock.Name=TEXT("Vendor arrow stock");Stock.ItemType=ACEItemType::MissileWeapon;
			Stock.StackSize=1;Stock.MaxStackSize=2500;Stock.VendorQuantityAvailable=-1;Stock.Value=1;Stock.IconId=0x060010FA;
			VR->Client->Session->WorldObjects.Add(Stock.Guid,Stock);VR->Client->Session->VendorMerchandise={Stock};
			Gameplay->VendorSelectedGuid=Stock.Guid;VR->Client->SelectObject(Stock.Guid);Gameplay->TickRefresh();PaintRetail();
			TestEqual(TEXT("Unlimited vendor stock exposes the entire stack quantity range"),Gameplay->SelectedStackMax,2500);
			Gameplay->SelectedStackAmount=200;Gameplay->HandleSelectionChanged(VR->Client->GetSelectedObject());
			TestEqual(TEXT("Vendor refresh retains chosen purchase quantity"),Gameplay->SelectedStackAmount,200);
			Gameplay->AddSelectedVendorItemToBuyCart();
			TestTrue(TEXT("Buy cart receives the chosen quantity rather than one"),Gameplay->VendorBuyCart.ContainsByPredicate([&](const auto& P){return P.Value==Stock.Guid && P.Key==200;}));
			const FTransform PanelPose=VR->RetailPanel->GetComponentTransform();VR->Head->AddWorldRotation(FRotator(0,30,0));VR->RevealRetailDialog(400);
			TestTrue(TEXT("Refreshing an open vendor leaves its VR panel anchored"),VR->RetailPanel->GetComponentTransform().Equals(PanelPose,.001));
			VR->Head->AddWorldRotation(FRotator(0,-30,0));Gameplay->VendorBuyCart.Reset();VR->Client->Session->VendorMerchandise.Reset();VR->Client->Session->WorldObjects.Remove(Stock.Guid);VR->Client->SelectObject(0);
		}
		Gameplay->TickRefresh(); PaintRetail();
		if (auto Tab = Manager->FindElementByName(TEXT("VendorSellTab")))
		{
			PointAt(VR->RetailPanel, FVector2D(Tab->GetScreenOrigin()) + FVector2D(Tab->Width, Tab->Height) * .5);
			VR->Trigger(false, true); VR->Trigger(false, false);
			TestEqual(TEXT("Controller clicks switch the actual vendor tab"), Gameplay->ActiveVendorPage, 2);
		}
		PaintRetail();
		const int32 BreadIndex = Gameplay->InventorySlotGuids.Find(Bread.Guid);
		if (TestTrue(TEXT("Inventory fixture exposes the sellable item"), BreadIndex != INDEX_NONE))
		{
			const FVector2D ItemPoint = LocalCenter(Gameplay->InventorySlots[BreadIndex]);
			Gameplay->LastInvClickGuid = Bread.Guid; Gameplay->LastInvClickTime = FPlatformTime::Seconds() - .65;
			PointAt(VR->RetailPanel, ItemPoint); VR->Trigger(false, true);
			TestTrue(TEXT("VR accepts a comfortably spaced second inventory click"), Gameplay->bInvDoubleClickPending);
			PointAt(VR->RetailPanel, ItemPoint + FVector2D(18, 0));
			TestFalse(TEXT("Small controller jitter does not turn a double click into a drag"), Gameplay->bInvDragActive);
			Gameplay->CancelPointerGestures(); VR->Trigger(false, false);
			PointAt(VR->RetailPanel, LocalCenter(Gameplay->InventorySlots[BreadIndex])); VR->Trigger(false, true);
			TestTrue(TEXT("Item press preserves inventory and starts a drag"), VR->bInventoryOpen && Gameplay->bInvDragPending);
			if (auto SellList = Manager->FindElementUnder(TEXT("VendorSellPage"), TEXT("VendorSellList")))
			{
				PointAt(VR->RetailPanel, FVector2D(SellList->GetScreenOrigin()) + FVector2D(15, 15)); VR->Trigger(false, false);
				TestTrue(TEXT("Controller drag stages inventory in the vendor sell cart"), Gameplay->VendorSellCart.ContainsByPredicate([&](const auto& P) { return P.Value == Bread.Guid && P.Key == 3; }));
			}
		}
		Gameplay->HandleEscape(); PaintRetail();
		// Exercise release outside the UI, including a quick drag with no canvas
		// move event, using the opposite hand from the configured weapon hand.
		auto* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		auto Address = Sockets->CreateInternetAddr(); bool Valid = false; Address->SetIp(TEXT("127.0.0.1"), Valid); Address->SetPort(0);
		auto* Receiver = Sockets->CreateSocket(NAME_DGram, TEXT("VR item transfer fixture"), false);
		if (Receiver && Receiver->Bind(*Address) && BreadIndex != INDEX_NONE)
		{
			Receiver->GetAddress(*Address); auto& Session = *VR->Client->Session;
			Session.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("VR item transfer sender"), false);
			Session.ServerC2SAddr = Address; Session.IssacClient = MakeUnique<FACEIsaac>(123);
			// Inspect uses the actual pointer, sends identify once, and cannot equip,
			// use, drag, or fall through to the previous spell while over inventory.
			Session.CachedC2SPackets.Reset();
			PointAt(VR->RetailPanel, LocalCenter(Gameplay->InventorySlots[BreadIndex]));
			VR->FeedbackHand = 1;
			const uint64 IdentifyBefore = VR->Client->GetIdentifyRequestSerial();
			VR->PreviousSpell();
			TestEqual(TEXT("B requests one appraisal for the pointed item"), VR->Client->GetIdentifyRequestSerial(), IdentifyBefore + 1);
			TestEqual(TEXT("B identifies the hovered inventory item"), VR->Client->GetIdentifyRequestGuid(), Bread.Guid);
			TestFalse(TEXT("B does not start an inventory drag"), Gameplay->bInvDragPending || Gameplay->bInvDragActive);
			TestTrue(TEXT("B keeps the inventory panel open"), VR->bInventoryOpen);
			int32 IdentifyActions=0;
			for (const auto& Packet : Session.CachedC2SPackets)
			{
				FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
				if (Wire.ReadUInt32()!=ACEOpcode::GameAction) continue; Wire.ReadUInt32();
				const uint32 Action=Wire.ReadUInt32();
				if (Action==ACEGameAction::QueryHealth) continue; // Retail selection also queries target health.
				TestEqual(TEXT("Inspect sends no use/equip/drop action"), Action, ACEGameAction::IdentifyObject);
				++IdentifyActions;
			}
			TestEqual(TEXT("B produces exactly one item action"), IdentifyActions, 1);
			VR->bRightPointerPressed=true; VR->PreviousSpell();
			TestEqual(TEXT("B cannot interrupt a held inventory gesture"), VR->Client->GetIdentifyRequestSerial(), IdentifyBefore+1);
			VR->bRightPointerPressed=false;
			VR->bActive=false; VR->PreviousSpell();
			TestEqual(TEXT("B has no effect outside VR"), VR->Client->GetIdentifyRequestSerial(), IdentifyBefore+1);
			VR->bActive=true;
			Gameplay->CancelPointerGestures();
			{
				TGuardValue<uint32> StandardServer(Session.VRCapabilities,0);
				Gameplay->ShowPanelPage(TEXT("InventoryPanel_Field"));PaintRetail();
				TGuardValue<int32> Usable(Session.WorldObjects[Bread.Guid].ItemUseable,8);
				for(bool Left:{false,true})
				{
					PointAt(VR->RetailPanel,LocalCenter(Gameplay->InventorySlots[BreadIndex]));
					const auto Pose=VR->RightAim->GetComponentTransform();
					VR->LeftAim->SetWorldLocationAndRotation(FVector(0,0,200),FRotator(0,180,0));
					if(Left)
					{
						VR->LeftAim->SetWorldTransform(Pose);
						VR->RightAim->SetWorldLocationAndRotation(FVector(0,0,200),FRotator(0,180,0));
					}
					// Do not tick the pointers: an action must refresh the current aim.
					VR->FeedbackHand=Left?0:1;Session.ClearPendingUse();Session.CachedC2SPackets.Reset();
					Gameplay->CancelPendingUseWith();VR->SelectPressed();
					int32 Uses=0;
					for(const auto& Packet:Session.CachedC2SPackets)
					{
						FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);
						if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;Wire.ReadUInt32();
						if(Wire.ReadUInt32()==ACEGameAction::Use){++Uses;TestEqual(TEXT("Either hand uses its pointed retail item"),Wire.ReadUInt32(),uint32(Bread.Guid));}
					}
					TestEqual(TEXT("Primary action refreshes either controller and uses once on a stock server"),Uses,1);
				}
				Session.ClearPendingUse();PollHands();PaintRetail();
			}
			FACEWorldObject NPC; NPC.Guid = 601; NPC.ItemType = ACEItemType::Creature; NPC.bHasPosition = true;
			NPC.UseRadius = .6f; NPC.Position.CellId = 1;
			auto* Actor = World->SpawnActor<AACEWorldEntityActor>();
			for (float Distance : {20.f, 1.f})
			{
				NPC.Position.Location = FVector(0, Distance, 0); Session.WorldObjects.Add(NPC.Guid, NPC);
				Actor->InitializeFromObject(NPC, 100, false); Actor->ConfigureWorldCollision(true);
				Session.CachedC2SPackets.Reset();
				// Start with the left controller and point the right controller away.
				PointAt(VR->RetailPanel, LocalCenter(Gameplay->InventorySlots[BreadIndex]));
				VR->LeftAim->SetWorldTransform(VR->RightAim->GetComponentTransform()); VR->LeftPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
				VR->Trigger(true, true);
				TestTrue(TEXT("Off-hand item press starts a pending transfer"), Gameplay->bInvDragPending);
				const FVector Center = Actor->FindComponentByClass<UCapsuleComponent>()->Bounds.Origin;
				VR->LeftAim->SetWorldLocationAndRotation(Center - FVector(80, 0, 0), FRotator::ZeroRotator);
				VR->RightAim->SetWorldLocationAndRotation(FVector(0, 0, 200), FRotator(0, 180, 0));
				VR->LeftPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
				VR->Trigger(true, false);
				bool Gave = false;
				for (const auto& Packet : Session.CachedC2SPackets)
				{
					FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
					if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue; Wire.ReadUInt32();
					if (Wire.ReadUInt32() != ACEGameAction::GiveObjectRequest) continue;
					Gave = true; TestEqual(TEXT("Transfer targets the releasing hand's NPC"), Wire.ReadUInt32(), 601u);
					TestEqual(TEXT("Transfer preserves the dragged item"), Wire.ReadUInt32(), 301u);
				}
				TestEqual(TEXT("World transfer requires reach and never asks the server to auto-walk"), Gave, Distance == 1.f);
			}
			Actor->Destroy(); Session.WorldObjects.Remove(NPC.Guid);
			const uint32 DropCapabilities = Session.VRCapabilities;
			for (bool VRRelease : {false, true}) for (int32 Amount : {3, 1})
			{
				Session.VRCapabilities = VRRelease ? DropCapabilities | 1024u : DropCapabilities & ~1024u;
				Session.CachedC2SPackets.Reset(); Gameplay->LastInvClickGuid = 0;
				Gameplay->SelectInventoryGuid(Bread.Guid);
				Gameplay->SelectedStackAmount = Amount;
				PointAt(VR->RetailPanel, LocalCenter(Gameplay->InventorySlots[BreadIndex])); VR->Trigger(false, true);
				TestEqual(TEXT("VR drag freezes quantity when grabbed"), Gameplay->InvDragAmount, Amount);
				Gameplay->SelectedStackAmount = 3; // Later toolbar changes cannot alter the release.
				VR->RightAim->SetWorldLocationAndRotation(FVector(0, 0, 200), FRotator(-45, 180, 0));
				VR->Trigger(false, false);
				int32 Drops = 0;
				for (const auto& Packet : Session.CachedC2SPackets)
				{
					FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
					if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue; Wire.ReadUInt32();
					const uint32 Action = Wire.ReadUInt32();
					if (VRRelease && Action == 0xF7D0u)
					{
						TestEqual(TEXT("VR release protocol version"), Wire.ReadUInt32(), 1u);
						TestEqual(TEXT("VR drop has an explicit release action"), Wire.ReadUInt32(), 6u);
						Wire.Skip(12);
						TestEqual(TEXT("VR release preserves inventory item"), Wire.ReadUInt32(), uint32(Bread.Guid));
						TestEqual(TEXT("VR release preserves selected stack amount"), Wire.ReadUInt32(), Amount == 3 ? 0u : 1u);
						const float X=Wire.ReadFloat(), Y=Wire.ReadFloat(), Z=Wire.ReadFloat();
						TestTrue(TEXT("VR release sends a reachable hand position"), FMath::IsFinite(X+Y+Z) && Z>=.1f && Z<=2.5f);
						TestEqual(TEXT("VR release has no trailing fields"), Wire.Remaining(), 0);
						++Drops; continue;
					}
					if (Action != ACEGameAction::DropItem && Action != ACEGameAction::StackableSplitTo3D) continue;
					TestFalse(TEXT("Supported VR release never also sends desktop placement"), VRRelease);
					++Drops; TestEqual(TEXT("Ground release preserves stack or selected split"), Action,
						Amount == 3 ? ACEGameAction::DropItem : ACEGameAction::StackableSplitTo3D);
					TestEqual(TEXT("Ground release sends the dragged item to the server"), Wire.ReadUInt32(), uint32(Bread.Guid));
					if (Amount == 1) TestEqual(TEXT("Split preserves selected amount"), Wire.ReadUInt32(), 1u);
				}
				TestEqual(TEXT("One controller release sends exactly one ground drop"), Drops, 1);
				TestFalse(TEXT("Ground release clears the captured inventory gesture"), Gameplay->IsInventoryDragActive());
			}
			VR->bActive=false;
			TestFalse(TEXT("Desktop uses its unchanged placement even on a VR-capable server"), VR->TryDropInventoryItem(Bread.Guid));
			VR->bActive=true; Session.VRCapabilities=DropCapabilities;
			FACEWorldObject Falling = Bread;
			// Supply an authored physical body; SetupId=0 has no movement spheres
			// and retail deliberately leaves sphere-less decorations stationary.
			Falling.SetupId=0x02000A97;
			Falling.Guid=602; Falling.ContainerId=Falling.WielderId=Falling.ParentGuid=0;
			Falling.PhysicsState=ACEPhysicsState::Gravity;
			Falling.bHasPosition=Falling.bHasVelocity=true;
			Falling.Position.CellId=1; Falling.Position.Location=FVector(0,0,1.1f);
			Falling.Position.bHasVelocity=false; Falling.Position.bIsGrounded=false;
			Falling.Velocity=FVector(0,0,-.15f);
			auto* Dropped=World->SpawnActor<AACEWorldEntityActor>();
			Dropped->InitializeFromObject(Falling,100,false);
			const double ReleaseZ=Falling.Position.ToUnrealLocation(100).Z;
			TestTrue(TEXT("CreateObject fall velocity preserves hand-height spawn without floor snapping"),FMath::Abs(Dropped->GetActorLocation().Z-ReleaseZ)<.1);
			Dropped->Tick(.1f);
			TestTrue(TEXT("Released inventory model integrates gravity"),Dropped->GetActorLocation().Z<ReleaseZ-3.f);
			Dropped->Destroy();
			Falling.Velocity=FVector(.8f,0,.55f);
			auto* Tossed=World->SpawnActor<AACEWorldEntityActor>();Tossed->InitializeFromObject(Falling,100,false);
			const FVector TossStart=Tossed->GetActorLocation(); const FQuat TossRotation=Tossed->GetActorQuat();
			Tossed->Tick(.05f);
			TestTrue(TEXT("Outward release rises slightly and travels away from the hand"),Tossed->GetActorLocation().Z>TossStart.Z && FMath::Abs(Tossed->GetActorLocation().X-TossStart.X)>3.5f);
			TestTrue(TEXT("A dropped item does not turn to face its velocity"),Tossed->GetActorQuat().Equals(TossRotation,.001f));
			Tossed->Tick(.2f);
			TestTrue(TEXT("The short outward arc turns downward under gravity"),Tossed->GetActorLocation().Z<TossStart.Z-10.f);
			Tossed->Destroy();
			{
				const bool OldMenu=VR->bInventoryOpen;const int32 OldMode=Session.PlayerVitals.CombatMode;
				VR->bInventoryOpen=false;Session.PlayerVitals.CombatMode=ACECombatMode::NonCombat;VR->UpdatePanels();PollHands();VR->ResetHandContacts();
				const FVector Eyes=VR->Head->GetComponentLocation();
				VR->Head->SetWorldRotation(FRotator::ZeroRotator);
				VR->LeftGrip->SetWorldLocation(Eyes+FVector(55,-20,-20));VR->RightGrip->SetWorldLocation(Eyes+FVector(55,20,-20));
				FACEWorldObject Stone;Stone.Guid=75001;Stone.SetupId=0x02000001;Stone.ItemType=ACEItemType::LifeStone;Stone.ObjectDescriptionFlags=ACEObjectDescFlag::Stuck;
				Stone.ItemUseable=32;Stone.UseRadius=3;Stone.bHasPosition=true;Stone.Position.CellId=1;
				Stone.Position.SetLocationFromUnreal(Eyes+FVector(65,0,-100),100);Session.WorldObjects.Add(Stone.Guid,Stone);
				auto* StoneActor=World->SpawnActor<AACEWorldEntityActor>();StoneActor->InitializeFromObject(Stone,100,true);
				auto* Surface=NewObject<UBoxComponent>(StoneActor);StoneActor->AddInstanceComponent(Surface);Surface->SetupAttachment(StoneActor->GetRootComponent());
				Surface->SetBoxExtent(FVector(3,60,100));Surface->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Surface->SetCollisionResponseToAllChannels(ECR_Block);Surface->RegisterComponent();
				Surface->SetWorldLocation(Eyes+FVector(65,0,-40));
				VR->RightAim->SetWorldLocationAndRotation(Eyes+FVector(0,0,-20),FRotator::ZeroRotator);
				const uint64 BeforeID=VR->Client->GetIdentifyRequestSerial();VR->PreviousSpell();
				TestEqual(TEXT("B identifies the world object in peace with menus closed"),VR->Client->GetIdentifyRequestSerial(),BeforeID+1);
				TestEqual(TEXT("World B identifies the pointed stone"),VR->Client->GetIdentifyRequestGuid(),Stone.Guid);
				VR->bInventoryOpen=true;VR->PreviousSpell();TestEqual(TEXT("Open UI cannot identify through itself into the world"),VR->Client->GetIdentifyRequestSerial(),BeforeID+1);VR->bInventoryOpen=false;
				Session.CachedC2SPackets.Reset();VR->ContactTriangles.Reset();VR->MoveStick=FVector2D::ZeroVector;
				// The first gesture after closing a menu must not require an unseen
				// retract/rearm gesture. Use a tall visible model whose center is
				// above the gaze cone, while its surface is directly ahead.
				VR->bTouchUseArmed=true;VR->bTouchUsePrevious=false;
				FACEVRContactTriangle Touch;Touch.ObjectGuid=Stone.Guid;Touch.A=Eyes+FVector(65,-100,-100);Touch.B=Eyes+FVector(65,100,-100);Touch.C=Eyes+FVector(65,0,100);
				Touch.Bounds=FBox(Touch.A,Touch.A);Touch.Bounds+=Touch.B;Touch.Bounds+=Touch.C;
				StoneActor->AddActorWorldOffset(FVector(110,0,0));Stone.Position.SetLocationFromUnreal(StoneActor->GetActorLocation(),100);Session.WorldObjects[Stone.Guid]=Stone;
				StoneActor->SetActorScale3D(FVector(1,1,6));
				FBox TallBounds(ForceInit);StoneActor->Appearance->GetVisualWorldBounds(TallBounds);
				TestTrue(TEXT("Tall usable fixture has its center outside the gaze cone"),FVector::DotProduct((TallBounds.GetCenter()-Eyes).GetSafeNormal(),VR->Head->GetForwardVector())<.8f);
				auto UseCount=[&](){int32 Count=0;for(const auto& Packet:Session.CachedC2SPackets){FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;Wire.ReadUInt32();if(Wire.ReadUInt32()==ACEGameAction::Use)++Count;}return Count;};
				VR->LeftGrip->AddWorldOffset(FVector(-50,0,0));for(int32 I=0;I<60;++I)VR->UpdateTwoHandUse(.02f);TestEqual(TEXT("One hand alone cannot use an object"),UseCount(),0);
				VR->LeftGrip->AddWorldOffset(FVector(50,0,0));VR->MoveStick=FVector2D(0,1);for(int32 I=0;I<60;++I)VR->UpdateTwoHandUse(.02f);TestEqual(TEXT("Walking past with both hands cannot use an object"),UseCount(),0);VR->MoveStick=FVector2D::ZeroVector;
				for(int32 I=0;I<10;++I)VR->UpdateTwoHandUse(.02f);TestEqual(TEXT("Brief two-hand brush does not use"),UseCount(),0);
				for(int32 I=0;I<50;++I)VR->UpdateTwoHandUse(.02f);TestEqual(TEXT("Deliberate two-hand reach within use range sends one use without mesh contact"),UseCount(),1);
				for(int32 I=0;I<60;++I)VR->UpdateTwoHandUse(.02f);TestEqual(TEXT("Holding both hands cannot repeat use"),UseCount(),1);
				VR->ContactTriangles.Reset();VR->UpdateTwoHandUse(.02f);Session.PlayerVitals.CombatMode=ACECombatMode::Melee;VR->ContactTriangles.Add(Touch);
				for(int32 I=0;I<60;++I)VR->UpdateTwoHandUse(.02f);TestEqual(TEXT("Combat disables contact use"),UseCount(),1);
				StoneActor->Destroy();Session.WorldObjects.Remove(Stone.Guid);VR->ContactTriangles.Reset();VR->ResetHandContacts();
				Session.PlayerVitals.CombatMode=OldMode;VR->bInventoryOpen=OldMenu;PollHands();VR->UpdatePanels();
			}
			Session.SocketC2S->Close(); Sockets->DestroySocket(Session.SocketC2S); Session.SocketC2S = nullptr;
		}
		else AddError(TEXT("World transfer loopback fixture could not initialize"));
		if (Receiver) Sockets->DestroySocket(Receiver);
		VR->SelectSpell(2); PaintRetail();
		if (auto* Surface = VR->WristPanel->GetRenderTarget())
		{
			TArray<FColor> Pixels; Surface->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
			TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Surface->SizeX, Surface->SizeY, Pixels, PNG);
			FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectSavedDir() / TEXT("Automation/VR/RetailWristHotbar.png")));
			TestTrue(TEXT("Retail wrist surface paints its real icons"), Pixels.ContainsByPredicate([](FColor P) { return P.R > 80 || P.G > 80 || P.B > 80; }));
		}
		VR->ToggleInventory();
		TestFalse(TEXT("Explicitly closing inventory hides its scene quad"), VR->RetailPanel->bRenderInMainPass);
		for (float Roll : {-170.f, -90.f, 0.f, 90.f, 170.f})
		{
			VR->LeftGrip->SetRelativeRotation(FRotator(0, 0, Roll)); VR->UpdatePanels();
			TestTrue(TEXT("Turning the wrist cannot turn text away from the eyes"), FVector::DotProduct(VR->WristPanel->GetForwardVector(),
				(VR->Head->GetComponentLocation() - VR->WristPanel->GetComponentLocation()).GetSafeNormal()) > .999);
		}
		const FVector OldWrist = VR->WristPanel->GetComponentLocation();
		VR->LeftGrip->AddWorldOffset(FVector(10, 0, 0)); VR->UpdatePanels(1.f / 90.f);
		const float WristStep = FVector::Distance(OldWrist, VR->WristPanel->GetComponentLocation());
		TestTrue(TEXT("Wrist stabilization filters a tracked-position step"), WristStep > 0.f && WristStep < 5.f);
		const FQuat LookingAtWrist = VR->Head->GetComponentQuat();
		VR->Head->SetWorldRotation(FRotator(0, 180, 0)); VR->UpdatePanels(.1f);
		TestTrue(TEXT("Brief gaze loss does not flicker the wrist"), VR->WristPanel->IsVisible());
		VR->UpdatePanels(.1f);
		TestTrue(TEXT("Looking away keeps the combat wrist hotbar visible"), VR->WristPanel->IsVisible());
		VR->Head->SetWorldRotation(LookingAtWrist); VR->UpdatePanels();
		TestTrue(TEXT("Looking back reveals the wrist hotbar"), VR->WristPanel->IsVisible());
		{
			auto& Session=*VR->Client->Session; const auto Bars=Session.SpellBars; const int32 Tab=Session.ActiveSpellBar;
			const int32 Mode=Session.PlayerVitals.CombatMode;Session.PlayerVitals.CombatMode=ACECombatMode::NonCombat;
			VR->ToggleSpellWheel();TestFalse(TEXT("No wand means no spell wheel"),VR->bSpellWheelOpen);
			FACEWorldObject WheelWand;WheelWand.Guid=75002;WheelWand.WielderId=Session.PlayerGuid;
			WheelWand.CurrentWieldedLocation=ACEEquipMask::Held;WheelWand.ItemType=ACEItemType::Caster;Session.WorldObjects.Add(WheelWand.Guid,WheelWand);
			Session.SpellBars.SetNum(8); Session.SpellBars[0]={1,2,3}; Session.SpellBars[1].Reset(); Session.ActiveSpellBar=0;
			VR->TurnStick=FVector2D::ZeroVector; VR->ToggleSpellWheel();
			TestTrue(TEXT("Left-stick click opens the radial picker without blocking movement"),VR->bSpellWheelOpen && !VR->IsInputBlocked());
			VR->TurnStick=FVector2D(0,1); VR->UpdateSpellWheel();
			TestEqual(TEXT("Up selects the first spell in the current tab"),VR->WheelHover,0);
			for(int32 I=0;I<5;++I){VR->UpdateSpellWheel();VR->SpellWheelPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();}
			if(auto* RT=VR->SpellWheelPanel->GetRenderTarget())
			{
				TArray<FColor> Pixels; RT->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
				TestTrue(TEXT("Spell wheel renders icons and ring"),Pixels.ContainsByPredicate([](FColor P){return P.A>100 && P.R>80;}));
				TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(RT->SizeX,RT->SizeY,Pixels,PNG);
				FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/VR/SpellWheel.png")));
			}
			VR->SelectPressed();TestTrue(TEXT("A changes to the next tab, including an empty tab"),Session.ActiveSpellBar==1 && VR->WheelSpells.IsEmpty());
			const uint32 Sequence=Session.VRSequence;VR->ConfirmWheelSpell();
			TestTrue(TEXT("Empty tab cannot confirm or cast"),VR->bSpellWheelOpen && Session.VRSequence==Sequence);
			VR->PreviousSpell();VR->TurnStick=FVector2D::ZeroVector;VR->UpdateSpellWheel();VR->TurnStick=FVector2D(0,1);VR->UpdateSpellWheel();
			VR->Trigger(VR->Settings->bLeftHanded,true); VR->Trigger(VR->Settings->bLeftHanded,false);
			TestTrue(TEXT("Trigger confirms selection without casting or leaving a held trigger"),!VR->bSpellWheelOpen && VR->SelectedSpell==1 && Session.VRSequence==Sequence);
			TestEqual(TEXT("Selecting from the wheel leaves peace stance unchanged"),Session.PlayerVitals.CombatMode,ACECombatMode::NonCombat);
			Session.SpellBars[0]={1,2,3,1,2,3,1,2,3,1,2,3,2}; VR->ToggleSpellWheel(); VR->ChangeWheelPage(1);
			TestEqual(TEXT("Long hotbars have a second radial page"),VR->WheelPage,1);
			VR->ToggleInventory();TestFalse(TEXT("X cancels the wheel without opening inventory"),VR->bSpellWheelOpen || VR->bInventoryOpen);
			Session.SpellBars[0]={1,2,3}; Session.SpellBars[1]={4,5,6};
			Gameplay->SetCombatSpellBar(0); VR->SelectSpell(3); VR->CycleSpellBar(1);
			TestEqual(TEXT("First VR visit selects the tab's first spell"),VR->SelectedSpell,4);
			VR->SelectSpell(6); VR->CycleSpellBar(-1);
			TestEqual(TEXT("Returning to a VR tab restores its cast spell"),VR->SelectedSpell,3);
			TestEqual(TEXT("Wrist cursor matches restored VR cast spell"),Gameplay->SelectedCombatSpellSlot,2);
			VR->ToggleSpellWheel(); VR->ChangeWheelTab(1);
			TestEqual(TEXT("Wheel tab restores its remembered spell"),VR->SelectedSpell,6);
			TestEqual(TEXT("Wheel highlight matches remembered spell"),VR->WheelHover,2);
			TestEqual(TEXT("Restoring tabs does not change stance"),Session.PlayerVitals.CombatMode,ACECombatMode::NonCombat);
			TestEqual(TEXT("Restoring tabs never casts"),Session.VRSequence,Sequence);
			VR->CloseSpellWheel();
			Session.SpellBars=Bars;Session.ActiveSpellBar=Tab;VR->SelectSpell(2);VR->TurnStick=FVector2D::ZeroVector;
			Session.WorldObjects.Remove(WheelWand.Guid);Session.PlayerVitals.CombatMode=Mode;
		}
		for(int32 Mode:{ACECombatMode::Melee,ACECombatMode::Missile,ACECombatMode::NonCombat,ACECombatMode::Magic})
		{
			VR->Client->Session->PlayerVitals.CombatMode=Mode;VR->UpdatePanels();
			TestEqual(TEXT("Only magic stance shows the wrist spellbar"),VR->WristPanel->IsVisible(),Mode==ACECombatMode::Magic);
		}
		{
			FACEWorldObject Mob;Mob.Guid=200001;Mob.ItemType=ACEItemType::Creature;Mob.SetupId=0x0200003D;
			auto* Enemy=World->SpawnActor<AACEWorldEntityActor>();Enemy->InitializeFromObject(Mob,100,true);
			Enemy->SetActorLocation(VR->Head->GetComponentLocation()+FVector(200,0,-100));
			VR->EnemyHealth(Mob.Guid,0.f);
			TestTrue(TEXT("One-hit kills never allocate a black meter"),VR->EnemyHealthBars.IsEmpty());
			VR->EnemyHealth(Mob.Guid,.75f);
			TestEqual(TEXT("Confirmed damage creates one overhead meter"),VR->EnemyHealthBars.Num(),1);
			if(!VR->EnemyHealthBars.IsEmpty())
			{
				auto& Bar=VR->EnemyHealthBars[0];
				TestTrue(TEXT("Overhead health is visible and uses authoritative fraction"),Bar.Panel->IsVisible() && Bar.Meter->GetFraction()==.75f);
				for(int32 Pass=0;Pass<5;++Pass){VR->UpdateEnemyHealthBars();Bar.Panel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();}
				if(auto* Target=Bar.Panel->GetRenderTarget())
				{
					TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
					int32 VisiblePixels=0,RedPixels=0;for(const auto P:Pixels){VisiblePixels+=P.A>100;RedPixels+=P.R>80 && P.R>P.G*2;}
					AddInfo(FString::Printf(TEXT("Enemy target %dx%d visible=%d red=%d"),Target->SizeX,Target->SizeY,VisiblePixels,RedPixels));
					TestTrue(TEXT("Vector overhead bar paints a clear red health fill"),RedPixels>Target->SizeX*Target->SizeY*.4f);
					TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Target->SizeX,Target->SizeY,Pixels,PNG);
					FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/VR/EnemyHealth.png")));
				}
				else AddError(TEXT("Overhead bar did not allocate a render target"));
				VR->EnemyHealth(Mob.Guid,.25f);TestEqual(TEXT("Subsequent damage updates the same bar"),Bar.Meter->GetFraction(),.25f);
				VR->EnemyHealth(Mob.Guid,1.f);TestEqual(TEXT("Healing updates that bar"),Bar.Meter->GetFraction(),1.f);
				Enemy->SetActorLocation(VR->Head->GetComponentLocation()+FVector(3000,0,-100));VR->UpdateEnemyHealthBars();
				TestTrue(TEXT("Distant targets retain readable angular width"),Bar.Panel->GetComponentScale().X*Bar.Panel->GetDrawSize().X>=400.f);
				TestEqual(TEXT("Health bar has a shorter profile"),Bar.Panel->GetDrawSize().Y,40.);
				VR->EnemyHealth(Mob.Guid,0.f);TestFalse(TEXT("Death immediately hides an existing meter"),Bar.Panel->IsVisible());
				Bar.Expires=0;VR->UpdateEnemyHealthBars();TestFalse(TEXT("Expired health bars hide"),Bar.Panel->IsVisible());
			}
			Enemy->Destroy();
		}
		VR->Settings->bPinHotbarToView = true; VR->Settings->MenuAnchorMode=0; VR->UpdatePanels();
		TestEqual(TEXT("Pinned retail menus use camera late update"), VR->RetailPanel->GetAttachParent(), static_cast<USceneComponent*>(VR->Head.Get()));
		TestEqual(TEXT("Pinned hotbar uses camera late update"), VR->WristPanel->GetAttachParent(), static_cast<USceneComponent*>(VR->Head.Get()));
		VR->Settings->bPinHotbarToView = false; VR->Settings->MenuAnchorMode=2; VR->UpdatePanels();
		PollHands();
		VR->RightPointer->TickComponent(.016f, LEVELTICK_All, nullptr);
		VR->Trigger(false, true); VR->Trigger(false, false);
		TestFalse(TEXT("A standard server does not block casting on VR capability negotiation"), VR->GetCastFeedback().Contains(TEXT("Waiting for server")));
		FACEBinaryWriter CombatAck; CombatAck.WriteUInt32(100); CombatAck.WriteUInt32(2); CombatAck.WriteUInt32(0xF7D0);
		CombatAck.WriteUInt32(1); CombatAck.WriteUInt32(15); CombatAck.WriteUInt32(0); CombatAck.WriteUInt32(0);
		FACEBinaryReader CombatAckInput(CombatAck.GetData()); VR->Client->Session->HandleGameEvent(CombatAckInput);
		const uint32 BeforeCast = VR->Client->Session->VRSequence;
		VR->LastCast=-100;
		VR->Trigger(false, true); VR->Trigger(false, false);
		TestEqual(TEXT("A trigger after selecting produces one VR spell action"), VR->Client->Session->VRSequence, BeforeCast+1u);
		{
			VR->LastCast=-100;
			const FTransform WristTransform=VR->WristPanel->GetComponentTransform();
			const FVector2D WristSize=VR->WristPanel->GetDrawSize();
			const FVector WristCenter=VR->RightAim->GetComponentLocation()+VR->RightAim->GetForwardVector()*100.f;
			VR->WristPanel->SetWorldLocationAndRotation(WristCenter,FRotationMatrix::MakeFromX(-VR->RightAim->GetForwardVector()).Rotator());
			VR->WristPanel->SetWorldScale3D(FVector(.06));VR->WristPanel->SetDrawSize(FVector2D(500,100));
			VR->WristPanel->AddWorldOffset(VR->WristPanel->GetUpVector()*3.5f);
			TestTrue(TEXT("Wrist border retains a one-centimeter safety margin"),VR->IsPointerNearPanel(false));
			VR->Trigger(false,true);VR->Trigger(false,false);
			TestTrue(TEXT("A guarded cast explains how to move the ray clear"),VR->GetCastFeedback().Contains(TEXT("panel")));
			VR->WristPanel->AddWorldOffset(VR->WristPanel->GetUpVector()*1.5f);
			TestFalse(TEXT("Aiming two centimeters beyond the wrist is a world gesture"),VR->IsPointerNearPanel(false));
			VR->WristPanel->SetWorldTransform(WristTransform);VR->WristPanel->SetDrawSize(WristSize);
			const uint32 BeforeAnalog=VR->Client->Session->VRSequence;
			VR->TriggerAxis(false,.8f); VR->TriggerAxis(false,1.f); VR->TriggerAxis(false,.5f); VR->TriggerAxis(false,0.f);
			TestEqual(TEXT("Touch-compatible analog-only trigger sends exactly one spell action"),VR->Client->Session->VRSequence,BeforeAnalog+1u);
			TestFalse(TEXT("Analog trigger release clears the mapped action"),VR->HeldControllerActions.Contains("VRRightTrigger"));
			const int32 SavedSpell=VR->SelectedSpell;
			for(int32 Spell:{1,2,23,24,2645})
			{
				VR->SelectedSpell=Spell; VR->UpdatePointerVisuals(true,true);
				TestEqual(TEXT("Magic pointer remains blue for every spell even with a persistent chat/menu visible"),VR->PointerBeamModes[1],1);
			}
			VR->SelectedSpell=SavedSpell;
			VR->LastCast=World->GetTimeSeconds();
		}
		VR->Client->OnChatMessage.Broadcast(TEXT("Not enough mana."), TEXT(""), ACEChatMessageType::TransientInfo);
		TestEqual(TEXT("Server cast errors remain visible on the wrist"), VR->GetCastFeedback(), FString(TEXT("Not enough mana.")));
		if (TestTrue(TEXT("Avatar fixture loads retail DAT"), Dat->LoadDatDirectory(RetailDatDirectory)))
		{
			auto CombatAddress = Sockets->CreateInternetAddr(); CombatAddress->SetIp(TEXT("127.0.0.1"), Valid); CombatAddress->SetPort(0);
			auto* CombatReceiver = Sockets->CreateSocket(NAME_DGram, TEXT("VR combat fixture"), false);
			TestTrue(TEXT("Combat packet receiver binds"), CombatReceiver && CombatReceiver->Bind(*CombatAddress));
			CombatReceiver->GetAddress(*CombatAddress);
			auto& CombatSession = *VR->Client->Session;
			CombatSession.SocketC2S = Sockets->CreateSocket(NAME_DGram, TEXT("VR combat sender"), false);
			CombatSession.ServerC2SAddr = CombatAddress;
			{
				TGuardValue<bool> InventoryClosed(VR->bInventoryOpen, false);
				TGuardValue<bool> SettingsClosed(VR->bSettingsOpen, false);
				TGuardValue<bool> Handedness(VR->Settings->bLeftHanded, false);
				TGuardValue<int32> Spell(VR->SelectedSpell, 1);
				const auto PreviousSelection = VR->Client->GetSelectedObject();
				const auto KnownSpells = CombatSession.KnownSpells;
				const auto Profiles = CombatSession.VRSpellProfiles;
				CombatSession.KnownSpells.Append({1, 2, 3, 5, 6, 23, 24, 27, 2645});
				for (int32 Id : {1, 2, 3, 5, 6, 23, 24, 2645}) CombatSession.VRSpellProfiles.Add(Id, FVector::ZeroVector);
				FACEWorldObject Recipient; Recipient.Guid = 71001; Recipient.Name = TEXT("Buff recipient");
				Recipient.ItemType = ACEItemType::Creature; Recipient.bIsPlayer = true;
				Recipient.bHasPosition = true; Recipient.Position.CellId = 1;
				CombatSession.WorldObjects.Add(Recipient.Guid, Recipient);
				auto* Player = World->SpawnActor<AACEWorldEntityActor>(); Player->InitializeFromObject(Recipient, 100, false);
				Player->ConfigureWorldCollision(true);
				auto* Body = Player->FindComponentByClass<UCapsuleComponent>();
				Body->SetCapsuleSize(30, 80);
				FACEWorldObject Other = Recipient; Other.Guid = 71002;
				CombatSession.WorldObjects.Add(Other.Guid, Other);
				auto PointAtPlayer = [&]()
				{
					// Restore tracked poses and aim forward below eye height. A hand
					// above the head points through its head-facing wrist panel.
					PollHands();
					VR->ResetHandContacts();
					const FVector Hand = VR->Head->GetComponentLocation() + FVector(2000, 0, -60);
					VR->WeaponGrip()->SetWorldLocationAndRotation(Hand, FRotator::ZeroRotator);
					VR->WeaponAim()->SetWorldLocationAndRotation(Hand, FRotator::ZeroRotator);
					FVector Origin, Direction; VR->GetSpellAim(Origin, Direction);
					Player->SetActorLocation(Origin + Direction * 500 - FVector(0, 0, 80));
					Body->SetWorldLocation(Origin + Direction * 500);
					VR->UpdatePanels();
					TestFalse(TEXT("Buff gesture points into the world, clear of wrist UI"), VR->IsPointerNearPanel(VR->Settings->bLeftHanded));
				};
				auto ClickSpell = [&](int32 ExpectedTarget, int32 ExpectedCount = 1)
				{
					CombatSession.CachedC2SPackets.Reset(); VR->LastCast = -100;
					VR->Trigger(VR->Settings->bLeftHanded, true); VR->Trigger(VR->Settings->bLeftHanded, false);
					int32 Casts = 0;
					for (const auto& Packet : CombatSession.CachedC2SPackets)
					{
						FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
						if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue; Wire.ReadUInt32();
						if (Wire.ReadUInt32() != 0xF7D0u) continue;
						Wire.ReadUInt32(); if (Wire.ReadUInt32() != 1u) continue;
						Wire.Skip(12); Wire.ReadUInt32();
						TestEqual(TEXT("Trigger sends the selected spell"), Wire.ReadUInt32(), uint32(VR->SelectedSpell));
						TestEqual(TEXT("Trigger sends the expected buff recipient"), Wire.ReadUInt32(), uint32(ExpectedTarget));
						++Casts;
					}
					TestEqual(TEXT("One trigger click sends exactly one cast unless blocked"), Casts, ExpectedCount);
				};
				for (bool LeftHanded : {false, true})
				{
					VR->Settings->bLeftHanded = LeftHanded; PointAtPlayer();
					for (int32 Buff : {1, 5, 23}) // Strength Other, Heal Other, Armor Other from retail DAT.
					{
						VR->SelectSpell(Buff); VR->Client->SelectObject(Other.Guid);
						TestTrue(TEXT("Retail Other buff recognizes a live player recipient"), VR->IsPointedBuffRecipient(Player));
						VR->UpdateCombat(.016f);
						TestEqual(TEXT("Other buff preview highlights the pointed player over an old selection"), VR->PredictedCombatTarget.Get(), Player);
						TestEqual(TEXT("Aiming does not change selection before the click"), VR->Client->GetSelectedObject().Guid, Other.Guid);
						ClickSpell(Recipient.Guid);
						TestEqual(TEXT("Trigger selects the player it buffs"), VR->Client->GetSelectedObject().Guid, Recipient.Guid);
					}
				}
				VR->Client->SelectObject(0); ClickSpell(Recipient.Guid);
				TestEqual(TEXT("Buff trigger also selects a player from no selection"), VR->Client->GetSelectedObject().Guid, Recipient.Guid);
				for (int32 UnchangedSpell : {2, 3, 27}) // Self buff, harmful Other, projectile.
				{
					VR->SelectSpell(UnchangedSpell); VR->Client->SelectObject(Other.Guid);
					TestFalse(TEXT("Other spell kinds retain their targeting rules"), VR->IsPointedBuffRecipient(Player));
					ClickSpell(UnchangedSpell == 2 ? CombatSession.GetPlayerGuid() : Other.Guid);
					TestEqual(TEXT("Self, debuff and projectile clicks do not replace selection"), VR->Client->GetSelectedObject().Guid, Other.Guid);
				}
				VR->SelectSpell(1); Player->bIsPlayer = false;
				ClickSpell(Other.Guid); Player->bIsPlayer = true;
				VR->bInventoryOpen = true; VR->UpdatePanels(); ClickSpell(Other.Guid, 0);
				TestEqual(TEXT("Open menus cannot select a buff target behind them"), VR->Client->GetSelectedObject().Guid, Other.Guid);
				VR->bInventoryOpen = false; VR->UpdatePanels();
				auto* Blocker = World->SpawnActor<AActor>();
				auto* Wall = NewObject<UBoxComponent>(Blocker); Blocker->SetRootComponent(Wall);
				Wall->SetBoxExtent(FVector(20, 100, 100)); Wall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Wall->SetCollisionResponseToAllChannels(ECR_Ignore); Wall->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
				Wall->RegisterComponent(); Blocker->SetActorLocation(Body->GetComponentLocation() - FVector(200, 0, 0));
				VR->Client->SelectObject(0); VR->UpdateCombat(.016f);
				TestNull(TEXT("A player behind a wall is not highlighted for a buff"), VR->PredictedCombatTarget.Get());
				ClickSpell(0, 0); // A targeted Other spell cannot fire without a recipient.
				TestFalse(TEXT("Trigger cannot select a buff recipient through a wall"), VR->Client->GetSelectedObject().bValid);
				VR->SelectSpell(5); VR->Client->SelectObject(CombatSession.GetPlayerGuid());
				ClickSpell(0, 0);
				TestTrue(TEXT("Other healing gives feedback instead of self-casting"), VR->GetCastFeedback().Contains(TEXT("appropriate target")));
				for (bool LeftHanded : {false,true})
				{
					VR->Settings->bLeftHanded=LeftHanded; PointAtPlayer();
					Blocker->SetActorLocation(Body->GetComponentLocation()-FVector(200,0,0));
					for (int32 SelfSpell : {2,6,24,2645}) // Strength, Heal, Armor Self and recall.
					for (int32 Selection : {0,Other.Guid})
					{
						VR->Client->SelectObject(Selection); VR->SelectSpell(SelfSpell);
						TestTrue(TEXT("Caster-only spell prompt requires no aiming or self selection"),VR->GetCastFeedback().Contains(TEXT("cast on yourself")));
						ClickSpell(CombatSession.GetPlayerGuid());
						TestEqual(TEXT("Caster-only VR cast preserves the previous combat selection"),VR->Client->GetSelectedObject().Guid,Selection);
					}
				}
				VR->SelectSpell(27); VR->Client->SelectObject(0);
				ClickSpell(0); // Projectile gestures retain free aim without a selected object.
				{
					// The older cast fixture supplies a generic held object; the
					// wheel correctly requires its explicit caster item type.
					TGuardValue<int32> CasterType(CombatSession.WorldObjects[Wand.Guid].ItemType,ACEItemType::Caster);
					const auto Bars=CombatSession.SpellBars;
					const int32 ActiveTab=CombatSession.ActiveSpellBar;
					CombatSession.SpellBars.SetNum(8); CombatSession.SpellBars[0]={2}; CombatSession.ActiveSpellBar=0;
					VR->TurnStick=FVector2D::ZeroVector; VR->ToggleSpellWheel(); VR->WheelHover=0;
					const uint32 BeforeConfirm=CombatSession.VRSequence;
					VR->ConfirmWheelSpell();
					TestTrue(TEXT("Spell wheel retains self-cast guidance"),!VR->bSpellWheelOpen && VR->SelectedSpell==2 && VR->GetCastFeedback().Contains(TEXT("cast on yourself")));
					TestEqual(TEXT("Self spell wheel confirmation does not cast prematurely"),CombatSession.VRSequence,BeforeConfirm);
					CombatSession.SpellBars=Bars; CombatSession.ActiveSpellBar=ActiveTab;
				}
				// Ordinary ACE servers never advertise VR capabilities. Exercise the
				// physical trigger and inspect standard actions, including built-in wands.
				{
					TGuardValue<uint32> StandardServer(CombatSession.VRCapabilities, 0);
					Blocker->SetActorLocation(FVector(0,0,-10000));
					auto StandardCast = [&](int32 SpellId, int32 Selection, int32 ExpectedTarget, bool BuiltIn, bool Expected)
					{
						TGuardValue<int32> ItemSpell(CombatSession.WorldObjects[Wand.Guid].SpellDID, BuiltIn ? SpellId : 0);
						VR->SelectSpell(SpellId); VR->Client->SelectObject(Selection); VR->LastCast=-100;
						CombatSession.ClearPendingUse(); CombatSession.CachedC2SPackets.Reset();
						VR->Trigger(VR->Settings->bLeftHanded,true); VR->Trigger(VR->Settings->bLeftHanded,false);
						int32 Count=0;
						for(const auto& Packet:CombatSession.CachedC2SPackets)
						{
							FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
							if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue; Wire.ReadUInt32();
							const uint32 Action=Wire.ReadUInt32();
							TestNotEqual(TEXT("Stock server never receives a VR combat opcode"),Action,0xF7D0u);
							if(Action!=ACEGameAction::CastTargetedSpell && Action!=ACEGameAction::CastUntargetedSpell && Action!=ACEGameAction::UseWithTarget)continue;
							++Count;
							TestEqual(TEXT("Stock cast uses the appropriate retail action"),Action,BuiltIn?ACEGameAction::UseWithTarget:ACEGameAction::CastTargetedSpell);
							TestEqual(TEXT("Stock cast retains target or casting item"),Wire.ReadUInt32(),uint32(BuiltIn?Wand.Guid:ExpectedTarget));
							TestEqual(TEXT("Stock cast retains spell or built-in target"),Wire.ReadUInt32(),uint32(BuiltIn?ExpectedTarget:SpellId));
						}
						TestEqual(TEXT("Standard ACE trigger emits one cast or rejects an invalid target"),Count,Expected?1:0);
					};
					for(bool Left:{false,true})
					{
						VR->Settings->bLeftHanded=Left; PointAtPlayer();
						StandardCast(2,0,CombatSession.GetPlayerGuid(),false,true);
						StandardCast(1,Other.Guid,Recipient.Guid,false,true);
						StandardCast(27,Other.Guid,Other.Guid,false,true);
						StandardCast(2,0,CombatSession.GetPlayerGuid(),true,true);
						Player->SetActorLocation(FVector(0,0,-10000)); Body->SetWorldLocation(FVector(0,0,-10000));
						StandardCast(27,0,0,false,false); // Stock projectiles require a target.
					}
					TestFalse(TEXT("Standard server status does not wait for optional VR extensions"),VR->GetStatusText().Contains(TEXT("Waiting")));
					CombatSession.ClearPendingUse();
				}
				Blocker->Destroy(); Player->Destroy();
				CombatSession.WorldObjects.Remove(Recipient.Guid); CombatSession.WorldObjects.Remove(Other.Guid);
				CombatSession.KnownSpells = KnownSpells; CombatSession.VRSpellProfiles = Profiles;
				VR->Client->SelectObject(PreviousSelection.Guid); VR->LastCast = -100;
				PollHands(); VR->ResetHandContacts();
			}
			auto* App = NewObject<UACECharacterAppearanceComponent>(Pawn); Pawn->AddInstanceComponent(App); App->RegisterComponent();
			App->bAlignMeshToCapsuleBottom = true;
			FACEWorldObject Self; Self.SetupId = 0x02000001; Self.bIsPlayer = true;
			for(int32 OlthoiSetup:{0x02001A21,0x02001A20})
			{
				Self.SetupId=OlthoiSetup;
				TestTrue(TEXT("Olthoi avatar builds in VR"),App->ApplyWorldObject(Self,100,false));
				for(bool ShowBody:{true,false})
				{
					VR->Settings->bShowBody=ShowBody;VR->UpdateArms();
					TestFalse(TEXT("Olthoi keeps authored motion instead of human IK"),App->bVRPoseControlled);
					for(int32 I=0;I<App->GetPartCount();++I)
					{
						auto* Part=Cast<UPrimitiveComponent>(App->GetPartMesh(I));
						TestTrue(TEXT("Olthoi body remains available for rendering and shadows"),Part && Part->IsVisible());
						if(Part)TestEqual(TEXT("Olthoi respects show-body preference"),bool(Part->bOwnerNoSee),!ShowBody);
					}
				}
			}
			VR->Settings->bShowBody=true;Self.SetupId=0x02000001;
			TestTrue(TEXT("Retail human fixture builds"), App->ApplyWorldObject(Self, 100, false));
			VR->AvatarEyeHeight = 165.f; VR->Head->SetRelativeLocationAndRotation(FVector(25,15,165),FRotator::ZeroRotator); VR->UpdateArms();
			FTransform HeadBind; App->GetPartBindTransform(16, HeadBind);
			const FVector BodyEye = App->GetPartMesh(16)->GetComponentTransform().TransformPosition(FVector(0, -8, 17));
			TestTrue(TEXT("Avatar eye position aligns with room-scale headset"), BodyEye.Equals(VR->Head->GetComponentLocation(), .1));
			const FVector Shoulder = App->GetPartMesh(10)->GetComponentLocation();
			TestTrue(TEXT("Eyes remain anatomically above the shoulders"), BodyEye.Z - Shoulder.Z > 20);
			for (int32 Side = 0; Side < 2; ++Side)
			{
				int32 Part; FTransform Holding; Dat->GetHoldingLocation(Self.SetupId, Side == 0 ? 8 : 1, Part, Holding, 100);
				auto* Grip = Side == 0 ? VR->LeftGrip.Get() : VR->RightGrip.Get();
				const FTransform Calibrated = VR->GetAvatarGrip(Side == 0);
				const FTransform WorldHold = Holding * App->GetPartMesh(Part)->GetComponentTransform();
				TestTrue(TEXT("Authored hand socket aligns with the controller grip"), WorldHold.GetLocation().Equals(Grip->GetComponentLocation(), .1));
				TestTrue(TEXT("Weapon shaft follows the calibrated grasp"), FVector::DotProduct(WorldHold.GetUnitAxis(EAxis::Z), Calibrated.GetUnitAxis(EAxis::X)) > .999);
				TestTrue(TEXT("Retail fingertips point down from the grip instead of toward the wearer"),
					FVector::DotProduct(-App->GetPartMesh(Part)->GetUpVector(), -Calibrated.GetUnitAxis(EAxis::Z)) > .999);
				TestEqual(TEXT("Equipment anchors use the hand, not the forearm"), VR->GetHeldAnchor(Side == 0 ? 2 : 1), App->GetPartMesh(Part));
			}
			VR->Settings->bShowBody = false; VR->UpdateArms();
			for (int32 I = 0; I < App->GetPartCount(); ++I)
			{
				auto* Part = Cast<UPrimitiveComponent>(App->GetPartMesh(I));
				const bool Arm = (I >= 10 && I <= 15) || I == 27 || I == 28;
				TestEqual(FString::Printf(TEXT("Arms-only owner visibility part %d"), I), bool(Part->bOwnerNoSee), !Arm);
				TestTrue(TEXT("Every body part remains available for a complete player shadow"), Part->IsVisible() && Part->CastShadow && Part->bCastHiddenShadow);
				TestEqual(TEXT("Body parts cannot collide with player motion"), Part->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
			}
			for (float Height : {110.f, 165.f, 190.f})
			{
				VR->Head->SetRelativeLocation(FVector(25, 15, Height)); VR->UpdateArms();
				TestTrue(TEXT("Camera remains at the avatar eyes across user heights"), App->GetPartMesh(16)->GetComponentTransform()
					.TransformPosition(FVector(0, -8, 17)).Equals(VR->Head->GetComponentLocation(), .1));
			}
			for (FRotator Rotation : {FRotator::ZeroRotator, FRotator(20, 30, 60), FRotator(-20, -25, -75)})
			{
				VR->LeftGrip->SetWorldRotation(Rotation); VR->RightGrip->SetWorldRotation(Rotation); VR->UpdateArms();
				for (int32 Part : {11, 14})
				{
					FTransform LowerBind, HandBind; App->GetPartBindTransform(Part, LowerBind); App->GetPartBindTransform(Part + 1, HandBind);
					const FVector LocalAxis = LowerBind.GetRotation().UnrotateVector(HandBind.GetLocation() - LowerBind.GetLocation()).GetSafeNormal();
					auto* Lower = App->GetPartMesh(Part); auto* Hand = App->GetPartMesh(Part + 1);
					const FVector Axis = (Hand->GetComponentLocation() - Lower->GetComponentLocation()).GetSafeNormal();
					TestTrue(TEXT("Both forearms point from elbow toward their wrist across controller rotations"),
						FVector::DotProduct(Lower->GetComponentQuat().RotateVector(LocalAxis), Axis) > .999);
					TestTrue(TEXT("Forearm roll stays on the palm's side rather than reversing"), FVector::DotProduct(
						FVector::VectorPlaneProject(Lower->GetRightVector(), Axis).GetSafeNormal(),
						FVector::VectorPlaneProject(Hand->GetRightVector(), Axis).GetSafeNormal()) > 0.f);
					const FVector ActualElbow = Lower->GetComponentTransform().TransformPosition(FVector(0, 0, 14));
					const FVector ActualWrist = Lower->GetComponentTransform().TransformPosition(LowerBind.InverseTransformPosition(HandBind.GetLocation()));
					TestTrue(TEXT("Visible forearm wrist meets the hand, including the forearm's internal origin offset"), ActualWrist.Equals(Hand->GetComponentLocation(), .1));
					FTransform UpperBind; App->GetPartBindTransform(Part - 1, UpperBind);
					const FVector UpperElbow = App->GetPartMesh(Part - 1)->GetComponentTransform().TransformPosition(
						UpperBind.InverseTransformPosition(LowerBind.TransformPosition(FVector(0, 0, 14))));
					TestTrue(TEXT("Visible upper arm and forearm share their actual elbow landmark"), ActualElbow.Equals(UpperElbow, .1));
					TestTrue(TEXT("Forearm extends a full anatomical length, not just origin-to-wrist"), FVector::Distance(ActualElbow, ActualWrist) > 25.f);
				}
			}
			// Render the actual retail meshes for visual inspection, not only joint axes.
			Pawn->SetActorHiddenInGame(false);
			PollHands(); VR->Head->SetRelativeLocation(FVector(0, 0, 165)); VR->UpdateArms();
			VR->Head->SetRelativeRotation(FRotator(-55,0,10));
			for (int32 Frame=0;Frame<90;++Frame) VR->UpdateArms(1.f/90);
			FTransform ChestBind; App->GetPartBindTransform(9,ChestBind);
			TestTrue(TEXT("Local headset tilt actually bends the rendered chest"),
				App->GetPartMesh(9)->GetRelativeRotation().Quaternion().AngularDistance(ChestBind.GetRotation()) > FMath::DegreesToRadians(3.f));
			TestTrue(TEXT("Bending the torso preserves the tracked eye position"),App->GetPartMesh(16)->GetComponentTransform()
				.TransformPosition(FVector(0,-8,17)).Equals(VR->Head->GetComponentLocation(),.1));
			for (int32 Side=0;Side<2;++Side)
			{
				int32 Hand;FTransform Hold;Dat->GetHoldingLocation(Self.SetupId,Side==0?8:1,Hand,Hold,100);
				TestTrue(TEXT("Torso sway preserves controller grip alignment"),(Hold*App->GetPartMesh(Hand)->GetComponentTransform()).GetLocation()
					.Equals((Side==0?VR->LeftGrip:VR->RightGrip)->GetComponentLocation(),.1));
			}
			auto* CaptureOwner = World->SpawnActor<AActor>();
			auto* Capture = NewObject<USceneCaptureComponent2D>(CaptureOwner); CaptureOwner->AddInstanceComponent(Capture);
			Capture->RegisterComponent(); Capture->bCaptureEveryFrame = Capture->bCaptureOnMovement = false;
			Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
			Capture->ShowOnlyActorComponents(Pawn); Capture->ShowFlags.SetLighting(false);
			Capture->CaptureSource = SCS_FinalColorLDR; Capture->ShowFlags.SetEyeAdaptation(false); Capture->ShowFlags.SetAtmosphere(false);
			auto* ArmImage = NewObject<UTextureRenderTarget2D>(CaptureOwner); ArmImage->InitCustomFormat(1024, 1024, PF_B8G8R8A8, false); Capture->TextureTarget = ArmImage;
			FBox ArmBounds(ForceInit); for (int32 Part = 10; Part <= 15; ++Part) ArmBounds += Cast<UPrimitiveComponent>(App->GetPartMesh(Part))->Bounds.GetBox();
			const FVector Center = ArmBounds.GetCenter(); const FVector Eye = Center + VR->Head->GetForwardVector() * 130 + FVector(0, 0, 60);
			Capture->SetWorldLocationAndRotation(Eye, (Center - Eye).Rotation()); Capture->FOVAngle = 70;
			World->SendAllEndOfFrameUpdates(); Capture->CaptureScene(); FlushRenderingCommands();
			TArray<FColor> ArmPixels; ArmImage->GameThread_GetRenderTargetResource()->ReadPixels(ArmPixels);
			TArray64<uint8> ArmPNG; FImageUtils::PNGCompressImageArray(1024, 1024, ArmPixels, ArmPNG);
			FFileHelper::SaveArrayToFile(ArmPNG, *(FPaths::ProjectSavedDir() / TEXT("Automation/VR/ArmGeometry.png")));
			const FVector BodyCenter=App->GetMeshRoot()->GetComponentLocation()+FVector(0,0,100);
			const FVector BodyCaptureEye=BodyCenter+FVector(160,-160,25);
			Capture->SetWorldLocationAndRotation(BodyCaptureEye,(BodyCenter-BodyCaptureEye).Rotation());
			World->SendAllEndOfFrameUpdates();Capture->CaptureScene();FlushRenderingCommands();
			ArmImage->GameThread_GetRenderTargetResource()->ReadPixels(ArmPixels);
			FImageUtils::PNGCompressImageArray(1024,1024,ArmPixels,ArmPNG);
			FFileHelper::SaveArrayToFile(ArmPNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/VR/TorsoBend.png")));
			CaptureOwner->Destroy();
			VR->Head->SetRelativeRotation(FRotator::ZeroRotator);
			for (int32 Frame=0;Frame<90;++Frame) VR->UpdateArms(1.f/90);
			PollHands(); VR->UpdateArms();
			// Untracked controllers must hide arms once, without recreating their
			// render proxies on every subsequent headset frame.
			const bool WasTracking = VR->bTracking, WasEntering = PC->bEnterWorldLoading, WasRevealing = PC->bWorldRevealActive;
			VR->bTracking = true; PC->bEnterWorldLoading = PC->bWorldRevealActive = false;
			VR->UpdateArms(); World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
			TestTrue(TEXT("Tracking fixture has a live arm render state"), App->GetPartMesh(10)->IsVisible() && App->GetPartMesh(10)->IsRenderStateCreated());
			Controllers.Connected = false; PollHands(); VR->UpdateArms();
			World->SendAllEndOfFrameUpdates(); FlushRenderingCommands();
			for (int32 Frame = 0; Frame < 3; ++Frame) VR->UpdateArms(1.f / 90.f);
			for (int32 Part = 10; Part <= 15; ++Part)
			{
				TestFalse(TEXT("Untracked arm remains hidden"), App->GetPartMesh(Part)->IsVisible());
				TestFalse(TEXT("Unchanged tracking does not recreate the arm render proxy"), App->GetPartMesh(Part)->IsRenderStateDirty());
			}
			Controllers.Connected = true; PollHands(); VR->UpdateArms();
			for (int32 Part = 10; Part <= 15; ++Part)
				TestTrue(TEXT("Tracking recovery reveals the arm immediately"), App->GetPartMesh(Part)->IsVisible());
			VR->bTracking = WasTracking; PC->bEnterWorldLoading = WasEntering; PC->bWorldRevealActive = WasRevealing;
			Wand.SetupId = 0x0200017C; Wand.ParentGuid = 100; Wand.ParentLocation = 1;
			VR->Client->Session->WorldObjects[Wand.Guid] = Wand;
			auto* WandActor = World->SpawnActor<AACEWorldEntityActor>(); WandActor->InitializeFromObject(Wand, 100, true);
			WandActor->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
			int32 WandHandPart; FTransform WandHolding; Dat->GetHoldingLocation(Self.SetupId, 1, WandHandPart, WandHolding, 100);
			WandActor->SetActorTransform(WandHolding * App->GetPartMesh(WandHandPart)->GetComponentTransform());
			const FVector Tip = VR->GetWeaponTip();
			TestTrue(TEXT("Real wand muzzle is away from the controller and at the outward end"), FVector::Distance(Tip, VR->WeaponGrip()->GetComponentLocation()) > 35.f);
			TestTrue(TEXT("Wand aim follows the rendered part shaft"), FVector::DotProduct(
				WandActor->GetActorTransform().TransformVectorNoScale(VR->WeaponAxisLocal), WandActor->Appearance->GetPartMesh(0)->GetUpVector()) > .999);
			const FTransform WandTransform = WandActor->GetActorTransform();
			const int32 WandSetup = Wand.SetupId;
			Wand.SetupId = 0x02000B5A; // Dark Sorcerer's Phylactery: no meaningful shaft.
			CombatSession.WorldObjects[Wand.Guid] = Wand;
			WandActor->InitializeFromObject(Wand, 100, true);
			for (float Yaw : {0.f, 180.f})
			{
				WandActor->SetActorLocationAndRotation(VR->WeaponGrip()->GetComponentLocation(), FRotator(0,Yaw,0));
				FVector CastOrigin, CastDirection; VR->GetSpellAim(CastOrigin,CastDirection);
				TestTrue(TEXT("Phylactery casts along the controller aim, even with a reversed model"),CastDirection.Equals(VR->WeaponAim()->GetForwardVector(),.001));
				TestTrue(TEXT("Phylactery muzzle remains near the held item"),FVector::Dist(CastOrigin,WandActor->GetActorLocation())<60.f);
			}
			VR->UpdatePointerVisuals(true,false);
			TestTrue(TEXT("Magic stance displays a visible casting direction"),VR->PointerBeams[VR->Settings->bLeftHanded?0:1]->IsVisible());
			Wand.SetupId=WandSetup;CombatSession.WorldObjects[Wand.Guid]=Wand;
			WandActor->InitializeFromObject(Wand,100,true);WandActor->SetActorTransform(WandTransform);
			// Animation of the rendered weapon must not manufacture a controller swing.
			Wand.CurrentWieldedLocation = ACEEquipMask::MeleeWeapon; VR->Client->Session->WorldObjects[Wand.Guid] = Wand;
			VR->Client->Session->PlayerVitals.CombatMode = ACECombatMode::Melee;
			VR->UpdateCombat(.02f);
			for (int32 Frame = 0; Frame < 30; ++Frame)
			{
				WandActor->AddActorWorldOffset(FVector(Frame % 2 ? 40 : -40, 0, 0)); VR->UpdateCombat(.02f);
				TestTrue(TEXT("A moving visual weapon with a still controller cannot create a swing"), VR->Swing.Travel == 0.f);
			}
			Gameplay->bCombatRepeatActive = Gameplay->bCombatAttackRequestPending = true; Gameplay->TickCombatAutoAttack(.1f);
			TestFalse(TEXT("Retail auto attack cannot bypass tracked VR combat"), Gameplay->bCombatRepeatActive || Gameplay->bCombatAttackRequestPending);
			const uint32 BeforeWalking = CombatSession.VRSequence;
			const FTransform BeforeWalkTracking = VR->TrackingOrigin->GetComponentTransform();
			for (int32 Frame = 0; Frame < 30; ++Frame)
			{
				VR->TrackingOrigin->AddWorldOffset(FVector(0, 12, 0)); VR->UpdateCombat(.02f);
				TestTrue(TEXT("Moving the tracking rig cannot manufacture controller swing travel"), VR->Swing.Travel < .01f);
			}
			TestEqual(TEXT("Walking with a still sword cannot dispatch a strike"), CombatSession.VRSequence, BeforeWalking);
			VR->TrackingOrigin->SetWorldTransform(BeforeWalkTracking); VR->Swing.Reset();
			// Sweep a calibrated blade through a real world-entity collision proxy,
			// then inspect the reliable packet rather than only a local counter.
			VR->WeaponGrip()->SetWorldLocationAndRotation(FVector(70, -90, 130), FRotator::ZeroRotator);
			VR->Swing.Reset(); VR->UpdateCombat(.02f);
			const FVector BladeSample = VR->WeaponGrip()->GetComponentTransform().TransformPosition(VR->BladeTipInGrip * .7f);
			FACEWorldObject Creature; Creature.Guid = static_cast<int32>(0x80000777u); Creature.ItemType = ACEItemType::Creature;
			Creature.ObjectDescriptionFlags = ACEObjectDescFlag::Attackable;
			Creature.SetupId = 0x020007DD; Creature.bHasPosition = true; Creature.Position.CellId = 1;
			CombatSession.WorldObjects.Add(Creature.Guid, Creature);
			auto* CreatureActor = World->SpawnActor<AACEWorldEntityActor>(); CreatureActor->InitializeFromObject(Creature, 100, false);
			CreatureActor->ConfigureWorldCollision(true);
			auto* CreatureCapsule = CreatureActor->FindComponentByClass<UCapsuleComponent>();
			CreatureActor->SetActorLocation(BladeSample + FVector(0,45,-80));
			CreatureCapsule->SetCapsuleSize(18,55); CreatureCapsule->SetWorldLocation(BladeSample + FVector(0,45,0));
			CombatSession.CachedC2SPackets.Reset(); const uint32 BeforeSwing = CombatSession.VRSequence;
			for (int32 Frame = 0; Frame < 12; ++Frame)
			{
				VR->WeaponGrip()->AddWorldOffset(FVector(0, 8, 0)); VR->UpdateCombat(.02f);
			}
			TestEqual(TEXT("Deliberate controller sweep sends one attack through a creature"), CombatSession.VRSequence, BeforeSwing + 1);
			TestEqual(TEXT("Struck creature becomes the selected target"), VR->Client->GetSelectedObject().Guid, Creature.Guid);
			bool SentSwing = false;
			for (const auto& Packet : CombatSession.CachedC2SPackets)
			{
				FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
				if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue; Wire.ReadUInt32();
				if (Wire.ReadUInt32() != 0xF7D0u) continue;
				Wire.ReadUInt32(); if (Wire.ReadUInt32() != 2u) continue;
				Wire.Skip(12); TestEqual(TEXT("Swing packet identifies equipped weapon"), Wire.ReadUInt32(), uint32(Wand.Guid));
				Wire.ReadUInt32(); TestEqual(TEXT("Swing packet identifies struck high-bit creature GUID"), Wire.ReadUInt32(), uint32(Creature.Guid));
				SentSwing = true;
			}
			TestTrue(TEXT("Tracked melee reaches the reliable network packet"), SentSwing);
			// Retail arrows share ItemType::MissileWeapon with bows. Exercise the
			// desktop charge-to-packet path with arrows still equipped after a swap.
			FACEWorldObject RemainingArrows; RemainingArrows.Guid = 7777; RemainingArrows.WielderId = 100;
			RemainingArrows.CurrentWieldedLocation = ACEEquipMask::MissileAmmo;
			RemainingArrows.ItemType = ACEItemType::MissileWeapon;
			CombatSession.WorldObjects.Add(RemainingArrows.Guid, RemainingArrows);
			Gameplay->PlayerController = nullptr; // Desktop input, independent of the test's tracked pawn.
			Gameplay->ApplyCombatMode(Gameplay->ResolveEquippedCombatMode());
			TestEqual(TEXT("Desktop enters melee while a sword and arrows are equipped"), Gameplay->CombatMode, int32(ACECombatMode::Melee));
			Gameplay->RequestedAttackPower = .75f;
			Gameplay->BeginCombatPowerCharge(2);
			Gameplay->CombatPowerBuildStartTime -= 2.;
			CombatSession.CachedC2SPackets.Reset();
			Gameplay->TickCombatAutoAttack(.1f);
			bool SentDesktopMelee = false;
			for (const auto& Packet : CombatSession.CachedC2SPackets)
			{
				FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
				if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue; Wire.ReadUInt32();
				if (Wire.ReadUInt32() != ACEGameAction::TargetedMeleeAttack) continue;
				TestEqual(TEXT("Charged desktop melee targets the creature for server approach and attack"), Wire.ReadUInt32(), uint32(Creature.Guid));
				TestEqual(TEXT("Desktop melee retains the chosen attack height"), Wire.ReadUInt32(), 2u);
				TestEqual(TEXT("Desktop melee retains charged power"), Wire.ReadFloat(), .75f);
				SentDesktopMelee = true;
			}
			TestTrue(TEXT("Desktop charge dispatches melee instead of silently rejecting a missile stance"), SentDesktopMelee);
			Gameplay->PlayerController = PC;
			Gameplay->bCombatRepeatActive = Gameplay->bCombatAttackRequestPending = Gameplay->bCombatPowerCharging = false;
			CombatSession.WorldObjects.Remove(RemainingArrows.Guid);
			// Drive both real rig hands through the same creature with no weapon.
			CombatSession.WorldObjects.Remove(Wand.Guid);
			WandActor->SetActorEnableCollision(false);
			CombatSession.VRCapabilities |= 128u;
			CombatSession.PlayerVitals.CombatMode = ACECombatMode::Melee;
			const FVector PunchFeet = Pawn->GetActorLocation()-FVector(0,0,Capsule->GetScaledCapsuleHalfHeight());
			Creature.SetupId=0x0200003D; // Real rat, including its physical body.
			CreatureActor->InitializeFromObject(Creature,100,false); CreatureActor->ConfigureWorldCollision(true);
			for (bool LeftHanded : {false,true}) for (bool Left : {false,true})
			{
				VR->Settings->bLeftHanded=LeftHanded; VR->CancelGestures(); VR->ResetHandContacts();
				auto* Hand = Left ? VR->LeftGrip.Get() : VR->RightGrip.Get();
				const FVector Start = PunchFeet+FVector(20,0,25);
				Hand->SetWorldLocation(Start);
				CreatureActor->SetActorLocation(PunchFeet+FVector(95,0,0));
				VR->UpdateHandContacts(.02f); VR->UpdateCombat(.02f); const uint32 BeforePunch=CombatSession.VRSequence;
				for(int32 Frame=0;Frame<10;++Frame) { Hand->AddWorldOffset(FVector(6,0,0)); VR->UpdateHandContacts(.02f); VR->UpdateCombat(.02f); }
				TestEqual(TEXT("Either empty hand punches with either handedness setting"),CombatSession.VRSequence,BeforePunch+1);
				TestFalse(TEXT("Combat fist is not stopped by the rat mesh or locomotion body"),VR->ContactHands[Left?0:1].State.bContact);
				TestTrue(TEXT("Combat fist follows tracking through the rat while submitting damage"),
					VR->GetPhysicalGrip(Left).GetLocation().Equals(Hand->GetComponentLocation(),.02));
				for(int32 Frame=0;Frame<20;++Frame) VR->UpdateCombat(.02f);
				TestEqual(TEXT("Resting fist on creature cannot repeat damage"),CombatSession.VRSequence,BeforePunch+1);
				VR->CancelGestures(); Hand->SetWorldLocation(Start); VR->bSpellWheelOpen=true;
				VR->UpdateCombat(.02f);
				for(int32 Frame=0;Frame<10;++Frame) { Hand->AddWorldOffset(FVector(6,0,0)); VR->UpdateHandContacts(.02f); VR->UpdateCombat(.02f); }
				TestEqual(TEXT("Choosing a spell cannot punch a creature with an empty hand"),CombatSession.VRSequence,BeforePunch+1);
				VR->bSpellWheelOpen=false; VR->UpdateCombat(.02f);
				TestEqual(TEXT("Closing the wheel cannot release a queued punch"),CombatSession.VRSequence,BeforePunch+1);
				FACEWorldObject Shield; Shield.Guid=13001; Shield.WielderId=100; Shield.CurrentWieldedLocation=ACEEquipMask::Shield;
				CombatSession.WorldObjects.Add(Shield.Guid,Shield);
				VR->CancelGestures(); Hand=LeftHanded ? VR->RightGrip.Get() : VR->LeftGrip.Get();
				Hand->SetWorldLocation(Start); VR->UpdateCombat(.02f);
				for(int32 Frame=0;Frame<10;++Frame) { Hand->AddWorldOffset(FVector(6,0,0)); VR->UpdateHandContacts(.02f); VR->UpdateCombat(.02f); }
				TestEqual(TEXT("Shield hand cannot submit a punch"),CombatSession.VRSequence,BeforePunch+1);
				CombatSession.WorldObjects.Remove(Shield.Guid);
			}
			// Switching stances affects only hand resistance, not creature collision
			// used by the pawn or the independent melee damage query.
			for (int32 Mode : {ACECombatMode::NonCombat,ACECombatMode::Melee,ACECombatMode::Missile,ACECombatMode::Magic,ACECombatMode::NonCombat})
			{
				CombatSession.PlayerVitals.CombatMode=Mode; VR->ResetHandContacts();
				VR->RightGrip->SetWorldLocation(PunchFeet+FVector(80,0,25)); VR->UpdateHandContacts(.02f);
				TestEqual(TEXT("Only peaceful hands stop at the visible creature surface"),VR->ContactHands[1].State.bContact,Mode==ACECombatMode::NonCombat);
				TestTrue(TEXT("Combat hand pass-through leaves creature world collision enabled"),CreatureActor->GetActorEnableCollision());
			}
			VR->Settings->bLeftHanded=false; VR->CancelGestures(); VR->ResetHandContacts();
			CreatureActor->Destroy(); CombatSession.WorldObjects.Remove(Creature.Guid);
			WandActor->Destroy(); VR->Client->Session->WorldObjects.Remove(Wand.Guid);
			FACEWorldObject TwoHand;TwoHand.Guid=12001;TwoHand.WielderId=100;TwoHand.CurrentWieldedLocation=ACEEquipMask::TwoHanded;
			CombatSession.WorldObjects.Add(TwoHand.Guid,TwoHand);CombatSession.PlayerVitals.CombatMode=ACECombatMode::Melee;
			for(bool LeftHanded:{false,true})
			{
				VR->Settings->bLeftHanded=LeftHanded;
				TestEqual(TEXT("Retail left-handed two-hand attachment follows VR dominant hand"),VR->GetHeldAnchor(2,true),App->GetPartMesh(LeftHanded?12:15));
				const FTransform Before=VR->GetAvatarGrip(!LeftHanded);
				VR->BowGrip()->AddWorldOffset(FVector(30,10,0));
				TestTrue(TEXT("Two-handed support grip cannot steer the weapon"),Before.Equals(VR->GetAvatarGrip(!LeftHanded),.001));
				VR->WeaponGrip()->AddWorldOffset(FVector(10,0,0));
				TestTrue(TEXT("Support hand follows the dominant hand's weapon"),VR->GetAvatarGrip(!LeftHanded).GetLocation().Equals(Before.GetLocation()+FVector(10,0,0),.001));
			}
			CombatSession.WorldObjects.Remove(TwoHand.Guid);VR->Settings->bLeftHanded=false;
			FACEWorldObject Bow; Bow.Guid = static_cast<int32>(0x80000EB2u); Bow.SetupId = 0x0200158D; Bow.WielderId = Bow.ParentGuid = 100;
			Bow.ParentLocation = 1; Bow.CurrentWieldedLocation = ACEEquipMask::MissileWeapon; Bow.ItemType = ACEItemType::MissileWeapon; Bow.AmmoType=1;
			FACEWorldObject Ammo; Ammo.Guid = static_cast<int32>(0x80000EB4u); Ammo.SetupId = 0x02001A87; Ammo.WielderId = 100;
			Ammo.CurrentWieldedLocation = ACEEquipMask::MissileAmmo; Ammo.StackSize = 20; Ammo.AmmoType = 1;
			VR->Client->Session->WorldObjects.Add(Bow.Guid, Bow); VR->Client->Session->WorldObjects.Add(Ammo.Guid, Ammo);
			VR->Client->Session->PlayerVitals.CombatMode = ACECombatMode::Missile;
			auto* BowActor = World->SpawnActor<AACEWorldEntityActor>(); BowActor->InitializeFromObject(Bow, 100, true);
			CombatSession.VRMissileWeapon=Bow.Guid; CombatSession.VRMissileSpeed=24.f;
			BowActor->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
			{
				TGuardValue<bool> InventoryClosed(VR->bInventoryOpen,false);
				TGuardValue<bool> SettingsClosed(VR->bSettingsOpen,false);
				TGuardValue<int32> Spell(VR->SelectedSpell,70001);
				CombatSession.VRSpellProfiles.Add(70001,FVector(10,1,.05));
				const FVector Origin(50000,50000,2000), Direction=FVector::ForwardVector;
				FACEWorldObject Body; Body.Guid=70002; Body.ItemType=ACEItemType::Creature;
				Body.SetupId=0x02000001; Body.ObjectDescriptionFlags=ACEObjectDescFlag::Attackable;
				Body.bHasPosition=true; Body.Position.CellId=1;
				auto* First=World->SpawnActor<AACEWorldEntityActor>(); First->InitializeFromObject(Body,100,true); First->ConfigureWorldCollision(true);
				Body.Guid=70003;
				auto* Second=World->SpawnActor<AACEWorldEntityActor>(); Second->InitializeFromObject(Body,100,true); Second->ConfigureWorldCollision(true);
				const FVector ArcCenter=Origin+ACEVRMath::BallisticOffset(Direction,10,1,.5f,100);
				First->SetActorLocation(ArcCenter-FVector(0,0,First->GetMeleeBodyHeight()*.5f));
				Second->SetActorLocation(First->GetActorLocation()+FVector(40,0,0));
				VR->FocusPanel->SetVisibility(false);
				VR->UpdateMissileTrajectory(Origin,Direction,1,10,true);
				TestEqual(TEXT("Arc preview highlights the first overlapping creature body"),VR->PredictedCombatTarget.Get(),First);
				TestFalse(TEXT("Arc aiming cannot open a combat help panel"),VR->FocusPanel->IsVisible());
				First->SetActorLocation(First->GetActorLocation()+FVector(0,300,0));
				VR->UpdateMissileTrajectory(Origin,Direction,1,10,true);
				TestEqual(TEXT("Arc highlight follows the next creature when the first leaves the path"),VR->PredictedCombatTarget.Get(),Second);
				Second->SetActorLocation(Origin+FVector(500,0,-Second->GetMeleeBodyHeight()*.5f));
				VR->UpdateMissileTrajectory(Origin,Direction,1,10,false);
				TestEqual(TEXT("Straight magic uses the creature-body contact test too"),VR->PredictedCombatTarget.Get(),Second);
				TestEqual(TEXT("Switching arc to bolt rebuilds the three-strip straight-line topology"),VR->MissileTrajectory->GetProcMeshSection(0)->GetRenderIndexCount(),18);
				VR->UpdateWorldSelectionHighlights(VR->PredictedCombatTarget.Get(),nullptr);
				const auto* Part=Cast<UPrimitiveComponent>(Second->Appearance->GetPartMesh(0));
				TestTrue(TEXT("Predicted hit uses the same model tint as world selection"),Part && Part->GetCustomPrimitiveData().Data.IsValidIndex(1) && Part->GetCustomPrimitiveData().Data[1]==1.f);
				auto* Blocker=World->SpawnActor<AActor>();
				auto* Wall=NewObject<UBoxComponent>(Blocker); Blocker->SetRootComponent(Wall);
				Wall->SetBoxExtent(FVector(20,100,100)); Wall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				Wall->SetCollisionResponseToAllChannels(ECR_Ignore); Wall->SetCollisionResponseToChannel(ECC_Pawn,ECR_Block);
				Wall->RegisterComponent(); Blocker->SetActorLocation(Origin+FVector(200,0,0));
				VR->UpdateMissileTrajectory(Origin,Direction,1,10,false);
				TestNull(TEXT("A wall before the creature prevents a misleading magic highlight"),VR->PredictedCombatTarget.Get());
				VR->UpdateWorldSelectionHighlights(VR->PredictedCombatTarget.Get(),nullptr);
				TestEqual(TEXT("Losing a predicted hit removes the previous model tint"),Part->GetCustomPrimitiveData().Data[1],0.f);
				Blocker->Destroy(); First->Destroy(); Second->Destroy(); CombatSession.VRSpellProfiles.Remove(70001);
			}
			for (bool LeftHanded : {false, true})
			{
				VR->Settings->bLeftHanded = LeftHanded; VR->CancelGestures(); PollHands();
				VR->Settings->BowAnchorOffset=0; // Uncalibrated center-line reference.
				VR->BowAim()->SetWorldRotation(FRotator(70, 20, 30)); VR->BowGrip()->SetWorldRotation(FRotator::ZeroRotator);
				VR->Client->Session->PlayerVitals.CombatMode = ACECombatMode::NonCombat;
				TestTrue(TEXT("Bow uses support hand immediately while peaceful"), VR->UpdateMissileAttachment(BowActor));
				TestTrue(TEXT("Peaceful bow sits at offhand grip"), BowActor->GetActorLocation().Equals(VR->BowGrip()->GetComponentLocation(), .1));
				VR->Client->Session->PlayerVitals.CombatMode = ACECombatMode::Missile;
				VR->UpdateCombat(.02f);
				TestTrue(TEXT("Missing public combat style still resolves the equipped bow"), VR->IsAmmoLauncher());
				TestTrue(TEXT("Launcher accepts offhand VR placement"), VR->UpdateMissileAttachment(BowActor));
				TestEqual(TEXT("Bow effects stay outside controller late update"), BowActor->GetAttachParentActor(), static_cast<AActor*>(Pawn));
				TestTrue(TEXT("Bow limbs point up and shaft faces along offhand aim"), FVector::DotProduct(BowActor->GetActorUpVector(), -VR->BowAim()->GetForwardVector()) > .999);
				const FQuat OffhandBowRotation = BowActor->GetActorQuat();
				const FVector DrawHandLocation = VR->WeaponGrip()->GetComponentLocation();
				for (bool Drawing : {true, false})
				{
					VR->bDrawing = Drawing;
					for (FVector Offset : {FVector::ZeroVector, FVector(30, 20, 10), FVector(-50, -20, 5)})
					{
						VR->WeaponGrip()->SetWorldLocation(VR->BowGrip()->GetComponentLocation() + Offset);
						VR->UpdateMissileAttachment(BowActor);
						TestTrue(TEXT("Drawing and releasing cannot swivel the bow independently of the offhand"), BowActor->GetActorQuat().Equals(OffhandBowRotation, .001));
					}
				}
				VR->WeaponGrip()->SetWorldLocation(DrawHandLocation);
				if (TestNotNull(TEXT("Equipped arrow has a ready visual before drawing"), VR->AmmoActor.Get()))
				{
					auto* AmmoAppearance = VR->AmmoActor->FindComponentByClass<UACECharacterAppearanceComponent>();
					TestTrue(TEXT("Arrow renders the actual equipped DAT setup"), AmmoAppearance->HasAppearance() && AmmoAppearance->GetSetupId() == Ammo.SetupId && !VR->AmmoActor->IsHidden());
				}
				VR->WeaponGrip()->SetWorldLocation(VR->BowGrip()->GetComponentLocation() - FVector(12, 0, 0));
				VR->Grip(LeftHanded, true); TestTrue(TEXT("Grabbing the visible arrow nocks it"), VR->bDrawing);
				for (int32 Frame = 1; Frame <= 10; ++Frame)
				{
					VR->WeaponGrip()->SetWorldLocation(VR->BowGrip()->GetComponentLocation() - FVector(12 + Frame * 4.8f, 0, 0));
					VR->UpdateCombat(.02f);
					TestTrue(TEXT("Nocked bow paints its arc from partial through full draw"),VR->MissileTrajectory && VR->MissileTrajectory->IsVisible());
				}
				TestTrue(TEXT("Pulling equipped arrow behind the bow reaches full draw"), VR->BowFraction > .99f);
				VR->Settings->BowAnchorOffset=10;
				// The support hand is deliberately pitched/rolled above. A cheek-side
				// anchor is lateral in that hand's aim frame, not in world-space Y.
				const FVector CheekOffset=VR->GetPhysicalAim(!LeftHanded).GetUnitAxis(EAxis::Y)
					* (LeftHanded ? -1.f : 1.f) * 10.f * VR->BowFraction;
				VR->WeaponGrip()->AddWorldOffset(CheekOffset);
				TestTrue(TEXT("Mirrored cheek-side draw offset retains forward aim"),VR->BowDrawDirection().Equals(FVector::ForwardVector,.001));
				VR->WeaponGrip()->AddWorldOffset(-CheekOffset); VR->Settings->BowAnchorOffset=0;
				CombatSession.CachedC2SPackets.Reset();
				const uint32 Before = VR->Client->Session->VRSequence; VR->Grip(LeftHanded, false);
				TestEqual(TEXT("Releasing drawn arrow sends one VR missile action"), VR->Client->Session->VRSequence, Before + 1);
				bool SentArrow = false;
				for (const auto& Packet : CombatSession.CachedC2SPackets)
				{
					FACEBinaryReader Wire(Packet.Value.Payload); Wire.Skip(16);
					if (Wire.ReadUInt32() != ACEOpcode::GameAction) continue; Wire.ReadUInt32();
					if (Wire.ReadUInt32() != 0xF7D0u) continue;
					Wire.ReadUInt32(); if (Wire.ReadUInt32() != 3u) continue;
					Wire.Skip(12); TestEqual(TEXT("Arrow packet retains high-bit bow GUID"), Wire.ReadUInt32(), uint32(Bow.Guid));
					Wire.ReadUInt32(); TestEqual(TEXT("Free shot does not require a selected enemy"), Wire.ReadUInt32(), 0u);
					Wire.Skip(12); const float AimX = Wire.ReadFloat(), AimY = Wire.ReadFloat(), AimZ = Wire.ReadFloat();
					TestTrue(TEXT("Arrow packet follows the physical two-hand direction"), FVector(AimX, AimY, AimZ).Equals(FACEPosition::AceVectorToUnreal(FVector::ForwardVector), .001));
					TestTrue(TEXT("Full draw strength reaches the server"), Wire.ReadFloat() > .99f);
					TestTrue(TEXT("Held draw duration reaches the server"), Wire.ReadFloat() >= .15f); SentArrow = true;
				}
				TestTrue(TEXT("Releasing the actual equipped arrow produces a reliable missile packet"), SentArrow);
				{
					TGuardValue<uint32> RecoveryCapabilities(CombatSession.VRCapabilities,CombatSession.VRCapabilities|2048u);
					CombatSession.VRRecoveryTeleport=CombatSession.TeleportSeq;CombatSession.VRRecoveryDuration=1;CombatSession.VRRecoveryReadyAt=FPlatformTime::Seconds()+1;
					VR->bDrawing=true;VR->BowHoldTime=.3f;
					VR->ReleaseArrow();
					TestEqual(TEXT("Bow release respects the same shot recovery as other missiles"),CombatSession.VRSequence,Before+1);
					TestFalse(TEXT("Recovery cancels the released draw without a delayed shot"),VR->bDrawing);
					CombatSession.VRRecoveryReadyAt=0;
				}
				VR->Client->Session->WorldObjects.Remove(Ammo.Guid);
				VR->WeaponGrip()->SetWorldLocation(VR->BowGrip()->GetComponentLocation() - FVector(12, 0, 0)); VR->Grip(LeftHanded, true);
				TestFalse(TEXT("Without equipped ammo a new arrow cannot be drawn"), VR->bDrawing);
				VR->Client->Session->WorldObjects.Add(Ammo.Guid, Ammo);
			}
			Bow.SetupId=0x0200012D; Bow.DefaultCombatStyle=0x20; Ammo.AmmoType=2;
			CombatSession.WorldObjects[Bow.Guid]=Bow;CombatSession.WorldObjects[Ammo.Guid]=Ammo;
			BowActor->InitializeFromObject(Bow,100,true);
			for(bool LeftHanded : {false,true})
			{
				VR->Settings->bLeftHanded=LeftHanded; VR->CancelGestures(); PollHands();
				VR->BowAim()->SetWorldRotation(FRotator(10,35,0));
				VR->UpdateMissileAttachment(BowActor); VR->UpdateCombat(.02f);
				for (int32 Setup:{0x0200012D,0x0200134C,0x0200134D,0x02001596})
				{
					Bow.SetupId=Setup;CombatSession.WorldObjects[Bow.Guid]=Bow;BowActor->InitializeFromObject(Bow,100,true);VR->UpdateMissileAttachment(BowActor);
					AddInfo(FString::Printf(TEXT("Crossbow %08X grip=%s muzzle=%s"),Setup,*VR->CrossbowGripLocal.ToString(),*VR->CrossbowMuzzleLocal.ToString()));
					TestTrue(TEXT("All crossbow families preserve held-frame barrel alignment"),FVector::DotProduct(-BowActor->GetActorRightVector(),VR->BowAim()->GetForwardVector())>.999);
					TestTrue(TEXT("Crossbow limbs lie across the aim, not vertically"),FVector::DotProduct(-BowActor->GetActorForwardVector(),VR->BowAim()->GetUpVector())>.999);
					TestTrue(TEXT("Crossbow is held beneath the rail, not by its top"),VR->CrossbowGripLocal.X>VR->CrossbowMuzzleLocal.X);
					TestTrue(TEXT("Bolt lies on the stock centerline"),FMath::Abs(VR->CrossbowMuzzleLocal.Z-VR->CrossbowGripLocal.Z)<.01);
					if (!LeftHanded)
					{
						BowActor->SetActorHiddenInGame(false);
						for(int32 PartIndex=0;PartIndex<BowActor->Appearance->GetPartCount();++PartIndex)
							if(auto* Part=Cast<UPrimitiveComponent>(BowActor->Appearance->GetPartMesh(PartIndex)))
							{Part->SetVisibility(true);Part->SetHiddenInGame(false);Part->SetOwnerNoSee(false);Part->SetOnlyOwnerSee(false);}
						auto* C=NewObject<USceneCaptureComponent2D>(BowActor);BowActor->AddInstanceComponent(C);C->RegisterComponent();
						C->bCaptureEveryFrame=C->bCaptureOnMovement=false;C->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
						C->ShowOnlyActorComponents(BowActor);C->ShowFlags.SetLighting(false);C->ShowFlags.SetAtmosphere(false);C->ShowFlags.SetEyeAdaptation(false);C->CaptureSource=SCS_FinalColorLDR;
						auto* T=NewObject<UTextureRenderTarget2D>(BowActor);T->InitCustomFormat(768,512,PF_B8G8R8A8,false);C->TextureTarget=T;
						FBox B(ForceInit);BowActor->Appearance->GetVisualWorldBounds(B);const FVector CrossbowCenter=B.GetCenter();
						const FVector CrossbowEye=CrossbowCenter-VR->BowAim()->GetForwardVector()*100+VR->BowAim()->GetRightVector()*90+VR->BowAim()->GetUpVector()*65;
						C->SetWorldLocationAndRotation(CrossbowEye,(CrossbowCenter-CrossbowEye).Rotation());C->FOVAngle=65;
						World->SendAllEndOfFrameUpdates();C->CaptureScene();FlushRenderingCommands();TArray<FColor> Pixels;T->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
						TestTrue(TEXT("Crossbow fixture renders visible geometry"),Pixels.ContainsByPredicate([](FColor Color){return Color.R>30 || Color.G>30 || Color.B>30;}));
						TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(768,512,Pixels,PNG);
						FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/FString::Printf(TEXT("Automation/VR/Crossbow-%08X.png"),Setup)));C->DestroyComponent();
					}
				}
				VR->Grip(LeftHanded,true);VR->Grip(LeftHanded,false);
				TestFalse(TEXT("Crossbow never requires or starts a bow draw"),VR->bDrawing);
				CombatSession.CachedC2SPackets.Reset();const uint32 Before=CombatSession.VRSequence;
				VR->Trigger(LeftHanded,true);VR->Trigger(LeftHanded,false);
				TestEqual(TEXT("One crossbow trigger click sends one shot without pulling a string"),CombatSession.VRSequence,Before+1);
				bool SentBolt=false;
				for(const auto& Packet:CombatSession.CachedC2SPackets)
				{
					FACEBinaryReader Wire(Packet.Value.Payload);Wire.Skip(16);
					if(Wire.ReadUInt32()!=ACEOpcode::GameAction)continue;Wire.ReadUInt32();
					if(Wire.ReadUInt32()!=0xF7D0u)continue;Wire.ReadUInt32();if(Wire.ReadUInt32()!=3u)continue;
					Wire.Skip(36);
					const float X=Wire.ReadFloat(),Y=Wire.ReadFloat(),Z=Wire.ReadFloat();
					TestTrue(TEXT("Bolt trajectory matches the visible crossbow barrel"),FVector(X,Y,Z).Equals(FACEPosition::AceVectorToUnreal(VR->BowAim()->GetForwardVector()),.001));
					TestEqual(TEXT("Trigger fires a full-power bolt"),Wire.ReadFloat(),1.f);
					TestEqual(TEXT("Crossbow does not fabricate a held draw duration"),Wire.ReadFloat(),0.f);SentBolt=true;
				}
				TestTrue(TEXT("Crossbow trigger reaches reliable combat protocol"),SentBolt);
				CombatSession.WorldObjects.Remove(Ammo.Guid);const uint32 Empty=CombatSession.VRSequence;
				VR->Trigger(LeftHanded,true);VR->Trigger(LeftHanded,false);
				TestEqual(TEXT("Empty crossbow cannot send another shot"),CombatSession.VRSequence,Empty);
				CombatSession.WorldObjects.Add(Ammo.Guid,Ammo);
			}
			for(bool LeftHanded:{false,true})
			{
				VR->Settings->bLeftHanded=LeftHanded;VR->CancelGestures();PollHands();
				Bow.DefaultCombatStyle=LeftHanded?0:0x400;Bow.AmmoType=4;CombatSession.WorldObjects[Bow.Guid]=Bow;
				TestEqual(TEXT("Atlatl is recognized with explicit or public-only weapon metadata"),VR->MissileStyle(),0x400);
				VR->WeaponGrip()->SetWorldRotation(FRotator(20,45,35));VR->WeaponAim()->SetWorldRotation(FRotator(-5,-60,0));VR->ResetHandContacts();
				FVector Origin,Direction;VR->GetThrownAim(Origin,Direction);
				TestTrue(TEXT("Atlatl aims out the heel of the palm, independent of pointing ray"),Direction.Equals(-VR->WeaponGrip()->GetForwardVector(),.001));
				TestTrue(TEXT("Atlatl path starts below the grasp"),Origin.Equals(VR->WeaponGrip()->GetComponentLocation()+Direction*8.f,.001));
				Ammo.AmmoType=2;CombatSession.WorldObjects.Add(Ammo.Guid,Ammo);
				const uint32 WrongAmmo=CombatSession.VRSequence;VR->Trigger(LeftHanded,true);VR->Trigger(LeftHanded,false);
				TestEqual(TEXT("Atlatl does not consume crossbow bolts"),CombatSession.VRSequence,WrongAmmo);
				VR->Grip(LeftHanded,true);TestFalse(TEXT("Atlatl never asks for a bow draw"),VR->bDrawing);VR->Grip(LeftHanded,false);
				Ammo.AmmoType=4;CombatSession.WorldObjects.Add(Ammo.Guid,Ammo);
				const uint32 BeforeAtlatl=CombatSession.VRSequence;VR->Trigger(LeftHanded,true);VR->Trigger(LeftHanded,false);
				TestEqual(TEXT("Atlatl fires on the click without a held draw"),CombatSession.VRSequence,BeforeAtlatl+1);
				{
					TGuardValue<uint32> RecoveryCapabilities(CombatSession.VRCapabilities,CombatSession.VRCapabilities|2048u);
					CombatSession.VRRecoveryTeleport=CombatSession.TeleportSeq;CombatSession.VRRecoveryDuration=1;CombatSession.VRRecoveryReadyAt=FPlatformTime::Seconds()+1;
					VR->Trigger(LeftHanded,true);VR->Trigger(LeftHanded,false);
					TestEqual(TEXT("Click during missile recovery cannot send another shot"),CombatSession.VRSequence,BeforeAtlatl+1);
					TestTrue(TEXT("Missile recovery uses next-shot HUD"),VR->GetCombatTimerText().StartsWith(TEXT("Next shot")));
					CombatSession.VRRecoveryReadyAt=0;
				}
				Bow.DefaultCombatStyle=0;Bow.AmmoType=0;CombatSession.WorldObjects[Bow.Guid]=Bow;CombatSession.WorldObjects.Remove(Ammo.Guid);
				TestEqual(TEXT("A missile with no ammo type is a thrown weapon"),VR->MissileStyle(),0x80);
				VR->Grip(LeftHanded,true);TestFalse(TEXT("Thrown weapon cannot start a bow draw"),VR->bDrawing);
				VR->GetThrownAim(Origin,Direction);TestTrue(TEXT("Thrown item uses the same heel-of-palm direction as the atlatl"),Direction.Equals(-VR->WeaponGrip()->GetForwardVector(),.001));
				CombatSession.WorldObjects.Add(Ammo.Guid,Ammo);TestEqual(TEXT("Unrelated equipped arrows are ignored by a thrown weapon"),VR->EquippedAmmo().Guid,0);
				const uint32 Before=CombatSession.VRSequence;VR->Trigger(LeftHanded,true);TestEqual(TEXT("Thrown trigger fires immediately without hold time"),CombatSession.VRSequence,Before+1);
				VR->Trigger(LeftHanded,false);TestEqual(TEXT("Releasing trigger cannot throw twice"),CombatSession.VRSequence,Before+1);
				CombatSession.WorldObjects.Remove(Ammo.Guid);
			}
			VR->Settings->bLeftHanded = false; VR->CancelGestures(); VR->Client->Session->PlayerVitals.CombatMode = ACECombatMode::NonCombat;
			VR->UpdateMissileAttachment(BowActor); BowActor->Destroy();
			VR->Client->Session->WorldObjects.Remove(Bow.Guid); VR->Client->Session->WorldObjects.Remove(Ammo.Guid); VR->UpdateCombat(.02f);
			CombatSession.SocketC2S->Close(); Sockets->DestroySocket(CombatSession.SocketC2S); CombatSession.SocketC2S = nullptr;
			Sockets->DestroySocket(CombatReceiver);
		}
		Controllers.LeftTracked = false; PollHands(); VR->UpdatePanels();
		TestTrue(TEXT("Losing a hand does not unregister the shared UI user"), VR->LeftPointer->IsActive() && VR->RightPointer->IsActive());
		TestTrue(TEXT("Remaining hand keeps hit testing"), !VR->LeftPointer->bEnableHitTesting && VR->RightPointer->bEnableHitTesting);
		// Components cache the provider for render-thread late updates. Clear those
		// references and finish queued rendering before the scoped provider goes away.
		Controllers.Connected = false; PollHands(); FlushRenderingCommands();
	}
	VR->Head->SetRelativeLocation(FVector(30, 15, 165)); const FVector Eye = VR->Head->GetComponentLocation();
	VR->RotateTracking(30); TestTrue(TEXT("Snap turns pivot around user's actual head"), Eye.Equals(VR->Head->GetComponentLocation(), .001));
	float F = 1, R = 1; bool Run = false; VR->MoveStick = FVector2D(1, 1); VR->bTracking = false; VR->GetMovement(F, R, Run);
	TestTrue(TEXT("Tracking loss stops locomotion"), F == 0 && R == 0);
	VR->bTracking = true; VR->bInventoryOpen = VR->bSettingsOpen = VR->bKeyboardOpen = false;
	VR->MoveStick = FVector2D(0, 1); VR->SmoothedMoveStick = FVector2D::ZeroVector;
	VR->PrepareMovement(1.f / 90.f); VR->GetMovement(F, R, Run);
	TestTrue(TEXT("Starting locomotion ramps toward character Run speed"), F > 0 && F < .1f && Run);
	for (int32 I = 0; I < 120; ++I) VR->PrepareMovement(1.f / 90.f);
	VR->GetMovement(F, R, Run); TestTrue(TEXT("Movement settles at the configured scale"), FMath::IsNearlyEqual(F, VR->Settings->MovementScale, .001f));
	VR->Settings->MovementScale = 1.f;
	for (float Rate : {30.f,45.f,72.f,90.f,144.f})
	{
		VR->MoveStick=FVector2D(.04f,.93f); VR->SmoothedMoveStick=FVector2D::ZeroVector;
		for (int32 I=0;I<FMath::CeilToInt(Rate*2.f);++I) VR->PrepareMovement(1.f/Rate);
		VR->GetMovement(F,R,Run);
		TestTrue(TEXT("Full forward Touch input reaches retail maximum independently of FPS"),FMath::IsNearlyEqual(F,1.f,.0001f) && FMath::IsNearlyZero(R));
	}
	VR->Client->SetRunSkill(100); const float LowSkillSpeed = F * VR->Client->GetLocomotionSpeed(Run);
	VR->Client->SetRunSkill(600); const float HighSkillSpeed = F * VR->Client->GetLocomotionSpeed(Run);
	TestTrue(TEXT("VR speed increases with the character's Run skill"), HighSkillSpeed > LowSkillSpeed);
	VR->ChangeSetting(TEXT("Run")); VR->GetMovement(F, R, Run);
	TestFalse(TEXT("VR settings toggle selects walking"), Run);
	TestTrue(TEXT("Walking uses the actual retail walking speed"), FMath::IsNearlyEqual(VR->Client->GetLocomotionSpeed(Run), 3.12f));
	VR->bLeftGripHeld = VR->bRightGripHeld = true; VR->GetMovement(F, R, Run);
	TestFalse(TEXT("Combat grips cannot change the saved movement mode"), Run);
	VR->ChangeSetting(TEXT("Run")); VR->GetMovement(F, R, Run); TestTrue(TEXT("VR settings toggle restores running"), Run);
	VR->Client->SetRunSkill(200);
	VR->MoveStick = VR->SmoothedMoveStick = FVector2D::ZeroVector;
	VR->Client->SetJumpSkill(400);
	float TapVelocity = 0.f;
	for (float Hold : {.1f, 1.1f})
	{
		PC->bJumpAirborne = PC->bJumpCharging = false; PC->ForwardAxis = PC->RightAxis = 0.f;
		VR->JumpDown();
		for (int32 Frame = 0; Frame < FMath::RoundToInt(Hold * 100); ++Frame)
		{
			VR->PrepareMovement(.01f); VR->JumpDown(); // duplicate presses must not reset charge
		}
		TestTrue(TEXT("Holding jump builds the requested charge"), FMath::IsNearlyEqual(PC->JumpChargeExtent, FMath::Min(Hold, 1.f), .001f));
		VR->UpdatePanels();
		TestTrue(TEXT("Jump charge shows the native height bar in the headset with inventory closed"), VR->JumpPanel->IsVisible());
		TestTrue(TEXT("Jump feedback has its own native Slate surface"), VR->NativeJump.IsValid() && !VR->JumpPanel->GetUserWidgetObject());
		if(VR->NativeJump)TestTrue(TEXT("Native bar shows the controller's actual charge"),FMath::IsNearlyEqual(VR->NativeJump->GetChargeFraction(),PC->JumpChargeExtent,.001f));
		TestEqual(TEXT("Jump meter cannot intercept controller or melee traces"), VR->JumpPanel->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
		VR->JumpUp();
		VR->UpdatePanels(); TestFalse(TEXT("Releasing the jump hides its temporary meter"), VR->JumpPanel->IsVisible());
		TestTrue(TEXT("Release uses character Jump skill and held charge"), FMath::IsNearlyEqual(float(PC->JumpLocalAceVelocity.Z),
			FMath::Sqrt(VR->Client->GetJumpHeight(FMath::Min(Hold, 1.f)) * 19.6f), .001f));
		if (Hold < 1.f) TapVelocity = PC->JumpLocalAceVelocity.Z;
		else TestTrue(TEXT("Full charge launches higher than a tap"), PC->JumpLocalAceVelocity.Z > TapVelocity);
	}
	PC->bJumpAirborne = false; VR->JumpDown(); VR->PrepareMovement(.1f); VR->CancelGestures();
	TestFalse(TEXT("Cancelling input cancels the held jump"), PC->bJumpCharging || VR->IsJumpHeld());
	VR->bInventoryOpen = true;
	TestTrue(TEXT("Inventory allows locomotion while blocking combat gestures"),!VR->IsMovementBlocked() && VR->IsInputBlocked());
	for (const FVector2D Stick : {FVector2D(0,1),FVector2D(1,0),FVector2D(0,-1),FVector2D(.7f,.7f)})
	{
		VR->MoveStick=Stick; VR->SmoothedMoveStick=FVector2D::ZeroVector;
		for(int32 Frame=0;Frame<180;++Frame)VR->PrepareMovement(1.f/90.f);
		VR->GetMovement(F,R,Run);
		const FVector2D MenuMovement(R,F);
		VR->bInventoryOpen=false; VR->GetMovement(F,R,Run); VR->bInventoryOpen=true;
		TestTrue(TEXT("Inventory preserves normal stick movement in every direction"),!MenuMovement.IsNearlyZero() && MenuMovement.Equals(FVector2D(R,F),.001f));
	}
	for (bool* Blocker : {&VR->bSettingsOpen,&VR->bTextKeyboardOpen})
	{
		*Blocker=true; VR->PrepareMovement(1.f/90.f); VR->GetMovement(F,R,Run);
		TestTrue(TEXT("Settings and text entry still stop locomotion"),F==0 && R==0 && VR->SmoothedMoveStick.IsNearlyZero());
		*Blocker=false;
	}
	VR->PanelEditHand=0; VR->GetMovement(F,R,Run);
	TestTrue(TEXT("Panel placement reserves the sticks instead of moving the player"),F==0 && R==0 && VR->IsMovementBlocked());
	VR->PanelEditHand=INDEX_NONE;
	VR->MoveStick=VR->SmoothedMoveStick=FVector2D::ZeroVector;
	VR->bDrawing = true; VR->CancelGestures();
	TestFalse(TEXT("Cancel releases nocked shots"), VR->bDrawing);
	VR->Client->Session->State = EACESessionState::InWorld; VR->Client->Session->PlayerGuid = 100;
	VR->Client->Session->KnownSpells = {1, 2, 3, 4, 5, 6, 7, 8};
	VR->Client->OnVendorOpened.Broadcast(200);
	TestTrue(TEXT("Vendor response reveals the interactive retail panel"), VR->bInventoryOpen);
	TestFalse(TEXT("Vendor response cannot leave a drawn bow armed"), VR->bDrawing);
	// Follow a sustained uphill/downhill motion through the actual rig tick.
	// Comfort filtering must not accumulate height error along a long slope.
	VR->LastPawnLocation = Pawn->GetActorLocation(); VR->VerticalComfortOffset = 0;
	const FVector TerrainStartPawn = Pawn->GetActorLocation(), TerrainStartRig = VR->TrackingOrigin->GetComponentLocation();
	for (float Step : {4.f, -4.f})
		for (int32 Frame = 0; Frame < 90; ++Frame)
		{
			Pawn->AddActorWorldOffset(FVector(6, 0, Step)); VR->TickComponent(1.f / 90.f, LEVELTICK_All, nullptr);
			const FVector Expected = TerrainStartRig + Pawn->GetActorLocation() - TerrainStartPawn;
			TestTrue(TEXT("Sustained terrain travel keeps the headset floor within 3cm"), FMath::Abs(VR->TrackingOrigin->GetComponentLocation().Z - Expected.Z) <= 3.01f);
		}
	PC->bJumpAirborne = true; Pawn->AddActorWorldOffset(FVector(0, 0, 15)); VR->TickComponent(1.f / 90.f, LEVELTICK_All, nullptr);
	TestTrue(TEXT("A jump immediately clears floor smoothing"), VR->VerticalComfortOffset == 0.f);
	PC->bJumpAirborne = false;
	{
		auto* Portal = World->SpawnActor<AACELoadingScreenActor>(); PC->LoadingScreenActor = Portal;
		Portal->BeginTunnel(); Portal->bMeshReady = true;
		VR->bWasLoading = PC->bEnterWorldLoading = true; VR->bTracking = true;
		VR->UpdatePortalView(); VR->UpdateComfort(1.f);
		TestTrue(TEXT("VR renders original portal space instead of a black loading view"), VR->bPortalViewActive && !Portal->IsHidden() && VR->Fade == 0);
		auto* PortalWeapon = World->SpawnActor<AACEWorldEntityActor>();
		PortalWeapon->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
		PortalWeapon->SetActorHiddenInGame(false);
		VR->UpdatePortalEquipmentVisibility();
		TestTrue(TEXT("Equipped weapons are hidden throughout portal space"), PortalWeapon->IsHidden());
		auto* LateWeapon = World->SpawnActor<AACEWorldEntityActor>();
		LateWeapon->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
		LateWeapon->SetActorHiddenInGame(false);
		TestTrue(TEXT("Equipment attached after the rig tick remains in portal space"), VR->UpdateMissileAttachment(LateWeapon));
		TestTrue(TEXT("Late attachment updates cannot reveal a portal weapon"), LateWeapon->IsHidden());
		auto* HiddenAmmo = World->SpawnActor<AACEWorldEntityActor>();
		HiddenAmmo->AttachToActor(Pawn, FAttachmentTransformRules::KeepWorldTransform);
		HiddenAmmo->SetActorHiddenInGame(true);
		VR->UpdatePortalEquipmentVisibility();
		const FVector PortalOrigin = VR->TrackingOrigin->GetComponentLocation();
		Pawn->AddActorWorldOffset(FVector(250, 100, 50)); VR->MoveStick = FVector2D(0, 1); VR->PrepareMovement(.016f);
		VR->GetMovement(F, R, Run); VR->UpdatePortalView();
		TestTrue(TEXT("Portal loading locks stick and room-scale pawn movement"), F == 0 && R == 0 && VR->GetRoomScaleDelta().IsNearlyZero());
		TestTrue(TEXT("Destination pawn movement cannot move the portal view"), VR->TrackingOrigin->GetComponentLocation().Equals(PortalOrigin));
		const FQuat BeforeLook = VR->Head->GetComponentQuat(); VR->Head->AddLocalRotation(FRotator(0, 45, 0)); VR->UpdatePortalView();
		TestFalse(TEXT("The HMD can look around inside portal space"), VR->Head->GetComponentQuat().Equals(BeforeLook));
		VR->bWasLoading = PC->bEnterWorldLoading = VR->bPortalViewActive = false; VR->ResetTrackingOrigin();
		VR->UpdatePortalEquipmentVisibility();
		TestFalse(TEXT("Portal exit restores visible equipment"), PortalWeapon->IsHidden());
		TestFalse(TEXT("Portal exit also restores late equipment"), LateWeapon->IsHidden());
		TestTrue(TEXT("Portal exit preserves intentionally hidden equipped ammo"), HiddenAmmo->IsHidden());
		PortalWeapon->Destroy(); HiddenAmmo->Destroy(); LateWeapon->Destroy();
		TestTrue(TEXT("Portal exit restores the player floor"), FVector::Distance(VR->TrackingOrigin->GetComponentLocation(), Pawn->GetActorLocation()) < 300);
		PC->LoadingScreenActor = nullptr; Portal->Destroy();
	}
	{
		VR->Settings->bShowCompass=true;VR->Settings->bPinVitalsToView=true;
		VR->bInventoryOpen=false;VR->bSettingsOpen=false;VR->bKeyboardOpen=false;
		VR->Settings->bPinChatToView=false;VR->Settings->bShowWristSpellBar=false;PC->bJumpCharging=false;
		++GFrameCounter;VR->UpdatePanels();
		TestFalse(TEXT("Native-only HUD does not redraw the closed desktop canvas"),VR->RetailPanel->IsVisible() || VR->RetailPanel->IsComponentTickEnabled());
		VR->bInventoryOpen=true;VR->bUseDesktopMenu=true;VR->UpdatePanels();
		TestTrue(TEXT("Opening inventory restores desktop rendering and interactions"),VR->RetailPanel->IsVisible() && VR->RetailPanel->IsComponentTickEnabled() && VR->RetailPanel->GetCollisionEnabled()==ECollisionEnabled::QueryOnly);
		VR->bInventoryOpen=false;VR->bChatOpen=true;VR->UpdatePanels();
		TestTrue(TEXT("Native chat opens without redrawing the desktop canvas"),VR->ChatPanel->IsVisible() && !VR->RetailPanel->IsVisible() && !VR->RetailPanel->IsComponentTickEnabled());
		VR->bChatOpen=false;VR->UpdatePanels();
		VR->Settings->bShowFellowship=true;
		VR->NextNativeHUDUpdate=0;VR->UpdateNativeHUD(true);
		TestTrue(TEXT("Native compass is independent of the desktop canvas"),VR->CompassPanel->IsVisible() && VR->NativeCompass.IsValid());
		TestTrue(TEXT("Native vitals use Slate instead of a desktop crop"),VR->VitalsPanel->IsVisible() && VR->NativeVitals.IsValid() && VR->VitalsPanel->GetWidget()==nullptr);
		TestTrue(TEXT("Native fellowship subscribes while visible"),VR->NativeFellowship.IsValid() && VR->Client->bVRFellowshipUpdates);
		VR->Settings->bShowFellowship=false;VR->UpdateNativeHUD(true);
		TestFalse(TEXT("Hidden fellowship releases updates and ticking"),VR->Client->bVRFellowshipUpdates || VR->FellowshipPanel->IsComponentTickEnabled());
		VR->Settings->bShowCompass=false;VR->Settings->bPinVitalsToView=false;VR->UpdateNativeHUD(true);
		TestFalse(TEXT("Hidden compass stops ticking"),VR->CompassPanel->IsComponentTickEnabled());
		TestFalse(TEXT("Hidden vitals cannot intercept controls"),VR->VitalsPanel->GetCollisionEnabled()!=ECollisionEnabled::NoCollision);
		FACEPlayerVitals V;V.bValid=true;V.Health=275;V.MaxHealth=400;V.Stamina=310;V.MaxStamina=450;V.Mana=500;V.MaxMana=600;
		VR->Settings->bVitalsLocked=false; VR->Settings->bCompassLocked=false;
		VR->NativeVitals->Refresh(V); VR->NativeCompass->UpdateHeading(30,.016f,true);
		VR->NativeCompass->Coordinates=TEXT("33.6S, 72.8E");
		VR->NativeCompass->Markers={{{160,120},FLinearColor::Yellow,1,0,true},{{245,180},FLinearColor::Green,2,0,false},{{275,250},FLinearColor::Red,3,1,false}};
		FACEFellowshipInfo Fellow;Fellow.bValid=true;Fellow.Name=TEXT("Adventurers of Dereth");Fellow.bOpen=true;Fellow.bEvenShare=true;Fellow.LeaderGuid=100;
		for(int32 I=0;I<9;++I){FACEFellowshipMember M;M.Guid=100+I;M.Name=FString::Printf(TEXT("Adventurer %d"),I+1);M.Level=275;M.HealthCur=275-I*20;M.HealthMax=400;M.StaminaCur=310;M.StaminaMax=450;M.ManaCur=500;M.ManaMax=600;Fellow.Members.Add(M);}
		TestTrue(TEXT("Fellowship roster updates native HUD"),VR->NativeFellowship->Refresh(Fellow,101,100));
		TestFalse(TEXT("Unchanged fellowship data avoids repainting"),VR->NativeFellowship->Refresh(Fellow,101,100));
		TestEqual(TEXT("First fellowship row skips self"),VR->NativeFellowship->MemberGuidAt(10),101);
		TestEqual(TEXT("Last fellowship row remains selectable"),VR->NativeFellowship->MemberGuidAt(8+7*69),108);
		TestTrue(TEXT("Fellowship removes title/status and empty rows"),VR->NativeFellowship->ComputeDesiredSize(1).Y<=608);
		Fellow.Name=TEXT("Renamed");Fellow.Members[0].HealthCur=1;
		TestFalse(TEXT("Hidden fellowship title and self-vitals do not redraw"),VR->NativeFellowship->Refresh(Fellow,101,100));
		if(FApp::CanEverRender())
		{
			FWidgetRenderer Renderer(true,true);
			const FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation/VR");IFileManager::Get().MakeDirectory(*Directory,true);
			for(int I=0;I<3;++I)
			{
				const TSharedRef<SWidget> Widget=I==0?StaticCastSharedRef<SWidget>(VR->NativeVitals.ToSharedRef()):I==1?StaticCastSharedRef<SWidget>(VR->NativeCompass.ToSharedRef()):StaticCastSharedRef<SWidget>(VR->NativeFellowship.ToSharedRef());
				const FIntPoint Size=I==0?FIntPoint(480,240):I==1?FIntPoint(400,500):FIntPoint(VR->NativeFellowship->ComputeDesiredSize(1).X,VR->NativeFellowship->ComputeDesiredSize(1).Y);
				auto* Target=FWidgetRenderer::CreateTargetFor(FVector2D(Size),TF_Bilinear,true);
				for(int Pass=0;Pass<3;++Pass){Renderer.DrawWidget(Target,Widget,FVector2D(Size),0);FlushRenderingCommands();}
				TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
				TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(Size.X,Size.Y,Pixels,PNG);
				FFileHelper::SaveArrayToFile(PNG,*(Directory/(I==0?TEXT("NativeVitals.png"):I==1?TEXT("NativeCompass.png"):TEXT("NativeFellowship.png"))));
			}
		}
	}
	{
		VR->bInventoryOpen=true;VR->bUseDesktopMenu=false;VR->bSettingsOpen=false;
		VR->Settings->bPinChatToView=false;VR->Settings->bShowWristSpellBar=false;
		VR->UpdatePanels();
		TestNotNull(TEXT("Inventory creates the native VR menu"),VR->GameplayMenu.Get());
		TestTrue(TEXT("Native inventory owns interactions"),VR->GameplayMenuPanel && VR->GameplayMenuPanel->IsVisible() && VR->GameplayMenuPanel->GetCollisionEnabled()==ECollisionEnabled::QueryOnly);
		TestFalse(TEXT("Native inventory does not render a second desktop menu"),VR->RetailPanel->IsVisible());
		for(auto* HUD:{VR->VitalsPanel.Get(),VR->CompassPanel.Get(),VR->FellowshipPanel.Get(),VR->WristPanel.Get(),VR->ChatPanel.Get()})
			TestTrue(TEXT("Native menu paints and receives clicks above floating HUD panels"),VR->GameplayMenuPanel->TranslucencySortPriority>HUD->TranslucencySortPriority);
		auto* Menu=VR->GameplayMenu.Get();Menu->TakeWidget();Menu->OpenPage("Inventory");Menu->RefreshIfDirty();
		const int32 Refreshes=Menu->GetRefreshCount();
		const double MenuStart=FPlatformTime::Seconds();
		for(int I=0;I<90;++I)VR->UpdateGameplayMenu(true,1.f/90.f);
		AddInfo(FString::Printf(TEXT("Unchanged native menu update: %.3f ms/frame"),(FPlatformTime::Seconds()-MenuStart)*1000./90.));
		TestEqual(TEXT("Unchanged native inventory avoids widget rebuilding at 90 Hz"),Menu->GetRefreshCount(),Refreshes);
		VR->Client->OnTradeStateChanged.Broadcast(ACEGameEvent::ResetTrade);Menu->RefreshIfDirty();
		TestEqual(TEXT("Trade updates refresh native data without replacing the current page"),Menu->GetPage(),FName("Inventory"));
		TestEqual(TEXT("Server trade events invalidate the native view"),Menu->GetRefreshCount(),Refreshes+1);
		FACEWorldObject Item;Item.Guid=99210;Item.Name=TEXT("Treated Healing Kit");Item.ContainerId=VR->Client->GetPlayerGuid();Item.StackSize=10;Item.MaxStackSize=250;Item.IconId=0x060013B8;
		VR->Client->Session->WorldObjects.Add(Item.Guid,Item);
		Menu->SelectItem(Item.Guid);PC->DatGameplayBinder->SelectedStackAmount=3;
		Menu->RefreshIfDirty();
		TestTrue(TEXT("Native icons use the same mask and overlay composition as desktop inventory"),Menu->IconTextures.Contains(VR->Client->GetUIResourceResolver()->ResolveItemForeground(Item.IconId,Item.IconOverlayId,Item.UiEffects)));
		{
			auto& Presented=VR->Client->Session->WorldObjects[Item.Guid];
			Presented.Name=TEXT("Ring");Presented.MaterialType=59;Presented.Structure=50;Presented.MaxStructure=100;
			Menu->bDirty=true;Menu->RefreshIfDirty();
			TFunction<bool(const TSharedRef<SWidget>&)> HasMaterialName;
			HasMaterialName=[&](const TSharedRef<SWidget>& Widget)
			{
				if(Widget->GetType()==FName("STextBlock") && StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString()==TEXT("Copper Ring"))return true;
				auto* Children=Widget->GetChildren();
				for(int32 I=0;I<Children->Num();++I)if(HasMaterialName(Children->GetChildAt(I)))return true;
				return false;
			};
			TestTrue(TEXT("Native inventory selection displays its material prefix"),HasMaterialName(Menu->TakeWidget()));
			TestTrue(TEXT("Native item icons preserve the retail partial-structure meter"),Menu->IconTextures.Contains(
				VR->Client->GetUIResourceResolver()->ResolveItemForeground(Presented.IconId,Presented.IconOverlayId,Presented.UiEffects,false,50,100)));
			Presented=Item;Menu->bDirty=true;Menu->RefreshIfDirty();
		}
		if(FApp::CanEverRender())
		{
			FVRTestControllers MenuControllers;
			auto PollMenuHands=[&]()
			{for(auto* Hand:{VR->LeftGrip.Get(),VR->RightGrip.Get(),VR->LeftAim.Get(),VR->RightAim.Get()})Hand->TickComponent(.016f,LEVELTICK_All,nullptr);};
			PollMenuHands();VR->CancelGestures();VR->UpdatePanels();
			auto PaintMenu=[&]()
			{
				VR->UpdateGameplayMenu(true,.016f);VR->GameplayMenuPanel->SetRedrawTime(0);
				for(int Pass=0;Pass<3;++Pass){VR->GameplayMenuPanel->RequestRedraw();VR->GameplayMenuPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();}
				if(VR->MenuInspectionPanel && VR->MenuInspectionPanel->IsVisible())
				{VR->MenuInspectionPanel->SetRedrawTime(0);for(int Pass=0;Pass<3;++Pass){VR->MenuInspectionPanel->RequestRedraw();VR->MenuInspectionPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();}}
			};
			auto ClickMenu=[&](bool Left,FName Tag)
			{
				PaintMenu();const auto Control=FindVRMenuControl(Menu->TakeWidget(),Tag);
				if(!TestTrue(FString::Printf(TEXT("Native menu control '%s' exists"),*Tag.ToString()),Control.IsValid()))return;
				const FGeometry G=Control->GetCachedGeometry();
				const FVector2D Pixel=Menu->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5));
				const FVector2D Size=VR->GameplayMenuPanel->GetDrawSize();
				const FVector Point=VR->GameplayMenuPanel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
				const FVector From=Point+VR->GameplayMenuPanel->GetForwardVector()*90;
				(Left?VR->LeftAim:VR->RightAim)->SetWorldLocationAndRotation(From,(Point-From).Rotation());
				auto* Pointer=Left?VR->LeftPointer.Get():VR->RightPointer.Get();Pointer->TickComponent(.016f,LEVELTICK_All,nullptr);
				TestTrue(FString::Printf(TEXT("Controller reaches native menu control '%s' through overlapping HUD"),*Tag.ToString()),Pointer->GetHoveredWidgetComponent()==VR->GameplayMenuPanel);
				TestTrue(FString::Printf(TEXT("Rendered button '%s' participates in the controller hit grid"),*Tag.ToString()),Pointer->IsOverInteractableWidget());
				VR->Trigger(Left,true);PaintMenu();VR->Trigger(Left,false);PaintMenu();
			};
			// Deliberately cover the item/nav area with a nearer HUD plane.
			const FTransform VitalsPose=VR->VitalsPanel->GetComponentTransform();
			const bool VitalsVisible=VR->VitalsPanel->IsVisible();
			VR->VitalsPanel->SetWorldTransform(VR->GameplayMenuPanel->GetComponentTransform());
			VR->VitalsPanel->AddWorldOffset(VR->GameplayMenuPanel->GetForwardVector()*5);
			VR->VitalsPanel->SetVisibility(true);VR->VitalsPanel->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			VR->Client->SelectObject(0);
			ClickMenu(false,FName(*FString::Printf(TEXT("Item_%d"),Item.Guid)));
			TestEqual(TEXT("Right trigger selects a native inventory item"),Menu->Selected,Item.Guid);
			const FName SelectionTag(*FString::Printf(TEXT("ItemSelection_%d"),Item.Guid));
			const auto SelectionFrame=FindVRMenuControl(Menu->TakeWidget(),SelectionTag);
			TestTrue(TEXT("Selected inventory item has a visible retail frame without stealing pointer hits"),SelectionFrame && SelectionFrame->GetVisibility()==EVisibility::HitTestInvisible);
			Menu->ClearItemSelection();PaintMenu();
			const auto ClearedSelectionFrame=FindVRMenuControl(Menu->TakeWidget(),SelectionTag);
			TestTrue(TEXT("Retail frame disappears on the next paint after clearing selection"),ClearedSelectionFrame && ClearedSelectionFrame->GetVisibility()==EVisibility::Hidden);
			TestFalse(TEXT("Equipment is not a separate navigation tab"),FindVRMenuControl(Menu->TakeWidget(),"Equipment").IsValid());
			ClickMenu(true,"EquipmentToggle");TestEqual(TEXT("Left trigger on player icon opens equipment"),Menu->GetPage(),FName("Equipment"));
			ClickMenu(false,"EquipmentToggle");TestEqual(TEXT("Player icon toggles back to inventory"),Menu->GetPage(),FName("Inventory"));
			ClickMenu(true,"EquipmentToggle");
			const FName MainPackTag(*FString::Printf(TEXT("Pack_%d"),VR->Client->GetPlayerGuid()));
			const auto PlayerButton=FindVRMenuControl(Menu->TakeWidget(),"EquipmentToggle");
			const auto MainPackButton=FindVRMenuControl(Menu->TakeWidget(),MainPackTag);
			TestTrue(TEXT("Player icon sits above pack icons"),PlayerButton && MainPackButton && PlayerButton->GetCachedGeometry().GetAbsolutePosition().Y<MainPackButton->GetCachedGeometry().GetAbsolutePosition().Y);
			ClickMenu(false,MainPackTag);TestEqual(TEXT("Selecting a pack returns from equipment to that inventory"),Menu->GetPage(),FName("Inventory"));
			VR->VitalsPanel->SetWorldTransform(VitalsPose);VR->VitalsPanel->SetVisibility(VitalsVisible);
			TestEqual(TEXT("Switching pages clears invisible inventory context"),Menu->Selected,0);
            TestEqual(TEXT("Switching pages also clears the shared hidden-item target"),VR->Client->GetSelectedObject().Guid,0);
            TestEqual(TEXT("Full main pack includes all occupied and empty destinations"),Menu->ItemDestinations.FilterByPredicate([&](const auto& D){return D.Container==VR->Client->GetPlayerGuid() && D.Widget.Pin()->GetTag().ToString().StartsWith(TEXT("PackSlot_"));}).Num(),102);
            TestFalse(TEXT("Inventory no longer has page navigation"),FindVRMenuControl(Menu->TakeWidget(),"Next").IsValid() || FindVRMenuControl(Menu->TakeWidget(),"Previous").IsValid());
            Menu->SelectItem(Item.Guid);PaintMenu();
            TestFalse(TEXT("Inventory has no duplicate self-use action"),FindVRMenuControl(Menu->TakeWidget(),"Use selected on yourself").IsValid());
            // Drive native controls and inspect real loopback packets, not just local state.
            auto* NativeSockets=ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            auto NativeAddress=NativeSockets->CreateInternetAddr();bool AddressValid=false;
            NativeAddress->SetIp(TEXT("127.0.0.1"),AddressValid);NativeAddress->SetPort(0);
            auto* NativeReceiver=NativeSockets->CreateSocket(NAME_DGram,TEXT("Native menu receiver"),false);
            auto& NativeSession=*VR->Client->Session;
            if(NativeReceiver && NativeReceiver->Bind(*NativeAddress))
            {
                NativeReceiver->GetAddress(*NativeAddress);
                NativeSession.SocketC2S=NativeSockets->CreateSocket(NAME_DGram,TEXT("Native menu sender"),false);
                NativeSession.ServerC2SAddr=NativeAddress;NativeSession.IssacClient=MakeUnique<FACEIsaac>(123);
                auto ActionPayload=[&](uint32 Wanted,int32 ExpectedCount=1)
                {
                    TArray<uint8> Result;int32 Count=0;
                    for(const auto& P:NativeSession.CachedC2SPackets)
                    {
                        FACEBinaryReader NativeReader(P.Value.Payload);NativeReader.Skip(16);
                        if(NativeReader.ReadUInt32()!=ACEOpcode::GameAction)continue;NativeReader.ReadUInt32();
                        if(NativeReader.ReadUInt32()!=Wanted)continue;
                        Result.Reset();Result.Append(P.Value.Payload.GetData()+28,P.Value.Payload.Num()-28);++Count;
                    }
                    TestEqual(FString::Printf(TEXT("Expected native action count for %x"),Wanted),Count,ExpectedCount);
                    return Result;
                };
                const uint32 OldCaps=NativeSession.VRCapabilities;
                {
                    TGuardValue<uint32> StandardServer(NativeSession.VRCapabilities,0);
                    FACEWorldObject Helmet;Helmet.Guid=99221;Helmet.Name=TEXT("VR equipment test");
                    Helmet.ContainerId=VR->Client->GetPlayerGuid();Helmet.ItemType=ACEItemType::Armor;
                    Helmet.ItemUseable=1;Helmet.ValidLocations=ACEEquipMask::HeadWear;
                    for(bool Left:{false,true})
                    {
                        NativeSession.WorldObjects.Add(Helmet.Guid,Helmet);NativeSession.ClearPendingUse();
                        NativeSession.CancelEquipmentSwap();PC->DatGameplayBinder->CancelPendingUseWith();
                        Menu->OpenPage("Inventory");Menu->SelectItem(Helmet.Guid);PaintMenu();
                        NativeSession.CachedC2SPackets.Reset();ClickMenu(Left,"Use / Equip");
                        const auto Equip=ActionPayload(ACEGameAction::GetAndWieldItem);
                        if(TestTrue(TEXT("Standard equipment action carries item and slot"),Equip.Num()>=8))
                        {
                            FACEBinaryReader EquipReader(Equip);TestEqual(TEXT("Controller equips the selected armor"),EquipReader.ReadUInt32(),uint32(Helmet.Guid));
                            TestEqual(TEXT("Controller uses the armor's retail slot"),EquipReader.ReadUInt32(),uint32(ACEEquipMask::HeadWear));
                        }
                        FACEBinaryWriter Wield;Wield.WriteUInt32(Helmet.Guid);Wield.WriteUInt32(ACEEquipMask::HeadWear);
                        FACEBinaryReader WieldAck(Wield.GetData());NativeSession.HandleWieldItem(WieldAck);
                        TestFalse(TEXT("Standard equip acknowledgement completes the pending swap"),NativeSession.IsUseBusy());
                        Menu->OpenPage("Equipment");PaintMenu();ClickMenu(Left,"Item_99221");
                        TestEqual(TEXT("Controller selects the worn armor"),Menu->Selected,Helmet.Guid);
                        TestFalse(TEXT("Worn gear cannot be offered directly to another player"),FindVRMenuControl(Menu->TakeWidget(),"Give")->IsEnabled());
                        TestFalse(TEXT("Worn gear must be removed before a world drop"),FindVRMenuControl(Menu->TakeWidget(),"Drop")->IsEnabled());
                        Menu->BeginItemPointer(Helmet.Guid);TestEqual(TEXT("Worn gear can start a drag back to a pack"),Menu->DragItem,Helmet.Guid);Menu->CancelItemPointer();
                        Menu->ContentScroll->ScrollDescendantIntoView(FindVRMenuControl(Menu->TakeWidget(),"Use / Equip"),false,EDescendantScrollDestination::Center);PaintMenu();
                        NativeSession.CachedC2SPackets.Reset();ClickMenu(Left,"Use / Equip");
                        const auto Unequip=ActionPayload(ACEGameAction::PutItemInContainer);
                        if(TestTrue(TEXT("Standard unequip action carries item and pack"),Unequip.Num()>=8))
                        {
                            FACEBinaryReader UnequipReader(Unequip);TestEqual(TEXT("Controller unequips the selected armor"),UnequipReader.ReadUInt32(),uint32(Helmet.Guid));
                            TestEqual(TEXT("Controller returns armor to the main pack"),UnequipReader.ReadUInt32(),uint32(Helmet.ContainerId));
                        }
                        FACEBinaryWriter Put;Put.WriteUInt32(Helmet.Guid);Put.WriteUInt32(Helmet.ContainerId);Put.WriteInt32(0);Put.WriteInt32(0);
                        FACEBinaryReader PutAck(Put.GetData());NativeSession.HandleInventoryPutObjInContainer(PutAck);
                    }
                    NativeSession.WorldObjects.Remove(Helmet.Guid);Menu->bDirty=true;
                }
                {
                    TGuardValue<uint32> StandardServer(NativeSession.VRCapabilities,0);
                    Menu->OpenPage("Inventory");Menu->SelectItem(Item.Guid);PaintMenu();
                    NativeSession.WorldObjects[Item.Guid].ItemUseable=8;
                    NativeSession.ClearPendingUse();PC->DatGameplayBinder->CancelPendingUseWith();
                    const auto Button=FindVRMenuControl(Menu->TakeWidget(),"Use / Equip");
                    const auto G=Button->GetCachedGeometry();
                    const FVector2D Pixel=Menu->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5));
                    const FVector2D Size=VR->GameplayMenuPanel->GetDrawSize();
                    const FVector Point=VR->GameplayMenuPanel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
                    VR->RightAim->SetWorldLocationAndRotation(Point+VR->GameplayMenuPanel->GetForwardVector()*90,(-VR->GameplayMenuPanel->GetForwardVector()).Rotation());
                    VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);
                    NativeSession.CachedC2SPackets.Reset();VR->Trigger(false,true);
                    const int32 BeforeRefresh=Menu->GetRefreshCount();
                    VR->Client->OnObjectCreated.Broadcast(NativeSession.WorldObjects[Item.Guid]);PaintMenu();
                    TestEqual(TEXT("Inventory updates preserve a pressed native action until release"),Menu->GetRefreshCount(),BeforeRefresh);
                    VR->Trigger(false,false);PaintMenu();
                    ActionPayload(ACEGameAction::Use);
                    TestTrue(TEXT("Deferred inventory refresh resumes after release"),Menu->GetRefreshCount()>BeforeRefresh);
                    NativeSession.ClearPendingUse();
                }
                // Physical A/B route to native actions, retain separate world
                // bindings, and never synthesize trigger clicks or begin a drag.
                const auto SavedMenuBindings=VR->Settings->MenuButtonBindings;
                VR->Settings->MenuButtonBindings.Reset();Menu->OpenPage("Inventory");Menu->SelectItem(Item.Guid);PaintMenu();
                auto AimMenuControl=[&](FName Tag)
                {
                    PaintMenu();const auto Control=FindVRMenuControl(Menu->TakeWidget(),Tag);
                    if(!Control)return;
                    const auto G=Control->GetCachedGeometry();
                    const FVector2D Pixel=Menu->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5));
                    const FVector2D Size=VR->GameplayMenuPanel->GetDrawSize();
                    const FVector Point=VR->GameplayMenuPanel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
                    VR->RightAim->SetWorldLocationAndRotation(Point+VR->GameplayMenuPanel->GetForwardVector()*90,(-VR->GameplayMenuPanel->GetForwardVector()).Rotation());
                    VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);VR->FeedbackHand=1;
                };
                const FName ItemTag(*FString::Printf(TEXT("Item_%d"),Item.Guid));
                AimMenuControl(ItemTag);
                NativeSession.WorldObjects[Item.Guid].ItemUseable=8;
                NativeSession.ClearPendingUse();PC->DatGameplayBinder->CancelPendingUseWith();
                NativeSession.CachedC2SPackets.Reset();VR->RouteControllerButton("VRSelect",true);VR->RouteControllerButton("VRSelect",false);
                {const auto Bytes=ActionPayload(ACEGameAction::Use);if(Bytes.Num()>=4){FACEBinaryReader NativeReader(Bytes);TestEqual(TEXT("A uses pointed native item"),NativeReader.ReadUInt32(),uint32(Item.Guid));}}
                NativeSession.ClearPendingUse();
                AimMenuControl(ItemTag);Menu->UseSource=Item.Guid;
                VR->RouteControllerButton("VRPrevious",true);VR->RouteControllerButton("VRPrevious",false);PaintMenu();
                TestTrue(TEXT("B opens native item inspector alongside inventory"),Menu->bInspectionOpen && Menu->InspectItem==Item.Guid && Menu->Page=="Inventory");
                TestEqual(TEXT("Inspection preserves armed use-on-item without using it"),Menu->UseSource,Item.Guid);Menu->UseSource=0;
                TestTrue(TEXT("Inspector is a separate visible panel"),VR->MenuInspectionPanel && VR->MenuInspectionPanel->IsVisible());
                TestTrue(TEXT("Inspector sits to the right in the menu frame"),VR->MenuInspectionPanel && VR->MenuInspectionPanel->GetRelativeLocation().Y<-800);
                TestFalse(TEXT("Quick actions do not capture a pointer or drag"),Menu->DragItem || VR->bRightPointerPressed);
                if(VR->MenuInspectionPanel)
                {
                    const auto Close=FindVRMenuControl(Menu->GetInspectionWidget(),"Close inspection");
                    const auto RootGeometry=Menu->GetInspectionWidget()->GetCachedGeometry();
                    const auto CloseGeometry=Close->GetCachedGeometry();
                    const FVector2D Pixel=RootGeometry.AbsoluteToLocal(CloseGeometry.LocalToAbsolute(CloseGeometry.GetLocalSize()*.5));
                    const FVector2D Size=VR->MenuInspectionPanel->GetDrawSize();
                    const FVector Point=VR->MenuInspectionPanel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
                    VR->RightAim->SetWorldLocationAndRotation(Point+VR->MenuInspectionPanel->GetForwardVector()*90,(-VR->MenuInspectionPanel->GetForwardVector()).Rotation());
                    VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);VR->Trigger(false,true);VR->Trigger(false,false);PaintMenu();
                    TestFalse(TEXT("Controller can close the side inspector without leaving inventory"),Menu->bInspectionOpen);
                }
                TestFalse(TEXT("Closed inspector stops drawing and interaction"),VR->MenuInspectionPanel && (VR->MenuInspectionPanel->IsVisible() || VR->MenuInspectionPanel->IsComponentTickEnabled() || VR->MenuInspectionPanel->GetCollisionEnabled()!=ECollisionEnabled::NoCollision));
                VR->Settings->MenuButtonBindings.Add("VRSelect","VRMenuInspect");
                AimMenuControl(ItemTag);VR->RouteControllerButton("VRSelect",true);VR->RouteControllerButton("VRSelect",false);
                TestTrue(TEXT("Rebound A inspects instead of using"),Menu->bInspectionOpen);
                VR->Settings->MenuButtonBindings=SavedMenuBindings;
                Menu->bInspectionOpen=false;
                // Selection cannot change the inventory extent or move its slots.
                Menu->ClearItemSelection();Menu->bDirty=true;PaintMenu();
                const auto BeforeSlot=FindVRMenuControl(Menu->TakeWidget(),ItemTag)->GetCachedGeometry();
                const FVector2D BeforeSize=Menu->Body->GetDesiredSize();
                Menu->SelectItem(Item.Guid);PaintMenu();
                TestTrue(TEXT("Selecting a stack leaves inventory content extent unchanged"),Menu->Body->GetDesiredSize().Equals(BeforeSize,.1f));
                TestTrue(TEXT("Selecting a stack leaves its icon in place"),FindVRMenuControl(Menu->TakeWidget(),ItemTag)->GetCachedGeometry().GetAbsolutePosition().Equals(BeforeSlot.GetAbsolutePosition(),.1f));
                // An Ust is a local UI action in every mode, followed by one
                // explicit salvage packet. Offers must never drop real items.
                FACEWorldObject Ust;Ust.Guid=99220;Ust.Name=TEXT("Ust");Ust.ItemType=0x20000000;
                Ust.ContainerId=VR->Client->GetPlayerGuid();Ust.ItemUseable=1;Ust.IconId=0x060013B8;
                NativeSession.WorldObjects.Add(Ust.Guid,Ust);NativeSession.WorldObjects[Item.Guid].MaterialType=63;
                Menu->bDirty=true;AimMenuControl("Item_99220");NativeSession.CachedC2SPackets.Reset();
                VR->RouteControllerButton("VRSelect",true);VR->RouteControllerButton("VRSelect",false);PaintMenu();
                TestTrue(TEXT("Using Ust opens the native salvage page"),Menu->Page=="Salvage" && PC->DatGameplayBinder->OpenSalvageToolGuid==Ust.Guid);
                TestTrue(TEXT("Empty native salvage starts with default inventory squares"),FindVRMenuControl(Menu->TakeWidget(),"SalvageEmpty_0").IsValid()
                    && FindVRMenuControl(Menu->TakeWidget(),"SalvageEmpty_11").IsValid());
                ActionPayload(ACEGameAction::Use,0);
                Menu->SelectItem(Item.Guid);PaintMenu();ClickMenu(false,"Add item");
                TestTrue(TEXT("Native salvage can queue the selected item"),PC->DatGameplayBinder->SalvageQueueGuids.Contains(Item.Guid));
                TestTrue(TEXT("Native salvage keeps a visible empty square after its offer"),FindVRMenuControl(Menu->TakeWidget(),"SalvageEmpty_1").IsValid());
                AimMenuControl("SalvageItem_99210");VR->Trigger(false,true);
                Menu->bItemDragging=true;Menu->bDragFromSalvage=true;
                Menu->FinishItemPointer(false,FVector2D(-100,-100),true);VR->Trigger(false,false);PaintMenu();
                TestFalse(TEXT("Dragging an offer out removes it from salvage"),PC->DatGameplayBinder->SalvageQueueGuids.Contains(Item.Guid));
                TestTrue(TEXT("Removing offer leaves inventory ownership unchanged"),NativeSession.WorldObjects[Item.Guid].ContainerId==VR->Client->GetPlayerGuid());
                Menu->SelectItem(Item.Guid);PaintMenu();ClickMenu(false,"Add item");
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Salvage items");
                {const auto Bytes=ActionPayload(ACEGameAction::CreateTinkeringTool);FACEBinaryReader NativeReader(Bytes);
                    if(TestTrue(TEXT("Native salvage sends tool and selected item list"),NativeReader.CanRead(12)))
                    {TestEqual(TEXT("Salvage uses activated Ust"),NativeReader.ReadUInt32(),uint32(Ust.Guid));TestEqual(TEXT("Salvage has one offered item"),NativeReader.ReadUInt32(),1u);TestEqual(TEXT("Salvage contains selected inventory item"),NativeReader.ReadUInt32(),uint32(Item.Guid));}}
                TestTrue(TEXT("Submitted native offers clear like retail"),PC->DatGameplayBinder->SalvageQueueGuids.IsEmpty());
                ClickMenu(false,"Close salvage");TestTrue(TEXT("Close returns to pack and clears the active Ust"),Menu->Page=="Inventory" && !PC->DatGameplayBinder->OpenSalvageToolGuid);
                NativeSession.WorldObjects.Remove(Ust.Guid);Menu->SelectItem(Item.Guid);Menu->bDirty=true;PaintMenu();
                for(bool VRDrop:{false,true})for(int32 Amount:{3,10})
                {
                    NativeSession.VRCapabilities=VRDrop?OldCaps|1024u:OldCaps&~1024u;
                    NativeSession.CachedC2SPackets.Reset();Menu->DropItem(Item.Guid,Amount);
                    const auto Bytes=ActionPayload(VRDrop?0xF7D0u:Amount==10?ACEGameAction::DropItem:ACEGameAction::StackableSplitTo3D);
                    FACEBinaryReader NativeReader(Bytes);
                    if(VRDrop)NativeReader.Skip(20);
                    TestEqual(TEXT("Native drop uses selected inventory GUID"),NativeReader.ReadUInt32(),uint32(Item.Guid));
                    if(VRDrop || Amount!=10)TestEqual(TEXT("Only partial native drops request a split"),NativeReader.ReadUInt32(),VRDrop && Amount==10?0u:uint32(Amount));
                }
                NativeSession.VRCapabilities=OldCaps;
                Menu->OpenPage("Inventory");Menu->SelectItem(Item.Guid);PaintMenu();
                // A real trigger drag leaves the quad, shows the held icon, then sends one drop.
                PC->DatGameplayBinder->SelectedStackAmount=3;
                const auto DragControl=FindVRMenuControl(Menu->TakeWidget(),FName(*FString::Printf(TEXT("Item_%d"),Item.Guid)));
                if(TestTrue(TEXT("Native drag source is rendered"),DragControl.IsValid()))
                {
                    const auto G=DragControl->GetCachedGeometry();
                    const FVector2D Pixel=Menu->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5));
                    const FVector2D Draw=VR->GameplayMenuPanel->GetDrawSize();
                    const FVector Hit=VR->GameplayMenuPanel->GetComponentTransform().TransformPosition(FVector(0,Draw.X*.5-Pixel.X,Draw.Y*.5-Pixel.Y));
                    VR->RightAim->SetWorldLocationAndRotation(Hit+VR->GameplayMenuPanel->GetForwardVector()*90,(-VR->GameplayMenuPanel->GetForwardVector()).Rotation());
                    VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);VR->Trigger(false,true);
                    TestEqual(TEXT("Native trigger freezes the selected split amount"),Menu->DragAmount,3);
                    VR->RightAim->SetWorldLocationAndRotation(FVector(0,0,200),FRotator(-45,180,0));
                    VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);VR->UpdateGameplayMenu(true,.016f);
                    TestTrue(TEXT("Dragging into the world shows the held item icon"),Menu->bItemDragging && VR->MenuDragPanel && VR->MenuDragPanel->IsVisible());
                    NativeSession.CachedC2SPackets.Reset();VR->Trigger(false,false);VR->UpdateGameplayMenu(true,.016f);
                    ActionPayload((OldCaps&1024u)?0xF7D0u:ACEGameAction::StackableSplitTo3D);
                    TestFalse(TEXT("Release clears the native drag preview"),Menu->DragItem || VR->MenuDragPanel->IsVisible());
                }
                // Empty-slot drops split or reorder using the same packet as retail.
                Menu->OpenPage("Inventory");Menu->SelectItem(Item.Guid);PaintMenu();
                for(int32 Amount:{3,10})
                {
                    PC->DatGameplayBinder->SelectedStackAmount=Amount;Menu->BeginItemPointer(Item.Guid);
                    const auto Empty=Menu->ItemDestinations.FindByPredicate([](const auto& D){return D.Container && !D.Item && D.Slot==4;});
                    if(TestTrue(TEXT("Native full pack has a usable empty destination"),Empty!=nullptr))
                    {
                        const auto G=Empty->Widget.Pin()->GetCachedGeometry();
                        const FVector2D Pixel=Menu->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5));
                        NativeSession.CachedC2SPackets.Reset();Menu->bItemDragging=true;
                        Menu->FinishItemPointer(true,Pixel,false);
                        const auto Bytes=ActionPayload(Amount==10?ACEGameAction::PutItemInContainer:ACEGameAction::StackableSplitToContainer);
                        FACEBinaryReader NativeReader(Bytes);
                        TestEqual(TEXT("Native slot move/split preserves source GUID"),NativeReader.ReadUInt32(),uint32(Item.Guid));
                        TestEqual(TEXT("Native slot move/split preserves destination pack"),NativeReader.ReadUInt32(),uint32(Menu->Pack));
                    }
                }
                // Use-on-item keeps the source, validates target descriptors and sends both GUIDs.
                auto& Kit=NativeSession.WorldObjects[Item.Guid];Kit.ItemUseable=0x000C0008;Kit.TargetType=ACEItemType::Armor;
                FACEWorldObject Armor;Armor.Guid=99211;Armor.Name=TEXT("Test chest armor");Armor.ItemType=ACEItemType::Armor;Armor.ContainerId=100;
                NativeSession.WorldObjects.Add(Armor.Guid,Armor);
                Menu->UseSource=Item.Guid;NativeSession.CachedC2SPackets.Reset();Menu->SelectItem(Armor.Guid);
                {
                    const auto Bytes=ActionPayload(ACEGameAction::UseWithTarget);FACEBinaryReader NativeReader(Bytes);
                    TestEqual(TEXT("Native targeted item use preserves source"),NativeReader.ReadUInt32(),uint32(Item.Guid));
                    TestEqual(TEXT("Native targeted item use preserves receiving item"),NativeReader.ReadUInt32(),uint32(Armor.Guid));
                }
                NativeSession.WorldObjects.Remove(Armor.Guid);
                // Creation reads the edited name and subsequent membership drives management.
                NativeSession.Fellowship={};Menu->OpenPage("Fellowship");PaintMenu();
                Menu->TextEntry->SetText(FText::FromString(TEXT("VR Companions")));
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Create fellowship");
                {
                    const auto Bytes=ActionPayload(ACEGameAction::FellowshipCreate);FACEBinaryReader NativeReader(Bytes);
                    TestEqual(TEXT("Create sends the entered fellowship name"),NativeReader.ReadString16L(),FString("VR Companions"));
                    TestEqual(TEXT("Create sends the share-XP choice"),NativeReader.ReadUInt32(),1u);
                }
                FACEFellowshipMember Self;Self.Guid=100;Self.Name=TEXT("You");Self.Level=100;
                NativeSession.Fellowship.bValid=true;NativeSession.Fellowship.Name=TEXT("VR Companions");
                NativeSession.Fellowship.LeaderGuid=100;NativeSession.Fellowship.Members={Self};
                FACEWorldObject Recruit;Recruit.Guid=99212;Recruit.Name=TEXT("Nearby friend");Recruit.bIsPlayer=true;
                Recruit.ItemType=ACEItemType::Creature;Recruit.bHasPosition=true;Recruit.Position.CellId=1;
                NativeSession.WorldObjects.Add(Recruit.Guid,Recruit);Menu->Changed();
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Recruit Nearby friend");
                {
                    const auto Bytes=ActionPayload(ACEGameAction::FellowshipRecruit);FACEBinaryReader NativeReader(Bytes);
                    TestEqual(TEXT("Recruit targets the chosen nearby player"),NativeReader.ReadUInt32(),uint32(Recruit.Guid));
                }
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Open fellowship");
                {
                    const auto Bytes=ActionPayload(ACEGameAction::FellowshipChangeOpenness);FACEBinaryReader NativeReader(Bytes);
                    TestEqual(TEXT("Open sends retail openness request"),NativeReader.ReadUInt32(),1u);
                }
                FACEFellowshipMember Other;Other.Guid=Recruit.Guid;Other.Name=Recruit.Name;
                NativeSession.Fellowship.Members.Add(Other);Menu->FellowSelection=Other.Guid;Menu->Changed();
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Make leader");
                {const auto Bytes=ActionPayload(ACEGameAction::FellowshipAssignNewLeader);FACEBinaryReader NativeReader(Bytes);TestEqual(TEXT("Leadership targets the selected roster member"),NativeReader.ReadUInt32(),uint32(Other.Guid));}
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Dismiss member");
                {const auto Bytes=ActionPayload(ACEGameAction::FellowshipDismiss);FACEBinaryReader NativeReader(Bytes);TestEqual(TEXT("Dismiss targets the selected roster member"),NativeReader.ReadUInt32(),uint32(Other.Guid));}
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Leave fellowship");
                {const auto Bytes=ActionPayload(ACEGameAction::FellowshipQuit);FACEBinaryReader NativeReader(Bytes);TestEqual(TEXT("Leave does not disband"),NativeReader.ReadUInt32(),0u);}
                NativeSession.WorldObjects.Remove(Recruit.Guid);NativeSession.Fellowship={};
                // Drive every school, intersect level/search, and learn/forget
                // spells while the rendered book stays open.
                NativeSession.KnownSpells={81,5349,51,27,6,5,4,3,2,1};
                Menu->SpellSchool=Menu->SpellLevel=0;Menu->Search.Reset();Menu->OpenPage("Spellbook");PaintMenu();
                TestEqual(TEXT("Reopened spellbook restores the reused search entry"),Menu->TextEntry->GetVisibility(),ESlateVisibility::Visible);
                auto BookIds=[&]()
                {
                    TArray<int32> Ids;for(const auto& Target:Menu->MenuTargets)if(Target.SpellId && Target.Bar==INDEX_NONE)Ids.Add(Target.SpellId);
                    return Ids;
                };
                auto TypeSearch=[&](const TCHAR* Query)
                {
                    auto Keyboard=MakeShared<FACEVRPlatformTextEntry>(Menu->TextEntry,[]{});
                    Keyboard->SetTextFromVirtualKeyboard(FText::FromString(Query),ETextEntryType::TextEntryUpdated);
                    PaintMenu();
                };
                auto* BookDat=GI->GetSubsystem<UACEDatSubsystem>();
                auto ExpectedOrder=NativeSession.KnownSpells;
                ExpectedOrder.Sort([&](int32 A,int32 B)
                {
                    uint32 OrderA=0,OrderB=0;BookDat->TryGetSpellDisplayOrder(A,OrderA);BookDat->TryGetSpellDisplayOrder(B,OrderB);
                    return OrderA!=OrderB?OrderA<OrderB:A<B;
                });
                ExpectedOrder.SetNum(6);
                TestTrue(TEXT("Spellbook uses authored retail display order before pagination"),BookIds()==ExpectedOrder);
                ClickMenu(false,"Next");TestEqual(TEXT("Book fixture has a second page"),Menu->PageIndex,1);
                const TArray<TArray<int32>> SchoolSpells={{27,81},{5,6},{51},{1,2,3,4},{5349}};
                for(int32 School=1;School<=5;++School)
                {
                    ClickMenu(false,"SpellSchoolFilter");auto Actual=BookIds();Actual.Sort();
                    TestEqual(TEXT("Changing schools resets book page"),Menu->PageIndex,0);
                    TestEqual(TEXT("Controller cycles to the requested retail school"),Menu->SpellSchool,School);
                    TestTrue(TEXT("Each school includes only its known spells"),Actual==SchoolSpells[School-1]);
                }
                ClickMenu(false,"Clear filters");
                Menu->PageIndex=1;TypeSearch(TEXT("  fLaMe  "));
                TestEqual(TEXT("Native keyboard edits reach the spellbook search delegate"),Menu->Search,FString(TEXT("  fLaMe  ")));
                TestTrue(TEXT("Typing filters case-insensitively and trims surrounding whitespace"),BookIds().Num()==2 && Menu->PageIndex==0);
                ClickMenu(false,"SpellLevelFilter");
                TestTrue(TEXT("Level one intersects the current search"),BookIds()==TArray<int32>{27});
                ClickMenu(false,"SpellLevelFilter");
                TestTrue(TEXT("Level two excludes level one without losing search"),BookIds()==TArray<int32>{81});
                ClickMenu(false,"SpellSchoolFilter");
                TestTrue(TEXT("School, level and search intersect"),BookIds()==TArray<int32>{81});
                TypeSearch(TEXT("no such spell"));
                TestTrue(TEXT("Empty filter result is explicitly explained"),BookIds().IsEmpty() && FindVRMenuControl(Menu->TakeWidget(),"SpellBookEmpty").IsValid());
                ClickMenu(false,"Clear filters");
                TestTrue(TEXT("Clear filters restores all schools, levels and query"),Menu->Search.IsEmpty() && Menu->SpellSchool==0 && Menu->SpellLevel==0 && BookIds().Num()==6);
                TypeSearch(TEXT("Flame"));
                const int32 StableBookRefreshes=Menu->GetRefreshCount();TypeSearch(TEXT("Flame"));
                TestEqual(TEXT("Unchanged book avoids per-frame rebuilds"),Menu->GetRefreshCount(),StableBookRefreshes);
                auto CancelSearch=MakeShared<FACEVRPlatformTextEntry>(Menu->TextEntry,[]{});
                CancelSearch->SetTextFromVirtualKeyboard(FText::FromString(TEXT("discarded query")),ETextEntryType::TextEntryUpdated);PaintMenu();
                TestTrue(TEXT("Native search updates filter before keyboard dismissal"),BookIds().IsEmpty());
                CancelSearch->SetTextFromVirtualKeyboard(FText::FromString(TEXT("discarded query")),ETextEntryType::TextEntryCanceled);PaintMenu();
                TestTrue(TEXT("Canceling native search restores the original query and matches"),Menu->Search==TEXT("Flame") && BookIds().Num()==2);
                auto UpdateKnownSpell=[&](bool Remove)
                {
                    FACEBinaryWriter W;W.WriteUInt16(27);W.WriteUInt16(0);FACEBinaryReader R(W.GetData());
                    if(Remove)NativeSession.HandleMagicRemoveSpell(R);else NativeSession.HandleMagicUpdateSpell(R);
                    PaintMenu();
                };
                UpdateKnownSpell(true);TestTrue(TEXT("Server removal refreshes the open filtered book"),BookIds()==TArray<int32>{81});
                UpdateKnownSpell(false);TestEqual(TEXT("Server learning refreshes the open filtered book"),BookIds().Num(),2);
                TestFalse(TEXT("Learning a spell retains the active native book surface"),VR->bUseDesktopMenu);
                TestEqual(TEXT("The matching learned spell becomes the book selection"),Menu->Spell,27);
                TestEqual(TEXT("Learning preserves the current matching search"),Menu->Search,FString(TEXT("Flame")));
                ClickMenu(false,"Clear filters");
                TestTrue(TEXT("Filter controls remain usable after learning a spell"),Menu->Search.IsEmpty() && BookIds().Num()==6);
                NativeSession.KnownSpells={1,2};NativeSession.SpellBars.SetNum(8);NativeSession.SpellBars[7]={1,0,0};NativeSession.ActiveSpellBar=7;
                Menu->OpenPage("Spellbook");Menu->Spell=2;Menu->bDirty=true;
                NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Add to bar 8");
                {
                    const auto Bytes=ActionPayload(ACEGameAction::AddToSpellBar);
                    if(TestEqual(TEXT("Spellbook emits a complete add-to-bar payload"),Bytes.Num(),12))
                    {
                        FACEBinaryReader NativeReader(Bytes);
                        TestEqual(TEXT("Spellbook adds the inspected spell"),NativeReader.ReadUInt32(),2u);
                        TestEqual(TEXT("Spellbook appends past existing spells, before empty padding"),NativeReader.ReadUInt32(),1u);
                        TestEqual(TEXT("Spellbook targets the displayed hotbar"),NativeReader.ReadUInt32(),7u);
                    }
                }
                TestEqual(TEXT("Appended spell remains after the existing first spell"),NativeSession.SpellBars[7][0],1);
                AimMenuControl("Spell_2_7_1");VR->RouteControllerButton("VRPrevious",true);VR->RouteControllerButton("VRPrevious",false);PaintMenu();
                TestTrue(TEXT("B inspects the pointed spell in the side panel"),Menu->bInspectionOpen && Menu->InspectSpell==2 && Menu->Page=="Spellbook");
                TestFalse(TEXT("Inspection is not a dedicated navigation page"),FindVRMenuControl(Menu->TakeWidget(),"Close inspection").IsValid());
                const auto* BookTarget=Menu->MenuTargets.FindByPredicate([](const auto& Target){return Target.SpellId==2 && Target.Bar==INDEX_NONE;});
                const auto BookSpell=BookTarget?BookTarget->Widget.Pin():nullptr;
                TestTrue(TEXT("Short spellbook pages keep compact icon tiles"),BookSpell && BookSpell->GetCachedGeometry().GetLocalSize().Y<=80);
                if(BookSpell)TestTrue(TEXT("Spellbook entries and hotbar fit in the visible content"),Menu->ContentScroll->GetCachedGeometry().IsUnderLocation(BookSpell->GetCachedGeometry().LocalToAbsolute(BookSpell->GetCachedGeometry().GetLocalSize()*.5)));
                auto DropSpell=[&](int32 Id,int32 Source,int32 TargetBar,int32 Index,bool Remove=false)
                {
                    PaintMenu();const auto* Destination=Menu->SpellDestinations.FindByPredicate([&](const auto& D){return D.Bar==TargetBar && D.Index==Index && D.Remove==Remove;});
                    if(!TestTrue(TEXT("Spell drop destination exists"),Destination!=nullptr))return;
                    const auto G=Destination->Widget.Pin()->GetCachedGeometry();
                    const FVector2D Pixel=Menu->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*.5));
                    Menu->BeginSpellPointer(Id,Source);Menu->bItemDragging=true;NativeSession.CachedC2SPackets.Reset();
                    Menu->FinishItemPointer(true,Pixel,false);
                };
                DropSpell(2,7,7,0);
                TestEqual(TEXT("Drag reorders a hotbar spell without a duplicate"),NativeSession.SpellBars[7][0],2);
                ActionPayload(ACEGameAction::RemoveFromSpellBar);ActionPayload(ACEGameAction::AddToSpellBar);
                DropSpell(2,7,6,MAX_int32);
                TestTrue(TEXT("Dropping onto another tab moves the spell between bars"),!NativeSession.SpellBars[7].Contains(2) && NativeSession.SpellBars[6].Contains(2));
                DropSpell(2,INDEX_NONE,7,1);
                TestTrue(TEXT("Dragging from spellbook copies to the selected bar"),NativeSession.SpellBars[7].Contains(2) && NativeSession.SpellBars[6].Contains(2));
                DropSpell(2,7,INDEX_NONE,0,true);
                TestTrue(TEXT("Dragging back to the book removes only that hotbar entry"),!NativeSession.SpellBars[7].Contains(2) && NativeSession.KnownSpells.Contains(2));
                ActionPayload(ACEGameAction::RemoveFromSpellBar);
                Menu->bInspectionOpen=false;
                const auto SavedVitals=NativeSession.PlayerVitals;
                NativeSession.PlayerVitals.bValid=true;NativeSession.PlayerVitals.AvailableExperience=10000000;
                NativeSession.PlayerVitals.StrengthXpSpent=0;
                Menu->OpenPage("Character");Menu->bSkillsPage=false;
                Menu->RefreshIfDirty();
                TArray<FString> CharacterLabels;
                TFunction<void(const TSharedRef<SWidget>&)> CollectCharacterLabels=[&](const TSharedRef<SWidget>& Widget)
                {
                    if(Widget->GetTypeAsString()==TEXT("STextBlock"))CharacterLabels.Add(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
                    FChildren* Children=Widget->GetChildren();for(int32 I=0;I<Children->Num();++I)CollectCharacterLabels(Children->GetChildAt(I));
                };
                CollectCharacterLabels(Menu->TakeWidget());
                TestTrue(TEXT("Attribute row displays its name without a custom description"),CharacterLabels.Contains(TEXT("Strength")) && !CharacterLabels.ContainsByPredicate([](const FString& Label){return Label.Contains(TEXT("\nAttribute")) || Label.Contains(TEXT("Maximum vital"));}));
                auto* NativeDat=GI->GetSubsystem<UACEDatSubsystem>();
                for(int32 Count:{1,10})
                {
                    int64 ExpectedXP=0;NativeDat->TryGetAttributeXpToNextRank(0,ExpectedXP,nullptr,Count);
                    NativeSession.CachedC2SPackets.Reset();
                    ClickMenu(false,FName(*FString::Printf(TEXT("+%d\n%lld XP"),Count,ExpectedXP)));
                    const auto Bytes=ActionPayload(ACEGameAction::RaiseAttribute);FACEBinaryReader NativeReader(Bytes);
                    if(TestTrue(TEXT("Attribute action contains stat and XP amount"),NativeReader.CanRead(8)))
                    {
                        TestEqual(TEXT("Attribute button raises selected Strength"),NativeReader.ReadUInt32(),1u);
                        TestEqual(TEXT("Attribute button spends the displayed retail XP cost"),NativeReader.ReadUInt32(),uint32(ExpectedXP));
                    }
                }
                Menu->bSkillsPage=true;
                FACESkillInfo Spec;Spec.SkillId=6;Spec.Name=TEXT("Melee Defense");Spec.AdvancementClass=3;
                FACESkillInfo Trained;Trained.SkillId=7;Trained.Name=TEXT("Missile Defense");Trained.AdvancementClass=2;
                FACESkillInfo Untrained;Untrained.SkillId=22;Untrained.Name=TEXT("Jump");Untrained.AdvancementClass=1;
                NativeSession.PlayerVitals.Skills={Untrained,Trained,Spec};Menu->bDirty=true;PaintMenu();
                CharacterLabels.Reset();CollectCharacterLabels(Menu->TakeWidget());
                TestTrue(TEXT("Skill rows show names without repeating their advancement description"),CharacterLabels.Contains(TEXT("Melee Defense")) && CharacterLabels.Contains(TEXT("Jump"))
                    && !CharacterLabels.ContainsByPredicate([](const FString& Label){return Label.Contains(TEXT("\nSpecialized")) || Label.Contains(TEXT("\nTrained")) || Label.Contains(TEXT("\nUntrained"));}));
                const auto SpecHeading=FindVRMenuControl(Menu->TakeWidget(),"Specialized");
                const auto TrainedHeading=FindVRMenuControl(Menu->TakeWidget(),"Trained");
                TestTrue(TEXT("Skills have retail advancement headings"),SpecHeading.IsValid() && TrainedHeading.IsValid());
                if(SpecHeading && TrainedHeading)TestTrue(TEXT("Specialized section precedes trained section"),SpecHeading->GetCachedGeometry().GetAbsolutePosition().Y<TrainedHeading->GetCachedGeometry().GetAbsolutePosition().Y);
                NativeSession.PlayerVitals=SavedVitals;

                // Native loot serves both VR and touch: chest side-slot packs
                // must be selectable, browsable and removable just like items.
                {
                    FACEWorldObject Chest;Chest.Guid=99370;Chest.ItemType=ACEItemType::Container;Chest.ContainersCapacity=2;Chest.ItemsCapacity=24;
                    FACEWorldObject Bag=Chest;Bag.Guid=99371;Bag.ContainerId=Chest.Guid;Bag.IconId=0x06004CF7;
                    FACEWorldObject Contents=Item;Contents.Guid=99372;Contents.ContainerId=Bag.Guid;
                    const int32 PreviousContainer=NativeSession.OpenExternalContainerGuid;
                    for(const auto& Object:{Chest,Bag,Contents})NativeSession.UpsertWorldObject(Object);
                    NativeSession.ContainerContents.Add(Chest.Guid,{{Bag.Guid,1}});
                    NativeSession.ContainerContents.Add(Bag.Guid,{{Contents.Guid,0}});
                    NativeSession.OpenExternalContainerGuid=Chest.Guid;
                    PC->DatGameplayBinder->OpenLootSelectedPackGuid=Chest.Guid;
                    Menu->bInspectionOpen=false;Menu->OpenPage("Loot");PaintMenu();
                    ClickMenu(false,"Item_99371");
                    TestEqual(TEXT("Native loot selects a pack in a chest"),Menu->Selected,Bag.Guid);
                    ClickMenu(false,"Open selected pack");
                    TestTrue(TEXT("Native loot opens the chest pack's contents"),FindVRMenuControl(Menu->TakeWidget(),"Item_99372").IsValid());
                    ClickMenu(false,"Back to container");ClickMenu(false,"Item_99371");
                    NativeSession.CachedC2SPackets.Reset();ClickMenu(false,"Take selected");
                    const auto Bytes=ActionPayload(ACEGameAction::PutItemInContainer);
                    if(Bytes.Num()>=12)
                    {
                        FACEBinaryReader Reader(Bytes);
                        TestEqual(TEXT("Native take moves the entire chest pack"),Reader.ReadUInt32(),uint32(Bag.Guid));
                        TestEqual(TEXT("Native take puts a chest pack in the player's side slots"),Reader.ReadUInt32(),uint32(NativeSession.PlayerGuid));
                    }
                    TestEqual(TEXT("Native take awaits server confirmation"),NativeSession.WorldObjects[Bag.Guid].ContainerId,Chest.Guid);
                    PC->DatGameplayBinder->OpenLootSelectedPackGuid=Bag.Guid;
                    FACEBinaryWriter Ack;Ack.WriteUInt32(Bag.Guid);Ack.WriteUInt32(NativeSession.PlayerGuid);Ack.WriteInt32(0);Ack.WriteInt32(1);
                    FACEBinaryReader Reader(Ack.GetData());NativeSession.HandleInventoryPutObjInContainer(Reader);
                    Menu->bDirty=true;PaintMenu();
                    TestEqual(TEXT("Native loot returns to root after pack removal"),PC->DatGameplayBinder->OpenLootSelectedPackGuid,Chest.Guid);
                    TestFalse(TEXT("Native chest no longer lists the removed pack"),FindVRMenuControl(Menu->TakeWidget(),"Item_99371").IsValid());
                    TestEqual(TEXT("Native pack removal keeps nested contents"),NativeSession.WorldObjects[Contents.Guid].ContainerId,Bag.Guid);
                    NativeSession.OpenExternalContainerGuid=PreviousContainer;
                    PC->DatGameplayBinder->OpenLootSelectedPackGuid=0;
                    for(int32 Guid:{Chest.Guid,Bag.Guid,Contents.Guid})NativeSession.WorldObjects.Remove(Guid);
                    NativeSession.ContainerContents.Remove(Chest.Guid);NativeSession.ContainerContents.Remove(Bag.Guid);
                    if(auto* Owned=NativeSession.ContainerContents.Find(NativeSession.PlayerGuid))Owned->RemoveAll([&](const auto& Entry){return Entry.ItemGuid==Bag.Guid;});
                }
                Menu->OpenPage("Inventory");Menu->SelectItem(Item.Guid);
                NativeSession.SocketC2S->Close();NativeSockets->DestroySocket(NativeSession.SocketC2S);NativeSession.SocketC2S=nullptr;
            }
            else AddError(TEXT("Native menu loopback socket could not bind"));
            if(NativeReceiver)NativeSockets->DestroySocket(NativeReceiver);
            // Placement uses the visible native surface while retaining the common saved transform.
            VR->Settings->bMenuLocked=false;
            auto PointNative=[&](FVector2D Pixel)
            {
                const FVector2D Size=VR->GameplayMenuPanel->GetDrawSize();
                const FVector Point=VR->GameplayMenuPanel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
                const FVector From=Point+VR->GameplayMenuPanel->GetForwardVector()*90;
                VR->RightAim->SetWorldLocationAndRotation(From,(Point-From).Rotation());VR->RightPointer->TickComponent(.016f,LEVELTICK_All,nullptr);
            };
            PointNative(FVector2D(1000,800));VR->BeginPanelEdit("Menu",false,false);
            TestEqual(TEXT("Hidden classic canvas does not block native menu movement"),VR->EditingPanel,FName("Menu"));
            const FVector BeforeMove=VR->RetailPanel->GetComponentLocation();VR->RightAim->AddWorldOffset(FVector(0,12,0));VR->UpdatePanelEdit(.016f);VR->UpdateGameplayMenu(true,.016f);
            TestTrue(TEXT("Native menu follows controller translation"),FVector::Dist(VR->GameplayMenuPanel->GetComponentLocation(),BeforeMove)>10);
            VR->EndPanelEdit(false);
            PointNative(FVector2D(1000,800));VR->BeginPanelEdit("Menu",false,true);
            TestTrue(TEXT("Native menu can resize"),VR->EditingPanel=="Menu" && VR->bPanelResize);
            VR->EndPanelEdit(false);VR->Settings->bMenuLocked=true;
            // Use-on-item remains armed across equipment/pack navigation.
            Menu->UseSource=Item.Guid;Menu->OpenPage("Equipment");
            TestEqual(TEXT("Use-on-item source survives equipment tab selection"),Menu->UseSource,Item.Guid);
            Menu->OpenPage("Character");TestEqual(TEXT("Leaving item pages cancels use-on-item"),Menu->UseSource,0);
            Menu->OpenPage("Inventory");Menu->SelectItem(Item.Guid);
            MenuControllers.Connected=false;PollMenuHands();FlushRenderingCommands();PC->DatGameplayBinder->SelectedStackAmount=3;
		}
		TestEqual(TEXT("Native quantity uses the retail stack-selection model"),PC->DatGameplayBinder->GetSelectedItemAmount(Item.Guid),3);
		const auto Count=VR->Client->Session->NextGameActionSequence;
		Menu->Execute("Drop");TestEqual(TEXT("Native drop requires confirmation"),VR->Client->Session->NextGameActionSequence,Count);
		Menu->Execute("Drop");TestTrue(TEXT("Native drop confirms selected quantity through inventory actions"),VR->Client->Session->NextGameActionSequence>Count);
		Menu->OpenPage("Spellbook");Menu->RefreshIfDirty();
		TestNotNull(TEXT("Native search provides a platform keyboard entry"),Menu->TextEntry.Get());
		VR->FocusTextEntry(Menu->TextEntry);TestTrue(TEXT("Native search activates the existing VR keyboard"),VR->bTextKeyboardOpen);
		Menu->OpenPage("Inventory");TestFalse(TEXT("Changing native page dismisses its keyboard"),VR->bTextKeyboardOpen);
		if(auto* Source=Pawn->FindComponentByClass<UACECharacterAppearanceComponent>();Source && Source->HasAppearance())
		{
			auto& Record=VR->Client->Session->WorldObjects.FindOrAdd(VR->Client->GetPlayerGuid());Record.SetupId=Source->GetSetupId();Record.bIsPlayer=true;
			VR->UpdateGameplayMenu(true,.016f);
			TestTrue(TEXT("Inventory mirror builds a live 3D character"),VR->MenuMirrorAppearance && VR->MenuMirrorAppearance->HasAppearance());
			if(VR->MenuMirrorAppearance && VR->MenuMirrorAppearance->HasAppearance())
			{
				FTransform Pose;Source->GetPartCurrentTransform(16,Pose);const FTransform Original=Pose;
				for(float Yaw:{-40.f,0.f,40.f})
				{
					Pose=Original;Pose.AddToTranslation(FVector(3,5,2));Pose.SetRotation(FRotator(25,Yaw,10).Quaternion()*Original.GetRotation());Source->GetPartMesh(16)->SetRelativeTransform(Pose);
					VR->UpdateGameplayMenu(true,.016f);
					const FTransform SourceFrame(VR->MenuMirrorActor->GetActorQuat(),Source->GetMeshRoot()->GetComponentLocation());
					for(FVector Vertex:{FVector::ZeroVector,FVector(0,-8,17),FVector(10,0,0)})
					{
						FVector Expected=SourceFrame.InverseTransformPosition(Source->GetPartMesh(16)->GetComponentTransform().TransformPosition(Vertex));Expected.X=-Expected.X;
						Expected=VR->MenuMirrorActor->GetActorTransform().TransformPosition(Expected);
						TestTrue(TEXT("Tracked face reflects correctly through the full component hierarchy"),VR->MenuMirrorAppearance->GetPartMesh(16)->GetComponentTransform().TransformPosition(Vertex).Equals(Expected,.01));
					}
				}
				Source->GetPartMesh(16)->SetRelativeTransform(Original);
                const FVector LookAt=VR->MenuMirrorActor->GetActorLocation()+FVector(0,0,75);
                VR->Head->SetWorldRotation((LookAt-VR->Head->GetComponentLocation()).Rotation());
                VR->UpdateArms(.016f);VR->UpdateGameplayMenu(true,.016f);
                const FVector ReflectedFace=VR->MenuMirrorAppearance->GetPartMesh(16)->GetComponentTransform().TransformVector(FVector(0,-1,0)).GetSafeNormal2D();
                const FVector ToViewer=(VR->Head->GetComponentLocation()-VR->MenuMirrorActor->GetActorLocation()).GetSafeNormal2D();
                TestTrue(TEXT("Looking at the off-center mirror makes its face look back at the viewer"),FVector::DotProduct(ReflectedFace,ToViewer)>.97);

				if(FApp::CanEverRender())
				{
					VR->UpdateGameplayMenu(true,.016f);VR->GameplayMenuPanel->SetRedrawTime(0);
					for(int I=0;I<3;++I){VR->GameplayMenuPanel->RequestRedraw();VR->GameplayMenuPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();}
					TestNotNull(TEXT("Native panel has a rendered world-space texture"),VR->GameplayMenuPanel->GetRenderTarget());
					// This fixture runs within one frame; publish the widget's new
					// render-target scene proxy before taking the combined capture.
					if(GShaderCompilingManager)GShaderCompilingManager->FinishAllCompilation();
					VR->GameplayMenuPanel->MarkRenderStateDirty();
					World->SendAllEndOfFrameUpdates();FlushRenderingCommands();
					if(auto* Surface=VR->GameplayMenuPanel->GetRenderTarget())
					{
						TArray<FColor> UI;Surface->GameThread_GetRenderTargetResource()->ReadPixels(UI);
						TestTrue(TEXT("Native world panel draws visible content"),UI.ContainsByPredicate([](FColor C){return C.A>128 && (C.R>64 || C.G>64 || C.B>64);}));
						TArray64<uint8> SurfacePNG;FImageUtils::PNGCompressImageArray(Surface->SizeX,Surface->SizeY,UI,SurfacePNG);
						FFileHelper::SaveArrayToFile(SurfacePNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/VR/GameplaySurface.png")));
					}
					auto* CaptureActor=World->SpawnActor<AActor>();auto* Capture=NewObject<USceneCaptureComponent2D>(CaptureActor);CaptureActor->SetRootComponent(Capture);Capture->RegisterComponent();
					Capture->bCaptureEveryFrame=false;Capture->bCaptureOnMovement=false;Capture->PrimitiveRenderMode=ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
					Capture->ShowOnlyActorComponents(VR->MenuMirrorActor);Capture->ShowOnlyComponent(VR->GameplayMenuPanel);
					Capture->ShowFlags.SetLighting(false);Capture->ShowFlags.SetAtmosphere(false);Capture->ShowFlags.SetEyeAdaptation(false);Capture->ShowFlags.SetWidgetComponents(true);Capture->CaptureSource=SCS_FinalColorLDR;
					const auto Frame=VR->GameplayMenuPanel->GetComponentTransform();const FVector Aim=Frame.GetLocation()+Frame.GetRotation().RotateVector(FVector(0,22,0));
					Capture->SetWorldLocationAndRotation(VR->Head->GetComponentLocation(),(LookAt-VR->Head->GetComponentLocation()).Rotation());Capture->FOVAngle=85;
					auto* Target=NewObject<UTextureRenderTarget2D>();Target->RenderTargetFormat=RTF_RGBA8_SRGB;Target->InitAutoFormat(1600,1000);Capture->TextureTarget=Target;Capture->CaptureScene();FlushRenderingCommands();
					TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> PNG;FImageUtils::PNGCompressImageArray(1600,1000,Pixels,PNG);
					FFileHelper::SaveArrayToFile(PNG,*(FPaths::ProjectSavedDir()/TEXT("Automation/VR/GameplayMirror.png")));CaptureActor->Destroy();
				}
			}
		}
		if(FApp::CanEverRender())
		{
			FWidgetRenderer Renderer(true,true);const FString Directory=FPaths::ProjectSavedDir()/TEXT("Automation/VR");
			for(const TCHAR* Page:{TEXT("Inventory"),TEXT("Equipment"),TEXT("Spellbook"),TEXT("Character"),TEXT("Skills"),TEXT("Fellowship"),TEXT("Vendor"),TEXT("Salvage")})
			{
				const bool SkillsPage=FCString::Strcmp(Page,TEXT("Skills"))==0;
				Menu->OpenPage(SkillsPage?TEXT("Character"):Page);
                auto& VisualSession=*VR->Client->Session;
                if(Menu->GetPage()=="Inventory")
                {
                    for(int32 Index=0;Index<35;++Index){auto Visual=Item;Visual.Guid=99500+Index;Visual.Name=FString::Printf(TEXT("Inventory item %d"),Index+1);Visual.PlacementPosition=Index+2;Visual.StackSize=Index%3==0?1:Index+2;VisualSession.WorldObjects.Add(Visual.Guid,Visual);}
                    Menu->SelectItem(Item.Guid);PC->DatGameplayBinder->SelectedStackAmount=3;
                }
                if(Menu->GetPage()=="Character"){Menu->bSkillsPage=SkillsPage;VisualSession.PlayerVitals.bValid=true;VisualSession.PlayerVitals.Level=150;VisualSession.PlayerVitals.AvailableExperience=1000000;VisualSession.PlayerVitals.AvailableSkillCredits=12;}
                if(SkillsPage)
                {
                    FACESkillInfo Spec;Spec.SkillId=6;Spec.Name=TEXT("Melee Defense");Spec.AdvancementClass=3;Spec.Current=325;
                    FACESkillInfo Trained;Trained.SkillId=7;Trained.Name=TEXT("Missile Defense");Trained.AdvancementClass=2;Trained.Current=250;
                    FACESkillInfo Untrained;Untrained.SkillId=22;Untrained.Name=TEXT("Jump");Untrained.AdvancementClass=1;Untrained.Current=100;
                    VisualSession.PlayerVitals.Skills={Spec,Trained,Untrained};
                }
                if(Menu->GetPage()=="Vendor")
                {
                    VisualSession.OpenVendorGuid=500;PC->DatGameplayBinder->OpenVendorGuid=500;Menu->bVendorSelling=true;
                    PC->DatGameplayBinder->VendorSellCart={{10,Item.Guid}};Menu->SelectItem(Item.Guid);
                }
                if(Menu->GetPage()=="Salvage")
                {
                    auto Tool=Item;Tool.Guid=99220;Tool.ItemType=0x20000000;VisualSession.WorldObjects.Add(Tool.Guid,Tool);
                    PC->DatGameplayBinder->ShowSalvagePanel(Tool.Guid);
                    PC->DatGameplayBinder->AddItemToSalvageQueue(Item.Guid);Menu->SelectItem(Item.Guid);
                }
                Menu->bDirty=true;Menu->RefreshIfDirty();const FVector2D Size(1200,960);
				auto* Target=FWidgetRenderer::CreateTargetFor(Size,TF_Bilinear,true);
				for(int I=0;I<3;++I){Renderer.DrawWidget(Target,Menu->TakeWidget(),Size,0);FlushRenderingCommands();}
				TArray<FColor> Pixels;Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);TArray64<uint8> PNG;
				FImageUtils::PNGCompressImageArray(1200,960,Pixels,PNG);FFileHelper::SaveArrayToFile(PNG,*(Directory/FString::Printf(TEXT("Gameplay%s.png"),Page)));
			}
            Menu->OpenPage("Spellbook");Menu->Spell=2;Menu->InspectSelection();Menu->RefreshIfDirty();
            const FVector2D InspectSize(460,960);auto* InspectionTarget=FWidgetRenderer::CreateTargetFor(InspectSize,TF_Bilinear,true);
            for(int32 Pass=0;Pass<3;++Pass){Renderer.DrawWidget(InspectionTarget,Menu->GetInspectionWidget(),InspectSize,0);FlushRenderingCommands();}
            const auto SpellIcon=FindVRMenuControl(Menu->GetInspectionWidget(),TEXT("SpellInspectionIcon"));
            TestTrue(TEXT("Spell inspection keeps the retail icon square instead of stretching across the row"),SpellIcon && SpellIcon->GetCachedGeometry().GetLocalSize().Equals(FVector2D(64,64),.01f));
            TArray<FColor> InspectionPixels;InspectionTarget->GameThread_GetRenderTargetResource()->ReadPixels(InspectionPixels);TArray64<uint8> InspectionPNG;
            FImageUtils::PNGCompressImageArray(460,960,InspectionPixels,InspectionPNG);FFileHelper::SaveArrayToFile(InspectionPNG,*(Directory/TEXT("GameplaySpellInspection.png")));

            Menu->InspectSpell=0;Menu->InspectItem=Item.Guid;Menu->bInspectionOpen=true;
            FACEAppraisalInfo Appraisal;Appraisal.ObjectGuid=Item.Guid;Appraisal.Name=TEXT("Appraisal color regression sword");Appraisal.bSuccess=true;
            Appraisal.bHasValue=Appraisal.bHasBurden=Appraisal.bHasWeaponProfile=true;Appraisal.Value=2345;Appraisal.Burden=300;
            Appraisal.ItemType=ACEItemType::MeleeWeapon;Appraisal.Damage=50;Appraisal.DamageVariance=.2f;Appraisal.DamageType=1;Appraisal.WeaponTime=40;
            Appraisal.WeaponEnchantments=8u|(8u<<16)|4u;Appraisal.Inscription=TEXT("An inscription retains its own paragraph.");
            Appraisal.StringProperties.Add(8,TEXT("A Scribe"));
            Menu->InspectionAppraisal=Appraisal;Menu->BuildInspection();
            const FString ExpectedBody=ACEAppraisalFormatting::ItemExaminationText(Appraisal,GI->GetSubsystem<UACEDatSubsystem>(),false,false);
            UACERetailTextBlock* ActualBody=nullptr;
            for(TObjectIterator<UACERetailTextBlock> It;It;++It)if(It->GetText().ToString()==ExpectedBody){ActualBody=*It;break;}
            if(TestNotNull(TEXT("VR inspection uses the same complete retail body and glyph renderer"),ActualBody))
            {
                const auto Colors=ACEAppraisalFormatting::ItemTextColors(Appraisal,ExpectedBody);
                for(const TCHAR* Stat:{TEXT("Damage:"),TEXT("Speed:")})
                {
                    const int32 Offset=ExpectedBody.Find(Stat);
                    TestTrue(TEXT("Every enchanted line uses its character-offset color after preceding text"),Offset!=INDEX_NONE && Colors.IsValidIndex(Offset)
                        && ActualBody->GetGlyphColor(Offset,FLinearColor::White)==Colors[Offset]);
                }
            }
            for(int32 Pass=0;Pass<3;++Pass){Renderer.DrawWidget(InspectionTarget,Menu->GetInspectionWidget(),InspectSize,0);FlushRenderingCommands();}
            InspectionPixels.Reset();InspectionPNG.Reset();InspectionTarget->GameThread_GetRenderTargetResource()->ReadPixels(InspectionPixels);
            FImageUtils::PNGCompressImageArray(460,960,InspectionPixels,InspectionPNG);FFileHelper::SaveArrayToFile(InspectionPNG,*(Directory/TEXT("GameplayItemInspection.png")));

            Appraisal.bIsCreature=true;Appraisal.Name=TEXT("Creature appraisal");Appraisal.Level=180;
            Appraisal.Strength=300;Appraisal.Endurance=280;Appraisal.Coordination=200;Appraisal.Quickness=210;Appraisal.Focus=190;Appraisal.Self=195;
            Appraisal.Health=120;Appraisal.MaxHealth=200;Appraisal.Stamina=180;Appraisal.MaxStamina=250;Appraisal.Mana=90;Appraisal.MaxMana=150;
            Appraisal.AttributeHighlights=1|8;Appraisal.AttributeColors=1;
            Menu->InspectionAppraisal=Appraisal;Menu->BuildInspection();
            for(int32 Pass=0;Pass<3;++Pass){Renderer.DrawWidget(InspectionTarget,Menu->GetInspectionWidget(),InspectSize,0);FlushRenderingCommands();}
            InspectionPixels.Reset();InspectionPNG.Reset();InspectionTarget->GameThread_GetRenderTargetResource()->ReadPixels(InspectionPixels);
            FImageUtils::PNGCompressImageArray(460,960,InspectionPixels,InspectionPNG);FFileHelper::SaveArrayToFile(InspectionPNG,*(Directory/TEXT("GameplayCreatureInspection.png")));

            auto Jump=SNew(SACEVRJumpCharge);const FVector2D JumpSize=Jump->ComputeDesiredSize(1);
            auto* JumpTarget=FWidgetRenderer::CreateTargetFor(JumpSize,TF_Bilinear,true);
            int32 PreviousFill=-1;
            for(float Charge:{0.f,.5f,1.f})
            {
                Jump->Refresh(Charge);Renderer.DrawWidget(JumpTarget,Jump,JumpSize,0);FlushRenderingCommands();
                TArray<FColor> JumpPixels;JumpTarget->GameThread_GetRenderTargetResource()->ReadPixels(JumpPixels);
                // Count only the meter interior, excluding labels and the gold
                // frame. Relative channel contrast survives the render target's
                // gamma conversion without assuming raw linear color bytes.
                int32 Fill=0;
                for(int32 Y=45;Y<71;++Y)for(int32 X=13;X<467;++X)
                {
                    const FColor C=JumpPixels[Y*480+X];
                    if(C.A>128 && int32(C.R)>int32(C.B)+50 && int32(C.G)>int32(C.B)+30)++Fill;
                }
                TestTrue(TEXT("Native jump meter visibly fills as charge increases"),Fill>PreviousFill);PreviousFill=Fill;
                TArray64<uint8> JumpPNG;FImageUtils::PNGCompressImageArray(480,90,JumpPixels,JumpPNG);
                FFileHelper::SaveArrayToFile(JumpPNG,*(Directory/FString::Printf(TEXT("JumpCharge%d.png"),FMath::RoundToInt(Charge*100))));
            }
		}
		VR->bInventoryOpen=false;VR->UpdatePanels();
		TestFalse(TEXT("Hidden native menu stops rendering and hit testing"),VR->GameplayMenuPanel->IsComponentTickEnabled() || VR->GameplayMenuPanel->IsVisible() || VR->GameplayMenuPanel->GetCollisionEnabled()!=ECollisionEnabled::NoCollision);
		TestTrue(TEXT("Closing inventory hides the 3D mirror"),!VR->MenuMirrorActor || VR->MenuMirrorActor->IsHidden());
		VR->OpenRetailPanel(NAME_None);TestTrue(TEXT("Server confirmation dialogs retain a visible input surface"),VR->bUseDesktopMenu && VR->RetailPanel->IsVisible());
		VR->bInventoryOpen=false;VR->bUseDesktopMenu=false;VR->UpdatePanels();
		PC->DatGameplayBinder->OpenGameplayPanel(TEXT("MiniGamePanel_Field"));
		TestTrue(TEXT("Chess automatically reveals the playable retail board in VR"),VR->bInventoryOpen && VR->bUseDesktopMenu && VR->RetailPanel->IsVisible());
	}
	if (FApp::CanEverRender())
	{
		FWidgetRenderer Renderer(true, true);
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("Automation/VR"); IFileManager::Get().MakeDirectory(*Directory, true);
		for (int32 I = 0; I < 7; ++I)
		{
			if(I>=3)VR->MenuWidget->SettingsCategory=I-2;
			auto* Widget = (I == 0 || I>=3) ? VR->MenuWidget.Get() : I == 1 ? VR->WristWidget.Get() : VR->KeyboardWidget.Get();
			const FIntPoint Size = (I == 0 || I>=3) ? FIntPoint(720, 1000) : I == 1 ? FIntPoint(480, 800) : FIntPoint(1200, 400);
			auto* Target = FWidgetRenderer::CreateTargetFor(FVector2D(Size), TF_Bilinear, true);
			for (int32 Pass = 0; Pass < 3; ++Pass) { Renderer.DrawWidget(Target, Widget->TakeWidget(), FVector2D(Size), 0); FlushRenderingCommands(); }
			TArray<FColor> Pixels; Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
			TestEqual(TEXT("VR menu render has expected size"), Pixels.Num(), Size.X * Size.Y);
			TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Pixels, PNG);
			FFileHelper::SaveArrayToFile(PNG, *(Directory / FString::Printf(TEXT("Menu%d.png"), I)));
		}
	}
	VR->Client->Session->State = EACESessionState::Disconnected;
	Login->NativeDestruct(); IFileManager::Get().Delete(*Login->ProfilePath,false,true);
	Pawn->Destroy(); PC->Destroy(); GI->Shutdown(); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FACEVRMenuRecenterTest, "ACE.VR.MenuRecenter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FACEVRMenuRecenterTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UACEVRSettings> SavedPreferences(NewObject<UACEVRSettings>());
	ON_SCOPE_EXIT { SavedPreferences->Persist(); };
	const auto Values = UWorld::InitializationValues().AllowAudioPlayback(false).RequiresHitProxies(false)
		.CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
	auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
	auto* GI = NewObject<UGameInstance>(GEngine); GI->InitializeStandalone();
	auto* DummyWorld = GI->GetWorld();
	GI->GetWorldContext()->SetCurrentWorld(World); World->SetGameInstance(GI); DummyWorld->DestroyWorld(false);
	auto* PC = World->SpawnActor<AACEPlayerController>(); auto* Pawn = World->SpawnActor<APawn>();
	PC->SetPlayer(NewObject<ULocalPlayer>(GEngine));
	World->AddController(PC);
	auto* Capsule = NewObject<UCapsuleComponent>(Pawn); Capsule->InitCapsuleSize(34, 96); Pawn->SetRootComponent(Capsule); Capsule->RegisterComponent();
	auto* Camera = NewObject<UCameraComponent>(Pawn); Camera->SetupAttachment(Capsule); Camera->RegisterComponent();
	PC->Possess(Pawn);
	auto* VR = NewObject<UACEVRComponent>(Pawn); Pawn->AddInstanceComponent(VR); VR->RegisterComponent();
	VR->PC = PC; VR->Client = GI->GetSubsystem<UACEClientSubsystem>();
	VR->Settings = NewObject<UACEVRSettings>(); VR->ActivateRig();
	VR->Client->GetSession()->State=EACESessionState::InWorld;
	VR->Settings->MenuAnchorMode=1;
	VR->SettingsPanel->SetRedrawTime(0);
	FVRTestControllers Controllers;
	for(auto* Hand:{VR->LeftGrip.Get(),VR->RightGrip.Get(),VR->LeftAim.Get(),VR->RightAim.Get()})
		Hand->TickComponent(.016f,LEVELTICK_All,nullptr);
	VR->Head->SetRelativeLocationAndRotation(FVector(0,0,165),FRotator::ZeroRotator);
	VR->UpdateTrackingState(true);VR->bInventoryOpen=true;VR->bUseDesktopMenu=true;VR->UpdatePanels();
	{
		TGuardValue<FVector> MenuOffset(VR->Settings->MenuViewOffset,FVector(90,0,-10));
		TGuardValue<FRotator> MenuRotation(VR->Settings->MenuViewRotation,FRotator::ZeroRotator);
		TGuardValue<float> MenuScale(VR->Settings->PanelScale,.11f);
		TGuardValue<float> MenuRadius(VR->Settings->MenuRecenterDegrees,55.f);
		TGuardValue<bool> RetailMode(VR->bUseDesktopMenu,true);
		for (float Side : {-1.f,1.f})
		{
			VR->Head->SetWorldRotation(FRotator(0,170,0));VR->bMenuAnchorReady=false;VR->UpdateMenuAnchor(0);
			const FTransform ReadingFrame=VR->MenuAnchorFrame;
			VR->Head->SetWorldRotation(FRotator(25,170+Side*50,0));
			for(int32 Frame=0;Frame<180;++Frame)VR->UpdateMenuAnchor(1.f/90);
			TestTrue(TEXT("Inventory stays still through a sustained 50 degree edge scan"),VR->MenuAnchorFrame.Equals(ReadingFrame,.001));
			VR->Head->SetWorldRotation(FRotator(0,170+Side*80,0));
			float Previous=0;
			for(int32 Frame=0;Frame<180;++Frame)
			{
				VR->UpdateMenuAnchor(1.f/90);
				const float Progress=Side*FMath::FindDeltaAngleDegrees(170,VR->MenuAnchorFrame.Rotator().Yaw);
				TestTrue(TEXT("A deliberate turn recenters inventory smoothly across the yaw seam"),Progress>=Previous-.001f && Progress-Previous<1.5f);
				Previous=Progress;
			}
			TestTrue(TEXT("Inventory still follows a deliberate turn beyond its reading radius"),Previous>30.f && Previous<37.f);
			TestFalse(TEXT("Inventory settles without hunting at the follow boundary"),VR->bMenuAnchorTurning);
		}
		// A compact panel permits the full slider range, including a smaller radius.
		VR->Settings->PanelScale=.05f;VR->Settings->MenuViewOffset=FVector(180,0,-10);
		VR->Head->SetWorldRotation(FRotator::ZeroRotator);VR->bMenuAnchorReady=false;VR->UpdateMenuAnchor(0);
		VR->SetSliderSetting("MenuRecenter",1.f);
		TestEqual(TEXT("Slider reaches the maximum inventory reading radius"),VR->Settings->MenuRecenterDegrees,120.f);
		const FTransform TunedFrame=VR->MenuAnchorFrame;
		VR->Head->SetWorldRotation(FRotator(0,80,0));
		for(int32 Frame=0;Frame<90;++Frame)VR->UpdateMenuAnchor(1.f/90);
		TestTrue(TEXT("Increasing the radius immediately allows a wider head scan"),VR->MenuAnchorFrame.Equals(TunedFrame,.001));
		VR->SetSliderSetting("MenuRecenter",0.f);
		TestEqual(TEXT("Slider reaches the minimum inventory reading radius"),VR->Settings->MenuRecenterDegrees,30.f);
		VR->UpdateMenuAnchor(1.f/90);
		TestTrue(TEXT("Reducing the radius starts follow without reopening inventory"),VR->bMenuAnchorTurning && VR->MenuAnchorFrame.Rotator().Yaw>0);
		const FTransform MovingFrame=VR->MenuAnchorFrame;
		VR->SetSliderSetting("MenuRecenter",1.f);
		for(int32 Frame=0;Frame<90;++Frame)VR->UpdateMenuAnchor(1.f/90);
		TestTrue(TEXT("Increasing the radius stops active follow without resetting its anchor"),!VR->bMenuAnchorTurning && VR->MenuAnchorFrame.Equals(MovingFrame,.001));
		TestTrue(TEXT("Radius tuning preserves panel placement and scale"),VR->Settings->MenuViewOffset.Equals(FVector(180,0,-10)) && VR->Settings->MenuViewRotation.IsZero() && VR->Settings->PanelScale==.05f);
		VR->SetSliderSetting("MenuRecenter",0.f);
		VR->Settings->PanelScale=.15f;VR->Settings->MenuViewOffset=FVector(70,75,-10);
		VR->Settings->MenuViewRotation=FRotator(10,15,5);
		VR->Head->SetWorldRotation(FRotator::ZeroRotator);VR->bMenuAnchorReady=false;VR->UpdateMenuAnchor(0);
		const FTransform WideFrame=VR->MenuAnchorFrame;
		// Read the actual panel corners, rather than assume the stock centered size.
		VR->PositionPanel(VR->RetailPanel);VR->RetailPanel->SetWorldScale3D(FVector(.15f));
		for(float Side:{-1.f,1.f})for(float Up:{-1.f,1.f})
		{
			const auto Size=VR->RetailPanel->GetDrawSize();
			const FVector Corner=VR->RetailPanel->GetComponentTransform().TransformPosition(FVector(0,Side*Size.X*.5,Up*Size.Y*.5));
			VR->Head->SetWorldRotation((Corner-VR->Head->GetComponentLocation()).Rotation());
			for(int32 Frame=0;Frame<90;++Frame)VR->UpdateMenuAnchor(1.f/90);
			TestTrue(TEXT("Resizing and offsetting inventory retains a stable view of every corner"),VR->MenuAnchorFrame.Equals(WideFrame,.001));
		}
	}
	VR->Head->SetRelativeRotation(FRotator::ZeroRotator);VR->bMenuAnchorReady=false;VR->UpdatePanels();
	if(FApp::CanEverRender())
	{
		TGuardValue<float> Radius(VR->Settings->MenuRecenterDegrees,55.f);
		TGuardValue<int32> AnchorMode(VR->Settings->MenuAnchorMode,1);
		TGuardValue<bool> OptionsOpen(VR->bSettingsOpen,true);
		auto* Options=Cast<UACEVRWidget>(VR->SettingsPanel->GetUserWidgetObject());
		if(TestNotNull(TEXT("VR settings widget is available in the headset"),Options))
		{
			TGuardValue<int32> Category(Options->SettingsCategory,1);
			VR->UpdatePanels();VR->FocusPanelSurface(VR->SettingsPanel);
			auto PaintOptions=[&]()
			{for(int32 Pass=0;Pass<3;++Pass){VR->SettingsPanel->RequestRedraw();VR->SettingsPanel->TickComponent(.016f,LEVELTICK_All,nullptr);FlushRenderingCommands();}};
			PaintOptions(); // The switcher's active children update during the Slate prepass.
			const auto Slider=FindVRMenuControl(Options->TakeWidget(),"MenuRecenter");
			const auto Panels=FindVRMenuControl(Options->TakeWidget(),"Panels");
			if(TestTrue(TEXT("Panels settings expose the recenter slider"),Slider.IsValid() && Panels.IsValid()))
			{
				StaticCastSharedPtr<SScrollBox>(Panels)->ScrollDescendantIntoView(Slider,false,EDescendantScrollDestination::Center);PaintOptions();
				for(bool Left:{false,true})
				{
					auto AimAtSlider=[&](float Fraction)
					{
						const FGeometry G=Slider->GetCachedGeometry();
						const FVector2D Pixel=Options->GetCachedGeometry().AbsoluteToLocal(G.LocalToAbsolute(G.GetLocalSize()*FVector2D(Fraction,.5)));
						const auto Size=VR->SettingsPanel->GetDrawSize();
						const FVector Point=VR->SettingsPanel->GetComponentTransform().TransformPosition(FVector(0,Size.X*.5-Pixel.X,Size.Y*.5-Pixel.Y));
						const FVector From=Point+VR->SettingsPanel->GetForwardVector()*35;
						(Left?VR->LeftAim:VR->RightAim)->SetWorldLocationAndRotation(From,(Point-From).Rotation());
						auto* Pointer=Left?VR->LeftPointer.Get():VR->RightPointer.Get();Pointer->TickComponent(.016f,LEVELTICK_All,nullptr);
						TestEqual(TEXT("Controller reaches the settings slider"),Pointer->GetHoveredWidgetComponent(),VR->SettingsPanel.Get());
					};
					AimAtSlider(.3f);VR->Trigger(Left,true);PaintOptions();
					const float PressedRadius=VR->Settings->MenuRecenterDegrees;
					AimAtSlider(.5f);PaintOptions();
					AimAtSlider(.75f);PaintOptions();
					TestTrue(FString::Printf(TEXT("Dragging the %s controller updates the radius before release (pressed %.2f, dragged %.2f)"),Left?TEXT("left"):TEXT("right"),PressedRadius,VR->Settings->MenuRecenterDegrees),VR->Settings->MenuRecenterDegrees>95.f && VR->Settings->MenuRecenterDegrees<102.f);
					TestTrue(TEXT("Slider thumb reads the live setting"),FMath::IsNearlyEqual(VR->GetSliderSetting("MenuRecenter"),(VR->Settings->MenuRecenterDegrees-30.f)/90.f));
					VR->Trigger(Left,false);PaintOptions();
					auto* Reloaded=NewObject<UACEVRSettings>();Reloaded->LoadConfig();
					TestEqual(TEXT("Releasing either controller saves the radius across restart"),Reloaded->MenuRecenterDegrees,VR->Settings->MenuRecenterDegrees);
				}
				for(int32 Mode:{0,2})
				{
					VR->Settings->MenuAnchorMode=Mode;PaintOptions();
					TestFalse(TEXT("Recenter slider is inactive for Head and World anchors"),Slider->IsEnabled());
				}
			}
		}
	}
	VR->Client->GetSession()->State=EACESessionState::Disconnected;
	// Clear render-thread provider references before the scoped controller goes away.
	Controllers.Connected=false;
	for(auto* Hand:{VR->LeftGrip.Get(),VR->RightGrip.Get(),VR->LeftAim.Get(),VR->RightAim.Get()})
		Hand->TickComponent(.016f,LEVELTICK_All,nullptr);
	FlushRenderingCommands();
	Pawn->Destroy();PC->Destroy();GI->Shutdown();GEngine->DestroyWorldContext(World);World->DestroyWorld(false);
	return true;
}
#endif
