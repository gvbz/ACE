# Public plugin API 2

API 2 exposes UCM's gameplay data and native action pipeline to independently
named Lua plugins, plus structured native UI. API 1 manifests remain supported.
This is a client API; server rules, spell timing, collision and ownership still
apply. It does not load Decal DLLs or execute arbitrary native widget code.

Start with [API Workbench](Examples/api_workbench/main.lua), its
[manifest](Examples/api_workbench/plugin.json) and
[default profile](Examples/api_workbench/default.json). Copy that directory into
`Saved/ClientPlugins/Installed`, refresh Manage Plugins, enable its permissions,
then Start. The example's activities start off. It has its own profiles and does
not edit the built-in UCM configuration. The machine-readable action/field catalog is
[`ClientMods/api.json`](../../ClientMods/api.json).

## Find the functionality you need

- Installation, enabled limits and execution: [lifecycle](#lifecycle-and-ownership).
- Character, inventory, equipment and action results: [snapshots and permissions](#permissions-and-snapshots).
- Item classification: [Loot matching](LootMatching.md), including appraisal and resumable evaluation.
- Trackers that read server messages: [chat delivery and filters](#read-and-filter-chat).
- Custom PNGs, gauges, timers, animation and controls: [native UI](#native-ui) and [Graphics Gallery](Examples/graphics_gallery/main.lua).
- Color pickers, scale, offsets and container item selection: [Appearance Controls](Examples/appearance_controls/main.lua).
- Target health, effects and opaque/translucent nameplates: [target data and world plates](#target-data-and-object-attached-plates).
- Casting, combat, equipment, transfers and navigation: [game intents](#game-intents) and [built-in policies](#reuse-or-replace-the-built-in-policies).
- Configuration, imports, outgoing chat and sounds: [services](#services-and-profiles).
- Terrain navigation overlays: [terrain API](#optional-terrain-overlays-api-2).
- Administrative content tools: [content service](#administrative-content-service-api-2).
- Local appearance conversion: the separate [Local Cosmetics service](LocalCosmetics.md).

## Lifecycle and ownership

The host discovers every valid installed plugin; disabled plugins no longer hide
later folders. **Plugin Manager > Enabled plugin limit** defaults to **16**.
Players may raise it or set **0** for unlimited. Only enabled plugins count;
disabled plugins take no slot. An enabled but stopped plugin still counts, as
does an enabled UI-only tool. Closing a window does not disable its plugin.
Disabling frees a slot immediately. The limit is saved across restarts, and the
manager rejects lowering it below the current enabled count without stopping
any plugin. Bundled and third-party plugins use the same allowance.

This is a host preference, not a manifest property or Lua setter. Plugins cannot
raise the limit, enable themselves, or grant permissions. `snapshot.plugins`
lists discovered IDs with `{enabled, running}`; request `plugins` explicitly when
using `snapshot_fields`. This map does not include the configured limit or other
plugins' private data. The machine-readable catalog's `host.plugin_limit`
describes the setting; it is not a live settings query. Increasing this allowance
does not change VM memory/work quotas, world-plate limits, or automation ownership.

Use `"api":2` with either `"mode":"automation"` or `"mode":"companion"`.
Automation explicitly starts, owns game actions, and stops on logout/character
change. Only one ordinary automation driver runs at a time; starting another
stops the previous driver. Built-in Item Tools temporarily pauses built-in UCM.
Companions auto-start when enabled and logged in, run beside the driver, and do
not stop its casting, attacks, movement or recovery. Stopping a companion never
stops UCM. Use companions for trackers and UI; automation for tools that actually
fight, navigate, equip, loot or cast. Closing either window does not stop it.

Callbacks remain `return function(snapshot, profile) ... end`. The output can
combine a game intent with `presentation`, `ui`, `chat_filters`, `state`, `markers`, `messages`, `requests`, and
`profile_patch`. Return nil for no change. Explicit empty UI/marker/message/request
arrays clear those lists; omitted presentation fields retain the previous display.
The tables given to Lua are copies. To change native state, return an intent or a
service request; mutating a snapshot does nothing to the game.

Automation uses UCM's completion/vitals/target-death wakes and action gates, not a
second timer that slows it down. Companion callbacks are limited to 2 Hz. There
is no permission to override casting delays, teleport, bypass collision or send
raw packets. Each VM has a 32 MiB heap (including host-provided snapshots and profiles) and
250,000 instructions per callback. Use
`workavailable()` to split large jobs across callbacks.

Companion manifests may set `"update_interval": 2.0` (seconds, 0.5–60;
default 0.5). Due callbacks are scheduled round-robin, at most one companion
per rendered frame. Input events may wake a callback earlier. Native snapshot
data is shared with other readers in that host tick and invalidated by actions;
each Lua VM still receives its own tables.

API 2 companions may opt into `"snapshot_fields"`, an array of top-level field
names. For example, `"snapshot_fields":["character_name","level","char_ints",
"action_serial","health","max_health"]` avoids building inventory, spells,
target scans, and vendor data. With no list, the existing full snapshot remains
available. `api`, `time`, `unix_time`, `player`, `server`, `state`, `error`,
`events`, and `capabilities` are always retained. Permissions still apply;
listing a field never grants access. Character qualities, `character_name`,
`level`, and action results require `data`. Inventory-only observers use the
cached inventory path described below. Other full automation fields can build
the shared full snapshot before filtering; use only
the fields your observer needs to reduce native work and Lua allocations.

Character quality maps (`char_ints`, `char_quads`, `char_doubles`, `char_bools`,
and `char_strings`) use decimal property IDs as string keys. They reflect the
server's received character qualities, including custom IDs, live replacements
and removals. A removed key is absent (`nil` in Lua); an explicit zero, false
quality or empty string remains present. `char_bools` values are numeric `0`/`1`,
so compare them explicitly rather than using Lua truthiness. Missing qualities
are not universally equivalent to zero: for example, an absent
`SpellComponentsRequired` bool (68) means components are required. Use the
derived `components_required` field for that policy. `char_strings["1"]` may
still fall back to the player's public object name when the name quality is
absent. These maps describe the current character, not arbitrary nearby objects.

## Plugin title and toolbar icon

Every plugin can set a default display title and its own artwork in `plugin.json`:

```json
"presentation": { "title": "Kill Task Tracker", "icon": "tracker.png" }
```

Put the PNG at `images/tracker.png` inside that plugin's folder. It appears in
Manage Plugins and the plugin bar. The full title appears on hover and in the
window header. Standalone plugin windows do not repeat that title above the
Start/Stop controls; authors do not need to add a title row to `ui`. The embedded
login/VR manager keeps a heading to identify the selected plugin. Without a title,
the manifest's `name` is used. Without a usable
icon, the built-in emblem or generic extension sigil remains visible. Images
keep their original aspect ratio inside the 26-pixel icon area; square transparent
artwork is recommended.

API 1 companions and API 2 plugins with `ui` permission may also return:

```lua
return function(s, p)
  return {
    presentation = {title="Kill Task Tracker - 12 / 50", icon="tracker.png"},
    ui = {{id="progress", text="12 of 50 kills"}}
  }
end
```

Omitting `presentation` keeps the current values. Returning it replaces the
runtime overrides: missing keys inherit the manifest defaults; `{}` restores
all defaults. An empty title uses `name`; an empty icon uses the native emblem.
Changes update open windows and the bar without restarting. Runtime overrides
last until a character change or plugin reload; use the manifest for defaults
that must appear before Lua starts. Metadata does not change the plugin's ID,
permissions or saved files.

Plugins can also supply window defaults in the manifest or runtime presentation:

```lua
presentation = {
  title = "Kill Task Tracker",
  window = {opacity = 0.35, pinned = true}
}
```

`window.opacity` is a number from 0 to 1. It fades backgrounds and borders only;
text, item icons and interactive controls remain readable. `window.pinned` hides
the title bar and border and locks the window's position, keeping its contents at
the same screen position. The background retains the configured opacity whether
pinned or unpinned. Hold the rebindable **Quick-show pinned
windows** key (Left Control by default) to restore the header and resize controls.
Runtime changes require `ui` permission and update existing widgets without
rebuilding their controls. Player overrides in Manage Plugins take precedence
over these defaults; **Use this opacity for all plugins** overrides every plugin's
opacity. Omitted window fields inherit manifest defaults, just like title/icon.

Titles allow up to 80 characters on one line. Icons are PNG filenames only (up
to 96 characters), with no paths or URLs. PNGs are limited to 2 MiB compressed and
1024 by 1024 pixels. Toolbar and UI images share one cache per plugin: at most 32
assets and 16 MiB decoded. Missing or invalid images use the fallback; use Refresh
in Manage Plugins after replacing image files on disk.

## Native window controls

The manifest's `presentation` object and runtime `presentation` output accept:

```lua
presentation = {
  controls = "toggle",       -- "start_stop" (default), "toggle", or "none"
  start_label = "Start tracker", stop_label = "Pause tracker",
  settings = false,          -- hide the complete Settings and profiles section
  status = false,            -- optional: hide the host's status text too
  scroll = false             -- optional: paginated panel without host scrolling
}
```

The toggle starts/stops the actual plugin; labels follow its running state.
Labels are single-line strings of at most 40 characters; empty strings use
Start/Stop. `settings`, `status` and `scroll` default to true. Defaults and overrides use
the same inheritance/reset rules as title/icon. Changes preserve the open
window and its control identities. These options apply to custom plugin panels;
specialized built-in panels keep their own layout. Companions auto-start when
enabled, making `controls="none"` useful for passive HUDs. Hiding settings only
changes presentation; it does not remove the profile API or change permissions.

With `scroll=false`, the host allocates the remaining window height to the custom
UI and does not create a scroll box. While that panel is displayed, snapshots
include `panel={width=...,height=...}` in logical UI units, updated on resize.
Paginated plugins can use this to choose how many rows fit; provide a fallback
before the panel's first layout. The last measured size remains when closed.
Plugins are responsible for fitting their controls into this space.

## Read and filter chat

`chat` permission already provides `snapshot.chat_events`, including `raw_text`,
formatted `text`, `sender`, `color`/`chat_type` (chat message type), `time`, and `serial`.
Use these events to parse server-specific responses without hiding a chat window.

To receive only relevant lines, declare `chat_subscriptions` in the manifest or
return it from Lua. These rules filter delivery before messages enter the plugin's
inbox; they do not change what players or other plugins see. For example:

```json
"chat_history": false,
"chat_subscriptions": [
  {"id":"kills", "prefix":"You have killed", "sender_type":"server"},
  {"id":"solves", "contains":" solves (", "sender_type":"server"}
]
```

Subscriptions use the same matching predicates as display filters below. Rules
are ORed; predicates within a rule are ANDed. Omission retains subscriptions;
an empty list subscribes to no lines. Without this option, existing plugins
continue receiving all chat. A runtime change starts at the newest line; repeating
the same registration preserves pending messages. Register in the manifest to
cover responses before the first callback. Each subscriber retains up to 2,048
matching events / 2 MiB of message text; delivery remains 128 events / 64 KiB per
callback. `chat_pending`, `chat_more`, `chat_gap` and `chat_dropped` describe this
matching inbox, so unrelated combat messages cannot delay quest responses.

`chat_history=false` starts or restarts at the newest message. The default, true,
allows retained messages from this login. Refresh preserves the existing cursor
instead of replaying messages already delivered. Neither option replays past logins.

Local `notice/show` output uses the **Plugin chat** category in Options > Chat.
Players can independently enable it for each chat window. Notices retain their
normal text color and enter bounded chat history, including while a window is
closed. This category also covers UCM loot announcements. Server messages and
echoes of `chat/send` retain their normal channel categories.

The separate **`chat_filter` permission** allows display-only filtering. Return a
`chat_filters` array to replace your plugin's active rules:

```lua
chat_filters = {{
  id = "quest_dump", contains = " solves (", sender = "", chat_type = 0,
  window = 0
}}
```

This matches ACE/GDLe-format `/myquests` rows in main chat. It does not hide other
Broadcast messages. Custom server formats may require different literals. The
matching uses incoming `raw_text` before speaker/timestamp decoration. Rules run
natively before a row is drawn, so a response does not flash until the next Lua
callback. Hidden messages still reach **all** plugin readers and an enabled chat
log file. Filtering never changes server state or messages, chat window settings,
or existing chat history.

Each rule has a unique `id` (1–64 characters) and at least one text, sender or
message-type predicate. `contains`, `prefix`, `exact`, and `pattern` strings allow
at most 256 characters. All supplied tests must
match. Optional `sender` is an exact match; `sender=""` restricts matching to
messages with no sender. `case_sensitive` defaults to false. `chat_type` optionally
matches a numeric message type, 0–63. `window` is 0 for main chat, 1–4 for extra
windows, or -1/omitted for all five. `seconds` optionally expires the rule after
more than 0 and at most 300 seconds; omitting it keeps the rule until cleared.
`contains`, `prefix`, and `exact` are literal. `pattern` is a regular expression
using the same bounded regex engine as loot rules, not a Lua pattern. Matching
has native work/stack limits. Invalid patterns are rejected during registration.

`sender_type` optionally matches `player`, `npc`, `vendor`, `server`, or `unknown`.
It is also included on chat events. Classification uses the currently known
speaker, fellowship and character data; an unrecognized name remains `unknown`.
`server` means the message has no sender. The chat protocol does not reliably
supply spell-school metadata, so there is no inferred `spell_type` field.

Display rules accept `action="hide"` (default), `"show"` or `"color"`, plus
`color="red"` (also green, blue, yellow, white or `#RRGGBB`). A color alone implies
the color action. `show` adds the line to the selected windows even if their
message-category filters are off. Hiding wins conflicting plugin rules; global
filters and player squelches still apply. For example:

```lua
chat_filters = {{id="kill_alert", pattern="^You (have |)killed [0-9]+",
  sender_type="server", action="show", window=2, color="#FF8800"}}
```

At most 32 rules may be registered per plugin. Omitted `chat_filters` retains
them; `{}` clears them. A replacement is validated before applying any of its
rules. Stop, disable, reload, logout, or character change removes that plugin's
filters. One plugin cannot remove another plugin's rules. Any matching active
hide rule can hide a line in its chosen window.

For outgoing custom server commands, API 2 already supports the separate `say`
permission and `requests={{id="bonus",service="chat",operation="send",text="/bonus"}}`.
This sends text to the server; it does not grant local shell access. `/b` and
`/fship` can be sent the same way. Register filters in the same output or before
the request so the first response is covered. Sending is limited to once per
second; use `events` result records to check acceptance.

See the runnable [Chat Tools example](Examples/chat_tools/main.lua) and its
[manifest](Examples/chat_tools/plugin.json): optional two-minute quest polling,
on-demand server commands, readable responses, and no native Start/Stop or
Settings and profiles section.

## Target data and object-attached plates

All companion snapshots (API 1 and 2) include `selection` (a target object, or
nil), `tracked_targets` (objects keyed by the decimal string of their GUID),
and `target_effects` (effect arrays keyed the same way). Tracked objects include
the selection, active world-plate targets, and every still-known object with an
unexpired confirmed timer, independently of the 512 nearby-object limit. Reading
these fields alone does not send requests or require UCM or Action Deck.
`objects` also includes the descriptor-derived `object_type`.

Target fields:

- `id`, `name`, `wcid`, `item_type`, `monster`, `dead`.
- `object_type`: descriptor-derived `player`, `creature`, `vendor`, `portal`,
  `container` (bags/chests), `lifestone`, `door`, `corpse`, or `item`.
  The database's raw `weenie_type` is **only** included if an assessment supplies
  property 9007. Standard ACE public object descriptions do not transmit that
  database enum; `object_type` is the usable client classification.
- `creature_type` and localized `creature_class` (for example, Banderling),
  `title` (for example, Royal Guard), and `level`, when assessed and known.
- `pk_status`: `NPK`, `PK`, or `PK Lite` for players, from the public descriptor.
- `vitals.health`, `.stamina`, `.mana`: omitted when unknown. Each known pair
  contains `current` and `max`; zero current is a known value. A newer health
  update supplies `fraction` (0–1) and, if assessed, `max`, but omits `current`:
  the health packet is a percentage, not an integer hit-point count. A new
  assessment restores the exact pair. No values are copied from the player.
  `assessment_age` on the target and `age` on a percentage health sample report
  seconds since receipt; assessed stamina/mana are samples, not live estimates.

Each `target_effects[string.format("%.0f", id)]` entry has `spell`, DAT `icon`, `remaining`
(seconds), `duration`, and `source="own_confirmed_cast"`. These are the same
records used by Action Deck: successful server cast confirmations start timers;
resists, fizzles, and ambiguous same-name confirmations do not. They are duration
estimates from local spell data, not authoritative target enchantments. Casts by
other players, early dispels, and custom server durations cannot be inferred.
Changing selection preserves other targets' timers. Expiry, death, loss of
awareness, GUID reuse, and character/session changes clear them.

With **both `ui` and `markers` permissions**, return `world_plates`:

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

The native renderer draws brackets around the rendered actor, a role/name/level
heading, known health/stamina/mana bars, and DAT effect icons with countdowns.
It updates the billboard transform after world movement every frame, with a
stereo world-space surface in VR; Lua never needs camera coordinates. Countdown
labels advance locally between callbacks. Hidden or unloaded actors, corpses,
portal transitions and stopped plugins do not leave floating plates behind.
Plates are visual only: they do not intercept clicks, controller rays or movement.

`opacity` controls the entire composed plate (text, brackets, bars and icons).
`background_opacity` controls the black backing behind labels, bars and icons.
Both are finite numbers in [0,1], defaulting to 1: solid content and backing,
with the unused canvas still transparent. Overall opacity is applied once after
composition, so overlapping text and backing do not compound the fade.
Optional `vr_opacity` and `vr_background_opacity` override the corresponding
desktop value while VR is active; omitted overrides inherit it. These fields
never change the selected creature's yellow mesh tint or selection settings.
For solid text with a softer backing, keep `opacity=1` and lower
`background_opacity`. Set both to 1 for an opaque nameplate.

Plate colors accept `green`, `yellow`, `red`, `blue`, `white`, `purple`, `black`,
`transparent`, `#RRGGBB`, or `#RRGGBBAA` (sRGB RGB, linear alpha). `color` styles
the name and brackets; `text_color` styles role, level, vital numbers and timers.
`background_color` styles the backing, multiplied by `background_opacity`.
`health_color`, `stamina_color`, and `mana_color` style their respective bar fills.
Omitting these fields preserves the existing palette. Overall `opacity` also
multiplies each chosen color's alpha; use `FF` alpha for fully opaque content.

`scale` is a uniform multiplier from 0.25 to 4 (default 1). `offset_x` and
`offset_y` are finite plate-local logical pixels from -2,048 to 2,048 (default 0):
positive X moves right and positive Y moves down in the camera-facing plane.
Offsets scale with the plate; the whole plate, including brackets, moves together.
Optional `vr_scale`, `vr_offset_x`, and `vr_offset_y` override individual values
in VR; each omitted override inherits the corresponding desktop value. These
controls affect the plugin's plate only, never the actor or its selection tint.
Use native `type="color"` and slider controls below to expose addon preferences.

An omitted `world_plates` retains this plugin's list; `{}` clears it. A supplied
list atomically replaces it. `selection_only` defaults to true, hiding the old
target immediately when selection changes; set false for an additional tracked
target. Stop, disable, reload, logout and character changes remove the plates.
Plates automatically request background assessments, including after selection
changes. `refresh=false` opts out. Requests share the global one-per-second
appraisal budget, rotating among nearby living plate targets; with automation
running, plates use at most alternate seconds. Successful targets are revisited
no sooner than two seconds; failed or unanswered assessments wait 30 seconds.
The normal server health feed is accepted for every known target. No health-query
polling steals the server's selected target. On ordinary servers, off-target
vitals therefore refresh at assessment cadence, not every damage event. Assessment
skills and server restrictions still apply; unknown fields remain absent.

At most 4 plates per plugin and 16 globally are rendered. Object IDs must be
unique within a plugin. Names are at most 128 single-line characters, roles 80;
long text is ellipsized. Optional `level` is a nonnegative integer. Colors are
`yellow`, `red`, `blue`, `green`, `white`, `purple`. Missing vitals are hidden;
each supplied vital needs a valid `current/max` pair or a `fraction` in [0,1].
Effects are limited to 16 per plate, with a `0x06xxxxxx` DAT icon ID and
`remaining` in [0,86400] seconds; optional `spell` identifies the spell. Zero
remaining hides that icon. Plugins choose which effects to show. This surface
accepts no custom meshes, shaders, file paths or native code.

The runnable [Target Plate example](Examples/target_plate/main.lua) caps effects
and passes only known fields. Copy its folder into `Saved/ClientPlugins/Installed`,
refresh Manage Plugins and enable it. It is an API example, not an enabled-by-default
HUD and does not modify another companion's configuration.

## Inventory-style icons

Use `item_icon` for the same square background and composited artwork as the
inventory and Supply Ledger. The original `icon` control remains a plain DAT icon.

```lua
local item = s.inventory[1]
local row = {
  id="supply", type="item_icon", size=32, text=item.name,
  icon=item.icon, item_type=item.type,
  overlay=item.icon_overlay, underlay=item.icon_underlay, effects=item.ui_effects
}
```

Full inventory snapshots now expose `icon_overlay`, `icon_underlay`, and
`ui_effects` alongside `icon` and `type`. A companion needs `data` permission for
that full snapshot. Item icon fields are unsigned 32-bit numbers; texture IDs
include the `0x06000000` prefix. `size` is 16–128 pixels (default 32); optional
appearance fields default to zero. The native compositor applies the type
background, underlay, icon, overlay, and enchantment effects in inventory order.
Returning new values updates the retained icon rather than rebuilding its row.

Assessed casting implements expose `innate_spell` with
`id,name,power,category,level,caster_target,beneficial,duration,flags,school,skill`,
plus `innate_spell_available`. `skill` is the item's reported spellcraft.
These describe the item's spell independently of learned spells and components.
Availability checks known activation requirements, ownership/trade state and
item mana; it does not promise a successful cast or a valid target.
For backward compatibility, beneficial instant self spells also expose the same
metadata as `innate_recovery` and `innate_recovery_available`.
Policies may equip an eligible implement and submit `cast_item` below.

Assessed physical weapons can expose `proc_debuff` with
`id,name,power,category,school,flags,duration,skill`, and `proc_debuff_available`.
This represents the first appraised spell only, when it is an offensive timed
non-War/non-Item spell. It is a potential on-hit effect, **not** an activatable
item spell. Availability checks activation requirements, ownership/trade state
and item mana; callers must also check `can_wield` and ammunition. UCM uses normal
physical attacks and waits for an actual effect confirmation. Its internal
`debuff_spell` attack field is restricted to UCM and revalidated against the
currently equipped source; third-party plugins cannot invent proc spell IDs.

Route-door rows in `route_objects` include `door_open` (live passage state) and
`door_identified`. An assessed door adds `door_locked` and `lock_difficulty`.
Unknown appraisal is not evidence of an unlocked door. Lockpick inventory rows
provide `lockpick_available`; retained picks remain usable, but traded, equipped
or exhausted picks do not. `structure` reports remaining uses.

## Reuse or replace the built-in policies

Optionally add `"policy":"ucm"` or `"policy":"itemtools"` to an automation
manifest. This loads the exact shipped Lua policy as `builtinpolicy` and its
configuration defaults as `builtinprofile`. Each plugin gets an independent
function/VM state. Defaults are not silently applied to the author's saved
profile: copy the wanted values into your own default.json.

```lua
return function(s, p)
  local intent = builtinpolicy(s, p) or {}
  intent.ui = {{id="status", text=intent.status or "Waiting"}}
  return intent
end
```

`policy` is optional. Write your own decision function and return the same native
intents to replace the algorithms. Both approaches get the same cast confirmation,
fast buff movement, equipment requirements, attack cancellation, target-death
wake, inventory transfer, route collision/door use and action-failure handling.
The built-in policies are readable in `ClientMods/ucm/main.lua` and
`ClientMods/itemtools/main.lua`; the UCM defaults define its full configuration
including spells, recovery, summons, equipment mana, vendors, monster rules,
loot/salvage and meta state machines. No reserved plugin ID is needed.

`mana_charges_when_off=true` also opts an enabled automation plugin with inventory
permission into the native stopped-state gear recharge service. With no driver
running, the first eligible plugin supplies its settings. Only the stock recharge
policy runs then; the author's stopped callback is not invoked.

The sandbox also supplies `lootmatch`, `spellindex`, `regexmatch`, `regextest`,
`string.contains`, `string.findplain`, `ucmcommand` (legacy alias `vtcommand`) and `workavailable`.
These are the same bounded helpers used by UCM. A command returned by
`ucmcommand()` is a policy command record, not a host intent: pass it to a
policy-command request or process the record in your own Lua code.

`spellindex(spells, inventory, time, skill_margin, unavailable_until, components_required)`
returns `by_id, supplied, usable, groups`. The first two are keyed by spell ID;
`usable` retains all eligible tiers in snapshot order. `groups` provides the
same records as `offense` (non-beneficial usable spells), `instant_self`
(usable caster-targeted spells with zero duration), `other_buffs` (usable
beneficial timed spells that are neither caster-targeted nor self buffs),
and `by_category[category]` (all usable spells in that family). `buffs` contains
known self buffs and beneficial timed Item Enchantment spells, including those
blocked by skill, supplies or retry delay, so a policy can apply its separate
buff margin and report unavailable families. Groups retain every tier and
reference the original records; they do not choose which spell to cast.
Indexing remains subject to the per-callback native work and memory limits.

The optional sixth `components_required` argument defaults to true. Pass
`s.components_required ~= false` to honor an explicit server exemption; do not
infer an exemption from missing component data. `unavailable_until` is a table
keyed by numeric spell IDs with monotonic retry times. Indexing accepts at most
16,384 spells and 4,096 inventory entries, with 131,072 combined native visits per
callback. `supplied[id]` is a boolean, not an inventory count. Eligibility is not
a guarantee of line of sight, target validity, or server acceptance.

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

The VM exposes base functions plus `table`, `math`, `utf8`, and bounded `string`
operations. There is no `require`, `io`, `os`, `debug`, coroutine API, dynamic
loading, `pcall`/`xpcall`, metatable access, `collectgarbage`, or `print`. Standard
Lua pattern functions `string.find`, `match`, `gmatch`, `gsub`, and `dump` are
removed. Use the helpers above and the notice/log services for output.

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

Full snapshots include every field UCM receives: known spells and eligibility,
enchantments/debuffs/cooldowns, current/base vitals, skills and quality maps,
appraised inventory and worn equipment, all packs and capacities, reachable
monsters/corpses, route visibility and movement breadcrumbs, vendor stock/currency,
fellowship vitals, confirmations, portal events and combat results. Item Tools'
transfer eligibility, vendor room/price and inventory failure fields are included
as well. `identified=false` means a property may be unknown; it is not evidence
that the property is zero. Automation appraisal uses inventory permission;
companions use the narrower identify permission above.

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

API 2 adds `api`, `unix_time`, `server`, `state`, `events`, `error`, `capabilities`,
`plugins`, `automation_owner`, `profile_name`, `waiting_action`, `log`, and the
lightweight nearby `objects` list. `chat_events` has independent per-plugin
cursors and works with UCM disabled. Large bursts are delivered oldest first
in batches of up to 128 events / 64 KiB of UTF-8 message text per callback;
unread events remain queued for later callbacks. `chat_serial` advances only
through the delivered batch, while `chat_latest_serial` identifies the latest
received event. `chat_pending` and `chat_more` expose the remaining backlog.
The shared backlog retains up to 32,768 events / 8 MiB of UTF-8 message text;
`chat_gap` and `chat_dropped` report actual unread overflow, not pagination.
See [companion chat delivery](Companions.md#callback-and-snapshot) for the field limits and
session scope. Chat events are empty without chat permission, regardless of
whether a full snapshot is requested.

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

Return one action per callback. Do not treat submission as completion: observe
`ready`, `busy`, serials, inventory revisions and cast confirmations, or use the
shipped policy's pending-action logic. Native code rechecks targets/ownership and
server state before sending an action.

The field list for every action and its permission is in `api.json`. The actions
cover:

- `cast` with spell/target and optional `fast_cast`; attack (`mode` 2 melee or 4
  missile, `height` 1-3, `power` 0-1), `cancel_attack`, `combat_mode`, `attack_bar`.
- `cast_item` with `item,spell,target` requires both `cast` and `inventory`.
  The owned, equipped implement must expose that eligible innate spell.
  Supported targets are the player for beneficial instant self recovery, or a
  living, attackable non-player/non-pet for a harmful timed debuff with clear
  projectile sight. It sends item use with target in magic mode, never a
  learned-spell cast. Completion is a `use` receipt with the implement as source;
  confirm the recovered vital or the actual debuff separately. A successful use
  receipt alone is not proof the spell landed.
- `cast` and `cast_item` optionally accept `cast_jiggle` (backward movement with
  alternating strafes); `cast_item` also accepts `jump_out_wand` (a brief jump
  request after use). Both default to false, require `navigation`, use ordinary
  movement/collision and are cancelled by manual movement, stop, session change,
  completion or failure. UI-blocked or stale requests are discarded. The client
  does not change normal casting restrictions or promise server-side animation
  cancellation. `fast_cast` remains the separate fast self-buff tap behavior.
- `move` with AC `cell,x,y,z`, metre `arrival_radius`, optional pursuit `target`;
  `face`, `jump`, `recall`, `use_world`, `close_vendor` (current vendor GUID),
  `select`, `logout`.
- `identify`, `equip` (`offhand`/`unequip`), `use_item`, `apply_item`, `store_item`,
  `give`, `merge`.
- `open_corpse`, `close_corpse`, `loot`, `salvage`, `combine_salvage`, `read`,
  `sell`, `buy`, `split_note`, `sell_note`. `combine_salvage.items` is a comma
  separated list of 2-64 distinct salvage-bag GUIDs, with an owned `tool` Ust.
- `it_give`, `it_buy`, `it_sell`, `it_merge`, `it_split`, `it_open`, `it_key`,
  `it_refresh_chest`, `it_close`, `it_pickup`: the Item Tools transaction paths.
  `it_merge` takes `item` (source GUID), `target` (receiving GUID), and `amount`;
  both must be owned, unequipped, unoffered, non-retained stacks of the same WCID,
  with enough source units and destination space. Wait for both stack updates
  before issuing another merge; `UseDone` alone does not confirm it.
- `say` accepts `text` (up to 500 characters), optional `recipient` (up to
  128 characters) for a private tell, or `channel=2048` for fellowship chat.
  Without either it sends local talk. Embedded line breaks are rejected.
- `say`, `fellowship`, `confirm` (current `type,context,accept`). Recall destinations
  are marketplace, lifestone, allegiance, house and mansion; spell recalls use cast.
  `fellowship` requires the `fellowship` capability and `operation`: `create`
  (`name`, shares XP), `quit`, `disband`, `open`, `close`, `recruit`, `dismiss`, or
  `leader`. The last three use `target` (unsigned player GUID). Dismiss/leader
  require another current member and leader authority; an open fellowship also
  permits recruiting by members. A locked fellowship cannot change openness.
  These actions send requests, not predicted results: wait for `fellowship`
  snapshot changes (`valid`, `name`, `open`, `leader`, `members`) before dependent
  actions. `action_events`/`UseDone` do not acknowledge fellowship management.
  UCM meta sequences wait up to 30 seconds, yield to recovery, and hold navigation
  until the matching fellowship state arrives. They do not resend an unanswered
  invitation. UCM also has an optional automatic tell-based fellowship manager
  under Buff others; its private command queue is not exposed in other plugins'
  data snapshots. Use the public chat subscription API for your own chat handling.

UCM's integrated crafting uses the existing native Auto Craft recipe/receipt
engine. Its internal `craft`, `crafting`, `services_profile` and
`fellowship_tells` fields are UCM implementation details, not general plugin API
contracts. Other plugins should use the documented inventory/use/confirmation
APIs and correlate `action_events` with actual inventory updates.
- `profile_load` for validated imported dependencies (`kind`: nav/utl/met/usd),
  `plugin_command`, `buff_request_start`, `buff_request_done`, `force_buff_done`,
  `pause_navigation`, `pause_loot_approach`, `activity_failed`, `notice`, `stop`.

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

Controls: text, button, checkbox, slider, number, input, select, color, progress, icon,
item_icon, image, row, column, section, tabs, overlay and frame. Containers use `children`; tabs use each
child's text as its tab title. Sections have `expanded`; their expansion state
and selected tab are retained while the window is open. Common options are text,
tooltip, enabled, padding, size (font/icon), bold and color (green/red/yellow/blue).
The player's **Lua Plugin Text Scale** setting applies to native labels, button
captions and editable text, including already open third-party windows. It is
independent of chat text scale. Do not multiply font sizes by chat scale yourself.
Prefer flexible widths and wrapping; fixed-size panels must allow for larger
text. The same Lua UI runs on desktop, VR/Quest and Android phones/tablets, but
test touch targets and layout at each intended screen size.

Rows normally divide their width equally. Use `compact=true` on a row to size
all its children to their content, or `auto_width=true` on individual children
to reserve just their natural width while siblings fill the remainder. Row
children align vertically at the center. Labels accept `wrap=false` to remain
on one line and clip within their control; use a tooltip for full long names.
Tabs use the shared navigation style: dark inactive backgrounds, with a brighter
background and gold underline on the selected page. Selection follows the page
ID through live updates and reordering. Use `tabs` for page navigation so custom
plugins receive the same selected-state treatment as the built-in interfaces.
Text `size` is a point size from 10 to 36 (default 15), including intermediate
sizes such as 11, 13 and 15. Updating it on an existing control takes effect
without replacing the control or losing focus in neighboring inputs.
Progress is a fraction 0-1 with its label drawn over the bar. Select takes up to 256 string options. Number and
slider accept min/max/step; native code clamps/snaps the returned value.
For progress controls, `color` sets the fill and `text_color` sets the label
(light text by default).

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

Custom images preserve their source aspect ratio inside their width/height box.
All controls and containers accept `scale` (0.25–4, default 1), `offset_x` and
`offset_y` (-2,048–2,048, default 0). These are render transforms: scale and
rotation are applied around the control's center, then offsets move it in its
parent's logical pixels (right/down positive). Child controls and their hit
targets transform together. Nesting a transformed column scales its whole panel
content. Render transforms do not resize the window or reserve extra layout
space; provide sufficient surrounding space and avoid overlapping controls or
clipping at panel edges. Use width/height/padding for layout allocation. These
values multiply the normal UI scale and coexist with Lua Plugin Text Scale.
All controls accept `opacity` (0–1), `rotation` (degrees, -360–360), and
`effect="blink"`, `"pulse"`, or `"rotate"`. `period` is 0.1–60 seconds;
`min_opacity` defaults to 0.2 for blink/pulse. Native Slate animation advances each
frame even when Lua callbacks run slowly. Text `outline` (0–8) and `outline_color`
add an outline. `glow` (0–8) enables a simple tinted halo for images, or an outline
for text, using `glow_color`; these are lightweight UI effects, not postprocess
materials. See [Graphics Gallery](Examples/graphics_gallery/main.lua) for layered
artwork, a vital gauge, a countdown, native effects and clickable controls.

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
  Files must be directly inside your plugin's `sounds` directory: PCM16 WAV,
  mono/stereo, 8–48 kHz, at most ten seconds and 1 MiB. Paths and URLs are rejected.
  Uses the service rate limit; no looping or persistent audio source is created.
- **log**: append text and optional level, or clear. snapshot.log contains the last
  200 entries. API 2 also logs native activity errors/status milestones there.
  This is a session ring, not an unbounded disk log. Use bounded state for any
  selected history that should survive logout.

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

The host uses the same native gameplay implementation for UCM and API 2. The
PublicAPI regression creates a third-party ID with UCM disabled, verifies full
snapshots, target-death wakes, native route geometry, force buffs, failure
isolation, state restoration, nested UI events, profile archive/permission checks,
and the shipped policy module. Existing UCM/VM/companion regressions remain in use.
Source is shared with Windows/Linux/VR/Quest and Android phones/tablets. Platform-specific live gameplay and
server behavior still require testing; an editor automation pass does not prove
all plugins or servers behave identically.


## Optional terrain overlays (API 2)

The bundled **Dereth Lens** companion is disabled by default. Its Lua script owns
all settings and per-character persistence. It requests a native visual surface
using the new `terrain` permission, without a gameplay snapshot, native DLLs,
custom meshes, or shaders:

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

Third Eye provides name matching, alerts and world/map markers. Target plates
belong to separate addons. Each plate addon supplies its own opacity sliders and
saves its preferences using `state`; the host applies that addon's `world_plates`
opacity values independently. The [Target Plate example](Examples/target_plate/main.lua)
demonstrates separate desktop and VR controls. Updating Third Eye retains watch
rules and removes its obsolete plate preferences.


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
