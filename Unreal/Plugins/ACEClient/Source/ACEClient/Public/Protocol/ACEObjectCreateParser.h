#pragma once

#include "CoreMinimal.h"
#include "ACETypes.h"
#include "Protocol/ACEBinaryReader.h"

/** Decoded ObjectCreate (0xF745) fields needed for world representation. */
struct ACECLIENT_API FACEDecodedObject
{
	uint32 CreateFlags1 = 0, CreateFlags2 = 0, PhysicsDataFlags = 0;
	uint16 PublicPhysicsScript = 0;
	int32 Guid = 0;
	FString Name;
	FString PluralName;
	int32 WeenieClassId = 0;
	int32 IconId = 0;
	int32 IconOverlayId = 0;
	int32 IconUnderlayId = 0;
	int32 CooldownId = 0;
	double CooldownDuration = 0.0;
	/** PublicWeenieDesc UiEffects flags (Magical blue glow, elemental tints, …). */
	uint32 UiEffects = 0;
	int32 ItemType = 0;
	int32 ContainerId = 0;
	int32 WielderId = 0;
	int32 MonarchGuid = 0;
	int32 PetOwnerId = 0;
	int32 Value = 0;
	int32 Burden = 0;
	int32 ContainersCapacity = 0;
	int32 ItemsCapacity = 0;
	int32 StackSize = 0;
	int32 MaxStackSize = 0;
	int32 Structure = 0;
	int32 MaxStructure = 0;
	/** PublicWeenieDesc ItemUseable bitmask (Usable). */
	int32 ItemUseable = 0;
	/** PublicWeenieDesc TargetType (ItemType mask of valid use targets). */
	int32 TargetType = 0;
	int32 MaterialType = 0;
	float SalvageWorkmanship = -1.f;
	int32 ObjectDescriptionFlags = 0;
	/** HookItemTypes identifies the hook container, unlike HookType on hookable items. */
	bool bIsHouseHook = false;
	uint32 HookItemTypes = 0;
	uint16 HookType = 0;
	int32 HouseOwnerId = 0;
	int32 SetupId = 0;
	int32 MotionTableId = 0;
	int32 SoundTableId = 0;
	int32 PhysicsEffectTableId = 0;
	float Scale = 1.f;
	float Friction = .95f;
	float Elasticity = .05f;
	FACEPosition Position;
	bool bHasPosition = false;
	bool bIsPlayer = false;
	bool bParseOk = false;
	/** PhysicsDesc Parent — equipped / held by another object. */
	int32 ParentGuid = 0;
	int32 ParentLocation = 0; // ACE.Entity.Enum.ParentLocation
	int32 PlacementId = 0;    // ACE.Entity.Enum.Placement
	uint32 CurrentWieldedLocation = 0; // ACE.Entity.Enum.EquipMask
	uint32 ValidLocations = 0;         // ACE.Entity.Enum.EquipMask
	int32 ClothingPriority = 0;
	bool bHasClothingPriority = false;
	int32 PhysicsState = 0;
	float UseRadius = 0.6f;
	/** PublicWeenieDesc AmmoType flags (Arrow/Bolt/Atlatl). */
	int32 AmmoType = 0;
	/** PublicWeenieDesc CombatUse (ACE.Entity.Enum.CombatUse). */
	int32 CombatUse = 0;
	/** PublicWeenieDesc SpellDID (weenie header Spell flag) — wand innate / item spell. */
	int32 SpellDID = 0;
	/** PublicWeenieDesc RadarBlipColor (ACE.Entity.Enum.RadarColor). */
	uint8 RadarBlipColor = 0;
	/** PublicWeenieDesc RadarBehavior (ACE.Entity.Enum.RadarBehavior). */
	uint8 RadarBehavior = 0;
	/** PhysicsDesc translucency: 0 = opaque, 1 = fully transparent. */
	float Translucency = 0.f;
	/** Raw AC-space angular velocity. */
	FVector Omega = FVector::ZeroVector;
	/** Raw AC-space linear velocity from PhysicsDesc (projectiles / missiles). */
	FVector Velocity = FVector::ZeroVector;
	bool bHasVelocity = false;
	/** Explicit PhysicsScript data ID (DID type 0x34). */
	int32 DefaultScriptId = 0;
	float DefaultScriptIntensity = 0.f;
	/** Interpreted ForwardCommand from PhysicsDesc Movement (door On/Off, corpse Dead). */
	int32 InitialMotionCommand = 0;
	/** Interpreted MotionStance from PhysicsDesc Movement (packed ushort). */
	int32 InitialMotionStyle = 0;
	/** PhysicsDesc trailing 9×uint16 timestamps. */
	uint16 PhysicsTimestamps[ACEPhysicsTimeStamp::Count] = {};
	bool bHasPhysicsTimestamps = false;
	FACEObjDesc Appearance;
};

struct ACECLIENT_API FACEObjectCreateParser
{
	static bool Parse(FACEBinaryReader& Reader, FACEDecodedObject& Out);
	/** Vendor ApproachVendor merchandise: Guid + PublicWeenieDesc only (no model/physics). */
	static bool ParseGameDataOnly(FACEBinaryReader& Reader, FACEDecodedObject& Out);
	static bool ParseModelData(FACEBinaryReader& Reader, FACEObjDesc& Out);
	static uint32 ReadPackedDword(FACEBinaryReader& Reader);
	static uint32 ReadPackedKnown(FACEBinaryReader& Reader, uint32 KnownType);

private:
	static bool ParsePhysicsData(FACEBinaryReader& Reader, FACEDecodedObject& Out);
	static bool ParseWeenieHeader(FACEBinaryReader& Reader, FACEDecodedObject& Out);
	static bool SkipRestrictionDB(FACEBinaryReader& Reader);
};
