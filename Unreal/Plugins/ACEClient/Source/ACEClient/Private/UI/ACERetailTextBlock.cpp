#include "UI/ACERetailTextBlock.h"
#include "UI/ACEUIResourceResolver.h"
#include "UI/ACEUICanvasWidget.h"
#include "UI/ACEUIFontStyles.h"
#include "ACEDatSubsystem.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Dat/ACEDatTextLayout.h"
#include "Engine/Texture2D.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/CoreStyle.h"

class SACERetailTextBlock : public STextBlock
{
public:
	SLATE_BEGIN_ARGS(SACERetailTextBlock) {} SLATE_END_ARGS()
	void Construct(const FArguments&, UACERetailTextBlock* InOwner)
	{
		Owner = InOwner;
		STextBlock::Construct(STextBlock::FArguments());
		SetClipping(EWidgetClipping::ClipToBounds);
	}

	virtual bool SupportsKeyboardFocus() const override { return Owner.IsValid() && Owner->IsSelectable(); }
	virtual FCursorReply OnCursorQuery(const FGeometry& G, const FPointerEvent& E) const override
	{
		if (Owner.IsValid() && Owner->IsLinkAt(CharacterAt(G,E.GetScreenSpacePosition()))) return FCursorReply::Cursor(EMouseCursor::Hand);
		return SupportsKeyboardFocus() ? FCursorReply::Cursor(EMouseCursor::TextEditBeam) : STextBlock::OnCursorQuery(G,E);
	}
	virtual FReply OnMouseButtonDown(const FGeometry& G, const FPointerEvent& E) override
	{
		if (!SupportsKeyboardFocus()) return FReply::Unhandled();
		if (E.GetEffectingButton()==EKeys::LeftMouseButton)
		{
			const int32 At=CharacterAt(G,E.GetScreenSpacePosition());
			if (!E.IsShiftDown()) ClearPeerSelection();
			if (!E.IsShiftDown()) Anchor=At;
			Caret=At; PressPosition=E.GetScreenSpacePosition(); bDragged=E.IsShiftDown();
			Invalidate(EInvalidateWidgetReason::Paint);
			return FReply::Handled().SetUserFocus(SharedThis(this)).CaptureMouse(SharedThis(this));
		}
		if (E.GetEffectingButton()==EKeys::RightMouseButton)
		{
			FMenuBuilder Menu(true,nullptr);
			auto AddCopy=[&](const TCHAR* Caption,const FString& Text)
			{
				Menu.AddMenuEntry(FText::FromString(Caption),FText::GetEmpty(),FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([Text]{FPlatformApplicationMisc::ClipboardCopy(*Text);})));
			};
			if (!SelectedText().IsEmpty()) AddCopy(TEXT("Copy selection"),SelectedText());
			AddCopy(TEXT("Copy message"),GetText().ToString());
			if (Owner->GetCopyAllText) AddCopy(TEXT("Copy chat window"),Owner->GetCopyAllText());
			FSlateApplication::Get().PushMenu(SharedThis(this),FWidgetPath(),Menu.MakeWidget(),
				E.GetScreenSpacePosition(),FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu));
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	virtual FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E) override
	{
		if (!HasMouseCapture()) return FReply::Unhandled();
		bDragged|=FVector2D::Distance(PressPosition,E.GetScreenSpacePosition())>3.f;
		SelectThrough(G,E.GetScreenSpacePosition());
		Invalidate(EInvalidateWidgetReason::Paint);
		return FReply::Handled();
	}
	virtual FReply OnMouseButtonUp(const FGeometry& G,const FPointerEvent& E) override
	{
		if (!HasMouseCapture() || E.GetEffectingButton()!=EKeys::LeftMouseButton) return FReply::Unhandled();
		bDragged|=FVector2D::Distance(PressPosition,E.GetScreenSpacePosition())>3.f;
		if(bDragged)SelectThrough(G,E.GetScreenSpacePosition());
		if (!bDragged && Owner.IsValid())
		{
			if (Owner->IsLinkAt(Caret) && Owner->OnLinkClicked && Owner->OnLinkClicked(Caret))
				return FReply::Handled().ReleaseMouseCapture();
			Owner->OnTextClicked.ExecuteIfBound();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& G,const FPointerEvent& E) override
	{
		if (!SupportsKeyboardFocus() || E.GetEffectingButton()!=EKeys::LeftMouseButton) return FReply::Unhandled();
		ClearPeerSelection();
		const FString Text=GetText().ToString();
		Anchor=Caret=FMath::Min(CharacterAt(G,E.GetScreenSpacePosition()),FMath::Max(0,Text.Len()-1));
		// Double-click selects a word; whole-message/window copying remains in
		// the context menu. Dragging or Shift-click still chooses exact characters.
		if(!Text.IsEmpty())
		{
			const bool Space=FChar::IsWhitespace(Text[Anchor]);
			while(Anchor>0 && FChar::IsWhitespace(Text[Anchor-1])==Space)--Anchor;
			while(Caret<Text.Len() && FChar::IsWhitespace(Text[Caret])==Space)++Caret;
		}
		bDragged=true;
		Invalidate(EInvalidateWidgetReason::Paint);
		return FReply::Handled().SetUserFocus(SharedThis(this));
	}
	virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent& E) override
	{
		if (!SupportsKeyboardFocus()) return FReply::Unhandled();
		if (E.IsControlDown() && E.GetKey()==EKeys::A)
		{
			for (auto* Peer : Peers()) { Peer->Anchor=0; Peer->Caret=Peer->GetText().ToString().Len(); Peer->Invalidate(EInvalidateWidgetReason::Paint); }
			Anchor=0; Caret=GetText().ToString().Len(); Invalidate(EInvalidateWidgetReason::Paint);
			return FReply::Handled();
		}
		if (E.IsControlDown() && E.GetKey()==EKeys::C)
		{
			const FString Selection=SelectedText();
			FPlatformApplicationMisc::ClipboardCopy(*(Selection.IsEmpty() ? GetText().ToString() : Selection));
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}
	virtual void OnFocusLost(const FFocusEvent& E) override
	{
		STextBlock::OnFocusLost(E);
		// Preserve the selection for copying or moving focus between rows.
		// Starting another selection clears the entire window's old range.
		Invalidate(EInvalidateWidgetReason::Paint);
	}

	virtual void Tick(const FGeometry& Geometry, double Time, float DeltaTime) override
	{
		STextBlock::Tick(Geometry, Time, DeltaTime);
		const auto* Label = Owner.Get();
		if (!Label || !Label->GetBitmapFont() || Label->GetNativeWidth() > 0) return;
		const int32 Width = FMath::Max(1, FMath::FloorToInt(Geometry.GetLocalSize().X / Label->GetBitmapScale().X));
		if (AllocatedNativeWidth != Width)
		{
			// ScrollBox children have no authored width. Keep the allotted width for
			// the next prepass, so wrapped lines reserve height instead of being clipped.
			AllocatedNativeWidth = Width;
			Invalidate(EInvalidateWidgetReason::Layout);
		}
	}

	virtual FVector2D ComputeDesiredSize(float Scale) const override
	{
		const UACERetailTextBlock* Label = Owner.Get();
		// Slate can finish a prepass after UMG releases/rebuilds the owning label.
		// The inherited text style contains pointers into that UObject (StrikeBrush).
		if (!Label) return FVector2D::ZeroVector;
		const FACEDatFont* DatFont = Label->GetBitmapFont();
		if (!DatFont) return STextBlock::ComputeDesiredSize(Scale);
		UpdateLines(*Label, *DatFont, AllocatedNativeWidth);
		int32 Width = 0;
		for (const FACEBitmapTextLine& Line : Lines) Width = FMath::Max(Width, Line.Width);
		const FMargin TextInsets = GetMargin();
		return FVector2D(Width, Lines.Num() * DatFont->MaxCharHeight) * Label->GetBitmapScale()
			+ FVector2D(TextInsets.Left + TextInsets.Right, TextInsets.Top + TextInsets.Bottom);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& CullingRect,
		FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle& Style, bool bParentEnabled) const override
	{
		const UACERetailTextBlock* Label = Owner.Get();
		if (!Label) return Layer;
		const FACEDatFont* DatFont = Label->GetBitmapFont();
		if (!DatFont) return STextBlock::OnPaint(Args, Geometry, CullingRect, Elements, Layer, Style, bParentEnabled);
		const int32 Width = FMath::Max(1, FMath::FloorToInt(Geometry.GetLocalSize().X / Label->GetBitmapScale().X));
		if (Label->GetNativeWidth() <= 0 && AllocatedNativeWidth != Width)
		{
			AllocatedNativeWidth = Width;
			const_cast<SACERetailTextBlock*>(this)->Invalidate(EInvalidateWidgetReason::Layout);
		}
		UpdateLines(*Label, *DatFont, Geometry.GetLocalSize().X / Label->GetBitmapScale().X);
		const TSharedPtr<FACEUIElement> Element = Label->GetRetailElement();
		// The canvas flattens DAT children into one Slate layer. Reapply ancestor
		// clipping here so a scrolled label cannot paint over the panel's frame.
		// Glyph quads use the accumulated render transform. Layout bounds omit
		// render scaling (including DPI compensation) and can clip the text early.
		FSlateRect Clip = Geometry.GetRenderBoundingRect().IntersectionWith(CullingRect);
		if (Element && Label->UsesDatAncestorClipping())
		{
			const auto* Canvas = Label->GetTypedOuter<UACEUICanvasWidget>();
			if (!Canvas) return Layer;
			// OnPaint uses window space. Cached/tick geometry includes the desktop
			// window offset, which clipped static labels away in windowed mode.
			const FGeometry& CanvasGeometry = Canvas->GetPaintSpaceGeometry();
			for (auto Parent = Element->Parent.Pin(); Parent; Parent = Parent->Parent.Pin())
			{
				// SyntheticRoot is an ownership node, not an authored clipping region.
				if (!Parent->Parent.IsValid()) continue;
				const FIntPoint P = Parent->GetScreenOrigin();
				if (!Parent->bVisible || Parent->Width <= 0 || Parent->Height <= 0) return Layer;
				const FVector2D Min = CanvasGeometry.LocalToAbsolute(Canvas->LayoutToViewport(FVector2D(P)));
				const FVector2D Max = CanvasGeometry.LocalToAbsolute(Canvas->LayoutToViewport(FVector2D(P+FIntPoint(Parent->Width,Parent->Height))));
				Clip = Clip.IntersectionWith(FSlateRect(Min.X,Min.Y,Max.X,Max.Y));
			}
		}
		if (Clip.Right <= Clip.Left || Clip.Bottom <= Clip.Top) return Layer;
		Elements.PushClip(FSlateClippingZone(Clip));
		const FACEUIStateMedia* State = Element ? Element->States.Find(Element->PaintState) : nullptr;
		const bool bOutline = State && State->bTextOutline.IsSet() ? State->bTextOutline.GetValue()
			: Element && Element->bTextOutline;
		const FLinearColor TextColor = State && State->TextColor.IsSet() ? State->TextColor.GetValue()
			: GetColorAndOpacity().GetColor(Style);
		const FLinearColor OutlineColor = Element ? Element->TextOutlineColor : FLinearColor::Black;
		const FVector2D Scale = Label->GetBitmapScale();
		const FMargin TextInsets = GetMargin();
		const float AvailableWidth = FMath::Max(0.f, Geometry.GetLocalSize().X - TextInsets.Left - TextInsets.Right);
		float Top = TextInsets.Top;
		const float SpareHeight = FMath::Max(0.f, Geometry.GetLocalSize().Y - TextInsets.Top - TextInsets.Bottom
			- Lines.Num() * DatFont->MaxCharHeight * Scale.Y);
		if (Element && Element->TextVerticalJustification == 1) Top += FMath::FloorToFloat(SpareHeight * .5f);
		else if (Element && Element->TextVerticalJustification == 5) Top += SpareHeight;
		const FVector2D RasterScale = Scale * Geometry.GetAccumulatedLayoutTransform().GetScale();
		const bool IntegerScale = FMath::IsNearlyEqual(RasterScale.X,FMath::RoundToDouble(RasterScale.X),1.e-4)
			&& FMath::IsNearlyEqual(RasterScale.Y,FMath::RoundToDouble(RasterScale.Y),1.e-4);
		const ESlateDrawEffect Effects = (ShouldBeEnabled(bParentEnabled) ? ESlateDrawEffect::None : ESlateDrawEffect::DisabledEffect)
			| (IntegerScale ? ESlateDrawEffect::None : ESlateDrawEffect::NoPixelSnapping);
		if (Label->IsSelectable() && Anchor!=Caret)
		{
			for (int32 L=0; L<Lines.Num(); ++L)
			{
				const auto& Line=Lines[L];
				const int32 Begin=FMath::Max(Line.Begin,FMath::Min(Anchor,Caret));
				const int32 End=FMath::Min(Line.End,FMath::Max(Anchor,Caret));
				if (Begin>=End) continue;
				float X=TextInsets.Left, SelectedWidth=0;
				for (int32 I=Line.Begin; I<End; ++I)
				{
					const float Advance=ACEDatText::Advance(*DatFont,CachedText[I])*Scale.X;
					if (I<Begin) X+=Advance; else SelectedWidth+=Advance;
				}
				FSlateDrawElement::MakeBox(Elements,Layer,Geometry.ToPaintGeometry(
					FVector2D(SelectedWidth,DatFont->MaxCharHeight*Scale.Y),
					FSlateLayoutTransform(FVector2D(X,Top+L*DatFont->MaxCharHeight*Scale.Y))),
					FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")),Effects,FLinearColor(.18f,.28f,.45f,.8f));
			}
			++Layer;
		}
		auto DrawPass = [&](bool bBackground, FVector2D Offset)
		{
			UTexture2D* Atlas = Label->GetGlyphAtlas(bBackground);
			const bool bHasBackgroundAtlas = Atlas != nullptr;
			if (!Atlas) Atlas = Label->GetGlyphAtlas(false);
			FSlateBrush Brush;
			Brush.DrawAs = ESlateBrushDrawType::Image;
			Brush.SetResourceObject(Atlas);
			const bool bTint = Label->ShouldTintAtlas(bBackground && bHasBackgroundAtlas);
			FLinearColor Tint = bTint ? (bBackground ? OutlineColor : TextColor) : FLinearColor::White;
			Tint *= Style.GetColorAndOpacityTint();
			const int32 BorderX = bBackground && bHasBackgroundAtlas ? DatFont->NumHorizontalBorderPixels : 0;
			const int32 BorderY = bBackground && bHasBackgroundAtlas ? DatFont->NumVerticalBorderPixels : 0;
			for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
			{
				const FACEBitmapTextLine& Line = Lines[LineIndex];
				const float Spare = FMath::Max(0.f, AvailableWidth - Line.Width * Scale.X);
				float X = TextInsets.Left;
				if (Label->GetTextJustification() == ETextJustify::Center) X += FMath::FloorToFloat(Spare * .5f);
				else if (Label->GetTextJustification() == ETextJustify::Right) X += Spare;
				const float Y = Top + LineIndex * DatFont->MaxCharHeight * Scale.Y;
				for (int32 I = Line.Begin; I < Line.End; ++I)
				{
					if (CachedText[I] == '\n') continue;
					const FACEDatFontChar* Ch = ACEDatText::FindChar(*DatFont, CachedText[I]);
					if (!Ch) continue;
					if (!bBackground && Label->IsLinkAt(I))
					{
						const float Advance=ACEDatText::Advance(*DatFont,CachedText[I])*Scale.X;
						FSlateDrawElement::MakeLines(Elements,Layer+1,Geometry.ToPaintGeometry(),
							TArray<FVector2D>{FVector2D(X,Y+DatFont->MaxCharHeight*Scale.Y-1),FVector2D(X+Advance,Y+DatFont->MaxCharHeight*Scale.Y-1)},
							ESlateDrawEffect::None,FLinearColor(.18f,.72f,1.f),true,1.f);
					}
					X += static_cast<int8>(Ch->HorizontalOffsetBefore) * Scale.X;
					if (Ch->Width > 0 && Ch->Height > 0)
					{
						const FVector2D PixelSize(Ch->Width + 2 * BorderX, Ch->Height + 2 * BorderY);
						const FVector2D AtlasSize(Atlas->GetSizeX(), Atlas->GetSizeY());
						const FVector2D UV(Ch->OffsetX - BorderX, Ch->OffsetY - BorderY);
						Brush.SetUVRegion(FBox2f(FVector2f(UV / AtlasSize), FVector2f((UV + PixelSize) / AtlasSize)));
						const FVector2D Pos = FVector2D(X, Y + static_cast<int8>(Ch->VerticalOffsetBefore) * Scale.Y)
							+ (Offset - FVector2D(BorderX, BorderY)) * Scale;
						FSlateDrawElement::MakeBox(Elements, Layer + (bBackground ? 0 : 1),
							Geometry.ToPaintGeometry(PixelSize * Scale, FSlateLayoutTransform(Pos)), &Brush, Effects,
								!bBackground && bTint ? Label->GetGlyphColor(I, TextColor)
								* Style.GetColorAndOpacityTint() : Tint);
					}
					X += (Ch->Width + static_cast<int8>(Ch->HorizontalOffsetAfter)) * Scale.X;
				}
			}
		};
		if (bOutline)
		{
			if (Label->GetGlyphAtlas(true)) DrawPass(true, FVector2D::ZeroVector);
			else for (int32 Y = -1; Y <= 1; ++Y) for (int32 X = -1; X <= 1; ++X)
				if (X || Y) DrawPass(true, FVector2D(X, Y));
		}
		DrawPass(false, FVector2D::ZeroVector);
		Elements.PopClip();
		return Layer + 1;
	}

private:
	FString SelectedText() const
	{
		FString Result;
		for (const auto* Peer : Peers())
		{
			if (Peer->Anchor==Peer->Caret) continue;
			if (!Result.IsEmpty()) Result+=TEXT("\r\n");
			Result+=Peer->GetText().ToString().Mid(FMath::Min(Peer->Anchor,Peer->Caret),FMath::Abs(Peer->Anchor-Peer->Caret));
		}
		return Result;
	}
	TArray<SACERetailTextBlock*> Peers() const
	{
		TArray<SACERetailTextBlock*> Result;
		if (Owner.IsValid() && Owner->GetSelectionPeers)
			for (auto* Row : Owner->GetSelectionPeers())
				if (Row && Row->GetSelectionWidget()) Result.Add(static_cast<SACERetailTextBlock*>(Row->GetSelectionWidget().Get()));
		if (Result.IsEmpty()) Result.Add(const_cast<SACERetailTextBlock*>(this));
		return Result;
	}
	void ClearPeerSelection()
	{
		for (auto* Peer : Peers()) { Peer->Anchor=Peer->Caret=0; Peer->Invalidate(EInvalidateWidgetReason::Paint); }
	}
	void SelectThrough(const FGeometry& StartGeometry,FVector2D Position)
	{
		const auto Rows=Peers();
		const int32 Start=Rows.Find(this);
		if (Start==INDEX_NONE) return;
		// Scrolled/clipped peers can retain geometry from an earlier layout. A
		// drag within this message must use the actual event geometry, not let an
		// offscreen peer turn a partial-word selection into several whole lines.
		const auto Local=StartGeometry.AbsoluteToLocal(Position);
		const bool SameRow=Local.Y>=0 && Local.Y<StartGeometry.GetLocalSize().Y;
		int32 End=SameRow?Start:Rows.Num()-1;
		for (int32 I=0; !SameRow && I<Rows.Num(); ++I)
		{
			const auto& Geometry=Rows[I]->GetCachedGeometry();
			if (Position.Y < Geometry.LocalToAbsolute(FVector2D(0,Geometry.GetLocalSize().Y)).Y) { End=I; break; }
		}
		const int32 StartChar=Anchor;
		const int32 EndChar=Rows[End]->CharacterAt(SameRow?StartGeometry:Rows[End]->GetCachedGeometry(),Position);
		for (int32 I=0; I<Rows.Num(); ++I)
		{
			auto* Row=Rows[I];
			int32 A=0,B=0;
			if (I>=FMath::Min(Start,End) && I<=FMath::Max(Start,End))
			{
				A=I==Start?StartChar:(End<Start?Row->GetText().ToString().Len():0);
				B=I==End?EndChar:(End<Start?0:Row->GetText().ToString().Len());
			}
			Row->Anchor=A; Row->Caret=B; Row->Invalidate(EInvalidateWidgetReason::Paint);
		}
	}
	int32 CharacterAt(const FGeometry& G,FVector2D Absolute) const
	{
		const auto* Label=Owner.Get();
		const auto* GlyphFont=Label ? Label->GetBitmapFont() : nullptr;
		if (!GlyphFont) return 0;
		const auto Scale=Label->GetBitmapScale();
		UpdateLines(*Label,*GlyphFont,G.GetLocalSize().X/Scale.X);
		if (Lines.IsEmpty()) return 0;
		const auto Local=G.AbsoluteToLocal(Absolute);
		const auto Insets=GetMargin();
		const int32 L=FMath::Clamp(FMath::FloorToInt((Local.Y-Insets.Top)/(GlyphFont->MaxCharHeight*Scale.Y)),0,Lines.Num()-1);
		const auto& Line=Lines[L];
		float X=Insets.Left;
		for (int32 I=Line.Begin; I<Line.End; ++I)
		{
			const float Advance=ACEDatText::Advance(*GlyphFont,CachedText[I])*Scale.X;
			if (Local.X<X+Advance*.5f) return I;
			X+=Advance;
		}
		return Line.End;
	}
	int32 Anchor=0, Caret=0;
	FVector2D PressPosition=FVector2D::ZeroVector;
	bool bDragged=false;
	void UpdateLines(const UACERetailTextBlock& Label, const FACEDatFont& DatFont, float AllocatedWidth = 0) const
	{
		const FString Text = GetText().ToString();
		const TSharedPtr<FACEUIElement> Element = Label.GetRetailElement();
		const bool bOneLine = Element ? Element->bTextOneLine : !Label.GetAutoWrapText();
		const FMargin TextInsets = GetMargin();
		const int32 Width = FMath::Max(1, FMath::FloorToInt((Label.GetNativeWidth() > 0 ? Label.GetNativeWidth() : AllocatedWidth > 0 ? AllocatedWidth : 10000.f)
			- (TextInsets.Left + TextInsets.Right) / Label.GetBitmapScale().X));
		if (CachedText != Text || CachedFont != DatFont.Id || CachedWidth != Width || bCachedOneLine != bOneLine)
		{
			CachedText = Text;
			CachedFont = DatFont.Id;
			CachedWidth = Width;
			bCachedOneLine = bOneLine;
			Lines = ACEDatText::Layout(DatFont, Text, Width, bOneLine);
		}
	}
	TWeakObjectPtr<UACERetailTextBlock> Owner;
	mutable int32 AllocatedNativeWidth = 0;
	mutable FString CachedText;
	mutable uint32 CachedFont = 0;
	mutable int32 CachedWidth = -1;
	mutable bool bCachedOneLine = false;
	mutable TArray<FACEBitmapTextLine> Lines;
};

void UACERetailTextBlock::SetRetailElement(UACEUIResourceResolver* InResources,
	const TSharedPtr<FACEUIElement>& Element, FVector2D Scale, float InNativeWidth, bool bFlattenedOnCanvas)
{
	const FACEUIStateMedia* State = Element ? Element->States.Find(Element->PaintState) : nullptr;
	uint32 FontId = State && State->FontId ? State->FontId : Element ? Element->FontId : 0;
	if (!FontId) FontId = ACEUIFontStyles::ResolveForElement(Element, GetFont().Size).FontId;
	if (!FontId) FontId = GetFont().Size <= 8 ? 0x40000002u : 0x40000000u;
	if (FontOverride) FontId = FontOverride;
	bool bChanged = Resources != InResources || BitmapFont.Id != FontId || !ForegroundAtlas;
	if (bChanged)
	{
		Resources = InResources;
		BitmapFont = FACEDatFont();
		ForegroundAtlas = nullptr;
		BackgroundAtlas = nullptr;
		if (Resources && Resources->ResolveFont(FontId, BitmapFont))
		{
			ForegroundAtlas = Resources->ResolveFontAtlas(BitmapFont.ForegroundSurfaceDataID, bTintForeground);
			BackgroundAtlas = Resources->ResolveFontAtlas(BitmapFont.BackgroundSurfaceDataID, bTintBackground);
		}
	}
	bChanged |= BitmapScale != Scale || NativeWidth != InNativeWidth || RetailElement.Pin() != Element;
	bChanged |= Element && LastPaintState != Element->PaintState;
	bChanged |= bApplyDatAncestorClip != bFlattenedOnCanvas;
	// A real ScrollBox already clips its descendants. Projecting DAT ancestors
	// from its translated child geometry would move the clip with the text.
	bApplyDatAncestorClip = bFlattenedOnCanvas;
	LastPaintState = Element ? Element->PaintState : 0;
	RetailElement = Element;
	BitmapScale = Scale.ComponentMax(FVector2D(KINDA_SMALL_NUMBER, KINDA_SMALL_NUMBER));
	NativeWidth = InNativeWidth;
	if (bChanged) InvalidateLayoutAndVolatility();
}

UTexture2D* UACERetailTextBlock::GetGlyphAtlas(bool bBackground) const
{
	return bBackground ? BackgroundAtlas.Get() : ForegroundAtlas.Get();
}

TSharedRef<SWidget> UACERetailTextBlock::RebuildWidget()
{
	if (!ForegroundAtlas)
	{
		if (auto* Canvas = GetTypedOuter<UACEUICanvasWidget>())
			SetRetailElement(Canvas->GetResourceResolver(), RetailElement.Pin(), BitmapScale, NativeWidth, bApplyDatAncestorClip);
		else if (const UWorld* World = GetWorld())
			if (const UGameInstance* GI = World->GetGameInstance())
				if (auto* Dat = GI->GetSubsystem<UACEDatSubsystem>())
					SetRetailElement(Dat->GetUiResources(), RetailElement.Pin(), BitmapScale, NativeWidth, bApplyDatAncestorClip);
	}
	MyTextBlock = SNew(SACERetailTextBlock, this);
	return MyTextBlock.ToSharedRef();
}
