#pragma once
#include "VR/ACEVRComponent.h"
#include "VR/ACEVRSettings.h"
#include "Widgets/SLeafWidget.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"

namespace ACEVRHUDArt
{
 inline void Text(FSlateWindowElementList& Out,int32 Layer,const FGeometry& G,FVector2f At,const FString& Value,int32 Size,FLinearColor Color,bool Center=false,bool CenterY=false)
 {
  auto Font=FCoreStyle::GetDefaultFontStyle("Bold",Size);Font.OutlineSettings.OutlineSize=2;
  Font.OutlineSettings.OutlineColor=FLinearColor::Black;
  const auto Extent=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Value,Font);
  if(Center) At.X-=Extent.X*.5f;
  if(CenterY) At.Y-=Extent.Y*.5f;
  FSlateDrawElement::MakeText(Out,Layer,G.ToPaintGeometry(FVector2f(G.GetLocalSize()),FSlateLayoutTransform(At)),Value,Font,ESlateDrawEffect::None,Color);
 }
 inline void Box(FSlateWindowElementList& Out,int32 Layer,const FGeometry& G,FVector2f At,FVector2f Size,FLinearColor Color,float Radius=8)
 {
  const FSlateRoundedBoxBrush Brush(FLinearColor::White,Radius);
  FSlateDrawElement::MakeBox(Out,Layer,G.ToPaintGeometry(Size,FSlateLayoutTransform(At)),&Brush,ESlateDrawEffect::None,Color);
 }
}

namespace ACEVRHUDArt
{
 inline void Controls(FSlateWindowElementList& Out,int32 L,const FGeometry& G,float Width,float Top,float Height,bool Locked)
 {
  const float Third=Width/3;
  for(int I=0;I<3;++I)
  {
   Box(Out,L,G,{I*Third+2,Top+2},{Third-4,Height-4},FLinearColor(.035f,.025f,.012f),5);
   Text(Out,L+1,G,{(I+.5f)*Third,Top+Height*.5f},I==1?(Locked?TEXT("Unlock"):TEXT("Lock")):I==0?TEXT("Move"):TEXT("Resize"),
    18,Locked && I!=1?FLinearColor(.35f,.32f,.27f):FLinearColor(.95f,.82f,.53f),true,true);
  }
 }
 inline void PressControl(UACEVRComponent* Rig,FName Panel,float X,float Width,bool Left)
 {
  if(X>=Width/3 && X<Width*2/3)Rig->TogglePanelLock(Panel);
  else if(Panel=="Vitals" && X<Width/3)Rig->BeginVitalsDrag(Left);
  else Rig->BeginPanelEdit(Panel,Left,X>=Width*2/3);
 }
}

/** Placement tools on a separate surface keep existing menu hit tests intact. */
class SACEVRPanelControls : public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SACEVRPanelControls){} SLATE_ARGUMENT(TWeakObjectPtr<UACEVRComponent>,Rig) SLATE_ARGUMENT(FName,Panel) SLATE_END_ARGS()
 void Construct(const FArguments& Args){Rig=Args._Rig;Panel=Args._Panel;}
 bool Refresh()
 {
  const bool Locked=!Rig.IsValid() || Rig->IsPanelLocked(Panel);
  const bool Retail=Rig.IsValid() && Panel=="Menu" && Rig->bUseDesktopMenu;
  const bool WindowsLocked=!Rig.IsValid() || Rig->IsRetailLayoutLocked();
  if(Locked==bLocked && Retail==bRetail && WindowsLocked==bWindowsLocked)return false;
  const auto Reason=Retail!=bRetail?EInvalidateWidgetReason::Layout:EInvalidateWidgetReason::Paint;
  bLocked=Locked;bRetail=Retail;bWindowsLocked=WindowsLocked;Invalidate(Reason);return true;
 }
 virtual FVector2D ComputeDesiredSize(float)const override{return FVector2D(bRetail?720:480,48);}
 virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
 {
  ACEVRHUDArt::Controls(Out,L,G,480,0,48,bLocked);
  if(bRetail)
  {
   ACEVRHUDArt::Box(Out,L,G,{482,2},{236,44},FLinearColor(.025f,.02f,.015f),4);
   ACEVRHUDArt::Text(Out,L+1,G,{600,24},bWindowsLocked?TEXT("Unlock retail windows"):TEXT("Lock retail windows"),
    16,FLinearColor(.95f,.82f,.53f),true,true);
  }
  return L+1;
 }
 virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)override
 {
  if(!Rig.IsValid())return FReply::Unhandled();
  const FVector2D Point=G.AbsoluteToLocal(E.GetScreenSpacePosition());
  if(bRetail && Point.X>=480)Rig->ToggleRetailLayoutLock();
  else ACEVRHUDArt::PressControl(Rig.Get(),Panel,Point.X,480,E.GetPointerIndex()==0);
  return FReply::Handled().CaptureMouse(SharedThis(this));
 }
 virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E)override
 {if(Rig.IsValid())Rig->EndPanelEdit(true,E.GetPointerIndex());return FReply::Handled().ReleaseMouseCapture();}
private:
 TWeakObjectPtr<UACEVRComponent> Rig;FName Panel;bool bLocked=true,bRetail=false,bWindowsLocked=true;
};

/** Headset-sized charge feedback drawn directly from the controller's charge. */
class SACEVRJumpCharge : public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SACEVRJumpCharge){} SLATE_END_ARGS()
 void Construct(const FArguments&){SetVisibility(EVisibility::HitTestInvisible);}
 bool Refresh(float Charge)
 {
  const float Next=FMath::IsFinite(Charge)?FMath::Clamp(Charge,0.f,1.f):0.f;
  if(FMath::IsNearlyEqual(Next,Fraction,.001f))return false;
  Fraction=Next;Invalidate(EInvalidateWidgetReason::Paint);return true;
 }
 float GetChargeFraction()const{return Fraction;}
 virtual FVector2D ComputeDesiredSize(float)const override{return FVector2D(480,90);}
 virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
 {
  using namespace ACEVRHUDArt;
  const FLinearColor Gold(.95f,.74f,.31f);
  Text(Out,L,G,{8,0},TEXT("JUMP"),24,Gold);
  Text(Out,L,G,{390,0},FString::Printf(TEXT("%d%%"),FMath::RoundToInt(Fraction*100)),24,FLinearColor::White);
  Box(Out,L,G,{4,36},{472,44},Gold,7);
  Box(Out,L+1,G,{7,39},{466,38},FLinearColor(.004f,.006f,.01f),5);
  if(Fraction>0)Box(Out,L+2,G,{11,43},{458*Fraction,30},FLinearColor(.68f,.32f,.025f),3);
  return L+2;
 }
private:
 float Fraction=0;
};

/** Native vector meters; no dependency on a rendered desktop canvas. */
class SACEVRVitals : public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SACEVRVitals) {} SLATE_ARGUMENT(TWeakObjectPtr<UACEVRComponent>,Rig) SLATE_END_ARGS()
 void Construct(const FArguments& Args) { Rig=Args._Rig; }
 bool Refresh(const FACEPlayerVitals& V)
 {
  if(!Rig.IsValid())return false;
  const int32 NewValues[]={V.Health,V.MaxHealth,V.Stamina,V.MaxStamina,V.Mana,V.MaxMana,Rig->GetCombatMode()};
  bool Changed=false;for(int I=0;I<7;++I)if(Values[I]!=NewValues[I]){Values[I]=NewValues[I];Changed=true;}
  const bool Controls=Rig->ShouldShowVitalsControls(),Locked=Rig->GetSettings()->bVitalsLocked;
  const FString Timer=Rig->HasCombatTimer()?Rig->GetCombatTimerText():FString();
  const float Progress=Rig->HasCombatTimer()?Rig->GetCombatTimerProgress():1.f;
  if(Controls!=bControls || Locked!=bLocked || Timer!=TimerText || !FMath::IsNearlyEqual(Progress,TimerProgress,.01f))Changed=true;
  bControls=Controls;bLocked=Locked;TimerText=Timer;TimerProgress=Progress;
  if(Changed)Invalidate(EInvalidateWidgetReason::Paint);return Changed;
 }
 virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(480,240); }
 virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
 {
  using namespace ACEVRHUDArt;
  const TCHAR* Names[]={TEXT("Health"),TEXT("Stamina"),TEXT("Mana")};
  const FLinearColor Colors[]={FLinearColor(.32f,.006f,.004f),FLinearColor(.25f,.11f,.003f),FLinearColor(.003f,.065f,.3f)};
  const TCHAR* Stance=Values[6]==ACECombatMode::Magic?TEXT("MAGIC"):Values[6]==ACECombatMode::Missile?TEXT("MISSILE"):Values[6]==ACECombatMode::Melee?TEXT("MELEE"):TEXT("PEACE");
  Text(Out,L,G,{8,0},Stance,20,FLinearColor(.95f,.82f,.53f));
  // A vector backpack stays crisp at any panel scale and needs no texture load.
  Box(Out,L,G,{354,0},{122,30},FLinearColor(.035f,.025f,.012f),5);
  Box(Out,L+1,G,{367,2},{14,10},FLinearColor(.85f,.64f,.24f),4);
  Box(Out,L+2,G,{362,7},{24,22},FLinearColor(.56f,.31f,.09f),5);
  Box(Out,L+3,G,{367,17},{14,9},FLinearColor(.95f,.74f,.31f),2);
  Text(Out,L+3,G,{394,15},TEXT("Pack"),20,FLinearColor::White,false,true);
  for(int I=0;I<3;++I)
  {
   const float Y=32+I*46,Ratio=Values[I*2+1]>0?FMath::Clamp(float(Values[I*2])/Values[I*2+1],0.f,1.f):0.f;
   Box(Out,L,G,{4,Y},{472,40},FLinearColor(.72f,.59f,.32f));
   Box(Out,L+1,G,{6,Y+2},{468,36},FLinearColor(.002f,.003f,.005f));
   if(Ratio>0)Box(Out,L+2,G,{9,Y+5},{462*Ratio,30},Colors[I],5);
   Text(Out,L+3,G,{18,Y+20},Names[I],22,FLinearColor::White,false,true);
   Text(Out,L+3,G,{344,Y+20},FString::Printf(TEXT("%d / %d"),Values[I*2],Values[I*2+1]),22,FLinearColor::White,true,true);
  }
  if(!TimerText.IsEmpty())
  {
   Box(Out,L,G,{4,175},{472,6},FLinearColor(.06f,.07f,.09f));
   Box(Out,L+1,G,{4,175},{472*FMath::Clamp(TimerProgress,0.f,1.f),6},FLinearColor(.9f,.72f,.3f),3);
   Text(Out,L+2,G,{240,184},TimerText,20,FLinearColor::White,true);
  }
  if(bControls)Controls(Out,L+2,G,480,210,30,bLocked);
  return L+3;
 }
 virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)override
 {
  if(!Rig.IsValid())return FReply::Unhandled();
  const auto P=G.AbsoluteToLocal(E.GetScreenSpacePosition());
  if(P.X>=354 && P.Y<32){Rig->ToggleInventory();return FReply::Handled();}
  if(P.Y>=210 && bControls)ACEVRHUDArt::PressControl(Rig.Get(),"Vitals",P.X,480,E.GetPointerIndex()==0);
  else Rig->BeginVitalsDrag(E.GetPointerIndex()==0);
  return FReply::Handled().CaptureMouse(SharedThis(this));
 }
 virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E)override
 {if(Rig.IsValid()){Rig->EndVitalsDrag(true,E.GetPointerIndex());Rig->EndPanelEdit(true,E.GetPointerIndex());}return FReply::Handled().ReleaseMouseCapture();}
private:
 TWeakObjectPtr<UACEVRComponent> Rig;
 int32 Values[7]={};bool bControls=false,bLocked=true;float TimerProgress=1;FString TimerText;
};

/** Other members only; transparent between rows and redrawn on displayed changes. */
class SACEVRFellowship : public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SACEVRFellowship){} SLATE_ARGUMENT(TWeakObjectPtr<UACEVRComponent>,Rig) SLATE_ARGUMENT(TFunction<void(int32)>,OnSelect) SLATE_END_ARGS()
 void Construct(const FArguments& A){Rig=A._Rig;Select=A._OnSelect;}
 bool Refresh(const FACEFellowshipInfo& F,int32 SelectedGuid,int32 SelfGuid)
 {
  FACEFellowshipInfo Others;
  if(F.bValid)for(const auto& M:F.Members)if(M.Guid!=SelfGuid && Others.Members.Num()<8)Others.Members.Add(M);
  uint32 H=GetTypeHash(Others.Members.Num());
  for(const auto& M:Others.Members)
  {
   H=HashCombine(H,GetTypeHash(M.Name));
   for(int32 V:{M.Guid,M.Level,M.HealthCur,M.HealthMax,M.StaminaCur,M.StaminaMax,M.ManaCur,M.ManaMax})H=HashCombine(H,GetTypeHash(V));
  }
  H=HashCombine(H,GetTypeHash(SelectedGuid));
  const bool C=Rig.IsValid() && Rig->ShouldShowPanelControls("Fellowship"),Locked=!Rig.IsValid() || Rig->IsPanelLocked("Fellowship");
  H=HashCombine(H,GetTypeHash(C));H=HashCombine(H,GetTypeHash(Locked));
  if(bReady && H==Hash)return false;
  bReady=true;Hash=H;Info=MoveTemp(Others);Selected=SelectedGuid;bControls=C;bLocked=Locked;
  Invalidate(EInvalidateWidgetReason::Layout);return true;
 }
 float ControlsY()const{return 8+Info.Members.Num()*69;}
 int32 MemberGuidAt(float Y)const{const int32 I=FMath::FloorToInt((Y-8)/69);return Info.Members.IsValidIndex(I)?Info.Members[I].Guid:0;}
 virtual FVector2D ComputeDesiredSize(float)const override{return FVector2D(480,FMath::Max(1.f,ControlsY()+(bControls?48.f:0.f)));}
 virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
 {
  using namespace ACEVRHUDArt;
  const FLinearColor Colors[]={FLinearColor(.65f,.045f,.035f),FLinearColor(.6f,.32f,.015f),FLinearColor(.035f,.25f,.8f)};
  for(int32 I=0;I<Info.Members.Num();++I)
  {
   const auto& M=Info.Members[I];const float Y=8+I*69;
   if(M.Guid==Selected)Box(Out,L+1,G,{8,Y},{3,60},FLinearColor(.25f,.9f,.35f),1);
   Text(Out,L+2,G,{16,Y+3},M.Name.Left(32),20,FLinearColor::White);
   const int32 Cur[]={M.HealthCur,M.StaminaCur,M.ManaCur},Max[]={M.HealthMax,M.StaminaMax,M.ManaMax};
   for(int32 V=0;V<3;++V)
   {
    const float X=16+V*152;
    Box(Out,L+2,G,{X,Y+35},{144,20},FLinearColor(.001f,.002f,.004f),4);
    const float Fraction=Max[V]>0?FMath::Clamp(float(Cur[V])/Max[V],0.f,1.f):0.f;
    if(Fraction>0)Box(Out,L+3,G,{X+2,Y+37},{140*Fraction,16},Colors[V],3);
    Text(Out,L+4,G,{X+72,Y+45},Max[V]>0?FString::Printf(TEXT("%d/%d"),Cur[V],Max[V]):TEXT("--"),14,FLinearColor::White,true,true);
   }
  }
  if(bControls)Controls(Out,L+3,G,480,ControlsY(),44,bLocked);
  return L+4;
 }
 virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)override
 {
  if(!Rig.IsValid())return FReply::Unhandled();const auto P=G.AbsoluteToLocal(E.GetScreenSpacePosition());
  if(P.Y>=ControlsY() && bControls)ACEVRHUDArt::PressControl(Rig.Get(),"Fellowship",P.X,480,E.GetPointerIndex()==0);
  else if(const int32 Guid=MemberGuidAt(P.Y);Guid!=0 && Select)Select(Guid);
  return FReply::Handled().CaptureMouse(SharedThis(this));
 }
 virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E)override
 {if(Rig.IsValid())Rig->EndPanelEdit(true,E.GetPointerIndex());return FReply::Handled().ReleaseMouseCapture();}
private:
 TWeakObjectPtr<UACEVRComponent> Rig;TFunction<void(int32)> Select;FACEFellowshipInfo Info;
 uint32 Hash=0;int32 Selected=0;bool bReady=false,bControls=false,bLocked=true;
};

struct FACEVRRadarMarker { FVector2f Point; FLinearColor Color; int32 Guid=0; int32 Height=0; bool Selected=false; };
class SACEVRCompass : public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SACEVRCompass){} SLATE_ARGUMENT(TWeakObjectPtr<UACEVRComponent>,Rig) SLATE_ARGUMENT(TFunction<void(int32)>,OnSelect) SLATE_END_ARGS()
 void Construct(const FArguments& Args){Select=Args._OnSelect;Rig=Args._Rig;}
 bool UpdateHeading(float Yaw,float Dt,bool Reset=false)
 {
  const float Target=HALF_PI-FMath::DegreesToRadians(Yaw);
  const float Difference=FMath::FindDeltaAngleRadians(NorthAngle,Target);
  const float Step=(!bHeadingReady || Reset)?Difference:Difference*(1.f-FMath::Exp(-FMath::Max(0.f,Dt)/.04f));
  NorthAngle=FMath::UnwindRadians(NorthAngle+Step);bHeadingReady=true;
  const bool Controls=Rig.IsValid() && Rig->ShouldShowPanelControls("Compass");
  const bool Locked=!Rig.IsValid() || Rig->IsPanelLocked("Compass");
  const bool Changed=FMath::Abs(Step)>.00001f || Controls!=bControls || Locked!=bLocked || Reset;
  bControls=Controls;bLocked=Locked;
  if(Changed)Invalidate(EInvalidateWidgetReason::Paint);
  return Changed;
 }
 FVector2f MarkerPoint(const FACEVRRadarMarker& Marker)const
 {
  const float Yaw=HALF_PI-NorthAngle,C=FMath::Cos(Yaw),S=FMath::Sin(Yaw);
  const FVector2f P=Marker.Point-FVector2f(200,200);
  return FVector2f(200+C*P.X+S*P.Y,200-S*P.X+C*P.Y);
 }
 TArray<FACEVRRadarMarker> Markers;
 FString Coordinates;
 float NorthAngle=0;
 virtual FVector2D ComputeDesiredSize(float)const override{return FVector2D(400,500);}
 virtual int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 L,const FWidgetStyle&,bool)const override
 {
  using namespace ACEVRHUDArt;
  // A wider engraved bezel reserves space for equally centered compass labels.
  const FLinearColor Gold(.72f,.49f,.19f),LightGold(.95f,.79f,.43f),DarkGold(.15f,.085f,.025f);
  Box(Out,L,G,{3,3},{394,394},DarkGold,197);
  Box(Out,L+1,G,{6,6},{388,388},LightGold,194);
  Box(Out,L+2,G,{9,9},{382,382},DarkGold,191);
  Box(Out,L+3,G,{13,13},{374,374},Gold,187);
  Box(Out,L+4,G,{17,17},{366,366},FLinearColor(.018f,.012f,.007f),183);
  Box(Out,L+5,G,{38,38},{324,324},Gold,162);
  Box(Out,L+6,G,{40,40},{320,320},FLinearColor(.002f,.004f,.007f,.98f),160);
  for(int I=0;I<32;++I)
  {
   const float A=NorthAngle+I*PI/16;
   if(I%8==0)continue;
   const FVector2D D(FMath::Sin(A),-FMath::Cos(A)),C(200,200);
   TArray<FVector2D> Tick={C+D*185,C+D*(I%4==0?164:179)};
   FSlateDrawElement::MakeLines(Out,L+7,G.ToPaintGeometry(),Tick,ESlateDrawEffect::None,LightGold,true,I%4==0?2.f:1.f);
  }
  const TCHAR* Directions[]={TEXT("N"),TEXT("E"),TEXT("S"),TEXT("W")};
  for(int I=0;I<4;++I)
  {
   const float A=NorthAngle+I*HALF_PI;
   Text(Out,L+8,G,{200+FMath::Sin(A)*174,200-FMath::Cos(A)*174},Directions[I],22,LightGold,true,true);
  }
  for(const auto& M:Markers)
  {
   const FVector2f Point=MarkerPoint(M);
   if(M.Selected)Box(Out,L+7,G,Point-FVector2f(12,12),{24,24},FLinearColor::White,4);
   Box(Out,L+8,G,Point-FVector2f(7,7),{14,14},M.Color,7);
  }
  TArray<FVector2D> Arrow={{200,188},{193,205},{200,201},{207,205},{200,188}};
  FSlateDrawElement::MakeLines(Out,L+10,G.ToPaintGeometry(),Arrow,ESlateDrawEffect::None,FLinearColor::White,true,2.5f);
  Text(Out,L+8,G,{200,410},Coordinates,24,FLinearColor::White,true);
  if(bControls)Controls(Out,L+9,G,400,450,48,bLocked);
  return L+10;
 }
 virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)override
 {
  const FVector2f P(G.AbsoluteToLocal(E.GetScreenSpacePosition()));
  if(P.Y>=450 && bControls && Rig.IsValid())
  {
   ACEVRHUDArt::PressControl(Rig.Get(),"Compass",P.X,400,E.GetPointerIndex()==0);
   return FReply::Handled().CaptureMouse(SharedThis(this));
  }
  int32 Id=0;float Best=18*18;
  for(const auto& M:Markers){const float D=(MarkerPoint(M)-P).SizeSquared();if(D<Best){Best=D;Id=M.Guid;}}
  if(Id && Select)Select(Id);
  return FReply::Handled();
 }
virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent& E)override
 {if(Rig.IsValid())Rig->EndPanelEdit(true,E.GetPointerIndex());return FReply::Handled().ReleaseMouseCapture();}
private:
 TWeakObjectPtr<UACEVRComponent> Rig;
 bool bControls=false,bLocked=true,bHeadingReady=false;
 TFunction<void(int32)> Select;
};
