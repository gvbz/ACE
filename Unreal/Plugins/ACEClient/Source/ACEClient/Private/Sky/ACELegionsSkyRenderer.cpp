#include "Sky/ACELegionsSkyRenderer.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "TextureResource.h"

namespace
{
constexpr uint32 SkyDomeName = 0x80000001;
constexpr uint32 HorizonRampName = 0x80000004;

// The imported horizon matte is a one-column opacity ramp wrapped around
// an azimuthally symmetric mesh. Interpolate its UV at zero elevation.
float HorizonCoverage(const ACELegionsSky::FBackdrop& Sky)
{
	const auto* Ramp = Sky.Layers.FindByPredicate([](const auto& L){ return L.Name == HorizonRampName; });
	if (!Ramp || Ramp->Sections.IsEmpty()) return 0.f;
	const auto& Section = Ramp->Sections[0];
	const auto* Image = Sky.Images.Find(Section.Texture);
	if (!Image || Image->Width != 1 || Image->Mips.IsEmpty()) return 0.f;
	const auto* Rows = reinterpret_cast<const FColor*>(Image->Mips[0].GetData());
	for (int32 I=0; I<Section.Indices.Num(); ++I)
	{
		const int32 A = Section.Indices[I], B = Section.Indices[I%3==2 ? I-2 : I+1];
		const double Low = Ramp->Vertices[A].Z, High = Ramp->Vertices[B].Z;
		if (Low <= 0. && High > 0.)
		{
			const double V = FMath::Lerp(Ramp->UVs[A].Y,Ramp->UVs[B].Y,-Low/(High-Low));
			const int32 Row = FMath::Clamp(FMath::FloorToInt(V*Image->Height),0,Image->Height-1);
			return Rows[Row].A / 255.f * Section.Tint.A;
		}
	}
	return 0.f;
}
constexpr uint32 SunSetup = 0x04000023;
constexpr uint32 MoonSetup = 0x040000b6;
constexpr uint32 AsteroidBeltSetup = 0x04000729;
constexpr uint32 CloudSheetSetup = 0x04000089;
}

void FACELegionsSkyRenderer::Clear()
{
	for (auto& Weak : Meshes)
		if (auto* Mesh = Weak.Get())
		{
			Mesh->GetOwner()->RemoveInstanceComponent(Mesh);
			Mesh->DestroyComponent();
		}
	Meshes.Reset();
	CloudBases.Reset();
	CloudMask.Reset();
	Sky = {};
}

bool FACELegionsSkyRenderer::Build(AActor& Owner, ACELegionsSky::FBackdrop&& Imported)
{
	Clear();
	auto* Alpha = MaterialParent(false);
	auto* Add = MaterialParent(true);
	if (!Alpha || !Add)
		return false;
	TMap<uint32, UTexture2D*> Textures;
	for (const auto& Pair : Imported.Images)
	{
		const auto& Image = Pair.Value;
		auto* Tex = UTexture2D::CreateTransient(Image.Width, Image.Height, Image.Format);
		if (!Tex)
			return false;
		Tex->SRGB = true;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->Filter = TF_Trilinear;
		Tex->NeverStream = true;
		auto& Mips = Tex->GetPlatformData()->Mips;
		for (int32 I = 0; I < Image.Mips.Num(); ++I)
		{
			if (I > 0)
				Mips.Add(new FTexture2DMipMap());
			auto& Mip = Mips[I];
			Mip.SizeX = FMath::Max(1, Image.Width >> I);
			Mip.SizeY = FMath::Max(1, Image.Height >> I);
			const auto& Pixels = Image.Mips[I];
			const int64 Bytes = Pixels.Num();
			Mip.BulkData.Lock(LOCK_READ_WRITE);
			FMemory::Memcpy(Mip.BulkData.Realloc(Bytes), Pixels.GetData(), Bytes);
			Mip.BulkData.Unlock();
		}
		Tex->UpdateResource();
		Textures.Add(Pair.Key, Tex);
	}
	RampCoverage = HorizonCoverage(Imported);
	Imported.Images.Empty();
	CloudMask.Reset(Textures.FindRef(Imported.CloudMask));
	CloudMotion = Imported.CloudMotion;
	Sky = MoveTemp(Imported);
	for (auto& L : Sky.Layers)
	{
		auto& Basis = CloudBases.AddDefaulted_GetRef();
		if (L.Setup == CloudSheetSetup)
			for (const auto& S : L.Sections) Basis.Consider(L.Vertices,L.UVs,S.Indices);
		auto* Mesh = NewObject<UProceduralMeshComponent>(&Owner);
		Owner.AddInstanceComponent(Mesh);
		Mesh->SetupAttachment(Owner.GetRootComponent());
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetGenerateOverlapEvents(false);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->SetReceivesDecals(false);
		Mesh->bUseAsOccluder = false;
		Mesh->bNeverDistanceCull = true;
		// Leave room between the imported orbs and clouds for AC1's second moon.
		Mesh->SetTranslucentSortPriority(-2500 + 2 * Meshes.Num());
		for (int32 I = 0; I < L.Sections.Num(); ++I)
		{
			auto& S = L.Sections[I];
			Mesh->CreateMeshSection(I, L.Vertices, S.Indices, {}, L.UVs, {}, {}, false);
			auto* Mid = UMaterialInstanceDynamic::Create(S.Additive ? Add : Alpha, Mesh);
			Mid->SetTextureParameterValue(TEXT("ACETexture"), Textures.FindChecked(S.Texture));
			Mid->SetVectorParameterValue(TEXT("MaterialFactor"), S.Tint);
			Mid->SetVectorParameterValue(TEXT("StageOps"), S.StageOps);
			Mid->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(S.Offset[0].X,S.Offset[0].Y,S.Offset[1].X,S.Offset[1].Y));
			// CPU supplies wrapped offsets from the synchronized sky clock.
			Mid->SetVectorParameterValue(TEXT("UVVelocity"), FLinearColor::Transparent);
			Mesh->SetMaterial(I, Mid);
			S.Indices.Empty();
		}
		L.Vertices.Empty();
		L.UVs.Empty();
		Mesh->RegisterComponent();
		Meshes.Add(Mesh);
	}
	Update(.5f,{});
	Noon = Current;
	return !Meshes.IsEmpty();
}

void FACELegionsSkyRenderer::SetVisible(bool Visible)
{
	for (auto& Weak : Meshes)
		if (auto* Mesh = Weak.Get())
			Mesh->SetHiddenInGame(!Visible);
}

FLinearColor FACELegionsSkyRenderer::Update(float Time, const FACELegionsSkyFrame& Frame)
{
	CloudMotion = Sky.CloudMotion;
	if (!Frame.CloudDirection.IsNearlyZero())
	{
		const float Speed = FVector2D(Sky.CloudMotion.G,Sky.CloudMotion.B).Size();
		CloudMotion.G = -Frame.CloudDirection.X * Speed;
		CloudMotion.B = -Frame.CloudDirection.Y * Speed;
	}
	// Bracket the current time between native wet keys on the circular AC2
	// day. AC1 controls storm strength independently of those original hours.
	struct FRainAnchor
	{
		const ACELegionsSky::FRainDay* Day = nullptr;
		const ACELegionsSky::FKey* Wet = nullptr;
		float Distance = 2.f;

		void Consider(const ACELegionsSky::FRainDay& Candidate, const ACELegionsSky::FKey& Key, float Delta)
		{
			if (Delta < 0.f) Delta += 1.f;
			if (Delta < Distance)
			{
				Day = &Candidate;
				Wet = &Key;
				Distance = Delta;
			}
		}
	};
	FRainAnchor Previous, Following;
	const float Cover = FMath::Max(Frame.Overcast, Frame.Rain);
	if (Cover > 0.f)
		for (const auto& Day : Sky.RainDays)
			for (const auto& Key : Day.Keys)
				if (Key.Rain == Day.Keys[Day.Wet].Rain)
				{
					Previous.Consider(Day,Key,Time-Key.Time);
					Following.Consider(Day,Key,Key.Time-Time);
				}
	const auto* RainDay = Previous.Distance < Following.Distance ? Previous.Day : Following.Day;
	const auto& Keys = RainDay ? RainDay->Keys : Sky.Keys;
	auto Bracket = [Time](const TArray<ACELegionsSky::FKey>& Schedule)
	{
		int32 Next = 1;
		while (Next + 1 < Schedule.Num() && Time > Schedule[Next].Time)
			++Next;
		return Next;
	};
	const int32 Next = Bracket(Keys);
	const auto& A = Keys[Next - 1];
	const auto& B = Keys[Next];
	const float T = (Time - A.Time) / (B.Time - A.Time);
	auto DisplayColor = [](const FLinearColor& C) { return FLinearColor::FromSRGBColor(C.ToFColor(false)); };
	Haze = FLinearColor::LerpUsingHSV(A.Haze, B.Haze, T);
	FogMin = FMath::Lerp(A.FogMin,B.FogMin,T);
	FogMax = FMath::Lerp(A.FogMax,B.FogMax,T);
	Current.DirectColor = DisplayColor(FLinearColor::LerpUsingHSV(A.DirectColor,B.DirectColor,T));
	Current.AmbientColor = DisplayColor(FLinearColor::LerpUsingHSV(A.AmbientColor,B.AmbientColor,T));
	Current.Direct = FMath::Lerp(A.Direct,B.Direct,T);
	Current.Ambient = FMath::Lerp(A.Ambient,B.Ambient,T);
	if (RainDay)
	{
		// Scale the native approach-to-rain and peak fog by the AC1 cloud
		// and precipitation envelopes. A rainless gap in AC1 stays clear
		// even if the AC2 day would have scheduled a shower at this time.
		const int32 DryNext = Bracket(Sky.Keys);
		const auto& DryA = Sky.Keys[DryNext - 1];
		const auto& DryB = Sky.Keys[DryNext];
		const float DryT = (Time - DryA.Time) / (DryB.Time - DryA.Time);
		const float WetFraction = Frame.Rain / Cover;
		const float Span = Previous.Distance + Following.Distance;
		const float Phase = Span > 0.f ? Previous.Distance / Span : 0.f;
		const auto& CloudA = Previous.Day->Keys[Previous.Day->Cloudy];
		const auto& CloudB = Following.Day->Keys[Following.Day->Cloudy];
		const auto DryColor = FLinearColor::LerpUsingHSV(DryA.Haze, DryB.Haze, DryT);
		const auto CloudColor = FLinearColor::LerpUsingHSV(CloudA.Haze, CloudB.Haze, Phase);
		const auto RainColor = FLinearColor::LerpUsingHSV(Previous.Wet->Haze, Following.Wet->Haze, Phase);
		const auto StormColor = FLinearColor::LerpUsingHSV(CloudColor,RainColor,WetFraction);
		// Keep night storms dark when the server schedules rain outside the
		// source episode. Use its authored hue at the local palette brightness.
		const float Brightness = FMath::Min(1.f, Haze.GetLuminance() / FMath::Max(.001f, StormColor.GetLuminance()));
		Haze = FMath::Lerp(DryColor, StormColor * Brightness, Cover);
		Haze.A = FMath::Lerp(DryColor.A, StormColor.A, Cover);
		auto FogDistance = [&](float ACELegionsSky::FKey::*Field)
		{
			const float Dry = FMath::Lerp(DryA.*Field,DryB.*Field,DryT);
			const float Cloudy = FMath::Lerp(CloudA.*Field,CloudB.*Field,Phase);
			const float Wet = FMath::Lerp(Previous.Wet->*Field,Following.Wet->*Field,Phase);
			return FMath::Lerp(Dry,FMath::Lerp(Cloudy,Wet,WetFraction),Cover);
		};
		FogMin = FogDistance(&ACELegionsSky::FKey::FogMin);
		FogMax = FogDistance(&ACELegionsSky::FKey::FogMax);
		Current.DirectColor = FMath::Lerp(DisplayColor(FLinearColor::LerpUsingHSV(DryA.DirectColor,DryB.DirectColor,DryT)),Current.DirectColor,Cover);
		Current.AmbientColor = FMath::Lerp(DisplayColor(FLinearColor::LerpUsingHSV(DryA.AmbientColor,DryB.AmbientColor,DryT)),Current.AmbientColor,Cover);
		Current.Direct = FMath::Lerp(FMath::Lerp(DryA.Direct,DryB.Direct,DryT), Current.Direct, Cover);
		Current.Ambient = FMath::Lerp(FMath::Lerp(DryA.Ambient,DryB.Ambient,DryT), Current.Ambient, Cover);
	}
	// Night rain still has skylight scattered by the cloud deck. Use the
	// local AC1 weather colour for that floor, with the native haze alpha.
	const float NightScatter = Cover * (1.f - FMath::SmoothStep(.06f,.35f,Haze.GetLuminance()));
	FogColor = DisplayColor(Haze);
	if (RainDay)
	{
		const FLinearColor Scattered = Frame.WeatherFogColor;
		FogColor.R = FMath::Lerp(FogColor.R,FMath::Max(FogColor.R,Scattered.R),NightScatter);
		FogColor.G = FMath::Lerp(FogColor.G,FMath::Max(FogColor.G,Scattered.G),NightScatter);
		FogColor.B = FMath::Lerp(FogColor.B,FMath::Max(FogColor.B,Scattered.B),NightScatter);
	}
	SkyFogColor = FogColor;
	// A clear noon key stores neutral grey *with zero sky-fog opacity*.
	// At the terrain limit, use the hue of the visible sky dome for that
	// neutral component. Keep the authored extinction luminance and the
	// coloured sunset/opaque storm keys.
	const auto* DomeA = A.Objects.Find(SkyDomeName);
	const auto* DomeB = B.Objects.Find(SkyDomeName);
	if (DomeA && DomeB)
	{
		FLinearColor Equator = DisplayColor(FLinearColor::LerpUsingHSV(DomeA->Tint,DomeB->Tint,T));
		const auto* RimA = A.Objects.Find(HorizonRampName);
		const auto* RimB = B.Objects.Find(HorizonRampName);
		if (RimA && RimB)
		{
			const auto Rim = FLinearColor::LerpUsingHSV(RimA->Tint,RimB->Tint,T);
			Equator = FMath::Lerp(Equator,DisplayColor(Rim),Rim.A * RampCoverage);
		}
		const float Chroma = FMath::Max3(Haze.R,Haze.G,Haze.B) - FMath::Min3(Haze.R,Haze.G,Haze.B);
		const float VisibleDome = (1.f-Haze.A) * (1.f-Cover) *
			(1.f-FMath::SmoothStep(.02f,.24f,Chroma)) * FMath::SmoothStep(.08f,.4f,Haze.GetLuminance());
		const float EquatorLum = Equator.GetLuminance();
		if (EquatorLum > .001f)
			FogColor = FMath::Lerp(FogColor,Equator * (FogColor.GetLuminance()/EquatorLum),VisibleDome);
	}
	if (!HasRainDays())
	{
		// Older caches have just the clear day. Keep their AC1 storm layer
		// and lighting until the player reimports the weather records.
		FogColor = FMath::Lerp(FogColor,Frame.WeatherFogColor,Frame.Overcast);
		SkyFogColor = FMath::Lerp(SkyFogColor,Frame.WeatherFogColor,Frame.Overcast);
		FogMin = FMath::Lerp(FogMin,Frame.WeatherFogMin,Frame.Overcast);
		FogMax = FMath::Lerp(FogMax,Frame.WeatherFogMax,Frame.Overcast);
		Current.Direct = FMath::Lerp(Current.Direct,Frame.WeatherDirect,Frame.Overcast);
	}
	// The horizon stays bright near sunset even as the AC1 sun card fades.
	// Use the native palette's display luminance for that transition.
	const float SkyBrightness = Haze.GetLuminance();
	ClearDaylight = Frame.bClearForecast
		? FMath::SmoothStep(.04f, .45f, SkyBrightness) * (1.f - Cover) : 0.f;
	Current.Direction = Frame.LightDirection;
	const ACELegionsSky::FKey::FOverride Default{0, 0, FLinearColor::White};
	for (int32 I = 0; I < Meshes.Num(); ++I)
	{
		auto* Mesh = Meshes[I].Get();
		if (!Mesh)
			continue;
		const auto& L = Sky.Layers[I];
		const bool Up = L.Begin == L.End ||
		                (L.Begin < L.End ? Time >= L.Begin && Time < L.End : Time >= L.Begin || Time < L.End);
		const auto* First = A.Objects.Find(L.Name);
		const auto* Second = B.Objects.Find(L.Name);
		const auto& Before = First ? *First : Default;
		FLinearColor Color = FLinearColor::LerpUsingHSV(Before.Tint, Second ? Second->Tint : Before.Tint, T);
		const auto* Body = L.Setup == SunSetup ? &Frame.Sun : L.Setup == MoonSetup ? &Frame.Moon : nullptr;
		if (Body)
			Color.A = Body->Opacity;
		// A fully present AC1 clear-weather card represents scattered clouds.
		// Allow light through the opaque regions of the imported cloud sheet.
		if (L.Setup == CloudSheetSetup)
		{
			// The haze obscures the celestial layers. A storm cloud is in front
			// of that haze, even at hours when the clear-day cloud sheet sleeps.
			const bool StormSheet = HasRainDays() && Cover > 0.f;
			const int32 Order = StormSheet ? -2399 : -2500 + 2 * I;
			if (Mesh->TranslucencySortPriority != Order) Mesh->SetTranslucentSortPriority(Order);
			if (StormSheet)
			{
				// At night the authored fog approaches black. Keep enough scattered
				// light on the sheet for the moving texture to remain distinguishable.
				const FLinearColor CloudLight = FMath::Lerp(FLinearColor(.16f,.18f,.21f),
					FLinearColor(.36f,.40f,.48f), NightScatter);
				FLinearColor StormTint = Haze * .65f + CloudLight;
				StormTint.A = 1.f;
				Color = FMath::Lerp(Color,StormTint,Cover);
			}
			Color.A *= Frame.CloudOpacity * FMath::Lerp(1.f, .60f, ClearDaylight);
		}
		const bool Visible = (Body || Up || (L.Setup == CloudSheetSetup && HasRainDays() && Cover > 0.f)) && Color.A > KINDA_SMALL_NUMBER;
		Mesh->SetVisibility(Visible);
		if (!Visible)
			continue;
		const float Span = L.End - L.Begin + (L.End < L.Begin ? 1.f : 0.f);
		const float Phase = Span > 0 ? (Time - L.Begin + (Time < L.Begin ? 1.f : 0.f)) / Span : 0;
		auto Sweep = [&](float From, float To) {
			if (!L.West && To < From)
				From -= 360;
			if (L.West && From < To)
				To += 360;
			return FMath::Lerp(From, To, Phase);
		};
		const float Pitch = Before.Pitch + Sweep(L.BeginPitch, L.EndPitch);
		const float Heading = Before.Heading + Sweep(L.BeginHeading, L.EndHeading);
		const FQuat Rotation = FQuat(FVector::YAxisVector, -FMath::DegreesToRadians(Pitch)) *
		                       FQuat(FVector::UpVector, -FMath::DegreesToRadians(Heading));
		// Both formats store off-center geometry. Aim the center of the AC2
		// artwork at the AC1 card, keeping the texture upright at the horizon.
		Mesh->SetRelativeRotation(Body
			? Body->Direction.Rotation().Quaternion() * L.Center.Rotation().Quaternion().Inverse()
			: Rotation);
		auto Tint = DisplayColor(Color);
		// The source sun is an additive LDR flare. Give its emissive pixels
		// the directional light's intensity so it reads through the daytime clouds.
		if (L.Setup == SunSetup)
			Tint *= FMath::Max(1.f, Current.SunIntensity());
		Tint.A = Color.A;
		for (int32 J = 0; J < L.Sections.Num(); ++J)
		{
			auto* Mid = CastChecked<UMaterialInstanceDynamic>(Mesh->GetMaterial(J));
			const auto& Section = L.Sections[J];
			FVector2D Offset[2];
			for (int32 Stage = 0; Stage < 2; ++Stage)
			{
				auto Velocity = Section.Velocity[Stage];
				if (L.Setup == CloudSheetSetup && !Frame.CloudDirection.IsNearlyZero())
					Velocity = CloudBases[I].Along(Rotation.UnrotateVector(Frame.CloudDirection),Velocity.Size());
				Offset[Stage] = FVector2D(ACESkyMotion::Offset(Frame.Seconds,Velocity.X,Section.Offset[Stage].X),
					ACESkyMotion::Offset(Frame.Seconds,Velocity.Y,Section.Offset[Stage].Y));
			}
			Mid->SetVectorParameterValue(TEXT("UVOffset"),FLinearColor(Offset[0].X,Offset[0].Y,Offset[1].X,Offset[1].Y));
			Mid->SetVectorParameterValue(TEXT("SkyTint"), Tint);
			if (L.Setup == SunSetup && L.Sections[J].Additive)
				Mid->SetScalarParameterValue(TEXT("SunHorizonGlow"),
					ClearDaylight * (1.f - FMath::SmoothStep(0.f,.65f,float(Frame.Sun.Direction.Z))));
			if (L.Setup == SunSetup || L.Setup == AsteroidBeltSetup)
				Mid->SetScalarParameterValue(TEXT("DaylightBackdrop"), ClearDaylight * SkyBrightness);
		}
	}
	for (const auto* Body : {&Frame.Sun, &Frame.Moon})
		if (Body->Opacity > 0.f && Body->Direction.Z > 0.)
		{
			Current.Direction = -Body->Direction;
			break;
		}
	return Haze;
}
