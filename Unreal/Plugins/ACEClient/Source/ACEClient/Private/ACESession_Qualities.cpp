#include "ACESession.h"

void FACESession::HandleRemoveProperty(FACEBinaryReader& Reader, uint32 Family, bool bPublic)
{
	// CM_Qualities: byte stamp, [public GUID], property ID. No value/alignment,
	// even for strings. Removal and replacement share a WTimeStamper key.
	if (!Reader.CanRead(bPublic ? 9 : 5)) return;
	Reader.ReadUInt8();
	const int32 Guid = bPublic ? Reader.ReadInt32() : PlayerGuid;
	const int32 Key = Reader.ReadInt32();
	if (bPublic && Guid != PlayerGuid && Family == ACEOpcode::PrivateUpdatePropertyInt && (Key == 107 || Key == 108))
	{
		if (auto* Object = WorldObjects.Find(Guid))
		{
			// Missing ItemCurMana represents an empty stone, not its previous
			// filled reading. Remove and update share the transport stamp gate.
			if (Key == 107) Object->ItemCurrentMana = -2; else Object->ItemMaximumMana = -2;
			if (HasPlayerInventoryState(*Object)) ++InventoryDataRevision;
		}
		return;
	}
	if (!PlayerGuid || Guid != PlayerGuid) return;

	bool bChanged = false, bVitaeChanged = false;
	switch (Family)
	{
	case ACEOpcode::PrivateUpdatePropertyInt:
		bChanged = PlayerVitals.StatQualityInts.Remove(Key) != 0;
		if (!bChanged) break;
		// These are cached character qualities, unlike public object descriptor
		// fields. Clear the cached value along with the quality so panels and Lua
		// agree. StatResolver recomputes skill/vital augmentation contributions.
		switch (Key)
		{
		case 25: PlayerVitals.Level = 0; break;
		case 24: PlayerVitals.AvailableSkillCredits = 0; break;
		case 113: PlayerVitals.Gender = 0; break;
		case 188: PlayerVitals.HeritageGroup = 0; break;
		case 125: PlayerVitals.AgeSeconds = 0; break;
		case 43: PlayerVitals.NumDeaths = 0; break;
		case 181: PlayerVitals.ChessRank = 0; break;
		case 192: PlayerVitals.FishingSkill = 0; break;
		case 322: PlayerVitals.AetheriaUnlocked = 0; break;
		case 230: PlayerVitals.CarryingCapacityAugs = 0; break;
		case 129: VitaeCpPool = 0; bVitaeChanged = true; break;
		case 139: DeathLevel = 0; bVitaeChanged = true; break;
		default: break;
		}
		break;
	case ACEOpcode::PrivateUpdatePropertyInt64:
		bChanged = PlayerVitals.QualityInt64s.Remove(Key) != 0;
		// A removal can be the first authoritative balance message, before the
		// login description. Absence is then known, even without a prior map entry.
		if (Key == 6 && !PlayerVitals.bHasLuminanceBalance) bChanged = true;
		if (!bChanged) break;
		switch (Key)
		{
		case 1: PlayerVitals.TotalExperience = 0; break;
		case 2: PlayerVitals.AvailableExperience = 0; break;
		case 6:
			PlayerVitals.AvailableLuminance = 0;
			PlayerVitals.bHasLuminanceBalance = true; // Authoritatively absent, not waiting for login data.
			break;
		case 7: PlayerVitals.MaximumLuminance = 0; break;
		default: break;
		}
		break;
	case ACEOpcode::PrivateUpdatePropertyBool:
		bChanged = PlayerVitals.QualityBools.Remove(Key) != 0;
		break;
	case ACEOpcode::PrivateUpdatePropertyFloat:
		bChanged = PlayerVitals.QualityDoubles.Remove(Key) != 0;
		break;
	case ACEOpcode::PrivateUpdatePropertyString:
		bChanged = PlayerVitals.QualityStrings.Remove(Key) != 0;
		if (bChanged && Key == 5) PlayerVitals.TemplateName.Reset();
		break;
	default: return;
	}
	// Retail RemoveStat removes a quality and notifies its subscribers; it does
	// not call ACCWeenieObject::OnStatUpdated with a fabricated zero. Preserve
	// descriptor flags, inventory ownership, and the current combat mode (retail
	// ClientCombatSystem also retains its mode if InqInt(CombatMode) fails).
	if (bChanged) NotifyVitalsChanged();
	if (bVitaeChanged) OnEnchantmentsChanged.Broadcast();
}
