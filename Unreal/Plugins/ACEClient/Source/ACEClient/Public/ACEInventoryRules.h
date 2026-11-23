#pragma once

#include "ACETypes.h"

// Retail ItemHolder::DetermineUseResult (00588460), for an object owned by the
// player. World pickup, games and trade have their own non-owned branches.
enum class EACEOwnedItemUse : uint8
{
    None = 0, Use = 1, Backpack = 2, WieldRight = 3, AutoWear = 4,
    Salvage = 6, WieldLeft = 8
};

namespace ACEInventoryRules
{
    // Retail ItemHolder::CheckHookStatus (00586E90). Ownership here means
    // the house has an owner; the server separately checks access permissions.
    inline bool CheckHookStatus(const FACEWorldObject& Item, const FACEWorldObject& Container)
    {
        if (!Container.HookType || !Container.HookItemTypes) return true;
        return Container.HouseOwnerId && (Item.HookType & Container.HookType)
            && (uint32(Item.ItemType) & Container.HookItemTypes);
    }

    // Retail drops insert before the target ID, or append over an empty cell.
    // Resolve against the current list: transfers can compact it during a drag.
    // INDEX_NONE cancels a self-drop or a drop onto an item that has disappeared.
    inline int32 DropInsertionIndex(const TArray<int32>& Items, int32 SourceGuid, int32 TargetGuid, bool bWholeStack)
    {
        if (SourceGuid == TargetGuid) return INDEX_NONE;
        int32 Index = TargetGuid ? Items.IndexOfByKey(TargetGuid) : Items.Num();
        if (Index == INDEX_NONE) return INDEX_NONE;
        const int32 Source = Items.IndexOfByKey(SourceGuid);
        if (bWholeStack && Source != INDEX_NONE && Source < Index) --Index;
        return bWholeStack && Index == Source ? INDEX_NONE : Index;
    }

    // Vendor stock is a unit count, not a stack count. An unlimited supply does
    // not make a non-stackable item stackable (armor and weapons stay at one).
    inline int32 VendorPurchaseLimit(const FACEWorldObject& Stock)
    {
        const int32 StackLimit = FMath::Max(1, Stock.MaxStackSize);
        return Stock.VendorQuantityAvailable < 0 ? StackLimit
            : FMath::Clamp(Stock.VendorQuantityAvailable, 0, StackLimit);
    }

    // gmToolbarUI::UpdateSelectedObject: consumables/trade notes in vendor
    // stock start at one; ammunition retains the displayed stack.
    inline int32 VendorInitialQuantity(const FACEWorldObject& Stock, int32 Limit)
    {
        return Limit <= 0 ? 0 : (uint32(Stock.ItemType) & 0x0DC41CB0u) ? 1 : Limit;
    }

    // Retail ItemHolder::IsMergeAttemptLegal: matching WCID, stack capacity,
    // and neither object being offered in trade (checked by the caller).
    inline int32 MergeAmount(const FACEWorldObject& Source, const FACEWorldObject& Target)
    {
        if (Source.Guid==Target.Guid || !Source.WeenieClassId || Source.WeenieClassId!=Target.WeenieClassId
            || Source.MaxStackSize<=1 || Target.MaxStackSize<=1) return 0;
        return FMath::Min(FMath::Max(1,Source.StackSize),FMath::Max(0,Target.MaxStackSize-FMath::Max(1,Target.StackSize)));
    }

    // ItemHolder::IsTargetCompatibleWithTargetingObject. The cursor previews
    // these public-descriptor rules; the server still decides whether use succeeds.
    inline bool IsTargetCompatible(const FACEWorldObject& Source, const FACEWorldObject& Target,
        int32 PlayerGuid, bool bSourceOwned, bool bTargetOwned, bool bTargetOffered)
    {
        if (!Source.Guid || !Target.Guid || bTargetOffered) return false;
        auto LeastLimited=[](uint32 Flags)
        {
            for (uint32 Bit=0x20; Bit>=2; Bit>>=1) if (Flags&Bit) return Bit;
            return Flags&0x80;
        };
        const uint32 SourceUse=static_cast<uint32>(Source.ItemUseable);
        const uint32 TargetUse=SourceUse>>16;
        if (!bSourceOwned && (LeastLimited(SourceUse)&12)) return false;
        if (!bTargetOwned)
        {
            const uint32 Limit=LeastLimited(TargetUse);
            if ((Limit&8) && (Target.Guid!=PlayerGuid || !(TargetUse&2))) return false;
            if (Limit&4) return false;
        }
        if (Target.Guid==PlayerGuid && !(TargetUse&2)) return false;
        return (Target.ItemType&Source.TargetType)!=0;
    }

    inline bool IsUsable(int32 Useability)
    {
        // ItemUses::IsUseable tests the No bit, including combinations of flags.
        return (static_cast<uint32>(Useability) & 1u) == 0;
    }

    inline uint32 LeastLimitedSourceUse(int32 Useability)
    {
        for (uint32 Bit = 0x20; Bit >= 2; Bit >>= 1)
            if (static_cast<uint32>(Useability) & Bit) return Bit;
        return 0;
    }

    inline bool IsContainer(const FACEWorldObject& Object)
    {
        return Object.IsOpenable() || Object.ItemsCapacity != 0 || Object.ContainersCapacity != 0;
    }

    // ItemHolder::DetermineUseResult opens portable containers on Use. Being
    // eligible for an explicit pickup does not make pickup their default action.
    inline bool IsWorldPickupOnUse(const FACEWorldObject& Object)
    {
        return Object.IsWorldLootable() && !IsContainer(Object);
    }

    inline EACEOwnedItemUse DetermineOwnedUse(const FACEWorldObject& Object, int32 PlayerGuid)
    {
        // Own equipped objects fall through the outer test to PlaceInBackpack.
        // Spellcasting's force-use path is separate from ordinary double-click.
        if (!Object.ContainerId && !(Object.ObjectDescriptionFlags & ACEObjectDescFlag::Stuck)
            && (!Object.WielderId || Object.WielderId == PlayerGuid) && !IsContainer(Object))
            return EACEOwnedItemUse::Backpack;
        if ((Object.CombatUse || (Object.ItemType & ACEItemType::Caster) || Object.IsWieldOnUse())
            && Object.WielderId != PlayerGuid)
            return (Object.ObjectDescriptionFlags & ACEObjectDescFlag::WieldLeft)
                ? EACEOwnedItemUse::WieldLeft : EACEOwnedItemUse::WieldRight;
        const uint32 Valid = static_cast<uint32>(Object.ValidLocations);
        const uint32 Current = static_cast<uint32>(Object.CurrentWieldedLocation);
        if (((Valid & 0x00007E00u) && !(Current & 0x00007E00u))
            || ((Valid & 0x080001FFu) && !(Current & 0x080001FFu))
            || ((Valid & 0x7C0F8000u) && !(Current & 0x7C0F8000u)))
            return EACEOwnedItemUse::AutoWear;
        if (static_cast<uint32>(Object.ItemType) & 0x20000000u)
            return EACEOwnedItemUse::Salvage;
        return IsUsable(Object.ItemUseable) ? EACEOwnedItemUse::Use : EACEOwnedItemUse::None;
    }
}
