# Public plugin API 2

API 2 exposes native gameplay data, actions and UI to Lua plugins. API 1
manifests remain supported.

Install the [API Workbench](Examples/api_workbench) folder under
`Saved/ClientPlugins/Installed`, then refresh Manage Plugins, enable it and
press **Start**. Its activities default to off. Find action and field signatures
in [`ClientMods/api.json`](../../ClientMods/api.json).

## Find the functionality you need

- [Snapshots and permissions](#permissions-and-snapshots), [game intents](#game-intents), and [built-in policies](#reuse-or-replace-the-built-in-policies)
- [Native UI](#native-ui) and [target plates](#target-data-and-object-attached-plates)
- [Chat subscriptions and filters](#read-and-filter-chat), [local client channels](#channels-between-local-clients), and [companion APIs](Companions.md)
- [Profiles and services](#services-and-profiles), [loot matching](LootMatching.md), and [local cosmetics](LocalCosmetics.md)

## Lifecycle and ownership

Set `"api":2` and choose a mode.

- `"mode":"automation"` starts explicitly and stops on logout or character
  change. One automation driver owns actions at a time. Starting another stops
  the current driver. Bundled Item Tools temporarily pauses bundled UCM.
- `"mode":"companion"` starts when enabled and logged in and runs beside the
  automation driver. Closing either plugin window leaves the plugin running.

The host discovers every valid installed plugin. **Manage Plugins → Enabled
plugin limit** defaults to 16, with 0 meaning unlimited. Enabled plugins count
even when stopped, and disabling one frees a slot. The limit persists across
restarts. The manager rejects a limit below the enabled count. The player
controls this preference. Request `plugins` in `snapshot_fields` to read
each discovered ID's `{enabled, running}` state. The host preference is not
exposed as a Lua setter; `host.plugin_limit` in the catalog describes its schema.

Write `return function(snapshot, profile) ... end`. Return an intent and any
supported structured output in one table, or `nil` for no change. Omitted
presentation fields retain their current values. An explicit empty UI or marker
list clears it. An empty message or request list submits no work. Lua receives
copies of the snapshot and profile. Return an intent, service request or
`profile_patch` to change host state.

Automation uses native action gates and wakes on game updates. Each Lua VM
has a 32 MiB heap and 250,000 instructions per callback. Use
[`workavailable()`](#sandbox-helper-functions) for incremental jobs.

Companions default to a 0.5-second callback interval. The manifest may set
`update_interval` from 0.5 to 60 seconds. Due companions run round-robin, with
at most one callback per rendered frame. Input events may wake them sooner.

Companions may set `snapshot_fields` to the top-level fields they need.
Omit it for the full permitted snapshot. `api`, `time`, `unix_time`,
`player`, `server`, `state`, `error`, `events` and `capabilities` are
always included. Character qualities, `character_name`, `level` and action
results require `data`. Selective snapshots can use the cached inventory path. Some automation
fields still require building the full snapshot before filtering.

Character quality maps `char_ints`, `char_quads`, `char_doubles`, `char_bools`
and `char_strings` use decimal property IDs as string keys. Removed keys become
`nil`. Explicit zero, false and empty strings remain present. In `char_bools`,
compare numeric 0/1 explicitly. Use the derived `components_required` field,
since an absent bool 68 means components are required.
`char_strings["1"]` can fall back to the public character name.

`vendor_purchase_funds` is the open vendor's spendable balance shown as
"you have", or zero with no vendor open. It includes bank funds when the server
advertises them in `CoinValue`. Alternate currencies use the live physical or
server-managed balance with pending purchase reservations. `pyreals` counts
carried coins separately. Confirm purchases through inventory receipts.

## Plugin title and toolbar icon

Set a default title and icon filename in `plugin.json`:

```json
"presentation": { "title": "Kill Task Tracker", "icon": "tracker.png" }
```

Save the PNG under the plugin's `images/` directory. The title appears in the
window header and on hover. Without a usable title or icon, the host uses the
manifest name and a built-in emblem. Icons preserve aspect ratio in the
26-pixel toolbar slot.

With `ui` permission, API 1 companions and API 2 plugins can return runtime
`presentation`:

```lua
return {presentation = {title="Kill Task Tracker - 12 / 50",
                        icon="tracker.png",
                        window={opacity=0.35, pinned=true}}}
```

Omit `presentation` to keep the current values. Supplying it replaces the
runtime overrides. Missing fields inherit the manifest, and `{}` resets all
overrides. An empty title or icon uses the host fallback. Runtime overrides
last until character change or reload.

`window.opacity` is 0–1 and fades the window background and border.
`window.pinned` hides the frame and locks the position. **Quick-show pinned
windows** restores the frame temporarily. Player overrides take priority over
plugin defaults, including **Use this opacity for all plugins**.

Titles are single-line, up to 80 characters. Use a bare PNG filename of up
to 96 characters for `icon`. Toolbar and UI assets share the [PNG cache limits](#native-ui).
Refresh Manage Plugins after replacing a file.

## Native window controls

Manifest and runtime `presentation` accept these settings:

```lua
presentation = {
  controls = "toggle",       -- "start_stop" (default), "toggle", or "none"
  start_label = "Start tracker", stop_label = "Pause tracker",
  settings = false,
  status = false,
  scroll = false
}
```

The toggle starts or stops the plugin. Labels allow up to 40 characters on one
line and fall back to Start/Stop. `settings`, `status` and `scroll` default
to true. These options apply to custom native panels and inherit defaults
like title and icon. Profile and permission APIs remain available when
the host settings controls are hidden.

With `scroll=false`, fit your UI within the available `panel={width,height}`
in logical units. The snapshot reports it after the panel first lays out,
updates it on resize, and retains the last measurement when closed.

## Read and filter chat

`chat` grants `snapshot.chat_events`, with `raw_text`, formatted `text`,
`sender`, `color`/`chat_type`, `time` and `serial`.

Set `chat_subscriptions` in the manifest or return it from Lua to filter the
plugin's inbox. Rules are ORed, with all predicates within one rule required.
An empty list subscribes to no lines. Omitting runtime subscriptions retains
them. By default the plugin receives all chat. A changed subscription starts
at the newest line. Repeating a registration preserves pending messages.

```json
"chat_history": false,
"chat_subscriptions": [
  {"id":"kills","prefix":"You have killed","sender_type":"server"}
]
```

`chat_history=false` starts at the newest line. The default, true, allows
retained messages from this login. Refresh preserves the delivery cursor.
Each filtered inbox retains 2,048 matching events or 2 MiB of text. Callbacks
receive up to 128 events or 64 KiB. Check `chat_pending`, `chat_more`,
`chat_gap` and `chat_dropped` for backlog or loss.

Use the separate `chat_filter` permission and return `chat_filters` to alter
how lines display. Rules apply to incoming `raw_text`. Hidden lines still reach
plugin readers and any active chat log. Filtering applies to new lines.

```lua
chat_filters = {{id="quests", contains=" solves (", sender="", chat_type=0,
                 window=0, action="hide"}}
```

Both rule types require an ID of 1–64 characters and at least one predicate.
`contains`, `prefix`, `exact` and `pattern` allow up to 256 characters.
Text predicates are literal except `pattern`, which uses bounded regex.
`case_sensitive` defaults to false. `sender` is exact and `sender=""`
matches a missing sender. `chat_type` is 0–63. `sender_type` accepts
`player`, `npc`, `vendor`, `server` (no sender) or `unknown`, based on
known speaker data. It is also included on chat events.

Display rules accept `action="hide"` (default), `"show"` or `"color"`.
`color` accepts red, green, blue, yellow, white or `#RRGGBB` and alone
implies the color action. `window` is 0 for main chat, 1–4 for extra windows,
or -1/omitted for all windows. `seconds` can expire a rule after a positive
duration up to 300 seconds. `show` can bypass a window's category filter.
Hide rules win conflicts. Global filters and player squelches still apply.

The host validates each replacement atomically. At most 32 display rules
may be registered per plugin. Omit `chat_filters` to retain them or return
`{}` to clear them. Stop, disable, reload, logout or character change removes
the plugin's rules.

The [chat service](#services-and-profiles) sends outgoing server commands with
`say` permission. Register display rules with or before that request to cover
the first response. Local notices appear in the **Plugin chat** category.
See [Chat Tools](Examples/chat_tools/main.lua) for a working example.

## Target data and object-attached plates

Companions receive `selection` (a target or `nil`), `tracked_targets` and
`target_effects`, keyed by decimal GUID strings. Tracked objects include the
selection, plate targets and known objects with active confirmed timers, even
outside the 512-object snapshot list. Reading these fields sends no requests.

Target records can include:

- `id`, `name`, `wcid`, `item_type`, `monster`, `dead` and `object_type`.
  `object_type` identifies players, creatures, vendors, portals, containers,
  lifestones, doors, corpses and items. Raw `weenie_type` requires assessment
  property 9007.
- `creature_type`, `creature_class`, `title` and `level` when known.
  Players can include `pk_status` (`NPK`, `PK` or `PK Lite`).
- `vitals.health`, `.stamina` and `.mana` as `{current,max}` pairs when
  assessed. A newer health packet provides `fraction` (0–1) and any known
  `max`, and omits `current`. `assessment_age` and health sample `age`
  report seconds since receipt. A known zero is included.

Each effect has `spell`, DAT `icon`, `remaining` in seconds, `duration` and
`source="own_confirmed_cast"`. Timers estimate the duration of your confirmed
casts. They cannot account for other casters, early dispels or custom durations.
Expiry, death, loss of awareness, GUID reuse and session changes clear timers.

Return `world_plates` with both `ui` and `markers` permissions:

```lua
local t = s.selection
return {world_plates = t and {{
    id = t.id, name = t.name, role = t.title or t.creature_class or t.object_type,
    level = t.level, color = "yellow", selection_only = true,
    opacity = 1, background_opacity = 1,
    vr_opacity = 1, vr_background_opacity = 0.75,
    vitals = t.vitals, effects = s.target_effects[string.format("%.0f", t.id)] or {}
}} or {}}
```

The host follows rendered actors, advances countdowns locally and renders in
stereo in VR. Plates do not intercept input. Omission retains your list and
`{}` clears it. Replacements apply atomically. `selection_only` defaults to
true and hides a plate immediately on deselection. Use false to display an
additional tracked target. Stop, disable, reload, logout and character changes
remove the list.

- `opacity` and `background_opacity` accept finite values from 0 to 1 and
  default to 1. The first fades the composed plate; the second fades the
  backing. Optional `vr_opacity` and `vr_background_opacity` inherit
  their desktop values unless supplied.
- `color` styles the brackets and name. `text_color`, `background_color`,
  `health_color`, `stamina_color` and `mana_color` style those components.
  Colors accept green, yellow, red, blue, white, purple, black, transparent,
  `#RRGGBB` or `#RRGGBBAA` (sRGB with linear alpha).
- `scale` is 0.25–4 (default 1). `offset_x` and `offset_y` accept
  -2,048–2,048 plate-local logical pixels (default 0), right/down positive.
  Offsets scale with the plate. `vr_scale`, `vr_offset_x` and `vr_offset_y`
  override individual values in VR.
- Up to 4 plates per plugin and 16 globally. IDs must be unique. Names allow
  128 single-line characters and roles 80. `level` is a nonnegative integer.
  Vitals require a valid pair or fraction. Up to 16 effects per plate, each
  with a `0x06xxxxxx` icon and `remaining` from 0 to 86,400 seconds.
  Zero hides the effect. `spell` is optional.

Plates request background assessments unless `refresh=false`. They share the
global one-per-second appraisal budget and use at most alternate seconds
while automation runs. A successful target is revisited no sooner than two
seconds, and a failed or unanswered request waits 30 seconds. Off-target
vitals may refresh only when assessed. Normal health updates are also accepted.

See the [Target Plate example](Examples/target_plate/main.lua).

## Inventory-style icons

`item_icon` uses the inventory type background and composited artwork.
`icon` alone displays a plain DAT texture.

```lua
local item = s.inventory[1]
local row = {
  id="supply", type="item_icon", size=32, text=item.name,
  icon=item.icon, item_type=item.type,
  overlay=item.icon_overlay, underlay=item.icon_underlay, effects=item.ui_effects
}
```

Companions need `data` for the inventory snapshot. Icon fields are unsigned
32-bit DAT texture IDs including the `0x06000000` prefix. `size` is 16–128
pixels (default 32). Optional appearance values default to zero.

Assessed implements expose `innate_spell` with
`id,name,power,category,level,caster_target,beneficial,duration,flags,school,skill`
and `innate_spell_available`. `skill` is the spellcraft reported for the item.
Availability checks known requirements, ownership, trade state and item mana.
Beneficial instant self spells also expose `innate_recovery` and
`innate_recovery_available`. Use [`cast_item`](#game-intents) to activate an
eligible equipped implement.

Assessed physical weapons can expose `proc_debuff` with
`id,name,power,category,school,flags,duration,skill` and
`proc_debuff_available`. It describes the first appraised spell when that spell
is a timed offensive non-War/non-Item proc. Use physical attacks and observe
effect confirmation. Check `can_wield` and ammunition separately.
`debuff_spell` is restricted to UCM's attack path.

Door rows in `route_objects` include `door_open` and `door_identified`, plus
`door_locked` and `lock_difficulty` when assessed. Lockpicks expose
`lockpick_available` and `structure` for remaining uses. Retained picks are
eligible. Traded, equipped and exhausted picks are excluded.

## Reuse or replace the built-in policies

An automation manifest may select `"policy":"ucm"` or
`"policy":"itemtools"`. The shipped Lua function is then available as
`builtinpolicy` and its defaults as `builtinprofile`. Each plugin has its
own runtime and profile. Copy the values you want into your `default.json`.

```lua
return function(s, p)
  local intent = builtinpolicy(s, p) or {}
  intent.ui = {{id="status", text=intent.status or "Waiting"}}
  return intent
end
```

A custom decision function can return the same native intents. See
[UCM source](../../ClientMods/ucm/main.lua) and
[Item Tools source](../../ClientMods/itemtools/main.lua).
`mana_charges_when_off=true` lets the first eligible enabled automation
plugin with inventory permission supply settings for stopped-state recharge
while no driver is running. The native recharge policy runs that service.

`spellindex(spells, inventory, time, skill_margin, unavailable_until, components_required)`
returns `by_id, supplied, usable, groups`. The first two are keyed by spell ID,
with boolean entries in `supplied`. `usable` retains eligible tiers in
snapshot order. Groups reference the supplied records:

- `offense` contains usable non-beneficial spells.
- `instant_self` contains usable caster-targeted spells with zero duration.
- `other_buffs` contains usable beneficial timed spells that are neither
  caster-targeted nor self buffs.
- `by_category[category]` contains all usable tiers in the family.
- `buffs` contains known self buffs and beneficial timed Item Enchantment
  spells, including entries blocked by skill, supplies or retry delay.

`unavailable_until` maps numeric spell IDs to monotonic retry times.
`components_required` defaults to true. Pass `s.components_required ~= false`
to honor a server exemption. `components` maps the current formula's
component names to counts, including taper and focus adjustments.
`components_known` indicates a resolved formula. Older snapshots can fall
back to `scarabs`. The helper accepts up to 16,384 spells, 4,096 inventory
entries and 131,072 combined visits per callback.

## Sandbox helper functions

These helpers are available without loading a module or enabling UCM. They run
inside the same quota as the plugin; they do not grant game-action permissions.

- `lootmatch(rule, item, snapshot, scan)` evaluates one rule. Returns **true**,
  **false**, **nil** (needs assessment data), or **"pending"** (resume next callback).
  Always pass a mutable `scan` table and compare explicitly with `true` before
  acting. See the [full signature, conditions and example](LootMatching.md).
- `string.contains(text, value)` returns a case-sensitive literal-match boolean.
  Text is limited to 4,096 UTF-8 bytes and the search value to 256 bytes.
- `string.findplain(text, value, start)` returns the first literal match's one-based
  byte position, or nil. `start` defaults to 1 and is clamped to at least 1.
  Text/search limits are 16,384/8,192 UTF-8 bytes. This returns one index, not the
  start/end pair of the standard Lua `string.find` function.
- `regextest(text, pattern)` returns a boolean; `regexmatch(text, pattern)` returns
  a capture object or nil. Capture keys are strings, including `"0"` for the whole
  match. Text/pattern limits are 16,384/8,192 UTF-8 bytes. Use the supported bounded
  [VT regex subset](VirindiCompatibility.md), including scoped `x`/`n` options,
  named captures, .NET-numbered backreferences, capture/assertion conditionals
  and balancing groups. Matching is case-sensitive unless the pattern opts into
  case folding, such as `(?i)`. Cache keys and capture names always preserve
  case. Repeated captures return the most recent capture still on the group's
  stack; a completely balanced-away group is absent. Both numeric and named
  aliases are returned. Invalid expressions or work-limit failures raise an
  error and stop the plugin; they are not cached as ordinary nonmatches.
  Engine bounds are 4,096 input characters, 2,048 pattern characters, 32 capture
  groups, 2 ms per match and 64 KiB of backtracking storage, within the existing
  cumulative 20 ms callback allowance. Conditional/balancing patterns additionally
  allow up to 32 repetition constructs, 64 nested groups/assertions, capture slot
  numbers through 32 and 64 KiB of capture history.
- `ucmcommand(text)` compiles up to 4,096 UTF-8 bytes into a policy command record;
  `vtcommand` is the compatibility alias. It does not execute the command.
  Unsupported commands raise an error. The [policy service](#services-and-profiles)
  provides the corresponding command-queue bridge.
- `workavailable()` becomes false at 200,000 Lua instructions or 5 ms of cumulative
  regex work. Preserve your cursor and return to resume on the next callback;
  spinning until it becomes true cannot work. It never extends the hard limits.
- `spellindex(...)`: the [indexing helper above](#reuse-or-replace-the-built-in-policies)
  returns four tables and leaves the decision of what to cast to your policy.

The source-only Lua 5.4 VM exposes base functions plus `table`, `math`, `utf8`,
and bounded `string` operations. There is no `require`, `io`, `os`, `debug`, coroutine API, dynamic
loading, `pcall`/`xpcall`, metatable access, `collectgarbage`, or `print`. Standard
Lua pattern functions `string.find`, `match`, `gmatch`, `gsub`, and `dump` are
removed. Use the helpers above and the notice/log services for output.

## Channels between local clients

API 2 plugins with `client_messaging` exchange JSON with other Windows/Linux
client processes, including desktop VR. Use `messaging` within one process.

```lua
-- Join first and wait for snapshot.client_bus.connected.
return {client_channels={"team"}}
-- In a later callback, broadcast or supply to=peer.instance.
return {client_messages={{channel="team",data={operation="ready"}}}}
```

`client_channels` replaces subscriptions. Omission retains them and `{}`
leaves all channels. Names use 1–64 case-sensitive ASCII letters, digits,
underscores, dots or dashes. Send a JSON object on a subscribed channel.
Broadcasts omit the sending instance. Receivers get an `events` entry:

```lua
{type="client_message",sender="instance-id",character="Example",
 player=0x50000001,plugin="my_plugin",channel="team",
 data={operation="ready"},serial=123}
```

Request `client_bus` in selective snapshots for `{available, connected,
instance, channels, peers, error}`. Peer records contain `instance`,
`character`, `player` and `channels` for other clients on the same plugin
and server. Instance IDs are transient and change on reconnect or transport
restart. The host supplies sender metadata.

Channels are scoped by computer, OS user, configured server host/port,
reported server name, plugin ID and channel. Log both clients into the same
server address. The clients share a per-user loopback relay on a port in
37000–37999. Another client can take over when the host exits. A port conflict
sets a connection error.

Limits are 8 subscriptions per plugin, 64 per client, 32 clients, 8 messages
per callback, 20 sends/second per plugin and 80 across the client. A payload
allows 4,096 UTF-8 bytes and 10 nested levels. Socket queues allow 256 KiB,
and transport and plugin event inboxes each hold up to 64 events.
Changing subscriptions invalidates deliveries for the old set. Stop, disable,
reload, logout or session changes clear subscriptions and pending deliveries.

Delivery is best effort, with errors in `client_bus.error` and no replay on
reconnect. Use request IDs and acknowledgements where completion matters.
The receiver interprets each payload under its own permissions and action
gates. Keep credentials and secrets off the channel. Local apps are outside
its trust boundary.

See the [Client Channel example](Examples/client_channel/main.lua).
`/acueplugin client_channel hello` sends its test greeting.

## Permissions and snapshots

Declare only the permissions you need. Enable in Manage Plugins grants that
manifest's permission set; adding permissions requires enabling it again.

- `cast`: casting, force-buff control.
- `combat`: attack, cancellation, stance, power-bar updates.
- `navigation`: movement, facing, jumps, recalls, world use, route output/recording,
  automation selection and logout.
- `inventory`: appraisal, equipment, item use/application, moving, merging, giving.
- `loot`: corpse use/transfers, salvage, selling, purchases and trade-note funding.
  Item Tools' `it_*` intents require both inventory and loot.
- `say`, `fellowship`, `confirm`: outgoing chat/tells, fellowship operations/vitals
  subscription, and responding to a current server confirmation respectively.
- `chat`, `state`, `ui`, `markers`, `waypoint`, `messaging`, `commands`: the
  [companion capabilities](Companions.md). API 2 automation can use these too.
- `client_messaging`: opt-in channels between local client processes; see below.
- `admin_content`: explicit ACE content commands and local JSON/SQL workspace browsing; see [Dereth Foundry](Foundry.md). Server permissions still apply.
- `profiles`: write your own configuration/profile library and import/export data.
- `data`: opt a companion into the full gameplay snapshot. Ordinary automation
  already receives it. This is read access; it does not grant any game actions.
- `identify`: allow a companion to request one background appraisal with
  `action="identify", target=<guid>`. Targets must be owned or currently nearby.
  Requests share a one-per-second companion command limit and leave the player's
  selection, examine panel and automation ownership alone. Success means requested;
  wait for assessment data and handle a denied or unavailable appraisal.
- `sound`: play bounded WAV notifications from your plugin's `sounds` directory.

Companions may request observational permissions, profiles, data, identify, sound and say. The
optional `admin_content` capability provides bounded content authoring operations;
it does not grant general automation permissions or server privileges. Request a selection through a
UI button to select an observed world object or, in API 2, an owned inventory item.

Full snapshots include UCM gameplay data and Item Tools transfer, vendor
and inventory failure fields. See [the catalog](../../ClientMods/api.json)
for field names. With `identified=false`, appraisal-only properties may
be unknown. Automation requests appraisal using `inventory`.

`combat_death` records the most recent confirmed combat target death as
`id`, `time`, `cell`, `x`, `y`, `z` and `teleport_sequence`. Bound its age.
The record survives object deletion until superseded or the session changes.
The inventory field `auto_wield_left` comes from the live object descriptor.

`{action="identify",item=guid,pipeline=true,continue_work=true}` lets a policy
skip the appraisal wait for items directly inside the currently open
external container. The host rechecks membership, permissions and busy state.
Ordinary appraisal and companion requests retain their usual gates.
`continue_work` requests the next frame after an accepted nonblocking
action and a 50 ms poll when waiting. Appraisal and inventory receipts wake
the policy.

Track pending appraisals by item, cap the queue, bound retries and wait for
`identified=true` before using appraisal-only properties. Discard pending
work on container change or stop. UCM uses up to eight pending requests and
three attempts per item, spaced five seconds apart. Loot transfers still
require server ownership or quantity confirmation.

Combat equipment also exposes `combat_identified` and `combat_appraisal_cached`.
When `combat_appraisal_cached=true`, an owned weapon, shield or ammunition item
has a previous successful assessment restored from a character/server-specific
cache. Its flat combat fields (`damage_type`, damage/modifier, weapon skill/time,
variance, slayer/element bonuses and current `can_wield`/`wield_reason`) remain
available through swaps, background appraisal failures and relogging.
`combat_int_properties` and `combat_float_properties` contain only the retained
combat qualities (requirements, element, rend/cleave and combat bonuses).
Fresh assessments continue to use the ordinary property maps; choose the combat
map when present and otherwise use the ordinary map for weapon selection.
Records require matching GUID, WCID, name, type, setup, icon and material, expire
after 30 days, and are replaced by successful fresh appraisals. Current ownership
and character requirements are checked again before equipping.
This does **not** change `identified`: cached combat knowledge must never stand
in for a fresh appraisal in loot matching, salvage, mana/charge management or
other item-use decisions. Resource counts, spell lists and general loot qualities
are not restored by this cache. Use the server response as the final authority.

Assessed inventory entries expose `can_wield` and, when a requirement fails,
`wield_reason`. The reason identifies the requirement slot, type/property, required
value and observed value. Eligibility checks all four wield-requirement triplets,
including boolean, creature-type and ILT Int64Stat (13) checks used by custom servers.
For type 13, the property ID refers to a 64-bit character quality, not an integer
quality or skill. When that count has not been replicated, `can_wield=true` permits
an equip attempt and `wield_requires_server_check=true` indicates that the server
must validate the missing count. A received zero is known and checked normally;
other known unmet requirements still prevent the attempt. Already-worn
equipment is usable in the snapshot; sending a new equip action rechecks current
requirements. Server responses remain authoritative. `identified=true` alone
does not mean an item meets requirements or has a usable combat loadout.

An API 2 companion requesting only `inventory`, `inventory_revision` and
`appraisal_revision` through `snapshot_fields` builds the cached inventory section
without also building spells, vendor data or scanning combat targets. It still
requires data permission. Include additional fields only when your plugin needs them.

Item `icon` fields contain full DAT texture IDs for rendering. In
`int_properties`, the Decal compatibility keys `218103809` (Icon), `218103849`
(IconOverlay), and `218103850` (IconUnderlay) instead contain the packed texture
index, without the `0x06000000` type prefix. Imported vTank rules compare these
indices; for example, the level-4 overlay is `27703`, not `0x06006C37`.
These public fields are available before appraisal. Extended custom texture
indices retain all 24 index bits.

Beneficial `enchantments` include `duration` (the server's full duration, including
augmentation bonuses) and `remaining` (time left now). Permanent effects have a
negative duration. Use remaining time and power/category to decide whether a
refresh is needed; base spell duration alone does not describe the active timer.

`cooldowns` is a map from decimal item cooldown IDs to remaining seconds, for
example `s.cooldowns["213"]`. Match the ID in an item's `int_properties["280"]`;
the map removes the `0x8000` marker used by the wire enchantment ID. Items sharing
that ID share the timer. Values reflect server updates and elapsed time, including
timers restored at login. A missing key or zero means no known active timer for
that ID. Cooldowns are separate from `enchantments` and `harmful_enchantments`.
API 2 companions can request just `cooldowns` in `snapshot_fields` with `data`
permission; automation policies receive the same map.

Inventory entries expose `salvage_tool_available` for an owned Ust that is usable
from its current pack and not offered in trade (including its containing pack).
This remains true for a retained Ust: retention protects it from destruction,
but does not prevent using the tool. `resource_available` keeps the stricter
retention check for items being salvaged, sold or otherwise consumed.

`mana_supply_available` identifies an owned ManaStone-category supply that is
unequipped and outside trade, including its containing pack. Retention does not
exclude it; the flag does not authorize consuming a retained donor to fill an
empty stone. Also check `identified`, `usable`, `mana`, count and the shared
`cooldowns` map before using a supply. Availability is rechecked when executing
the action. Inventory `mana`, `max_mana`, `mana_empty` and integer properties
107/108 incorporate live server resource updates as well as appraisals; a zero
or removal clears a previous reading. A use receipt alone does not confirm a
refill: wait for the corresponding resource change while allowing other work
once the native busy gate releases.

API 2 adds `api`, `unix_time`, `server`, `state`, `events`, `error`,
`capabilities`, `plugins`, `automation_owner`, `profile_name`,
`waiting_action`, `log` and nearby `objects`. With `chat` permission,
`chat_events` uses an independent cursor, even when UCM is stopped.
`chat_serial` tracks the batch delivered to you and `chat_latest_serial`
tracks the latest received event. Unread events stay in the bounded backlog
(32,768 events or 8 MiB shared). See [chat delivery](#read-and-filter-chat)
and the [companion snapshot](Companions.md#callback-and-snapshot).

`action_error` remains the numeric server result associated with `action_serial`.
Plugins with data permission also receive native `events` for item use and casts:
`{type="use",stage="requested",source=<guid>,target=<guid>,wcid=<source wcid>,time=...}`
or `{type="cast",stage="requested",spell=<id>,target=<guid>,time=...}`. Applying an
item includes `target_wcid` when known. A `stage="completed"` event copies those
fields and adds `action_serial` and numeric `error`. Request events alone do not
confirm success; a completed cast does not prove that its projectile hit.
Completion reports the client action gate being released, including local
cancellation/timeouts; an error of zero alone is not proof an item was consumed.
Use inventory revisions or the corresponding server result to confirm effects.

Full snapshots also provide `action_events`, the most recent 64 completed use/cast
events for the current session. Data-authorized selective snapshots can request
it without building world, inventory or spell data. Unlike the per-plugin `events`
delivery queue, this bounded history repeats between callbacks: deduplicate by
`action_serial`. Each completion retains `requested_at` (monotonic client seconds),
plus the original source/target/spell and completion `time`/`error`. Cast completions
also include `confirmed`, indicating the tracked spell effect was confirmed;
this is not an offensive projectile-hit receipt. Match both action identity and
request time before acknowledging a pending operation. A new session clears the
history; overflow evicts the oldest entries, so retain bounded local pending state
and timeouts rather than treating absence as a success.

`inventory_failure_serial`, `inventory_failure_item` and
`inventory_failure_error` describe the latest inventory failure in this session.
They also support lightweight selective snapshots with `data` permission. Compare
the serial with the value before your request and match the unsigned item GUID;
an unrelated failure cannot reject that request. This is a last-result record,
not a durable event log. Giving items on ACE/GDLE is confirmed by the source's
inventory removal or reduced stack count, not a generic completed-use event.
Do not infer removal from a capped snapshot; keep a bounded timeout for missing
or overwritten evidence.

`combat_events` entries have `serial`, `target`, `hit`, `unhittable` and `time`.
Correlated single-target spell hits additionally carry `spell` and, when supplied
by the damage result, positive `damage`. Neither UseDone nor spell words count as
damage. Target snapshots expose `health` when supplied by an appraisal, or
`health_fraction` from a later health reply, with `health_observed_at` and known
`max_health`. Fractional health is not an exact integer measurement. A newer
health sample supersedes earlier damage estimates; avoid subtracting those hits
again. Unknown current health remains absent.

`activity_failure` is a separate optional object with the native failure's
`activity` and `status`. The stock policy also receives this through
`profile.ucm_activity_failure`. A rejected loot/route/equipment activity does not
shut down an API 2 automation plugin. Your policy decides how to retry or pause;
a Lua exception or quota violation still stops that plugin.

## Game intents

Return one action per callback. Check `ready` and `busy` before sending.
Confirm completion through the matching action result and resulting game state.
The host rechecks ownership, targets and server state at dispatch. An intent
may include a `status` message of up to 1,024 UTF-8 bytes. See
[`api.json`](../../ClientMods/api.json) for the complete action and permission
list.

- `cast` takes spell and target, with optional `fast_cast`. `attack` accepts
  `mode` 2 for melee or 4 for missile, `height` 1–3 and `power` 0–1.
  Use `cancel_attack`, `combat_mode` and `attack_bar` for combat controls.
- `cast_item` takes `item,spell,target` and requires `cast` and `inventory`.
  Equip the eligible implement first. It uses the implement in magic mode,
  targeting self for instant beneficial recovery or a living attackable
  non-player/non-pet for a timed harmful effect with clear projectile sight.
  The completion is a `use` receipt. Confirm the vital or effect separately.
- `cast_jiggle` is optional on `cast` and `cast_item`, and `jump_out_wand`
  on `cast_item`. Both default to false, require `navigation` and use normal
  collision. Manual movement, stop, session changes and completion or failure
  cancel them. `fast_cast` is a separate self-buff behavior.
- `move` accepts AC `cell,x,y,z`, a metre `arrival_radius` and optional
  pursuit `target`. Navigation also includes `face`, `jump`, `use_world`,
  `close_vendor` (current vendor GUID), `select`, `logout` and `recall`.
  Recall destinations include marketplace, lifestone, allegiance, house and
  mansion. Use `cast` for spell recalls.
- Inventory actions include `identify`, `equip` (with `offhand` or
  `unequip`), `use_item`, `apply_item`, `store_item`, `give` and `merge`.
- Loot actions include `open_corpse`, `close_corpse`, `loot`, `salvage`,
  `combine_salvage`, `read`, `sell`, `buy`, `split_note` and `sell_note`.
  `combine_salvage.items` is a comma-separated list of 2–64 distinct salvage
  bag GUIDs and `tool` is an owned Ust.
- Item Tools' `it_*` actions require `inventory` and `loot`. `it_merge`
  takes `item` (source GUID), `target` (receiving GUID) and `amount`.
  Use owned, unequipped, unoffered, non-retained stacks of the same WCID.
  Check source quantity and destination space, then wait for both stack
  updates before another merge.
- `say` accepts one line of text up to 500 characters. Supply `recipient`
  up to 128 characters for a tell, or `channel=2048` for fellowship chat.
  Otherwise it sends local talk.
- `fellowship` accepts `operation` `create` (`name`, shares XP), `quit`,
  `disband`, `open`, `close`, `recruit`, `dismiss` or `leader`.
  The last three take a player GUID in `target`. Dismiss and leader require
  the leader and another current member. Members can recruit when the
  fellowship is open. A locked fellowship cannot change openness. Wait for
  the `fellowship` snapshot to confirm changes.
- `confirm` uses the current prompt's `type,context,accept`.
  `profile_load` loads a validated nav/utl/met/usd dependency.
  Other policy actions include `plugin_command`, `buff_request_start`,
  `buff_request_done`, `force_buff_done`, `pause_navigation`,
  `pause_loot_approach`, `activity_failed`, `notice` and `stop`.

UCM's internal `craft`, `crafting`, `services_profile` and
`fellowship_tells` are reserved for the bundled implementation.

Policy metadata is also public: `activity`, `meta_state`, `targeting_status`,
`route_point`, `route_join_pending`, `route_track_movement`, `helper_updates`,
`continue_work`, `runtime_route`, `runtime_loop_route`, `runtime_reverse_route`.
The runtime route uses the same ground-following overlay as UCM (2,048 points).
Fellowship updates require fellowship; route output requires navigation.
Coordinates are AC local metres/cell IDs, not Unreal centimetres. GUIDs are unsigned
32-bit numbers, even though some native code stores them in signed integers.

## Native UI

`ui` is a list of controls. Every control, including containers, needs a unique
string `id`. Nested layouts support 256 total controls, eight levels, and 128 roots.
The native window retains its normal move/resize controls. Text-input focus uses
the same native editable widgets and gameplay-key blocking as the game's panels.

```lua
ui = {
  {id="main", type="section", text="Hunting", expanded=true, children={
    {id="fight", type="checkbox", text="Combat", value=true},
    {id="range", type="slider", text="Acquire within (m)",
      min=1, max=240, step=1, value=60},
    {id="name", type="input", text="Monster name", value="Red Rat"},
    {id="mode", type="select", text="Strategy", value="Automatic",
      options={"Automatic", "Melee", "Magic"}},
    {id="select", type="button", text="Select target",
      action="select", target=1234}
  }}
}
```

Controls include `text`, `button`, `checkbox`, `slider`, `number`,
`input`, `select`, `color`, `progress`, `icon`, `item_icon`, `image`,
`row`, `column`, `section`, `tabs`, `overlay` and `frame`.
Containers use `children`. Tabs label pages with each child's `text`
and keep selection by ID. Sections use `expanded`. Common options are
`text`, `tooltip`, `enabled`, `padding`, `size`, `bold` and `color`.

Rows divide width evenly. Set `compact=true` to size all children to their
content, or `auto_width=true` on individual children. Labels can use
`wrap=false` and `ellipsis=true`. Text `size` is 10–36 pt (default 15)
and follows **Lua Plugin Text Scale**. Allow room for larger text. Select
supports up to 256 string options. Number and slider use `min/max/step`.
Progress takes a fraction 0–1, with `color` for the fill and `text_color`
for the label.

`type="color"` provides a swatch, editable hex value, and an expandable color
wheel with brightness and opacity sliders. `value` is required and accepts a
named color or `#RRGGBB`/`#RRGGBBAA`; `expanded=true` initially opens the wheel.
Changes emit `{type="change", id=..., value="#RRGGBBAA"}` using uppercase hex,
including alpha. Malformed values are rejected. Handle the event and return
the new value, just like other controls; save it with `state` (requires `state`
permission) or a profile to persist it. The picker requires `ui` permission.

```lua
{id="tint", type="color", text="Nameplate color", value="#FFC040FF"}
```

`width` and `height` reserve fixed logical dimensions (0–2,048), including on
columns and row children. Text accepts `align="left"`, `"center"`, or `"right"`.
`overlay` draws children in order in the same space; `frame` adds a colored backing
(default `#00000080`). Colors also accept `#RRGGBB` and `#RRGGBBAA`.
The panel palette includes green, red, yellow, blue, purple, white, black and
transparent. Hex RGB is sRGB; alpha is linear.
Any frame, image, text or progress control may set `clickable=true` to emit the
normal click event, or use the same action fields as buttons.

`hide_when_pinned=true` hides a control while the window chrome is hidden and
restores it during Quick-show immediately, without waiting for Lua. The same state
is available as `window_chrome_hidden` in the snapshot.

All controls and containers accept `scale` (0.25–4, default 1),
`offset_x` and `offset_y` (-2,048–2,048, default 0), `opacity` (0–1),
and `rotation` (-360–360 degrees). Scale and rotation apply around the
control's center, followed by offsets in parent logical pixels, right/down
positive. Hit targets and children transform with the control. Use
`width`, `height` and `padding` to reserve layout space.

`effect` accepts `blink`, `pulse` or `rotate`. `period` is 0.1–60 seconds,
and `min_opacity` defaults to 0.2 for blink and pulse. Text uses
`outline` (0–8) and `outline_color`; `glow` (0–8) and `glow_color` add a
tinted halo to images or an outline to text.
See [Graphics Gallery](Examples/graphics_gallery/main.lua).

Buttons default to `{type="click",id=...}` events. Optional `action` is event,
select, select_item, waypoint or command; a command button uses `command="/acueucm ..."`. Native
buttons revalidate their current target at click time. Inputs and other editable
controls emit `{type="change",id=...,value=...}`. Input commits on Enter/focus loss;
`live=true` also emits while typing. Input is capped at 1,024 characters. The host
reuses controls with the same `id` and `type` across callbacks. Text, progress,
and other rows keep updating during mouselook or while a dropdown is open.
Use stable IDs: removing a control or changing its type replaces that control.
Focused text edits are preserved; an open select keeps its current options until
it closes. Handle events and return the updated value yourself. Event delivery
does not automatically save configuration.

`action="select_item", target=item.id` selects a known item in your main pack,
a nested owned pack, or the currently open external container (chest/corpse).
Optional `container=item.container` verifies its **direct** containing object;
IDs are positive unsigned 32-bit numbers, not inventory slot indexes or WCIDs.
The current server contents list takes precedence over stale object metadata.
Missing items, closed external containers, stale membership and containment
cycles are rejected. It requires API 2 and `ui` permission; no inventory/action
ownership is needed. Selection updates the existing inventory selection and
highlight; it does not open another pack, scroll its panel, use, equip, loot,
move or attack. `action="select"` on a UI button or companion also supports
accessible container items and retains its world-target behavior.

```lua
{id="pick", type="button", text="Select item", action="select_item",
 target=item.id, container=item.container}
```

A click reports `{type="click",id="pick",ok=...,error=...}`. Companions and
API 2 automation plugins can also return `{action="select_item",target=id,
container=container_id}` directly; failures appear in `s.error`. Prefer explicit
user clicks over reselecting every update. Read current inventory via the `data`
permission and `snapshot_fields={"inventory"}`; never save session-local GUIDs.
See [Appearance Controls](Examples/appearance_controls/main.lua) for saved color,
scale and offset settings applied to panels and nameplates, plus item selection.

`icon` uses a numeric DAT `icon` ID (available on inventory/spell records).
`image` uses `asset="my_icon.png"` from your own plugin's `images` folder. No
absolute paths, URLs or arbitrary disk image reads are accepted. PNGs are bounded
to 2 MiB compressed and 1,024 pixels per side; each panel caches at most 32 images
and 16 MiB decoded pixels. Images are UI artwork, not world material overrides.
Arbitrary C++/Slate, browser scripting and native injection are not exposed.

## Services and profiles

Return `requests={{id="my_request",service="profiles",operation="list",group="Routes"}}`.
Results arrive in snapshot.events as `{type="result",id=...,ok=boolean,error=...}`
plus operation data. At most four requests per callback, with batches at least
0.5 seconds apart. Queue/retry on a failed rate-limit result. There is no implicit
retry of destructive or outgoing actions. The inbox is bounded to 64 events and
384 KiB; consume it each callback. IDs correlate requests; they are not idempotency
keys. Plugin chat/messages and this inbox are separate bounded streams.

- **profiles**: list, read, save, load, delete. `group` defaults to Default; `name`
  identifies the JSON file. Names/groups use 1-64 letters/digits/underscore/hyphen.
  Save takes `data` or uses the current profile. Read returns data; load also makes
  it the active configuration and saves that choice for restart. List returns
  names. Delete moves the file to a unique archive, never erases it permanently.
  Files live under `Profiles/<plugin-id>/Library/<group>`. Plugins cannot write
  another plugin's settings. Files allow 8 MiB, like UCM; returned data allows
  256 KiB. Read optionally accepts field to retrieve one top-level section.
  Load returns profile_name; the active data arrives in the next callback's
  profile, without a duplicate copy in its events.
- **import**: list returns files in the user's configured import folder. Inspect
  returns the lossless parsed document; convert returns UCM-compatible data and
  issues, including validated dependencies. Unsupported imports return ok=false.
  Neither changes the running configuration or original file. Export_loot takes
  a parsed/edited UTL document in data and a safe name, writes a new `.utl` under
  `Exports/<plugin-id>`, and chooses another filename if one already exists.
  Activate converts and applies the imported sections to your own configuration;
  supply profile_name for its native saved name. This handles large imports
  without returning another full copy through the event inbox. Source files
  remain untouched, and unsupported conversions are rejected before activation.
- **route**: record adds a walk/jump/pause/use/portal point to your own active
  profile. Uses the same AC coordinates, object class/name matching and player
  facing as UCM recording. Optional charge (0-1) and seconds. Requires automation
  and navigation permission. Save the profile separately to persist edits.
- **force_buff**: start or cancel, automation/cast only. Start optionally accepts
  only=true for a buff-only cycle. Pair with the shipped policy or implement
  consumption of force_buff_request/force_buff_only and force_buff_done yourself.
- **policy**: command with text such as `/acueucm jump`; compiles into your own driver's
  command queue, using the same meta adapters. Requires commands. This does not
  enable UCM or edit its configuration. Execution retains each action's permissions.
- **waypoint**: set with coordinate text or clear. Uses the existing NAV destination.
- **window**: show/hide/toggle your own desktop window. Does not stop your plugin.
- **chat**: send text, optional recipient for tell or channel=2048 for fellowship;
  otherwise local/server-command text. Requires say; one send per second, at most
  500 characters, one line. For `/myquests`, the commands bridge still works.
- **notice**: show text in local chat, prefixed with the plugin's manifest name,
  using retail's green broadcast color. Requires ui; 1–500 characters on one line.
  This display-only service sends no server packet and does not change the host's
  global notice. It shares the four-request / half-second service budget. Read
  the result event and retry a rate-limited notice if delivery matters.
  Optional `window` (0 main, 1–4 extra, -1 all) explicitly routes past that window's
  category settings. Optional `color` uses the display-filter colors above. Both
  are retained in local history across HUD rebuilds. Global filters still apply.
- **sound**: play with `asset="alert.wav"`, optional `volume` (0–1). Requires sound.
  `source="bundled"` (the default) reads directly inside your plugin's `sounds`
  directory. `source="custom"` plays an opaque asset token returned by a
  user-initiated `action="sound_file"` button. The host validates and copies that
  selected file into persistent plugin storage; Lua never receives filesystem
  access. Files must be PCM16 WAV, mono/stereo, 8–48 kHz, at most ten seconds and
  1 MiB. Paths and URLs are rejected. Playback respects Master, Effects, and
  inactive-window audio settings and uses the normal service rate limit.
  No looping or persistent audio source is created. A successful result includes
  `duration` in seconds, including when audio is muted or no device is present.
- **log**: append text and optional level, or clear. snapshot.log contains the last
  200 entries. API 2 also logs native activity errors/status milestones there.
  This is a session ring, not an unbounded disk log. Use bounded state for any
  selected history that should survive logout.

`action="sound_file"` requires `ui` and `sound` permissions. On Windows and Linux,
clicking the button opens a native WAV picker. It emits a completion event
`{type="sound_file", id=<button id>, ok, cancelled, error}` instead of a generic
click event. A successful selection adds `source="custom"`, `asset` (opaque
token), and `name` (display filename). Save those values in character state and
pass the token to the sound service. Cancellation or invalid input preserves
previously imported files. On platforms without a picker, the event explains
that custom selection requires the desktop client; bundled playback still works.
Switching back to a bundled sound should change the plugin preference, without
deleting a custom asset another character may still use.

`profile_patch={...}` merges top-level fields into your own active configuration
and requires profiles. Unchanged patches do nothing. Changed native configuration
notifies the policy to re-evaluate pending work. A patch alone is not a disk save.
For reusable Character, Route, Loot, Monster, Meta and Vendor profiles, use separate
groups, read them, merge their data, and patch the wanted sections. This leaves
composition and your interface under the plugin author's control.
Profile patches and service request trees allow 32,768 nodes, 2,048 entries per
array, twelve nesting levels and 256 KiB of text per field tree. Individual strings
allow 4,096 bytes. Other structured output retains the API 1 limits of 4,096 nodes,
1,024 array entries and 64 KiB text, plus the UI/state-specific limits above.
The same VM memory limit still applies.

## Markers, NAV, state and messages

API 2 retains all [companion APIs](Companions.md): per-character/server persistent
state (64 KiB, atomic writes), independent chat with overflow detection, selected
object actions, existing NAV arrow targets, object-attached world markers,
Waypoint-map annotations, and local plugin-to-plugin messages. Markers are on the
Waypoint map, not the retail compass. The receiving plugin chooses what messages
mean; sending a message cannot grant permissions or start its automation.
`/acueplugin <id> start|stop|<text>` works for API 2 automation as well as companions.
The `/acueucm` command bridge intentionally controls built-in UCM; use the policy
service when the command should belong to your own automation plugin instead.

## Validation and portability

Run the public API, UCM and companion automation suites described in the
[test guide](../../Tests/README.md). Shared Lua source runs on Windows,
Linux, VR, Quest and Android. Verify the plugin's layout and server actions
on each target you intend to support.

## Optional terrain overlays (API 2)

Return `terrain_overlay` with the `terrain` permission. Dereth Lens is a
bundled example.

```lua
return {terrain_overlay={
    slopes=true, water=true, radar=false, dungeon=false,
    opacity=.45, range=120, brightness=.3,
    slope_color={1,.08,.02}, water_color={.15,.25,1},
    radar_color={.1,.15,.25}, light_color={1,1,1}
}}
```

Booleans default to false; opacity accepts 0–0.8, terrain range 24–192 meters,
brightness and each RGB channel 0–1. A supplied object replaces the configuration
atomically; `{}` clears it and omission retains it. One active surface is rendered
globally (first active provider in discovery order), avoiding compounded overlays.
Stop, disable, reload, logout, world change and portal space remove the surface.

Slope shading uses the loaded DAT terrain triangles and retail walking normal
threshold 0.6641741. Walkable triangles remain clear; blocked slopes transition
from yellow just beyond the limit toward the configured color as steepness grows.
Only impassable EntirelyWater landblocks receive water shading. Wadeable coastal
cells remain clear. This describes terrain, not reachability:
bridges, buildings, doors, jumps and server restrictions can change travel.
No terrain loads or collision scans are started by this overlay. Geometry refreshes
at most twice per second from at most nine cached blocks, and only when settings,
loaded terrain, indoor/outdoor state or nearby position change. Stationary
callbacks reuse mesh sections. All surfaces depth-test,
have no collision, and render in stereo. The radar wall follows the player each
frame at the native 75 m outdoor / 25 m indoor radius. Dungeon light is a temporary
world color-grading lift, with adjustable strength and tint; it does not overwrite
saved video settings and is removed outdoors or when disabled.

## Administrative content service (API 2)

The optional `admin_content` permission exposes `service="content"` to companions
and automation plugins. [Dereth Foundry](Foundry.md) is the bundled Lua example.
Requests and asynchronous `result` events use the normal public service protocol.

```lua
requests = {
    {id="files", service="content", operation="list", kind="weenies",
     format="json", filter="chest", offset=0}
}
-- A separate user-triggered request, with the current non-player selection:
-- {id="move",service="content",operation="command",text="/nudge n 0.1",target=s.selection.id}
```

`command` accepts only the content verbs listed in `ClientMods/api.json`, one
single-line command of at most 500 characters, at most once per second. It requires
the running plugin's active character/session. Targeted commands require an
explicit matching current non-player selection. The host sends manual assessment
before the command using the ordered game-action queue. The returned `command`
means queued, not server success: read the normal chat response. ACE independently
enforces access levels. This permission cannot execute arbitrary chat or SQL.

`list` reads a nonrecursive `json/<kind>` or `sql/<kind>` folder under the selected
local content root; kind is `weenies`, `landblocks`, `quests` or `recipes`.
Results contain `root`, `total` and up to 128 relative filenames. Use `filter` and
`offset` for searching/paging. `edit` accepts a relative JSON/SQL `name` in that
root and opens a text editor (Notepad on Windows). Absolute paths, traversal,
control characters and executable files are rejected. It does not import or
execute the file. These file operations are user actions, not periodic scans.

A button with `action="content_folder"` opens the Windows folder picker and
produces `{type="content_root",root=...}`. The choice is saved in host settings;
Lua cannot set an arbitrary root. Other platforms use the default
`ClientMods/Content` directory and open it with the system file manager. External
editor/file-manager availability depends on the platform. The local content root
is separate from the server's `content_folder`; remote-server import/export still
requires transferring files through the administrator's normal workflow.


## Server crafting catalogs

Auto Craft accepts a local `.json` file while logged into its intended server.
Use **Import for this server** to replace that server's custom overlay, or
**Use bundled recipes** to remove it. Both Auto Craft and UCM automatic crafting
use the merged catalog. Import does not execute a job. Recipe IDs override
matching bundled IDs; new IDs add recipes. Each import is a complete overlay,
not an incremental patch to the previous overlay. Catalogs persist beneath the
user data directory's `Crafting/` folder, keyed by server identity.

Use the same schema as `ClientMods/autocraft/catalog.json`:

```json
{
  "schema": 1,
  "version": "server-author-version",
  "items": [
    {"id": 700001, "name": "Example tool", "max_stack": 1, "icon": 0, "type": 0},
    {"id": 700002, "name": "Example ingredient", "max_stack": 100, "icon": 0, "type": 0},
    {"id": 700003, "name": "Example output", "max_stack": 100, "icon": 0, "type": 0}
  ],
  "recipes": [{
    "id": "example-recipe", "category": "Custom", "source": 700001,
    "target": 700002, "output": 700003, "yield_count": 1,
    "skill": 0, "difficulty": 0, "source_count": 0, "target_count": 1,
    "fail_source": 0, "fail_target": 0, "fail_output": 0, "fail_yield": 0,
    "chain": false
  }]
}
```

These IDs are illustrative, not a usable server recipe. Supply verified WCIDs,
source/target direction, consumption, failure behavior, skill and output from the
server's actual recipe data. Include every referenced source, target and output
item in the overlay. `source_count:0` represents a non-consumed operand, not a
missing operand. Optional recipe flags are `conditional`, `modified`,
`optional_source`, `optional_target`, `optional_fail_source`,
`optional_fail_target`, and optional text `fail_message`; consult bundled entries
with matching semantics. `chain:true` permits the planner to produce missing
intermediates; modified/self-output recipes are never chained.

Files are limited to 16 MiB and merged catalogs to 30,000 items and recipes each.
Malformed catalogs preserve the previous installed overlay. A corrupt stored
catalog blocks crafting until corrected or removed. An active standalone or UCM
craft job blocks import/removal. Protection for traded/equipped/retained inputs,
ordinary server requests and inventory-confirmed completion apply equally to
custom recipes. Plugins cannot install recipes through an automation intent.
