#pragma once

#include "ACESession.h"
#include "UI/ACEUIResourceResolver.h"

namespace ACEItemDragIcon
{
	inline UTexture2D* Resolve(UACEUIResourceResolver* Resources, const FACESession* Session, int32 Guid, int32 SourceIconId)
	{
		if (!Resources) return nullptr;
		// The player's GUID represents the main backpack on the inventory and shortcut bars.
		if (Session && Guid != Session->GetPlayerGuid())
		{
			if (const FACEWorldObject* Item = Session->GetWorldObjects().Find(Guid))
			{
				// Retail IconData::RenderIcons saves the drag image before adding the slot background and underlay.
				return Resources->ResolveItemForeground(Item->IconId, Item->IconOverlayId, Item->UiEffects);
			}
		}
		return Resources->ResolveIconTexture(SourceIconId);
	}
}
