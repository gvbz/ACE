# Companion extension (API 1 and 2)

For richer UI, full read snapshots, profile/import services and independent
automation, see [Public API 2](PublicAPI.md). The API 1 examples below still work.

Both APIs also support opt-in [chat display filters](PublicAPI.md#read-and-filter-chat),
[native button/settings visibility](PublicAPI.md#native-window-controls), and
[inventory-style item icons](PublicAPI.md#inventory-style-icons).

## Companion lifecycle

Use `"mode": "companion"` for quest trackers, nearby-object lists and other
helpers that should run alongside UCM. Existing automation scripts retain their
exclusive action ownership. Companions automatically start when enabled and a
character is in the world, continue across death, and stop on logout. Explicit
Stop lasts until Start or the next login. Closing their window keeps them running.
Starting, stopping or crashing a companion does not cancel another plugin's
movement, casting or attacks. They cannot directly cast, attack or move.

Copy an example directory into `Saved/ClientPlugins/Installed`, then **Manage
Plugins > Refresh**, enable it and enter the game. The installed client's
`ClientMods/api.json` advertises `companion_api: 1`; earlier releases do not have
this extension. These features do not require UCM to be enabled.

Every valid installed plugin remains listed, including disabled plugins.
Plugin Manager's **Enabled plugin limit** defaults to 16 and can be raised or
set to **0** for unlimited. Disabled plugins use no slot; an enabled companion
still counts while stopped or with its window closed. The setting persists
across restarts and is controlled by the player, not Lua. See
[host lifecycle and limits](PublicAPI.md#lifecycle-and-ownership).

- [Kill tracker example](Examples/kill_tracker/main.lua): configurable progress
  regex, persisted count, NPC and monster markers, clickable selection, and NAV.
- [Nearby targets / Vsense example](Examples/vsense/main.lua): click any listed
  object to select it, with a configurable name filter.

The kill tracker is a starting point, not a cross-server quest database. It tracks
one configured task's server progress pattern. Customize the regex to identify
the specific quest and parse your server's `/myquests` output; do not infer a kill
from an object disappearing. Some servers use different quest query commands.

## Manifest and permissions

```json
{
  "api": 1, "mode": "companion", "id": "my_tracker",
  "name": "My Tracker", "version": "1.0.0",
  "permissions": ["chat", "state", "ui", "markers", "waypoint", "messaging", "commands"],
  "settings": [{"key":"filter", "type":"text", "label":"Name contains"}]
}
```

Only request capabilities you use. Adding a permission requires enabling the
plugin again. `chat` exposes received messages (including tells); `state` grants
one character-specific saved object; `ui` allows rows and selection; `markers`
allows world/map annotations; `waypoint` controls the enabled Waypoint arrow;
`messaging` exchanges local events; `commands` invokes the documented native
command bridges. `identify` permits targeted background appraisal, and `sound`
permits bounded WAV notifications from bundled assets or user-selected imported
sounds; see [API 2](PublicAPI.md#permissions-and-snapshots).
Companion manifests cannot request automation or native-tool
permissions. Native settings also support bool, number and choice controls.

## Callback and snapshot

Return a function `tick(snapshot, profile)` as before. Companions run at most twice
per second, independently of UCM's action waits. The 32 MiB heap and 250,000 Lua
instruction limit still apply. Structured outputs are copied through a bounded
serializer; cyclic tables, non-finite numbers, functions and mixed/sparse arrays
are rejected. Each structured field is limited to 4,096 nodes, 12 nesting levels,
64 KiB of text and 1,024 array entries, with the lower feature limits below.

Snapshots include:

- `time`: monotonic seconds; `player`: GUID; `server`: server name.
- `position`: `{cell,x,y,z}`; `selected`: current selected GUID.
- `health`, `max_health`, `stamina`, `max_stamina`, `mana`, `max_mana`.
- `objects`: up to 512 nearby selectable world objects, nearest first, within
  240 meters and the same dungeon/outdoor area. Each has `id`, `name`, `wcid`,
  `distance` in meters, `position`, `monster`, `player`, `corpse`, and `dead`.
  This is client awareness, not a global monster database or line-of-sight query.
  `objects_truncated` reports a full list. No automatic appraisals are sent.
- `state`: the last accepted saved state object, initially `{}`.
- `error`: last native action/save error, or `""`. Display it or handle it.
- `plugins`: map from installed ID to `{enabled,running}`. It exposes no profile
  or private state and grants no capability to bypass the other plugin's checks.
- `chat_events`: new received events for this companion, each
  `{serial,time,text,raw_text,sender,sender_type,color,chat_type}`. `text` includes rendered speaker
  wording; `raw_text` is the original message. `color` is the retail chat type.
  Events arrive regardless of visible chat-window filters and without running
  UCM. Each callback delivers up to 128 events and 64 KiB of UTF-8 message text,
  oldest first. Larger bursts (including `/myquests`) remain queued and arrive
  automatically across subsequent callbacks; keep processing `chat_events` on
  every callback instead of issuing the command again for each batch.
  `chat_serial` is the last delivered event's cursor; `chat_latest_serial` is
  the latest received serial. `chat_pending` counts queued events still to be
  delivered to this plugin, and `chat_more` is true while that count is nonzero.
  These describe the received backlog, not whether the server has finished
  sending a command's response.
  The shared backlog retains up to 32,768 events or 8 MiB of UTF-8 message text
  (`text`, `raw_text`, and `sender`), whichever limit is reached first. Each
  message field is truncated to 4,096 characters and the sender to 256.
  `chat_gap` reports unread events lost to these limits; `chat_dropped` is the
  number missed before the first event in this batch. Normal pagination does
  not set `chat_gap`. Chat is not persisted or replayed from previous logins.
  Separate plugins have separate delivery cursors; a faster reader does not
  consume a slower reader's messages. Validate the sender/type before trusting
  progress messages.
  For low-latency delivery of only relevant lines, use native
  [chat_subscriptions and chat_history](PublicAPI.md#read-and-filter-chat).
  Their inbox limits and counters apply to matched messages only.
- `events`: up to 64 queued clicks, local commands and plugin messages, consumed
  by this callback. Each has a global `serial`; gaps can represent other plugins'
  events. Oldest entries are replaced on overflow.

Input tables are copies. Mutating `snapshot.state` or `profile` alone does not
save anything. `state`, `ui`, `markers`, and `messages` are optional output fields;
omitting a field retains the current presentation/state. Return `{}` for an empty
state, and `ui={}` or `markers={}` to clear those lists.

## State that survives login

```lua
return function(s, p)
  local state = s.state
  state.count = (state.count or 0) + 1
  return {state=state, status="Count: "..state.count}
end
```

State lives under `Saved/ClientPlugins/State/<plugin-id>/<server-hash>_<character-guid>.json`,
separate from editable profiles. It is limited to 64 KiB serialized JSON. Changed
state is atomically replaced at most every five seconds, and flushed on Stop,
disable, reload and logout. A crash can lose the last five seconds. Write failures
are reported and retried; unreadable existing files are preserved rather than
overwritten. Store quest names/keys and counts, not session-local object GUIDs.

## Interactive rows

```lua
return {
  ui={
    {id="progress", text="Red Rat task: 12 / 50"},
    {id="rat", text="Select nearby Red Rat", action="select", target=object_id},
    {id="nav", text="Point NAV arrow here", action="waypoint", target=object_id},
    {id="refresh", text="Refresh quests", action="command", command="/myquests"},
    {id="reset", text="Reset count", action="event"}
  }
}
```

Up to 128 rows, unique IDs of at most 64 characters, text up to 512 characters.
Rows without actions are labels. Action rows are native buttons, shared by desktop
and VR, with no gameplay input leakage. A click queues
`{type="click",id="reset",ok=true,error=""}`; `select` and `waypoint` revalidate
the object at click time. A vanished or out-of-area target is rejected. `command`
uses the separate `command` field so its button caption can be friendly text.
Button clicks never implicitly attack or use a selected object.

API 2 companions can use `action="select_item", target=item_id` with optional
`container=direct_container_id` to select items in owned packs or a currently
open chest/corpse. This requires `ui` permission. The current container and item
membership are checked on every click; closed/stale containers are rejected.
This only changes selection, without opening, using, moving or looting items.
API 2 `select` also accepts these accessible container items. See the
[Public API](PublicAPI.md#native-ui) for the color picker, scale/offset fields,
selection rules and complete example.

The same actions can be returned directly as flat fields:
`{action="waypoint",target=id}`, `{action="select",target=id}`, or
`{action="command",text="/myquests"}`. Direct repeated actions are the author's
responsibility; prefer explicit buttons for navigation changes and commands.

## World and map markers

```lua
return {markers={
  {id=npc_id,shape="!",color="yellow"},
  {id=monster_id,shape="arrow",color="green",world=true,map=true}
}}
```

Up to 32 markers per companion, 128 combined. Shapes: `!`, `arrow`, `diamond`.
Colors: `green`, `yellow`, `red`, `blue`, `white`, `purple`. `world` and `map`
default true. IDs must refer to current nearby world objects. Annotations disappear
when the object dies/leaves awareness or the plugin stops. World markers are small
depth-tested meshes above the object, including in VR, with no collision/shadows.
Map markers use Waypoint's overworld/dungeon projection, zoom and orientation.
They currently annotate the Waypoint map, not the retail compass radar.
This API does not load arbitrary textures or execute custom Slate/widget code.

## Local messages and commands

```lua
return {messages={{to="other_tracker",topic="quest_changed",text="red_rats"}}}
```

For messages between separate client processes, API 2 companions can instead
use [`client_messaging`](PublicAPI.md#channels-between-local-clients).

Both sender and receiver must have `messaging`; the receiver must be a running
companion. Up to eight messages per callback, topic up to 64 characters, text up
to 1,024. The recipient receives `{type="message",sender="my_tracker",topic=...,text=...}`
on a subsequent callback. Sender identity is assigned by native code. Messages
are local, not chat/network packets; delivery to a stopped recipient reports an
error instead of starting it. Avoid message echoes.

Players can type `/acueplugin my_tracker <text>` to queue
`{type="command",text=...}`. `/acueplugin my_tracker start` and `stop` control its
lifecycle. Text is capped at 512 characters. These commands are not sent to the
server. Plugin IDs are case sensitive. The same `/acueplugin <id> <text>` syntax is
supported by UCM meta command actions and `ucmcommand()`, using UCM's existing
`say` capability for local command delivery.

The `commands` bridge accepts one command per second:

- `/acueucm ...`: existing native adapter, for example `start`, `stop`, `forcebuff`,
  `enablecombat true`, `nav load <file>`, or `setmetastate <state>`.
  `/acueucm help` lists the supported commands. UCM must be enabled.
- `/acueitems give|shop|profile|chest|restock|stop`: existing Item
  Tools jobs using its configured recipient, profiles and normal permissions.
  Item Tools must be enabled; it pauses UCM during inventory jobs as usual.
- `/myquests`: sends that exact server query; the results arrive via chat.

Arbitrary slash commands, remote code execution and direct cross-plugin profile
writes are not exposed. Automation plugins can continue to use the existing
`ucmcommand()` parser for supported meta adapters. Companion messages carry data,
and the receiving plugin chooses what to do with it.


## Titles and icons

With `ui` permission, API 1 companions can return `presentation={title="Quest Tracker", icon="tracker.png"}` alongside their rows. The same object in `plugin.json` supplies defaults before startup. See [presentation metadata](PublicAPI.md#plugin-title-and-toolbar-icon) for asset limits and reset behavior.
