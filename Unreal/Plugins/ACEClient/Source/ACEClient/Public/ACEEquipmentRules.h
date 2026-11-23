#pragma once
#include "ACETypes.h"

namespace ACEEquipmentRules
{
    inline uint32 ClothingCoverage(const FACEWorldObject& Item, int64 Slot)
    {
        // Reduced armor can retain its original multi-slot EquipMask. Coverage
        // from the server is authoritative; infer only for older descriptions.
        return Item.bHasClothingPriority || Item.ClothingPriority != 0
            ? static_cast<uint32>(Item.ClothingPriority) : ACEInferClothingPriority(Slot, Item.ItemType);
    }
    // Retail's left weapon slot shares Shield. One-handed melee descriptors
    // advertise MeleeWeapon only; ACE explicitly permits this alternate slot.
    inline bool CanWieldInSlot(const FACEWorldObject& Item, int64 Slot)
    {
        return (Item.ValidLocations & Slot) != 0
            || (Slot == ACEEquipMask::Shield && Item.ValidLocations == ACEEquipMask::MeleeWeapon);
    }
}
