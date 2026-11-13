#pragma once

#include "CoreMinimal.h"
#include "ACEOpcodes.h"
#include "ACETypes.generated.h"

UENUM(BlueprintType)
enum class EACESessionState : uint8
{
	Disconnected UMETA(DisplayName = "Disconnected"),
	Connecting UMETA(DisplayName = "Connecting"),
	AwaitConnectRequest UMETA(DisplayName = "Await Connect Request"),
	AwaitCharacterList UMETA(DisplayName = "Await Character List"),
	CharacterSelect UMETA(DisplayName = "Character Select"),
	EnteringWorld UMETA(DisplayName = "Entering World"),
	InWorld UMETA(DisplayName = "In World"),
	Failed UMETA(DisplayName = "Failed")
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACECharacterInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 CharacterId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 DeleteSeconds = 0;
};

/** Asheron's Call landblock cell + local origin + quaternion (WXYZ). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEPosition
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	int32 CellId = 0;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	FVector Location = FVector::ZeroVector;

	/** AC quaternion stored as (X,Y,Z) + W separately for clarity. */
	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	float RotationW = 1.f;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	FVector RotationXYZ = FVector::ZeroVector;

	/** Physics velocity from PositionPack (ACE space units/sec). Valid when bHasVelocity. */
	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	bool bHasVelocity = false;

	/** PositionFlags.IsGrounded (0x04) — contact with walkable surface. */
	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	bool bIsGrounded = false;

	// Local metadata from F748, not additional wire fields. CreateObject positions
	// have no contact bit; distinguish them from an explicitly airborne update.
	bool bHasContactState = false;
	bool bHasTeleportSequence = false;
	uint16 TeleportSequence = 0;

	bool IsValid() const { return CellId != 0; }

	/**
	 * AC is right-handed Z-up; Unreal is left-handed Z-up. Negating X (East) converts
	 * handedness so that when facing North (+Y), East stays on screen-right — matching retail.
	 * Copying XYZ 1:1 mirrors the whole town left↔right.
	 */
	static FVector AceVectorToUnreal(const FVector& Ac, float Scale = 1.f)
	{
		return FVector(-Ac.X * Scale, Ac.Y * Scale, Ac.Z * Scale);
	}

	/** Outdoor landblock is 192 x 192 game units. Cell high bytes → landblock origin + local. */
	FVector ToUnrealLocation(float Scale = 100.f) const
	{
		const uint32 Cell = static_cast<uint32>(CellId);
		const uint32 LandblockX = (Cell >> 24) & 0xFF;
		const uint32 LandblockY = (Cell >> 16) & 0xFF;
		// Retail retains cell-local coordinates. UE uses a world-space FVector,
		// so keep its double precision until the block origin is removed again.
		// A float global intermediate rounds away sub-centimetre wall slides and
		// can put a doorway sample on the opposite side of its portal plane.
		const double GlobalX = (LandblockX * 192.) + Location.X;
		const double GlobalY = (LandblockY * 192.) + Location.Y;
		return AceVectorToUnreal(FVector(GlobalX, GlobalY, Location.Z), Scale);
	}

	/**
	 * When outdoor local XY leaves [0,192), bump the landblock CellId and wrap Location.
	 * Indoor cells ((CellId & 0xFFFF) >= 0x100) are left alone — EnvCell transitions are
	 * handled by portal classification on the client.
	 */
	void NormalizeOutdoorLandblock()
	{
		uint32 Cell = static_cast<uint32>(CellId);
		if ((Cell & 0xFFFFu) >= 0x0100u)
		{
			return;
		}

		int32 Lbx = static_cast<int32>((Cell >> 24) & 0xFF);
		int32 Lby = static_cast<int32>((Cell >> 16) & 0xFF);

		while (Location.X >= 192.f && Lbx < 255)
		{
			Location.X -= 192.f;
			++Lbx;
		}
		while (Location.X < 0.f && Lbx > 0)
		{
			Location.X += 192.f;
			--Lbx;
		}
		while (Location.Y >= 192.f && Lby < 255)
		{
			Location.Y -= 192.f;
			++Lby;
		}
		while (Location.Y < 0.f && Lby > 0)
		{
			Location.Y += 192.f;
			--Lby;
		}

		Location.X = FMath::Clamp(Location.X, 0.f, 191.999f);
		Location.Y = FMath::Clamp(Location.Y, 0.f, 191.999f);
		// Outdoor landcells are 8×8 of 24u squares; ACE LandCell id = CY + 8*CX + 1.
		const int32 CX = FMath::Clamp(FMath::FloorToInt(Location.X / 24.f), 0, 7);
		const int32 CY = FMath::Clamp(FMath::FloorToInt(Location.Y / 24.f), 0, 7);
		const uint32 Low = static_cast<uint32>(CY + 8 * CX + 1);
		CellId = static_cast<int32>((static_cast<uint32>(Lbx) << 24) | (static_cast<uint32>(Lby) << 16) | Low);
	}

	/** Inverse of ToUnrealLocation; updates CellId when crossing outdoor landblock borders. */
	void SetLocationFromUnreal(const FVector& UnrealLoc, float Scale = 100.f)
	{
		if (Scale <= KINDA_SMALL_NUMBER)
		{
			return;
		}
		const uint32 Cell = static_cast<uint32>(CellId);
		const uint32 LandblockX = (Cell >> 24) & 0xFF;
		const uint32 LandblockY = (Cell >> 16) & 0xFF;
		const double GlobalX = -UnrealLoc.X / Scale;
		const double GlobalY = UnrealLoc.Y / Scale;
		const double GlobalZ = UnrealLoc.Z / Scale;
		Location.X = GlobalX - LandblockX * 192.;
		Location.Y = GlobalY - LandblockY * 192.;
		Location.Z = GlobalZ;
		NormalizeOutdoorLandblock();
	}

	/**
	 * AC quaternion → Unreal actor rotation (packet order WXYZ).
	 * With AceVectorToUnreal (flip X): R' = S R S → quat (x, -y, -z, w).
	 * AC facing is local +Y — use GetAceForwardVector(), not GetForwardVector().
	 */
	FQuat ToUnrealQuat() const
	{
		return AceQuatToUnreal(RotationW, RotationXYZ);
	}

	/** AC WXYZ → Unreal quat after RH→LH (flip X). Shared by actors and DAT Frames. */
	static FQuat AceQuatToUnreal(float W, const FVector& XYZ)
	{
		return FQuat(XYZ.X, -XYZ.Y, -XYZ.Z, W).GetNormalized();
	}

	static FQuat AceQuatToUnreal(const FQuat& Ac)
	{
		return AceQuatToUnreal(Ac.W, FVector(Ac.X, Ac.Y, Ac.Z));
	}

	/**
	 * Retail PartArray.UpdateParts / AFrame.Combine(parent, child, scale).
	 * Unreal composition: Parent * Child (child in parent space).
	 */
	static FTransform CombineAceFrames(const FTransform& Parent, const FTransform& Child, const FVector& Scale = FVector::OneVector)
	{
		const FVector ScaledChildLoc(
			Child.GetLocation().X * Scale.X,
			Child.GetLocation().Y * Scale.Y,
			Child.GetLocation().Z * Scale.Z);
		const FVector Loc = Parent.GetLocation() + Parent.GetRotation().RotateVector(ScaledChildLoc);
		const FQuat Rot = (Parent.GetRotation() * Child.GetRotation()).GetNormalized();
		const FVector ChildS = Child.GetScale3D();
		const FVector ParentS = Parent.GetScale3D();
		return FTransform(Rot, Loc, FVector(ParentS.X * ChildS.X, ParentS.Y * ChildS.Y, ParentS.Z * ChildS.Z));
	}

	/** Raw AC quat (WXYZ) — no RH→LH. Use for protocol / Location updates. */
	FQuat GetAcQuat() const
	{
		return FQuat(RotationXYZ.X, RotationXYZ.Y, RotationXYZ.Z, RotationW).GetNormalized();
	}

	/**
	 * AC-space facing (+Y) and strafe-right (+X). Use when updating FACEPosition.Location
	 * (still in Asheron's Call units, before AceVectorToUnreal).
	 */
	FVector GetAceForwardInAcSpace() const
	{
		return GetAcQuat().RotateVector(FVector::YAxisVector);
	}

	FVector GetAceRightInAcSpace() const
	{
		return GetAcQuat().RotateVector(FVector::XAxisVector);
	}

	/** Same axes expressed in Unreal world space (after AceQuatToUnreal). */
	FVector GetAceForwardVector() const
	{
		return ToUnrealQuat().RotateVector(FVector::YAxisVector);
	}

	FVector GetAceRightVector() const
	{
		// AC +X maps to UE -X under AceVectorToUnreal.
		return ToUnrealQuat().RotateVector(FVector(-1.f, 0.f, 0.f));
	}

	/** AFrame heading: clockwise from AC +Y, in [0, 360) degrees. */
	float GetAceHeadingDegrees() const
	{
		const FVector Dir = GetAceForwardInAcSpace();
		return FMath::Fmod(FMath::RadiansToDegrees(FMath::Atan2(Dir.X, Dir.Y)) + 360.f, 360.f);
	}

	/**
	 * Wire DesiredHeading (AFrame degrees 0–360) → Unreal XY travel direction.
	 * AFrame.set_heading uses AC forward (sin θ, cos θ); AceVectorToUnreal flips X.
	 */
	static FVector UnrealDirFromAceHeadingDegrees(float AceHeadingDegrees)
	{
		const float Rad = FMath::DegreesToRadians(AceHeadingDegrees);
		return FVector(-FMath::Sin(Rad), FMath::Cos(Rad), 0.f);
	}

	/**
	 * Point AC local +Y at a Unreal-world XY direction (Z ignored).
	 * Used for Use-approach facing — Turn-axis integration alone is unreliable across RH→LH.
	 */
	void SetAceFacingFromUnrealDir2D(const FVector& UnrealDir)
	{
		FVector AcDir(-UnrealDir.X, UnrealDir.Y, 0.f);
		if (!AcDir.Normalize())
		{
			return;
		}
		// Yaw about AC +Z that takes local +Y to AcDir.
		const float HeadingRad = FMath::Atan2(-AcDir.X, AcDir.Y);
		const FQuat Ac(FVector::UpVector, HeadingRad);
		RotationW = Ac.W;
		RotationXYZ = FVector(Ac.X, Ac.Y, Ac.Z);
	}

	/** Dot of current Unreal ACE-forward vs a Unreal XY target direction. */
	float GetFacingDotUnreal2D(const FVector& UnrealDir) const
	{
		const FVector Fwd = GetAceForwardVector().GetSafeNormal2D();
		const FVector Dir = UnrealDir.GetSafeNormal2D();
		if (Fwd.IsNearlyZero() || Dir.IsNearlyZero())
		{
			return 1.f;
		}
		return FVector::DotProduct(Fwd, Dir);
	}

	/** Actor quat that points AC local +Y along a Unreal XY travel direction (Z up). */
	static FQuat QuatFromUnrealTravelDir2D(const FVector& UnrealDir)
	{
		FACEPosition Tmp;
		Tmp.SetAceFacingFromUnrealDir2D(UnrealDir);
		return Tmp.ToUnrealQuat();
	}

	/** Normalize WXYZ to unit length (identity if degenerate). */
	void NormalizeRotation()
	{
		FVector4 V(RotationXYZ.X, RotationXYZ.Y, RotationXYZ.Z, RotationW);
		const float LenSq = V.SizeSquared();
		if (LenSq > KINDA_SMALL_NUMBER)
		{
			V *= FMath::InvSqrt(LenSq);
			RotationXYZ = FVector(V.X, V.Y, V.Z);
			RotationW = V.W;
		}
		else
		{
			RotationW = 1.f;
			RotationXYZ = FVector::ZeroVector;
		}
	}

	/**
	 * After reading a PositionPack: OrientationHasNo* means that component was exactly 0
	 * and omitted from the wire (ACE PositionPack). The reader already wrote 0 — do not
	 * invent a +sqrt reconstruction (that corrupts yaw near component zeros).
	 */
	void ReconstructOmittedRotation(uint32 /*PositionFlags*/)
	{
		NormalizeRotation();
	}
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACELoginCredentials
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	FString Host = TEXT("45.59.45.208");

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	int32 Port = 9000;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	FString Account;

	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	FString Password;

	/** GDLE uses NetAuthType::Account with username:password, ACE AccountPassword. */
	UPROPERTY(BlueprintReadWrite, Category = "ACE")
	bool bGDLE = false;
};

/** Sub-palette range from ObjectCreate ModelData (wire Offset/Length are ×8 units). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEObjDescSubPalette
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 SubPaletteId = 0;

	/** Color start index (already ×8 from wire byte). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Offset = 0;

	/** Color count (already ×8; wire 0 means 256*8). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 NumColors = 0;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEObjDescTextureChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	uint8 PartIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 OldTexture = 0; // SurfaceTexture 0x05

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 NewTexture = 0;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEObjDescAnimPartChange
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	uint8 PartIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 PartId = 0; // GfxObj 0x01
};

/** Server-resolved appearance (face + clothing) from ObjectCreate ModelData. */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEObjDesc
{
	GENERATED_BODY()
	/** Decal palette slots retain wire order before render-layer replacement. */
	TArray<FACEObjDescSubPalette> WireSubPalettes;
	bool bHasWireSubPalettes = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 PaletteBaseId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	TArray<FACEObjDescSubPalette> SubPalettes;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	TArray<FACEObjDescTextureChange> TextureChanges;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	TArray<FACEObjDescAnimPartChange> AnimPartChanges;

	bool HasVisualOverrides() const
	{
		return PaletteBaseId != 0 || SubPalettes.Num() > 0 || TextureChanges.Num() > 0 || AnimPartChanges.Num() > 0;
	}
	/** Geometry replacements do not recolor surfaces; texture changes are part-specific. */
	bool HasSurfaceOverrides(int32 PartIndex) const
	{
		return PaletteBaseId != 0 || !SubPalettes.IsEmpty()
			|| TextureChanges.ContainsByPredicate([PartIndex](const FACEObjDescTextureChange& Change)
				{ return static_cast<int32>(Change.PartIndex) == PartIndex; });
	}

	/** Stable fingerprint for skipping redundant ApplyWorldObject rebuilds (ObjDesc spam on wield). */
	uint64 GetContentHash() const
	{
		uint64 H = GetTypeHash(PaletteBaseId);
		H = HashCombine(H, GetTypeHash(SubPalettes.Num()));
		for (const FACEObjDescSubPalette& Sp : SubPalettes)
		{
			H = HashCombine(H, GetTypeHash(Sp.SubPaletteId));
			H = HashCombine(H, GetTypeHash(Sp.Offset));
			H = HashCombine(H, GetTypeHash(Sp.NumColors));
		}
		H = HashCombine(H, GetTypeHash(TextureChanges.Num()));
		for (const FACEObjDescTextureChange& T : TextureChanges)
		{
			H = HashCombine(H, GetTypeHash(T.PartIndex));
			H = HashCombine(H, GetTypeHash(T.OldTexture));
			H = HashCombine(H, GetTypeHash(T.NewTexture));
		}
		H = HashCombine(H, GetTypeHash(AnimPartChanges.Num()));
		for (const FACEObjDescAnimPartChange& Ap : AnimPartChanges)
		{
			H = HashCombine(H, GetTypeHash(Ap.PartIndex));
			H = HashCombine(H, GetTypeHash(Ap.PartId));
		}
		return H;
	}
};

/** Interpreted locomotion / object motion broadcast by UpdateMotion (0xF74C). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEObjectMotionState
{
	GENERATED_BODY()

	/** Signed input-like forward amount: positive forward, negative backward. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float Forward = 0.f;

	/** Signed input-like strafe amount: positive right, negative left. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float Strafe = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float Turn = 0.f;

	/** Authoritative interpreted forward velocity in AC units/second. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float ForwardUnitsPerSecond = 0.f;

	/** Authoritative interpreted sidestep velocity in AC units/second. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float StrafeUnitsPerSecond = 0.f;

	/**
	 * Motion-table playback multiplier from InterpretedState speeds.
	 * Walk is typically 1; run is GetRunRate() (often ~1.5–3+).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float AnimPlayRate = 1.f;

	/** True when the server selected a run cycle / run-rate move-to. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	bool bRunning = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	bool bMoving = false;

	/** Raw InterpretedState ForwardCommand (e.g. On=0x000B, Off=0x000C for doors). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	int32 ForwardCommand = 0;

	/**
	 * Current MotionStance (HandCombat / SwordCombat / NonCombat…).
	 * Packed as full MotionCommand (0x8000003C etc.); 0 = use MotionTable DefaultStyle.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	int32 CurrentStyle = 0;

	/**
	 * One-shot action from InterpretedState.Commands (attack / cast / emote).
	 * Full MotionCommand after Raw→Interpreted expand; 0 = none this update.
	 * When CommandList has multiple actions (FastTick multi-scarab windups), this is the
	 * FIRST action — follow-ups are in ActionFollowups so they play in order.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	int32 ActionCommand = 0;
	/** True only for an action with a newly accepted network sequence stamp. */
	bool bNewAction = false;

	/** Playback speed for ActionCommand (usually 1). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float ActionSpeed = 1.f;

	/** Remaining CommandList actions after ActionCommand (scarab windup chain). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	TArray<int32> ActionFollowups;
	/** Each MotionItem carries its own speed, including follow-up windups. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	TArray<float> ActionFollowupSpeeds;
	float FollowupSpeed(int32 Index) const { return ActionFollowupSpeeds.IsValidIndex(Index) ? ActionFollowupSpeeds[Index] : ActionSpeed; }

	/** MovementType from MovementData (0=interpreted, 6=MoveToObject, 7=MoveToPosition). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	uint8 MovementType = 0;

	/** Authoritative facing target carried by interpreted combat motion (StickToObject). */
	int32 StickyTargetGuid = 0;

	/** MoveToObject target GUID when MovementType==6. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	int32 MoveToTargetGuid = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float MoveToDistance = 0.6f;
	float MoveToMinDistance = 0.f;

	uint32 MoveToFlags = 0;
	float MoveToSpeed = 1.f;
	float MoveToRunRate = 1.f;
	float MoveToWalkRunThreshold = 1.f;

	float MoveToRemaining(float DistanceAc) const
	{
		// MovementParameters::get_command / towards_and_away: a negative
		// distance requests retreat, rather than treating an overlap as arrival.
		const bool Away = (MoveToFlags & 0x100u) != 0;
		if ((!Away || (MoveToFlags & 0x200u)) && DistanceAc > MoveToDistance)
			return DistanceAc - MoveToDistance;
		return Away && DistanceAc < MoveToMinDistance ? DistanceAc - MoveToMinDistance : 0.f;
	}

	// Retail MoveToManager::GetCurrentDistance / Position::CylinderDistance.
	// Inputs use one consistent unit (AC metres or Unreal centimetres).
	float ApproachDistance(const FVector& Offset, float SelfRadius, float SelfHeight,
		float TargetRadius, float TargetHeight) const
	{
		if ((MoveToFlags & 0x400u) == 0) return Offset.Size();
		const float Reach = Offset.Size() - SelfRadius - TargetRadius;
		const float HeightGap = Offset.Z >= 0.f ? Offset.Z - SelfHeight : -Offset.Z - TargetHeight;
		if (HeightGap > 0.f && Reach > 0.f) return FMath::Sqrt(HeightGap * HeightGap + Reach * Reach);
		if (HeightGap < 0.f && Reach < 0.f) return -FMath::Sqrt(HeightGap * HeightGap + Reach * Reach);
		return Reach;
	}

	void UpdateMoveToGait(float DistanceAc)
	{
		// Retail MovementParameters::get_command (0052AA00). RunRate is a
		// multiplier, not the gait selector; wandering creatures can only walk.
		bRunning = (MoveToFlags & 0x10u) != 0 || ((MoveToFlags & 2u) != 0
			&& ((MoveToFlags & 1u) == 0 || DistanceAc - MoveToDistance > MoveToWalkRunThreshold));
		AnimPlayRate = FMath::Abs(MoveToSpeed) * (bRunning ? FMath::Max(MoveToRunRate, 0.f) : 1.f);
		ForwardUnitsPerSecond = (bRunning ? 4.f : 3.1199999f) * AnimPlayRate;
		ForwardCommand = bRunning ? 0x0007 : 0x0005;
	}

	/** Authoritative MoveTo destination (ACE cell + local origin). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	int32 MoveToCellId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	FVector MoveToLocalAce = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	bool bHaveMoveToTarget = false;

	/** DesiredFacing from MovementParameters (degrees, AC heading). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float MoveToDesiredHeading = 0.f;

	/** FailDistance from MoveToParameters (AC units). Melee charge uses 15. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float MoveToFailDistance = 0.f;

	/** Turn speed for TurnToObject / TurnToHeading (MovementType 8/9). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE|Movement")
	float TurnToSpeed = 1.f;
	/** TurnToObject carries an absolute fallback heading separately from its offset. */
	float TurnToFallbackHeading = 0.f;
	uint32 TurnToFlags = 0;

	bool IsDeathMotion() const
	{
		return ACEMotion::NormalizeCommand(ActionCommand) == ACEMotion::Dead
			|| ACEMotion::NormalizeCommand(ForwardCommand) == ACEMotion::Dead;
	}
};

/** ACE.Entity.Enum.RadarColor — minimap blip tint. */
namespace ACERadarColor
{
	constexpr uint8 Default = 0x00;
	constexpr uint8 Blue = 0x01;
	constexpr uint8 Gold = 0x02;
	constexpr uint8 White = 0x03;
	constexpr uint8 Purple = 0x04;
	constexpr uint8 Red = 0x05;
	constexpr uint8 Pink = 0x06;
	constexpr uint8 Green = 0x07;
	constexpr uint8 Yellow = 0x08;
	constexpr uint8 Cyan = 0x09;
	constexpr uint8 BrightGreen = 0x10;
	constexpr uint8 Creature = Gold;
	constexpr uint8 LifeStone = Blue;
	constexpr uint8 NPC = Yellow;
	constexpr uint8 PlayerKiller = Red;
	constexpr uint8 Portal = Purple;
	constexpr uint8 Vendor = Yellow;
	constexpr uint8 PKLite = Pink;
}

/** ACE.Entity.Enum.RadarBehavior. */
namespace ACERadarBehavior
{
	constexpr uint8 Undefined = 0;
	constexpr uint8 ShowNever = 1;
	constexpr uint8 ShowMovement = 2;
	constexpr uint8 ShowAttacking = 3;
	constexpr uint8 ShowAlways = 4;
}

/** ACE ObjectDescriptionFlag bits we care about. */
namespace ACEObjectDescFlag
{
	constexpr int32 HiddenAdmin = 0x00000040;
	constexpr int32 UiHidden = 0x00000080;
	constexpr int32 Portal = 0x00040000;
	constexpr int32 Admin = 0x00100000;
	constexpr int32 FreePkStatus = 0x00200000;
	constexpr int32 ImmuneCellRestrictions = 0x00400000;
	constexpr int32 RequiresPackSlot = 0x00800000;
	constexpr int32 Retained = 0x01000000;
	constexpr int32 PkLiteStatus = 0x02000000;
	constexpr int32 IncludesSecondHeader = 0x04000000;
	constexpr int32 BindStone = 0x08000000;
	constexpr int32 VolatileRare = 0x10000000;
	/** Retail: double-click wields instead of Use when this flag is set. */
	constexpr int32 WieldOnUse = 0x20000000;
	constexpr int32 WieldLeft = 0x40000000;
	constexpr int32 LifeStone = 0x00004000;
	constexpr int32 Door = 0x00001000;
	constexpr int32 Corpse = 0x00002000;
	constexpr int32 Openable = 0x00000001;
	constexpr int32 Stuck = 0x00000004;
	constexpr int32 Attackable = 0x00000010;
	/** ObjectDescriptionFlag.PlayerKiller — PublicWeenieDesc bit for PK (not NPK / Free). */
	constexpr int32 PlayerKiller = 0x00000020;
	constexpr int32 Vendor = 0x00000200;
}

/** PropertyInt.PlayerKillerStatus bits used for pawn-pawn collision. */
namespace ACEPlayerKillerStatus
{
	constexpr int32 PK = 0x04;
	constexpr int32 PKLite = 0x40;
	// PublicWeenieDesc::SetPlayerKillerStatus: replace only the three PK flags.
	inline int32 UpdateDescriptionFlags(int32 Flags, int32 Status)
	{
		Flags &= ~(ACEObjectDescFlag::PlayerKiller | ACEObjectDescFlag::PkLiteStatus | ACEObjectDescFlag::FreePkStatus);
		if (Status == PK) Flags |= ACEObjectDescFlag::PlayerKiller;
		else if (Status == PKLite) Flags |= ACEObjectDescFlag::PkLiteStatus;
		else if (Status == 0x20) Flags |= ACEObjectDescFlag::FreePkStatus;
		return Flags;
	}
	inline bool BlocksPlayers(int32 Status)
	{
		return (Status & PK) != 0 || (Status & PKLite) != 0;
	}
	inline bool FlagsBlockPlayers(int32 ObjectDescriptionFlags)
	{
		return (ObjectDescriptionFlags & ACEObjectDescFlag::PlayerKiller) != 0
			|| (ObjectDescriptionFlags & ACEObjectDescFlag::PkLiteStatus) != 0;
	}
}

/** ACE ItemType bits we care about. */
namespace ACEItemType
{
	constexpr int32 MeleeWeapon = 0x00000001;
	constexpr int32 Armor = 0x00000002;
	constexpr int32 Clothing = 0x00000004;
	constexpr int32 Jewelry = 0x00000008;
	constexpr int32 Creature = 0x00000010;
	constexpr int32 Food = 0x00000020;
	constexpr int32 Money = 0x00000040;
	constexpr int32 Misc = 0x00000080;
	constexpr int32 MissileWeapon = 0x00000100;
	constexpr int32 Container = 0x00000200;
	constexpr int32 Useless = 0x00000400;
	constexpr int32 Gem = 0x00000800;
	constexpr int32 SpellComponents = 0x00001000;
	constexpr int32 Writable = 0x00002000;
	constexpr int32 Key = 0x00004000;
	constexpr int32 Caster = 0x00008000;
	constexpr int32 Portal = 0x00010000;
	constexpr int32 PromissoryNote = 0x00040000;
	constexpr int32 ManaStone = 0x00080000;
	constexpr int32 MagicWieldable = 0x00200000;
	constexpr int32 LifeStone = 0x10000000;
}

/** ItemUseable helpers (PublicWeenieDesc / Mag-nus ItemUses). */
namespace ACEItemUseable
{
	/** Mag-nus ItemUses::IsUseable_Targeted — any target-side bits in the high word. */
	inline bool IsTargeted(int32 Useable)
	{
		return (static_cast<uint32>(Useable) & 0xFFFF0000u) != 0;
	}

	/** Source (low word) is usable — not Undef (0) or No (1). Fountains, levers, etc. */
	inline bool IsSourceUsable(int32 Useable)
	{
		return (static_cast<uint32>(Useable) & 0xFFFFu) > 1u;
	}

	/** Usable.NeverWalk — Use in place without a MoveTo approach. */
	inline bool HasNeverWalk(int32 Useable)
	{
		return (static_cast<uint32>(Useable) & 0x40u) != 0;
	}
}

/** ACE EquipMask bits (PublicWeenieDesc.CurrentlyWieldedLocation). */
namespace ACEEquipMask
{
	constexpr int64 HeadWear = 0x00000001;
	constexpr int64 ChestWear = 0x00000002;
	constexpr int64 AbdomenWear = 0x00000004;
	constexpr int64 UpperArmWear = 0x00000008;
	constexpr int64 LowerArmWear = 0x00000010;
	constexpr int64 HandWear = 0x00000020;
	constexpr int64 UpperLegWear = 0x00000040;
	constexpr int64 LowerLegWear = 0x00000080;
	constexpr int64 FootWear = 0x00000100;
	constexpr int64 ChestArmor = 0x00000200;
	constexpr int64 AbdomenArmor = 0x00000400;
	constexpr int64 UpperArmArmor = 0x00000800;
	constexpr int64 LowerArmArmor = 0x00001000;
	constexpr int64 UpperLegArmor = 0x00002000;
	constexpr int64 LowerLegArmor = 0x00004000;
	constexpr int64 NeckWear = 0x00008000;
	constexpr int64 WristWearLeft = 0x00010000;
	constexpr int64 WristWearRight = 0x00020000;
	constexpr int64 FingerWearLeft = 0x00040000;
	constexpr int64 FingerWearRight = 0x00080000;
	constexpr int64 MeleeWeapon = 0x00100000;
	constexpr int64 Shield = 0x00200000;
	constexpr int64 MissileWeapon = 0x00400000;
	constexpr int64 MissileAmmo = 0x00800000;
	constexpr int64 Held = 0x01000000;
	constexpr int64 TwoHanded = 0x02000000;
	constexpr int64 TrinketOne = 0x04000000;
	constexpr int64 Cloak = 0x08000000;
	constexpr int64 SigilOne = 0x10000000;
	constexpr int64 SigilTwo = 0x20000000;
	constexpr int64 SigilThree = 0x40000000;
}

/**
 * ACE CoverageMask (PropertyInt.ClothingPriority) — used for clothing/armor conflict checks.
 * Pants (UnderwearLegs) and boots (Feet) share LowerLegWear on EquipMask but do not conflict here.
 */
namespace ACECoverageMask
{
	constexpr uint32 UnderwearUpperLegs = 0x00000002;
	constexpr uint32 UnderwearLowerLegs = 0x00000004;
	constexpr uint32 UnderwearChest = 0x00000008;
	constexpr uint32 UnderwearAbdomen = 0x00000010;
	constexpr uint32 UnderwearUpperArms = 0x00000020;
	constexpr uint32 UnderwearLowerArms = 0x00000040;
	constexpr uint32 OuterwearUpperLegs = 0x00000100;
	constexpr uint32 OuterwearLowerLegs = 0x00000200;
	constexpr uint32 OuterwearChest = 0x00000400;
	constexpr uint32 OuterwearAbdomen = 0x00000800;
	constexpr uint32 OuterwearUpperArms = 0x00001000;
	constexpr uint32 OuterwearLowerArms = 0x00002000;
	constexpr uint32 Head = 0x00004000;
	constexpr uint32 Hands = 0x00008000;
	constexpr uint32 Feet = 0x00010000;
}

/** Infer ClothingPriority from EquipMask + ItemType when the property is not on the wire. */
inline uint32 ACEInferClothingPriority(int64 EquipLoc, int32 ItemType)
{
	using namespace ACEEquipMask;
	using namespace ACECoverageMask;
	if (EquipLoc == 0)
	{
		return 0;
	}
	uint32 Cov = 0;
	if (EquipLoc & HeadWear) { Cov |= Head; }
	if (EquipLoc & HandWear) { Cov |= Hands; }
	if (EquipLoc & FootWear) { Cov |= Feet; }

	constexpr int64 ArmorBits = ChestArmor | AbdomenArmor | UpperArmArmor | LowerArmArmor
		| UpperLegArmor | LowerLegArmor;
	const bool bArmor = (EquipLoc & ArmorBits) != 0
		|| (ItemType & ACEItemType::Armor) != 0;

	if (bArmor)
	{
		if (EquipLoc & ChestArmor) { Cov |= OuterwearChest; }
		if (EquipLoc & AbdomenArmor) { Cov |= OuterwearAbdomen; }
		if (EquipLoc & UpperArmArmor) { Cov |= OuterwearUpperArms; }
		if (EquipLoc & LowerArmArmor) { Cov |= OuterwearLowerArms; }
		if (EquipLoc & UpperLegArmor) { Cov |= OuterwearUpperLegs; }
		if (EquipLoc & LowerLegArmor) { Cov |= OuterwearLowerLegs; }
	}
	else
	{
		// Underwear / clothes. Match ACE CoverageMaskHelper.UnderwearShirt / UnderwearPants:
		// shirts and pants both carry AbdomenWear in EquipMask — do not map it onto both
		// or underpants/undershirt fight for the same ClothingPriority bit.
		const bool bHasChest = (EquipLoc & ChestWear) != 0;
		const bool bHasLegs = (EquipLoc & (UpperLegWear | LowerLegWear)) != 0;
		const bool bShirtLike = bHasChest && !bHasLegs;
		const bool bPantsLike = bHasLegs && !bHasChest;

		if (bShirtLike)
		{
			if (EquipLoc & ChestWear) { Cov |= UnderwearChest; }
			if (EquipLoc & UpperArmWear) { Cov |= UnderwearUpperArms; }
			if (EquipLoc & LowerArmWear) { Cov |= UnderwearLowerArms; }
		}
		else if (bPantsLike)
		{
			if (EquipLoc & UpperLegWear) { Cov |= UnderwearUpperLegs; }
			if ((EquipLoc & LowerLegWear) != 0 && (EquipLoc & FootWear) == 0)
			{
				Cov |= UnderwearLowerLegs;
			}
		}
		else
		{
			// Robe / full-body clothing covering torso and legs.
			if (EquipLoc & ChestWear) { Cov |= UnderwearChest; }
			if (EquipLoc & AbdomenWear) { Cov |= UnderwearAbdomen; }
			if (EquipLoc & UpperArmWear) { Cov |= UnderwearUpperArms; }
			if (EquipLoc & LowerArmWear) { Cov |= UnderwearLowerArms; }
			if (EquipLoc & UpperLegWear) { Cov |= UnderwearUpperLegs; }
			if ((EquipLoc & LowerLegWear) != 0 && (EquipLoc & FootWear) == 0)
			{
				Cov |= UnderwearLowerLegs;
			}
		}
	}
	return Cov;
}

/** One entry from GameEvent ViewContents (0x0196). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEContainerItemRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ItemGuid = 0;
	/** 0=item, 1=container/pack, 2=foci (RequiresPackSlot). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ContainerType = 0;
};

/** ACE PhysicsState bits we care about. */
namespace ACEPhysicsState
{
	constexpr int32 Static = 0x00000001;
	constexpr int32 Ethereal = 0x00000004;
	constexpr int32 ReportCollisions = 0x00000008;
	constexpr int32 IgnoreCollisions = 0x00000010;
	constexpr int32 NoDraw = 0x00000020;
	constexpr int32 Missile = 0x00000040;
	constexpr int32 AlignPath = 0x00000100;
	constexpr int32 Gravity = 0x00000400;
	constexpr int32 ParticleEmitter = 0x00001000;
	/** Retail CPhysicsObj hidden bit — set during portal/teleport until destination is ready. */
	constexpr int32 Hidden = 0x00004000;
	constexpr int32 HasPhysicsBSP = 0x00010000;
	constexpr int32 Inelastic = 0x00020000;
	constexpr int32 HasDefaultAnim = 0x00040000;
	constexpr int32 HasDefaultScript = 0x00080000;
	constexpr int32 Cloaked = 0x00100000;
}

/** PhysicsDesc / PositionPack sequence indices (ACE PhysicsTimeStamp). */
namespace ACEPhysicsTimeStamp
{
	constexpr int32 Position = 0;
	constexpr int32 Movement = 1;
	constexpr int32 State = 2;
	constexpr int32 Vector = 3;
	constexpr int32 Teleport = 4;
	constexpr int32 ServerControl = 5;
	constexpr int32 ForcePosition = 6;
	constexpr int32 ObjDesc = 7;
	constexpr int32 Instance = 8;
	constexpr int32 Count = 9;

	/** Retail PhysicsObj.is_newer — true if Incoming is strictly newer than Current. */
	inline bool IsNewer(uint16 Current, uint16 Incoming)
	{
		const int32 Diff = static_cast<int32>(Incoming) - static_cast<int32>(Current);
		if (FMath::Abs(Diff) > 32767)
		{
			return Incoming < Current;
		}
		return Current < Incoming;
	}
}

/**
 * Local player attributes + vitals, parsed from GameEvent PlayerDescription (0x0013)
 * and kept current by PrivateUpdateVital / PrivateUpdateAttribute2ndLevel messages.
 * Max vitals use the retail formulas (MaxHealth = End/2, MaxStamina = End, MaxMana = Self)
 * without enchantments/vitae — server private updates keep Current authoritative.
 */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACESkillInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SkillId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Ranks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 InitLevel = 0;
	/** 0 Inactive, 1 Untrained, 2 Trained, 3 Specialized (ACE SkillAdvancementClass). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 AdvancementClass = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Current = 0;
	/** Retail raw value, also used for base-skill equipment requirements. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Base = 0;
	/** Permanent gains applied after enchantment/vitae math, not included in Base. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 UnscaledAugmentationBonus = 0;
	/** Unbuffed interface baseline includes every permanent augmentation. */
	int32 GetDisplayBase() const { return Base + UnscaledAugmentationBonus; }
	int32 GetDisplayModifier() const { return Current - GetDisplayBase(); }
	/** Cumulative XP spent raising this skill (PropertySkill.PP). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 XpSpent = 0;
	/** Compact SAC updates recalculate ranks from XP once the DAT curve is available. */
	bool bRecomputeRanksFromXp = false;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEPlayerVitals
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Health = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxHealth = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Stamina = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxStamina = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Mana = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxMana = 0;

	/** Raw DAT-derived maximums, including innate/gear bonuses but not enchantments or vitae. */
	int32 BaseMaxHealth = 0, BaseMaxStamina = 0, BaseMaxMana = 0;
	int32 GetVitalBase(int32 Id) const
	{
		return Id == 1 ? BaseMaxHealth : Id == 3 ? BaseMaxStamina : Id == 5 ? BaseMaxMana : 0;
	}

	/** Base attribute currents (StartingValue + Ranks), indexed Strength..Self order. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Strength = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Endurance = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Quickness = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Coordination = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Focus = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Self = 0;

	/**
	 * Enchantment multipliers/additives for primary attributes (StatModType Attribute).
	 * Buffed Strength = max(10, Round(Base * Mul + Add)) — matches ACE CreatureAttribute.Current.
	 */
	float StrengthEnchantMul = 1.f;
	float StrengthEnchantAdd = 0.f;
	float EnduranceEnchantMul = 1.f;
	float EnduranceEnchantAdd = 0.f;
	float QuicknessEnchantMul = 1.f;
	float QuicknessEnchantAdd = 0.f;
	float CoordinationEnchantMul = 1.f;
	float CoordinationEnchantAdd = 0.f;
	float FocusEnchantMul = 1.f;
	float FocusEnchantAdd = 0.f;
	float SelfEnchantMul = 1.f;
	float SelfEnchantAdd = 0.f;

	/** PropertyInt.AugmentationIncreasedCarryingCapacity (0..5). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CarryingCapacityAugs = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 AetheriaUnlocked = 0;

	/** Buffed attribute current (enchantments applied). */
	int32 GetBuffedStrength() const
	{
		const int32 Base = FMath::Max(1, Strength);
		const int32 Total = FMath::RoundToInt(static_cast<float>(Base) * StrengthEnchantMul + StrengthEnchantAdd);
		return FMath::Max(Base >= 10 ? 10 : 1, Total);
	}
	int32 GetBuffedEndurance() const
	{
		const int32 Base = FMath::Max(1, Endurance);
		const int32 Total = FMath::RoundToInt(static_cast<float>(Base) * EnduranceEnchantMul + EnduranceEnchantAdd);
		return FMath::Max(Base >= 10 ? 10 : 1, Total);
	}
	int32 GetBuffedQuickness() const
	{
		const int32 Base = FMath::Max(1, Quickness);
		const int32 Total = FMath::RoundToInt(static_cast<float>(Base) * QuicknessEnchantMul + QuicknessEnchantAdd);
		return FMath::Max(Base >= 10 ? 10 : 1, Total);
	}
	int32 GetBuffedCoordination() const
	{
		const int32 Base = FMath::Max(1, Coordination);
		const int32 Total = FMath::RoundToInt(static_cast<float>(Base) * CoordinationEnchantMul + CoordinationEnchantAdd);
		return FMath::Max(Base >= 10 ? 10 : 1, Total);
	}
	int32 GetBuffedFocus() const
	{
		const int32 Base = FMath::Max(1, Focus);
		const int32 Total = FMath::RoundToInt(static_cast<float>(Base) * FocusEnchantMul + FocusEnchantAdd);
		return FMath::Max(Base >= 10 ? 10 : 1, Total);
	}
	int32 GetBuffedSelf() const
	{
		const int32 Base = FMath::Max(1, Self);
		const int32 Total = FMath::RoundToInt(static_cast<float>(Base) * SelfEnchantMul + SelfEnchantAdd);
		return FMath::Max(Base >= 10 ? 10 : 1, Total);
	}

	/** Ranks / starting / cumulative XP spent for primary attributes (PropertyAttribute 1..6). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StrengthRanks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StrengthStart = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StrengthXpSpent = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 EnduranceRanks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 EnduranceStart = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 EnduranceXpSpent = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 QuicknessRanks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 QuicknessStart = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 QuicknessXpSpent = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CoordinationRanks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CoordinationStart = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CoordinationXpSpent = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 FocusRanks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 FocusStart = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 FocusXpSpent = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SelfRanks = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SelfStart = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SelfXpSpent = 0;

	/** Vital ranks + starting (max = these + attribute formula). */
	int32 HealthRanks = 0, HealthStart = 0, HealthXpSpent = 0;
	int32 StaminaRanks = 0, StaminaStart = 0, StaminaXpSpent = 0;
	int32 ManaRanks = 0, ManaStart = 0, ManaXpSpent = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Level = 0;

	/** PropertyInt.Gender (1=Male, 2=Female). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Gender = 0;
	/** PropertyInt.HeritageGroup (Aluvian=1 …). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 HeritageGroup = 0;
	/** Effective PropertyInt.AllegianceRank, including winning rank enchantments. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 EffectiveAllegianceRank = -1;
	/** PropertyInt.PlayerKillerStatus flags. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 PlayerKillerStatus = 0;
	/** PropertyInt.Age — total in-game seconds (retail CharacterInfo birth/age line). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 AgeSeconds = 0;
	/** PropertyInt.NumDeaths. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 NumDeaths = 0;
	/** PropertyInt.ChessRank (retail CharacterInfo "fake skills" section, default 1400). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ChessRank = 0;
	/** PropertyInt.FakeFishingSkill. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 FishingSkill = 0;
	/** PropertyInt.AvailableSkillCredits. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 AvailableSkillCredits = 0;
	/** PropertyInt.CombatMode (NonCombat/Melee/Missile/Magic). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CombatMode = static_cast<int32>(ACECombatMode::NonCombat);
	/** PropertyString.Template — character class label (e.g. "War Mage"). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString TemplateName;

	/** PropertyInt64.TotalExperience / AvailableExperience. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int64 TotalExperience = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int64 AvailableExperience = 0;
	/** PropertyInt64.AvailableLuminance / MaximumLuminance (0 before luminance is unlocked). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int64 AvailableLuminance = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int64 MaximumLuminance = 0;
	/** A complete login description also establishes a zero balance when the optional quality is absent. */
	bool bHasLuminanceBalance = false;

	/** Run skill current (init + ranks + Quickness formula) — feeds prediction speed. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 RunSkillCurrent = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 JumpSkillCurrent = 0;

	/** Full skill table from PlayerDescription. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FACESkillInfo> Skills;
	/** Integer qualities contributing to skills (augmentation bonuses). */
	TMap<int32, int32> StatQualityInts;
	/** Server character qualities retained for plugin queries, including custom IDs. */
	TMap<int32, int64> QualityInt64s;
	TMap<int32, bool> QualityBools;
	TMap<int32, double> QualityDoubles;
	TMap<int32, FString> QualityStrings;

	int32 GetAttributeBase(int32 Id) const
	{
		switch (Id)
		{
		case 1: return Strength; case 2: return Endurance; case 3: return Quickness;
		case 4: return Coordination; case 5: return Focus; case 6: return Self;
		default: return 0;
		}
	}

	void RecomputeMaxVitals()
	{
		// Unbuffed retail formulas. Keep the high-water mark so Current (often full / buffed
		// at login) never displays larger than Max — and Max stays correct after damage.
		const int32 FormulaH = BaseMaxHealth = HealthStart + HealthRanks + FMath::RoundToInt(Endurance / 2.f)
			+ StatQualityInts.FindRef(379) + 2 * StatQualityInts.FindRef(390);
		const int32 FormulaS = BaseMaxStamina = StaminaStart + StaminaRanks + Endurance;
		const int32 FormulaM = BaseMaxMana = ManaStart + ManaRanks + Self;
		MaxHealth = FMath::Max3(FormulaH, MaxHealth, Health);
		MaxStamina = FMath::Max3(FormulaS, MaxStamina, Stamina);
		MaxMana = FMath::Max3(FormulaM, MaxMana, Mana);
	}

	int32 GetAttributeRanks(int32 AttributeId) const
	{
		switch (AttributeId)
		{
		case 1: return StrengthRanks;
		case 2: return EnduranceRanks;
		case 3: return QuicknessRanks;
		case 4: return CoordinationRanks;
		case 5: return FocusRanks;
		case 6: return SelfRanks;
		default: return 0;
		}
	}

	int32 GetAttributeXpSpent(int32 AttributeId) const
	{
		switch (AttributeId)
		{
		case 1: return StrengthXpSpent;
		case 2: return EnduranceXpSpent;
		case 3: return QuicknessXpSpent;
		case 4: return CoordinationXpSpent;
		case 5: return FocusXpSpent;
		case 6: return SelfXpSpent;
		default: return 0;
		}
	}

	int32 GetAttributeCurrent(int32 AttributeId) const
	{
		switch (AttributeId)
		{
		case 1: return GetBuffedStrength();
		case 2: return GetBuffedEndurance();
		case 3: return GetBuffedQuickness();
		case 4: return GetBuffedCoordination();
		case 5: return GetBuffedFocus();
		case 6: return GetBuffedSelf();
		default: return 0;
		}
	}
};

/** Chess GameEvent (retail CM_Game: Join/Start/MoveResponse/OpponentTurn/Stalemate/GameOver). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEChessEvent
{
	GENERATED_BODY()

	/** GameEvent opcode (0x0281 join, 0x0282 start, 0x0283 move result, 0x0284 opponent turn, 0x0285 stalemate, 0x028C over). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 EventType = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 BoardGuid = 0;
	/** Join/Start: ChessColor. MoveResponse: ChessMoveResult. Stalemate: offer flag. GameOver: winning team. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Value = 0;
	/** OpponentTurn: ChessMoveType (4=Grid, 5=FromTo, 6=SelectedPiece). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MoveType = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 FromX = -1;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 FromY = -1;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ToX = -1;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ToY = -1;
};

/** World object known to the client (from ObjectCreate / updates). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEWorldObject
{
	GENERATED_BODY()
	/** Original public/physics header masks for Decal-compatible property queries. */
	uint32 CreateFlags1 = 0, CreateFlags2 = 0, PhysicsDataFlags = 0;
	uint16 PublicPhysicsScript = 0;
	/** Transient event metadata: cached placement is not a fresh physics update. */
	bool bAppearanceOnlyUpdate = false;
	/** Transient event metadata: a wield/parent notification, not a new item description. */
	bool bAttachmentOnlyUpdate = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Guid = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FString Name;

	/** Optional server-authored name for stacks larger than one. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FString PluralName;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 WeenieClassId = 0;

	/** PublicWeenieDesc icon DID (0x06…) — inventory / radar / selection art. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 IconId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 IconOverlayId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 IconUnderlayId = 0;

	/** Shared cooldown group and full duration from the second PublicWeenieDesc header. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 CooldownId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	double CooldownDuration = 0.0;

	/** PublicWeenieDesc UiEffects (ACE.Entity.Enum.UiEffects) — Magical / elemental icon tint. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 UiEffects = 0;

	/** Container holding this object (pack/bag GUID). 0 = not in a container. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ContainerId = 0;

	/** Creature currently wielding this object (weenie header). May equal ParentGuid. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 WielderId = 0;

	/** PublicWeenieDesc allegiance identity used by retail radar shapes. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 MonarchGuid = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Value = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Burden = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ContainersCapacity = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ItemsCapacity = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 StackSize = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 MaxStackSize = 0;

	/** Vendor ItemProfile supply; -1 is unlimited. Independent of the item stack size. */
	int32 VendorQuantityAvailable = -1;

	/** Remaining uses or salvage units (PublicWeenieDesc Structure); not item mana. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Structure = 0;

	/** Maximum uses or salvage units (PublicWeenieDesc MaxStructure). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 MaxStructure = 0;

	/** PublicWeenieDesc SpellDID — equipped wand/orb innate spell (BuiltInSpell hotbar). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 SpellDID = 0;

	/** PublicWeenieDesc ItemUseable bitmask. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ItemUseable = 0;

	/** PublicWeenieDesc TargetType (ItemType mask of valid use targets). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 TargetType = 0;

	/** PublicWeenieDesc MaterialType (0 = none). Used for salvage suitability. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 MaterialType = 0;

	/** PublicWeenieDesc workmanship; fractional for combined salvage, -1 if absent. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	float SalvageWorkmanship = -1.f;

	/** Latest appraisal/public mana qualities: -1 unknown, -2 authoritatively absent. */
	int32 ItemCurrentMana = -1;
	int32 ItemMaximumMana = -1;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 SetupId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 MotionTableId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 SoundTableId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 PhysicsEffectTableId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ItemType = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	float Scale = 1.f;
	/** PublicWeenieDesc has HookItemTypes: this container displays a house-hook item. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bIsHouseHook = false;
	/** Public hook compatibility and ownership, used by ItemHolder::CheckHookStatus. */
	uint32 HookItemTypes = 0;
	uint16 HookType = 0;
	int32 HouseOwnerId = 0;
	float Friction = .95f;
	float Elasticity = .05f;
	/** CPhysicsObj::set_elasticity limits restitution to [0, 0.1]. */
	float GetValidElasticity() const { return FMath::IsFinite(Elasticity) ? FMath::Clamp(Elasticity, 0.f, .1f) : .05f; }

	/** Retail physics/model scale is server-authored, including custom weenies. */
	float GetValidObjectScale() const { return FMath::IsFinite(Scale) && Scale > 0.f ? Scale : 1.f; }

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FACEPosition Position;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bHasPosition = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bIsPlayer = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bIsSelf = false;

	/** Equipped/held on another object (PhysicsDesc Parent). 0 = world object. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ParentGuid = 0;

	/** ACE ParentLocation (RightHand=1, LeftHand=2, Shield=3, ...). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ParentLocation = 0;

	/** ACE Placement (RightHandCombat=1, …) — drives weapon Setup part frames when held. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 PlacementId = 0;
	/** Full server PhysicsDesc owns animation-table selection, including zero. Local previews use setup defaults. */
	bool bHasServerPhysicsDescription = false;

	/** PropertyInt.PlacementPosition — slot order inside a pack (0..ItemsCapacity-1). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 PlacementPosition = INDEX_NONE;
	/** Retail PublicWeenieDesc priority, used to select the outermost worn layer. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ClothingPriority = 0;

	/** ACE EquipMask from PublicWeenieDesc.CurrentlyWieldedLocation. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int64 CurrentWieldedLocation = 0;

	/** ACE EquipMask from PublicWeenieDesc.ValidLocations — where this item may be wielded. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int64 ValidLocations = 0;

	/** PublicWeenieDesc AmmoType (bow/crossbow/atlatl) — used for missile stance. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 AmmoType = 0;

	/** PublicWeenieDesc / PropertyInt.CombatUse (shield, two-handed, …). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 CombatUse = 0;

	/** PropertyInt.DefaultCombatStyle — retail weapon stance when present. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 DefaultCombatStyle = 0;

	/** PropertyInt.Attuned (0=Normal, 1=Attuned, 2=Sticky). Attuned items cannot be dropped. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Attuned = 0;

	/** PropertyInt.Bonded (-2=Destroy, -1=BadBonded, 0=Normal, 1=Bonded). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 Bonded = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 ObjectDescriptionFlags = 0;
	/** Retail excludes summoned combat pets from spell targets. */
	int32 PetOwnerId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 PhysicsState = 0;

	/** PhysicsDesc 9×uint16 timestamps (ACEPhysicsTimeStamp). */
	uint16 PhysicsTimestamps[ACEPhysicsTimeStamp::Count] = {};
	bool bHasPhysicsTimestamps = false;
	// CMotionInterp's independent 15-bit action stamp. Movement updates can
	// repeat outstanding actions without asking observers to replay them.
	uint16 ReceivedActionStamp = 0;
	bool bHasReceivedActionStamp = false;
	/** UI-only record awaiting CreateObject's full physics description. Events cannot complete it. */
	bool bPhysicsDescriptionPending = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	float UseRadius = 0.6f;

	/** ACE RadarColor byte from PublicWeenieDesc (0 = Default). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	uint8 RadarBlipColor = 0;

	/** ACE RadarBehavior byte (0 = Undefined, 1 = ShowNever, 4 = ShowAlways, …). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	uint8 RadarBehavior = 0;

	/** PhysicsDesc translucency: 0 = opaque, 1 = fully transparent. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	float Translucency = 0.f;

	/** Raw AC-space angular velocity from PhysicsDesc. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FVector Omega = FVector::ZeroVector;

	/** Raw AC-space linear velocity (spell/missile projectiles). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bHasVelocity = false;

	/** Explicit PhysicsScript data ID from PhysicsDesc. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 DefaultScriptId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	float DefaultScriptIntensity = 0.f;

	/** Setup DefaultAnimation DID when present (looping static props / lifestones). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 DefaultAnimationId = 0;

	/** Interpreted ForwardCommand from ObjectCreate Movement blob (On/Off doors, Dead corpses). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 InitialMotionCommand = 0;

	/** Interpreted MotionStance / style from ObjectCreate Movement blob (packed ushort). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	int32 InitialMotionStyle = 0;

	/** Authoritative death motion, retained until removal or a new incarnation. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	bool bDying = false;

	UPROPERTY(BlueprintReadOnly, Category = "ACE")
	FACEObjDesc Appearance;

	bool IsDoor() const { return (ObjectDescriptionFlags & ACEObjectDescFlag::Door) != 0; }
	/** Retail ItemUses::IsUseable: the No bit forbids direct activation, even on doors. */
	bool IsDirectDoorUseBlocked() const { return IsDoor() && (ItemUseable & 1) != 0; }
	bool IsOpenable() const { return (ObjectDescriptionFlags & ACEObjectDescFlag::Openable) != 0; }
	/**
	 * Doors / chests / switches — MotionTable On/Off, not locomotion.
	 * Server clears Openable when locked; still treat Container+MT as On/Off so locked
	 * chests hold closed instead of bind-pose (often looks open).
	 */
	bool UsesOnOffMotion() const
	{
		// Corpses are containers with a creature MTable, not animated chests.
		if (IsCorpse()) return false;
		// Authored remains can be ordinary containers, with Dead/Sleeping in
		// Movement rather than the Corpse descriptor. Preserve that explicit pose.
		if (ACEMotion::IsHeldRestCommand(InitialMotionCommand)) return false;
		// Hook containers are Openable too, but display the item's own animation.
		// Only an explicit On/Off motion may put a hooked decoration in that state.
		if (IsDoor() || (!bIsHouseHook && IsOpenable()))
		{
			return true;
		}
		// Weak Spot / doorpedestal (16919) — WeenieType.Door; ObjectDesc Door bit can lag.
		if (WeenieClassId == 16919)
		{
			return true;
		}
		if (!bIsHouseHook && (ItemType & ACEItemType::Container) != 0 && MotionTableId != 0)
		{
			return true;
		}
		const int32 Cmd = InitialMotionCommand;
		return Cmd == static_cast<int32>(ACEMotion::OnCommandU16)
			|| Cmd == static_cast<int32>(ACEMotion::OffCommandU16)
			|| Cmd == static_cast<int32>(ACEMotion::On)
			|| Cmd == static_cast<int32>(ACEMotion::Off);
	}
	bool IsCorpse() const { return (ObjectDescriptionFlags & ACEObjectDescFlag::Corpse) != 0; }
	/** Remote-use NPCs can deliberately hold Dead as an emote (quest remains). */
	bool AllowsDeadPoseInteraction() const
	{
		return !bIsPlayer && (ItemUseable & 0x21) == 0x20
			&& (ObjectDescriptionFlags & ACEObjectDescFlag::Attackable) == 0;
	}
	bool IsGameBoard() const { return (static_cast<uint32>(ItemType) & 0x80000000u) != 0; }
	bool IsUnnamedDecoration() const
	{
		return Name.IsEmpty() && (ObjectDescriptionFlags & ACEObjectDescFlag::Stuck) != 0
			&& !bIsPlayer && !IsAttackable() && !IsVendor() && !IsDoor() && !IsLifeStone()
			&& (ItemType & (ACEItemType::Creature | ACEItemType::Portal)) == 0;
	}
	bool IsSelectableWorldObject() const
	{
		return Guid != 0 && bHasPosition && ContainerId == 0 && WielderId == 0 && ParentGuid == 0
			&& !IsUiHidden() && !IsUnnamedDecoration()
			&& (!bDying || IsCorpse())
			&& (PhysicsState & (ACEPhysicsState::Missile | ACEPhysicsState::ParticleEmitter | ACEPhysicsState::NoDraw)) == 0;
	}
	bool IsWieldOnUse() const { return (ObjectDescriptionFlags & ACEObjectDescFlag::WieldOnUse) != 0; }
	/** Server DropItem rejects Attuned >= Attuned (incl. Sticky). */
	bool CanDropToWorld() const { return Attuned < 1; }
	/** Portable ground object eligible for pickup; containers may also be opened with Use. */
	bool IsWorldLootable() const
	{
		if (Guid == 0 || bIsPlayer || !bHasPosition)
		{
			return false;
		}
		if (ContainerId != 0 || WielderId != 0 || ParentGuid != 0)
		{
			return false;
		}
		if ((ItemType & (ACEItemType::Creature | ACEItemType::Portal)) != 0)
		{
			return false;
		}
		if (IsDoor() || IsLifeStone() || IsCorpse() || IsGameBoard())
		{
			return false;
		}
		if ((ObjectDescriptionFlags & ACEObjectDescFlag::Stuck) != 0)
		{
			return false;
		}
		return true;
	}
	bool IsLifeStone() const
	{
		return (ItemType & ACEItemType::LifeStone) != 0
			|| (ObjectDescriptionFlags & ACEObjectDescFlag::LifeStone) != 0;
	}
	bool IsAttackable() const
	{
		return !bDying && !IsCorpse() && (ObjectDescriptionFlags & ACEObjectDescFlag::Attackable) != 0;
	}
	bool IsVendor() const
	{
		return (ObjectDescriptionFlags & ACEObjectDescFlag::Vendor) != 0;
	}
	/**
	 * Drag-Give / world-pick target: players, vendors, creatures, and Stuck/radar
	 * town NPCs (collectors, quest givers) that may omit the Creature ItemType bit.
	 */
	bool IsGiveOrCreatureTarget() const
	{
		if (bIsPlayer || IsVendor())
		{
			return true;
		}
		if ((ItemType & ACEItemType::Creature) != 0)
		{
			return true;
		}
		if (IsDoor() || IsOpenable() || IsCorpse() || IsLifeStone())
		{
			return false;
		}
		if ((ItemType & ACEItemType::Portal) != 0)
		{
			return false;
		}
		// Chests / switches share MotionTables but are not give targets.
		if (UsesOnOffMotion())
		{
			return false;
		}
		const bool bStuck = (ObjectDescriptionFlags & ACEObjectDescFlag::Stuck) != 0;
		const bool bOnRadar = RadarBlipColor != 0 || RadarBehavior != 0;
		// Stuck+MotionTable collectors often lack Creature; radar catches other NPCs.
		if (MotionTableId != 0 && (bStuck || bOnRadar))
		{
			return true;
		}
		// Some trophy collectors omit MotionTable in create but still sit on radar.
		if (bStuck && !IsAttackable() && bOnRadar)
		{
			return true;
		}
		return false;
	}
	/**
	 * Client should run into Use range for these (doors, chests, loot, NPCs).
	 * Non-usable stuck scenery and non-usable attackable monsters are select/identify only.
	 */
	bool ShouldClientApproachForUse() const
	{
		if (Guid == 0 || bIsPlayer || !bHasPosition)
		{
			return false;
		}
		if (ContainerId != 0 || WielderId != 0 || ParentGuid != 0)
		{
			return false;
		}
		if (IsWorldLootable())
		{
			return true;
		}
		if (IsDoor() || IsOpenable() || IsLifeStone() || IsCorpse() || IsGameBoard())
		{
			return true;
		}
		if ((ItemType & ACEItemType::Portal) != 0
			|| (ObjectDescriptionFlags & ACEObjectDescFlag::Portal) != 0
			|| (ObjectDescriptionFlags & ACEObjectDescFlag::BindStone) != 0)
		{
			return true;
		}
		if (IsVendor())
		{
			return true;
		}
		if ((ItemType & ACEItemType::Creature) != 0)
		{
			// Attackability does not exclude Use (e.g. cow tipping). NeverWalk
			// still sends Use, but leaves any movement decision to the server.
			return (!IsAttackable() || ACEItemUseable::IsSourceUsable(ItemUseable))
				&& !ACEItemUseable::HasNeverWalk(ItemUseable);
		}
		// Stuck signs: select-only. Stuck world objects with Usable (fountains, etc.): approach
		// unless NeverWalk (Use in place).
		if ((ObjectDescriptionFlags & ACEObjectDescFlag::Stuck) != 0)
		{
			if (!ACEItemUseable::IsSourceUsable(ItemUseable) || ACEItemUseable::HasNeverWalk(ItemUseable))
			{
				return false;
			}
			return true;
		}
		return UseRadius > 0.f;
	}
	bool IsEthereal() const { return (PhysicsState & ACEPhysicsState::Ethereal) != 0; }
	bool HasDefaultAnim() const { return (PhysicsState & ACEPhysicsState::HasDefaultAnim) != 0; }
	/** ObjectDescriptionFlag.HiddenAdmin — retail draws these only for admins. */
	bool IsHiddenAdmin() const { return (ObjectDescriptionFlags & ACEObjectDescFlag::HiddenAdmin) != 0; }
	/** UIHidden suppresses interaction/radar, not the visible world geometry. */
	bool IsUiHidden() const { return (ObjectDescriptionFlags & ACEObjectDescFlag::UiHidden) != 0; }
	/** PhysicsState.Cloaked — admin-cloak / invisible unless Adminvision. */
	bool IsCloaked() const { return (PhysicsState & ACEPhysicsState::Cloaked) != 0; }
	bool IsAdminOnlyVisible() const { return IsHiddenAdmin() || IsCloaked(); }
	/** PK or PK Lite — the only player statuses that collide with other players. */
	bool IsPkOrPkLite() const { return ACEPlayerKillerStatus::FlagsBlockPlayers(ObjectDescriptionFlags); }
};

/** ACE.Entity Placement from ParentLocation (held-item Setup part frames). */
inline int32 ACEPlacementFromParentLocation(int32 ParentLocation)
{
	switch (ParentLocation)
	{
	case 1: return 1; // RightHand → RightHandCombat
	case 2: return 3; // LeftHand
	case 3: return 6; // Shield
	case 4: return 4; // Belt
	case 5: return 5; // Quiver
	case 8: return 2; // LeftWeapon: ACE sends RightHandNonCombat, not placement 7.
	case 9: return 8; // LeftUnarmed
	default: return 0;
	}
}

/** Appraisal / identify result (GameEvent 0x00C9). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEAppraisalInfo
{
	GENERATED_BODY()
	uint32 CreatureProfileFlags = 0;
	/** Public descriptor flags used by retail item examination (e.g. Healer). */
	int32 ObjectDescriptionFlags = 0;
	TMap<uint32, int32> IntProperties;
	TMap<uint32, uint64> Int64Properties;
	TMap<uint32, bool> BoolProperties;
	TMap<uint32, double> FloatProperties;
	TMap<uint32, FString> StringProperties;
	TArray<float> ArmorResistances;
	int32 ItemType = 0;
	bool bHasWeaponProfile = false, bWeaponMaxVelocityEstimated = false;
	int32 WeaponSkill = 0;
	double WeaponDamageMod = 0, WeaponLength = 0, WeaponMaxVelocity = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ObjectGuid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bSuccess = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Level = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bHasValue = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CreatureType = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bHasBurden = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 AttributeHighlights = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 AttributeColors = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ArmorEnchantments = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 WeaponEnchantments = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ResistanceEnchantments = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Value = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Burden = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Health = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxHealth = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Stamina = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxStamina = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Mana = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxMana = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Strength = 0;
    /** Presence in the creature appraisal, independent of zero-valued vitals. */
    bool bHasCreatureHealth = false, bHasCreatureAttributes = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Endurance = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Quickness = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Coordination = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Focus = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Self = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Damage = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 DamageType = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 WeaponTime = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float DamageVariance = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float WeaponOffense = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 DamageRating = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 DamageResistRating = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CritRating = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CritDamageRating = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CritResistRating = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CritDamageResistRating = 0;
	/** Body-part armor levels: Head, Chest, Abdomen, UpperArm, LowerArm, Hand, UpperLeg, LowerLeg, Foot. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<int32> ArmorLevels;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bIsCreature = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<int32> SpellIds;
	// Preserve appraisal's high-bit enchantment marker for retail presentation.
	// SpellIds remains the normalized list consumed by automation and loot rules.
	TArray<int32> SpellBookEntries;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Summary;
	/** PropertyString.Inscription (7) when present. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Inscription;
	/** Icon DID from world object / appraisal DID props when known. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 IconDid = 0;
};

/** Active enchantment from PlayerDescription / MagicUpdate* (excludes cooldown UI). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEActiveEnchantment
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SpellId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Layer = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SpellCategory = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 PowerLevel = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StatModType = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StatModKey = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float StatModValue = 0.f;
	/** Seconds; -1 = permanent / item. Server StartTime is typically negative elapsed. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float Duration = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float StartTime = 0.f;
	/** Monotonic receipt time; server StartTime is an offset at receipt, not a ticking clock. */
	double ReceivedAt = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CasterGuid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bBeneficial = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bVitae = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bCooldown = false;
};

/** Link / ping status for the LinkStatus panel + indicator. */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACELinkStatus
{
	GENERATED_BODY()

	/** Round-trip seconds from last PingResponse or EchoResponse. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float RoundTripSeconds = -1.f;
	/** Approximate packet loss 0..100 from retransmit requests in the last ten seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float PacketLossPercent = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float SecondsSinceLastPacket = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bConnected = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bHasPing = false;
};

/** Currently selected world object (client-side; QueryHealth keeps server target in sync). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACESelectedObject
{
	GENERATED_BODY()
	/** Local selection action serial; health/name refreshes preserve this value. */
	uint32 SelectionSerial = 0;

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Guid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float HealthFraction = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bShowHealth = false;
	/** Item mana is supplied by QueryItemMana, independently of structure/durability. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") float ManaFraction = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bShowMana = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bValid = false;
};

/** One fellowship member from FellowshipFullUpdate / UpdateFellow. */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEFellowshipMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Guid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Level = 1;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 HealthCur = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 HealthMax = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StaminaCur = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StaminaMax = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ManaCur = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ManaMax = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bShareLoot = true;
	// Monotonic receive time; automation must not heal from stale fellowship data.
	double VitalsReceivedAt = 0;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEFellowshipInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 LeaderGuid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bShareXP = true;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bEvenShare = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bOpen = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bLocked = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FACEFellowshipMember> Members;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEFriendInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Guid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bOnline = false;
};

/** One entry of the retail SquelchDB (GameEvent SetSquelchDB 0x01F4). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACESquelchEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Guid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	/** Account-wide squelch (retail forces AllChannels for these). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bAccount = false;
	/** SquelchMask of filtered channels. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Mask = 0;
	// Retail vlong is a 128-bit channel mask, not four copies to OR together.
	TArray<uint32> FilterWords;
	bool Blocks(uint32 Type) const
	{
		if (Type == 1)
		{
			if (FilterWords.IsEmpty()) return uint32(Mask) == MAX_uint32;
			for (int32 I=0;I<4;++I) if (!FilterWords.IsValidIndex(I) || FilterWords[I]!=MAX_uint32) return false;
			return true;
		}
		if (Type>=128) return false;
		const uint32 Word=FilterWords.IsEmpty() ? (Type<32 ? uint32(Mask) : 0u)
			: (FilterWords.IsValidIndex(Type/32) ? FilterWords[Type/32] : 0u);
		return (Word & (1u<<(Type%32)))!=0;
	}
};

/** One row of the server's ContractTracker table (GameEvent 0x0314 / 0x0315). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEContractEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 ContractId = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Stage = 0;
	/** Remaining seconds supplied by ContractTracker; 0 when not timed. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") double TimeWhenDone = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") double TimeWhenRepeats = 0.0;
	double ReceivedAt = 0.0;
};

/** One buy / maintenance line item from GameEvent HouseData (0x0225). */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEHousePayment
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Required = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Paid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Wcid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString PluralName;
};

/** Retail slumlord offer, separate from the player's owned-house information. */
struct ACECLIENT_API FACEHouseProfile
{
	int32 LordGuid = 0, DwellingId = 0, OwnerGuid = 0;
	uint32 Flags = 0;
	int32 MinLevel = -1, MaxLevel = -1, MinRank = -1, MaxRank = -1, Type = 0;
	bool bMaintenanceFree = false;
	FString OwnerName;
	TArray<FACEHousePayment> Buy, Rent;
};

/** House panel state: GameEvent HouseData (0x0225) for owners, HouseStatus (0x0226) otherwise. */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEHouseInfo
{
	GENERATED_BODY()

	/** True once HouseData has arrived — the character owns (or co-owns) a dwelling. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bOwned = false;
	/** True once either HouseData or HouseStatus answered our HouseQuery. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bQueried = false;
	/** 1=cottage, 2=villa, 3=mansion, 4=apartment. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 HouseType = 0;
	/** Unix timestamps from the server. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int64 BuyTime = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int64 RentTime = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bMaintenanceFree = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FACEPosition Position;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FACEHousePayment> Buy;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FACEHousePayment> Rent;
	/** WeenieError from HouseStatus when no dwelling is owned. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 StatusError = 0;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEBookPageAuthor
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Guid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bIgnoreAuthor = false;
};

/** Open book state from BookDataResponse / BookPageDataResponse. */
USTRUCT(BlueprintType)
struct ACECLIENT_API FACEBookInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bOpen = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 BookGuid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxPages = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 NumPages = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MaxCharsPerPage = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CurrentPage = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString AuthorName;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Inscription;
	/** Page texts keyed by 0-based index (filled as BookPageData arrives). */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FString> Pages;
	/** A received empty page is different from a page whose text has not arrived. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<bool> PageTextIncluded;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FACEBookPageAuthor> PageAuthors;
	bool HasPageText(int32 Index) const
	{
		return Pages.IsValidIndex(Index) && PageTextIncluded.IsValidIndex(Index) && PageTextIncluded[Index];
	}
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEAllegianceMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Guid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Level = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Rank = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Gender = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 HeritageGroup = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bOnline = false;
	/** 0=monarch, 1=patron, 2=self, 3=vassal */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Role = 0;
	/** Unpassed XP this member is holding, and XP already passed up. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CPCached = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 CPTithed = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Loyalty = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Leadership = 0;
};

USTRUCT(BlueprintType)
struct ACECLIENT_API FACEAllegianceInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "ACE") bool bValid = false;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString AllegianceName;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 Rank = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 TotalMembers = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 TotalVassals = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString MonarchName;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 MonarchGuid = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FString PatronName;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 PatronGuid = 0;
	/** Full network records retain rank titles and online state for presentation. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FACEAllegianceMember Monarch;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FACEAllegianceMember Patron;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") FACEAllegianceMember Self;
	/** Own unpassed / passed allegiance XP, shown on the allegiance page footer. */
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SelfCPCached = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") int32 SelfCPTithed = 0;
	UPROPERTY(BlueprintReadOnly, Category = "ACE") TArray<FACEAllegianceMember> Vassals;
};

/** Retail tinkering tool WCID (Ust). */
namespace ACEWeenieClass
{
	constexpr int32 TinkeringTool = 20646;
}
