#include "ACEDatSubsystem.h"
#include "Dat/ACEDatCursor.h"
#include "Dat/ACEDatDatabase.h"

void UACEDatSubsystem::RecomputePlayerStats(FACEPlayerVitals& V, const TArray<FACEActiveEnchantment>& Enchantments)
{
	// gmSkillUI::RebuildSkillList iterates the DAT catalog, not every skill in
	// PlayerDesc. ACE may send retired weapon/internal skills that retail never
	// displays. Keep the shared player-stat view (desktop, VR and Lua) restricted
	// to the loaded catalog on login and every live update. Do not filter until a
	// complete table is available, or hard-code a blacklist that breaks custom DATs.
	if (EnsureSkillTableLoaded())
		V.Skills.RemoveAll([this](const FACESkillInfo& Skill) { return !SkillInfoCache.Contains(Skill.SkillId); });
	if (!bVitalFormulasLoaded && EnsureLoaded() && PortalDat)
	{
		TArray<uint8> Bytes;
		if (PortalDat->ReadFile(0x0E000003u, Bytes) && Bytes.Num() >= 76)
		{
			FACEDatCursor Cursor(Bytes); bool Ok = true;
			Cursor.ReadU32(Ok);
			for (auto& Formula : VitalFormulas)
			{
				uint32 Values[6]; for (auto& Value : Values) Value = Cursor.ReadU32(Ok);
				if (Ok && Values[3] && Values[4] <= 6 && Values[5] <= 6)
					FMemory::Memcpy(Formula, Values, sizeof(Values));
			}
		}
		bVitalFormulasLoaded = true;
	}
	// CEnchantmentRegistry::CullEnchantmentsFromList / Duel: only the winning
	// enchantment in each category affects a quality. Suppressed item buffs still
	// exist in the registry, so summing every row overstates attributes and skills.
	auto Modifiers = [&](uint32 Type, int32 Key, float& Mul, float& Add)
	{
		Mul = 1.f; Add = 0.f;
		TMap<int32, const FACEActiveEnchantment*> Winners;
		for (const auto& E : Enchantments)
		{
			const uint32 Flags = static_cast<uint32>(E.StatModType);
			if (E.bCooldown || E.bVitae || !(Flags & Type)) { continue; }
			const bool Attack = Key == 41 || (Key >= 43 && Key <= 47) || Key == 49 || Key == 33 || Key == 34;
			const bool Defense = Key == 6 || Key == 7 || Key == 15 || Key == 48;
			if (!(Flags & 0x2000u) && E.StatModKey != Key
				&& !(Type == 0x10u && ((Attack && (Flags & 0x10000u)) || (Defense && (Flags & 0x20000u))))) { continue; }
			const auto* Previous = Winners.FindRef(E.SpellCategory);
			if (!Previous || E.PowerLevel > Previous->PowerLevel
				|| (E.PowerLevel == Previous->PowerLevel && E.StartTime >= Previous->StartTime))
			{
				Winners.Add(E.SpellCategory, &E);
			}
		}
		for (const auto& Pair : Winners)
		{
			const auto& E = *Pair.Value;
			if (E.StatModType & 0x4000) { Mul *= E.StatModValue; }
			else if (E.StatModType & 0x8000) { Add += E.StatModValue; }
		}
	};
	V.EffectiveAllegianceRank = -1;
	if (const int32* Rank = V.StatQualityInts.Find(30); Rank && *Rank >= 0)
	{
		float Mul, Add;
		Modifiers(0x4, 30, Mul, Add); // Int quality, AllegianceRank.
		V.EffectiveAllegianceRank = FMath::TruncToInt(*Rank * Mul + Add);
	}
	// Innate attribute augmentations are already in the server's starting values
	// (AttributeCache::InqAttribute). Their quality counts must not add them again.
	Modifiers(1, 1, V.StrengthEnchantMul, V.StrengthEnchantAdd);
	Modifiers(1, 2, V.EnduranceEnchantMul, V.EnduranceEnchantAdd);
	Modifiers(1, 3, V.QuicknessEnchantMul, V.QuicknessEnchantAdd);
	Modifiers(1, 4, V.CoordinationEnchantMul, V.CoordinationEnchantAdd);
	Modifiers(1, 5, V.FocusEnchantMul, V.FocusEnchantAdd);
	Modifiers(1, 6, V.SelfEnchantMul, V.SelfEnchantAdd);
	float Vitae = 1.f;
	for (const auto& E : Enchantments) { if (E.bVitae) { Vitae = E.StatModValue; } }
	for (auto& Skill : V.Skills)
	{
		if (Skill.bRecomputeRanksFromXp && EnsureXpTableLoaded())
		{
			// Skill::SetSkillAdvancementClass / ExperienceSystem: reinterpret the
			// same unsigned XP using the new curve. Full skill updates keep their
			// explicit server ranks, including those used by custom servers.
			const TArray<uint32>& Curve = Skill.AdvancementClass == 3 ? SpecializedSkillXpList : TrainedSkillXpList;
			if (!Curve.IsEmpty())
			{
				const uint32 Xp = static_cast<uint32>(Skill.XpSpent);
				int32 Low = 0, High = Curve.Num();
				while (Low < High)
				{
					const int32 Mid = Low + (High - Low) / 2;
					if (Curve[Mid] <= Xp) Low = Mid + 1;
					else High = Mid;
				}
				Skill.Ranks = FMath::Max(0, Low - 1);
				Skill.bRecomputeRanksFromXp = false;
			}
		}
		FString Name; uint32 Icon = 0;
		TryGetSkillInfo(Skill.SkillId, Name, Icon);
		const auto* Info = SkillInfoCache.Find(Skill.SkillId);
		auto Formula = [&](bool Buffed) -> int32
		{
			if (!Info || !Info->FormulaZ || Skill.AdvancementClass < static_cast<int32>(Info->MinLevel)) { return 0; }
			const int32 A = Buffed ? V.GetAttributeCurrent(Info->FormulaAttr1) : V.GetAttributeBase(Info->FormulaAttr1);
			const int32 B = Buffed ? V.GetAttributeCurrent(Info->FormulaAttr2) : V.GetAttributeBase(Info->FormulaAttr2);
			// SkillFormula::Calculate rounds, it does not truncate integer division.
			return FMath::RoundToInt((static_cast<double>(Info->FormulaW) + A * Info->FormulaX + B * Info->FormulaY) / Info->FormulaZ);
		};
		if (!Name.IsEmpty()) { Skill.Name = Name; }
		// Retail CACQualities::InqSkill (00592660) includes these augmentations
		// in both raw and enchanted values, so they do not produce a green bonus.
		int32 Innate = Skill.InitLevel + Skill.Ranks + FMath::Max(0, V.StatQualityInts.FindRef(0x16D)); // LumAugAllSkills
		// ACE also grants Enlightenment to trained/specialized skills, before
		// vitae, just as its extra health bonus is included in the vital below.
		if (Skill.AdvancementClass >= 2) { Innate += FMath::Max(0, V.StatQualityInts.FindRef(390)); }
		const bool Magic = Skill.SkillId == 31 || Skill.SkillId == 32 || Skill.SkillId == 33 || Skill.SkillId == 34 || Skill.SkillId == 43;
		const bool Melee = Skill.SkillId == 41 || Skill.SkillId == 44 || Skill.SkillId == 45 || Skill.SkillId == 46 || Skill.SkillId == 49;
		if ((Magic && V.StatQualityInts.FindRef(0x12E) > 0) || (Melee && V.StatQualityInts.FindRef(0x12C) > 0)
			|| (Skill.SkillId == 47 && V.StatQualityInts.FindRef(0x12D) > 0)) { Innate += 10; }
		Skill.Base = Innate + Formula(false);
		float Mul, Add; Modifiers(0x10, Skill.SkillId, Mul, Add);
		Skill.Current = FMath::Max(0, FMath::RoundToInt((Innate + Formula(true)) * Vitae * Mul + Add));
		// Retail applies these permanent gains after enchantments and vitae.
		// The skill window and UCM's base-skill wield check include them in GetDisplayBase().
		Skill.UnscaledAugmentationBonus = V.StatQualityInts.FindRef(0x146) > 0 ? 5 : 0;
		if (Skill.AdvancementClass == 3) { Skill.UnscaledAugmentationBonus += 2 * FMath::Max(0, V.StatQualityInts.FindRef(0x158)); }
		Skill.Current += Skill.UnscaledAugmentationBonus;
		if (Skill.SkillId == 22) { V.JumpSkillCurrent = Skill.Current; }
		if (Skill.SkillId == 24) { V.RunSkillCurrent = Skill.Current; }
	}
	// Maximums are derived values, never a historical high-water mark. Buff expiry
	// must lower them even when the corresponding Current packet arrives later.
	auto Vital = [&](int32 Key, int32 Base)
	{
		float Mul, Add; Modifiers(2, Key, Mul, Add);
		return FMath::Max(Base < 5 ? 1 : 5, FMath::RoundToInt(Base * Vitae * Mul + Add));
	};
	auto Formula = [&](int32 Index, bool Buffed)
	{
		const auto& F = VitalFormulas[Index];
		auto Attribute = [&](int32 Id) { return Buffed ? V.GetAttributeCurrent(Id) : V.GetAttributeBase(Id); };
		return FMath::RoundToInt((double(F[0]) + double(F[1])*Attribute(F[4])
			+ double(F[2])*Attribute(F[5])) / F[3]);
	};
	// CACQualities::InqAttribute2nd adds GearMaxHealth before enchantments.
	// ACE also publishes Enlightenment; its health bonus is two per level.
	const int32 HealthBonus = V.StatQualityInts.FindRef(379) + 2*V.StatQualityInts.FindRef(390);
	V.BaseMaxHealth = V.HealthStart + V.HealthRanks + Formula(0, false) + HealthBonus;
	V.BaseMaxStamina = V.StaminaStart + V.StaminaRanks + Formula(1, false);
	V.BaseMaxMana = V.ManaStart + V.ManaRanks + Formula(2, false);
	V.MaxHealth = Vital(1, V.HealthStart + V.HealthRanks + Formula(0, true) + HealthBonus);
	V.MaxStamina = Vital(3, V.StaminaStart + V.StaminaRanks + Formula(1, true));
	V.MaxMana = Vital(5, V.ManaStart + V.ManaRanks + Formula(2, true));
}
