# Client plugins

AC:Unreal runs Lua plugins on Windows, Linux, PC VR, Quest and Android. Start with
the [public Lua API](PublicAPI.md), [companion guide](Companions.md) or
[machine-readable catalog](../../ClientMods/api.json) if you're writing a plugin.

## Manage plugins

Open **Plugins** at login, **Game Play → Manage Plugins** on desktop, or
**More → Manage Plugins** in VR. In chat, `/acueplugins` opens the same manager.
**Enable** grants the listed permissions. Enable the plugin again if an update
changes them. Automation needs **Start** after entering the world. Companions
start when enabled and logged in and can run alongside one automation driver.

The manager's **Open** restores the existing window. Closing it leaves the
plugin running. The bar's gear opens the manager. Drag icons to reorder them
while the UI is unlocked. **Show plugin bar** and **Horizontal plugin bar** set
its visibility and orientation. The **Enabled plugin limit** defaults to 16.
Use 0 for unlimited. Enabled tools count even when stopped.

Windows remember their position, size, opacity, visibility and pin state.
**Pin** hides the frame and locks the window. Hold **Quick-show pinned windows**
(Left Control by default) to restore the controls temporarily. The manager
sets global and per-plugin background opacity. **Options → Config → Keep UI
windows on screen** controls placement. **Options → User Interface Options →
Lua Plugin Text Scale** changes plugin text independently of chat.

Type `/acuehelp` for client commands, including `/acueucm`, `/acueitems`,
`/acueplugin <id> <command>`, `/acueperf` and `/acueobject`. The parser
also accepts `@`. Older plugin command spellings remain aliases. Use
`/acehelp` for the server's commands.

## Bundled plugins

- [Waypoint](Waypoint.md) provides a navigation arrow, pinned map, GoArrow atlas
  import and route planning.
- [Item Tools](ItemTools.md) handles stack merging, bulk giving, vendor exchanges,
  chest distribution and supply restocking.
- [Auto Craft](AutoCraft.md) finds recipes, prepares ingredients and crafts a
  chosen quantity. It also documents UCM ammunition crafting.
- [Dereth Foundry](Foundry.md) is an opt-in content workshop for server
  administrators. Commands use the server's admin permissions.
- [Local cosmetics](LocalCosmetics.md) documents the Windows provider API used
  by separately distributed appearance plugins.
- **Supply Ledger** tracks an item type across your packs and equipment. Select
  the item and click **+**. Counts and icons remain visible at zero, and each
  row has a remove button. Click a row to toggle item names.
- **Hunt Ledger** tracks XP and luminance over five minutes and since login or
  **Reset run**. Its level estimate uses XP earned during the current run.

### Third Eye

In **Setup**, add an exact or partial name or use **Add selected**. Matching
ignores case and treats punctuation literally. **Detected** lists nearby
matches with distance, selection and a Waypoint arrow. **Options** sets range,
chat alerts, and world/map markers. The plugin checks up to 512 nearby objects
already known to the client, within the same area and at most 240 metres.
Up to 32 matches can have markers.

On **Audio**, enable a chime independently of chat alerts. **Choose WAV** imports
a PCM16 mono/stereo WAV at 8–48 kHz, up to 10 seconds and 1 MiB. **Test** plays the
current sound, and **Use default** restores the bundled chime. **Watch for
matches** pauses scanning and alerts. The watch list and settings save per
character and server.

### Action Deck

Drag inventory or spell icons onto the deck. Drag between slots to swap
actions, right-click to edit, or drag out of the window to remove a shortcut.
Dragging the main backpack icon adds **Target self**. The gear sets individual
bindings, row modifiers, attack shortcuts and native bar visibility.

**Override** uses the deck's assigned keys. By default its three rows use
`1` through `=`, Ctrl plus those keys, and Shift plus those keys. An optional
fourth row uses Alt. Empty slots fall through to native bindings. Closing the
deck restores native hotkeys. **Lock** pins it. Hold **Quick-show pinned windows**
or right-click a pinned slot to restore the controls. Assignments and hotkeys
save per character and server.

Right-click a slot and choose **Add macro…** to run chat commands in sequence.
Use one `/` or `@` command per line, and `/wait 30000` for a local 30-second
pause. A macro allows up to 128 steps, 8,192 characters total and 500 characters
per command. Waits and spacing are capped at one hour. Click the slot or use
its hotkey to run it. **Stop macro** cancels remaining steps. Portal travel
defers further commands until arrival. Logout, a character/server change,
disconnect or disabling the plugin cancels the macro.

**Show target debuff and damage-over-time timers** shows estimates from
confirmed casts on the selected target. Item slots use a matching stack
from another owned pack if the original runs out. Hover a slot to see its
binding or why it is unavailable.

## UCM

Enable **Unattended Combat Manager** in Plugin Manager. In **Overview →
Configuration**, select profiles and enable the activities you want before
pressing **Start**. Use **UCM Micro**, the optional **UCM Start / Stop** binding,
or `/acueucm start` and `/acueucm stop` for quick control. For manual combat
with automatic looting or recovery, turn **Combat** off and enable the
activities you want. **Peace mode when idle** is off by default.

### Profiles and imports

Edit the selected profile in its tab. The six independent selections are
**Character**, **Routes**, **Loot**, **Mobs**, **Metas**, and **Vendors**. Files
are under `Saved/ClientPlugins/Profiles` (Mobs uses `Monsters` on disk).
Selections and activity switches are saved in `ucm-selection.json`.
Use **New** or **Delete** in the editing tabs. Deleted files go to
`Profiles/Deleted`. Selecting a different profile manually stops UCM.

The **Import** tab reads `.usd` character profiles, `.nav` routes, `.met`
metas and `.utl` loot rules from the configured folder, initially
`Saved/ClientPlugins/ImportInbox`. **Refresh** scans for added files. Individual
and bulk imports create named profiles and leave source files intact. Reimporting
a known file selects its saved conversion and preserves its native edits.
Check the report before activating an imported meta or loot profile. Supported
meta dependencies become named profiles and load when the meta selects them.
See [Virindi compatibility](VirindiCompatibility.md) for supported formats and
unsupported features.

On **Metas**, choose a starting state, override activities and order transitions.
The first matching transition wins. Native transitions include vital thresholds,
elapsed time, target availability and route completion. Stop/Start retains
eligible imported meta progress if the character, profile and script match.

### Route

Create a route with **+ New route**. At turns and door approaches, choose
**Add waypoint at my position**. **Special steps** adds use, portal, jump,
pause and recall actions. Record a destination point on the far side of a
portal or recall. **Route behavior** controls doors, looping, reversing and
following. Start joins the nearest reachable waypoint, then follows the route
in order.

After combat or looting, UCM rejoins the current route leg by a walkable shortcut
when available. It retraces the detour when blocked. **Edit position** on a
waypoint lets you enter coordinates or **Use my position**, then **Save position**
while the route is running. A saved change to the current point updates steering.

**Show route in world** previews recorded points and marks jumps in amber.
For following, select a player and enable **Follow Character** (**FC** in
Micro). **Follow around corners** (**C**) follows their observed positions.
Turning off follow also turns off navigation.

### Combat, buffs and recovery

- **Combat** sets attack mode, acquisition and attack ranges, power, and
  equipment or spell preferences. An empty equipment pool allows eligible
  inventory items. An explicit damage type in a mob rule constrains the
  weapon, spell or ammunition. Use **Mobs → Default** for shared rules, and
  **Details** for per-mob equipment, attack families, debuffs, pets, priority
  and matching. Rules can use name prefixes, exact matches like
  `name==Snow Tusker`, or expressions such as `species==Tusker && maxhp>1000`.
- For pets without automatic attacks, add an eligible essence to the
  **Equipment Pool**, enable **Summoning** and set the default attack mode to
  **No attacks** with **Pet → Auto**. Ignore, negative priority and **Pet →
  None** exclude a target. See [ammunition crafting](AutoCraft.md#ammunition-in-ucm)
  for bow, crossbow and atlatl restocking.
- **Buffs** lets you choose automatic highest or exact spell tiers and set
  family exceptions and **Rebuff before expiry**. **Buff only while idle**
  waits for nearby visible monsters to clear and caps routine refreshes at
  the final 10% of an effect's duration. **Force Buff** and recovery remain
  available during combat. Accept tell-based buff requests in Overview and
  edit exact role keywords in **Buff others**. Item buffs for another player
  require the server to expose their equipment.
- **Recovery** sets health, stamina and mana thresholds. UCM uses eligible
  spells or assessed supplies. **Prefer kits and consumables** changes priority,
  and **Use imported recovery order** restores a converted profile's order.
  Configure equipment-mana supplies there, including optional **Recharge
  equipment while UCM is stopped**. **Automatically fill empty mana stones**
  obtains eligible donors from corpse loot while looting is on, after normal
  loot rules have been considered.

### Loot Profile Editor

Open **Loot** in UCM or the standalone **LOOT** window. The first matching
rule decides Keep, Skip, Salvage, Sell or Read. Requirements can match item
properties, materials, spells or text. **Apply requirement** accepts the
requirement draft, then **Apply rule** saves it and stops UCM. Use **Keep #**
for count limits, choosing name or item-template matching. See
[loot matching](LootMatching.md) for conditions, property IDs and missing values.

**Weapon skill** tests a weapon's attack skill (`EquipSkill`). **Wield
requirement skill** tests its required skill (`WieldReqAttribute`).
**Weapon type** tests `WeaponMasteryCategory`. Combine requirements with AND
or OR. VT regular expressions retain their imported case sensitivity.

**Edit UTL rules / save compatible copy** opens the original-format editor.
It can preserve all Classic requirement types, custom expressions and unknown
blocks in a separate `.utl` copy. **Check compatibility of saved copy** tests
whether the rules can run in UCM. Unsupported requirements can be edited and
saved in UTL format. See [Virindi compatibility](VirindiCompatibility.md)
for the distinction between editable and executable rules.

In **Looting settings**, opt in to **Apply salvage rules to existing inventory**
to process owned items under the current Salvage rules. Use **Salvage
combination** for material/workmanship groups and value targets.
**Auto-combine stackable items** fills compatible partial stacks after both
server updates confirm each merge. **Announce looted rule matches** and
**Announce item ID matches** use the local **Plugin chat** category under
**Options → Chat**.

### Vendor supplies and diagnostics

In **Vendors**, open a vendor, refresh its stock and add the quantities to keep.
**Automatically buy saved supplies** buys shortages when you open that vendor
while UCM is running. It uses the vendor's spendable balance, including bank
funds reported by custom servers. Eligible trade notes can fund a pyreal
shortfall. Record the route and vendor Use point separately if you want UCM
to travel there.

Open **UCM Log** from the bar, Micro, the Log tab or `/acueucm log`. The latest
1,000 local events include the active route and waypoint when relevant.
**Problems only** filters warnings and errors, and **Copy log** copies the
visible history. The bounded log persists in `Saved/ClientPlugins/ucm-log.json`.

## Install a Lua plugin

Use **Open plugin folder** in Plugin Manager, or create
`Saved/ClientPlugins/Installed/<id>/`. Install the plugin's `plugin.json`,
`main.lua` and `default.json` together, then **Refresh** and enable it.
Refresh reloads installed plugins and stops running instances. IDs must
be unique, including bundled IDs. Quest installation uses ADB to reach app
storage.

The manifest declares identity, mode and permissions. Its optional `short_name`
sets a bar label up to four characters. `main.lua` returns a
`function(snapshot, profile)`; `default.json` supplies defaults. Manifest
`settings` rows use `key`, `label` and `type`. Use `bool`, `number`
with `min` and `max`, or `choice` with string `values`. Optional
`tools` entries `spells` and `route` add the shared editors.
Settings and named profiles live under `Saved/ClientPlugins`.

Start with [API Workbench](Examples/api_workbench/main.lua) for automation or
the [companion guide](Companions.md) for trackers. The [API reference](PublicAPI.md)
covers snapshots, permissions, action confirmation, UI, sandbox limits and
local messaging.
