# Asheron's Call UI documentation (Stages 1–4)

| File | Purpose |
|------|---------|
| [DATPath.md](DATPath.md) | Retail DAT directory |
| [UIAssetManifest.md](UIAssetManifest.md) | Stage 2 summary |
| [UIAssetManifest.json](UIAssetManifest.json) | Full LayoutDesc trees + media |
| [UIAssetManifest.csv](UIAssetManifest.csv) | Flat asset rows |
| [ElementTypeRegistry.md](ElementTypeRegistry.md) | Type → C++ class |
| [ElementTypeRegistry.csv](ElementTypeRegistry.csv) | Same as CSV |
| [Architecture.md](Architecture.md) | Unreal class mapping |

Regenerate Stage 2:

```text
dotnet run --project Unreal/diag/UIInventory/UIInventory.csproj -c Release -- "C:\Turbine\Asheron's Call" "Unreal/Plugins/ACEClient/Docs/UI"
```

## Background audio and automation

In Options > Sound Options, enable **Play Audio in Background** and click
**Apply** to hear game sounds and plugin alerts while alt-tabbed. Reset discards
an unapplied change. Existing sound preferences are preserved; new installs
default to muting background audio.

UCM continues processing combat, recovery, looting, and navigation regardless of
the audio toggle. Desktop clients do not enter Unreal's inactive-window idle
mode, and editor play sessions bypass the editor's background CPU throttle.
The login FPS cap and saved gameplay frame limit remain in effect.

## Chat selection and fellowship rosters

Drag across chat text to select exact characters, or double-click a word, then
use Ctrl+C. Ctrl+V inserts the copied text at the chat-entry caret. Right-click
still offers Copy message and Copy chat window, plus Copy selection when text
is selected. Partial selection uses the active row's event geometry so retained
offscreen rows cannot redirect the range after scrolling or scaling.

The fellowship list displays every server-supplied member, using the available
panel height and its scrollbar. Retail `gmFellowshipUI::Update` likewise iterates
the complete fellowship table; the usual nine-member recruitment limit is not a
UI display limit. Regression coverage: `ACE.RetailParity.ChatInputAndCommands`
and `ACE.RetailParity.PanelResizeSocial` (including a sixteen-member wire roster).

Closed chat windows continue collecting matching messages in their bounded
scrollback. Closing/reopening does not change routing filters or clear history;
Clear remains explicit. Contract Abandon is enabled whenever a tracked contract
is selected, including Available contracts, and removal waits for the server's
tracker confirmation. The vendor category arrow uses the retail closed (1/2)
and open (6/7) DAT media states. These paths are covered by
`ACE.RetailParity.UIInteractions`.

Equipped ammo consumption is covered by `ACE.RetailParity.InventoryOrder` and
`ACE.VR.Protocol`: ACE sends Wielder=0 and CurrentWieldedLocation=0 on UIQueue
plus ObjectDelete on SmartboxQueue for the final shot, without an inventory
remove receipt. The client reconciles both queues before releasing the spent
stack. A projectile Pickup alone, preliminary unequip qualities, or a world
visual deletion alone must retain owned inventory.
