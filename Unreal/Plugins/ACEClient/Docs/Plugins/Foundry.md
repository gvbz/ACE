# Dereth Foundry

Enable **Dereth Foundry** in Plugin Manager and open its anvil icon to edit ACE
world content. Foundry was inspired by [AceCreator](https://github.com/harliq/AceCreator).

## Workflows

- **Items:** enter a WCID/classname or copy the current selection. Create a live
  object in the world/inventory or a persistent instance; inspect, import/export,
  remove an instance or delete the selected live object.
- **Move:** nudge the selected instance in eight compass directions, vertically,
  or to your location. Rotate to an explicit heading or your direction. ACE uses
  0 degrees north and 90 degrees west.
- **Land:** use the current hexadecimal landblock ID, import/export JSON/SQL,
  reload the landblock, print your location or clear the server cache.
- **Quests:** import/export quest names and recipe IDs in JSON/SQL.
- **Spawns:** create a generator instance from its WCID, capture a parent GUID,
  link a child instance, or add/remove an encounter.
- **Files:** choose a local ACE content folder, filter and page through JSON/SQL
  files, select a file to fill the matching tab's ID, or open it in a text editor.
- **Help:** the creation workflow, access requirements and recent request results.

For an initial edit, export an existing object, open the exported file in Files,
edit/save it, import its ID, then create a preview. Create a persistent instance
when ready, position it, and export the landblock. Persistent edits, imports and
deletes have an inline review step; changing selection cancels a pending targeted
edit. Commands are queued individually with a one-second minimum interval.

## Local files and server permissions

The server must authorize each developer or admin command. Check chat for the
server's response.

The workshop displays slash commands, while the native content service sends
ACE's `@` command prefix in the Talk packet (for example `/ci 123` becomes
`@ci 123`). `ACE.Plugins.Foundry` exercises both creation buttons through Lua,
the public service dispatcher and the outgoing packet, in addition to checking
permissions and target validation.

The local content folder contains `json/weenies`, `sql/weenies` and equivalent
`landblocks`, `quests` and `recipes` directories. Windows offers a native folder
picker and opens files in Notepad. Other platforms use `ClientMods/Content` and
the platform file manager/editor where available. Imports read the **server's**
`content_folder`; for a remote server, copy the edited file there separately.
**Show server folder** requests that setting through ACE.

## Lua integration and runtime cost

`foundry/main.lua` owns the tabs, validation, review, per-character fields and
bounded request queue. `admin_content` supplies only the native command and file
services documented in [PublicAPI.md](PublicAPI.md). The plugin requests only
selection and position at one-second intervals, scans files on demand, pages 128
results at a time, and retains only eight recent status messages. Opening it does
not create objects, change the server, scan directories or request inventory.
