# AC:Unreal / AC:VR release 160

Windows desktop, PC VR, native Linux, standalone Quest, and the Android phone and tablet preview share version **2026.11.18.160**.

## VR controls and inventory

- Cast targeted spells and use casting-item spells on standard ACE servers from the VR controls. Enhanced free-aim combat still requires a server that supports the VR extension.
- Equipped items can be inspected and removed through the VR inventory. Either controller can activate inventory items, and inventory refreshes no longer interrupt held clicks.
- Use **Unlock retail windows** below the desktop-style floating panel to move and resize individual retail windows.
- Tune **Inventory recenter radius** in VR settings from 30 to 120 degrees. The default is 55 degrees, and wider panels expand the radius so you can look toward their edges. This setting applies to body-anchored menus and saves in the headset.

## Desktop and custom-server fixes

- Waypoint town markers use geographic coordinates rather than map-art label positions, including Mayoi and Wai Jhou.
- Windows display scaling, including 135%, now scales the interface consistently and preserves text clipping boundaries.
- The journal's **New** button creates and selects a new page, with navigation and deletion controls updated as pages change.
- Vendors support server-managed alternate currency such as DreamWeave Class Ability Points without requiring an inventory token. Pending purchases reserve the balance, and rejected purchases release it.

## Sky animation and admin preview commands

- Classic and imported AC2 cloud movement now follows the synchronized sky clock. Imported clouds follow the classic sky's travel direction; sun and moon positions remain aligned with AC1. Imported cloud shadows advance with the sky clock, while falling rain keeps its normal speed.
- The optional [LocalSkyPreview server mod](https://github.com/Thwargle/ACE/tree/master/Source/Mods/LocalSkyPreview) provides `/weather`, `/time set`, and `/timelapse [multiplier]`. All require an admin character, including status/reset/stop and aliases; an admin account's `sudo` cannot bypass that restriction.
- `/timelapse` defaults to 4x. Use a positive multiplier such as `/timelapse 64` for faster transitions, `/timelapse stop` for 1x, or `/weather reset` to restore live time. Preview changes affect only the invoking session.
- Thanks to **gvbz** for the far-distance rendering contribution introduced in v159.

## Verification

Automated regression checks cover VR controls and panel recentering, journal pages, fractional DPI, vendor currency, minimap coordinates, and classic/imported sky behavior. Server checks cover admin permissions, custom clock speeds, and session isolation. Package validation does not replace a physical headset or live custom-server playtest.

Players supply their own AC1 data and, for the optional imported sky, their own Legions data. No retail game data, saved profiles, or server credentials are included.
