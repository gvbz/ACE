#include "Protocol/ACEObjectCreateParser.h"
#include "ACEOpcodes.h"
#include "ACEAppearanceComposition.h"

namespace ACEPhysicsDescFlags
{
	constexpr uint32 CSetup                 = 0x000001;
	constexpr uint32 MTable                 = 0x000002;
	constexpr uint32 Velocity               = 0x000004;
	constexpr uint32 Acceleration           = 0x000008;
	constexpr uint32 Omega                  = 0x000010;
	constexpr uint32 Parent                 = 0x000020;
	constexpr uint32 Children               = 0x000040;
	constexpr uint32 ObjScale               = 0x000080;
	constexpr uint32 Friction               = 0x000100;
	constexpr uint32 Elasticity             = 0x000200;
	constexpr uint32 STable                 = 0x000800;
	constexpr uint32 PeTable                = 0x001000;
	constexpr uint32 DefaultScript          = 0x002000;
	constexpr uint32 DefaultScriptIntensity = 0x004000;
	constexpr uint32 Position               = 0x008000;
	constexpr uint32 Movement               = 0x010000;
	constexpr uint32 AnimationFrame         = 0x020000;
	constexpr uint32 Translucency           = 0x040000;
}

namespace ACEWeenieHeaderFlags
{
	constexpr uint32 PluralName               = 0x00000001;
	constexpr uint32 ItemsCapacity            = 0x00000002;
	constexpr uint32 ContainersCapacity       = 0x00000004;
	constexpr uint32 Value                    = 0x00000008;
	constexpr uint32 Usable                   = 0x00000010;
	constexpr uint32 UseRadius                = 0x00000020;
	constexpr uint32 Monarch                  = 0x00000040;
	constexpr uint32 UiEffects                = 0x00000080;
	constexpr uint32 AmmoType                 = 0x00000100;
	constexpr uint32 CombatUse                = 0x00000200;
	constexpr uint32 Structure                = 0x00000400;
	constexpr uint32 MaxStructure             = 0x00000800;
	constexpr uint32 StackSize                = 0x00001000;
	constexpr uint32 MaxStackSize             = 0x00002000;
	constexpr uint32 Container                = 0x00004000;
	constexpr uint32 Wielder                  = 0x00008000;
	constexpr uint32 ValidLocations           = 0x00010000;
	constexpr uint32 CurrentlyWieldedLocation = 0x00020000;
	constexpr uint32 Priority                 = 0x00040000;
	constexpr uint32 TargetType               = 0x00080000;
	constexpr uint32 RadarBlipColor           = 0x00100000;
	constexpr uint32 Burden                   = 0x00200000;
	constexpr uint32 Spell                    = 0x00400000;
	constexpr uint32 RadarBehavior            = 0x00800000;
	constexpr uint32 Workmanship              = 0x01000000;
	constexpr uint32 HouseOwner               = 0x02000000;
	constexpr uint32 HouseRestrictions        = 0x04000000;
	constexpr uint32 PScript                  = 0x08000000;
	constexpr uint32 HookType                 = 0x10000000;
	constexpr uint32 HookItemTypes            = 0x20000000;
	constexpr uint32 IconOverlay              = 0x40000000;
	constexpr uint32 MaterialType             = 0x80000000;
}

namespace ACEWeenieHeaderFlags2
{
	constexpr uint32 IconUnderlay     = 0x01;
	constexpr uint32 Cooldown         = 0x02;
	constexpr uint32 CooldownDuration = 0x04;
	constexpr uint32 PetOwner         = 0x08;
}

namespace ACEObjectDescFlags
{
	constexpr uint32 Player               = 0x00000008;
	constexpr uint32 Door                 = 0x00001000;
	constexpr uint32 IncludesSecondHeader = 0x04000000;
}

uint32 FACEObjectCreateParser::ReadPackedDword(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(2))
	{
		return 0;
	}
	const uint16 Low = Reader.ReadUInt16();
	if ((Low & 0x8000) == 0)
	{
		return Low;
	}
	if (!Reader.CanRead(2))
	{
		return Low & 0x7FFF;
	}
	const uint16 High = Reader.ReadUInt16();
	return (static_cast<uint32>(Low & 0x7FFF) << 16) | High;
}

uint32 FACEObjectCreateParser::ReadPackedKnown(FACEBinaryReader& Reader, uint32 KnownType)
{
	return ReadPackedDword(Reader) | KnownType;
}

bool FACEObjectCreateParser::ParseModelData(FACEBinaryReader& Reader, FACEObjDesc& Out)
{
	Out = FACEObjDesc();
	if (!Reader.CanRead(4))
	{
		return false;
	}
	const uint8 Marker = Reader.ReadUInt8();
	if (Marker != 0x11)
	{
		return false;
	}
	const uint8 NumPalettes = Reader.ReadUInt8();
	Out.bHasWireSubPalettes = true;
	const uint8 NumTextures = Reader.ReadUInt8();
	const uint8 NumAnimParts = Reader.ReadUInt8();

	if (NumPalettes > 0)
	{
		Out.PaletteBaseId = static_cast<int32>(ReadPackedKnown(Reader, 0x04000000));
	}
	for (uint8 i = 0; i < NumPalettes; ++i)
	{
		FACEObjDescSubPalette Sub;
		Sub.SubPaletteId = static_cast<int32>(ReadPackedKnown(Reader, 0x04000000));
		if (!Reader.CanRead(2))
		{
			return false;
		}
		const uint8 OffsetByte = Reader.ReadUInt8();
		const uint8 LengthByte = Reader.ReadUInt8();
		// Match ACE.DatLoader.Entity.SubPalette: Offset *= 8; Length 0 → 256; then *= 8
		Sub.Offset = static_cast<int32>(OffsetByte) * 8;
		const int32 Length = LengthByte == 0 ? 256 : static_cast<int32>(LengthByte);
		Sub.NumColors = Length * 8;
		Out.WireSubPalettes.Add(Sub);
		ACEAppearanceComposition::AddSubPalette(Out, Sub);
	}
	for (uint8 i = 0; i < NumTextures; ++i)
	{
		if (!Reader.CanRead(1))
		{
			return false;
		}
		FACEObjDescTextureChange Tex;
		Tex.PartIndex = Reader.ReadUInt8();
		Tex.OldTexture = static_cast<int32>(ReadPackedKnown(Reader, 0x05000000));
		Tex.NewTexture = static_cast<int32>(ReadPackedKnown(Reader, 0x05000000));
		ACEAppearanceComposition::AddTextureChange(Out, Tex);
	}
	for (uint8 i = 0; i < NumAnimParts; ++i)
	{
		if (!Reader.CanRead(1))
		{
			return false;
		}
		FACEObjDescAnimPartChange Part;
		Part.PartIndex = Reader.ReadUInt8();
		Part.PartId = static_cast<int32>(ReadPackedKnown(Reader, 0x01000000));
		ACEAppearanceComposition::AddAnimPartChange(Out, Part);
	}
	Reader.Align();
	return true;
}

bool FACEObjectCreateParser::ParsePhysicsData(FACEBinaryReader& Reader, FACEDecodedObject& Out)
{
	using namespace ACEPhysicsDescFlags;
	if (!Reader.CanRead(8))
	{
		return false;
	}
	const uint32 Flags = Reader.ReadUInt32();
	Out.PhysicsDataFlags = Flags;
	Out.PhysicsState = static_cast<int32>(Reader.ReadUInt32());

	if (Flags & Movement)
	{
		if (!Reader.CanRead(4)) return false;
		const uint32 MoveLen = Reader.ReadUInt32();
		if (MoveLen > MAX_int32 - 4 || !Reader.CanRead(static_cast<int32>(MoveLen) + 4)) return false;
		const int32 MoveStart = Reader.Tell();
		FACEBinaryReader Motion(Reader.GetData() + MoveStart, static_cast<int32>(MoveLen));
		// InterpretedState: door On/Off resting pose, corpse Dead hold, etc.
		if (MoveLen >= 8)
		{
			const uint8 MovementType = Motion.ReadUInt8();
			Motion.ReadUInt8(); // motion flags
			const uint16 PackedStyle = Motion.ReadUInt16();
			Out.InitialMotionStyle = static_cast<int32>(PackedStyle);
			if (MovementType == 0)
			{
				const uint32 Packed = Motion.ReadUInt32();
				const uint32 MFlags = Packed & 0x7Fu;
				auto ReadU16Flag = [&](uint32 Flag, uint16& OutVal)
				{
					if ((MFlags & Flag) != 0)
					{
						if (!Motion.CanRead(2)) return false;
						OutVal = Motion.ReadUInt16();
					}
					return true;
				};
				uint16 StyleOverride = 0;
				uint16 ForwardCommand = 0;
				uint16 IgnoredCommand = 0;
				if (!ReadU16Flag(0x01u, StyleOverride) || !ReadU16Flag(0x02u, ForwardCommand)
					|| !ReadU16Flag(0x08u, IgnoredCommand) || !ReadU16Flag(0x20u, IgnoredCommand)) return false;
				for (uint32 SpeedFlag : {0x04u, 0x10u, 0x40u})
					if (MFlags & SpeedFlag)
					{
						if (!Motion.CanRead(4) || !FMath::IsFinite(Motion.ReadFloat())) return false;
					}
				if (StyleOverride != 0)
				{
					Out.InitialMotionStyle = static_cast<int32>(StyleOverride);
				}
				// On/Off = door rest; Dead/Sleeping/Sitting/Crouch/chat-pose = held idle.
				// Ready (0x0003) is the default when ForwardCommand isn't present.
				if (ForwardCommand == ACEMotion::OnCommandU16
					|| ForwardCommand == ACEMotion::OffCommandU16
					|| ACEMotion::IsHeldRestCommand(ForwardCommand))
				{
					Out.InitialMotionCommand = static_cast<int32>(ForwardCommand);
				}
				// Retail InterpretedMotionState::UnPack also reads the five-bit
				// MotionItem list. A resting body may put Dead/Sleeping here while
				// ForwardCommand remains Ready. Skipping it leaves a standing idle.
				const uint32 CommandCount = (Packed >> 7) & 0x1Fu;
				if (!Motion.CanRead(static_cast<int32>(CommandCount * 8))) return false;
				for (uint32 I = 0; I < CommandCount; ++I)
				{
					const uint16 Command = Motion.ReadUInt16();
					Motion.ReadUInt16(); // action stamp / autonomous bit
					if (!FMath::IsFinite(Motion.ReadFloat())) return false;
					if (ACEMotion::IsHeldRestCommand(Command)) Out.InitialMotionCommand = Command;
				}
			}
		}
		Reader.Seek(MoveStart + static_cast<int32>(MoveLen));
		Reader.ReadUInt32(); // IsAutonomous
	}
	else if (Flags & AnimationFrame)
	{
		// ACE writes Placement here when Movement is absent (held items / static props).
		if (!Reader.CanRead(4)) return false;
		Out.PlacementId = static_cast<int32>(Reader.ReadUInt32());
	}

	if (Flags & ACEPhysicsDescFlags::Position)
	{
		if (!Reader.CanRead(32)) return false;
		Out.Position = Reader.ReadPosition();
		Out.bHasPosition = Out.Position.IsValid();
	}

	if (Flags & MTable)
	{
		if (!Reader.CanRead(4)) return false;
		Out.MotionTableId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Flags & STable)
	{
		if (!Reader.CanRead(4)) return false;
		Out.SoundTableId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Flags & PeTable)
	{
		if (!Reader.CanRead(4)) return false;
		Out.PhysicsEffectTableId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Flags & CSetup)
	{
		if (!Reader.CanRead(4)) return false;
		Out.SetupId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Flags & Parent)
	{
		if (!Reader.CanRead(8)) return false;
		Out.ParentGuid = static_cast<int32>(Reader.ReadUInt32());
		Out.ParentLocation = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Flags & Children)
	{
		if (!Reader.CanRead(4)) return false;
		const uint32 ChildCount = Reader.ReadUInt32();
		for (uint32 i = 0; i < ChildCount; ++i)
		{
			if (!Reader.CanRead(8)) return false;
			Reader.ReadUInt32(); // Guid
			Reader.ReadUInt32(); // LocationId
		}
	}
	if (Flags & ObjScale)
	{
		if (!Reader.CanRead(4)) return false;
		Out.Scale = Reader.ReadFloat();
	}
	if (Flags & Friction) { if (!Reader.CanRead(4)) return false; Out.Friction = Reader.ReadFloat(); }
	if (Flags & Elasticity) { if (!Reader.CanRead(4)) return false; Out.Elasticity = Reader.ReadFloat(); }
	if (Flags & Translucency)
	{
		if (!Reader.CanRead(4)) return false;
		Out.Translucency = FMath::Clamp(Reader.ReadFloat(), 0.f, 1.f);
	}
	if (Flags & Velocity)
	{
		if (!Reader.CanRead(12)) return false;
		Out.Velocity.X = Reader.ReadFloat();
		Out.Velocity.Y = Reader.ReadFloat();
		Out.Velocity.Z = Reader.ReadFloat();
		Out.bHasVelocity = !Out.Velocity.IsNearlyZero();
	}
	if (Flags & Acceleration)
	{
		if (!Reader.CanRead(12)) return false;
		Reader.ReadFloat(); Reader.ReadFloat(); Reader.ReadFloat();
	}
	if (Flags & Omega)
	{
		if (!Reader.CanRead(12)) return false;
		Out.Omega.X = Reader.ReadFloat();
		Out.Omega.Y = Reader.ReadFloat();
		Out.Omega.Z = Reader.ReadFloat();
	}
	if (Flags & DefaultScript)
	{
		if (!Reader.CanRead(4)) return false;
		Out.DefaultScriptId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (Flags & DefaultScriptIntensity)
	{
		if (!Reader.CanRead(4)) return false;
		Out.DefaultScriptIntensity = Reader.ReadFloat();
	}

	// 9 x ushort timestamps (PhysicsTimeStamp Position..Instance)
	if (!Reader.CanRead(18)) return false;
	for (int32 i = 0; i < ACEPhysicsTimeStamp::Count; ++i)
	{
		Out.PhysicsTimestamps[i] = Reader.ReadUInt16();
	}
	Out.bHasPhysicsTimestamps = true;
	Reader.Align();
	return true;
}

bool FACEObjectCreateParser::SkipRestrictionDB(FACEBinaryReader& Reader)
{
	if (!Reader.CanRead(12)) return false;
	Reader.ReadUInt32(); // Version
	Reader.ReadUInt32(); // OpenStatus
	Reader.ReadUInt32(); // MonarchID
	if (!Reader.CanRead(4)) return false;
	const uint16 Count = Reader.ReadUInt16();
	Reader.ReadUInt16(); // NumBuckets
	for (uint16 i = 0; i < Count; ++i)
	{
		if (!Reader.CanRead(8)) return false;
		Reader.ReadUInt32();
		Reader.ReadUInt32();
	}
	return true;
}

bool FACEObjectCreateParser::ParseWeenieHeader(FACEBinaryReader& Reader, FACEDecodedObject& Out)
{
	using namespace ACEWeenieHeaderFlags;
	using namespace ACEWeenieHeaderFlags2;
	if (!Reader.CanRead(4)) return false;
	const uint32 WeenieFlags = Reader.ReadUInt32();
	Out.CreateFlags1 = WeenieFlags;
	Out.Name = Reader.ReadString16L();
	Out.WeenieClassId = static_cast<int32>(ReadPackedDword(Reader));
	Out.IconId = static_cast<int32>(ReadPackedKnown(Reader, 0x06000000));
	if (!Reader.CanRead(8)) return false;
	Out.ItemType = static_cast<int32>(Reader.ReadUInt32());
	Out.ObjectDescriptionFlags = static_cast<int32>(Reader.ReadUInt32());
	Out.bIsPlayer = (Out.ObjectDescriptionFlags & ACEObjectDescFlags::Player) != 0;
	Reader.Align();

	uint32 WeenieFlags2 = 0;
	if (Out.ObjectDescriptionFlags & ACEObjectDescFlags::IncludesSecondHeader)
	{
		if (!Reader.CanRead(4)) return false;
		WeenieFlags2 = Reader.ReadUInt32();
		Out.CreateFlags2 = WeenieFlags2;
	}

	auto Need = [&](int32 N) { return Reader.CanRead(N); };

	if (WeenieFlags & PluralName) Out.PluralName=Reader.ReadString16L();
	// NOTE: ItemsCapacity/ContainersCapacity/CombatUse are single bytes on the wire (per the
	// real CreateObject/PublicWeenieDesc layout), NOT 4-byte ints — the underlying ACE server
	// fields are nullable byte/sbyte, but only 1 byte is actually serialized when present.
	// Reading these as int32 was silently consuming 3 extra bytes each time the flag was set,
	// desyncing every field parsed afterwards (this is what broke the player's own ObjectCreate,
	// since players always have ItemsCapacity+ContainersCapacity set — see "ObjectCreate parse
	// failed" / Enter World never completing).
	if (WeenieFlags & ItemsCapacity) { if (!Need(1)) return false; Out.ItemsCapacity = Reader.ReadUInt8(); }
	if (WeenieFlags & ContainersCapacity) { if (!Need(1)) return false; Out.ContainersCapacity = Reader.ReadUInt8(); }
	if (WeenieFlags & AmmoType)
	{
		if (!Need(2)) return false;
		Out.AmmoType = static_cast<int32>(Reader.ReadUInt16());
	}
	if (WeenieFlags & Value) { if (!Need(4)) return false; Out.Value = static_cast<int32>(Reader.ReadUInt32()); }
	if (WeenieFlags & Usable)
	{
		if (!Need(4)) return false;
		Out.ItemUseable = static_cast<int32>(Reader.ReadUInt32());
	}
	if (WeenieFlags & UseRadius)
	{
		if (!Need(4)) return false;
		Out.UseRadius = Reader.ReadFloat();
	}
	if (WeenieFlags & TargetType)
	{
		if (!Need(4)) return false;
		Out.TargetType = static_cast<int32>(Reader.ReadUInt32());
	}
	if (WeenieFlags & UiEffects)
	{
		if (!Need(4)) return false;
		Out.UiEffects = Reader.ReadUInt32();
	}
	if (WeenieFlags & CombatUse)
	{
		if (!Need(1)) return false;
		Out.CombatUse = static_cast<int32>(Reader.ReadUInt8());
	}
	if (WeenieFlags & Structure) { if (!Need(2)) return false; Out.Structure = Reader.ReadUInt16(); }
	if (WeenieFlags & MaxStructure) { if (!Need(2)) return false; Out.MaxStructure = Reader.ReadUInt16(); }
	if (WeenieFlags & StackSize) { if (!Need(2)) return false; Out.StackSize = Reader.ReadUInt16(); }
	if (WeenieFlags & MaxStackSize) { if (!Need(2)) return false; Out.MaxStackSize = Reader.ReadUInt16(); }
	if (WeenieFlags & Container)
	{
		if (!Need(4)) return false;
		Out.ContainerId = static_cast<int32>(Reader.ReadUInt32());
	}
	if (WeenieFlags & Wielder)
	{
		if (!Need(4)) return false;
		const int32 WielderGuid = static_cast<int32>(Reader.ReadUInt32());
		Out.WielderId = WielderGuid;
		// PhysicsDesc Parent is authoritative when present; Wielder is the fallback for
		// equipped items that only advertise the weenie header relationship.
		if (Out.ParentGuid == 0 && WielderGuid != 0)
		{
			Out.ParentGuid = WielderGuid;
		}
	}
	if (WeenieFlags & ValidLocations)
	{
		if (!Need(4)) return false;
		Out.ValidLocations = Reader.ReadUInt32();
	}
	if (WeenieFlags & CurrentlyWieldedLocation)
	{
		if (!Need(4)) return false;
		const uint32 WieldedLoc = Reader.ReadUInt32();
		Out.CurrentWieldedLocation = WieldedLoc;
		// ACE only assigns a real ParentLocation for hand-held weapons. Clothing/armor/cloak
		// keep ParentLocation.None (0) — they modify the wearer's ObjDesc and must NOT be
		// spawned as separate attached meshes (that piles a torso chunk at the player's feet).
		if (Out.ParentGuid != 0 && Out.ParentLocation == 0)
		{
			constexpr uint32 MeleeWeapon = 0x00100000;
			constexpr uint32 Held = 0x01000000;
			constexpr uint32 TwoHanded = 0x02000000;
			constexpr uint32 Shield = 0x00200000;
			constexpr uint32 MissileWeapon = 0x00400000;
			if (WieldedLoc & (MeleeWeapon | Held | TwoHanded))
			{
				Out.ParentLocation = 1; // RightHand
			}
			else if (WieldedLoc & Shield)
			{
				Out.ParentLocation = 3; // Shield
			}
			else if (WieldedLoc & MissileWeapon)
			{
				// Bow/crossbow → LeftHand; thrown → RightHand (approx; Physics Parent is authoritative).
				Out.ParentLocation = 2; // LeftHand
			}
			// Equipped ammunition is an inventory stack, not a quiver attachment.
			// Only PhysicsDesc Parent or a later reload ParentEvent may make it visible.
			// Otherwise it appears at the shoulder on login even with a wand equipped.
			// Clothing/armor/cloak and unloaded ammo keep ParentLocation.None (0).
		}
	}
	if (WeenieFlags & Priority) { if (!Need(4)) return false; Out.ClothingPriority = Reader.ReadUInt32(); Out.bHasClothingPriority = true; }
	if (WeenieFlags & RadarBlipColor)
	{
		if (!Need(1)) return false;
		Out.RadarBlipColor = Reader.ReadUInt8();
	}
	if (WeenieFlags & RadarBehavior)
	{
		if (!Need(1)) return false;
		Out.RadarBehavior = Reader.ReadUInt8();
	}
	if (WeenieFlags & PScript) { if (!Need(2)) return false; Out.PublicPhysicsScript = Reader.ReadUInt16(); }
	if (WeenieFlags & Workmanship)
	{
		if (!Need(4)) return false;
		Out.SalvageWorkmanship = Reader.ReadFloat();
		if (!FMath::IsFinite(Out.SalvageWorkmanship) || Out.SalvageWorkmanship < 0.f) return false;
	}
	if (WeenieFlags & Burden) { if (!Need(2)) return false; Out.Burden = Reader.ReadUInt16(); }
	if (WeenieFlags & Spell)
	{
		if (!Need(2)) return false;
		Out.SpellDID = static_cast<int32>(Reader.ReadUInt16());
	}
	if (WeenieFlags & HouseOwner) { if (!Need(4)) return false; Out.HouseOwnerId = Reader.ReadUInt32(); }
	if (WeenieFlags & HouseRestrictions) { if (!SkipRestrictionDB(Reader)) return false; }
	if (WeenieFlags & HookItemTypes)
	{
		if (!Need(4)) return false;
		Out.HookItemTypes = Reader.ReadUInt32();
		Out.bIsHouseHook = true;
	}
	if (WeenieFlags & Monarch) { if (!Need(4)) return false; Out.MonarchGuid = static_cast<int32>(Reader.ReadUInt32()); }
	if (WeenieFlags & HookType) { if (!Need(2)) return false; Out.HookType = Reader.ReadUInt16(); }
	if (WeenieFlags & IconOverlay) { Out.IconOverlayId = static_cast<int32>(ReadPackedKnown(Reader, 0x06000000)); }
	if (WeenieFlags2 & IconUnderlay) { Out.IconUnderlayId = static_cast<int32>(ReadPackedKnown(Reader, 0x06000000)); }
	if (WeenieFlags & MaterialType)
	{
		if (!Need(4)) return false;
		Out.MaterialType = static_cast<int32>(Reader.ReadUInt32());
	}
	if (WeenieFlags2 & Cooldown) { if (!Need(4)) return false; Out.CooldownId = Reader.ReadInt32(); }
	if (WeenieFlags2 & CooldownDuration)
	{
		if (!Need(8)) return false;
		Out.CooldownDuration = Reader.ReadDouble();
		if (!FMath::IsFinite(Out.CooldownDuration) || Out.CooldownDuration < 0.0) return false;
	}
	if (WeenieFlags2 & PetOwner) { if (!Need(4)) return false; Out.PetOwnerId = Reader.ReadInt32(); }

	Reader.Align();
	return true;
}

bool FACEObjectCreateParser::Parse(FACEBinaryReader& Reader, FACEDecodedObject& Out)
{
	Out = FACEDecodedObject();
	if (!Reader.CanRead(4))
	{
		return false;
	}
	Out.Guid = static_cast<int32>(Reader.ReadUInt32());
	if (!ParseModelData(Reader, Out.Appearance))
	{
		return false;
	}
	if (!ParsePhysicsData(Reader, Out))
	{
		return false;
	}
	if (!ParseWeenieHeader(Reader, Out))
	{
		return false;
	}

	// Match ACE Creature_Equipment.GetPlacementLocation for held item part frames.
	// ParentLocation.None (0) means worn clothing — do not invent a hand Placement.
	if (Out.ParentGuid != 0 && Out.PlacementId == 0 && Out.ParentLocation != 0)
	{
		Out.PlacementId = ACEPlacementFromParentLocation(Out.ParentLocation);
	}
	Out.bParseOk = true;
	return true;
}

bool FACEObjectCreateParser::ParseGameDataOnly(FACEBinaryReader& Reader, FACEDecodedObject& Out)
{
	Out = FACEDecodedObject();
	if (!Reader.CanRead(4))
	{
		return false;
	}
	Out.Guid = static_cast<int32>(Reader.ReadUInt32());
	if (!ParseWeenieHeader(Reader, Out))
	{
		return false;
	}
	Out.bParseOk = true;
	return true;
}
