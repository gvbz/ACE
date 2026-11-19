#include "Sky/ACELegionsSkyRenderer.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "TextureResource.h"

namespace
{
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
	CloudShadowOpacity = 0.f;
	MoonShape = {};
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
	Imported.Images.Empty();
	CloudMask.Reset(Textures.FindRef(Imported.CloudMask));
	CloudMotion = Imported.CloudMotion;
	Sky = MoveTemp(Imported);
	for (auto& L : Sky.Layers)
	{
		if (L.Setup == MoonSetup)
			for (const auto& Section : L.Sections)
				if (!Section.Additive)
				{
					MoonShape = ACESkyMotion::FBodyShape::Measure(L.Vertices,Section.Indices);
					break;
				}
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
	CloudShadowOpacity = 0.f;
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
	// CDayDesc::GetWorldFog supplies the authored fog RGB independently of
	// each sky object's tint. LScapeSky::Draw uses fog_sky only as the alpha
	// of its sky quad; neither the dome/ramp hue nor cloud fill recolors
	// distance fog. Preserve that separation when adapting AC1 weather.
	SkyFogColor = DisplayColor(Haze);
	FogColor = SkyFogColor;
	FogColor.A = 1.f;
	// Keep the readability adjustment on the storm cloud artwork only.
	const float NightScatter = Cover * (1.f - FMath::SmoothStep(.06f,.35f,Haze.GetLuminance()));
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
		if (L.Setup == CloudSheetSetup)
		{
			// The two-stage cloud material can double its texture alpha before
			// saturation. Its constant material alpha still bounds coverage; a
			// completely transparent material must never leave a ground shadow.
			float MaterialOpacity = 0.f;
			for (const auto& Section : L.Sections)
				MaterialOpacity = FMath::Max(MaterialOpacity,FMath::Clamp(Section.Tint.A * (Section.StageOps.A > .5f ? 2.f : 1.f),0.f,1.f));
			CloudShadowOpacity = FMath::Max(CloudShadowOpacity,FMath::Clamp(Color.A,0.f,1.f)*MaterialOpacity);
		}
		const float Span = L.End - L.Begin + (L.End < L.Begin ? 1.f : 0.f);
		// Celestial lifetime comes from AC1 too. Using palette time here would
		// extrapolate the native moon's roll outside its shorter visibility window
		// and make it snap as that unrelated window wraps around midnight.
		const float Phase = Body ? Body->OrbitPhase
			: Span > 0 ? (Time - L.Begin + (Time < L.Begin ? 1.f : 0.f)) / Span : 0;
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
		if (L.Setup == MoonSetup && !MoonShape.Center.IsNearlyZero())
		{
			// Keep the native orbital roll: a zero-roll look-at turns this artwork
			// upside down early in the night. Transport its authored frame to AC1's
			// bearing, aiming the solid disc rather than the offset glow bounds.
			const FVector NativeDirection = Rotation.RotateVector(MoonShape.Center).GetSafeNormal();
			const FQuat Aim = FQuat::FindBetweenNormals(NativeDirection,Body->Direction) * Rotation;
			const double Scale = Body->TanHalfAngle > 0. && MoonShape.TanHalfAngle > 0.
				? Body->TanHalfAngle / MoonShape.TanHalfAngle : 1.;
			Mesh->SetRelativeRotation(Aim);
			Mesh->SetRelativeScale3D(FVector(Scale));
			// Scaling about the disc keeps its bearing and sky depth fixed, and
			// scales the native halo in proportion instead of leaving a huge glow.
			Mesh->SetRelativeLocation(Aim.RotateVector(MoonShape.Center*(1.-Scale)));
		}
		else
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
