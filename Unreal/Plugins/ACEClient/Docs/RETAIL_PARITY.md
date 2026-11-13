# Retail client replacement: acceptance and evidence

Vendor affordability follow-up (2026-11-13): normal vendor single-item, cart-row
and Buy All actions now check the full retail purchase price before sending Buy.
The pyreal purse comes from player `CoinValue` (int quality 20), as in retail
`gmVendorUI::UpdateTotalValue` (004C48E0); `BuySingleItem` (004C2820) and the Buy All
handler reject insufficient funds before `CM_Vendor::Event_Buy`. Rejection keeps
the cart and selection and displays "You don't have enough money" without starting
the hourglass. Mobile/VR vendor menus share the same check. Alternate-currency
shops retain the existing live owned-currency count; currency identity uses its
advertised WCID, not the display name. Item Tools' explicit server-bank opt-in
remains separate from normal vendor UI. Packet, UI and Item Tools regressions
cover insufficient/exact funds, cart totals, balance updates, price rounding,
trade-note premiums and alternate currencies. This is local source and fixture
verification; the reported custom server has not been tested live.
Validation: Win64 Development Editor build and all seven suites in
`TestOutput/UCMKnownGaps-Nov12/vendor-affordability-tests/index.json` passed
(vendor transactions, three Item Tools suites, retail UI screens, mobile menus
and VR menus). The large retail screen fixture was split into vendor helpers to
stay within MSVC optimizer limits without disabling checks or optimizations.

UCM follow-up (2026-11-13) adds learned/wand/physical-proc/grenade debuff selection,
idle helper buffs, optional casting movement, server-scoped recipe imports and
Classic expression/regex compatibility. Physical proc weapons use actual attacks
and confirmed effects, thrown weapons now support configured offhands, and regex
`x`/`n` options plus mixed capture backreferences follow .NET semantics. The final
conditional/balancing regex implementation now includes capture restoration on
backtracking, assertion conditions and case-sensitive caches/capture names.
The known in-scope UCM implementation register is closed; external integrations
are excluded by user request. This is local source/fixture validation, not live
server or packaged-device acceptance. See the current
[gap register](Plugins/VirindiCompatibility.md) for evidence and external-data
requirements that remain.

Automatic fellowship management and UCM-integrated crafting are now implemented
(2026-11-13). The optional manager includes nearby tell recruitment, FIFO
reservations, membership-confirmed operations and member votes. Crafting uses
verified catalog recipes, imported stock targets and inventory-confirmed jobs,
with recovery taking priority between actions. See the current
[Virindi gap register](Plugins/VirindiCompatibility.md) for configuration,
intentional differences and remaining custom-recipe limitations.

The target is the complete retail 1802 client reproduced in Unreal: gameplay,
network behavior, controls, DAT content, and interface design. Graphical fidelity
work follows feature completion. A feature is not complete merely because its
button exists, its opcode has a constant, or a focused regression test passes.

This is the working acceptance record, most recently reviewed on 2026-11-13. The client
is **not yet a feature-complete retail replacement**.

## UCM known-gap review: 2026-11-13

Reconciled the historical VT gap lists against current runtime and source.
Closed additional receipt/scheduler races across metas, assistance, components,
pets and loot maintenance; added innate recovery-implement activation and
configured lockpicking for route doors. API metadata, permissions and selective
snapshot support are documented. Native profile replacement also preserves
damage-format metadata across repeated saves and reloads. The follow-up adds
server-observed fellowship command sequencing and Lua dismiss/leader operations,
and isolates AutoCraft errors by ingredient/request identity. See the
[current comparison and remaining feature categories](Plugins/VirindiCompatibility.md#known-gap-audit-and-interaction-follow-up-2026-11-13).
This is local source/policy/packet verification, not complete VT parity or live
ACE/GDLE acceptance. No release was published by this review.

## UCM buff/mana receipt follow-up: 2026-11-12

Compared VT's buff and worn-mana rules with ACE and pinned GDLE mana-stone
handling. Fixed blocking recharge appraisal waits, lost/misattributed buff and
mana outcomes, unavailable supply selection, and consumable buff retry stalls.
GDLE's public item-mana updates now reach Lua instead of being discarded; ACE
still refreshes from full appraisals. Missing/removed mana remains absent in
integer property maps, so resource recovery cannot change loot-rule semantics.
See [the comparison and coverage record](Plugins/VirindiCompatibility.md#buff-receipts-and-equipment-mana-follow-up-2026-11-12).
Live-server timing remains an acceptance check; this is not a claim of full VT
or retail parity.

## Equipment mana while UCM is stopped: 2026-11-12

An Encrusted Bloodstone Jewel could run empty despite a 33% equipment threshold.
Two client paths can explain missed recharges: the general one-ID-per-second
scan revisited the first eligible inventory entries every 120 seconds, starving
later items in large inventories; a stopped-service stance timeout also latched
`IdleManaFailed` until a restart or profile change.

- Refresh worn mana before general inventory appraisals, starting with the
  lowest known percentage, with a 30-second per-item retry/refresh interval and
  the existing global request budget. Include jewelry and parent-attached gear.
  General inventory refreshes now select the oldest request. Known carried
  supplies retain the general cadence because their mana does not drain.
- After a successful mana use, prioritize fresh equipped-mana readings without
  discarding equipment appraisals. Confirm an actual mana increase before another
  refill; do not mistake `UseDone` alone for a full suit. Refresh source/target
  resources after manual use as well as UCM/Action Deck use.
- Pass stopped-service operational failures back to the restricted recharge
  policy for its bounded cooldown. Keep UCM stopped, respect inventory action
  locks, and reserve the permanent failure latch for script/load errors.
- Cover 33% jewelry thresholds, an empty jewel, repeated recharge cycles, mana
  stones/charges/custom ManaStone-category supplies, player-targeted suit use,
  large inventories, manual recharge and recovery from a stance timeout.

References: ACE `Player.ManaConsumersTick` updates server mana without sending
an ordinary item property for every drain. VT `ev.cs` refreshes worn appraisals,
compares each item's mana percentage and targets the player for suit recharging;
`cLogic.InitializeDefaultLogicRules` enables `RefillWieldedMana` while stopped
through `ManaChargesWhenOff`. The user's precise supply and failure sequence are
unknown; these are reproduced client failure paths, not a live-server replay.

Validation: editor build, resource-maintenance/recovery policy tests, actual
stopped-service dispatch and packet tests, equipment assessment-cache tests and
the 512-item idle workload passed. The affected host/cache/workload suites were
rerun after adding manual recharge handling. Quest/Android shared-source checks
passed. Release v156 carries this change across the supported platforms; its
packaging and regression evidence is recorded in the release receipt. The
reporting character's exact setup has not been verified live.

## GDLE combat stance and UCM buffing: 2026-11-12

Reefcull reports `Combat stance was not confirmed; action deferred` when UCM
attempts to buff. UCM required a new `CombatMode` integer quality (40) before
dispatching the spell. GDLE's `ChangeCombatMode(mode, playerRequested=true)`
intentionally omits that quality on success; it only publishes a correction or
a server-initiated change. It can also omit the motion update when the requested
stance is already active, so waiting for motion is not a reliable substitute.

- Match retail's immediate requested-mode state for GDLE. Send the stance and
  subsequent spell through the same reliable, ordered weenie action queue.
- Retain the actual received quality rather than inventing a server update.
  Explicit server corrections remain authoritative, including rejection to
  peace. ACE continues to require its normal stance acknowledgment.
- Cover magic, melee, missile and peace transitions, repeated silent requests,
  correction handling, and consecutive UCM buffs with packet-order assertions.

Evidence: retail `ClientCombatSystem::SetCombatMode` (`0x0056BE30`) updates its
mode and sends `CM_Combat::Event_ChangeCombatMode` without a success acknowledgment;
`OnQualityChanged` applies server corrections. Public GDLE reference:
[`CMonsterWeenie::ChangeCombatMode`](https://github.com/esoterick/gdle-linux/blob/542a209d89e13d6fbb5df3f3e77fd72d10a4b647/Source/Monster.cpp#L2044)
and its `CClientEvents::ChangeCombatMode` caller. The regression tests reproduce
GDLE's silent-success contract locally; Reefcull's deployed source and a live
session have not been verified.

Validation: Windows editor build and all ten selected automation suites passed,
including `ACE.Network.GDLEInteractionTransport`,
`ACE.Plugins.RouteAndTellRequests`, buffing/recovery, Action Deck, AutoCraft and
network/quality regressions. Quest and Android shared-source synchronization
checks passed. Release v156 includes this fix; see its release receipt for
packaged-platform validation. Live Reefcull acceptance remains unverified.

## Public retail chat command audit: 2026-11-11

Compared all 130 registered names in retail
`ClientCommunicationSystem::InitializeCommands` and `StartupTurbineChatSystem`:
123 executable names (including aliases) and seven help-only topics. The local
help catalog has no missing or extra names relative to those registries. This
covers public retail client commands, not every server-specific or privileged
debug command. Unknown commands still reach the server through Talk.

- Implement `/consent on|off|who|clear|remove` and `/permit add|remove` with
  retail character-option IDs, action opcodes, string payloads and queues.
- Implement `/corpse` / `/cor`. Retain the login position-quality table and
  ordered private/public position-quality updates, including last outdoor death
  position 14. Keep these separate from player movement, reject incomplete and
  stale updates, and clear them on logout. Coordinates follow retail's outdoor
  cell conversion rather than the current character's position.
- Implement `/render fov 10-160` and `/render radius 5-25` with saved settings,
  numeric validation and incremental terrain streaming. Preserve native Android's
  terrain memory cap and disabled login-background streaming.
- Restore `/all` and `/o`, the legacy `/friends old` request, house boot/remove
  aliases, storage permission shortcuts and guest/storage `show`/`list` commands.
  Validate housing types instead of silently requesting cottages. `/house` alone
  displays help rather than recalling the character.
- Make `/afk on` and `/afk off` idempotent, with authoritative AFK-state updates;
  match retail's AFK message limit/reset behavior. Accept whitespace-separated
  subcommands and quoted multiword names in housing, friends and allegiance
  commands. Correct recall argument handling and age/birth query payloads.
- Require the retail confirmation for `/die` and both confirmations for
  `/house abandon`. Cancellation sends no destructive action.
- Provide local syntax/help for every public retail name and all seven help
  topics. Retain detailed AC:Unreal-specific help and server-defined commands.

`ACE.RetailParity.ChatCommandRegistry` checks the complete name fixture, local
help/dispatch, actual serialized action packets, confirmation/cancellation,
position-quality login and update handling, and render preferences. The audit
also exercises existing chat, arena recall, transport, player options, housing,
social, UI-layout and character-quality suites. Development Editor build and
all 11 regression suites passed; Quest and Android source synchronization checks
passed. The expanded UI test caught an options-array ordering dependency;
controls now resolve settings by name, preserving brightness, contrast and light
controls when new options are added. Build/test evidence and the independent
registry comparison are in `TestOutput/ChatCommandAudit-Nov11/`.
These are local/offline tests; live-server execution and native-device rendering
remain unverified. No packaged release was made for this audit.

## PK and PK Lite arena recall commands: 2026-11-10

Compared retail `ClientCommunicationSystem::DoPKArena` / `DoPKLArena`, their
registered `pka` / `pla` aliases, `ACCWeenieObject::IsPK` / `IsPKLite`, and
`CM_Character::Event_TeleToPKArena` / `Event_TeleToPKLArena`. ACE confirms the
same empty action payloads in `GameActionTeleToPkArena` / `GameActionTeleToPklArena`.

- `/pkarena` / `/pka` send action `0x0027`; `/pklarena` / `/pla` send `0x0026`
  on the reliable weenie queue. Previously these unrecognized client commands
  fell through to ordinary Talk, so the server never received a recall request.
- Accept both `/` and `@`, case-insensitively, through typed chat and Action Deck.
  Reject arguments with the retail usage notice and provide help for all aliases.
- Match retail's player-descriptor eligibility check, including rejecting Free PK
  as a substitute for PK/PK Lite. If the player object is unavailable, defer to
  the server. Busy state, PK timers, recall animation and teleport remain server-owned.
- Add `ACE.RetailParity.ArenaRecallCommands` covering actual command submission,
  serialized queue/opcode/empty payload, alias/case handling, eligibility, arguments,
  local help, Action Deck draft preservation, and disconnected-session rejection.

Validation: Development Editor build and `ArenaRecallCommands`,
`ChatInputAndCommands`, `PkStatus`, `EquipmentAssessmentCache`, and
`QualityRemoval` passed. The build also exposed existing anonymous-namespace
helper collisions in equipment-cache code and the quality-removal fixture;
isolated those helpers in named namespaces without changing behavior. Quest
and mobile source synchronization passed. Evidence is in
`TestOutput/ArenaRecall-Nov10/`. These are offline command/protocol tests;
no live-server arena teleport or new packaged release was performed.

## Character quality removal and live panel values: 2026-11-10

Compared retail `CM_Qualities` private/public remove dispatchers,
`ClientObjMaintSystem::RemoveStat`, `QualityRegistrar::CallRemoveHandler` and
`ClientCombatSystem::OnQualityChanged`. These removal messages have no value
payload; public removals put the GUID before the property, including strings.

- Handle private/public removal of character int, int64, bool, float and string
  qualities (`0x1D1`–`0x1D8`, `0x2B8`/`0x2B9`). Delete the map entry instead of
  replacing it with zero. Lua character maps and component-policy defaults now
  reflect server deletion without needing a relog.
- Use each replacement family's timestamp for removals too. A late replacement
  cannot resurrect a removed value, and an old removal cannot delete a newer
  value. Preserve retail's equal-stamp and wraparound behavior; reject partial
  headers before advancing the stamp.
- Recompute derived skills/vitals after deletion, remove cached augmentation,
  aetheria, character-info, XP/luminance and template values, and refresh vitae
  subscribers where relevant. Public/private character integer replacements
  now share their application path, including live level, credits, age, deaths,
  fishing/chess and heritage values instead of leaving the panel mirror stale.
- Removal is not a synthetic object update: preserve public descriptor flags,
  icons, burden, ownership and the current combat stance. Retail does not call
  the descriptor's `OnStatUpdated` on removal, and its combat handler keeps the
  current mode when the CombatMode quality is absent. Duplicate removals retain
  ordering without rebuilding unchanged UI.

Scope: these are the five quality maps already stored for the current character.
Generic non-player qualities, DataID/InstanceID/position quality removal and
their subscriber APIs remain unimplemented; no inventory move/drop is inferred
from removing a quality. Public API docs explain absent keys versus zero/false.
Validation: Win64 editor build and all ten focused suites passed:
QualityRemoval, PrivateStatParity, LivePropertyUpdates, RouteAndTellRequests,
AugmentationStats, CharacterInformation, CharacterStats, CombatProtocol,
NetworkTransport and PkStatus. Wire fixtures cover all five families and every
private/public update/removal pairing, malformed headers, equal/late/wrapped
stamps, character isolation, early luminance removal, real DAT stat recomputation,
Lua snapshot key deletion and restoration of the component-policy default.
Evidence: `TestOutput/Parity-Nov10/quality-removal-final-build.log` and
`quality-removal-tests/index.json`. No live-server removal stream was exercised.

## Live item DataIDs and string update compatibility: 2026-11-10

Compared retail `ACCWeenieObject::OnStatUpdated(DataID)`,
`ClientObjMaintSystem::UpdateStat<DataID_QualityType>` and
`UIQueueManager::HandleStringUpdateEvents`, plus ACE's
`GameMessagePublicUpdatePropertyString` writer and retail spellbar endowment
selection (`gmSpellcastingUI::UpdateEndowmentIcon`).

- Explicit zero DataIDs now clear icon, overlay and underlay fields; the same
  rule applies to our existing item-spell update path. Removing an innate spell
  no longer leaves a stale caster slot selected. An appraisal's explicit zero
  icon also clears the inspect result's fallback to the previous icon.
- Handle public descriptor PhysicsScript (44) separately from the physics
  default script. Changed inventory/held-item metadata invalidates the cached
  Lua inventory, so UCM/plugins receive current icons and innate spells without
  a manual inventory refresh. Equal retransmissions avoid cache churn; metadata
  changes do not recreate actors or discard successful appraisals.
- Inventory object arrival/replacement now also invalidates the cached list.
  This covers CreateObject completing after ContainId or an inventory snapshot,
  when no further containment receipt arrives to refresh a previously absent row.
- Decode public strings in retail GUID/property order and the local ACE writer's
  property/GUID order. Known object identity selects the ACE form, with retail
  taking precedence if ambiguous. Player identity works before ObjectCreate;
  no fixed property-ID range excludes custom server keys. Receive ordering and
  dispatch use the same decoder, sharing timestamps across private/public and
  both wire layouts. Reject incomplete text/alignment before consuming a stamp.

Scope: this is explicit DataID replacement/clearing, not DataID quality-removal opcode
support. Non-player string-quality storage/notifications remain a gap; this pass
does not rewrite public object names from string qualities. Retail's string
quality update path does not directly modify its public name descriptor.

Validation: Win64 editor build and all seven focused suites passed:
LivePropertyUpdates, SpellDataRevision, EquipmentAssessmentCache,
RouteAndTellRequests, InventoryOrder, ItemPresentation and NetworkTransport.
Fixtures cover explicit clears, duplicate/stale/wrapped stamps, every truncated
string prefix (including padding), both public string layouts, private/public
shared ordering, player updates before ObjectCreate, appraisal icon clearing,
spellbar selection, and immutable Lua inventory refresh with appraisal retention.
The cached-empty-inventory fixture exposed and now covers the object-arrival
invalidation gap. Evidence: `TestOutput/Parity-Nov10/live-properties-final-build.log`
and `live-properties-final-tests/index.json`. No live server has been exercised
for these changes.

## Writable book pages: 2026-11-10

Compared retail `gmBookUI::DisplayPageData`, `PageTextBlank`, `CloseCurPage`,
`SetCurPage`, `OpenBook` and `OnVisibilityChanged`, plus `CM_Writing` and
`PageData`. Cross-checked ACE's `Player_Book`, `Book`, and book response writers.

- Decode each page's author ID/name and `IgnoreAuthor` flag in both supported
  PageData formats. Account strings remain discarded. Only the author or a
  shared page gets an editor; other pages retain the read-only reader.
- The desktop DAT book panel now supports multiline writing with its DAT font,
  character limit, clipping, wheel scrolling and existing scrollbar. Keyboard
  focus suppresses gameplay input. Turning pages, closing the panel, or switching
  panels saves changes; closing/turning away from one's blank page deletes it.
  Blank detection follows retail's space/newline rule (tabs are not blank).
  The title now targets the actual DAT `DisplayedBookNameText` element and shows
  the book name (or inscription) and current page instead of an empty header.
- Add pages up to the supplied capacity, including the first page of an empty
  book and pages after another author's writing. Do not append repeatedly after
  an already blank last page. Read back authoritative metadata after creation.
- Implement modify/add/delete actions `0xAB`/`0xAC`/`0xAD` on WeenieQueue and
  responses `0xB5`/`0xB6`/`0xB7`, using zero-based page indices. Changed-page
  navigation waits for acknowledgement before changing local indices. Partial,
  mismatched and duplicate replies cannot commit the draft or move the reader.
  Failed writes retain the editable draft while the book is open. An unconfirmed
  write times out without resending a potentially non-idempotent add/delete;
  the player is asked to reopen the book before further editing. Duplicate open
  snapshots do not overwrite an unsaved or pending draft.
- World exit/disconnect discard editing state without writing as another
  character. Closing sends a save once and late write acknowledgements cannot
  reopen the panel. The existing subsystem navigation wrapper uses the same
  save-before-turn path.

Validation: `ACE.Network.BookWriting` uses actual encoded events, loopback
outbound packets and the DAT book panel, including rendering at 1x/1.5x,
editable-page scrollbar routing, author permissions, capacity, failed/truncated
replies, timeouts, close/panel-switch saves and world reset. Evidence is under
`TestOutput/Parity-Nov10/book-writing*`; rendered pages are in the isolated test
user directory's `Saved/Automation/BookWriting` folder. This is local protocol
and desktop UI validation, not a live-server persistence test. ACE's current
modify handler always acknowledges success, even when its internal modification
fails, so an acknowledgement alone is not proof of server-side persistence.
The native mobile reader and retail page-selection menu are not expanded by
this pass.

Win64 editor build and all seven focused suites passed (BookWriting,
BookContainerParity, ServerDialogs, KeyboardBindings, NetworkTransport,
TextEntryScroll and UIScreens). After fixing the title binding, the build and
BookWriting/UIScreens passed again; the final 1x/1.5x renders were inspected.

## Server attacker identity and Last Attacker selection: 2026-11-10

Compared retail `CM_Qualities::DispatchUI_PrivateUpdateInstanceID`,
`ClientObjMaintSystem::UpdateStat<InstanceID_QualityType>`,
`ACCWeenieObject::OnStatUpdated`, `ClientCombatSystem::AutoTarget` and its
defender/evasion notifications, plus `CPlayerSystem`'s Last Attacker action
(`0x10000038`) / `ObjectIsWithinRadarRange`. ACE's `Player.SetCurrentAttacker`
sends private IID 11 when the attacker changes, including evades and resisted
spells; it does not repeat that property on every hit.

- Added private InstanceID message `0x02D9` and shared its per-owner/per-property
  timestamp family with public `0x02DA`. Both feed the existing descriptor-IID
  application path. Truncated messages cannot change identity or consume stamps.
- CurrentAttacker uses the server GUID, including the initial PlayerDescription
  IID table, explicit clears, and targets not yet present locally. Incoming
  combat text no longer guesses a GUID by matching names. Another object's
  CurrentAttacker never changes the local player's selection identity.
- Hits and evades refresh the separate 15-second retaliation timer, even when
  the attacker ID did not change. Outgoing damage/misses and IID updates alone
  do not refresh it. Login snapshots, world exit and disconnect clear the timer;
  world exit/disconnect also clear the remembered attacker.
- The Last Attacker key (including rebinding) now requires a known selectable
  world object strictly inside the planar indoor/outdoor radar radius. It keeps
  the current selection if the attacker is absent or out of range. Manual Last
  Attacker selection has no 15-second expiration; that limit belongs to combat
  entry's retaliation preference.

Validation: Win64 Development Editor build succeeded. `ACE.Network.AttackerIdentity`
passed with duplicate creature names, hit/evade timing, private/public ordering,
wrap/equal timestamps, every truncated packet boundary, login/logout resets,
combat-entry targeting, default/rebound keyboard polling, and radar boundaries.
The six surrounding `ACE.RetailParity` suites (`CombatChat`, `InventoryOrder`,
`NetworkTransport`, `KeyboardBindings`, `SelectionToolbar`, `CharacterStats`)
also passed. Evidence: `TestOutput/Parity-Nov10/attacker-identity-final-build.log`,
`attacker-identity-final-tests/index.json`, and the six successful suites in
`attacker-identity-verified-tests/index.json`. The initial hotkey fixture processed
input twice and left the physical pawn at world origin; both fixture issues were
corrected before the focused test passed. No live server or device verification,
release, or claim of complete combat-system parity.

## Compact player-stat messages: 2026-11-10

Compared retail `CM_Qualities` private update dispatchers,
`ClientObjMaintSystem::UpdateStat`, `AttributeCache::SetAttribute/SetAttribute2nd`,
`CACQualities::SetSkillLevel`, `Skill::SetSkillAdvancementClass`, and
`ExperienceSystem::SkillLevelFromExperience` / `ExperienceTable::GetLevelForExperience`.

- Added the previously ignored private compact skill-level (`0x02DF`), skill
  advancement (`0x02E1`), and attribute-level (`0x02E5`) messages. A compact
  level replaces the starting value; it does not replace purchased ranks or XP.
  All six attributes refresh their dependent DAT formulas and movement skills.
- Skill advancement updates preserve starting bonuses and unsigned spent XP,
  then recalculate ranks using the trained/specialized DAT curve. Inactive and
  untrained skills have zero purchased ranks. Recalculation waits for the DAT
  resolver if unavailable; a later full skill packet supersedes it. Full packets
  retain explicit server ranks, including custom-server values.
- `0x02E9` now handles maximum-vital IDs 1/3/5 as starting values as well as
  current-vital IDs 2/4/6. Maximum updates preserve the corresponding ranks, XP,
  and current amount. Accepted messages publish through the existing shared
  stat notification used by gameplay, UI and plugins; no Lua API shape changed.
- Full and compact forms share retail's per-stat sequence family. Late packets
  cannot undo a newer update; equal stamps and byte wrap retain existing retail
  behavior. Compact packets validate length, IDs and representable values before
  committing timestamps, so rejected packets cannot suppress a valid update.

Validation: Win64 Development Editor build and all seven tests passed:
`ACE.Network.PrivateStatParity`, `ACE.RetailParity.CharacterStats`,
`ACE.RetailParity.Augmentations`, `ACE.RetailParity.AugmentationStats`,
`ACE.RetailParity.CharacterInformation`, `ACE.RetailParity.RunSpeed`, and
`ACE.RetailParity.NetworkTransport`. The new suite checks real DAT XP thresholds
(including unsigned XP and the cap), enchantments, dependent movement values,
shared publication, deferred resolution, custom full-packet ranks, new entries,
every compact-packet truncation boundary, invalid values, mixed full/compact
ordering, equal stamps, byte wrap and receive-state reset.

Evidence: `TestOutput/Parity-Nov10/private-stats-verified-build.log` and
`TestOutput/Parity-Nov10/private-stats-tests/index.json`. This pass covers private
player-stat messages; public object-stat messages and legacy full-skill
serialization are not claimed as complete. No live-server verification or release.

## Server instructions and confirmation lifetime: 2026-11-10

Compared retail `CM_Communication::DispatchUI_PopUpString`,
`ClientCommunicationSystem::Handle_Communication__PopUpString`,
`DialogFactory::CreateDialog_`, `MessageDialog::ListenToElementMessage`,
`Dialog::UpdatePopupSizeAndPosition`, the `UIMainFramework` destructor's dialog
reset, and ACE's `GameEventPopupString`, `GameEventConfirmationRequest` and
`GameEventConfirmationDone` writers.

- Previously ignored `PopupString` (`0x0004`) messages now show quest/minigame
  instructions using retail MessageDialog template `0x24` in DAT `0x2100003C`,
  its font/body, and its OK button (`0x26`). Dismissal is local: it never sends
  a `ConfirmationResponse` or other game action. Mobile uses an OK-only touch
  message, and VR opens the retail panel for controller interaction.
- Identical messages retain separate local identities. Stale dismissals cannot
  remove a subsequent message, including after character changes. Unlike retail's
  overlapping nonqueued message windows, this client presents instructions in
  arrival order and defers them while an action confirmation is visible. After
  64 pending messages, overflow text goes to chat rather than being discarded.
- Confirmation requests require the entire string and padding before replacing
  an existing prompt. Withdrawals match both type and context; replies retain
  the retail `0x0275` UI-queue payload. Malformed packets cannot partially update
  dialog state. This validation is additional to the retail wire format.
- Returning to character selection now clears confirmations as well as popup
  messages. Dialog rendering also checks world state, preventing old prompts
  from remaining visible outside gameplay. Full disconnect clears both queues.

Validation: Win64 Development Editor build passed, followed by all seven suites:
`ACE.Network.ServerDialogs`, `ACE.Network.SocialParity`,
`ACE.Network.BookContainerParity`, `ACE.RetailParity.Housing`,
`ACE.RetailParity.NetworkTransport`, `ACE.Mobile.TouchAndMenus`, and
`ACE.VR.RigAndMenus`. The new test checks every packet truncation boundary,
replacement/withdrawal identities, UI-queue reply bytes, repeated messages,
overflow, logout/disconnect cleanup, actual DAT fonts/controls and scaled OK
hit testing. Mobile and VR tests exercise local dismissal without a game action.
Rendered dialogs were reviewed at normal and enlarged scale.

Evidence: `TestOutput/Parity-Nov10/server-dialogs-verified-build.log`,
`TestOutput/Parity-Nov10/server-dialogs-tests/index.json`, and captures under
`TestOutput/Parity-Nov10/EditorUser-467d2e3d3a8847fd8de2b84d5e8a238f/Saved/Automation/ServerDialogs`.
These are local automation results. This pass does not claim full dialog-system
parity, physical-device testing, or live-server verification; no release was made.

## Housing guest reports and maintenance updates: 2026-11-10

Compared retail `ClientHousingSystem::Handle_House__Recv_UpdateHAR`,
`HAR::UnPack/Dump`, `GuestInfo::UnPack/Dump`, `HouseData::UnPack`,
`HousePaymentList::ClearPayment`, `gmHouseUI::RecvNotice_UpdateRentTime`,
`gmHouseUI::RecvNotice_UpdateRentPayment`, and the matching ACE event writers.

- `@house guest list` already sent the request, but its `0x0257` reply was
  discarded. It now prints the retail guest report, including storage-access
  asterisks, allegiance permissions, and open-house status. Both the legacy
  unversioned record and versioned records with/without roommate lists decode;
  roommate IDs are consumed without being printed, as in retail's `Dump(false)`.
  Complete names and nested lists are required before emitting any chat.
- The previously ignored rent-period (`0x0227`) and payment-list (`0x0228`)
  responses update the owned-house panel. A new period clears paid quantities
  while retaining requirements; a payment reply replaces the current list,
  without accumulating repeated replies. Neither response can create ownership
  after a no-house status. The separate slumlord offer remains authoritative to
  its own profile replies.
- Full house snapshots now require the complete location as well as both payment
  lists. They share the slumlord's checked string/payment decoder. Truncated
  snapshots/status notices preserve the last valid housing state; invalid counts,
  negative payment amounts and nonfinite positions are rejected. List sizes are
  bounded by the payload instead of an arbitrary 256-payment cutoff.

Malformed-message rejection is defensive validation around the retail wire
format. This does not implement world-object restriction replication (`0x0248`)
or certify every housing operation.

Validation: Win64 Development Editor build passed. All seven selected suites
passed: `ACE.Network.HousingParity`, `ACE.Network.BookContainerParity`,
`ACE.Network.SocialParity`, `ACE.RetailParity.Housing`,
`ACE.RetailParity.NetworkTransport`, `ACE.RetailParity.UIScreens` and
`ACE.VR.RigAndMenus`. Coverage includes guest-list command serialization,
legacy/versioned HAR replies, storage/allegiance/open-house output, duplicate
guest IDs, every truncation boundary, invalid payment counts, full/partial rent
updates, period reset, late replies after ownership clears and logout. The real
DAT housing test checks visible amounts and paid/unpaid colors, and the rendered
new-period panel was reviewed. The added command check lives in the housing
suite because adding it to the existing oversized UIScreens function exceeded
the compiler's optimization size limit.

Evidence: `TestOutput/Parity-Nov10/housing-network-verified-build.log` and
`TestOutput/Parity-Nov10/housing-network-tests/index.json`; captures are under
`TestOutput/Parity-Nov10/EditorUser-64faff53d5d149f28ee14d881f039ce0/Saved/Automation/Housing`.
These are local automation results; no release or live-server verification was
performed in this pass.

## Enchantment refreshes and plugin cooldowns: 2026-11-10

Compared retail `Enchantment::UnPack`,
`CEnchantmentRegistry::ReplaceEnchantmentInList/AttemptToReplaceSpellInList/UpdateVitae`,
the packed-ID equality operator at `ContentProfile.cpp:37`, registry snapshot
unpacking, and ACE's enchantment event writers.

- Incremental enchantment refreshes use spell/layer identity rather than caster
  identity. A timed recast on a new layer replaces the earlier timed copy, while
  permanent equipment layers remain separate. Vitae is a singleton; cooldown,
  additive and multiplicative effects retain separate registry membership.
- Full login registries retain the server's complete lists instead of running
  incremental recast replacement on each snapshot entry. Each registry/list must
  decode completely before changing active effects. Truncated update/remove/dispel
  batches cannot partially refresh buffs, remove effects or notify observers;
  nonfinite timer/stat values are rejected. This protects the enchantment section,
  not every other field in the larger PlayerDescription message.
- Plugin snapshots now explicitly include the cooldown registry when building
  `cooldowns`. Previously the shared getter filtered it out before that map was
  built. Native UCM, API 2 and selective companions receive shared item cooldown
  IDs and remaining time, including elapsed time restored at login. Buff/debuff
  lists and the default UI getter continue to exclude cooldown records.
- Public Lua API documentation specifies the cooldown map's key format, item
  property 280 relationship, remaining-time semantics and selective data access.

Malformed-message rejection is defensive validation around the retail protocol.
Custom server records without additive/multiplicative flags remain supported.
Validation: Win64 Development Editor build passed. All eight selected suites
passed without test warnings: `ACE.Network.EnchantmentParity`,
`ACE.Plugins.PublicAPI`, `ACE.Plugins.Recovery`, `ACE.Plugins.ResourceMaintenance`,
`ACE.RetailParity.CharacterStats`, `ACE.RetailParity.PlayerModuleOptions`,
`ACE.RetailParity.SelectionToolbar` and `ACE.RetailParity.UIScreens`. Coverage
includes caster/layer refreshes, independent equipment effects, registry ordering,
truncation boundaries, nonfinite values, atomic removals, and native/public/selective
plugin cooldown data, expiry and removal. Evidence is
`TestOutput/Parity-Nov10/enchantments-build.log`,
`TestOutput/Parity-Nov10/enchantments-final-build.log` and
`TestOutput/Parity-Nov10/enchantments-verified-tests/index.json`.
These are local automation results; no release or live-server validation was
performed in this pass.

## Contract updates, repeat timers and journal sorting: 2026-11-10

Compared retail `CContractTracker::UnPack`, `CPlayerSystem::UpdateContractTrackerTable`,
`gmContractsUI::FillProgressString/ListenToElementMessage`,
`ContractInfoSortStatus/ContractInfoReverseSortStatus`, and
`ClientUISystem::DeltaTimeToString` with ACE's contract event writers and DAT loader.

- Contract tables are validated completely before replacing the journal. Truncated
  snapshots/deltas and nonfinite timers preserve existing entries and timestamps
  without notifying the UI. Valid empty tables still clear the journal. Timers
  retain the wire's double precision, and duplicate IDs retain the last value.
- Completed contracts display `Done`, a remaining `Done (... to Repeat)` countdown,
  or `Available`, using receipt time and the DAT repeat flag. Expiration changes
  the displayed status without requiring another server event or rewriting the
  server's stage. Countdown formatting follows retail's month/day/hour/minute/second
  units; existing progress-count descriptions and wrapping remain intact.
- Clicking the same journal heading reverses sorting; changing headings resets
  ascending order. Status sorting uses displayed status, numeric repeat time and
  name ties, rather than raw stage numbers. Expired timers join Available entries
  consistently; selection remains on the same contract when rows move.

Malformed-message handling is defensive validation around the retail protocol.
The item appraisal-completion dispatch was also checked: the retail dispatch calls
a no-op implementation, so no new completion behavior was introduced there.

Validation: Win64 Development Editor build passed (87 actions). All four selected
suites passed without test warnings: `ACE.Network.ContractParity`,
`ACE.Network.SocialParity`, `ACE.RetailParity.UIScreens` and
`ACE.RetailParity.PanelResizeSocial`. Coverage includes every truncation boundary,
nonfinite timers, precision, add/update/delete, valid empty snapshots, duplicate
IDs, abandon request serialization, countdown expiry, DAT repeatability, numeric
sorting, direction changes and selection retention. The rendered journal was also
checked for wrapped countdowns. Evidence is
`TestOutput/Parity-Nov10/contracts-build.log` and
`TestOutput/Parity-Nov10/contracts-tests/index.json`.
These are local automation results; no release or live-server validation was
performed in this pass.

## Book reading and container snapshots: 2026-11-10

Compared retail `PageData::UnPack`, `PageDataList::UnPack`,
`gmBookUI::OpenBook/SetCurPage/BookPageDataResponseEvent`, and
`ClientUISystem::OnViewContents` with ACE's book/container event writers.

- Book decoding accepts both legacy `textIncluded` and version-2 page records.
  The page-list capacity is no longer mistaken for the number of existing pages.
  Complete page records and the scribe trailer are required before replacing the
  current book. Page indices cannot grow the cache beyond the received list.
- Received text is cached, including empty pages. Reopening a page within the
  same reading session no longer refetches it, and inline text avoids the initial
  redundant request. Desktop/VR and mobile distinguish blank pages from loading.
- Refreshing the same book retains the selected page, clamped to the final
  existing page if the new list is shorter, matching retail `OpenBook`.
- Page navigation waits for the outstanding request as retail does. A five-second
  retry escape prevents a nonresponding host from locking navigation indefinitely.
  Late replies populate only their own cache slot and do not change the selected
  page, complete a different request, or reopen a closed reader. Logout/reset
  clears both the book and pending request. The wire format has no per-request
  nonce, so replies from reopening the same book cannot be distinguished by age.
- Container snapshots are validated in full before allocating their list or
  changing ownership, expiry queues or the open loot panel. Complete empty
  snapshots still clear contents; truncated lists preserve the last good state.

Malformed-packet checks and the retry escape are resilience improvements around
the retail protocol. This pass covers reading, not authoring/deleting book pages.

Validation: Win64 Development Editor build passed. All six selected suites
passed: `ACE.Network.BookContainerParity`, closed-container lifetime, inventory
order, world-loot placement, mobile touch/menus and VR rig/menus (including the
shared desktop book reader's blank/loading display). The packet suite exercises
both page formats, truncation at every byte boundary, inline/missing/empty text,
late replies, refresh/clamping, outgoing request counts, logout/reset, and
container ownership/expiry/UI isolation. Reports are
`TestOutput/Parity-Nov10/books-network-tests/index.json` and
`TestOutput/Parity-Nov10/books-related-tests/index.json`; the latter reruns the
packet suite after the same-book refresh fix. Build evidence is
`TestOutput/Parity-Nov10/books-build.log` and `books-final-build.log`.
These are local automation results; no release or live-server validation was
performed in this pass.

## Social network state and allegiance information: 2026-11-10

Compared the local retail `Fellow::UnPack`, `Fellowship::UnPack`,
`ClientFellowshipSystem`, `FriendData::UnPack`, `gmFriendsUI::RecvNotice_UpdateFriendsList`,
`AllegianceHierarchy::UnPack`, and
`ClientAllegianceSystem::Handle_Allegiance__AllegianceInfoResponseEvent` with ACE's
social event writers and the client handlers.

- Fellowship and friends updates no longer install partial rosters. Complete
  records, padded names, nested lists, metadata and update types are validated
  before replacing live data or notifying the UI/plugins. Counts are bounded by
  the available payload before allocation. Truncated updates do not freshen old
  vitals; explicit empty full updates still clear the corresponding list.
- Fellowship decoding accepts retail's form ending at the departed-members table
  and ACE's appended lock table. Individual updates retain the complete Fellow
  record for full/stats/vitals messages, and rosters remain uncapped at nine.
- Live-vitals subscriptions requested by a panel or plugin while outside the
  world are retained and reapplied on world entry. Relogging no longer leaves a
  subscriber enabled locally but disabled on the server. An offline unsubscribe
  cancels the pending request; no periodic resubscription loop is introduced.
- `/allegiance info <name>` already sent `0x027B`, but its `0x027C` response was
  discarded. The response now prints retail's member, patron and direct-vassal
  report with heritage/gender rank titles and online asterisks. It never replaces
  the requesting player's own allegiance panel data.
- Both allegiance message paths share a complete profile decoder, honoring the
  hierarchy version's optional fields. Incomplete hierarchies leave the current
  panel state intact. Monarchs now retain their own cached/passed-up XP fields,
  previously populated only when self appeared after the root record.

Packet validation is deliberately stricter than the permissive legacy dispatch;
this is resilience work around the retail wire layout, not a claim that retail
rejects every malformed packet in the same way. Live multiplayer validation and
the broader allegiance administration feature set remain outside this pass.

Validation: Win64 Development Editor build passed, including the new
`ACE.Network.SocialParity` automation suite. All seven selected suites passed:
social packets/state, trade packets/state, network transport, desktop social
panels, mouse cursors, plugin recovery and VR menus. Reports are
`TestOutput/Parity-Nov10/social-verified-tests/index.json` and
`TestOutput/Parity-Nov10/social-related-tests/index.json`; build evidence is
`TestOutput/Parity-Nov10/social-verified-build.log`. The social suite covers
truncated snapshots/deltas, retail and ACE fellowship layouts, hierarchy versions
0 through 11, query isolation, and outgoing subscription packets across relogging.
These are local automation results, not packaged-release or live-server testing.

## Trade protocol and acceptance display: 2026-11-10

Compared `Trade::Pack/Register/AddItem`, `ContentProfile::Pack`,
`ClientTradeSystem::AcceptTrade/Handle_Trade__Recv_AcceptTrade/DeclineTrade`, and
`gmSecureTradeUI::RecvNotice_AcceptTrade` in the local retail decompile.

- Acceptance previously serialized only a partial placeholder header: zero stamp,
  zero status, the initiator GUID where retail uses a boolean, and no offer lists.
  It now echoes the registered timestamp, registered status, initiator boolean,
  self/partner acceptance and both ordered ContentProfile lists. Stock ACE ignores
  most of this data, but that is not a substitute for the retail wire contract.
- One accepted-player GUID could not represent simultaneous acceptance. Separate
  self/partner flags now drive desktop and VR trade controls. Local accept/decline
  responds immediately; a partner's acknowledgement or decline does not overwrite
  the player's choice. Retail's zero-source acceptance notice clears self only.
- AddToTrade now honors the server's list position, rejects duplicate/cross-side
  items and invalid sides, and uses retail's list-size bound. A rejected offer
  item is removed from the self list instead of remaining visible. Offer changes
  invalidate acceptance, and truncated registration cannot replace a valid trade.

ACE's `Player_Trade.ClearTradeAcceptance` also clears server offer lists; that
existing compatibility behavior is preserved. This pass does not claim every
trade or fellowship behavior is complete, nor validation on a strict live server.

Validation: Win64 Development Editor build passed. The real-DAT UI suite,
network transport and new trade packet/state suite passed, with the authored
partner-accepted screenshot reviewed. Related vendor and VR-menu regressions
also passed. All 12 selected suites succeeded across
`TestOutput/Parity-Nov10/first-tests/index.json` and
`TestOutput/Parity-Nov10/related-tests/index.json`; build evidence is
`TestOutput/Parity-Nov10/build-fixed.log`. These are local automation results,
not a packaged-release or two-player live-server verification.

## UCM loot and salvage handoff latency: 2026-11-09

The host imposed a fixed three-second throttle after each item salvage and
salvage-bag combination, even when the server immediately consumed the inputs.
UCM now releases that throttle on authoritative inventory removal. A batch
waits for every input to disappear; visual detach, unrelated removals and
UseDone cannot acknowledge it. The next policy decision is scheduled outside
network dispatch, and normal busy, equipment and combat-transition gates remain.
Missing/rejected operations retain their timeout and failed-item protections.

Closing a corpse no longer inherits the generic half-second use delay: the
ordered close request clears the local container synchronously. Lua requests
the next bounded decision after 50 ms. Queuing inventory salvage and removing
obsolete/protected jobs also use that continuation instead of the idle poll.

Reference: local VTank decompile `c7.cs` subscribes to ItemDestroyed and
WorldFilter.ReleaseObject for salvage inputs; `ar.cs` drives SalvageItems.
`d0.cs` still applies a 0.75-second ItemUse/Navigation lock to ordinary pickup.
ACE `Player_Crafting.HandleSalvaging` consumes inputs before creating output
bags, and may omit SalvageOperationsResult when salvage chat is squelched, so
the optimization does not depend on that optional result or chat text.
No jump/animation-breaking or speculative parallel pickup was added.

Validation: Development Editor build and 11 regression suites passed. Native
loopback tests cover immediate acknowledgement, partial batches, visual-only
deletion, unrelated UseDone/removal, busy gates, Stop, and deferred scheduling.
Real Lua tests exercise two complete corpse/pickup/salvage cycles, inventory
salvage, failure recovery, combat priority and scheduling. The 1,001-rule,
20-item workload completed in 41 bounded decisions (80 ms total VM work,
2.9 ms peak in this run). This is CPU work, not live-server looting time.
Quest/mobile shared-source verification also passes. Evidence is under
`TestOutput/LootSalvageLatency/`: ten suites in `final-tests`, with the host
fixture corrected and passing in `scheduler-verified-tests`. Live-server
throughput has not been measured; the confirmed improvement removes client waits.

## Corpse pickup-key report: 2026-11-08

The current pickup command already excludes corpses from inventory pickup
and falls back to Use/open. No production change was made for the reported
"pick up the corpse" failure: it has not been reproduced with the available
information. The exact binding, client build, and affected corpse report are
still needed to distinguish an input override from different object metadata.

`ACE.RetailParity.WorldLootPlacement` now exercises real default and rebound
pickup keypresses through PlayerTick, verifies one Use request and no inventory
transfer, checks repeated presses while busy, and replays the server contents
response. Existing loose-item/backpack pickup and approach cases remain covered.
`ACE.RetailParity.SelectionToolbar` also verifies corpse opening with the loaded
DAT inventory binder and ordinary pickup of a corpse's contents. Both suites
pass in the Editor build; evidence is under `TestOutput/CorpseLootKey/verified-tests`.
This is protocol/fixture coverage, not a reproduction on the reporting server.

## Windows update access errors: 2026-11-08

A v150-to-v151 report showed the legacy PowerShell helper's generic
"Access is denied" dialog. That helper discarded the failing launch stage and
overwrote an already successful installation result if restarting the game
failed. The screenshot alone does not establish which executable was blocked
or why. v151 and later use the standalone native Windows helper; both helpers
have now been hardened for the same failure paths.

Failed installer/game launches retry access/sharing/lock errors for at most
eight seconds, without repeating a successfully launched installer or changing
permissions. Each result retains the stage, executable path, native Windows
error code, installer exit code, and installation success independently of
restart success. A failed restart explicitly tells the user that the update
installed and to reopen their normal shortcut. Verification still precedes
installer execution; nonzero installer exits never restart the game.

`Unreal/Build/VR/Test-UpdateClient.ps1` reproduced the old helper's loss of
installation status with an external executable lock. Both updated helpers
pass isolated fixture tests for desktop/VR window visibility, paths containing
spaces/punctuation, invalid hashes, temporary and persistent file locks,
actual denied-execute ACLs on the installer and game, and nonzero installer
exit. Tests restore their fixture ACLs and confirm the installer never reruns
during restart recovery. The native helper builds with warnings as errors.
Evidence is under `TestOutput/UpdateAccessDenied/`; no real game installation
was replaced and the reporting user's machine was not reproduced.

## UCM combine retries and spellbook filter follow-up: 2026-11-08

Rejected or unconfirmed stack merges and salvage-bag combinations previously
restarted after a 30-second activity cooldown, repeatedly waiting another
15 seconds and interrupting route navigation. UCM now remembers the attempted
items and skips them until their relevant item state changes or UCM restarts.
Unrelated inventory/appraisal revisions do not re-enable a rejected combination;
removed objects are pruned, and other eligible items can still combine.
Salvage bags cannot enter ordinary stack merging, and bag combining excludes
unsupported materials, tinkered bags, and inscribed bags. Failed post-loot
salvage also remains excluded from the existing-inventory salvage scanner.

`ACE.Plugins.CombineFailureRecovery` reproduced the retry loop before the
change. It now covers server errors, host rejection, timeouts, incomplete
consumption, continued navigation across cooldowns, unrelated inventory
changes, other valid combinations, and protected/unsupported bags.
`ACE.Plugins.StackCombining` also verifies that changed stacks can be retried.

The reported spellbook behavior could not be reproduced in current source:
the v151 filter guard already matches retail
`gmSpellbookUI::RecvNotice_SpellAdded` (0x0048B8D0): reveal a learned spell only
when its school and level both pass the existing filters. Learning does not
change those filters. `ACE.RetailParity.UIInteractions` now exercises pointer
clicks on all 13 filter controls, their saved PlayerModule value, the all-off
case, school-only/level-only cases, and a matching school/level followed by a
real spell-learning packet. No production spellbook behavior changed here.

The Development Editor build and six targeted suites passed:
`ACE.Plugins.CombineFailureRecovery`, `ACE.Plugins.StackCombining`,
`ACE.Plugins.InventorySalvage`, `ACE.Plugins.SchedulingParity`,
`ACE.RetailParity.UIInteractions`, and `ACE.Network.SpellDataRevision`.
Reports are under `TestOutput/UCMCombineSpellbook/`. These are local automated
tests using the real UCM script and retail DAT UI, not a live-server reproduction.

## Pet cage NPC collision: 2026-11-08

Shady's Cage Door (WCID 72984, setup `02000281`, motion `09000016`)
arrives as a creature with physics state `00010408`, without the Door
description flag. Its retail setup has no spheres or cylinders; its parts
provide 48 PhysicsBSP triangles. The creature appearance path discarded those
sections, then used the large selection capsule as movement collision. A
two-sphere player sweep from the reported location in `586002E6` reproduced
the obstruction against `CollisionProxy` immediately, before reaching the cage.

Retail `CPhysicsObj::FindObjCollisions` (0x0050F050) selects the BSP path from
physics state independently of creature classification, and
`CPartArray::CacheHasPhysicsBSP` (0x00518110) inspects the actual parts.
The client now retains authored physics sections for BSP-backed NPCs, using
the server physics bit or the setup metadata before that description arrives.
Actual model collision takes precedence over fallback shapes. Known setups
with no fallback body no longer manufacture a solid selection capsule while
their mesh loads. The change is based on physics data, not names or WCIDs.
Ethereal state, attachment exclusions, selection, and player PK collision
rules remain separate from geometry selection.

Regression coverage is `ACE.Collision.CageDoors`: the reported approach and
door contact, neighboring cells `586002E6/E7` with furniture, selection,
deferred appearance, state/visibility refresh, ethereal transitions, and
rotated/scaled NPC variants without the server BSP bit. This is local DAT
and collision validation; a live quest hand-in has not been exercised.
The Development Editor build and 11 suites passed: the eight `ACE.Collision`
suites, `ACE.Rendering.VisualMeshCollision`, `ACE.RetailParity.MovementReview`,
and `ACE.RetailParity.RuntimeActors`. Results are under
`TestOutput/PetCageCollision/` (`before`, `after`, and `final` reports).
The full-room fixture clears a verified lower-sphere floor contact by 0.5 cm
without shrinking horizontal collision or excluding any room component.
Quest and Android source synchronization passes; device runtime testing was
not performed for this change.

## Spell inspection with foci: 2026-11-07

Retail `SpellExamineUI::ExamineSpell` obtains its ingredients through
`ClientMagicSystem::GetAppropriateSpellFormula` (0x00567D50). That method uses
the matching school's infused augmentation or
`ACCWeenieObject::MagicPackIsOwned` (0x0058CB80), which searches the player's
container-slot list, not the ordinary inventory grid or nested bags. Spell
inspection now uses those same side slots; the previous grid-only lookup
excluded real foci. The open inspection's existing refresh path picks up
containment, removal and augmentation changes without reopening the panel.

`CSpellBase::InqScarabOnlyFormula` retains scarabs and chorizite, then uses
`SpellFormula::FindMostPowerfulPowerComponent` to choose the prismatic taper
count. This is distinct from the first component used for spell-icon level;
the formula helper now preserves that distinction for multi-scarab spells.

The reported intermittent need for old components on Levistras is not yet
reproduced. The player saw a yellow warning (exact text unknown), retail
worked, and several relogs eventually restored casting in Unreal.
Retail and this client send spell/target IDs through CastTargetedSpell or
CastUntargetedSpell, without a component-formula flag. ACE's
`Player.HasComponentsForSpell` calls `SpellFormula.GetPlayerFormula`, which
selects the formula from server-owned inventory/augmentation state on each
check. Correcting the inspection does not establish a fix for a server-side
component rejection. The exact rejection text and a log captured while it
happens are still needed to identify that remaining report.

Regression coverage: `ACE.RetailParity.SpellComponents` exercises real DAT
spells in all five schools, open-panel updates from containment/removal
receipts, wrong-school and nested foci, infused augmentations, all three
player/object/description login orderings, and actual
loopback cast packets with scarabs/prismatic tapers but no old components.
`SpellFormulaLevels` covers all ten power-component taper counts, chorizite
and mixed-scarab formulas. The Development Editor build and those two suites,
`ACE.Network.SpellDataRevision` and `ACE.RetailParity.UIInteractions` passed.
Results are recorded under `TestOutput/SpellComponents/`. Release v152 also
passes these suites in the packaged Windows and native Linux clients. Its
18 editor, 19 Windows and 19 Linux suite runs are recorded under
`TestOutput/Release-v152/`; the release artifact directory includes the
regression summary, APK verification and Windows upgrade-preservation results.
Quest and Android source synchronization and package checks pass; physical
headset/phone acceptance is not claimed by those automated checks.

## Linux display resolutions: 2026-11-05

UE 5.8's Vulkan `RHIGetAvailableResolutions` returns false without enumerating
modes. The video options therefore previously offered only the current saved
size, commonly the shared 1920x1080 default. Linux now queries SDL3 for the
display containing the game window and uses desktop pixel dimensions rather
than potentially scaled logical bounds. Native modes and the current custom
size are retained; common window/backbuffer sizes fitting the desktop cover
compositors that expose only one mode. Duplicate refresh-rate entries collapse
to one resolution, with no 1080p ceiling.

Opening the picker refreshes modes after a monitor move or hotplug while
preserving unapplied selections. Borderless still fills the desktop, and
changes retain the existing 15-second Keep/Revert confirmation. Linux platform
defaults now request desktop resolution on a fresh install; saved choices and
command-line overrides take precedence.

Validation: Win64 editor and native Linux package builds passed. Resolution
options and both retail UI suites passed on Windows and Linux; the Linux
DisplayRecovery fixture passed apply/confirm/disk-save/reload for 1280x800,
2560x1440, 3440x1440, 3840x2160 and a custom 1712x964 size, plus timeout/revert.
Native SDL enumeration passed under WSLg X11 (28 advertised modes, 22 unique
options) and Wayland (22 modes/options), both at a 2560x1440 desktop. Simulated
monitor lists cover 800p through 8K, ultrawide and empty/single-mode drivers.
Reports are in `TestOutput/Nov5LinuxResolutions/`; `linux-recovery-tests` is the
final persistence run. The initial persistence test read an INI cache key as
a filename; it now reads the generated settings path. Physical Linux monitor
hotplug and visible 4K/fullscreen rendering remain unverified here.

## Crowded-event dispatch, Recklessness and Linux login: 2026-11-05

Network dispatch retains the normal 0.5 ms allowance, but ready work waiting
at least 100 ms enables an 8 ms total receive/dispatch catch-up allowance.
Socket backlog can increase receive time to 2 ms per socket. The existing
128-work-unit cap remains; an atomic callback finishes before yielding, so one
expensive callback can exceed the time allowance. Idle queues return to the
normal allowance. `ace.Network.CatchUpBudgetMs=0` disables catch-up for comparison.
`/acueperf` now distinguishes socket backlog, undispatched packets, ordered
events, dispatchable age and events held on missing stamps/objects. The latter
do not cause catch-up work when no event can actually progress.

Retail `SharedNet::ProcessOptionalHeader` creates a `CEchoResponseHeader` for
an EchoRequest; `ClientNet` calls that base implementation. Echo handling is
therefore retained. A loopback traffic fixture compares sustained ordered
events under fixed and adaptive budgets, including final drain, ACK progress,
duplicates and ordering. This reproduces budget starvation, but does not prove
the cause of the reported live event delay without a capture from that event.

The Recklessness DAT field is a fixed range marker, not a charge fill. Exclude
`basic_recklessness_fill` and `adv_recklessness_fill` from meter-fill clipping.
Retail `UIElement_Meter` animates child 2; `gmCombatUI` gates Recklessness on
trained/specialized skill 50. End-of-retail DAT geometry and ACE
`Player.GetRecklessnessMod` use 10–90%, despite older 20–80% descriptions.

Linux Accounts now offers an unchecked-by-default password persistence option.
Opt-in writes unencrypted credentials to a uniquely named temporary file
created with owner-only permissions before replacement. Opt-out removes saved
passwords while retaining session credentials. Windows DPAPI and Android
Keystore behavior are unchanged. Persistence and launcher fixtures cover
opt-in, reopening, opt-out, all accounts and Linux file permissions.

Validation: Win64 Development editor and native Linux package builds passed.
Eight Windows suites passed (seven in `editor-final-tests`, UIScreens in its
final `ui-final-tests` rerun); all nine native Linux suites passed. Linux display
fixtures retained existing console-priority/viewport warnings. Reports and
rendered Accounts/Recklessness images are in
`TestOutput/Nov5CrowdLoginRecklessness/`. The 1,440-event Windows UDP fixture
peaked at 1,140 queued work items with catch-up disabled and 139 with it enabled;
Linux measured 1,142 and 139. All events drained in order without duplicates.
These are controlled loopback results, not a live crowded-server validation.

## Trade, input and turn-animation follow-up: 2026-11-04

ACE `Player_Trade.FinalizeTrade` removes outgoing items with ObjectDelete,
then sends TradeComplete and ResetTrade. The client previously retained these
objects as owned inventory, interpreting deletion only as a 3D removal. Offered
items now leave inventory on the authoritative delete/completion. Completion
also covers event/object queue ordering, and does not delete an item twice if
ACE has recreated it for its original owner after failed delivery. Cancellation
and items not offered are preserved. Trade offers use the retail item-foreground
mark; the inventory cache includes offer IDs so add/reset updates it immediately.

Spell-bar hover names use the actual visible spell icon tooltip. All twelve
numbered spell slots are exposed in keyboard configuration and retail keymap
round trips (default 1-9, 0, -, =). A hidden or detached text editor no longer
suppresses Give or Action Deck hotkeys merely because Slate retained its focus.
Visible typing continues to suppress gameplay hotkeys.

Stationary turn animation integrates elapsed turn speed instead of multiplying
total clip time by the most recent mouse delta. Short gaps between slow mouse
events retain footwork for at most 100 ms; physical heading and network movement
are unchanged. Use/Give clears the previous mouse-turn release bookkeeping so a
late neutral MoveToState cannot cancel the new server interaction chain. Retail
`ItemHolder::UseObject` and ACE `GameActionMoveToState` were the comparison paths.

The provided logs identify Corpse of Daimyo Nobanuga, WCID 600938, setup
02001B99. They show repeated Use requests followed by approach cancellation and
no UseDone. The stale mouse-release path is reproduced in a fixture using that
setup; the logs alone do not prove it caused this particular failed reward.
Live custom-server reward delivery and the rare Action Deck incident still need
verification. No synthetic loot receipt or automatic repeated Use was added.

Win64 Development editor build passed. KeyboardBindings, NetworkAnimationParity,
WorldEntry and ActionDeck passed in the initial run; final InteractionRecovery
and UIScreens reruns passed with zero errors/warnings, including returned-item
ordering and cached inventory mark removal. Reports and rendered UI fixtures are
under `TestOutput/Nov4Reports/` (final changed suites under `final/`).

## Backpack drag pickup and return transfers: 2026-11-01

Compared local retail `UIElement_SmartBoxWrapper` world drag handling,
`gmExternalContainerUI::PostInit`, `UIElement_ItemList::ItemList_BeginDrag` /
`PlaceItem`, and `ItemHolder::WillItemFitInContainer`. The previous chest-pack
fix covered installed packs but skipped the external root icon. Desktop world
presses also selected/used items without ever starting an inventory drag.

Portable world items now enter the captured inventory gesture in peace mode.
Click/release selects, double-click uses (opens a backpack), and movement starts
the drag; a pending item gesture takes priority over camera orbit. Capture loss
cancels it, and releasing an unowned ground item onto empty ground does not send
a DropItem request. Fixed containers and combat-mode world clicks keep their
existing selection/use/camera behavior. The open ground backpack's root icon
also supports dragging, while repeated clicks continue browsing its contents.

Container capacity and slot lists now drive pack drop feedback and submission.
Chests can receive packs, occupied chest pack slots permit insertion/reordering,
and empty chest pack slots accept packs. Feedback uses the icon arrays whose
indices match their GUID arrays; external pack backgrounds omit the root, while
external item backgrounds reserve index zero for it. Ordinary item grids reject
packs as in retail. Self/descendant containment, full containers, ordinary bags
without nested-pack slots, and traded items are rejected. Player records that
omit capacity retain the inventory UI's seven default side slots. Ownership and
contents change only after authoritative server updates.

Regression coverage includes actual world picking and mouse capture, root and
nested pack drags, repeated opening clicks, inspection, pickup-key destinations,
empty/occupied pack slots, chest return transfers, exact feedback textures,
cancellation, capacity/cycle checks, and contents retention at 1x/1.5x UI scale
with a window offset. The Windows Development Editor build and all nine suites
passed: DesktopHighlights, TouchAndMenus, InteractionRecovery,
ClosedContainerLifetime, InventoryOrder, SelectionToolbar, UIInteractions,
UIScreens, and RigAndMenus. Evidence: `TestOutput/BackpackParity-20261101/`
(final automation under `final/`).
This comparison uses retail source and automated rendered input/protocol
fixtures; live retail/server testing is still separate.

## Removing packs from chests: 2026-10-29

Retail `gmExternalContainerUI::PostInit` uses `UIElement_ItemList` for both the
external item grid and its container list. `ItemList_BeginDrag` handles either
list; `ListenToElementMessage` opens a nested pack on click and examines it on
right-click, without interpreting repeated container-list clicks as pickup.
`gmExternalContainerUI::RecvNotice_ServerSaysMoveItem` (`004CBB30`) returns to
the root when the server moves the open nested pack out of the chest.

Our external pack icons handled opening only; mouse-down never armed their
inventory drag. They now use the existing authoritative PutItemInContainer
path, including side-slot insertion, while ordinary clicks still browse the
pack. Right-click examines it. After confirmed removal the external view returns
to the chest, with the pack's own contents retained. Touch/VR loot also exposes
side-slot packs, with separate open and take actions; previously those packs
were omitted by the ordinary-item query.

Validation: Windows Development Editor build passed. UIScreens, InventoryOrder,
ClosedContainerLifetime, VR.RigAndMenus and Mobile.TouchAndMenus passed. The
rendered input fixtures cover repeated clicks, examination, drag cancellation,
occupied/empty destination pack slots at 1x/1.5x DPI with a desktop offset,
server-confirmed removal, and retained nested contents. The broader UI suite's
old hardcoded chat-filter group count was replaced with the current catalog
size so it still checks the intended windows after the new chat filters.
Evidence: `TestOutput/ChestPackRemoval/` (builds) and
`TestOutput/CombatChatParity/ChestPackRemoval{,-final}/` (automation). This is
checked against local retail source and encoded client/server event fixtures,
not a live-server retail recording.

## Retired skill records and DAT stat definitions: 2026-10-26

The reported skill page mixes modern Light Weapons with iconless Axe, Dagger,
Mace, Spear, Staff, Sword and Unarmed Combat entries at 5,231. Retail
`gmSkillUI::RebuildSkillList` (`0049C3A0`) enumerates the DAT skill table and then
looks up the player's values. Our shared player-stat view instead kept every
server skill record, with historical fallback names even for IDs absent from
the DAT. ACE deliberately adds retired definitions in
`ACE.DatLoader.FileTypes.SkillTable.AddRetiredSkills`; their presence in a server
packet is not evidence of an outdated client DAT.

The stat resolver now excludes server records absent from the successfully
loaded, complete DAT skill catalog before publishing values to desktop, VR and
Lua. It applies on login and subsequent skill updates. There is no historical-ID
blacklist: definitions explicitly present in a custom DAT remain usable.
DAT replacement now clears both skill definitions and secondary-attribute
formulas, which previously survived reloads. DAT-open logs include the embedded
iteration so reports can identify the actual selected files.

The baseline reproduction supplied 38 modern and 17 retired/internal/unknown
records and exposed all 55; live updates also restored the invalid entries.
The local fixture files report end-of-retail iterations matching ACE's
`DatManager`: portal 2072, cell 982, English language 994 and high-res 497.
Their skill table contains 38 definitions with names and icons. These checks
verify the local fixture set; they cannot identify another user's DAT files
from a screenshot alone.

Validation: Windows Development Editor build and eight suites passed:
CharacterStats, AugmentationStats, Augmentations, CharacterCreationData,
MissingDatLogin, UIScreens, VR.RigAndMenus and Plugins.PublicAPI. Tests cover
all 38 definitions, login/live obsolete-skill filtering, custom catalog
membership, DAT replacement, current attribute/vital formulas and permanent
augmentation handling. The rendered skill panel was inspected and has no row
for the injected obsolete Axe record. Shared Quest sources match. Evidence:
`TestOutput/SkillParity-2026-10-26/{baseline,after}/` and
`after-user/Saved/Automation/RetailParity/GameplayCookingBuffed.png` under that
parent folder. CharacterStats and MissingDatLogin also passed in `final/` after
the final DAT iteration-logging adjustment. The injected stale-formula baseline is a regression fixture,
not evidence that the reporting player's actual attributes were incorrect.

## Network action rates and interface lifecycle: 2026-10-26

This pass compared position/teleport sequencing, interpreted movement and action
playback with the local retail sources (`SmartBox::HandleReceivedPosition`,
`CPhysics::SetObjectMovement`, `CPhysicsObj::MoveOrTeleport`,
`InterpolationManager::InterpolateTo`, `CMotionInterp::move_to_interpreted_state`
at `005289C0`, and `CMotionTable::GetObjectSequence` at `00522860`). Retail passes
an action's speed to its animation sequence. Our receive, animation queue and
death-root paths imposed a minimum rate of 0.05, causing a paused or slower
server action to keep advancing. They now preserve nonnegative rates, including
zero. This does not add reverse action playback or retune position smoothing.
Regression coverage checks slow and paused interpreted motions, pause/resume
without restarting the pose, queued paused gestures, and paused death-root
displacement. The new rate assertions failed against the previous implementation.

The native VR spellbook had two separate lifecycle faults: reopening a page
reused a text entry that remained collapsed, and learning a spell switched to
the desktop book while the native book still appeared visible. That switch
redirected controller clicks away from the displayed filters. Reused entries
are now shown, and learning a spell refreshes the active native book in place.
A matching learned spell is selected and paged into view without clearing the
user's filters, following `gmSpellbookUI::RecvNotice_SpellAdded`. The three
previously failing VR filter/tile assertions now pass, with additional checks
for the visible search entry, retained surface and working filter controls.

The FPS overlay is also limited to the InWorld session state on desktop and VR.
Leaving gameplay hides it and clears its timing sample while preserving the
user's enabled setting and placement for the next character session.

Validation: Windows Development Editor build succeeded; all 29 selected suites
passed in an offscreen D3D12 run using retail DAT files. Coverage includes
network transport and pump budgeting, custom object replication, position and
movement replay, turning, emotes, combat/archery animation, support and runtime
actors, desktop screens/interactions, character management/button masks,
selection toolbar, text entries/tooltips, housing, FPS lifecycle, and VR menus
and interior networking. Native spellbook and fellowship screenshots were
visually inspected. Shared Quest source was synchronized and checked. Evidence:
`TestOutput/ParityPass-2026-10-26/{baseline,after,complete}/`,
`build-final.log` in that parent folder, and
`after-user/Saved/Automation/VR/{GameplaySpellbook,GameplayFellowship}.png`.
These are offline regression and rendered UI checks, not a live side-by-side
multiplayer comparison or a claim of complete retail parity.

## Foundry, blocked terrain overlays, and resource retention: 2026-10-25

The bundled optional **Dereth Foundry** Lua companion follows AceCreator's
content, placement, landblock, quest/recipe, generator/encounter and file workflows
using this fork's existing ACE developer/admin commands. It is disabled by
default and uses the shared plugin controls and a gold anvil / teal cube icon.
No server privileges are granted by enabling it. `admin_content` allows bounded,
selection-checked native command dispatch and local JSON/SQL browsing/editing;
server permissions and the server content directory remain authoritative.
See `Docs/Plugins/Foundry.md` and the public API contract for platform limits.

Dereth Lens previously colored wadeable coastal water and warned on slopes that
were still walkable. It now colors only triangles below the retail walking
normal threshold (`0.6641741`) and EntirelyWater landblocks. This matches
`CLandCell::find_env_collisions`' block-water-type 2 rejection; a coastal cell's
water depth by itself does not imply blocked movement. Mesh sections are retained
while standing still and rebuilt only after relevant settings, loaded geometry,
indoor/outdoor state or nearby position change. This is terrain guidance; it does
not claim to classify dynamic objects, bridges or server travel restrictions.

The supplied Yaraq sample has 126.20 ms in effects with 75 creatures, 74 emitters
and only 43 active particles; GPU time is 2.2 ms and plugin/Lua times are zero.
A later sample recovers to 0.22 ms effects. That identifies a CPU effects hitch,
not evidence that Lua or GPU rendering caused this particular stall. The source
review found unbounded strong material maps for shells, outdoor scenery, cells
and particles. Those maps now share a bounded warm-reference budget with textured
materials. Weak identities preserve visible-material reuse and daylight updates,
and unused identities disappear after collection. DAT reload also invalidates
the previously retained outdoor material map. This fixes retention independent
of whether it caused the reported hitch.

Loading-time surface classification now reads cached alpha/color/lighting flags
without copying complete decoded pixel arrays. Flags remain usable after pixel
payload eviction. Texture creation and authored visual semantics are unchanged.
`/acueperf` and slow-frame logs now split script hooks, particle simulation,
light updates, render uploads and cache maintenance, and include DAT resource
counts. These counters use fixed storage rather than retaining a growing history.

Validation: Windows editor build; 11 focused suites covering Foundry, public API,
companions/runtime, world plates/terrain, texture budgets, material lifetimes,
crowd particle updates, particle batches, portal materials and scenery lighting.
The cache fixture creates 7,200 distinct material instances over 12 area-like
cycles, asserts the shared cap after every trim, verifies visible material reuse
and daylight updates, and checks dead weak records after garbage collection.
Foundry runs in the actual bounded Lua VM; its seven tabs were rendered and
inspected. Tests validate commands, confirmation/cancellation, capability checks,
file traversal rejection and local file listing. No live server database imports,
object deletion or content edits were executed as part of these tests. Evidence:
`TestOutput/Foundry/after2.log`, `TestOutput/Foundry/final.log`, and
`TestOutput/Foundry/User/Saved/FoundryReview/`.

The controlled offline outdoor scene used 32 synthetic creatures at 2556x1354,
45 seconds warmup and 30 seconds measurement. Mean frame times were 3.781 ms
before and 4.373 / 4.955 ms in two after runs; therefore this is **not evidence
of an overall FPS improvement**. No measured frame exceeded 50 ms, so the user's
151.7 ms frame / 126.20 ms effects spike was not reproduced. The first after trace
kept effects near 0.11 ms per frame (similar to before), and 17 cache-maintenance
calls totaled 3.79 ms with a 0.269 ms maximum. Steady-state resource counts were
stable and texture allocations remained 1574.41 MiB in this small scene. The
slower aggregate timings span UI, player and renderer work; neither their cause
nor resolution of the live hitch is established. Evidence and traces are under
`TestOutput/FoundryPerformance/{before,after,repeat}/`. Capture the new effect
subcategories on a live recurrence before attributing that report to a specific
particle operation. Material lifetime/terrain regression results remain valid
independently of these aggregate measurements.

## Local combat movement and plugin chat: 2026-10-29

Retail `CMotionTable::GetObjectSequence` removes cyclic animation tails while
retaining unfinished links/actions; `CSequence` applies their position frames
before reaching the next walk/run cycle. `add_motion` supplies cycle physics,
while `combine_motion` supplies independent sidestep/turn modifiers. The local
Unreal controller previously predicted full forward/backward locomotion during
casting gestures and scarab windups, even though those links were still playing.

Desktop/mobile forward prediction now waits for the active combat link. This
also covers missile aim/reload entries and their return to Ready. Raw forward
intent still reaches the server immediately; sidestep/turn remain independent.
A forward command reapplied during server-control handoff releases the eventual
held cast tail even if the key was already down. Existing melee/stance-change
translation locks and airborne movement rules remain in effect.

The Editor build and nine suites passed: WalkingStep, RunSpeed, PvPCombat,
ArcheryMotion, AttackHeightMotion, UIInteractions, ChatInputAndCommands,
PublicAPI and LootNotifications. Added coverage runs actual DAT magic/bow/
crossbow/atlatl/thrown sequences with walk/run and forward/backward input at
30/144 Hz, checks movement resumes, and replays raw movement packets when casts
arrive before or after held input. These are local DAT/controller/protocol
tests; a live-server reproduction has not been recorded. Evidence is under
`TestOutput/CombatChatParity/`.

Plugin local notices now use a separate chat routing category, retain history,
and preserve their existing colors. Saved routes migrate once from the previous
System/Broadcast categories. Tests cover per-window opt-out persistence, normal
server messages, and Lua notices reaching the routed HUD. UCM exposes its existing
item-match switch on Configuration as well as Loot, independently of confirmed
pickup announcements, with regression coverage for discarding queued matches.

## UCM custom-profile memory and hunt history: 2026-10-25

The reported log repeatedly stops with Lua `not enough memory`, including
immediately after restart. This is the plugin heap quota, not evidence of
system-wide memory exhaustion. A combined native snapshot regression with
4,000 spells, 512 assessed items and 1,637 eight-condition custom loot rules
reproduced the failure on its first callback under the former 8 MiB cap.
The previous separate inventory and loot-profile tests both passed but ran
against the cap. Lua now has a hard 32 MiB limit, including host inputs; script
work limits remain unchanged. Allocation errors record the failing phase,
heap usage and snapshot/profile counts without dumping character data.

UCM also retained expired appraisal/corpse/debuff cooldowns and departed-item
attempt records for the entire run. It now periodically drops expired timers
and retries for objects no longer represented in the current inventory,
world, open container, fellowship or buff request. Live-object retry counts
remain intact. Successful equipment state no longer inserts zero-valued
entries for every item seen. The hunt regression changes 6,000 inventory
GUIDs across 600 combat/assessment/corpse cycles and checks bounded retained
history. VM input/callback quota failures remain contained and unwind the
Lua stack. These are automated workloads, not a live replay of Obi's custom
profile, which was not included with the report.

## Mana donor appraisal and local stopping: 2026-10-25

Mana-stone pickup can trigger a fresh CreateObject, which clears the plugin's
appraisal through ObserveTargetCreated/ClearPluginTarget. The policy treated
that temporarily unassessed pickup as unsuitable, removed its reservation,
and requested more junk for the still-empty stone. ResourceMaintenance now
replays this ordering (including another eligible item on the open corpse);
the pre-change test reproduced both the extra pickup and failure to fill.

The exact confirmed pickup remains reserved while its appraisal is missing.
UCM requests a fresh assessment with the existing bounded appraisal retry
policy, rechecks eligibility, then uses the stone on that item. Pending
assessment does not block combat/recovery or authorize consumption. Known
protected/changed items remain excluded, and arbitrary existing possessions
are never enlisted as donors. Completed fills still require observed stone
mana, not merely UseDone.

Local Stop input and chat focus previously disabled walking transitions and
cleared unfinished links. Retail CommandInterpreter::StopCompletely calls
CPhysicsObj::StopCompletely; CMotionTable::StopObjectCompletely and
StopSequenceMotion return through GetObjectSequence to the default substate.
CSequence::remove_cyclic_anims removes the cyclic tail, retaining pending
links. Local Stop/chat now retain the same collision-resolved step as key-up.
Raw input stops immediately while remaining authored movement keeps autonomous
position reporting active. No extra coast or invented full cycle is added.

The Windows Development Editor build and six suites passed: ResourceMaintenance,
Recovery, RouteAndTellRequests, WalkingStep, RunSpeed and EmoteTransitions.
WalkingStep adds explicit Stop during walk/run at 30/90/144 Hz, retaining its
key-release, held walking, wall collision and interruption coverage. Artifacts:
`TestOutput/ManaAndSteps/{before,after}`. These are offline policy, protocol and
DAT/controller regressions; live server reproduction remains unrecorded.

## Golem activation and resting stance exits: 2026-10-25

Bronze Statue of a Golem (WCID 19276, setup `020007CA`, motion table
`090000EF`, scale 1.5) uses a NonCombat/Sleeping heartbeat and wakes into
HandCombat. The stock definition is in ACE-World-16PY's
`Creature/Statue/19276 Bronze Statue of a Golem.sql`. Retail
`CMotionTable::GetObjectSequence` (00522860, style branch) first appends the
old substate-to-default pre-link, then the old-default-to-new-style link,
optionally routing through the table's default style, before the new cycle.

Our generic direct-link-first lookup selected the style-wide combat link and
omitted the sleeping exit. The DAT regression reproduced one clip instead of
the two authored clips (3.733 seconds total). Style transitions now follow
retail's pre-link order. Newly created resting bodies retain their initial
style, and a combat update during a lay-down entry queues behind that entry.
This is shared motion-table handling, with no golem or WCID-specific runtime
branch and no changes to server combat timing.

`EmoteTransitions` compares every displayed golem part against the authored
clips at 30/90 Hz, for spawn-asleep and live heartbeat paths, repeated Ready
updates, and combat arriving during lay-down. Validation artifacts are under
`TestOutput/GolemActivation/`. The Development Editor build and all six final
suites passed: EmoteTransitions, NetworkAnimationParity, ArcheryMotion,
AttackHeightMotion, AvatarMotion and WalkingStep. Quest source synchronization
and CheckOnly passed with zero differences. These are offline DAT/Unreal actor regressions;
live server activation has not been recorded side by side with retail.

## UCM firing corridors and live target positions: 2026-10-25

UCM's snapshot and final cast guard aimed a zero-width ray at the last received
target position. A creature can walk behind a corner between F748 updates while
that position remains in the open. The regression reproduced this: the snapshot
reported clear sight and the host sent CastTargetedSpell for the occluded body.

The shared query now resolves the live entity position, consistent with retail
`TargetManager::SendVoyeurUpdate` reading current physics frames. Combat endpoints
use scaled DAT body heights at two thirds, matching ACE's targeted bolt aim in
`WorldObject_Magic.CalculatePreOffset` / `CalculateProjectileVelocity`. Snapshot
eligibility and the final action guard use the same query; a target moving after
policy selection is checked again before a cast can start.

Combat also requires a 15 cm clearance corridor, reduced to one third of either
body's height for small creatures so its supporting floor does not block every
shot. This is a conservative automation safety margin, not a reconstruction of
server-only projectile templates or every curved trajectory. Sphere sweeps test
physical scenery; segment/triangle distance tests retain the same clearance for
loaded rooms whose collision is not cooked, including hidden and nonuniformly
scaled meshes. Corpses, route visibility and friendly recovery retain zero-width
sight tests. Blocked creatures drop out of UCM's existing target selection and
become eligible again when the live path clears, without a timed blacklist or
starting an awaiting-confirmation gate. Manual casting is unchanged.

Validation: Windows Development Editor build and seven suites passed:
RouteAndTellRequests, Decisions, TargetHandoff, LongCombatRouteRejoin, BuffParity,
LootCombatPriority and UCMIdleWorkload. New cases cover stale moving-target
positions, obstruction after selection, repeated rejected intents, grazing
corners with cooked/uncooked collision, scaled hidden rooms, low floor clearance,
and a clear path sending exactly one cast. The existing shield-bane cases still
pass, so stale inventory positions do not block equipment buffs. The fixture
reproduces the reported failure class, not the exact live Sea Temple screenshot.
Reports: `TestOutput/UCMCombatSight/{before,after}`.

## In-game tooltip cursor clearance: 2026-10-25

Retail `UIElementManager::StartTooltip` places the tooltip 32 pixels right and
below the mouse, then clamps to the display bounds. The existing cursor-aware
correction covered native Slate tooltip windows and world hover labels, but
inventory/plugin tooltips in the game viewport use `STooltipPresenter`. That
engine presenter uses a fixed 12-by-12 cursor offset and ignores popup force
fields, bypassing the correction entirely.

The shared tooltip now adjusts its content during the presenter's paint using
the actual viewport bounds, layout scale, and scaled software cursor bounds.
It retains the retail offset when sufficient and moves above or beside the
cursor when edge clamping would otherwise overlap. Native popup and world hover
placement retain their existing paths.

`TooltipCursorClearance` now renders the actual in-game presenter for inventory
and plugin tooltips, including an embedded viewport offset, UI scales of
0.75/1/1.5/2, cursor scales of 0.5/1/1.25/2/3, and all corners plus the center.
The 200 combinations compare rendered tooltip pixels against the DAT cursor
artwork dimensions independently of the placement helper. The test reproduced
overlap before the fix and passed afterward, with no tooltip text/frame pixels
under the cursor or outside the viewport. Existing native-popup and placement
checks also pass. Evidence: `TestOutput/TooltipViewport/{before,after}`.
The Windows Development Editor build passed. This verifies retail source and
rendered Unreal behavior; it is not a live retail-client screenshot comparison.

## Toolbar icon depth: 2026-10-23

The Allegiance/Social and Options icon rims were covered by their end trim.
Retail `UIRegion::AddChild` (006A0760) orders children from larger to smaller
depth, and `DrawChildren` (0069FCA0) paints that list from the head. The DAT
toolbar places trim at depth 2 behind buttons at depth 1. Our import preserved
the depth without converting to the canvas's higher-is-front convention.

The layout importer now negates signed retail depths, including unsigned JSON
encodings such as tooltip depth -1. Runtime window promotion and overlays keep
their existing higher-is-front ordering. The fix applies to authored layouts
without icon-specific position, size, texture, or depth overrides.

`SelectionToolbar` compares rendered normal/selected icon rims with and without
the trim at 100%, 150%, and 200% UI scale, checks edge hit targets and all six
panel-button sibling orders, and verifies signed tooltip depth conversion.

Validation: Windows editor build and Quest shared-source CheckOnly passed.
Seven UI suites passed: SelectionToolbar, UIScreens, UIInteractions, Housing,
CharacterButtonMasks, CharacterManagement, and CharacterScreenRendering.
Reports are in `Unreal/Saved/ToolbarLayerReview/{tests,retest,final}`; the first
run exposed two invalid fixture assumptions, corrected before successful reruns
(both combat panels forced visible, and a viewport below 600 logical pixels).
Final toolbar screenshots were inspected at native and enlarged UI scales.

## Apostate elevated selection and quest completion: 2026-10-20

Compared retail `Render::GfxObjUnderSelectionRay`, `GetMouseSelectionObjectID`
and `Position::cylinder_distance`: selection is a 3D drawing-polygon/sphere ray;
interaction reach separately includes vertical cylinder separation. A high
camera must not prevent a visible hit, and a high player must not bypass reach.

`DesktopHighlights` now drops an attuned Aetheric Resonator onto the stock
deposit in its real 0x7F020107 room with surrounding DAT geometry loaded. Eight
views cover four directions at 1.5/3-metre camera heights. Every view picks the
deposit and emits one GiveObjectRequest, never DropItem. It exports the actual
16-byte action to `Saved/Automation/ApostateGiveObject.bin` for server replay.
The existing gateway test also passes from the reported 12-metre-high ledge.

`ApostateQuestIntegrationTests` replays that client action through ACE's normal
network handler with stock 40808/40809/40807 templates and DAT setup dimensions.
Players 0/2 metres above the deposit are in reach; a player 12 metres above it
retains the resonator and receives no quest credit until the simulated approach
arrives. The original pending Give then consumes the resonator and stamps
NexusCrawlResonatorTurnIn0509. Using Ned completes NexusCrawlCompleted0509,
clears the intermediate flags, awards XP, 18,000 luminance and the Nexus Crawler
title. Repeated use grants no duplicate reward. All three cases pass.

This is a functional hand-in/reward integration test, starting with the quest
already accepted. It does not drive combat or navigation through the whole
dungeon, and does not reproduce the reported live invisible object. The fixture
captures persistence requests in an inert queue: no shard writes, sockets or
persistent GUID allocation. No runtime bounds/range exception was needed.

Validation: editor build, `DesktopHighlights`, `CustomWorldPresentation`, and
all three server cases passed. Reports: `Unreal/Saved/ApostateHandIn-client-tests`
and `Unreal/Saved/ApostateHandIn-server-tests`. The level-200 fixture earned
297,235,840 XP and 18,000 luminance in each case. Shared Quest source was synced.

## Chat expand/restore and text margins: 2026-10-20

Compared `gmMainChatUI::HandleMaximizeButton`, `gmFloatyMainChatUI`,
`gmFloatyChatUI`, and `ChatInterface` with the resolved retail main/extra chat
layouts (2100006F and 2100005B). The main chat control now explicitly selects
retail state 10000048/10000047 and textures 06005E65/06005E64. It adds half the
screen height, grows upward or downward according to its position, and restores
the prior height and Y within the current screen. It does not hide the chat.
Width changes and unsent drafts survive. The button remains available with UI
movement locked; auxiliary chat windows retain their retail close/resize controls.

All five logs now use the DAT text margins. The main layout already reserves
space for numbered window buttons, so adding another fixed 16-pixel text gutter
shifted text too far right and wrapped it prematurely. Entry placement also no
longer adds an extra horizontal inset. Expand/restore and unread-arrow artwork
and hit surfaces paint above selectable log text, within their own window's
stacking order. Unread arrows track messages received while browsing history,
clear on returning to the bottom or clearing the log, and do not appear merely
because the user scrolls upward.

The rendered chat fixture covers all five windows' resize grips, margins,
wrapping, saved geometry, independent scrolling, unread indicators and Slate
hit paths. Main expand/restore checks cover upper/lower placement, locked UI,
two display scales, both DAT icons, preserved width and drafts. It is kept in a
separate helper to avoid MSVC's optimizer limit on the larger screen suite.

Validation: Win64 Development editor build succeeded. `ChatInputAndCommands`
and `UIScreens` passed with zero errors or warnings; the expanded main chat and
main/auxiliary unread-control captures were visually reviewed. Results are in
`Unreal/Saved/ChatParity-tests/index.json`. Shared Quest source was synchronized
and `Quest/Sync-ClientSource.ps1 -CheckOnly` reported no differences.

## UCM post-combat corpse handoff: 2026-10-20

ACE's `Creature_Death` waits for the death animation before `CreateCorpse` and
destroying the defeated object. UCM already scheduled visible corpses before
ordinary navigation, but could start retracing its route during this gap and
then reverse again when the corpse arrived.

The host now exposes the confirmed death of its combat target from zero-health
or death-motion events, retaining its time and position across deletion. The
policy holds ordinary navigation for up to eight seconds while waiting for a
new visible corpse in loot range. Normal ownership, appraisal, transfer and
close handling then finish before retracing the existing route. Corpse arrival
wakes the scheduler. Existing combat/recovery priorities remain intact, and
navigation priority bypasses the spawn wait. Disappearance alone, disabled loot,
no loot rules, full inventory, range and teleport changes do not cause a wait.

`PostCombatLootHandoff` simulates a route departure, kill, delayed corpse creation,
delayed contents and transfer confirmation, and return to the original route.
It also covers navigation priority, recovery/combat, timeouts and excluded cases.
Host packet regressions cover both death signals, duplicate notifications,
deletion without death, and GUID reuse. Live server reproduction is still needed
to confirm the reported player's exact timing.

Validation: Win64 Development editor build succeeded. `PostCombatLootHandoff`,
`RouteAndTellRequests`, `LootCombatPriority`, `LongCombatRouteRejoin`,
`SchedulingParity`, `ActivityFailureIsolation`, and `Decisions` passed with zero
errors. The first handoff run exposed a fixture that moved the cached death
position instead of moving the player out of loot range; the corrected fixture
passed on rerun.

## Augmentation base values and bonus colors: updated 2026-10-23

Verified against retail `CACQualities::InqSkill` (00592660),
`AttributeCache::InqAttribute` (005CC4E0), `SkillInfoRegion::Update`, and
`gmSkillUI` in `C:/dev/retail_client_source`. Innate attribute augmentations are
already included in the attribute starting value sent by the server. Skilled
Melee/Missile/Magic (+10) and LumAugAllSkills are included in both raw and modified
skills. These gains must not produce a green bonus by themselves or be added
again from the augmentation count.

Retail treats Jack of All Trades (+5) and LumAugSkilledSpec (+2 per rank for
specialized skills) differently: `InqSkill` adds them after enchantments only
when `raw == false`. The retail skill row and footer compare against the raw
value and can show these as green bonuses with an empty enchantment list.
ACE's `AugmentationDevice::DoAugmentation` and
`CreatureSkill::GetAugBonus_Base/GetAugBonus_Current` corroborate this split
(Enlightenment is an ACE extension).

At the user's request, the interface now includes **all permanent augmentation
gains in its unbuffed baseline**. This intentionally differs from retail's raw
UI comparison. `FACESkillInfo::GetDisplayBase` includes Jack/spec luminance;
`GetDisplayModifier` supplies the skill row color and signed footer suffix.
Without spells or vitae, augmented skills are neutral and have no bonus suffix.
Direct/attribute buffs, debuffs and vitae compare against the complete permanent
baseline; even a small debuff cannot be hidden by a positive augmentation.
The fallback HUD uses the same baseline for its skill colors and base tooltip.

`Base` remains the retail raw value for equipment requirements and plugin
automation. `Current` still adds Jack/spec luminance **after** enchantment and
vitae math, so the presentation change does not alter combat, run/jump speed,
spell eligibility, or the order in which penalties and permanent gains apply.

`ACE.RetailParity.AugmentationStats` exercises live attribute and quality packets,
all six innate attributes, each skill family across untrained/trained/specialized
skills, all-skills luminance, buff expiry, and the modified-only exceptions under
vitae. `UIScreens` checks neutral augmented attribute/skill rows and footers,
including Jack/spec luminance, real positive and negative spell modifiers, vitae,
expiry, and augmentation removal. This is source and automated regression
verification, not a live side-by-side retail capture.

Validation (2026-10-23): Windows editor build passed. `AugmentationStats`,
`CharacterStats`, `Augmentations`, and `UIScreens` all passed in
`Unreal/Saved/AugmentationDisplayReview/tests`, with zero test errors or warnings.
The build log is `Unreal/Saved/AugmentationDisplayReview/build.log`. Shared Quest
source synchronization and CheckOnly passed.

## Screen brightness and contrast: 2026-10-20

The desktop brightness control now adjusts the final world image rather than
only changing gamma between 1.6 and 2.8 (which left white highlights bright).
Brightness spans 20–200%, with a separate 50–200% contrast control directly below
it. Both are neutral at 100%, update and save immediately, and restore through
Defaults. Existing gamma preferences migrate around the old neutral 2.2 value.

The SDR adjustment runs after retail particle composition and before Slate, so
world effects dim with the scene and menus stay readable. Neutral settings add
no rendering pass. Ordinary inventory/character scene captures are excluded to
avoid applying the settings twice. HDR uses native linear color grading rather
than clamping encoded HDR output. Validation covers Windows SDR; headset and HDR
output have not been measured in this change.

`ACE.Rendering.DisplaySettings` checks migration, slider-to-renderer updates,
on-disk persistence, reopening, defaults, and final-color GPU samples across
brightness/contrast combinations with opaque surfaces and retail additive
effects. The existing particle blend and retail UI suites also pass.

## Monster damage rules, route doors, and Apostate objects: 2026-10-20

UCM damage selection no longer exempts explicit melee weapons from an element
filter. Incompatible main-hand preferences fall back to eligible equipment;
monster elements can select another spell family despite a global attack-spell
preference. Offhands must also match or are removed before attacking. Matching
launcher ammunition remains mandatory for arrows, quarrels, and darts. Coverage
includes conflicting preferences/resistances, confirmed equipment, missing
matching equipment, and unchanged global spell selection for unmatched monsters.

Route snapshots now derive door passage from PhysicsState.Ethereal rather than
InitialMotionCommand. ACE Door.Open/Close broadcasts collision changes separately
from animation, so a stale On command must not suppress another open request
after an automatic close. Tests cover both stale On and stale Off with repeated
passability changes; existing retry limits remain in place. The login Play-tab action is now labeled
"Remove server".

The standard [Apostate Nexus Gateway (40810)](https://github.com/ACEmulator/ACE-World-16PY-Patches/blob/master/Database/Patches/9%20WeenieDefaults/Creature/Unsorted/40810%20Apostate%20Nexus%20Gateway.sql)
and [Apostate Aetherium Deposit (40809)](https://github.com/ACEmulator/ACE-World-16PY-Patches/blob/master/Database/Patches/9%20WeenieDefaults/Creature/Unsorted/40809%20Apostate%20Aetherium%20Deposit.sql)
are object-shaped creatures, using retail setups 0x02000443 / 0x02000B82 and
motion table 0x0900013D. The supplied screenshots are retail references for
standard ACE content. Regression fixtures exercise deferred presentation,
deposit clothing, idle ticks, material opacity, distance levels, room reentry,
world picking, and PView admission at their standard 0x0110028A / 0x7F020107
placements. They also check the gateway from the reported 0x011002E7 ledge
position at camera height, with the surrounding retail dungeon cells loaded.
Rendered-pixel comparisons with and without each object verify that its actual
geometry remains visible against the dungeon. Optional captures use
`-ACESaveApostateCaptures`.

The desktop drag fixture gives an attuned Aetheric Resonator to the stock
deposit model and checks the recipient, item, and quantity in GiveObjectRequest.
It also verifies that a missed drop neither gives to the previous recipient nor
drops the bound item on the ground. No per-object visibility or interaction
exception was added. Both standard objects work in the current regression
fixtures; the reported live invisibility remains unreproduced.

Validation: Windows editor build passed. Eight focused suites passed across
`Unreal/Saved/MonsterDoor-tests` and `Unreal/Saved/MonsterDoor-final-tests`,
including buff-to-combat equipment handoff and route recovery. The final
Apostate follow-up passed `CustomWorldPresentation` and `DesktopHighlights` in
`Unreal/Saved/Apostate-final-tests`, including rendered dungeon views and the
bound-item hand-in packet. The Windows editor rebuild also passed. These
fixtures do not substitute for reproducing the affected live session. Shared
Quest source synchronization passed.

### Apostate visibility after graphics reset: 2026-10-25

The initial screenshots did not isolate resolution or window mode: both visible
examples also showed a fresh login. That rendering investigation did not reproduce
the disappearance or establish a rendering fix. Retail references
remain `PView::DrawInside` and `CPhysicsPart::Draw` / degrade selection; neither
justifies a special visibility exemption for these two objects.

`ACE.RetailParity.CustomWorldPresentation` now builds the two stock models
while their room is hidden and checks their subsequent reveal. Rendered-pixel
checks include only appearance geometry and the surrounding rooms; actor-owned
particle effects can no longer make a missing model pass. Captures retain view
history and settle across frames, including hide/reveal and target resizing.
The optional `-ACEApostateGraphicsMatrix` additionally covers five scalability
levels, four target sizes (1280x720, 1920x1080, 1280x1024, 3440x1440), and
60/90/120-degree camera projections. It checks both models after twenty minutes
of simulated idle ticks and repeated room visibility changes. These are
offscreen renderer/portal-admission tests, not a reproduction of a live
windowed/fullscreen transition or the affected player's graphics driver.

`/acueobject` now writes its report to the client log as well as local chat. It
includes the actual actor cell, server-versus-rendered position, visibility and
physics flags, model bounds, camera projection, per-part render eligibility,
selected distance level, geometry, and opacity. This is on-demand diagnostics
for any selected object, with no added per-frame scan. When the problem recurs,
select the missing object via radar and run `/acueobject` and `/acueperf` before
resetting graphics or reconnecting, then preserve `ACUnreal.log`.

Validation: the Windows editor build and local chat-command suite passed. The
full optional Apostate matrix passed on D3D11 SM5 and D3D12 SM5; reports are in
`TestOutput/ApostateVisibility/final-d3d11` and `final-d3d12`. Shared Quest source
synchronization passed. No packaged release or affected live session was tested.

### Apostate falling velocity follow-up: 2026-10-25

The subsequent v136 `/acueobject` report identifies a physics failure: gateway
`801253F4` / WCID 40810 still had its visible mesh, but the actor was 8207.1819
AC units below the received origin in cell `0110028A` at `[40 -140 12.055]`.
Its create state included velocity `[0 0 -2.4225]`, gravity, and Creature item
type. Both the gateway setup `02000443` and deposit setup `02000B82` have zero
movement spheres in retail DAT. The earlier zero-velocity fixtures missed this.

Retail `CPhysicsObj::UpdateObjectInternal` (`005156B0`) checks
`CPartArray::GetNumSphere` (`00518060`): without spheres it retains the existing
XYZ while continuing orientation, animation, particles, and scripts. It does
not exempt Creature items. Our velocity path exempted all creatures from its
sphere-less prop protection, allowing these quest objects to fall indefinitely.

The client now records whether a successfully loaded setup explicitly has no
movement spheres. Such objects discard linear velocity, skip invented ground
support and predicted translation, and retain angular motion and authoritative
teleports. This applies by model physics, with no Apostate WCID special cases.
Missing metadata still uses the existing fallback; ordinary actors and loose
items with movement spheres keep their physics. Pick bounds remain independent.

`CustomWorldPresentation` now supplies the reported velocity through deferred
creation for both stock objects, then replays vector updates and room visibility
changes over twenty simulated minutes. It also checks angular motion, explicit
teleports, rendered model pixels, and picking. Both objects failed the new
falling-velocity assertion before the fix (`TestOutput/ApostateVelocity/before`).

Validation: the Windows editor build and nine suites passed in
`TestOutput/ApostateVelocity/final`: `CustomWorldPresentation`,
`DesktopHighlights` (including the resonator GiveObject request),
`GroundItemPlacement`, `SpawnSupport`, `RemoteSupport`, `RemoteMovementReplay`,
`NetworkAnimationParity`, `MovementReview`, and `ProjectileLifetime`.
Movement controls now use real creature/projectile setups instead of changing
a sphere-less sign's item type; the projectile fixture loads its required DAT
metadata. Tou-Tou's sphere-less model also retains height when Gravity is set.
Shared Quest source verification passed. This is an automated reproduction
using the reported state; no packaged release or affected live session was tested.

### Apostate gateway render-state follow-up: 2026-11-06

The v150 report for gateway `800031CF` / WCID 40810 has zero actor displacement,
normal scale, a registered opaque mesh, and bounds inside the camera frustum.
The player also reported standing on the invisible gateway. This does not match
the earlier falling-object failure. The supplied inside-pit screenshot means a
camera above the opening cannot alone explain the report. Game-thread visibility
flags and collision do not establish that the renderer actually drew the mesh.

`CustomWorldPresentation` now loads the surrounding static scenery plus the
stock acid and door at their authored placements. Pixel comparisons cover the
reported 6.44-unit distance / 110.01-degree FOV, four views of the populated pit,
and real camera-boom endpoints above the gateway. Camera collision is exercised
from both the gateway and ledge, at two distances and four orientations. Hidden
mesh rebuilds now advance actual rendered frames before revealing the object.
These local tests still render the gateway; they have not reproduced the live
failure or established a rendering fix.

`/acueobject` additionally reads the resident render-thread draw flag, transform,
bounds, and occlusion eligibility without rebuilding the proxy. It reports
triangle/vertex counts, material, draw-distance limits, recent rendering, camera
position/cell, near clip, boom collision settings, and two camera collision rays.
The rays describe collision only, since a PhysicsBSP hit need not correspond to
visible geometry. The diagnostic remains local and on demand. An affected
session should capture this expanded report and `/acueperf` before resetting
graphics or reconnecting; the old report cannot distinguish stale renderer
state from a camera/occlusion failure.

Validation: Windows editor build and five D3D12 SM5 suites passed in
`TestOutput/GatewayVisibility/final-d3d12-tests`: `CustomWorldPresentation`,
`ChatInputAndCommands`, `CameraAndEdges`, `CachedWorldDraws`, and
`IdleBodyTransforms`. The chat test invokes the diagnostic on a live mesh and
checks that it does not rebuild, move, or hide it, change selection, or send a
server packet. Offscreen captures do not reproduce a live fullscreen transition
or the affected player's hardware/session. No packaged release was tested.

## Stat colors without active spells: 2026-10-20

The Attributes panel painted all three vital values green unconditionally.
It now compares their maximums with raw maximums computed by the same DAT
formulas as the effective values. The selected vital footer shows current/max
and the matching signed bonus. Gear maximum health and ACE Enlightenment are
included in the raw maximum; they no longer appear as an enchantment bonus.
Damage and resource consumption do not change the comparison.

Retail `CACQualities::InqSkill` adds Jack of All Trades (+5) and specialized
skill luminance augmentations (+2 per rank) only to current skill values.
The original raw UI comparison showed these as bonuses with an empty enchantment
list. The 2026-10-23 presentation change above now includes them in the displayed
baseline while preserving retail's underlying calculation. Source references are
`CACQualities.cpp`, `gmAttributeUI.cpp`, `Attribute2ndInfoRegion.cpp`, and
`gmSkillUI.cpp` in `C:/dev/retail_client_source`.

`CharacterStats` covers empty registries, permanent skill augmentations,
raw vital maximums, Endurance buffs, and horn expiry. `UIScreens` exercises
neutral depleted vitals, gear/enlightenment, positive and negative modifiers,
expiry, and agreement between the row color and footer.
Both suites and the Windows editor build passed. The unbuffed vital capture
was inspected; Quest source synchronization and CheckOnly passed. Evidence:
`Unreal/Saved/StatDisplay-final-tests` and `Unreal/Saved/StatDisplay-build.log`.

## Remote positions and death-to-corpse handoff: 2026-10-20

Compared the remote actor with retail `CPhysicsObj::UpdatePositionInternal`,
`MoveOrTeleport`, `CMotionInterp::get_adjusted_max_speed`,
`InterpolationManager::adjust_offset`, `PositionManager::adjust_offset`, and
`TargetManager::SendVoyeurUpdate` in `C:/dev/retail_client_source`. ACE's
`Creature_Death::CreateCorpse` uses the final physics position after the death
sequence, not the last cached world-object position.

- Remote death previously played only part poses. It now consumes the DAT
  death transition's position/rotation frames through the actor's collision
  path, with object scale and action speed. The clock is independent of mesh
  culling; repeated Dead updates do not replay it. Airborne death retains the
  physical flight and advances the animation without grounded translation.
  The human sequence moves approximately 1.185 metres; the Gromnie and Wasp
  fixtures also contain movement that was previously omitted.
- Grounded corrections now replace that step's locomotion displacement toward
  a fixed authoritative endpoint at retail's bounded correction speed. They
  no longer advance an extrapolated endpoint while exponentially filtering
  another visible body. The five-centimetre completion tolerance and collision
  validation remain. Sticky melee spacing follows interpolation, as it does in
  retail. VR's buffered pose presentation remains separate.
- Pursuit and sticky spacing resolve another rendered player's/creature's
  current physics frame. Unrendered targets fall back to their network position;
  the local player continues to use its live predicted position.

`RemoteMovementReplay` checks independent correction distances at 30/90/144/240
FPS, fixed death endpoints, authored root displacement and final corpse mesh
positions for human/Carenzi/Gromnie/Wasp models, duplicate death, wall contact,
and death during a jump. `MovementReview` checks live remote target lookup
between packets. The original replay's assertion that late corrections could
never move backwards was contrary to retail's fixed-destination correction:
it now checks the retail speed bound. Landing uses retail's five-centimetre
completion tolerance instead of requiring a one-centimetre residual.

These are source and automated replay checks, not certification of every
movement path. A simultaneous retail/Unreal live-client comparison under
network latency remains an acceptance check.

Validation: Windows editor build passed with adaptive unity disabled.
All eight selected suites passed: AvatarMotion, CustomObjectReplication,
MovementReview, NetworkTransport, RemoteMovementReplay, RemoteSupport,
RuntimeActors, and VR.RenderingAndReplication. The new replay assertions
failed before the fixes. Reports are in `Unreal/Saved/RemoteParity-before`
and `Unreal/Saved/RemoteParity-final`; build log is
`Unreal/Saved/RemoteParity-build-final.log`. Quest shared-source sync and
CheckOnly passed; no Linux/Android binary or release was produced here.

## Cast movement control handoff: 2026-10-13

Reviewed retail `CommandInterpreter::HandleKeyboardCommand`,
`TakeControlFromServer`, `MaybeStopCompletely`, and `UseTime`, plus
`CMotionInterp::StopCompletely` and `CMotionTable::StopObjectCompletely`.
Taking control reapplies the held forward, sidestep, and turn command lists;
it is not conditional on the axes changing since the last outgoing packet.

The desktop controller previously cleared its server-control flag without
resending unchanged strafe/turn input. If a cast arrived after those keys
were pressed, the server's new control stamp was never acknowledged by a
movement command until another axis changed. Strafe, turn, and mouse facing
now share the handoff and force one ordinary MoveToState. Subsequent
unchanged frames do not send additional commands. Retail's
`CSequence::remove_cyclic_anims` preserves unfinished links: a strafe/turn
handoff keeps the cast entry animation playing while movement resumes,
rather than truncating the link or waiting for it before allowing movement.

A manual movement release now reaches the server even while Use is busy.
Previously the use-wait branch could absorb the release and update its
"sent" cache despite sending nothing. New server motion, use approaches,
manual combat stops, and teleports reset that release ownership. Windup
actions and locally controlled held emotes retain their existing behavior;
spell requests, projectile release, damage, and cooldown timing are unchanged.

`ACE.RetailParity.RunSpeed` now replays strafe, turn, and combined commands
with early/late cast delivery at 30/60/144/240 Hz, checks actual MoveToState
payloads and server-control stamps, verifies immediate translation/rotation,
and covers release during a pending cast. It also checks that Play Possum
can still slide and that taking control does not discard a scarab windup.
Windows editor and Linux Development builds passed. RunSpeed, PvPCombat,
and EmoteTransitions passed on Windows with no warnings. RunSpeed also
passed natively under Linux/WSL Vulkan with one non-failing console-lookup
performance diagnostic from the movement stress fixture. Evidence is in
`Saved/CastMovementReview`. Quest source synchronization reports zero
differences. These are deterministic input/network replays; live PvP under
variable server latency still needs in-game confirmation.

## Salvage drag feedback and doorway rendering: 2026-10-13

Salvage now shows the retail ItemSlot accept/reject artwork on the hovered
square, including empty slots. The preview uses `CanAddItemToSalvageQueue`,
the same ownership, retained-item, duplicate, tool, and material rules as the
drop operation. Hover only previews; it never queues or destroys an item.
The regression covers accept/reject icons, single/multiple material settings,
duplicate rejection, position/size at 100% and 150% Slate DPI, leaving the list,
and cancellation. The DAT path override permits the same test on Linux.

Found another global doorway render invalidation after the residency fixes:
`SetLandEnvCellFloorPriority` changed a material scalar and then explicitly
rebuilt the terrain proxy. Indoor occupancy changes that scalar across the
surrounding outdoor blocks, including those without buildings. Both individual
landblocks and terrain chunks now retain uploaded geometry for these parameter
updates. Actual material replacements still invalidate normally through
`SetMaterial`; the floor-depth values, collision, and visibility remain intact.
The rendered regression repeatedly toggles priority, checks retained proxies,
and compares pixels against the former forced-rebuild path.

Windows editor and Linux Development builds passed. Salvage, CachedWorldDraws,
EntranceTransitions, and ResidentTerrainCollision passed on Windows/D3D12 and
native Linux/Vulkan under WSL with lavapipe. Evidence and renders are in
`Saved/SalvageDoorwayReview`. Linux had two non-failing diagnostic warnings in
the entrance fixture. The Linux test runner exits with status 1 after exporting
successful results because `-TestExit` calls Unreal's forced Unix exit; results
were checked in the JSON report rather than inferred from that exit status.
The final salvage render fixture also passed on both platforms with its
simulated pointer held in place for screenshots. The Linux build caught misleading indentation in
the pending UCM Micro start/stop callback; explicit braces fix it without a
behavior change. These checks verify behavior and eliminate a demonstrated
rebuild path; they do not establish frame times on the reporter's GPU/driver.

## F-key stack overflow consolidation: 2026-10-12

Retail `ItemHolder::AttemptAutoMerge` requires the whole selected quantity to
fit, whereas `AttemptMerge` caps a direct merge at the destination's remaining
capacity. ACE `Player.DoHandleActionStackableMerge` supports partial transfers
and sends a count update for each surviving stack. At the user's request, F/hand
sorting now prefers a whole-stack merge but falls back to that capacity-limited
transfer when no whole-stack destination fits. This intentionally extends the
retail automatic-selection behavior using the ordinary StackableMerge packet.

For example, 15,137 and 15,000 pyreals become 5,137 and 25,000. The destination's
advertised maximum is used for every item type; 25,000 is not hard-coded.
Both stacks stay in their packs and quantities remain server-authoritative.
Repeated presses wait for both replies, including reversed arrival order. A full
stack cannot spill back into the remainder on another F press. Selection stays
on the destination, and existing full-merge preference and no-match pickup
behavior remain intact.

Validation: Windows Development editor build passed. UIScreens, InventoryOrder
and SelectionToolbar passed in `Saved/StackOverflowReview/Tests`, including
the 30,137-pyreal case in both reply orders, a 100-item maximum, whole-merge
preference, duplicate-input suppression and full-stack/remainder follow-ups.
Shared Unreal/Quest source matches. No live-server merge was performed.

## Pack drops onto closed world vendors: 2026-10-12

Retail `ItemHolder::AttemptPlaceIn3D` dispatches vendor drops to
`ClientUISystem::AttemptSellToVendor`: send ordinary Use when the shop is closed,
remember both vendor and item, and wait for `Handle_VendorInfo`. Only a matching
vendor reply invokes `VendorSellUI::AddItemToSell`, which opens Sell and expands
the pack through `gmVendorUI::AddItem` with individual acceptability filtering.
It stages an offer; it does not submit a Sell transaction.

Our main-backpack guard returned before world targeting because the main pack
uses the player GUID. Main packs now reach the shared world-drop path, while
remaining ineligible for giving/dropping/moving the player. Side packs retain
the same flow. Pending offers record the vendor identity, including the Give
hotkey path; unrelated replies and closing the panel cannot populate another
shop. Contents still use the authoritative vendor filters and full stack sizes.

Validation: Windows Development editor build passed. `DesktopHighlights`
exercised captured-pointer main/side-pack drops, the Use packet and replayed
ApproachVendor reply, repeated/open-shop drops, protected contents, cancellation,
busy use and mismatched replies. `UIScreens` passed its existing vendor, inventory,
partial-stack and Give-key checks. Both suites reported zero errors/warnings in
`Saved/VendorWorldDropReview/Tests`. Shared Unreal/Quest source matches. Live-server
verification remains; these were local protocol and rendered UI regressions.

## Scroll reading and held movement: 2026-10-12

Retail `ItemHolder::Use` sends the ordinary inventory Use event and marks the
interface busy. `ACSmartBox::HandleNetBlob` passes authoritative motion to
`CommandInterpreter::LoseControlToServer`; `UseTime` does not reapply held
movement while motions are pending. A fresh press can explicitly take control
through `TakeControlFromServer`. This is not an unconditional movement lock.
`CMotionTable::GetObjectSequence` treats Reading (0x4000001c) as a substate
with entry, held pose and return-to-Ready links. ACE `Scroll.ActOnUse` changes
to peace, plays Reading, consumes an eligible scroll and learns its spell,
returns to Ready, and releases busy state; `Player.TryUseItem` schedules UseDone.

The client previously replayed held forward input each frame and treated
Reading as a short gesture. It now suspends input held before server Reading,
clears autorun/mouse-forward and stale plugin steering, and retains the reading
pose until completion. A new desktop movement press may reclaim control and
exit the pose, matching retail. Rejection/timeout clears the pending-use gate.

UCM now waits for a confirmed peace stance before reading and tracks UseDone.
With Read unknown scrolls enabled, eligible carried scrolls are recovered after
an interrupted pickup or restart. Known spells and protected items are skipped;
unconfirmed reads retry with backoff, at most three times per item per run.
UseDone success alone never marks a spell learned. Repeated failures retain the
scroll, report the problem and leave other activities running. Imported
Read/Sell/Salvage rules alone still do not consume pre-existing possessions.

Validation: the Windows Development editor build passed. ScrollLearning,
LegacyAdapters, RouteAndTellRequests and SchedulingParity passed in
`Saved/ScrollReadingReview/Tests`; EmoteTransitions passed in
`Saved/ScrollReadingReview/Final` after correcting its input fixture and adding
stale-approach coverage. The request fixture emitted 34 JSON snapshot warnings,
with no test errors. Real human DAT transitions were checked at 30/90 Hz, plus
Reading completion at 60 Hz, without a live server consumption session.
Shared Unreal/Quest source synchronization reports no differences.

## Indoor/outdoor transition residency: 2026-10-12

Reviewed the two Bandit Castle underpasses reported at BDD20033 and BDD2003C
against retail `CPhysicsObj::change_cell`, `CEnvCell::init_static_objects`, and
the shared Unreal occupancy/visibility paths. Retail retains a loaded cell's
static objects across visits. Our building shells already retained bodies, but
room BSP, furniture, scripted bridge supports, and exterior terrain still
destroyed/recreated bodies at collision-residency changes. Scripted supports also
disabled and recooked their physics sections. The baseline regression reproduced
36 body-retention failures.

Room and furniture collision now uses actor filters on retained bodies. Terrain
retains its cooked body with queries disabled and every physical response ignored
while outside the player's collision neighborhood. Real ray, object, and capsule
queries verify both active support and inactive exclusion. Decorations without
PhysicsBSP still never create instance bodies. Unloading an actor still releases
its resources; retaining residency does not mean keeping every visited room.

PView visibility also recreated procedural render proxies and uploaded their
vertex/index buffers again. Stable cached meshes now use Unreal's scene visibility
update, including its lighting/ray-tracing bookkeeping. Geometry and material
changes still rebuild normally. Terrain-only visibility no longer invalidates
every attached scenery mesh through the default child propagation. Pixel comparisons
cover actor hiding, component visibility, independently visible attached scenery,
restoring visibility, geometry edits, and hidden-body shadows.

The crossing fixture includes both supplied lines extended past their thresholds,
both directions, walking/running, desktop/VR, and 15/30/60/144/240 FPS: 640 cases per
platform. A moving desktop camera exercises view occupancy separately from the
player. No Bandit Castle identifiers or coordinates are used by the runtime fix.
Tests accept `-RetailDatDir=` to run against the same DAT content on Linux.

Evidence is under `Saved/BanditTransitionReview`: Windows baseline body-retention
failures, development builds for Windows/Linux, and eight passing suites on each
platform in `FinalWindows` and `FinalLinux`. Linux runs the native executable under
WSL with Vulkan/lavapipe software rendering. This verifies Linux collision and
render behavior, not frame-time performance on the reporter's graphics driver;
the visible live-client stall still needs confirmation on that machine.
The final attached-scenery change also passed `CachedWorldDraws`,
`EntranceTransitions`, and `ResidentTerrainCollision` again on both platforms
(`SceneryWindows` / `SceneryLinux`).

## Enchantment panels after death: 2026-10-09

Corrected `MagicPurgeEnchantments` and `MagicPurgeBadEnchantments` to retain
duration -1 equipment effects. Retail `CEnchantmentRegistry::PurgeEnchantmentList`
and `PurgeBadEnchantmentList` explicitly preserve these records; Vitae and
cooldowns are separate registries and also survive the purge. ACE's
`EnchantmentManager.RemoveAllEnchantments/RemoveAllBadEnchantments` uses the same
exceptions, including cooldown spell IDs above 0x7fff. The client previously
discarded equipment effects on both paths and cooldowns on the ordinary purge.

The server still chooses whether a death removes temporary buffs or preserves
them through the spell-retention augmentation. Beneficial/harmful panel placement
continues to use retail SpellBase flags; purge decisions use the transmitted
enchantment flags. Explicit enchantment removals still remove permanent effects
when equipment is taken off or loses its enchantment. The screen regression now
checks both panels after each purge, a weaker gear buff reappearing after a
stronger temporary cast is removed, and a later explicit removal clearing the
harmful panel. It also verifies that Vitae and cooldowns do not appear as normal
buff/debuff rows.

Validation: Windows editor development build and both `ACE.RetailParity.UIScreens`
and `ACE.RetailParity.CharacterStats` passed without test warnings. Reports are
in `Saved/EnchantmentReview`; the retained equipment row is captured in
`Saved/Automation/RetailParity/GameplayEquipmentEffectsAfterDeath.png`.

## Playable Olthoi review: 2026-10-09

Reviewed both heritages (12 Ripper, 13 Acid Spitter) against retail
`CharGenState`, `gmCGHeritagePage`, `gmCGAppearancePage`, `gmCG3DView`,
`ClientCombatSystem::ObjectIsAttackable`, `ClientCommunicationSystem`, and
`gmMainChatUI`. Compared the existing ACE player factory, Olthoi templates
43480/43481 and Salivatory Goo 43489 with the client data and action packets.

The existing DAT-driven creator already offers both heritages, body/marking
customization, the authored preview/paperdoll animations, and the three-page
heritage/appearance/summary flow. Fixed examination previews to use native setup
scale as retail does; world-character scale had incorrectly shrunk the Acid
Spitter below the authored camera. The creation packet retains the normal DAT
selection and server-side template validation. VR now keeps nonhuman bodies
visible, subject to the show-body preference, using authored animation rather
than human IK. VR controller indicators remain the fallback for nonhuman hands.

Nearest-monster selection, automatic targeting, and repeated melee attacks now
use retail's shared hostility policy: matching PK/PKLite player pairs and either
side's Free-PK flag are eligible. Ordinary NPK players, dead targets, and self
are excluded. Hostility is determined from replicated server flags, not from an
assumption that every Olthoi is Free-PK. Existing target distance limits remain.
The UCM hunting plugin remains PvE; this does not introduce automated PvP.

Local speech, tells and emotes now decode ACE's Olthoi `&` and language-bypass
`^` name markers. Human/Olthoi speech is scrambled with retail's phrases unless
the listener has NoOlthoiTalk. Names are decoded before squelch/reply matching;
unintelligible tells do not trigger plugin commands or replace the reply target.
Desktop and VR chat destination selection follows retail's heritage restrictions.
Incoming heritage property changes update these rules without requiring relogging.

Combat and looting use the existing standard protocol: Ripper unarmed attacks,
Acid Spitter's equipped Salivatory Goo and six starter spells, opening corpses,
moving slag to inventory, and giving rewards to NPCs. ACE continues to own
eligibility, starting inventory/spells, equipment restrictions, PvP damage,
death/loot rewards, progression, portal/recall restrictions, and social limits.
Their existing Olthoi-specific server error messages are present in the client's
weenie-error table. This change does not enable Olthoi on servers that disable it.

Validation: Windows editor development build succeeded. Ten distinct automated
tests passed across the focused and final runs, covering creation data and wire
payloads, rendered creation controls/previews (including both Olthoi), customized
meshes, authored collision dimensions and animations, acid-spell metadata,
hostility, language handling, standard combat/loot packets, desktop chat, and VR
chat/rig/menus. The initial camera fixture emitted its existing contextless-world
cleanup warning; all seven tests in the final run passed without warnings.
Evidence is in `Saved/OlthoiReview` and rendered screenshots in
`Saved/Automation/RetailParity/Olthoi*Creation*.png`.

This is source/data, rendered-fixture and loopback-protocol verification, not a
completed live multiplayer acceptance run. Still required for release acceptance:
create both types on an Olthoi-enabled server, relog, test human/Olthoi PvP with a
retail observer, collect slag/glands and redeem upgrades, verify death/re-entry
and portal eligibility, and playtest physical VR on both headset runtimes.

## Rendering and collision cost review: 2026-10-09

Reviewed texture/material reuse, creature geometry and animation, particle
simulation/vertex submission, pooled lighting, and collision/selection queries.
Removed per-evaluation animation-list allocations from casting/death links by
borrowing the immutable motion-table records, retaining the same retail link
lookup order. Dense particle updates now compute exact section bounds during
worker-side vertex generation instead of calculating them during the game-thread
upload. Serial updates retain their original bounds path: moving that calculation
did not improve the large serial fixture. `ace.Particles.PreparedBounds=0` retains
the original path for A/B comparisons. No particle counts, textures, model detail,
animation cadence, collision sizes, or networking policy were reduced or changed.

The same-process desktop development benchmark with 120 emitters and 61,440 live
particles measured 9.405 ms per CPU flush with original adaptive updates versus
9.041 ms with prepared bounds (about 4%). The final worker-only run measured
9.483 versus 9.186 ms (about 3%). These are isolated CPU measurements,
not whole-frame FPS or a Quest/Linux GPU benchmark. Regression checks compare all
vertices, normals, colors, UVs, draw counts, and exact bounds with the original
path; rendered prefix tests also exercise shrinking, clearing, and refilling.

Existing caches remain effective: warm Snow Tusker creature/corpse construction
performs zero surface re-resolves, and repeated geometry requests use the shared
cache. First-time Snow Tusker construction still measured 20.57 ms in the cold
fixture, versus 0.80-0.87 ms warm; corpse construction was 4.11-4.37 ms. This does
not establish that live Frozen Valley hitches are eliminated. Lighting checks
measured 0.031 ms for 128 candidate sources and eight actual lights, and 0.035 ms
for lighting updates on 64 real Acid Staff effects, excluding GPU cost. Retained
the existing batched collision queries and exact authored body sizes rather than
lowering collision frequency or skipping contacts.

Local evidence is under `Saved/PerformanceOct09/`: baseline, build logs,
same-process benchmark, and regression reports. All 30 selected rendering,
animation, streaming, visibility, and collision tests passed (SceneAudit emits an
existing temporary-world cleanup warning). All three final focused particle tests
passed after the worker-only refinement. Remaining acceptance includes live crowded encounters,
GPU overdraw, and standalone/mobile hardware profiling.

## Cached monster targeting regression: 2026-10-09

`CPlayerSystem::SelectNext` checks planar distance against `GetRadarRadius`
before applying its distance-plus-height ordering. `GetRadarRadius` is 75 AC
meters outdoors and 25 indoors, classified by the player's cell suffix in
`SmartBox::is_player_outside`. Our monster/item traversal had omitted that
check, allowing previously seen monsters throughout retained landblocks.
Restored the range check using current player/object positions on every
selection, and shared the radius with the radar and other keyboard selection
paths. Fellowship traversal retains its separate member-list behavior.

Replaced erroneous tests expecting 100-110m selection with compass-rim cases.
`ACE.RetailParity.CameraAndEdges` now checks nearest/next/previous bounds,
cached targets after walking away and returning, exact boundaries, indoor
transitions, cross-landblock coordinates, and planar range versus weighted
height ordering. `ACE.RetailParity.UIScreens` exercises the keyboard and
automatic-targeting integration. Local evidence: `Saved/TargetRangeOct09/`.

## Inspection presentation review: 2026-10-09

Compared shared desktop/VR appraisal formatting with local retail
`ItemExamineUI`, `BasicCreatureExamineUI`, `CreatureExamineUI`,
`CharExamineUI`, and `ExperienceSystem`. This is source, packet-fixture and
rendered-screen verification, not live acceptance on every custom server.

- Restore the blank line between an item's spell list and requirements, and
  before Armor Level, including shields. Keep individual requirements and
  elemental resistance rows consecutive.
- Put equipment set/gear ratings before equipment stats, including the
  separate vitality line. Preserve retail armor resistance ordering/colors,
  bow damage modifiers and skill-limited effective shield levels.
- Preserve the appraisal spellbook's high-bit enchantment marker separately
  from the normalized spell IDs used by plugins. Inherent spells appear in
  the short list; active enchantments have their own description section.
  Failed appraisals show unknown spells rather than disclosing descriptions.
  Spellcraft, mana and mana cost accompany the inherent spell details.
- Display portal minimum/maximum levels and destination, owner-only wield
  restrictions, item-use level/skill/specialization requirements, and item
  level/XP using retail's three progression schemes. Salvage shows average
  workmanship and the number of contributing items.
- Include server-provided Crushing Blow, Biting Strike, Armor Cleaving and
  left-hand tether text. VR value/burden now preserve unknown fields and
  number grouping instead of treating absent values as zero.
- Check player/profession-bearing NPC routing, allegiance and body armor,
  monster stat rows, failed appraisals, door lock status/difficulty, and
  authored sign/description paragraphs without adding gear or player fields.

Regression coverage: `ACE.RetailParity.AppraisalPresentation`,
`ACE.RetailParity.ItemPresentation`, `ACE.RetailParity.UIInteractions`,
`ACE.RetailParity.UIScreens`, and `ACE.VR.RigAndMenus`. Local reports and
renders: `Saved/InspectionParityOct09/` and `Saved/Automation/RetailParity/`.
The generated screens verify the current renderer against retail source/DAT
contracts; they are not a simultaneous retail-client screenshot comparison.

## Interface and player-network review: 2026-10-07

Scope: review the classic desktop UI against the local retail decompile and DAT
layouts, and trace player transmission/reception through the retail protocol,
ACE server readers, local prediction, and remote presentation. This is a source
and automated-test review, not a pixel-by-pixel certification of every screen
or a live retail/Unreal multiplayer acceptance session. Existing user-requested
VR menus, plugin windows, server selection, and Unreal graphics controls remain
intentional extensions.

### Confirmed discrepancies corrected

- **Friends and Squelch:** Squelch now uses `classic_squelch`'s 24-pixel
  row template, alphabetical ordering, selection artwork, and separate
  yellow Character/red Account status column. Wheel, arrow, and thumb
  scrolling reach the full list instead of stopping at 24 entries. Both
  screens apply retail's selection/online/capacity button states. Friend
  tells retain multiword names, and friend update type 3 removes silently.
  Incoming speech, ranged speech, emotes, tells, fellowship/allegiance, and
  Turbine chat consult the server-provided squelches before chat/plugin
  delivery. Preserve the four-word channel mask and retail's personal
  spellcasting-text exception; clear stale squelches on session reset.
  Account-wide identity enforcement remains server-owned via the standard
  account-squelch request. Evidence: `gmFriendsUI`, `gmSquelchUI`,
  `SquelchDB::IsSquelched`, `SquelchInfo::IsSquelched`, ACE message writers,
  rendered social panels, and isolated incoming/outgoing wire regressions.
  This does not replace a live two-account server acceptance test.
- **Chat destination text:** match `gmMainChatUI::HandleSelection`,
  `InitTalkFocusMenu`, and language DAT `0x23000001` for the compact mode
  captions (`Chat`, `Fell`, `Gen`, etc.) and all fourteen popup labels
  (`Chat to All`, `Tell to General Chat`, etc.). Selected-character labels
  retain the retail wording; an ignored selection checks the squelch row
  without replacing its caption. The screen regression compares every
  retail caption and menu label directly against the DAT strings and verifies
  the existing bitmap font, named selection, and checked state.
- **Complete fragment identity and supersession:** retain all 64 bits of the
  server's blob ID rather than combining fragments by its low 32 bits. Track
  ephemeral sequence/server identity separately from its wrapping 16-bit ordering
  stamp; discard obsolete versions and reclaim their superseded partial data.
  Ordering history uses retail's five-second expiry and sweep interval.
  References: `NetBlobIDUtils`, `Indicator::FragIsObsoleteEmphemeral`,
  `Indicator::AcceptFrag`, and `ArrivedEphInfo::fTimedOut`.
- **Incomplete-message storage:** maintain exact buffered-byte accounting,
  release it on completion/supersession/world exit, and bound assemblies to
  1,024 messages / 32 MiB. Exhaustion reports a reconnect error instead of silently
  dropping authoritative gameplay data. These bounds are modern resource guards,
  not retail timeout constants. Delayed message payloads are not expired merely
  because their ordering-history entry expires. Reject unregistered queue IDs
  without rejecting later valid packet contents, following
  `PacketController::AddReceivedBlobToQueue` (queues 1 through 11).
- **Transport timing under loss:** checksum-validated echo requests and responses
  are processed on receipt, before gameplay packet reordering. Missing gameplay
  packets no longer delay keepalive replies or hide a received RTT sample; draining
  the reorder queue does not repeat either operation. Reference:
  `SharedNet::ProcessPacket` / optional-header dispatch. Gameplay ordering and
  chronological TimeSync handling remain separate.
- **Motion before object creation:** queue validated F74C updates for missing
  objects, inventory-only stubs, and future incarnations; replay them after the
  complete physics description arrives. Reject old incarnations and stale motion
  sequences. Previously these updates were broadcast before an actor existed or
  discarded for a future incarnation. Reference: `ACSmartBox::DispatchSmartBoxEvent`
  case F74C and `SmartBox::ProcessObjectNetBlobs` in the local retail decompile.
- **Unavailable retransmissions:** process the server's RejectRetransmit list
  after checksum validation. Release only previously requested missing packet
  IDs, then drain later received updates. Previously the optional header was
  skipped and the contiguous dispatcher could wait forever. A late real packet
  takes precedence over a pending rejection. References: retail
  `SharedNet::HandleEmptyAck`, ACE `PacketRejectRetransmit`, and `NetworkSession`.
  This restores progress; it cannot reconstruct data the server no longer has.
- **Retransmission requests and checksums:** service checksum-validated encrypted
  standalone NAKs as well as cleartext NAKs without consuming a gameplay packet
  sequence. Preserve the original separately hashed payload and ISAAC key on
  retries; hashing the concatenation could produce a different checksum across
  unaligned fragment boundaries. Reference: retail `SharedNet::HandleNak` and
  ACE packet/fragment checksum construction.
- **Server-controlled movement acknowledgements:** `FACESession` previously
  left its outgoing server-control timestamp at zero. Seed the four outgoing
  physics timestamps from the player's complete creation description, advance
  server-control from accepted movement, and reset them on world/session exit.
  AutonomousPosition, MoveToState, and Jump share these session timestamps.
  The wire test checks the incarnation and server-control fields actually sent.
- **Motion ordering:** accept only newer movement under a non-older server
  control timestamp. The previous OR condition allowed stale movement when
  control advanced, or newer movement from obsolete control. A player's own
  autonomous echo now acknowledges timestamps without replaying local movement;
  other players' autonomous updates still animate. Reference:
  `CPhysics::SetObjectMovement` in `CPhysics.cpp`.
- **Malformed physics messages:** reject incomplete PositionPack bodies rather
  than treating them as unsequenced positions. Validate the complete motion
  body, including sticky targets, before advancing sequence state; reject
  nonfinite position, vector, and motion fields. Truncation tests follow a bad
  message with the valid same-sequence message to prove it remains acceptable.
- **PlayerModule and spellbook persistence:** honor the flags for one/five/
  seven/eight spell bars, optional spellbook filters, options2, legacy timestamp
  strings, and GenericQualitiesData. The previous parser discarded old bar
  lists and unconditionally consumed a filter word, displacing later fields
  when that word was absent. Restore retail's default filter/options masks.
  References: `PlayerModule::UnPack` and `gmSpellbookUI::IsFilteredOut`.
- **Spellbook controls:** restore the received school/level filters, map the
  retail school bits to the DAT school identifiers, and send filter edits with
  retail action 0x0286. Saving other character options retains those filters.
  Preserve the unexposed ninth-level/reserved bits instead of clearing them.
- **HUD failure path:** the DAT-enabled desktop HUD no longer silently falls
  back to `UACEGameHUDWidget` when the retail interface cannot load. It returns
  to login with a data error. The custom widget remains an explicit development
  opt-out, not the production failure path.

### Screen coverage and remaining differences

The 101-entry layout asset manifest is an inventory of DAT resources, not proof
that 101 functional screens have been implemented. The ordinary gameplay tree
is `classic_gameplay` (0x21000005); its imported subtrees provide inventory,
vendor, trade, container, salvage, and other retail windows. Dynamic lists and
labels are implemented through the gameplay binder and retail text renderer.

- **Character selection/creation:** reviewed the DAT flow, profession and
  attribute/skill controls, and 3D preview path against the chargen/character
  management classes. Covered by CharacterCreationData, CharacterCreationScreens,
  CharacterManagement, and LocalLoginSettings tests.
- **Inventory, paperdoll, stack controls, selection toolbar, container, vendor,
  trade, salvage, and examination:** reviewed the imported layout subtrees,
  shared object properties, binder action dispatch, and corresponding session
  send/receive paths. Coverage includes InventoryOrder, ItemPresentation,
  AppraisalPresentation, Salvage, SelectionMana, SelectionToolbar, UIScreens,
  and UIInteractions. Existing checks do not exhaust every custom-server
  property combination or every vendor/trade rejection response.
- **Character attributes/skills/titles, vitals, spellbook/casting/components,
  combat power, and indicators:** use DAT assets and server-supplied state;
  CharacterStats, SpellFormulaLevels, CombatProtocol, AvatarMotion, UIScreens,
  and PlayerModuleOptions exercise the shared behavior. Filter persistence and
  legacy bar decoding were the concrete defects corrected in this pass.
- **Chat, keyboard/configuration, friends, fellowship, allegiance, journal,
  contracts/books, housing information, map/radar, and support dialogs:** traced
  their binder and session handlers. ChatInputAndCommands, KeyboardBindings,
  PanelResizeSocial, UILayout*, RadarArt, and UIScreens cover representative
  behavior. Support dialog text includes ACE-specific wording; these are
  reconstructed dialogs, not a verbatim reproduction of all retail CSR text.
- **Barber:** StartBarber (0x0075) now opens the authored DAT barber controls
  in gameplay, with face/hair choices, palette wheel, shade slider, rotating
  portrait, Apply and Cancel. Crown/flame suppression and Empyrean Earthbound
  use retail's heritage-specific options. Appearance restoration distinguishes
  bald styles sharing a head model; undead skeletons use the alternate facial
  texture entries. Untouched server fields survive editing. FinishBarber
  (0x0311) sends sixteen 32-bit fields on WeenieQueue; Cancel sends nothing,
  and the world appearance awaits the server update. Truncated incoming
  appearances cannot open the editor. VR opens the same retail panel.
  Sources: `gmBarberUI`, `ACCharGenData::GenerateBaseAppearanceData`,
  `CM_Character::Event_FinishBarber`, ACE `GameEventStartBarber`, and
  `Player.FinishBarber`/`SetupConst`. `ACE.RetailParity.Barber` exercises DAT
  hairstyle round trips, option changes, gameplay opening/replacement/closing,
  cancel/reopen, rotation, malformed events, exact outgoing packet fields,
  and visible portrait pixels in the composed UI. Capture:
  `Saved/Automation/Barber/Human.png`; build/test logs:
  `Saved/RetailInterfaceNetworkReview/barber-*`. Live server persistence and
  live VR interaction remain unverified; no Linux/Android binary is built
  by this source-parity pass.
- **House purchase/rent:** the follow-up now connects HouseProfile (0x021D)
  to the actual Slumlord DAT subtree, Buy/Maintenance tabs, payment slots,
  scrolling, ownership checks, stack splitting, pack expansion, and retail
  confirmations. Purchases require all payment requirements; maintenance can
  be partial. Trade notes count as pyreals at face value. Actions 0x021C/0x0221
  carry the lord and payment object IDs on the retail WeenieQueue, with no
  speculative removal of inventory. Failed transactions query the active lord
  again (0x0258); subsequent profiles replace the authoritative paid amounts.
  Reference: `gmSlumlordUI`, `CM_House`, `HouseProfile`, `HousePaymentList`,
  `ACCWeenieObject::GetHousePayment`, and ACE's matching event/action readers.
  `ACE.RetailParity.Housing` covers wire parsing and truncation, purchase/rent
  packets, notes, foreign/equipped/trade rejection, full and partial stacks,
  high-bit dynamic item IDs, main-pack drag, drag-out removal, confirmations,
  refresh after failure, and changing environment windows. Offscreen captures
  verify the authored tabs, slots, and labels. Live purchases, live VR use,
  and legacy incremental rent notices not emitted by the current ACE server
  remain unverified; this is not certification of every housing operation.
  Validation: Windows editor build and the Housing, Salvage,
  InteractionRecovery, and PlayerModuleOptions automation cases passed.
  Housing captures are under `Saved/Automation/Housing`; logs are under
  `Saved/RetailInterfaceNetworkReview/housing-*`. Quest shared-source sync and
  `-CheckOnly` passed; no new Linux or Android binary was built in this pass.
- **Allegiance detail response:** `AllegianceInfoResponse` is explicitly ignored
  in the session switch. Existing AllegianceUpdate-based display and commands
  do not establish parity for this separate response.
- **Saved gameplay UI options are incomplete:** the PlayerModule 0x200 archive
  remains opaque and is skipped by inventory-footer probing. Local layout
  persistence is not an implementation of retail's server-stored GameplayOptions
  archive. The new decoder only fixes the preceding flagged fields.
- **Administrative and uncommon response paths require more work:** an imported
  Admin layout alone does not establish a working retail AdminQualities screen.
  Editable-book response events and PopupString also lack explicit event cases;
  book reading, chat errors, and ordinary confirmation dialogs are separate paths.

### Networking coverage and limits

Reviewed packet headers, ISAAC/checksums, 464-byte fragment payloads, reliable
ordering/retransmission/acknowledgement, login/world lifecycle, and the movement
senders/receivers. References include retail `PositionPack`,
`AutonomousPositionPack`, `RawMotionState`, `SmartBox::UnpackPositionEvent`,
`CPhysics::SetObjectMovement`, and the matching ACE network structures/actions.
The new tests use independent field-by-field packets and UDP loopback, rather
than merely round-tripping the same serializer.

Movement/presentation coverage includes run speed, command decoding, turning,
teleport epochs, delayed corrections, collision support, jumping, slopes/stairs,
and remote replay at multiple render rates. VR extension messages are capability
gated, while standard position/motion remains available to retail observers.
Server tests exercise VR negotiation, raw motion, run speed, and network strings.
Head/hand poses remain a negotiated extension; an unmodified retail client
cannot display those extra joints.

**Open acceptance gates:** live retail-to-Unreal and Unreal-to-retail movement
under loss/reordering and portal/server-control changes; Linux and physical
Quest/SteamVR input/rendering; visual review of every resizable panel at each
supported scale. Tests with names such as LiveLogin can skip without configured
credentials and must not be presented as evidence of a live session.

The broad baseline completed 121 tests with six failures. Two were stale test
fixtures: standalone font labels were marked canvas-flattened, and the VR leg
reset compared against an animated idle frame instead of the authored bind pose.
Those fixtures were corrected; a clipboard test also now waits for the Windows
clipboard before pasting. Four baseline failures remain outside the fixes above:

- ShoushiGap: retreat from the Eiichi/lifestone collision gap.
- VR.StairCeiling: steep-slope body penetration and wading-height assertions.
- WorldDat: drawing triangle count for GfxObj 01001221 (20 expected, 44 actual).
- YaraqScene: pedestal material count (two expected, three actual).

The last two compare raw fixture geometry/material counts against runtime mesh
construction, which also resolves authored close-detail models. They need a
separate geometry/fixture investigation; the expected counts were not changed
merely to obtain a pass. Collision failures remain real acceptance blockers
until resolved. The overall client is therefore **not certified as full parity**.

Validation logs are under `Unreal/Saved/RetailInterfaceNetworkReview/` (local,
ignored artifacts):

- Message-identity continuation: editor build passed
  (`message-identity-final-build.log`), and all nine networking/movement/VR checks
  listed below passed again (`message-identity-tests.log`, exit 0). Added wire
  fixtures for 64-bit identity collisions, ordering-stamp wrap/supersession,
  obsolete complete messages, history expiry, assembly budget exhaustion,
  duplicate accounting, unsupported queues, immediate echo/RTT during a packet
  gap, bad echo CRC, and no repeated echo when the gap drains. Quest sync and
  CheckOnly passed. No physical-device or live multiplayer test was performed.
- Networking continuation: Windows editor build passed
  (`network-recovery-build.log`); all nine selected checks passed with exit 0
  (`network-recovery-final.log`): NetworkTransport, CustomObjectReplication,
  AvatarMotion, MovementReview, LinkTiming, NetworkWeather, RemoteMovementReplay,
  VR.RenderingAndReplication, and VR.UniformLocomotionProtocol. The wire tests
  cover malformed/unsolicited retransmission rejections, multiple gaps, late
  packets, encrypted NAK delivery and unaligned-fragment retry checksums.
  Motion tests cover pre-create, future incarnation, stale/duplicate updates,
  truncated payloads, and inventory-only stubs. An initial run aborted on a
  new loopback fixture's unconfigured socket/unchecked empty receive; the fixture
  was corrected before the complete nine-test rerun. Quest sync and CheckOnly
  passed. This is Windows automation and shared-source validation, not Linux,
  Android, or a live multiplayer loss test.
- Windows editor builds succeeded (`build-final.log`, `build-fixtures.log`,
  `build-verified.log`). After final HUD guard/message cleanup, all four smoke
  checks passed: MissingDatLogin, NetworkTransport, PlayerModuleOptions, and
  UIScreens (`smoke-tests.log`).
- Across `final-tests.log`, `wire-tests.log`, and `remaining-tests.log`, all
  123 selected client checks completed: 119 reported Success and four reported
  Fail (the four listed above). One Success is the explicitly skipped LiveLogin
  opt-in check, so this represents 118 executed passes, one skipped live check,
  and four failures. `results.txt` contains the consolidated results.
- The first final run stopped on a new test-fixture cache lookup using the next
  packet number instead of the packet just sent. The fixture was corrected,
  both new wire tests passed in isolation, and the remaining tests then ran to
  completion. That aborted process is not counted as a successful full run.
- ACE server network/VR selection: 84 passed, zero failed/skipped
  (`server-focused.log`, `server-tests/network-and-vr.trx`).
- Quest shared-source synchronization and `-CheckOnly` passed. This verifies
  source consistency, not an Android package or physical-headset playtest.

## Sources and comparison method

- Retail decompile: `C:/dev/retail_client_source`. Cite the actual class/function
  when changing behavior; decompiler variable names and inferred types are not
  specifications by themselves.
- Runtime assets: `C:/Turbine/Asheron's Call/client_*.dat`.
- Independent data decoder: `Source/ACE.DatLoader`, with fixture exporters in
  `Unreal/diag/UIInventory`. Resolved interface trees are in `Docs/UI/Resolved`.
- Protocol counterpart: `Source/ACE.Server/Network` and server world objects.
  Do not alter server rules to compensate for a client discrepancy.
- User reproduction: `Unreal/Saved/Logs/ACUnreal-user-20260906-afternoon.log` and
  the supplied screenshots. Reported Yaraq rooms include 7D64012C, 7D640131,
  and 7D640135. Preserve these fixtures when changing culling or streaming.

For each feature, compare retail data and control flow, implement the behavior,
verify runtime outcomes with independent fixtures, then exercise the live server
flow. Record any part that has not been exercised. Automated coverage and live
acceptance are distinct evidence.

## Changes in 2026.09.12.2

- The local walking radius now comes from the lower authored Setup sphere, as
  used by retail `CPhysicsObj` when initializing `CTransition::init_sphere`.
  Human setup 02000001 has two 0.48 AC spheres; the enclosing Setup radius was
  previously clamped to 0.55 AC and used for walking. This oversized body
  snagged the final Arcanum stair riser. Use-radius calculations still use the
  enclosing Setup radius, matching `CPartArray::GetRadius`.
- A validated stair descent retains the actual sphere contact above a tread's
  center-ray height. A radius bound and the step-up limit prevent substituting
  the top of a tall wall. Ground support checks use the walkable normal threshold
  (FloorZ), while airborne landing keeps LandingZ. Cell transit now starts at
  the actual capsule center, without adding its half-height a second time.
- Rising ramp support samples the lower sphere's clearance rather than placing
  its bottom on the center ray. A walkable collision re-sweeps the remaining
  motion along that plane so the first ramp cannot hide a later wall. Final
  downward ground adjustment also sweeps the full capsule, preventing the next
  tread's ray height from pulling the body through the landing lip. The contact
  normal, including its Z denominator, is used consistently at rounded edges.
- `/pkl` already sent EnterPkLite (028F). The missing path was public quality
  134 (02CE), used by `Player::HandleActionEnterPkLite` on completion. Both public
  and private updates now update self stats and the PK/PKL/Free descriptor flags
  exactly as `ACCWeenieObject::OnStatUpdated` and
  `PublicWeenieDesc::SetPlayerKillerStatus`. Existing actor collision updates
  through a status-only event, without restarting motion or appearance.
  Open inspection, Attributes, and selection/radar colors read the new state.
  The server still decides entry eligibility and damage; no predicted PKL toggle
  or server-rule changes were introduced.

Regression coverage uses the actual Arcanum rooms 7D640149-14F and staircase
02000A4A, with the player capsule derived from its Setup. It traverses both
directions at center and near both walls with 16, 33 and 50 ms frame intervals,
checks airborne state and body overlap,
holds forward against a roof edge, and reverses away without jumping. The previous
stair fixture used an unrealistically narrow 22 cm radius; its earlier passing
result did not cover the reported player. Protocol tests inject public/private
PK updates and transitions back to NPK/Free; UI tests keep an old NPK appraisal
open while a PKL broadcast changes its heading and selection/radar color.

Validation: editor LedgeStairs, RoofEdges and RuntimeActors pass in
`Saved/Logs/Test-2026.09.12.2-final.log`; InteriorStreaming, PkStatus and UIScreens
pass in `Saved/Logs/Test-2026.09.12.2-ramp.log`. BuildCookRun completed successfully
in `Saved/Logs/Package-2026.09.12.2.log`. The packaged executable passes
LedgeStairs, PkStatus, UIScreens and Packaging.Materials in
`Saved/Logs/Test-2026.09.12.2-packaged.log`. The package lacks Unreal's HTML report
template; this does not prevent running the tests or recording their results.

Live multiplayer damage and the user's exact movement path still require a
server playtest; automated fixtures are not evidence of a completed live session.

## Changes in 2026.09.12.1

- Loaded indoor cells no longer manufacture a floor when support is absent.
  Airborne collision recovery handles existing wall/soffit penetration and
  re-sweeps before landing; trying to jump while falling cannot charge a jump.
- Missile aim/reload and eating/drinking use their timed Ready-to-action links,
  not their stationary endpoint cycles. Unchanged object-descriptor updates
  cannot replay a completed action or duplicate its followup queue. Actual
  appearance rebuilds still restore the latest motion.
- Stable hand attachments no longer reset/recreate picking collision every
  tick. Part collision no longer unconditionally recreates its physics state;
  redundant appearance physics application is removed. Radar reads the session
  object view without copying every object's properties/clothing. Appearance
  queue sorting copies each candidate once, and ordinary pose updates avoid
  copying the complete transform array unless a transition needs blending.
- Console/debug key bindings are cleared, including inherited saved bindings.
  GameplayDebugger activation is disabled. Backtick remains the game's combat
  toggle; semicolon/apostrophe remain enemy-target controls.
- Escape dismisses popups, keyboard mapping, inspection, vendor/loot/trade/
  salvage, then the main panel. Once clear, it opens Options on Game Play,
  regardless of the previously selected settings tab; another press closes it.
- Dropping a pack on a vendor stages its full, sellable content stacks on Sell.
  The pack itself, retained/attuned items, and equipped contents are omitted.
  Repeated drops update quantities without duplicate entries or auto-selling.
  The main backpack is also a bulk drop source. Retail `gmVendorUI::AddItem`
  expands contained items and applies `VendorProfile::InqAcceptability`:
  preserve vendor type/min/max fields from ApproachVendor and filter per-unit
  values (including the promissory-note maximum-value exception).
- Pack slots paint retail's empty image (06000F6E), at the authored 32x32 inset
  in each 36x36 slot, up to the player's pack capacity. Occupied pack icons
  retain their item-type background and custom underlay; the main pack uses
  Container type as in `IconData::RenderIcons`. This also restores backgrounds
  for packs inside external containers. Empty-state evidence is DAT layout
  21000037, ItemSlot_Backpack/ItemSlot_Icon, state 1000001C, together with
  `UIElement_ItemList::UpdateEmptySlots` and `UIElement_UIItem::UIItem_SetState`.
  Removing, scrolling, or hiding a pack clears its former artwork; cached pack
  overlays now also react to type, underlay, effect, and player capacity changes.

Evidence: seven suites passed in `Saved/Automation/2026.09.12.1-review`:
InteriorStreaming, LedgeStairs, MovementReview, RoofEdges, RuntimeActors,
UIScreens, and WorldEntry, with actual D3D12 rendering enabled. LedgeStairs
loads Arcanum cells 7D640149-14F, including staircase setup 02000A4A, and walks
the controller from the basement onto the main floor. Missile actions cover
the human bow, crossbow, and thrown stances; food/drink must finish while idle.
Object echo tests verify no mesh rebuild or completed-action restart.
The final UIScreens run in `Saved/Automation/2026.09.12.1-vendor` verifies
actual side-pack and main-backpack drops, repeat drops, no automatic sale, and
vendor type/value filtering. An additional LedgeStairs run in
`Saved/Automation/2026.09.12.1-input`
verifies console/debug key removal even after injecting old saved bindings.
The final UI run in `Saved/Automation/2026.09.12.1-packs-final` also verifies
three occupied packs plus four default empty slots, main-pack type override,
underlay/type refresh, removal, augmented-slot scrolling, inventory reopen,
and nested-container background updates. Screenshots are in
`Saved/Automation/RetailParity/GameplayPackBackgrounds.png`,
`GameplayEmptyPackScroll.png`, and `GameplayLootPackBackground.png`.
The final editor run (`Saved/Automation/2026.09.12.1-final`) passes LedgeStairs,
MovementReview, and UIScreens. The built Windows executable also passes those
three suites plus Packaging.Materials, with zero warnings/errors, in
`Saved/Automation/2026.09.12.1-packaged-verified`. Test worlds explicitly bind
their owning game instance so runtime collision cooking receives a game world;
UI fixtures use explicit resize dimensions and absolute command-file paths,
independent of saved editor preferences and packaged relative directories.
BuildCookRun completes successfully (`Saved/Logs/Package-2026.09.12.1.log`).

Performance limits: the user screenshots show ~26-27 ms Draw versus ~5 ms GPU,
with 10,350-11,641 draws. This points to render-thread submission cost outdoors,
not lack of GPU capacity. The engine's procedural mesh proxy submits dynamic
sections separately, including shadow views. No terrain quality, visibility
distance, or shadow quality reduction is included. The focused 1,000 held-item
tick benchmark measured 0.118 ms versus 52.826 ms for the prior repeated
collision path; this is not a live crowded-scene FPS measurement. The roof,
stairs, crowded multiplayer scene, and full live consumable flow still require
user acceptance in the new executable. The captured user log was build
2026.09.11.5, predating the preceding roof recovery changes in .6.

## Changes in 2026.09.11.6

- Cold terrain workers share immutable TexMerge blends by PCode and DAT
  fingerprint. They consult the existing tile cache before baking and compress
  each missing blend once. Visible landblocks share pixel storage; unused recent
  blends have a 128 MiB retention limit. Worker count, geometry, texture quality,
  reveal gates, and the rollback of the precompiled world bundle are preserved.
- Roof overlap recovery uses the same LandingZ threshold as clean airborne
  contacts. It re-sweeps after recovery before accepting touchdown. Forced server
  corrections also release standing-jump locks and stale charge/launch state.
- Login remains visible until character selection is ready. Missing DAT and
  packaged UI assets produce an actionable error, instead of a black screen and
  per-frame retries. Login explicitly retries a failed index. DatDirectory now
  honors the documented Engine.ini setting.

Evidence: the preserved user logs show 65.35s and 80.4s world transitions,
mostly waiting for visible terrain. The two-worker cold CPU bake benchmark
improved from 10.301s to 2.909s at 2B120000 and 12.111s to 6.383s at 7E650000.
Raw-reference RGBA comparisons passed. A separate cold runtime test at 7E650000
completed all 121 terrain meshes in 21.490s, including DAT indexing, disk writes,
mesh application, and render flush. Its 1,209 sections shared 179 blends
(716 MiB, versus 4,836 MiB if each section owns its pixels). This test does not
measure live login, building/scenery completion, or steady-state FPS.

RoofEdges reproduced three stuck overlaps on the old code (50/60/75 degree
roofs) and passed after correction, along with the existing thin roof, corner,
ceiling, and wall cases. WorldEntry, MeshApplication, YaraqScene (130 rendered
views), RuntimeActors, LocalLoginSettings, and MissingDatLogin passed. The
missing-DAT test verifies visible errors, suppressed retries, explicit retry,
and recovery to valid character-selection resources. Initial missing-file test
output required declaring both expected loader error messages; the final run is
in `Saved/Automation/2026.09.11.6-login`.

Editor build and Windows BuildCookRun succeeded. Packaged Materials,
MeshApplication, MissingDatLogin, and Weather passed in
`Saved/Automation/2026.09.11.6-packaged`. The packaged automation runner lacks
its optional HTML report template; JSON and logs were written successfully.
Weather retains the existing ScreenPercentage priority warning. Live roof-jump,
full login duration, and the friend's installation still need user verification.
Distributable: `Saved/Packages/2026.09.11.6/Windows/ACUnreal.exe`.

## Changes in 2026.09.11.5

- Reverts the 2026.09.11.4 world-cache update after a reported performance
  regression. Restores prior DAT reads, cache identity, terrain baking and
  static model loading. The precompiled bundle is no longer staged.
- All fixes through 2026.09.11.3 are retained.

Validation: editor build and Windows BuildCookRun succeeded. WorldEntry,
MeshApplication and YaraqScene passed in `Saved/Automation/2026.09.11.5-check`;
WorldEntry emitted its three expected blocked-entry/recall warnings. Packaged
Materials, MeshApplication and Weather passed in `2026.09.11.5-packaged`, with
the existing ScreenPercentage priority warning in Weather. The package listing
contains no `ACE/WorldCache` entries. Live performance has not been remeasured.
Distributable: `Saved/Packages/2026.09.11.5/Windows/ACUnreal.exe`.

## Changes in 2026.09.11.3

- Empty spell hotbar slots 1–9 use the blue numbered DAT artwork
  (`0x060010FA`–`0x06001102`) shown in the user's retail comparison. Later empty
  slots remain brown (`0x06001A97`). Occupied spells retain their small key
  badges, and scrolling keeps bindings and artwork tied to absolute slots.

Validation: both rendered UI groups passed without warnings in
`Saved/Automation/2026.09.11.3-ui`. Captures `GameplayEmptyNumberedSpellBar.png`
and `GameplayShortSpellBar.png` in that folder confirm the blue 1–9 artwork,
brown later cells, and occupied-spell badges. Existing scrolling/key checks also
passed. Editor build and BuildCookRun succeeded. All three packaged smoke checks
(Materials, MeshApplication, Weather) passed in `2026.09.11.3-packaged`, one with
the existing scalability warning. Generated runtime settings/logs were moved to
`2026.09.11.3-packaged-runtime` under Automation.
Distributable: `Saved/Packages/2026.09.11.3/Windows/ACUnreal.exe`.

## Changes in 2026.09.11.2

- Empty item shortcuts 1–9 again use their blue numbered DAT artwork. This
  initially missed the spell bar's empty slots, corrected in 2026.09.11.3.
  Scrolling still leaves spell keys pinned to absolute entries 1–10.
- Chess draws all 32 pieces using DAT icons instead of unsupported Unicode
  glyphs. Its panel is titled Game Center, with the retail Resign and Stalemate
  controls and selection art.
- The small pack/burden button opens Character Information. Its scrollable
  content includes birth, time played, deaths, resistance/regeneration categories,
  innate attributes, chess/fishing ranks, weapon and summoning masteries,
  augmentations, and carrying burden.
- Outdoor world entry waits for terrain coverage across the configured load
  radius before revealing the world. Outer scenery can continue streaming;
  the indoor cell/neighborhood readiness path is unchanged.
- Bitmap text ignores nonprinting CR/BOM/zero-width characters instead of
  painting fallback question marks. Inscription editing uses the same DAT glyph
  atlas as the display, including multiline editing, selection, and clipboard.
- Spellbook selection draws the template's `ItemSlot_Icon_Selected` image over
  the selected row. Components use their authored header, row, count, and numeric
  entry templates. Desired counts are typed directly, clamped to 0–5000, and
  persisted through SetDesiredComponentLevel. The default list shows carried or
  requested components.
- `/fillcomps` fills the vendor buy list with deficits after counting all carried
  stacks and existing cart quantities. Category filters, maximum cost, finite
  stock and `clear` are supported. Purchase remains on Buy All. Vendor supply
  quantities are retained from the ApproachVendor packet.
- Miscellaneous command support includes `/age`, `/birth`, `/pklite` (`/pkl`),
  `/day`, `/endurance`, `/framerate`, `/loc`, `/version`, `/loadfile`, `/log`, and
  auxiliary-window `/title`, plus `/help <command>` and `@` equivalents. Age and
  birth use the dedicated string-payload actions; the age reply is displayed.
  Command files expand `%DATE%`, and log output appends until `/log` stops it.

Reference: retail `gmSpellComponentUI`, `gmVendorUI`, `gmCharacterInfoUI`,
`ClientCommunicationSystem`, DAT templates, and ACE server handlers. Validation
uses isolated loopback protocol fixtures and rendered UI; a live vendor/chess
session and real login/portal timing still need user acceptance.

Validation: all 36 `ACE.RetailParity` tests passed (32 clean, four with known
fixture/scalability warnings) in `Saved/Automation/2026.09.11.2-check`. After the
final title, sizing, input routing and command-help changes, both UI groups passed
in `2026.09.11.2-ui-focus`. Captures verify all 32 chess icons, component entry
editing, spellbook selection, blue shortcut slots and the Character Information
scroll area. Typed characters, Enter, and Escape were routed through a registered
Slate window; refill accounting and miscellaneous command packets used loopback.
The final BuildCookRun succeeded without header-order diagnostics after moving
each affected source's own header to its first include. Packaged Materials,
MeshApplication and Weather all passed in `2026.09.11.2-packaged`. The packaged
runner lacks Unreal's optional HTML report template; its JSON results are intact.
Distributable: `Saved/Packages/2026.09.11.2/Windows/ACUnreal.exe`. Generated
smoke-test settings/logs were moved to `Saved/Automation/2026.09.11.2-packaged-runtime`.

## Changes in 2026.09.11.1

- Spell keys 1-0 and their badges refer to absolute entries 1-10 even while the
  spell list scrolls. Item shortcuts select on a single click and use/equip on a
  double click; numeric activation stays immediate. Blank item shortcuts use the
  inventory slot background with a separate numeric overlay.
- The vendor uses its DAT dropdown arrow and a complete row of default empty
  slots. Inventory foreground composition adds the retail for-sale overlay to
  items queued in the vendor sell list.
- Effect, harmful-effect and vitae captions occupy their authored title bars.
  Runtime labels, menus, tooltips and legacy widget fallbacks use the shared DAT
  bitmap font resolver. Maxed trained/specialized skills display `Infinite` in
  the XP-to-raise footer.
- Escape closes inspection, then the active equipment/panel window, before
  opening gameplay options. A late identify reply no longer reopens a dismissed
  examination. Right-clicking an equipped item in the 3D doll sends Identify.
- Game boards are recognized by their public Gameboard item type and are not
  discarded as stuck non-usable scenery. Their interaction approaches the board
  and sends Use, allowing the server's JoinGame response to start chess.
- A corpse's Dead animation no longer makes its container a dying creature.
  Selection, markers and approach/use remain available. Actual dying creatures
  retain the prior smite protection. Missile/particle objects are excluded from
  targeting, and cosmetic emitter meshes never intercept selection traces.
- Particle lights are hidden when the last emitter expires or stops. A server
  projectile impact (NoDraw/Cloaked) stops extrapolated flight and the launch
  effect before the Explode effect. The server decides terrain collision and
  damage; the client no longer displays a spent projectile continuing to fly.
- Optional mouse controls follow the documented WoW distinction: left-drag
  orbits freely, right-drag faces the character to the view, both buttons advance.
  Forward keyboard movement retains a free-orbit view. Left click selects.
  With mouse turning disabled, mouse orbit stays disabled and wheel zoom works.
  Brackets cycle nearby selectable world objects; semicolon/apostrophe cycle
  nearby attackable enemies.
- Map hover uses the retail 53 named location rectangles and the original
  257x267 aspect ratio. Housing uses readable DAT text, wrapped purchase/rent
  descriptions, Bought/maintenance/location labels, and the retail extra rent
  period when the current period is already paid.
- Components use the 32-pixel DAT item row, separate count/desired columns, and
  a draggable scrollbar. Inventory/list scrollbar drags preserve the point
  grabbed on the thumb and its continuous position during content refresh.
- Config now scrolls through functional sound, camera, graphics and individual
  rendering-quality controls, using retail arrows, slider art and round toggles.
  Chat opacity sliders remain visible above its window/filter options.
  Master/effect/ambient gain, inactive-window audio, camera stiffness/adjustment,
  field of view, brightness and anisotropy persist locally.
- Scripted sky/cloud meshes receive the same current time-of-day ambient/sun
  parameters as other sky layers. They retain authored luminosity rather than
  inheriting the ordinary particle visibility fallback.
- Luminosity no longer forces a clipped sign into additive blending. World
  materials preserve authored luminosity independently of ambient-light updates;
  this applies to the small streetlamps in Yaraq (the user's clarification),
  including the luminous 0x0800012D surface. Outdoor material parents advance to
  v3 and are regenerated for cooking.
- The vertex-color fallback has its own cooked shader instead of depending on
  an editor-only EngineDebugMaterials asset. The runtime parent set now has 32
  materials. The module build stamp matches the project version.

Validation: the final full D3D12 offscreen run passed 35 groups; its one outdated
camera-release assertion was corrected for left-drag orbit and the camera group
then passed separately. All 36 groups have passing results. Added checks cover absolute spell binding, single/double/numeric item
activation, corpse death-pose selection, chess/corpse use approach, components
thumb dragging, town hover, maxed skill text, config scrolling, chat sliders,
cloud lighting and lamp luminosity. Editor build and material preparation succeed.
BuildCookRun succeeds with exit code 0. The final packaged executable passes
`ACE.Packaging.Materials` (all 32 cooked shader parents),
`ACE.RetailParity.MeshApplication`, and `ACE.RetailParity.Weather` with zero
failures. The only packaged-test warning is the deliberate 100% screen-percentage
fixture override taking priority over scalability. The prior missing vertex-color
material warning is gone. Reports: `Saved/Automation/2026.09.11.1-final`,
`2026.09.11.1-camera`, and `2026.09.11.1-package-final`. The distributable is
`Saved/Packages/2026.09.11.1/Windows/ACUnreal.exe`; generated smoke-test Saved
files were moved outside that folder.
The optional mouse-control reference is Blizzard's [WoW guide](https://media.battle.net/documents/wow/WoW-BradyGAMES-enUS-Guide.pdf)
and [classic manual](https://us.media.blizzard.com/manuals/wow/wow-classic-manual-enUS.pdf).
The authored interface/data references are `gmMapUI::s_rgLocations`, `gmHouseUI`,
`HousePayment::ComposeText2`, `ItemHolder::DetermineUseResult`, and
`classic_spellcomponent` (0x21000033). Projectile termination follows the
server's `ProjectileImpact` state transition.

Live server chess joins, combat damage, the specific Arcanum sign and exact sky
appearance at the user's server time still require in-game comparison; automated
fixtures are not a claim of full live retail parity.

## Changes in 2026.09.10.5

- Moving particle sprites use a 1.8 linear output gain, while the stationary
  glow returns to unity. The accepted DAT emission, twinkle, birth frames and
  running trails are preserved. Particle material version 7 is regenerated for
  cooking. Weather particles retain unity gain. No particle-light intensity
  was increased.
- All five chat windows use the DAT edge resize flags and width/height limits.
  Saved widths are restored alongside heights. Text rows reflow when their
  allocated width changes, including offscreen rows. Scroll input, both arrow
  buttons, and the segmented blue thumb work in each window; incoming messages
  preserve a scrolled-back reading position. Opacity applies to the message
  backdrop without blending each scrollbar layer into the gold track.
- Escape toggles the options/game menu from gameplay or a focused panel control.
  Text editing and key-binding capture retain their existing Escape handling.
- Numbers-off vitals propagate the authored ShowDetail state to the textured
  fill/background. Numbers-on uses HideDetail. Both stacked and horizontal
  layouts are covered, for health, stamina and mana.
- Airborne capsule sweeps recover an initial upward support penetration on a
  walkable sloped roof. Previously that overlap could hold the airborne state
  indefinitely and prevent manual movement. Recovery still rejects walls and
  soffits. RoofEdges covers launch, landing, thin edges/corners and ceilings.
- UE mouse smoothing is disabled in DefaultInput.ini. The supplied log's
  `SampleCount > 0` ensure originates in that engine smoother; the client already
  integrates raw mouse deltas through its own camera controls. The log alone
  does not establish the cause of the reported roof freeze.
- World occupants and their attached weapons follow the terrain presenter's
  admitted EnvCell visibility. Hidden surface-level dungeon rooms therefore
  hide their occupants, suppress picking/collision and radar dots, and clear
  selection. Appearance and attachment refreshes cannot reveal a hidden actor.
  Admitted rooms and exterior portal views restore visibility normally.
- Server Dead motion is retained on the object record, including deferred
  appearances and repeated descriptions of the same incarnation. A late Ready
  update cannot cancel a slain NPC's death pose. Dying creatures stop being
  attack targets/radar dots and release selection. A new incarnation may become
  attackable again; player revival remains allowed. `WeenieError::TargetNotAcquired`
  (0x042C) now displays readable text. Basis: server `Creature::Die` / `Smite`,
  `GameMessageUpdateMotion` and `WeenieError`.

Editor build succeeded. All 36 RetailParity tests pass (32 clean, four with the
existing fixture warnings), including 32 smite-shaped motion/removal sequences,
real creature death animations, room/occupant/weapon visibility transitions,
roof support overlap and rendered chat/vital/particle checks. Evidence:
`Saved/Automation/2026.09.10.5-final/index.json` and
`Saved/Logs/Test-2026.09.10.5-final.log`. The roof and admin-smite reports have
automated regression coverage; the user's exact live incidents have not been
reproduced in this pass. Live acceptance remains distinct from those checks.

BuildCookRun succeeded, including cooking the version 7 additive parent and
archiving `Saved/Packages/2026.09.10.5/Windows/ACUnreal.exe`. Packaged
MeshApplication and Weather tests pass with their existing fixture warnings
(`Saved/Automation/2026.09.10.5-packaged/index.json`). The separate
`ACE.Packaging.Materials` check passes cleanly and verifies all 31 persistent,
compiled runtime parents (`Saved/Automation/2026.09.10.5-packaged-materials/index.json`).
Generated package test profiles/logs are moved out of the release archive.

## Changes in 2026.09.10.4

The user confirmed the particle emission and running trails now behave as
expected, but requested more brightness. Additive particles now apply a 1.35
linear output gain after the existing display-space source modulation. This is
a requested visibility adjustment, not a change to the DAT brightness rules.
It preserves sprite color ratios and fade endpoints without clipping before
the gain. Emission, random spread, lifetime, motion, and ordinary alpha-blended
particles are unchanged. The additive parent is versioned to v6 for cooking.

The rendered `ACE.RetailParity.ParticleTiming` test passed, including the moving
baton lifecycle regression and pixel checks of both the boosted source and
unity-gain retail modulation, texture alpha, fading, and front/back brightness.
Its two warnings are the existing deliberately unresolved effect fixtures.
Evidence: `Saved/Automation/2026.09.10.4-particles/index.json`. Editor build and
BuildCookRun succeeded; the archive is `Saved/Packages/2026.09.10.4/Windows`.
Packaged Materials, MeshApplication, and Weather checks also passed (one clean,
two with the existing debug-material/scalability warnings); evidence is
`Saved/Automation/2026.09.10.4-packaged/index.json`.
Live visual acceptance of the new brightness remains to be confirmed.

## Changes in 2026.09.10.3

The follow-up Slashing Baton report exposed a lifecycle fault that the previous
isolated shader test did not cover. `TryAttachToParent` calls
`NotifyAppearanceReady` when refreshing held equipment. That notification was
overwriting **every live particle's** random offset with the exact hook origin
and moving its birth frame to the current weapon. `InitializeFromObject` also
reseeded the random stream on every property refresh. Together these collapsed
the drip into one line and pulled surviving particles along with the runner.

- Appearance readiness now rebinds emitter parents without rewriting existing
  particle offsets, birth frames, ages, or emitter travel history. Future births
  use the current hand/weapon pose; non-parent-local particles keep the saved
  birth frame as in retail `ParticleEmitter::UpdateParticles` and
  `Particle::Init`/`Update`.
- An object's random stream is seeded once for its identity, not restarted by
  property updates. Existing material/fade fixes and DAT emission rates, sizes,
  motion, and lifetimes remain in use.

The regression uses the actual baton setup attached through the presenter to a
DAT character moving at 4 m/s while refreshing the weapon's state/attachment.
Before the fix it failed radial-offset, birth-frame, trail, and random-diversity
checks, producing one distinct offset over five seconds. After the fix it
produces 21 distinct offsets in the authored 10–20 cm annulus, retains the
world-space gravity trajectory and remaining lifetime, leaves stars behind the
moving tip, and drains them after emission stops. Evidence:
`Saved/Automation/2026.09.10-baton-motion-baseline/index.json` and
`Saved/Automation/2026.09.10-baton-motion/index.json`. This verifies the
simulation/lifecycle correction; final live visual comparison remains distinct.

Validation: all 36 `ACE.RetailParity` tests passed (32 clean, four with the
existing fixture/scalability warnings) in
`Saved/Automation/2026.09.10.3-full/index.json`. Editor build and BuildCookRun
succeeded. Packaged Materials, MeshApplication and Weather passed (one clean,
two with the existing debug-material/scalability warnings) in
`Saved/Automation/2026.09.10.3-packaged/index.json`. The archive is
`Saved/Packages/2026.09.10.3/Windows/ACUnreal.exe`.

## Changes in 2026.09.10.2

The reported Slashing Baton is setup `02001491`, script `33000F6B`. Its DAT
contains a one-particle glow (`32000803`, surface `080002EC`) and a five-particle
star drip (`32000802`, surface `08000309`): one star every 0.2 seconds, one-second
lifetime, 0.1–0.2 AC unit radial offsets, and downward world acceleration. Both
layers have 0.7 translucency. The independent decoder in `Unreal/diag/BatonFx`
confirms these parameters and exports the actual glow and eight-point star
textures. No substitute artwork or weapon-specific emission multiplier is used.

- Particle materials now cull back faces because `ACEPolygonMeshBuilder` already
  emits the authored reverse-facing polygons. Previously both copies rendered
  from either side. A rendered white-card test measured twice the intended
  additive brightness; the corrected result is one contribution from either view.
- Additive particles apply their source color/fade in display color space before
  conversion for Unreal's linear framebuffer. Retail `D3DPolyRender::SetSurface`
  and `RenderDeviceD3D` use fixed-function modulation/blending with sRGB sampling
  and writes disabled. Linear-space 0.3 opacity made the broad glow too bright.
- Sprite textures follow `ImgTex::InitHardwareTexture`'s four-mip limit and
  `RenderDeviceD3D`'s linear min/mag, point mip selection. The additional 2x2/1x1
  levels and trilinear mip blending previously softened small stars into blobs.
  Terrain and other texture callers retain their full mip chains.

`ACE.RetailParity.ParticleTiming` now includes the exact baton, eight rendered
frames, its persistent star population, and twelve brightness checks across
front/back views, emitter fade, and texture alpha. The shader contribution is
verified against an independent sRGB formula on a black background. Unreal
still composites into a linear scene; this is not a claim of identical retail
gamma-space blending over every background. Live visual acceptance of the wand
and other rending effects remains necessary.

Validation: editor build and BuildCookRun succeeded. All 36 retail-parity tests
passed (32 clean, four with existing fixture/scalability warnings), recorded in
`Saved/Automation/2026.09.10.2-full/index.json`. The packaged Materials,
MeshApplication and Weather tests passed (one clean, two with the existing
uncooked debug-material fallback/scalability warnings) in
`Saved/Automation/2026.09.10.2-packaged/index.json`. Rendered baton frames are in
`Saved/Automation/BatonFx`. Package:
`Saved/Packages/2026.09.10.2/Windows/ACUnreal.exe`.

## Changes in 2026.09.10.1

- Inspection resets hidden-part state when reusing its capture rig. A creature
  with fewer or null parts no longer leaves the next subject's head hidden.
  The model is posed before camera fitting, UI previews bypass world-distance
  animation culling, and closed inspection stops animating. The rendered test
  includes an Undead head replacement after a Shreth preview.
- Bitmap chat reserves its actual wrapped height in main and auxiliary windows.
- Ordered UDP recovery handles and retries single-packet gaps. Manual movement
  cancels stale client approach/use-wait state. The reported marketplace freeze
  has not been reproduced; these are verified recovery-path fixes.
- Particle replacement retains matching pooled render proxies, effect lights
  update with simulation, and held effects follow the current animation pose.
  The real DAT wand/pillar fixtures pass; live rending appearance remains to be
  compared.
- Idle movement ticks, unchanged portrait captures, repeated terrain material
  updates and premature DAT-cache cleanup were removed. `stat ACE` and Insights
  scopes expose the remaining work. See
  [the performance review](PERFORMANCE_REVIEW_2026-09-10.md) for evidence,
  remaining costs and measurement limits.

Validation: all 36 `ACE.RetailParity` tests passed (32 clean, four with warnings,
zero failed/skipped). The editor build and BuildCookRun succeeded. The packaged
Materials, MeshApplication and Weather tests also passed (two with warnings).
The archive is
`Saved/Packages/2026.09.10.1/Windows/ACUnreal.exe`. See the review for the complete
test-report paths and live acceptance limitations.

## Changes in 2026.09.09.13

- Creature inspection chooses the character or creature header using retail's
  profession/display-title presence test, including NPC character profiles.
  Gender and heritage use their numeric appraisal enums; display titles fall back
  to profession when unknown. PK/PK Lite use the actual object flags. Inspected
  character names include the allegiance rank title for their heritage/gender.
- The nine attribute/vital rows retain the authored font, spacing, highlight
  colors and partial-appraisal health percentage. Character details use the
  actual two-column ExtraInfo list and row template, including monarch/patron,
  society rank, three body-armor groups, ratings, fellowship, birth/age, chess,
  fishing, deaths, title count and armor legend. Creature profiles retain their
  rating rows without inheriting character-only fields. The list scrolls when
  needed, grows with the frame and preserves its scroll position on refresh.
- Inspection portraits fit the subject's visible DAT mesh bounds with retail's
  heading and vertical field of view. The camera stays independent of object
  scale, and the capture composites its transparent background correctly. Editor
  camera visualization geometry is excluded from both the portrait and bounds.
  A missing appearance is retried instead of caching an empty preview.

Sources: retail `gmExaminationUI::RecvNotice_AppraisalInfo`,
`BasicCreatureExamineUI::Init`/`AddLineToMiscInfo`,
`CharExamineUI::SetAppraiseInfo`, `CreatureExamineUI::SetAppraiseInfo`,
`AttributeInfoRegion`/`Attribute2ndInfoRegion`, `AppraisalSystem`,
`AllegianceData::GetFullName`, `AllegianceSystem::GetTitle`, and
`ClientUISystem::DeltaTimeToString`. Layout/font evidence: DAT 2100006B and its
2100001C/10000337 detail-row template.

UIScreens covers character/creature switching, headers, property mappings,
partial appraisal, scrolling and resizing, rank/title fallback, and actual
rendered model coverage/framing at different scales. These checks use fixture
appraisals and real DAT models. Live identification of players, NPCs and enemies
still requires gameplay acceptance; the fixtures do not prove full retail parity.
Society relationship colors and periodic combat appraisal polling remain outside
the current verified coverage.

Validation: editor build succeeded. The full `ACE.RetailParity+ACE.Packaging`
suite passed 36 tests (32 clean, four with warnings, zero failures or skipped),
recorded in `Saved/Automation/2026.09.09.13-final/index.json`. Warnings concern
deliberate world-entry failure fixtures, unresolved effects in the particle
fixture, and saved console settings taking precedence over test scalability.
The final rank-title and legend changes passed UIScreens again in
`Saved/Automation/2026.09.09.13-rank/index.json`. BuildCookRun completed with
exit code 0 and archived `Saved/Packages/2026.09.09.13/Windows/ACUnreal.exe`.
The packaged executable passed Materials, MeshApplication and Weather (three
tests, zero failures, two with warnings). MeshApplication could not load the
optional engine debug VertexColorMaterial and used WorldGridMaterial; Weather
reported console-setting precedence. Results:
`Saved/Automation/2026.09.09.13-packaged/index.json`. UIScreens requires the
editor context, so packaged smoke tests do not repeat its inspection assertions.
Test-generated package settings/logs were moved out of the distribution to
`Saved/Automation/PackagedRuntime-2026.09.09.13`.

## Changes in 2026.09.09.12

- Walking off an outdoor ledge enters the same swept ballistic movement used by
  jumps, without sending a Jump action. Prediction and position reporting continue
  with no keys pressed. Idle reconciliation includes altitude; ForcePositionSeq
  corrections apply the server's full position without invoking portal placement.
  Landing expires the pre-contact server anchor so idle reconciliation cannot
  pull the character back up. WorldEntry covers an idle 30 metre fall with that
  stale anchor present, landing, and vertical server corrections.
- Right-clicking a side pack identifies the pack. Both trade offer lists now allow
  selecting and identifying their objects, with the retail selected-slot frame.
  Trade errors 044C through 0450 have readable messages. Protocol 044C means ignored
  requests; 044E means the trade approach/distance check failed, not trade blocking.
- Item inscription paper accepts editing for owned inscribable items, including
  nested inventory and equipped objects, when the scribe permits it. Focus loss
  sends SetInscription (00BF), including an empty string to erase text; Escape
  cancels. The client reads the appraisal back because ACE has no writing ack.
  Existing inscriptions show their scribe. Source: retail ItemExamineUI focus and
  editable-state handlers, ACE GameActionSetInscription and Player_Inventory.
  UIScreens covers pack/trade clicks and multiline writing, clearing, cancellation,
  another scribe's read-only text, and the emitted packet fields.
- Portal resource prefetch preserves the existing gameplay DAT tree when reusing
  the binder. This keeps inventory chrome and its overlays on the same panel tree.
  UIScreens checks panel identity and visibility after prefetch.
- Held objects stream against their parent's network position even before the
  parent actor has spawned. Residency restoration includes attached objects.
  InteriorStreaming covers either arrival order, deferred attachment, actual
  Weeping Wand geometry, and restoring the weapon when its player returns.

Live cliff/portal/trade exchanges and the reported remote player loadout still
require gameplay acceptance. Automated checks do not establish full retail parity.

Validation: editor build succeeded. The full `ACE.RetailParity+ACE.Packaging`
suite passed 36 tests (32 clean, four with warnings, zero failures or skipped),
recorded in `Saved/Automation/2026.09.09.12-final/index.json`. The final landing
change also passed WorldEntry and CameraAndEdges in
`Saved/Automation/2026.09.09.12-landing/index.json`. Warnings concern deliberate
world-entry failure fixtures, unresolved effects in the particle fixture, and
saved console settings taking precedence over test scalability settings.
BuildCookRun completed with exit code 0 and archived
`Saved/Packages/2026.09.09.12/Windows/ACUnreal.exe`. The packaged executable
passed Materials, MeshApplication and Weather (three tests, zero failures):
`Saved/Automation/2026.09.09.12-packaged/index.json`. The packaged runner lacks
the optional HTML report template; JSON results were exported successfully.
Test-generated package settings/logs were moved out of the distribution to
`Saved/Automation/PackagedRuntime-2026.09.09.12`.

## Changes in 2026.09.09.11

- Remote players reject stale position/teleport stamps and motion from a different
  instance. Extrapolation now uses a 50 cm minimum drift allowance, replacing the
  accidental 50 AC-unit (50 m) floor. Runtime tests cover stopped remote actors,
  equal elapsed time at different frame rates, and a subsequent server position.
  This addresses the position discrepancy reported for Stormbringer II; the exact
  live two-client portal scene has not yet been repeated.
- Terrain keeps its 128 authored triangles at every distance. Coarsening a
  landblock's 8x8 grid changed the ground elevation beneath fixed buildings.
  Distance transitions no longer rebuild meshes or recook collision. All requested
  blocks are queued nearest first to the bounded workers, with a 4 ms application
  budget and faster expansion. Radius 5 still means an 11x11 region (121 blocks),
  needed for the selected viewing distance, not 121 simultaneous synchronous loads.
- Sky particle children inherit their celestial slot's rendering order instead
  of drawing over every star/cloud layer. Background sky materials only shade
  clear-depth pixels, matching retail's before-world pass even where a large cloud
  card intersects distant terrain. Long-lived cloud emitters retain their authored
  3300 second lifespan; disabling weather retains before-pass celestial particles.
  The rendering fixture now includes actual PES 330007DB, the camera used for
  billboarding, every day group, and foreground occlusion at 5 m, 1 km and 5 km.
- Trade renders the complete visible offer grids and labels its Trade/Clear All
  controls. Register/add/clear events reset offers and acceptance correctly;
  0x0451 displays "Trade closed." The spell hotbar retains all 13 visible tiles,
  including its default blank spaces, while insertion appends after the last spell.
- Enter receives preview-key handling before other UI controls consume it and
  focuses the chat keyboard entry. Portal arrivals reuse the existing HUD/binder,
  preserving open windows instead of reinitializing their state.
- Chat configuration includes independent routing for the main window and all
  four auxiliary windows, plus linked inactive/active background opacity sliders.
  These settings persist and participate in Apply/Reset/Defaults. Incoming public
  channels retain distinct message types for filtering and avoid duplicate labels.
- Particle pooling first reuses matching geometry/materials, avoiding repeated
  rebuilds when glow and drip emitters retire meshes in alternating order. Actual
  fire, frost, acid, blunt and piercing weapon setups now have fixture coverage.
  Their timing/parenting checks do not establish visual parity for every rending
  effect; that remains a live comparison requirement.
- Hardware identified: Core Ultra 9 285K (24 cores) and RTX 5090. CPU fixture
  measurements include about 0.87 ms for the HUD tick and about 2.8 ms for a warm
  appearance construction. These are isolated costs, not a measured game frame.
  A populated live-world CPU/GPU capture is still required to establish the FPS
  bottleneck and verify the end-to-end improvement.
- Validation: all 36 editor regression tests passed (32 clean, four with logged
  warnings), with zero failures or skipped tests in
  `Saved/Automation/2026.09.09.11-final/index.json`. Win64 Development
  build/cook/stage/archive completed with exit 0 in
  `Saved/Logs/Package-2026.09.09.11.log`. The packaged executable passed
  `ACE.Packaging.Materials`, the rendered `ACE.RetailParity.Weather` test, and
  `ACE.RetailParity.MeshApplication`; JSON results are in
  `Saved/Automation/Packaged-2026.09.09.11` and
  `Saved/Automation/Packaged-world-2026.09.09.11`. The packaged runner cannot
  generate its optional HTML template, but exported JSON and test results are
  available. The archive is `Saved/Packages/2026.09.09.11/Windows`.

## Changes in 2026.09.09.10

- Commandlets disable MCP auto-start on their process-local settings object
  before the UE 5.8 plugin's post-engine-init callback. Previously, cooking
  inherited the editor's saved auto-start preference and failed when its HTTP
  listener could not also bind port 8000. The setting is never saved by this
  override; interactive editor MCP startup remains enabled.
- Validation: Win64 Development build/cook/stage/archive completed with exit 0
  while an exclusive loopback listener held port 8000 throughout packaging
  (`Saved/Logs/Package-2026.09.09.10.log`). A subsequent non-commandlet editor
  launch successfully created the MCP listener on port 8000 and passed
  `ACE.Packaging.Materials` (`Saved/Automation/Editor-MCP-2026.09.09.10/index.json`).
  The saved `bAutoStartServer=True` preference was preserved.

## Changes in 2026.09.09.9

- Packaging no longer compiles the offline DAT exporter as a runtime module.
  World Partition login holds use DisableStreamingIn/EnableStreamingIn, preserving
  the baked partition configuration and only restoring a hold owned by this subsystem.
  World Composition changes its live tile info without editor asset-update calls.
- All 31 active material parents are generated before cook asset discovery and
  loaded from cooked assets in game builds. Graph defaults are persistent textures;
  material editor-data subobjects are saved without inherited transient flags.
  Forced asset-registry rescanning includes newly generated parents on the first
  cook. The invalid, unused CustomStencil-masked terrain graph was retired in favor
  of the active portal-plane material. Examination variants also key on sidedness.
- Runtime static material slots no longer use the editor-only constructor signature.
  Baked landscapes use dynamic material instances for live fog in game worlds;
  constant-instance editing and editor-only test fixtures stay in editor builds.
- Generated static meshes are owned by the DAT subsystem, giving Chaos a valid
  game world for runtime collision cooking. Transient-package ownership produced
  a mesh with triangles but no blocking body outside the editor. MeshApplication
  now provides an actual game-instance/world context and runs in packaged clients;
  its DAT geometry counts and downward pawn sweeps pass in both builds.
- The default package cooks the startup map and runtime shader assets, avoiding
  unrelated offline world exports. DAT files and saved account credentials are
  external to the package. Cooked output uses disk files: a repeat package exposed
  Zen exiting between cook and staging, leaving the staging operation without its
  input store. See the project README for custom map packaging.
- Validation: Win64 Development build/cook/stage/archive succeeded, including
  repeated material generation. Editor and packaged GPU tests pass for all active
  shader parents, portal clipping/depth/texture updates, and DAT static collision.
  UIScreens, WorldEntry,
  and CameraAndEdges passed in the editor (WorldEntry's three intentional recovery
  warnings remain). Packaged startup reaches the login UI. Live server gameplay
  in the packaged executable is not yet accepted.

## Changes in 2026.09.09.8

- Attributes and skills now both toggle a selected row off, following
  gmAttributeUI::SetSelection and gmSkillUI::SetSelection. Attribute refresh no
  longer coerces an empty selection to Strength. Skill clicks refresh the footer
  and rows immediately. Clearing highlights goes through SetIconDid's cache, so
  selecting a previously cleared row restores the retail selection texture.
  Selected highlights remain hit-test-invisible, like the unselected row overlays.
- Automatic item approach retains the target heading when Mouse Turning is on.
  The generated forward input no longer activates camera-follow yaw. Keyboard
  movement still cancels approach, and manual camera-follow controls retain their
  existing behavior.
- MoveTo failure checks use full 3D displacement from the original approach start
  and let arrival succeed first, matching MoveToManager::HandleMoveToPosition.
  Crossing a finite server limit stops movement and reports the retail 003D
  "You charged too far!" error. Receiving that server error also cancels local
  approach prediction. Repeated MoveTo packets retain the starting position,
  while an unlimited update clears an earlier finite limit. Finite limits above
  100 AC are no longer incorrectly treated as unlimited.
  Limits remain server-authored: Player_Move::GetChargeParameters supplies 15 AC
  for melee charges; ordinary use/pickup defaults to float.MaxValue in both
  retail MovementParameters and the server. This change does not impose a new
  charge-distance limit on interactions for which the server supplies none.
- Validation: editor build succeeded. UIScreens, WorldEntry and CameraAndEdges
  passed (3 suites, no failures). WorldEntry's three warnings are intentional
  unsafe-spawn/recall failure fixtures. Report:
  Unreal/Saved/Automation/2026.09.09.8/index.json.
  Native mouse tests click icon, text and value areas repeatedly, checking the
  selected ID, immediate footer, actual selection texture and empty selection
  after stat refresh. Movement tests exercise the controller and loopback wire
  packets at orbit offsets -90, +90 and 180 degrees with Mouse Turning off/on;
  all six reach the use cylinder and emit exactly one Use action. Tests also
  cover the 15 AC charge boundary, vertical displacement, start preservation,
  arrival precedence, finite/unlimited packet decoding, stop packets and server
  failure handling. Live server reproduction of these fixes remains to be checked.

## Changes in 2026.09.09.7

- The September 9 12:00 crash (UECC-Windows-432B88E94F0F11BC4E4B48A011F2855C)
  enters SACERetailTextBlock's Slate fallback during prepass. The Slate DLL
  instruction maps to FPlainTextLayoutMarshaller::SetText reading StrikeBrush's
  resource. UTextBlock installs a pointer to its own StrikeBrush in Slate; a
  retained Slate label must not use it after the owning UObject is released.
  Desired-size and paint now return without entering fallback in that case.
  Live labels still fall back normally while their DAT font is unavailable.
- The blue banner-pillar glow at 87.4S, 67.2W uses emitter 3200075A, scripts
  33000DBA/33000DBB and setups 02000F8B/02000F8C in landblock 2B12. Its lifespan
  is .03 seconds and its scale shrinks from 3.3 to 1 before rebirth. Retail
  CPhysicsObj::animate_static_object and update_position only update particles
  after MIN_QUANTUM_97 (1/30 second, initialized in global_funcs.cpp). Advance
  particle simulation on that cadence so high-refresh rendering cannot expose
  a shrink/rebirth strobe between physics updates. Keep the authored timing,
  normal expiration, and one birth per update after a hitch.
- Login entries save after a short typing pause, on field commit, on Connect,
  and when the login widget closes. A fresh launch restores the last address,
  port, account and password. First-install defaults remain empty except for
  port 9000; clearing a field replaces the saved value. The entire payload in
  Saved/Login/LastLogin.dat uses Windows DPAPI for the current user, and no
  credentials are written as plaintext or logged. Damaged files fail closed.

Validation: editor build succeeds. LocalLoginSettings covers encrypted Unicode
round trips, replacement with empty entries and corrupt-file rejection.
TextLifetime retains Slate across UObject collection, then performs prepass and
GPU paint; it measures zero size and draws nothing after release. UIScreens,
ParticleLighting and Weather pass. ParticleTiming checks the actual glow's
visibility, size and opacity on every frame at 24, 30, 60, 120, 144 and 240 Hz.
ParticleTiming also verifies normal expiry after stopping the assigned emitter
instance and passes in the corrected follow-up run. Its two unresolved-effect
fixture warnings predate these changes. Reports: Saved/Automation/2026.09.09.7 and 2026.09.09.7-Particle.
The original live crash sequence and live pillar appearance still need user
play verification; the automated lifetime and emitter fixtures are distinct
from a live-server reproduction.

## Changes in 2026.09.09.6

- Login server address, account name and password now default to empty. The
  editable fields remain available, and the existing default port is retained.

## Changes in 2026.09.09.5

- Character selection still uses the native 800x600 layout and the authored
  40000009 sans font. Fixed the CPU text compositor storing premultiplied RGB
  where Slate expects straight alpha, which applied glyph coverage twice and
  made character names thin and dark. Text lands on integer pixel positions.
  Character rows now use the server slot limit and the twenty-row density cap
  from gmCharacterManagementUI::RebuildCharacterList; text and hit areas agree.
- Camera translation uses CameraManager's default stiffness 0.45 and its
  stiffness * delta-time * 10 response. Running lets the view trail the player,
  then it settles back when the player stops. This preserves manual zoom,
  camera collision and the existing mouse/keyboard control preferences.
- Corrected background-sky material behavior against D3DPolyRender::SetSurface
  and RenderMeshSubset. The starfield's black background now alpha-composites
  over the day dome. The clipped/translucent cloud surface takes retail's
  alpha-blend override instead of additive blending, retaining its authored
  opacity. Removed duplicate opacity modulation from the sky's emissive path.
- Sky vertex lighting now combines authored emissive, ambient and diffuse
  sunlight, with texture modulation in display space as in the fixed-function
  renderer. This restores the rainy night's purple cloud/horizon tint instead
  of the bright stars and green/yellow day-dome colors. Layer order, authored
  geometry, scrolling and depth occlusion remain active.

Validation: editor target builds successfully. CameraAndEdges, NetworkWeather,
UIScreens and Weather pass. NetworkWeather exercises handshake time, clock
advance, portal/interior transitions, day changes and reconnects using packet
fixtures. UI captures verify native-size placement, antialiased text and full
roster selection. Weather renders all twenty DAT day groups at three phases and
four directions, checks opaque foreground occlusion, and captures unobstructed
rainy-night horizons with an explicit purple-tint regression check. The latter
were visually compared with the supplied retail screenshot; no green strip
appears in those four captures. Reports are under Saved/Automation/2026.09.09.5
with follow-up reports in 2026.09.09.5-Visual and 2026.09.09.5-Horizon; images
are in Saved/Automation/RetailParity. Live server
play and an exact pixel-for-pixel retail sky comparison remain unverified.

## Changes in 2026.09.09.4

- Roof-edge jumping now sweeps the full airborne capsule, including its lower
  sphere, against both roof and soffit faces. Each remaining movement segment is
  clipped in three dimensions and swept again. A falling contact uses retail
  LandingZ; an initial support can be cleared only when the jump is departing it.
  This replaces center-ray landing and the airborne 2D wall-slide path. Grounded
  bench/step support remains on the existing lower-sphere solver.
- UI uses physical pixels at the default application scale. Unreal's inherited
  DPI curve had enlarged the 300-pixel panels and their glyphs at higher
  resolutions. Gameplay retains DAT edge anchoring; character selection and
  creation remain centered at their authored 800x600 size with black surrounds.
- Corrected the 2100003F font catalog mapping, including distinct base/sans
  families, field-value aliases and the fancy-font IDs. Generated text labels
  now resolve DAT glyph atlases too. Glyph drawing modulates RGBA atlases by the
  requested text color, matching TextureBasedFont::RenderText diffuse colors.
  This does not replace Slate's editable-text implementation with a DAT editor.
- Skill and attribute rows retain base/current data and show the current value
  in retail green/red. Their selected footer now includes the signed modifier,
  for example Focus: 240 (+60), with only the modifier colored. Coordination and
  Quickness follow retail's row order; XP raise actions use the corresponding IDs.
- Toolbar buttons maintain their DAT highlighted state while their panel is
  open. Equipment preview selection lights the visible garment parts identified
  by gmPaperDollUI::GetSelectionMaskFromObject/ApplyPartSelectionLighting. The
  preview capture updates through the two 0.2-second lighting phases and restores
  its materials afterward; selecting self also retains the world-model flash.
- Contracts now exposes Contracts, Journal and Page List with selected tab art.
  Journal pages support editable title/label/notes, location, timers, navigation,
  search, sorting, deletion and a scrollable page list. Single click selects;
  double click opens the page. Notes are stored locally per server/character,
  separately from server contracts, following gmJournalUI's client-local role.
- Config's implemented video and mouse controls use DAT slider bar/thumb
  06001285/86, green dropdown display/arrow 060012B3/B1, round toggle states
  06004D15..18 and 060012C5 dividers. The existing Apply/Reset/Defaults footer
  handles these controls, avoiding a duplicate set of native buttons. The
  original retail Config's complete sound/camera/renderer option set is not
  claimed implemented by this styling change.

Validation: Unreal Editor Development build succeeds. CameraAndEdges, RoofEdges
and WorldEntry pass, covering thin roof edges/corners, vertical departure,
underside blocking, airborne wall sliding and the existing actual-DAT bench
movement cases. CharacterCreationScreens passes with the unscaled layout.
UIScreens passes, including journal persistence/search/scrolling, selected-toolbar
states, stat modifiers and opaque character-screen surrounds. Captures were
visually checked for the native-size character screen, Config controls, attribute
footer and Journal tabs. Reports and captured images are recorded under
`Saved/Automation/2026.09.09.4-UI` and `Saved/Automation/RetailParity`.
Live traversal of every building and complete visual/interaction parity across
all retail windows remain acceptance work, not conclusions of these fixtures.

## Changes in 2026.09.09.3

- A/D and Left/Right arrows turn the player in either Mouse Turning mode; Q/E
  remain sidestep controls. Explicit keyboard turning takes precedence over
  camera-follow steering, including W+A/W+D with an existing orbit offset.
  The existing turn axis, predicted AC rotation, and movement-message path carry
  the turn; no server protocol changes are needed.
- Options button captions reset visibility before each page refresh. Apply,
  Reset, and Default no longer remain on Game Play after visiting a list page.
  Footer captions use the DAT button's full rectangle, authored font, margins,
  and centering instead of clipping an 18-pixel font into an 11-pixel estimate.
  Game Play button lookups are scoped to their active page.

Validation: the editor build succeeded. CameraAndEdges, UIScreens, and WorldEntry
all passed in `Saved/Automation/2026.09.09.3/index.json`. The movement fixture uses
Unreal input events and full controller ticks to check A/D and arrows both at rest
and with forward movement, under both mouse modes; predicted facing matches the
requested keyboard turn and Q/E still sidestep. UI coverage visits every list page
and returns to Game Play, checks caption visibility and full font-height allocation,
and refreshes each page repeatedly. `GameplayOptionsReturn.png` and
`GameplayChatOptions.png` were visually inspected: captions are centered and
unclipped, with no footer labels on Game Play. WorldEntry's existing deliberate
unsafe-arrival/recall fixtures still emit their expected warnings. Live hardware
input was not exercised in this pass.

## Changes in 2026.09.09.2

- Per the user's follow-up, mouse-wheel zoom is independent of Mouse Turning.
  OFF still disables orbit/capture and two-button movement; both ON and OFF allow
  zoom in/out. UI scrolling and chat/key-binding focus retain their input guards.
- Config now exposes Mouse turn speed (10%-300%) and a Default speed button.
  The default is 0.75 degrees per mouse pixel, up from 0.22 (about 3.4 times faster).
  The preference scales yaw and pitch immediately and saves locally to
  `[ACE.Camera] MouseTurnSpeed` in GameUserSettings.ini, alongside the existing
  client settings. This is a user-requested control preference, not a retail
  protocol extension. It remains available across reconnects and client restarts.
- Existing RMB orbit, two-button forward movement, and left-click selection are
  preserved. The controller clamps invalid/out-of-range sensitivity values.

Validation: build 2026.09.09.2 succeeded. CameraAndEdges and UIScreens both passed
with zero warnings/errors in `Saved/Automation/2026.09.09.2/index.json`. Coverage
includes zoom in/out with either turning mode, focus blocking, immediate yaw/pitch
scaling, persisted setting reload, recreating Config with the saved speed, and
restoring defaults through its button. `GameplayVideoOptions.png` was visually
checked for the new controls and layout. Live hardware mouse input was not
exercised in this pass.

## Changes in 2026.09.09.1

- The character's Mouse Turning option (PlayerOption 0x31) now gates world-camera
  orbit and mouse-wheel zoom. OFF preserves left-click selection and right-click
  inspection. ON enables captured RMB orbit, and holding both mouse buttons moves
  forward after aligning the character with the camera's horizontal heading.
  Left alone still selects. Keyboard camera controls remain available.
- Mouse gestures must start outside the UI. Inventory, spell, and scrollbar drags
  cannot start camera capture. Releasing either button stops mouse-driven forward
  movement; releasing RMB restores the cursor. A gesture used for movement cannot
  become an inspect click. Disabling the option or focusing chat/keybind editing
  cancels capture and requires a new press before another orbit.
- The equipment portrait composites the captured model over the existing DAT
  panel background. Retail `CreatureMode::Render` draws the model into its viewport;
  the PaperDoll element in layout 0x21000005 does not supply an opaque background.
  `UACECaptureImage` uses Slate's inverse-alpha and premultiplied-alpha draw flags
  with SceneCapture HDR RGB/inverse opacity. This preserves the panel texture,
  partial coverage, and widget fading without a material or editor-generated asset.
- The optional two-button movement behavior is the user's requested control scheme,
  not a claim that the retail camera uses those controls. Option bitfield/network
  persistence continues through the existing SetSingleCharacterOption path.

Validation: the editor build succeeded. CameraAndEdges, UIScreens, and WorldEntry
all passed in `Saved/Automation/2026.09.09.1-Final/index.json`. The UI test checks
inverse capture alpha, visible foreground pixels, transparent background pixels,
and the final panel composition (467 visible model samples; 18 of 2,133 background
samples differed at filtered edges). The rendered equipment panel was also inspected.
WorldEntry retains its deliberate unsafe-arrival warning fixture. Build/runtime logs
are `Saved/Logs/Build-2026.09.09.1.log` and `Saved/Logs/2026.09.09.1-Final.log`.
Live server mouse/viewport input has not been exercised in this pass.

## Changes in 2026.09.08.6

- The spell hotbar displays one trailing empty insertion cell and adds another when
  it is filled. Both arrows are hidden while the favorites plus insertion cell fit
  in the thirteen-cell viewport. Scrolling changes only the viewport; selection is
  an absolute favorite index and casting still uses that favorite. Inserting or
  moving a favorite preserves the selected spell's identity. Favorites can grow
  beyond the previous arbitrary 56-entry limit. The innate slot displays the held
  caster and casts through `UseWithTarget`, with its item/spell name beneath it.
- The preserved user log `Saved/Logs/ACUnreal-user-20260908-InventoryInspect.log`
  shows PlayerDescription before PlayerCreate on both logins. The inventory footer
  was stored under player ID zero and the equipment footer was discarded. The
  pending profile now transfers to the actual player on PlayerCreate. Equipped
  membership survives ObjectCreate arriving before or after that transfer, and
  missing creates retain their saved positions in each container's item/pack lists.
  This fixes a concrete cause of differing inventory and bag order after relogin.
- Inventory reordering continues to send the server's `PutItemInContainer` action
  and use authoritative acknowledgements. Forward pack insertion accounts for
  source removal, as `UIElement_ItemList::AcceptDragObject` does. Bag order and pack
  occupancy now participate in the panel's refresh hash. Retail capacity meters
  use the authored 5x30 background/fill artwork on side packs and the main pack.
  Open-pack markers and selected-item frames are separate, bag icons use the
  authored inner 32x32 rectangle, and the burden meter uses its original artwork
  without a second color tint. The paperdoll checkbox reads `Slots`; aetheria slots
  follow login/live PropertyInt 322 (`AetheriaBitfield`), not character level.
- Item inspection retains DAT spell descriptions and renders the item fields
  supplied by the appraisal: tinkering, workmanship adjective, imbue properties,
  defense/mana conversion bonuses, wield/activation requirements, elemental caster
  bonuses against monsters/players, spellcraft, mana, mana consumption rate, and
  spell descriptions. References: `ItemExamineUI::Appraisal_ShowMagicInfo`,
  `Appraisal_ShowShortMagicInfo`, and its workmanship/imbue/requirement formatting.
  Inscribable items use the authored centered, black `uninscribed` prompt state.
  These focused caster/item corrections do not certify every appraisal type or
  the editable-inscription workflow.
- Terrain radius 5 is the retail `LScape` constructor default: 11x11 = 121 terrain
  blocks. Outer blocks use coarse terrain LOD; nearby scenery preloads within
  radius 2. Keep that horizon coverage to avoid reintroducing town pop-in.
  Progress and large connected environment-cell sets now log at Verbose, while
  a terrain queue making no progress for 30 seconds with loading enabled still
  warns. Deliberately paused outdoor loading while indoors is not a stalled load.

Validation: `Launch.bat --build-only` succeeded. All 32 `ACE.RetailParity` tests
passed in 117.62 seconds (30 clean, two with the existing intentional missing-effect
and unsafe-world-entry warnings). Reports: `Saved/Automation/2026.09.08.6-Final/`
and `Saved/Logs/2026.09.08.6-Final.log`. The inventory test replays login before
player creation, reversed object arrivals, equipped membership, cross-bag moves,
failed moves and root/side contents refreshes. UIScreens exercises real mouse
input for both directions of bag insertion, item reordering, acknowledgements
without forced UI invalidation, short/overflow hotbars, scrolling followed by
casting, end-slot drops, innate item casting, network aetheria unlocks, main/side
pack capacity and a caster inspection that scrolls without artificial padding.
Rendered captures in `Saved/Automation/RetailParity/` were visually inspected;
the half-full pack has its authored yellow lower-half fill and the inscription
prompt uses black centered text.
Live cross-client logout/login comparison and every item appraisal type still
require acceptance; isolated tests do not alter the user's server inventory.

## Changes in 2026.09.08.5

- Selection now tests visible mesh triangles before falling back to selection
  bounds, using the same path for hover, click, identify, and drag. Retail
  `Render::GetMouseSelectionObjectID` / `GfxObjUnderSelectionRay` gives polygon
  hits priority over broad bounds. A regression puts an NPC in front of a statue
  whose oversized selection proxy extends closer to the camera; the NPC wins.
  World walls still limit selection distance.
- Grounded movement and step-up use the retail walkable normal threshold, while
  airborne landing retains the separate landing threshold. Floor probes begin
  within step reach instead of above the character's head, where an upper floor
  could hide the final basement tread. Penetration recovery accepts incremental
  movement out of the same deep contact but rejects a second wall. This addresses
  pedestal trapping without allowing recovery through neighboring geometry.
  Retail references: `CTransition::step_up`, `OBJECTINFO::get_walkable_z`.
- Bench traversal now separates horizontal movement from the downward step and
  retains lower-sphere support at narrow edges, following `CTransition::step_down`.
  Previously, a diagonal sweep cut through the seat edge, then a center-ray slide
  dropped the feet to the floor and penetration recovery pulled the body back onto
  the seat. Step-up candidates also require progress in the movement direction.
  `WorldEntry` walks the actual town-hall bench setup `02000120` in eight cases:
  both axes, both directions, and indoor/outdoor movement modes. Each case crosses
  the seat, rises and descends once, returns to the floor, and never penetrates
  the seat. This isolates the cooked bench; it does not simulate the whole hall.
- Translucent world surfaces now cast ordinary dynamic shadows through the
  material's alpha-mask shadow pass (cutoff 0.1); visible opacity stays continuous.
  This is the user's chosen ordinary shadow, without colored transmission. Sky,
  weather, and particle material bases do not enable this shadow pass.
  `RuntimeActors` compares the same half-transparent surface with casting disabled
  and enabled: 2,817 receiver pixels darken, with summed RGB loss 231,187. The
  fixture excludes black background noise and uses an actual DAT surface material.
- Cell filtering now excludes an unrelated room's geometry components instead
  of its entire actor. Furniture and stairs can cross cell boundaries even when
  owned by a neighboring cell, as in retail `CPhysicsObj::add_obj_to_cell` /
  `calc_cross_cells_static`. The reported town-hall stair setup `02000623` belongs
  to `0134010F`; all 506 sampled upward treads remain available from `0134010E`.
- Reversing direction during a running jump charge clears a temporarily held
  JumpCharging pose before restoring locomotion. Portal exit stretch no longer
  widens the separate portal camera: retail `gmSmartBoxUI` stretches the world
  projection, not the CreatureMode tunnel camera. The tunnel now covers the frame
  throughout that exit phase.
- The Yaraq bench setup `02000368` / mesh `01000EB3` has two seat faces exactly
  coplanar with room `7D64013A`: polygon 15 at X=95 and polygon 16 at Y=81.5.
  Retail `PView::DrawCells` draws room geometry before furniture with LEQUAL
  depth. Unreal now recesses room render depth by 0.0025 AC to preserve that
  tie-break despite opaque draw reordering. Authored geometry and collision stay
  unchanged. The runtime fixture includes the actual upper-floor benches and a
  GPU comparison of coplanar room/furniture materials under tiny camera shifts.
- Sky geometry retains DAT vertices, winding, and scale relationships. Removed
  reversed shells, radial cloud remapping, and interpolated replacement headings;
  `SkyDesc::GetSky` uses discrete replacement mesh/heading and interpolates only
  authored brightness/transparency values. DAT visibility replaces the hardcoded
  night cutoff. Sky layers preserve authored draw order without depth writes,
  while world geometry occludes them in Unreal's later translucency pass.
  `Weather` captures all 20 sky groups at three clock samples and four camera
  headings (240 renders), verifies foreground depth occlusion against an HDR
  reference, and retains rain motion/indoor gating and distance-fog coverage.
- General's F7DE transport was already distinct from local Speech; the incoming
  formatted message was incorrectly wrapped in another local-speech prefix.
  The UI now displays formatted Turbine and legacy channel messages once.
  `UIScreens` exercises the actual General dropdown and entry, checking a
  server-assigned room id, raw message text, and absence of a local speech action.
- `/r` uses the last player teller and a name-based tell that works across
  landblocks. NPC speech/emotes cannot replace that recipient. Added retail
  squelch/unsquelch argument handling, account/reply/category switches, `/chat`,
  `/notell`, networked filters, `/on`, `/off`, `/clist`, `/index`, legacy channel
  aliases, and allegiance management commands. Default squelch is AllChannels=1,
  not Broadcast=0. Fellowship, patron/vassal, co-vassal, and broadcast formatting
  follows `ClientCommunicationSystem`; typed payloads are checked against the
  corresponding server readers. Allegiance MOTD/name, bans, officers/titles,
  lock/bypass, house permissions, and chat moderation are covered by loopback
  packet fixtures. The DAT's 309 pose-command entries all resolve to motions.
- No live-account commands or server moderation actions were sent. The exact
  Sanamar, basement, and town-hall walkthroughs still require in-game acceptance;
  these offline fixtures do not establish complete retail collision or command
  parity.

Validation: `Unreal/Launch.bat --build-only` succeeded and the complete
`ACE.RetailParity` suite passed all 32 tests (zero failed or skipped) in 82.35
seconds. The report has warnings in the ParticleTiming unresolved-effect fixture
and WorldEntry failure/recovery fixtures. Final report:
`Unreal/Saved/Automation/2026.09.08.5-Final/index.json`; runtime log:
`Unreal/Saved/Logs/2026.09.08.5-Final.log`; build log:
`Unreal/Saved/Logs/Build-2026.09.08.5.log`. The coplanar GPU depth fixture resolves
furniture at 180.479 cm and the room at 180.729 cm; their combined render retains
the furniture depth across all three camera shifts. Eight bench walks have zero
seat penetrations, one rise-to-descent reversal, and end at floor height.

## Changes in 2026.09.08.4

- Enemy position corrections now replace the prediction anchor on every accepted
  F748, including corrections smaller than two metres. Collision sweeps constrain
  extrapolated movement from the previous predicted position; they no longer block
  interpolation from the displayed body to an authoritative position. This avoids
  trapping the visual creature behind a local obstacle while targeting, projectiles,
  and its subsequent corpse use the server position. Retail reference:
  `CPhysicsObj::MoveOrTeleport` / `InterpolateTo`. `RuntimeActors` verifies both
  creature blocking during prediction and correction across that blocker, plus a
  75 cm correction during locomotion.
- Worn Mukkir Wings use ClothingTable `10000867`, part 29 `01004D80`, and clip-map
  surface `08001906`. Their U coordinates extend from 0.893786 to 1.94818. Clamping
  to the first tile sampled the black edge and removed the wing membranes. Setup
  materials now wrap each axis whose UVs extend beyond the unit tile, with that
  addressing mode included in the material/texture cache key. Untiled face-patch
  edges remain clamped. This also preserves tiled slippers and other clothing.
  `AppearancePlacement` builds the actual wings with the Hoary Mattekar over-robe
  and Bunny Slippers and produces `Saved/Automation/MukkirWings.png`.
- Town terrain previously wrote depth 48 cm behind its physical surface (22 cm
  in the alternate floor-priority mode). The `WorldEntry` rendering regression
  measured 548 cm at a Yaraq floor physically 500 cm from the capture camera.
  Landblock and chunk terrain now use only a 0.25 cm coplanar tie-break, preserving
  contact depth for shadows and grounded characters. This addresses a rendering
  source of the apparent floating without shifting the model below its collision
  origin. The actual player tick settles the reported `7D64000D` Yaraq position
  to 1200 cm, matching the DAT floor and visual origin. The authored robe hem is
  approximately 4.18 cm above that origin; it has not been artificially lowered.
- Equipment definitions are preserved as SQL **data fixtures only** under
  `Tests/Fixtures`, sourced from ACEmulator/ACE-World-16PY-Patches:
  [Mukkir Wings](https://github.com/ACEmulator/ACE-World-16PY-Patches/blob/master/Database/Patches/9%20WeenieDefaults/Clothing/Clothing/52193%20Mukkir%20Wings.sql),
  [over-robe](https://github.com/ACEmulator/ACE-World-16PY-Patches/blob/master/Database/Patches/9%20WeenieDefaults/Clothing/Armor/45031%20Hoary%20Mattekar%20Over-robe.sql),
  [slippers](https://github.com/ACEmulator/ACE-World-16PY-Patches/blob/master/Database/Patches/9%20WeenieDefaults/Clothing/Clothing/34022%20White%20Bunny%20Slippers.sql).
  No database writes or live-account tests were performed.
- Build and runtime version are `2026.09.08.4`. Launch build evidence:
  `Saved/Logs/Build-2026.09.08.4.log`. Final offline regression evidence:
  `Saved/Automation/2026.09.08.4-Final/index.json` and
  `Saved/Logs/2026.09.08.4-Final.log`: all 32 tests passed in 94.25 seconds
  (30 clean, two with the five expected particle/recovery fixture warnings).
  The final appearance fixture also passes independently in
  `Saved/Automation/2026.09.08.4-Wings/index.json`.
  Rendered Yaraq ground depth is now 500.25 cm at a physical distance of 500 cm.
  The exact live Reedshark encounter and visual ground contact require in-game
  acceptance; these
  regressions do not establish complete retail movement or visual parity.

## Changes in 2026.09.08.3

- Khayyaban arrival: the preserved `ACUnreal-user-20260908-ParityReview.log`
  identifies `9F44001A (90,24.553,31.890)`. The destination is below the DAT
  outdoor terrain (36.54606 m). Outdoor placement now begins its support sweep
  at or above the decoded terrain, retaining the server XY and requiring cooked
  support, a clear body capsule, and a valid cell. `WorldEntry` loads this actual
  landblock and verifies the destination after collision cooking completes.
- Creature prediction uses swept DAT body dimensions and wall sliding instead
  of moving directly through other actors. Runtime coverage moves one Drawohan
  setup toward another and checks that it stops before the other body. Server
  position corrections remain authoritative. Local penetration recovery checks
  the return path against a second wall; step-up checks both the lateral tread
  adjustment and descent before accepting a landing beside an object. These
  retain retail `CTransition` body-collision intent; this is not a transcription
  of the complete retail transition solver.
- Close-range spell effects: retain projectile actors through the server's
  NoDraw/Cloaked physics update so the following Explode script has an owner.
  The real server `SpellProjectile::ProjectileImpact` sends that state before
  its impact script. Object deletion still destroys the projectile normally,
  consistent with `CObjectMaint`; no artificial post-delete lifetime is added.
  `ParticleTiming` covers a real bolt setup, this message order, effect creation,
  and final deletion. It does not simulate live combat damage.
- Object translucency is applied once in the material scalar rather than also
  being baked into resolved texture alpha. Transparent animation hooks can now
  replace that scalar without multiplying an already-faded texture. This affects
  translucent Shadows and devices. Resolved appearance textures clamp at their
  borders so filtering does not sample the opposite edge of a face patch.
  The material fixture checks preserved source alpha, 0.5 scalar opacity and
  both address modes. Full face-seam and Shadow visibility acceptance is visual.
- Default animation frames replace bind rotations, matching the normal animated
  pose path. Particle attachment offsets no longer receive the actor scale a
  second time. The Fianhe `02001A16` / `33001285` fixture verifies head-part eye
  offsets at scale 1.25 against the DAT. Retail `ParticleEmitter::SetParenting`
  and `Particle::Init` preserve this authored offset. DefaultScriptPart now
  dispatches the attached child's script/intensity instead of the wielder's
  setup script, following `CPhysicsObj::play_default_script(part_index)`.
- Base outdoor fill was raised modestly. Scenery remains active through the
  two-landblock Chebyshev ring while terrain keeps its separate LOD policy,
  reducing near-town streaming pop-in at a bounded additional memory cost.
  Sky rendering excludes PhysicsBSP-only sections and applies the authored
  global rotation before heading, as `GameSky::Update`, `CalcFrame` and
  `Frame::grotate` do. Weather tests cover the rotation axes. The reported flying
  sky sheets and the Arwic south traversal have not been visually reproduced.
- Restored vendor/panel selected and unselected DAT tab states; pressed toolbar
  icons use available DAT state media when the requested state has no image.
  Fixed visible-page lookup when a hidden keyboard page shares a name with the
  Character options page. Inventory/tooltips use a dark body and gold border.
  Explicit selection triggers a short brightness pulse in world materials and
  inventory icons; health/name refreshes do not retrigger it. Repeated clicks on
  the same object do. The pulse preserves the original material and texture.
- Contract journal rows select from either the name or status column. Names,
  stages, notes, contacts and locations use the DAT contract table and server
  contract state. Relative server timers count down from receipt time. Journal,
  component and social lists grow with the panel; footer controls retain their
  authored bottom anchors. The inactive personal-notes page no longer paints
  over contracts. Personal journal/page-list editing is still unimplemented;
  this change does not claim those retained DAT tabs are functional.
- Keyboard options now edit up to three chords for the 28 implemented actions,
  with defaults, revert, cancel and persistent save/load. Gameplay input pauses
  during capture. The fixture uses an isolated INI and verifies disk readback.
  Video options control supported resolutions, display mode, quality, frame cap
  and VSync through `UGameUserSettings`. Tests verify the UI without changing the
  user's display configuration. Full retail action coverage, named key maps,
  every video option and exact retail control art remain unfinished.
- Runtime UI captures cover vendor tabs, spellbook/components, contracts,
  character options, keybindings, video, social tabs, character information,
  map, effects, vitae, link status, books, chess and assistance pages. Existing
  inventory/equipment, skills/titles and character-creation coverage is retained.
  These are offline fixtures, not proof that every live control and server
  response matches retail. In particular, book/chess/support pages and some
  social layouts still need full content and interaction comparison.
- Build version and configured window title both read `2026.09.08.3`.
  `Launch.bat` continues to build `ACUnrealEditor` before launching and aborts
  on compilation failure. Build evidence: `Saved/Logs/Build-2026.09.08.3.log`.
  Final regression evidence: `Saved/Automation/2026.09.08.3-Final/index.json`
  and `Saved/Logs/2026.09.08.3-Final.log`. The final Launch build succeeded in
  9.63 seconds, and the loaded DLL reported build `2026.09.08.3`. All 31 tests
  passed in 73.93 seconds: 29 clean and two with expected fixture warnings
  (unresolved effect-table and deliberately unsafe world-entry cases); zero
  failures or skipped tests. Final contracts and friends-page captures were
  visually checked after correcting page visibility and scrollbar placement.
  Live Khayyaban travel, crowded combat,
  translucent creatures/devices, face/eye appearance, sky movement and Arwic
  streaming remain user acceptance checks; no live account was accessed.

## Changes in 2026.09.08.2

- Dungeon portal arrival: the preserved `ACUnreal-user-20260908-DungeonPortal.log`
  shows destination `0179010D` streaming successfully, then holding at
  `spawn-placement` for 45 seconds before the one-shot lifestone recovery. This
  cell has no room collision triangles; its floor is static setup `02000898`
  (GfxObj `01001CD6`). Static objects were deferred until room visibility, and
  the portal-space branch activated only the destination's empty collision mesh.
- Prepare the same bounded collision neighborhood during portal space as during
  indoor movement, including static objects, while keeping rendering hidden.
  This follows retail `CTransition::build_cell_array/check_other_cells` and
  `CEnvCell::find_collisions` (environment plus static-object collision).
  Collision-required stabs also load when the room is outside the camera view.
  Readiness includes deferred static objects and collision-neighbor static work;
  newly created animated stabs inherit their room's hidden state.
- `ACE.RetailParity.WorldEntry` now loads the reported dungeon cell and its six
  direct portal neighbors from real DATs. The hidden-arrival assertion failed
  before the fix. It now drains the asynchronous floor setup, validates a clear
  capsule at `(100,-100,0)`, checks cooked neighbor collision and hidden geometry,
  and retains the previous Yaraq placement and recovery wire tests. The fixture
  position is the DAT cell origin; the old log did not record exact arrival XYZ.
  Portal hold diagnostics now include destination cell and position.
- A live server dungeon portal/relogin has not been exercised by automation.
  The user's account was not accessed for these tests.
- Validation: `Launch.bat --build-only` compiled successfully, runtime logs loaded
  build `2026.09.08.2`, and all 31 `ACE.RetailParity` tests passed (29 clean,
  two with expected negative-fixture warnings). Report:
  `Unreal/Saved/Automation/2026.09.08.2-Final/index.json`; build log:
  `Unreal/Saved/Logs/Build-2026.09.08.2.log`.

## Changes in 2026.09.08.1

- Pedestal entrance: the eye-to-player cell sweep could return a room that did
  not contain the camera after hitting the entrance stair wall. Check endpoint
  containment and adjacent rooms before choosing the outside landcell. This
  keeps the exterior terrain visible at the aperture while retaining indoor
  visibility clipping. The Yaraq entrance orbit fixture reproduced 43 wrong
  room assignments before the correction and none afterward.
- Inventory: server `ContainId` acknowledgements now own ordered membership.
  Removed speculative placement shifts that ran twice across request/ack;
  honor the old container until its ordered move arrives. Restore both item
  and pack order from PlayerDescription and bag ViewContents even when object
  creates arrive later or in reverse order. Slot drops/sort send dense retail
  list positions and retain server state on rejected moves. The loopback test
  decodes actual reliable outgoing actions and replays login/container data;
  a live account logout/login and database persistence have not been exercised.
- Particles: implement the missing A4R4G4B4 image decoder. Destroyed portal
  surface 080012FA is an authored black alpha shadow, not a white fallback
  card or additive glow. Preserve alpha blend flags independently of luminosity,
  use a two-sided translucent particle material with one opacity multiplication,
  and remove synthesized luma alpha from additive-only images.
- Particle orientation now follows the first GfxObjDegradeInfo draw mode,
  using the camera-to-particle heading where the retail draw mode requires it.
  Weeping Wand uses mode 2; the destroyed portal retains its authored frame.
  DAT dimensions, size variation and particle acceleration remain intact.
- Match `ParticleManager::CreateParticleEmitter` / `CreateBlockingParticleEmitter`:
  numbered slots replace immediately regardless of emitter-info DID, blocking
  creates leave occupied slots alone, and zero-ID creates allocate independently.
  This removes old buff particles that previously survived the next equipment
  enchantment. `ScriptManager` FIFO and delayed CallPES hooks preserve hook order;
  pending PlayScriptType, PlayScriptID and sound messages retain arrival order.
- Validation: `Launch.bat --build-only` succeeded and all 31 ACE.RetailParity tests
  passed on the final binary (29 clean, two with expected negative-fixture warnings).
  Runtime log confirms build 2026.09.08.1. Evidence:
  `Saved/Logs/Build-2026.09.08.1.log`,
  `Saved/Logs/RetailParity-2026.09.08.1-Final.log`, and
  `Saved/Automation/Build2026.09.08.1-Final/index.json`.
  Coverage includes actual DAT entrance/material checks, inventory wire/login
  replay with partial/reversed creates and root/side-bag refresh, real enchantment
  slot replacement, and mixed-opcode queue replay. Side-by-side live retail
  acceptance remains outstanding.

## Changes in 2026.09.07.10

- Verified server time against `NetworkSession.WriteOptionalHeaders` (TimeSync,
  `Timers.PortalYearTicks`), `GameTime::CalcDayBegin` / `UseTime`,
  `SkyDesc::CalcPresentDayGroup`, and the retail RegionDesc DAT. Weather is a
  deterministic DAT schedule selected by the server's date, with per-time
  opacity and lighting changes. It is not a separate server rain message.
  One Dereth day lasts 7,620 seconds (127 real minutes); servers with the same
  clock and DATs intentionally show the same weather.
- Preserved user-log evidence in `Saved/Logs/ACUnreal-user-Parity9-network-weather.log`: the .9 connection supplied
  `302951982.695954`, selecting day 39757, group 7 (`Rainy`), phase about .951.
  This group rains at noon too. Earlier logs include Sunny, Clear, and Cloudy
  groups and advancing phases. The captured rainy night is consistent with
  retail time; the available evidence does not show a globally frozen clock.
- Fixed actual lifecycle gaps: disconnect/failure clears the old clock and sky,
  a new connection waits for its own authenticated timestamp, and a valid
  zero epoch is distinct from unknown time. Character switches on one server
  and portal travel preserve the clock. Queued TimeSync packets retain their
  receipt timestamp so waiting for missing packets does not hold time back.
  Non-finite and negative samples are rejected. Day-number and time-of-day
  consumers now share the retail epoch calculation instead of duplicate code.
- Added `ACE.RetailParity.NetworkWeather`, using cleartext handshake and
  ISAAC-authenticated TimeSync datagrams through the real receiver, DAT meshes,
  sky Tick, world-light updates, and terrain-presenter weather gating. Covers
  same-day rain cessation, stars/daylight, forward/backward corrections,
  extrapolated midnight rollover, portal entry/exit, indoor/outdoor travel,
  disconnect/reconnect, character switching, delayed/reordered packets,
  invalid checksums/timestamps, and the user's exact captured clock.
  Expected sky-build diagnostics now use normal log severity.

Validation: `Launch.bat --build-only` succeeded and the loaded client DLL reported
build 2026.09.07.10. All 29 `ACE.RetailParity` tests passed in 76.15 seconds
(27 clean, 2 with existing fixture warnings, 0 failures). NetworkWeather passed
without warnings, including the exact reported timestamp. Evidence:
`Saved/Logs/Build-2026.09.07.10.log`,
`Saved/Logs/RetailParity-2026.09.07.10.log`, and
`Saved/Automation/Build2026.09.07.10/index.json`.
No new live account login was performed; server-side clock behavior was checked
from local protocol code and saved user logs, and client transitions were
exercised with offline packet fixtures.

## Changes in 2026.09.07.9

- Reproduced the reported collision failure with the actual Small Creepy Statue
  (`0200042B`) and Drawohan setup (`02000A0B`): a capsule resting on terrain hit
  the supporting floor at time zero, masking the obstruction later in a lateral
  sweep. The movement query now clears that bottom contact while retaining the
  full body radius and crown. It never ignores the floor component, which can
  also contain building walls. Exact ground contact, 0.1/1 cm penetration,
  joined floor/wall geometry, downward support, NPCs, and actual streamed Yaraq
  building PhysicsBSP are covered. Authored ethereal objects remain passable.
- Doorway spill now starts 100 cm inside the actual aperture and uses a bounded
  outward spotlight with dynamic shadows. Closed doors and walls can occlude
  it. Room walls retain hidden shadows and share its lighting channels. The
  separate light actor survives portal visibility changes; distance fading now
  spans 20 m before its 40 m cutoff. Uniform interior illumination remains.
  Offline render checks compare the actual Yaraq terrain with spill off/on,
  a procedural door blocking the aperture, and the room culled. A static-cube
  test fixture did not render reliably; the procedural fixture matches runtime
  doors. A negative-control run ruled out an additional renderer-bounds change;
  no such change is included.
- Compared `Particle::Init`/`Update`, `Frame::rotate`,
  `SmartBox::HandlePlayScriptType`, and the reported items' actual DAT scripts.
  Fixed local-to-world transform order, virtual part transforms, axis-angle
  rotation, axial-vector handedness, explosion direction normalization, and
  swarm/implode initial positions. Parent-local behavior follows each emitter's
  DAT flag. Removed the unsupported filter on portal disc gfx `01003DE3`.
  Weeping Wand `02000F1C` runs `33000DAA` with `3200056C` and all three `32000743`
  drip hooks; Destroyed Portal `020019E4` runs `3300126D`, including virtual
  part 6 and the `320009C2` spinning disc. Independent decoder output is saved
  in `Saved/Logs/Parity9Reported{Setups,Scripts,Emitters}.txt`.
- Network effects run on the addressed object's own physics-effect table.
  Removed implicit replay on a weapon's wielder or the local player, which
  could turn item messages into extra body buffs. Assault Orb `02000EC3` and
  explicit body-target controls are covered separately. Projectile lights are
  brighter (10-20 unitless intensity) and follow the missile head rather than
  the average of its old trail particles; bounded light counts are retained.
- F/the hand action now merges the selected owned item into compatible stacks
  sequentially and places survivors/remainder at the front of the main pack.
  It waits for both authoritative quantity updates before another merge;
  repeated input, timeout, character changes and trade state cannot launch a
  second stale merge. No stack quantities or object removals are predicted.
  Loopback packet tests cover partial/full merges across packs and final order,
  following `ACCWeenieObject::UIAttemptMerge` and `CM_Inventory` payloads.
- Paper doll now uses retail `gmPaperDollUI` heading 191.3679, the correct
  heritage camera, UI enum group 7 animation, and held frame 1. The camera is
  independent of character scale, and the preview actor is positioned after
  its root exists. The checkbox uses its DAT child rectangle, textures, text
  margins, and pressed/selected state above the model capture. Slot art,
  nine-region DAT silhouette selection, clothing/armor layering, drag/drop,
  shortcut overlays and wield/unwield packet paths are exercised by UIScreens.

Validation: `Launch.bat --build-only` succeeded for 2026.09.07.9, and the loaded
DLL reported that exact build stamp. `ACE.RetailParity` completed all 28 tests:
26 successes and two successes with warnings, zero failures/skips. The warnings
are intentional negative controls: unmapped item effects stay unresolved rather
than becoming body buffs, and unsafe spawn placements are rejected. Evidence:
`Saved/Logs/Build-2026.09.07.9.log`, `Saved/Logs/RetailParity-2026.09.07.9.log`,
and `Saved/Automation/Build2026.09.07.9/index.json`. The door fixture removed
added spill from 30,340 terrain pixels. These are isolated render/physics tests
and loopback protocol tests, not a live server login. In-game confirmation is still
needed for actual open/closed animated doors, moving fire/acid lighting, Assault
Orb equip's complete single-blue-effect sequence, and side-by-side timing/size
for all particle assets. Full one-to-one retail parity is not claimed. The prior
bench underside flicker remains unproven outside the diagnostic camera fixtures.

## Changes in 2026.09.07.8

- World-object appearance was using unlit materials while streamed outdoor
  scenery used DefaultLit. Opaque and masked world textures now use the same
  ambient, diffuse and shadow response as scenery. This includes the local pawn,
  remote players, NPCs and network props. Authored luminous/translucent effects
  and UI previews retain their separate material behavior. Occupied interior
  cells retain uniform illumination; returning outside restores current weather
  lighting. Per-appearance material parameters preserve script fades and palette
  textures without unsupported dynamic-material parents.
- Collision now reads Setup cylinders and spheres, previously skipped by the
  Unreal DAT parser. Retail `CPhysicsObj::FindObjCollisions` prefers actual part
  PhysicsBSP, then cylinders, then spheres; `CPartArray::CacheHasPhysicsBSP` checks
  the actual gfx BSP pointers. Empty procedural collision sections no longer
  disable a prop's fallback body. The Small Creepy Statue (`0200042B`) has no
  physics polygons but authors a cylinder at `(0.02,-0.1,0)`, radius `1.384`,
  height `4.198`. It now uses that body independently of its selection volume.
  Retail `CCylSphere` uses flat ends, represented here by a 64-sided convex
  cylinder (radial error below 0.13%), rather than a rounded capsule. PhysicsBSP
  props keep their authored polygon collision; server ethereal state still
  disables blocking. This does not claim analytic equivalence of Chaos contact
  resolution and retail's complete transition solver.
- Entering a town room previously disabled pawn blocking for every building
  shell. Only the occupied building now yields to its EnvCell collision. Other
  buildings, including those in neighboring streamed landblocks, remain solid.
- Preserve uniform lighting deeper indoors, blending the final 1.8m near each
  exterior aperture toward the entrance light. The blend is bounded by aperture
  width and height, with independent per-cell parameters. Doorway spill is now
  intensity 18, radius 425cm, with the existing two-light limit and distance
  culling. This transition and spill are the requested visual extension, not a
  claim that retail used dynamic doorway lights.

Evidence: `diag/InteriorAudit` independently decodes the statue, Pool
(`02000117`), Drawohan (`02000A0B`), and Yaraq building physics from the retail
DATs; output is `Saved/Logs/InteriorAudit-Parity8.log`. Runtime regression renders
isolate each model as a light/shadow receiver, compare cast shadows with identical
visible geometry, sweep both simple and complex queries against the statue, and
check actual wall traces and movement sweeps for all eight Yaraq buildings.
Rendered threshold comparisons confirm spatial blending while the day/night
interior test retains uniform brightness. The player's `.7` log is preserved as
`Saved/Logs/ACUnreal-user-Parity7-light-collision.log`.

Build: `Launch.bat --build-only`, recorded in `Saved/Logs/Build-Parity8.log`.
`Saved/Automation/Parity8Verified/index.json` records all 28 parity tests passing
(27 clean, one warning-bearing test). No gameplay/editor session was driven
against the live server for this pass; the user's exact nighttime camera views
still need in-game acceptance. Build header and displayed version are
`2026.09.07.8`. The earlier unresolved bench-underside flicker remains open.

## Changes in 2026.09.07.7

- Preserve the accepted uniform interior illumination. Doorway glow now uses
  explicit unitless falloff, intensity 8 and a 350cm radius; the former intensity
  1.25 / 250cm radius barely reached the terrain below the aperture centre.
  The actual Yaraq terrain regression failed on the old settings (9 visibly
  brightened pixels) and passes with the stronger, bounded light. There remain
  at most two lights per exterior-connected cell, culled at 20m, without shadow
  maps, specular highlights, volumetric light or translucent-particle lighting.
- Doorway lights have a separate visible actor owned by the cell. Hiding room
  geometry through PView must not extinguish a glow that remains visible outside.
  Cell reload, destruction and EndPlay explicitly release that actor. Regression
  coverage checks exterior illumination with the room hidden, and light cleanup.
- Improve nearby shadow resolution while retaining the common character/building
  shadow path and density. Four 4x cascades reserve approximately the first 7m
  for close detail while retaining the 600m range. The old three 2x cascades gave
  the first map roughly 86m. Module startup and engine configuration agree on
  four cascades. No color-depth, tone-mapping or indoor brightness change is made.
  Engine source: `FDirectionalLightSceneProxy::ComputeAccumulatedScale` /
  `GetSplitDistance`. A real DAT character's rendered shadow is compared with
  the old configuration on a neutral receiver, independently of texture colors.
- Bench flicker remains open: independently decoded the reported building's
  static placements and bench `02000368` / `01000EB3`; no duplicate placements
  were found. Retail `D3DPolyRender::ConstructMesh` emits both windings for the
  bench's CullMode.None seat, as does the current builder. Five underside views
  compare local and actual Yaraq coordinates; seven balcony camera views compare
  PView against all room geometry. The user clarified that only the underside
  texture flashes, while the legs remain visible. Its texture already has a full
  trilinear mip chain. An optional 24-view comparison with PixelDepthOffset removed
  changes only 0Ã¢â‚¬â€œ4 edge pixels; it does not reproduce the missing seat. An isolated
  120-engine-frame moving-camera sweep with temporal anti-aliasing also reports
  zero missing interior pixels. These fixtures have not reproduced the user's
  live in-building flicker, so this issue remains unresolved. No speculative mesh
  displacement, depth-offset removal or blanket two-sided-material change is made.
  Diagnostic switches for RuntimeActors: `-ACEBenchDepthProbe` and
  `-ACEBenchTemporalProbe`; reports are in `Saved/Automation/Parity7BenchDepth`
  and `Saved/Automation/Parity7BenchTemporal`.

Build and validation results are recorded in `Saved/Logs/Build-Parity7.log` and
`Saved/Automation/Parity7Verified/index.json`: all 28 parity tests pass (26 clean,
two with warnings). Warning-bearing tests include the engine's
`r.MotionVectorSimulation` thread-access warning and deliberate unsafe-spawn /
failed-recall fixtures; there are no failed tests. The subsequent optional bench
diagnostics also pass. Launch.bat builds the current
Editor target before launch and refuses to open stale binaries after a build
failure. Build header and project/displayed version are `2026.09.07.7`.

## Changes in 2026.09.07.6

### Reported gameplay, appearance, and interface regressions

- Tutorial spawn marker: the red arrow in cell `8C0401AD` is a DAT Stab
  (`02000C39` / `010028CA`), not a network object carrying HiddenAdmin. Its
  `11000118` degrade table gives the visible model a maximum distance of zero.
  `FACESetupMeshBuilder` now honors that authored no-draw range, preserving its
  physics data. Existing network visibility flags remain in force. Mesh format
  121 and disk schema 8 invalidate geometry caches containing the old marker.
  Source: retail `GfxObjDegradeInfo::get_max_degrade_distance` and the independent
  `diag/InteriorAudit` DAT decoder. This is not a model-ID or object-name blacklist.
- Character changes: restore a script's temporary fade materials before applying
  new appearance data. Clear part material overrides, script effects, and the
  reused pawn's appearance when returning to character selection. Otherwise the
  old character's saved material instances could replace the next character's
  textures after login. The regression exercises restoration followed by a new
  material assignment and effect cleanup.
- LFG logout message: map channel enter/leave codes `051B` and `051C` to their
  retail text and normal system chat. A channel departure is no longer shown as
  an unknown error. An incoming packet fixture checks the decoded text and channel.
- Skills: render the icon DID from each SkillTable entry, using the canvas DAT
  resolver even when the binder does not have a player controller. Keep the name
  to the right of the icon and preserve the existing base/buffed calculations.
- Equipment: use retail `gmPaperDollUI::CreateClickMap`'s `25000010` map at the
  authored `PaperDollDragMask` origin. Hidden slot boxes no longer intercept body
  clicks. The nine colored body regions select the upper equipped item using
  `InventoryPlacement` priorities, including the incoming clothing-priority field.
  Dropping clothing on the model accepts retail's body mask `08007FFF`; peripheral
  slots retain their own targets. Existing 24 empty-slot DIDs and coordinates were
  checked against layouts `21000023` and `21000037`. Slot selection and accept/reject
  feedback now use `06004D09`, `060011F9`, and `060011F8` respectively.
- Examination: retain the typed appraisal property tables instead of discarding
  fields. Player details now include allegiance/monarch/followers, body armor,
  damage ratings, arrival date, age and deaths when supplied. Item details include
  armor material resistance descriptions, workmanship, tinkering, spellcraft,
  mana, activation/wield requirements, defense/mana-conversion modifiers, uses,
  cooldown and crafter information. Mana conversion is a zero-based bonus, unlike
  defense multipliers. Source: `AppraisalSystem`, `ItemExamineUI`, and server
  `AppraiseInfo`. The UI uses the supplied data; absent fields are not fabricated.
- Interior illumination: room geometry and opaque/masked DAT furnishings use
  uniform full illumination independently of outdoor ambient and camera-cell
  transitions. This implements the requested fully lit presentation; it deliberately
  replaces the earlier 0.2 ambient approximation. A maximum of two short-range,
  unshadowed warm lights per outside-connected cell provides the requested doorway
  spill. Lights belong to the cell and follow its visibility/lifetime. Interior
  furniture material variants retain culling and live fog updates.
- Shadows: moving entities now use the same cascaded directional shadow projection
  as building geometry, with separate inset shadows disabled. This removes an extra
  shadow path that gave entities a different density. Receiver textures and ambient
  light can still make apparent darkness differ; a matched live comparison remains
  necessary.
- Portal exit: apply `gmSmartBoxUI`'s two projection-distance stages: stretch the
  tunnel for one second, switch to the destination world with a stretched projection,
  then restore the normal world projection over one second. Use the retail
  `GetAnimLevel` table rather than interpolating the FOV angle. Interrupted transitions
  and character changes clear the temporary projection. Existing early network
  completion and collision readiness handling are preserved.

Build and validation:

- `Launch.bat --build-only` compiled and linked successfully. Build header,
  project version and displayed title are `2026.09.07.6`; the updated DLL is in
  `Plugins/ACEClient/Binaries/Win64`. See `Saved/Logs/Build-Parity6.log`.
- `Saved/Automation/Parity6Verified/index.json`: all 28 tests passed, none skipped.
  The run recorded three intentional blocked-spawn/failed-recall warnings in
  `WorldEntry` and one invalid-parent warning in the new palette test fixture.
  That fixture now clones from the base material and copies uniform parameters.
  After the final rebuild, `Saved/Automation/Parity6RuntimeVerified/index.json`
  confirms `RuntimeActors` passes with no warnings or errors. Both test processes
  exited 0. No gameplay source changed after the successful full-suite run.
- The actual Yaraq room/furniture render remains stable when outside ambient
  changes from dark blue to warm daylight (mean RGB difference below 1/255 on
  visible room textures). Captures were visually inspected for uniform walls,
  stairs and ceilings. Detailed item/player text, skill icons and selected
  equipment-slot captures were also inspected.
- Engine startup still reports its editor-layout migration, missing non-Windows
  SDKs and `r.MotionVectorSimulation` console-variable warning. They are not
  suppressed or represented as resolved client issues.

Rendered fixtures include `GameplayCookingBuffed`, `GameplayPlayerInspection`,
`GameplayDetailedItemInspection`, `GameplayEquipmentSelected`,
`GameplayEquipmentModelTargets`, and `YaraqUniformInterior_Day/Night` under
`Saved/Automation/RetailParity`. The body-map test uses real canvas coordinates,
selects outer armor over an undershirt, drags it from the model, and validates the
outgoing equip packet through an isolated UDP socket. No live account is mutated.

Still requiring live acceptance: switching two real characters through logout,
creation into the tutorial dungeon, giving/equipping multilayer outfits, comparing
player/item examinations with retail on the same objects, night-time doorway spill,
entity/building shadow density, and portal arrival under network delay. The equipment
model's retail blinking part-selection illumination and the complete set of item-type
specific appraisal layouts are still checklist work. These fixes do not establish
complete retail parity for the entire client.

## Changes in 2026.09.07.5

### Character creation preview and name-entry regressions

- The creation canvas is focusable while its binder is attached. The earlier test
  invoked `KeyChar` directly and inspected the mouse reply, so it missed the actual
  non-focusable Slate widget that prevented typing. The new fixture focuses a real
  Slate window and routes character events through `FSlateApplication`.
- The examination sequence uses absolute retail part frames, without multiplying
  Setup placement rotations into them. Face view seeds the authored frame before
  pausing; clothes and summary use the mapped animation at 30 frames per second.
  Examination updates bypass gameplay-camera distance culling. The test now registers
  a gameplay camera and checks individual part transforms at several animation times.
- Bare preview actors now receive their location after their root component exists.
  Unchanged appearances retain their meshes and animation phase; hidden previews stop
  ticking. Scene capture retains rendering history between captures.
- Clothing controls, replacement models/textures, and subpalette ranges/shades were
  checked against the independent `ACE.DatLoader` decoder: 20,400 combinations across
  the 35 starter clothing tables agree. Swatch and shade mouse tests also verify the
  generated appearance reaches the displayed model, with rendered examples of both.
- Dedicated examination materials reproduce `CreatureMode`'s 0.3 ambient light and
  `gmCG3DView`'s intensity-2 distant light, clamped at vertices before texture modulation.
  The old preview light did not affect its unlit world materials. The preview no longer
  inherits world fog or filmic tone/color adjustments, and uses separate material instances.
- Template skylight real-time capture is disabled before waiting for world entry,
  addressing the warning displayed on the creation pages.
- DAT handles now permit the retail client's existing write handles while remaining
  read-only themselves. Both retained and temporary reads support concurrent clients;
  the previous sharing violation prevented the parity tests from loading retail assets.

Build and verification artifacts:

- `Saved/Logs/CreationPreviewFixBuild.log`: `Launch.bat --build-only` compile/link.
  Build header and project display title/version are all `2026.09.07.5`.
- `Saved/Automation/PreviewParityVerified/index.json`: all 28 retail regression tests
  passed (27 without warnings; `WorldEntry` emits three expected blocked-spawn/recall
  fixture warnings). No failed or skipped tests; the editor test process exited 0.
- `Saved/Automation/RetailParity/CreationClothesColor2.png` and
  `CreationClothesColor8.png`: rendered color-control examples.
- `diag/CreationAppearanceDump`: independent read-only DAT oracle and Python comparison
  against `Saved/Automation/CreationClothingActual.csv` emitted by the creation data test.
  The final run matched all 20,400 garment/setup/color/shade combinations.

The rendered clothes and Summary examples were visually inspected for assembled limbs
and visible palette changes. Engine startup still reports editor-layout migration and
uninstalled non-Windows SDKs; the suite also logs UE's `r.MotionVectorSimulation`
render-thread console-variable warning. These are not suppressed or counted as client
fixes. The creation fixture itself completed without warnings or errors.

Actual character creation/persistence on the intended server and a direct retail visual
comparison of every heritage remain live acceptance tasks. Local loopback tests do not
mutate a live account. Character deletion, restoration and credits remain checklist gaps;
this pass prioritized the reported creation regressions.

## Changes in 2026.09.07.4

### Character creation

The Create Character button now opens all six retail pages, backed by the original
`21000038` layout, `2100004C` control templates, DAT fonts, textures, portraits,
tooltips and confirmation/error dialogs. The model reads CharGenData `0E000002`,
SkillTable `0E000004`, language strings and DID mappers directly from retail DATs.

- All 13 heritages and both genders, including the Olthoi page restrictions.
- Profession templates and custom attributes, locks, borrowing unspent points,
  derived vitals, skill training/specialization, refunds, free heritage skills,
  and profession fitting after custom changes.
- Face strips, hair styles, bald eye textures, skin/hair/eye palettes, all four
  starting clothing slots, clothing color templates and shade sliders. Compatible
  appearance choices survive heritage/gender changes. Clothing composition follows
  the retail hat/trousers/shirt/footwear order.
- Authored heritage preview rooms, face rest pose, animated full-body preview,
  rotation and 0.6-second zoom; an isolated distant light uses the retail direction
  and intensity, with fixed exposure instead of adapting to clothing colors.
- Starting-town map and heritage restrictions; name editing, selection, dragging,
  clipboard, 32-character limit and retail formatting; complete scrollable summary,
  page-specific Random behavior, exit confirmation and unspent-attribute warning.
- Finish sends the edited draft through the authenticated reliable UI queue
  (`F656`). It retains the draft on rejection and handles `F643` success using the
  server-assigned identity before returning to character selection. Pending requests,
  duplicate Finish, disconnected transport, malformed replies and full accounts are
  guarded. The request never asks for admin/sentinel privileges.
- String16L now explicitly uses Windows-1252 bytes and byte-counted padding. The
  independent server decoder exposed its own UTF-8 `ReadChars` mismatch; its shared
  reader now consumes exactly the declared byte count and decodes the same code page.
  This does not expand the retail character-name input alphabet.

Implementation references: `gmCharGenMainUI`, the six `gmCG*Page` classes,
`gmCG3DView`, `CharGenState` (templates, constraints, randomization and fitting),
`ACCharGenData::FormatName`, `CPlayerSystem` character creation, and
`Source/ACE.Server/Network/Handlers/CharacterHandler.cs`.

Validation artifacts:

- `Unreal/Saved/Logs/CharacterCreationBuild.log`: successful compile/link through
  `Launch.bat --build-only`, the same build path used for user launches; both build
  stamp and ProjectVersion are `2026.09.07.4`.
- `Unreal/Saved/Automation/CharGenFinal/index.json`: creation data and GPU/UI tests,
  including real mouse routing, attributes/skills, appearance constraints, names,
  tooltips, Olthoi navigation, Finish over local UDP, rejection/retry and acceptance.
- `Unreal/diag/chargen-server-validation.txt`: all 26 heritage/gender wire fixtures
  accepted by the independent `ACE.Entity.CharacterCreateInfo` parser, with DAT
  validation of class, towns, attributes, skills, styles, palettes and hues.
- `Unreal/Saved/Automation/ServerCharGen/CharacterCreationStrings.trx`: two server
  String16L tests pass, including extended-byte boundaries and truncated input.
- `Unreal/Saved/Automation/RetailParity/CharacterCreation1.png` through `6.png`:
  rendered and visually inspected native pages at 1600x1200.
- The broader retail suite has 26 other successful results; WorldEntry includes
  expected warnings from the deliberately blocked-spawn and failed-recall fixtures.
  The initial creation-screen failure assumed random templates left skill credits;
  the test now chooses Custom explicitly. Both creation tests pass in the final
  focused rerun, including the added Finish-button network integration checks.

Live acceptance remains: create a character on the intended server, reconnect,
and verify the persisted appearance, stats, trained skills, starter equipment,
profession title and starting location in-world. No live account was mutated by
these tests. Retail's Keystone Help plugin is not hosted by Unreal; Help currently
shows the original DAT instructions in a native dialog. Preview lighting uses UE's
renderer and still needs a direct retail screenshot comparison. These limits prevent
claiming complete visual and live-server acceptance from automated tests alone.

### Character statistics and titles

The attribute/skill panels now distinguish invested ranks, computed base values and
buffed values, including attribute contributions, enchantment stacking, vitae and
augmentation modifiers. Live quality changes invalidate the displayed values and
raise costs use cumulative XP tables. The Titles tab uses the retail layout and
title list, supports sorting/selection, and sends the selected title to the server.
These changes have focused `CharacterStats` and native UI regression coverage;
the reported Cooking increase and live title change still need user acceptance.

## Changes in 2026.09.07.3

- `Launch.bat` / `OpenACUnreal.bat` now run an incremental ACUnrealEditor
  Win64 Development build before opening Unreal. Build errors stop the launch;
  the launcher no longer falls back to an older engine than the project's 5.8.
  `Launch.bat --build-only` validates the same path without opening the editor.
- Update the compiled build stamp and ProjectVersion together. The Play window
  title also shows 2026.09.07.3 so the test build is visible without opening logs.
- The user's launches at 06:48:34 and 06:53:05 UTC already loaded .2, as recorded
  by the compiled module in ACUnreal.log and its 06:52:44 backup. Those reports
  cannot be attributed to stale binaries; unresolved visual behavior remains
  subject to the acceptance items below.

Validation: `Launch.bat --build-only` compiled and linked successfully; a normal
`Launch.bat` run then reported the target up to date and opened the same project.
ACUnreal.log at 06:58:49 UTC confirms the loaded module's build stamp and the
engine ProjectVersion are both 2026.09.07.3; engine initialization completed.
The gameplay changes retain the .2 regression results below; .3 changes only
launch/build identification.

## Changes in 2026.09.07.2

- Restore server mesh visibility after object initialization and deferred DAT
  appearance creation. Collision/fallback setup previously re-enabled the mesh
  after `NoDraw` had been applied. Appearance restoration now respects NoDraw,
  Hidden, Cloaked, and HiddenAdmin at the part boundary. Effect-only setups retain
  no fallback body after physics updates. A cloak update cancels queued spawns;
  an uncloak update can recreate a previously removed actor without requiring
  another ObjectCreate. Reference: retail `CPhysicsObj::set_nodraw`,
  `CPhysicsPart::SetNoDraw`, and server `SerializePhysicsData`.
- StopParticle/DestroyParticle match the authored emitter slot, not the emitter
  resource ID or our runtime serial. Retail PlayScript 0x76 creates slots
  1000..1013; 0x75 stops those slots and fades the body in over 0.75 seconds.
  Arrival bubbles now stop emitting and drain at their DAT lifespans. Tests
  exercise both the body fade and eventual absence of all arrival emitters.
- Room draw meshes use the union of DrawingBSP InPolys instead of the polygon
  dictionary minus a guessed portal set. The dictionary includes unused blue and
  black connectivity faces. Conversely, Town Network ceiling caps 000701A6/A7
  are portal polygons that also occur in InPolys and must render. Compare retail
  `ACRenderDevice::DrawEnvCell` / `BSPTREE::draw_check`. EnvCell cache schema is 15;
  independent ACE.DatLoader fixtures cover 4,255 cells and 159 GfxObjs.
- Preserve authored dimensions for ordinary particle geometry rather than
  normalizing small cubic meshes. Sample scale and translucency endpoint jitter
  with the clamps in `ParticleEmitterInfo::GetRandom*`. The Weeping Wand uses
  a flat authored quad; its reported apparent size still needs live comparison,
  and no wand-specific size multiplier has been introduced.

Validation: ACUnrealEditor Win64 Development compiled successfully. Offscreen
GPU suite `ACE.RetailParity` completed all 25 groups (24 clean, one with three
intentional blocked-spawn warnings, zero errors). Report:
`Saved/Automation/RetailParity20260907-visibility2/index.json`.
This does not establish live visual acceptance for the reported admin markers,
interior lighting, doorway transitions, or the remaining movement/UI issues.

## Changes in 2026.09.07.1

- Pedestal Weak Spot (weenie 16919, Setup 02000D55) uses motion table 090000F9,
  not the Setup's empty default motion table. Its 03000919 transition plays at
  +30/-30 FPS and runs directional TransparentPart hooks on part 0: 0 -> 1 in
  0.26666668 seconds when opening, and 1 -> 0 when closing. Retail
  `TransparentPartHook::Execute` delegates this to the physics part translucency
  transition. Our script component could sleep before receiving that hook;
  adding a scalar tween did not wake it. Start the authored fade and enable
  ticking when the hook arrives. Remove the pedestal-specific closed bind-pose
  workaround. Apply settled visual hook endpoints for already-open ObjectCreate
  messages without replaying sounds or particle bursts. Preserve server reset
  timing; the client does not invent an automatic close timer.
- Retain materials replaced by temporary dissolve materials through Unreal GC.
  Convert only the affected part for TransparentPart, and restore its original
  lighting/blend material when it becomes opaque again. This also restores solid
  shadow behavior after opening/closing or leaving portal space.
- Dungeon residency now includes the player's current landblock even when the
  previous outdoor keep ring remains cached. Preserve remote players and items
  in that dungeon, release the outdoor ring in pure dungeon landblocks, and avoid
  reloading the same cells at every internal cell transition. Destroy separately
  spawned scenery children when their landblock, terrain chunk or room is
  destroyed. This fixes a concrete route for birds and other scenery to accumulate
  after leaving and returning. No live Viridian Rise frame-time improvement is
  claimed without measuring the same traversal.
- Cell connectivity is not equivalent to a draw aperture. Preserve ordinary
  CellPortal polygons unless DrawingBSP identifies a PORT aperture or the portal
  leads outside. Town Network 000701A6/000701A7 otherwise lost their sole ceiling
  polygon. Keep the Yaraq outside blue 080006FD faces excluded. EnvCell cache
  schema is 14; independent ACE.DatLoader fixtures now include 4,255 cells and
  159 GfxObjs. Remove the extra 80-meter building aperture cutoff: the resident
  full-detail ring, portal side and clipped view already bound this work.
- Remote movement no longer discards elapsed time beyond 0.05 seconds per frame.
  Position prediction consumes the whole interval (including acceleration), so
  a 10 Hz renderer covers the same distance as 60 Hz. Nonmissile objects carrying
  velocity now use the ordinary appearance creation budget; velocity alone does
  not identify a projectile. Zero-framerate DAT motion segments hold their authored
  frame rather than falling back to 30 FPS, including ATOYOT frame 19.
- Apply ObjDesc biology subpalettes to matching original surface/texture palettes,
  following `CPartArray::SetPalette`, without suppressing them based on a guessed
  headwear count. Preserve the final texture remap's precedence. Indexed human
  head pixels are compared with real palette 0400008E. This is fixture evidence;
  all heritage/headwear combinations still require live visual acceptance.
- Remove Weeping Wand size/birth-rate overrides and fountain/torch/Still-emitter
  lifetime substitutions; use authored emitter scale, timing and motion. Preserve
  zero initial scale, and apply the missing Explode Z acceleration. Hidden/UnHide
  physics effects drive the portal bubble emitters, with body transparency
  independent of the particles' opacity. Blood retains its no-light behavior.
- Apply server NoDraw/Hidden to networked bodies regardless of item type, while
  allowing their portal particles to render. Item/admin marker visibility still
  depends on the actual flags sent by the server; all special marker classes have
  not been inspected live.
- Probe world-entry floors with the full character radius instead of an 8 cm
  sample. Sloped teleports at 15/30/45 degrees can now find a valid capsule position
  at the requested XY. Evaluate placement before waiting indefinitely for server
  unhide. Compute appearance foot offsets in root-local space to avoid applying
  pawn scale twice.
- Honor DAT minimum/maximum control dimensions and separate the jump meter fill
  from its text layer. Show the authored Height label and respect the 810-pixel
  maximum bar width. World drops use viewport pixels for picking, fixing the DPI
  coordinate mismatch in player/NPC give targets. Live player-to-player giving
  still requires acceptance with a second account.
- Move successful build stamps and portal/sky/head diagnostics to Log/Verbose.
  Keep actual failures and unsafe-spawn warnings visible. The user's latest
  preserved session, `Saved/Logs/ACUnreal-user-20260907-dungeon-streaming.log`,
  ends with normal shutdown; it is not evidence of a new crash stack.

Pedestal reference data is saved as `Tests/Fixtures/PedestalWeakSpot.sql`, from
[ACE World weenie 16919](https://github.com/ACEmulator/ACE-World-16PY/blob/master/Database/3-Core/9%20WeenieDefaults/SQL/Door/Misc/16919%20Pedestal%20Weak%20Spot.sql).
This is reference data only; no database migration was run. Reproduce the motion
inspection with `dotnet run --project Unreal/diag/UIInventory -- --pedestal-probe
"C:/Turbine/Asheron's Call"`. The primary animation values come from the local DAT.

Validation: ACUnrealEditor Win64 Development compiled successfully. Build
**2026.09.07.1** passed all **25 D3D12 offscreen groups**: 24 clean, one with
intentional unsafe-spawn warnings, zero failed/skipped/unfinished. The final
RuntimeActors test forces GC while the pedestal is faded out, verifies the
replaced material remains alive, and confirms the identical original material
is restored on each close. The run also covers already-open creation, actual
10/60 Hz movement, distant building admission, 130 Yaraq rendered views, dungeon
ceilings/residency, palette pixels, held emote frames, particles and jump UI.
Report: `Saved/Automation/RetailParity20260907-dungeon5-final/index.json`.
Log: `Saved/Logs/RetailParity20260907-dungeon5-final.log`. No NaN-bounds,
nearly-zero normals/tangents, or degenerate tangent warnings appeared. Remaining
startup warnings come from the engine layout service and third-party MCP plugin.
Reviewed capture: `Saved/Automation/RetailParity/GameplayJumpCharge.png`.

Live acceptance still required for this build: repeated pedestal use/reset and
re-entry; dungeon arrival with another player and loot; remote hair/eyes and held
emotes; player giving; sloped admin teleports and bubble exit; cold and repeated
Viridian Rise/Yaraq traversal with frame-time, actor-count and memory measurements.
The larger account/gameplay/interface checklist below remains open.

## Changes in 2026.09.06.8

- Investigated the actual inventory crash in
  `Saved/Crashes/UECC-Windows-1E429759433EF2919573C4884C46441A_0000`.
  Its stack is `RefreshSpellHotbarOverlays` -> `CollapseSpellBarWidgets`.
  `BuiltInSpellIconBorders` lacked a reflected reference: removed or unplaced
  widgets could be garbage-collected while still cached. Retain that array with
  `UPROPERTY(Transient)`. The regression unparents the cached widgets, forces GC,
  and refreshes the spell bar. Summoned-creature kills were concurrent with the
  user's crash; the stack does not implicate the creature death code.
- Camera-near doorway visibility: portal traversal must start at the eye, even
  though rendered triangles clip at the render near plane. Moving the camera to
  2cm/32cm inside the reported Yaraq portals reproduced missing room/stair pixels
  with the previous 10cm portal near-plane clipping. Move only the root traversal
  near plane to the eye; retain side, far and recursive aperture planes. Compare
  these views with the same all-resident geometry, including exterior terrain.
- The blue entrance had a second source in EnvCells 7E650100..106, environment
  0D000405: their CellPortal polygon IDs also referenced opaque blue 080006FD.
  Exclude these aperture polygons from EnvCell draw sections, just as retail
  `CEnvCell`/`BSPPORTAL` draws them through PView. Keep physics, portal vertices,
  and cell bounds. Bump EnvCell disk-cache schema to 12 and regenerate independent
  ACE.DatLoader drawable-side fixtures. Both shell and cell blue planes are tested.
- Runtime static meshes were being rebuilt by Unreal's editor once per material:
  post-build `SetMaterial` invokes `PostEditChange`, despite those same materials
  already being assigned before the fast build. Remove that redundant loop.
  This also removes the path producing NaN bounds and tangent warnings, and avoids
  repeated generation work. Verify finite bounds, preserved physics/materials,
  and `IsCompiling()==false` on completed runtime meshes. Cold live traversal FPS
  has not been measured by this change.
- Inventory shortcuts now match `gmToolbarUI::CreateShortcutToItem` and the
  inventory drop branch: remove an item's previous binding, assign the new slot,
  then put a displaced item into the first free slot to its right (with wrap).
  Support side packs, items inside them, and the main backpack/player shortcut;
  activating the latter opens inventory locally. Paint the same DAT numeral on
  occupied toolbar, inventory, equipped and pack icons. Native press/move/release
  coverage includes a refresh during drag and the user's larger viewport size.
- Resizing a window exposed stale inventory overlays: their cache included size
  but not the window's position. The chrome moved while item icons and click
  geometry stayed behind. Include viewport/scale and root-window geometry in
  inventory, vendor and loot caches.
- Mana/item targeting uses `ClientUISystem::UpdateCursorState` and DidMapper
  2500000F entries 39/40/41 (06004D73, 06005E6B, 06005E6A), with the authored 14,14
  hotspot. Match `ItemHolder::IsTargetCompatibleWithTargetingObject` for the cursor
  preview, including ownership, player-wide use and trade-offered exclusions.
  Give the software cursor an explicit Slate desired size so its drawn hotspot
  matches the pointer. The server remains authoritative about actual item use.
- Inventory paperdoll uses `gmPaperDollUI::PostInit`, `UpdateHeritage`,
  `RedressCreature`, and `Render::SetFOVInternal`: (.12,-2.4,.88) meters before
  coordinate conversion, 191.3679-degree creature heading, 45-degree vertical FOV,
  and heritage-specific camera distances/heights. Rotate the creature rather than
  its attached camera; invalidate the portrait when heritage changes. Render a real
  human DAT model through the live inventory rig to check geometry and framing.
  Full equipped-character lighting and all heritage appearances still need live
  side-by-side acceptance.
- Optional mouse turning now uses captured relative deltas immediately instead
  of dropping short movements until five consecutive nonzero samples. Preserve
  legacy movement when that option is off; distinguish orbit drags from right-click
  item use by accumulated pixel travel. Unit coverage exercises the actual camera
  delta path, pauses, short movements and pitch limits. Physical mouse capture and
  follow behavior still require live gameplay verification.
- Add the ACEWorldBake -> ACEClient plugin dependency declared by its module.
  Move routine successful loading, sky/particle telemetry and source-authored empty
  physics messages to Log/Verbose. Keep actual missing assets/build failures as
  warnings; do not suppress engine or third-party warnings globally.

Validation: ACUnrealEditor Win64 Development compiled successfully. The final
D3D12 offscreen run loaded build **2026.09.06.8** and passed all **25 groups**:
22 clean, three with warnings, zero failed/skipped/unfinished. Evidence:
`Saved/Automation/RetailParity20260907-final/index.json` and
`Saved/Logs/RetailParity20260907-final.log`. There were no NaN-bounds,
nearly-zero tangent/normal, or degenerate tangent warnings. The remaining test
warnings are portal-camera/sky diagnostics and intentionally failed world-entry
recovery scenarios; third-party plugin and editor-layout notices occur at startup.

The larger-viewport native backpack test failed before the overlay cache correction
(`RetailParity20260907g`) and passed afterward (`RetailParity20260907h`), including
main-pack activation and displaced bindings. The portal-near reference renders
failed before the frustum correction (`RetailParity20260907b`) and passed afterward.
The preserved user crash log is `Saved/Logs/ACUnreal-user-20260906-inventory-crash.log`.

Ready to try through `Unreal/Launch.bat`. Live acceptance still needs the user's
inventory/summon sequence, physical mouse capture/follow behavior, repeated
entrance traversal, equipped portraits and cold landscape frame times. No live
account was logged in during this verification. The complete client acceptance
checklist remains open; these fixes do not certify every retail feature.

## Changes in 2026.09.06.7

- Doorway and stairwell camera classification: use the actual player view origin
  for both PView and viewer-cell resolution. Classify the collision-resolved camera
  endpoint in resident cell BSPs before attempting the feet-to-camera path. That
  second path could hit a stair/floor and select the wrong room. Added the reported
  shop's 7E650100..10A cells and adjacent player/camera combinations: eleven of
  those combinations failed before this fix (`RetailParity20260906w-camera`).
  The scene fixture now compares 76 room views with all resident geometry.
- Blue doorway: GfxObj 010014C3 uses opaque blue surface 080006FD for a PORT
  polygon. Retail `CGfxObj::InitLoad`, `BSPPORTAL::portal_draw_portals_only` and
  `PView::DrawPortal` replace that plane with the portal view. Keep the aperture
  geometry for visibility traversal but remove PORT polygons from ordinary shell
  draw sections. Physics polygons are unchanged. The independent ACE.DatLoader
  fixture exporter now separately counts PORT apertures and drawable sides;
  the pedestal retains its two textured surfaces and black PORT aperture.
- Lighting: `SmartBox::SetWorldAmbientLight` uses raw RGB coefficients, and
  `LScape::calc_object_light` includes 0.2 times sunlight magnitude. Apply those
  values to cached and newly created shell/scenery materials, eliminating the
  previous shell ambient mismatch between creation and updates. `CellManager`
  supplies constant 0.2 white ambient to sealed cells; outdoor-visible rooms use
  the current outdoor ambient. Avoid resending unchanged ambient material values.
  Creature/player shadow casting is no longer explicitly disabled on entering an
  EnvCell. UE directional shadow maps remain an approximation, not certified
  equivalence to every retail shadow/lighting case.
- Movement/animation: retain MoveTo flags, walk/run threshold, speed and run rate.
  `MovementParameters::get_command` controls the gait; a large run multiplier
  does not turn a walk-only path into running. Read authored motion-cycle velocity
  for each model and clamp the final approach step. Cache motion tables by DID
  and use cached animation references instead of copying all frames per sample.
  StreamingCost exercises table alternation and walk/run transitions; a cold
  landscape walk still requires live frame-time acceptance.
- Native UI input: honor DAT panel-page tab IDs and Menu hit targets, and route
  Slate's distinct double-click event through inventory activation. Actual canvas
  press/release tests cover Say, vendor tabs/filter, mana stones and trade controls.
  Trade now displays names, totals, item backgrounds/counts, scrollable horizontal
  offers, acceptance state and the authored `Trade_ClearAllButon` action. Preserve
  DAT dimensions and scale overlay positions with the canvas.
- Inventory: restore the held caster's innate spell and caster icon to BuiltInSpell;
  draw bound shortcut numerals on equipped slots and side packs as well as grid
  items. Equipment slot coordinates and empty icons were checked against the
  resolved DAT templates. Equipped-slot tool drops target that item; the blank
  doll targets the player. Double-clicking a mana stone arms targeting without
  discharging it onto self. `ItemHolder::TargetAcquired` blocks draining Retained
  items; an empty stone requires the retail Yes/No template and destruction prompt
  before sending UseWithTarget. Dialog height follows the DAT glyph layout.
- Optional mouse-turning controls: stationary RMB orbits; movement turns the body
  smoothly toward the view while keeping camera heading continuous. RMB+LMB runs;
  A/D strafe in this mode. This is the user's requested control behavior, not a
  claim about original retail input. Unsupported ledges use support-boundary
  projection instead of alternating sideways nudges. CameraAndEdges checks frame
  rate independence and repeated stable edge sliding.

Preserved user log: `Saved/Logs/ACUnreal-user-20260906-ui-lighting.log`.
The revised UI, world-DAT and scene groups passed in `RetailParity20260906z`;
build 2026.09.06.7 compiles as ACUnrealEditor Win64 Development. Final D3D12
offscreen validation passed all 25 groups: 14 clean, eleven with warnings, zero
failed/skipped/unfinished (`Saved/Automation/RetailParity20260906aa-final/index.json`,
`Saved/Logs/RetailParity20260906aa-final.log`). That process loaded build .7 and
verified 76 Yaraq views, native mana confirmation text, trade actions, indoor
creature shadow retention and sealed/outdoor ambient transitions. Existing engine
transient mesh-bounds/material diagnostics and intentional recovery warnings remain.
Use `Unreal/Launch.bat` to try it.
These checks do not certify all inventory, trade, vendor, animation, lighting,
world traversal or networking behavior as a complete retail replacement.

## Changes in 2026.09.06.6

- Traversal investigation: preserved the user's session as
  `Saved/Logs/ACUnreal-user-20260906-traversal-weather.log`. Initial appearance
  application coincides with long frame gaps; warmer models cost less. The mesh
  builder decoded the same appearance-dependent texture and palette for every
  polygon side. Retail `D3DPolyRender::ConstructMesh` shares surfaces between
  subsets. Resolve once per surface in each part, retaining authored geometry,
  UVs, vertex colors and distinct front/back sides. The actual DAT human fixture
  now makes 23 surface resolves instead of 383 for 409 triangles. The isolated
  cold build fell from 42.85 ms to 2.80 ms; warm from 43.88 ms to 2.51 ms. Baseline:
  `RetailParity20260906k-baseline`; fixed: `RetailParity20260906l` in Saved/Automation.
  These numbers measure CPU mesh preparation, not total frame time.
- Loading cadence: the player's landblock now charges the same shared 4 ms
  scenery budget as its neighbors; failed/pending attempts also stop at the
  deadline. Ready terrain application stops starting more blocks once 4 ms have
  elapsed, preventing the old multiple-block burst on one frame. Retail
  `LScape::PreFetchCells` and `update_block` use resident cached blocks; retain
  the existing asynchronous DAT builds and residency ring. Added slow appearance
  stage timings (`ACEAppearance cost`) to distinguish decode/component work from
  mesh/material application in subsequent live traces. Individual synchronous
  texture uploads, collision work and changes to terrain LOD can still overrun a
  budget; cold long-walk frame-time acceptance remains open.
- Shadows: UE 5.8 `FScene` constructs `PrimitiveOctree` with
  `UE_OLD_HALF_WORLD_MAX` (about 10.5 km). AC positions extend to about 49 km;
  Yaraq was outside its shadow-culling root. A control scene cast shadows near
  the origin but failed at its real coordinates. Set
  `r.Shadow.UseOctreeForCulling=0` in startup and config to use Unreal's alternate
  primitive gather with frustum/distance checks, retaining AC coordinates.
  Actual DAT shells now cast shadows at Yaraq across all 232 keys in 20 day
  groups; instanced foliage 02000322 passes independently. This Unreal culling
  correction is not a claim that CSM reproduces retail's entire shadow renderer.
  Corrected the earlier shadow test: unsigned pixel changes and suppressed shell
  color could pass from noise/depth artifacts. It now keeps identical geometry,
  requires net darkening, and separately isolates building receivers.
- Rain restart: environment `StopAllEffects` was restoring the sky actor's mesh
  materials, cloning the MIDs still referenced by the sky animation slots.
  Streaks returned on exit but UV updates went to stale materials. Weather owns
  its particle visuals, so stopping it no longer modifies the sky host materials.
  GPU checks now verify visible, animated rain after entering and leaving a room.
- Fog: follow `RenderDeviceD3D::SetFFFogProperties` and its linear range fog.
  Remove the invented 0.65 power curve and unconditional 900 m cap; honor
  interpolated DAT start/end, including zero start. Update cached outdoor lit
  scenery as well as terrain and shells. Outdoor fog remains active when seen
  from indoors; EnvCell materials use their own indoor state. Tests cover cached
  and newly created materials across weather/indoor changes. Per-cell fog and
  exact retail sky/depth compositing still need broader visual acceptance.
- Particles: follow `ParticleEmitter::UpdateParticles/RecordParticleEmission` and
  `ParticleEmitterInfo::ShouldEmitParticle`: update existing particles, then emit
  at most once and reset the emission time to now. Remove the 30 Hz minimum birth
  interval and hitch catch-up bursts; newborns retain age zero for their birth
  frame. Update each visual's transform once instead of three separate updates.
  The DAT wand fixture 320005A8 tests birth timing under a 200 ms hitch. Blood
  stays non-luminous; spell/buff/weapon effects retain lights. The decompile omits
  the x87 comparison in distance-based emission; that path, light strengths,
  long-hitch clocks and all particle equations are not yet certified 1:1.

Build 2026.09.06.6 compiles as ACUnrealEditor Win64 Development. Full D3D12
offscreen validation passed all 24 groups: 14 clean, ten with warnings, zero
failed/skipped/unfinished (`Saved/Automation/RetailParity20260906r/index.json`,
`Saved/Logs/RetailParity20260906r.log`). A subsequent test-only correction waits
for the foliage shader before measuring leaf cutouts; RuntimeActors passed again
in `RetailParity20260906s`. Isolated building receiver darkening was 3,595,893 RGB
units and textured foliage shadow darkening 714,757; capture pairs are under
`Saved/Automation/RetailParity/YaraqBuildingReceivers_*` and `YaraqFoliage_*`.
The final suite measured human preparation at 3.08 ms cold / 2.70 ms warm.
Existing engine mesh bounds/material warnings and deliberately exercised failure
paths remain. No live account or live traversal FPS test was used. Launch with
`Unreal/Launch.bat`; live cold-cache movement remains an acceptance check.

## Changes in 2026.09.06.5

- Indoor login deadlock: the preserved log
  `Saved/Logs/ACUnreal-user-20260906-blocked-entry.log` shows cell 7D630112
  loaded, followed by more than 76 seconds waiting on `cell-collision`. Portal
  visibility returned before collision activation; readiness waited for that
  activation before releasing portal space. Destination physics now prepares
  while its rendering remains hidden. WorldEntry tests the actual DAT room at
  local (36.71, 89.43, 9.20), through the presenter's portal visibility path.
- Safe world entry: check walkable support and a non-overlapping player capsule
  before arrival/reveal and again at the camera cut. Try nearby supported
  placement within four AC units; retain the validated position through reveal.
  Collision/placement failure after 45 seconds requests one server TeleToLifestone
  (0x63), using the character's server-owned Sanctuary. This requested recovery
  behavior is an extension; it is not attributed to retail. Existing server recall
  eligibility, animation delay and mana cost apply. A new teleport destination
  resets readiness and LoginComplete state without allowing another recall. If
  no destination arrives within 45 seconds, or the lifestone also cannot load a
  safe placement, disconnect and explain the failure on login rather than reveal
  missing terrain or loop forever. WorldEntry verifies the actual loopback packet,
  one-attempt limit, destination reset, missing floors, obstacles and both failure
  paths. Live server acceptance of recall is still a manual check.
- Portal movement: match `gmSmartBoxUI::PostInit/UseTime`,
  `UIGlobals::Init/GetAnimLevel` and `CreatureMode::SetCameraDirection_Degrees`.
  Use the 100-entry quantized sine-integral easing table, direct numeric angle
  interpolation, eye (0.24, -2.7, 0.88), and AC-to-Unreal roll handedness. Keep DAT
  animation 030005AC, start frame 1 at 40 fps, interpolating display frames. One
  owner advances the appearance clock across repeated portal visits. PortalSpace
  verifies curve samples, eye, roll, rate, repeated visits and rendered captures.
- Animation stepping: a .04 transform tolerance was also used for quaternion
  components, discarding several degrees of rotation. Translation retains its
  .04 cm tolerance; rotation and scale have separate .00001 tolerances. The
  portal regression now observes 120 distinct poses in 120 simulated display
  frames. This also removes that quantization from creature part animations.
- Loading cadence: active editor play clients opt out of Unreal's 3 fps background
  throttle via a delegate scoped to their lifetime, without editing global user
  preferences. Portal scenery uses the normal shared 4 ms budget and includes the
  player landblock in that budget; rooms use 2 ms and distant chunks one per call.
  Appearance work has a 2 ms/two-object portal cap and a 4 ms normal cap even when
  boosted. Individual synchronous asset builds/collision cooks may exceed a budget;
  live cold-load and long-walk frame times still require measurement. Portal
  illumination and complete rendered comparison against a running retail client
  remain open; the camera/animation tests do not establish complete portal parity.

Final validation: ACUnrealEditor Win64 Development rebuilt successfully as
2026.09.06.5. The complete D3D12 offscreen suite passed all 22 groups: 13 clean,
nine with warnings, zero failures, skipped or unfinished tests. Final report:
`Saved/Automation/RetailParity20260906j/index.json`; log:
`Saved/Logs/RetailParity20260906j.log`. Existing engine mesh/material warnings
remain, and the recovery tests deliberately exercise logged failure paths.
Portal captures are `Saved/Automation/RetailParity/PortalSpace0.png` through
`PortalSpace2.png`. The exact user destination resolves to UE capsule center
(-2403671.094, 1909742.969, 1008.500), retaining the original XY and supported feet.
The final teleport check also keeps autonomous position reporting disabled until
the validated destination is revealed. No live account was used in the tests.
Use `Unreal/Launch.bat` for the rebuilt editor client. Verify real lifestone recall
and cold-cache landscape traversal in play; complete retail parity remains open.

## Changes in 2026.09.06.4

Final validation: ACUnrealEditor Win64 Development compiled successfully. All
20 groups passed with D3D12 offscreen rendering (13 clean, seven with warnings,
zero failed/skipped) in `Saved/Automation/RetailParity20260906f/index.json`;
log `Saved/Logs/RetailParity20260906f.log`. Warnings include transient static-mesh
bounds during engine serialization and material/cache diagnostics. The completed
mesh bounds and collision assertions pass; those engine warnings remain open.
`Unreal/Launch.bat` opens the rebuilt editor project for manual testing.

- Building-entry crash: the actual .3 minidump resolves to interior ISM instance
  insertion after failed body initialization. DAT decorations without PhysicsBSP
  now keep physics disabled in both interior and outdoor ISMs; solids retain
  collision. See [crash evidence](CRASH_20260906_BUILD3.md). A new regression
  repeatedly enters the three crash rooms and adds their real DAT furniture.
- Streaming work: remove full ISM physics recreation after every instance;
  combine room collision triangles before a single synchronous cook; update
  portal stencil render state only when it changes. Remove the landscape
  presenter's competing cache trim (192 setups), leaving the DAT subsystem's
  existing centralized budgets (2,048 setups). These eliminate identified
  repeated work; live long-walk frame times still need measurement.
- Setup lifetime: the first full .4 run exposed an array-iteration failure in
  building mesh creation when background completions changed the cache during
  material work. Setup cache entries now have stable shared ownership. Static,
  procedural, portal-space and sky builders retain their geometry while doing
  work that may pump game-thread tasks. Late duplicate builds cannot replace a
  completed setup; stale generations cannot consume a new pending request.
- Effect lighting: exclude the twelve directional blood splatter PlayScript IDs
  `0x5B..0x66` in both particle-light selection and SetLight hooks. Remove the
  color heuristic and blanket player-owner exclusion, retaining lights for red
  spells, buffs, weapon and wand effects on players and other actors. Source:
  retail `PScriptType` and server `PlayScript`. Particle brightness inferred from
  art remains an approximation and requires further retail comparison.
- Building shadows: shell shadow passes use two-sided casting for thin DAT
  polygons; main-pass material sidedness stays authored. The original scene test
  was insufficient evidence of working shadows. See .6 for its correction and
  the actual world-coordinate shadow-culling fault.
- Creature inspection: use the nine `2100006B/10000166` attribute rows in retail
  order, their fonts/margins/alignment, health percentage format, unknown values,
  and appraisal highlight/color masks decoded from the server's ushort fields.
  Creature types resolve through `25000001 -> 2200000E`; enum underscores become
  spaces as `AppraisalSystem::InqCreatureDisplayName` does. Restore the Character
  and Level labels using the English DAT strings. This is the basic creature
  pane, not complete player/allegiance or extra-rating inspection.
- Item inspection: retain value/burden presence instead of showing unknown as
  zero. Insert those fields into the body when optional controls are absent,
  following `ItemExamineUI::SetAppraiseInfo`. Preserve weapon/armor lines following
  spell lists. Updating the same appraisal retains its scroll widget/offset;
  a new target starts at the top. Actual scroll-box ancestors clip the bitmap
  text; a DAT clip projected from moving child geometry no longer erases the
  lower part of scrolled descriptions.

## Changes in 2026.09.06.3

- Window paint order: generated text, inventory items, chat, sliders, radar, and
  other overlays follow their owning gameplay window when it is raised. Cached
  content also moves with the window. DAT chrome and content share an ordering
  band; popups and drag images have their own top layer. Canvas Z values remain
  within exact single-precision integer range, as Slate stores them as floats.
  Basis: `UIRegion::DrawChildren` and the retail element hierarchy.
- Input order: a foreground window consumes clicks on its empty body. Covered
  inventory cells cannot be selected through either widget hit testing or the
  coordinate fallback. Covered sliders, paperdoll controls and item drop targets
  reject the hit. Unsupported drops over a window do not target the world behind
  it. Loot double-click activation waits for release, allowing select-then-drag;
  returning a drag to its source does not arm a new double-click.
- Say menu: uses classic_chat 21000006, popup 1000001C and entry 1000001E, including
  191x17 cells, two columns, bitmap font 40000001, text margins and DAT state art.
  Menu insertion follows `gmMainChatUI::InitTalkFocusMenu`: squelch selected,
  monarch, selected, patron, Say, vassals, fellows, allegiance, General, Trade,
  LFG, Roleplay, Society, Olthoi. Selected-player tell/squelch are wired and
  disabled without an eligible selection. Membership/listening-dependent state
  and all localization strings still require complete parity validation.
- Inventory activation: owned-object decisions now use retail's combat-use,
  WieldOnUse/WieldLeft flags, equipment masks and salvage ItemType. Charges no
  longer override targeted use or block advertised wielding. Explicit No and
  wield-only useability are respected; owned container shortcuts open their
  contents locally. Ordinary activation of equipped items retains the retail
  PlaceInBackpack branch. Source: `ItemHolder::DetermineUseResult/UseObject`,
  `ItemUses`, and `CPlayerSystem::UsingItem`. UIScreens checks actual constructed
  Use/Wield packets through an isolated loopback socket.
- Transport: incoming fragment data is held locally until checksum validation and
  contiguous packet ordering succeed. Partial-message assembly occurs during
  ordered delivery, preventing damaged, duplicate, or early packets from mutating
  live reassembly. Fragment indexes/counts and count arithmetic are validated.
  TimeSync likewise takes effect after authentication and ordered delivery.
- Outgoing long messages: every 448-byte fragment is sent in its own independently
  sequenced/retransmittable UDP packet, respecting the 464-byte payload budget.
  Previously all fragments were concatenated into one oversized packet.
  Basis: `FlowQueue::EnqueueBlob`, `Source/ACE.Server/Network/PacketFragment.cs`,
  and `NetworkSession` packet processing and assembly.
- World-entry handshake: F7C8 waits for F7DF before sending F657, as
  `CPlayerSystem::LogOnCharacter` does. Unknown and pending-deletion characters
  cannot enter. Duplicate readiness does not resend entry. Rejected entry returns
  to character selection so another attempt can be made.
- Mesh creation finalizes cache-version invalidation before obtaining a setup
  cache pointer. Tests now check the completed static render/collision meshes for
  finite bounds, separately from engine build/serialization warnings.

## Acceptance still required

### Account and character lifecycle

Character creation is implemented and has the offline validation listed above;
live creation and first-world-entry acceptance remain outstanding. Delete
confirmation, restore and credits actions remain explicit stubs in
`ACEUICharSelectBinder::OnElementActivated`. Selection of deleted characters, restore availability,
slot capacity, countdowns, and failure dialogs must follow
`gmCharacterManagementUI::UpdateButtons` and its dialog handling. The layout
manager currently forces RestoreCharacterButton hidden; this must be removed as
part of the functioning restore workflow. DDD asset patch negotiation is not a
complete replacement for retail patching; current setup uses server-side DDD off.

### Landscape, buildings, collision, and streaming

The geometry fixes have focused DAT and runtime coverage: camera-based PView
updates, DrawingBSP PORT apertures, terrain masks, room lighting, PhysicsBSP
collision, room residency and scenery reuse. The scene suite now checks 130 Yaraq
views, including cameras next to doorways and stairwells, with surrounding terrain
and resident cells in its reference. This is not exhaustive coverage of all buildings,
dungeons, water, slopes, portals, and transitions. Reproduce threshold crossing
in both directions with both the camera and player, test basements and stairs,
and measure frame time, memory, load queues, and visible pop-in on long walks.
No measured retail-equivalent traversal performance claim has been established.

### Animation, movement, combat, item use, and effects

Death actions, held corpse poses, cancellation of idle fidgets, and elapsed-time
animation throttling have regression coverage. Complete live acceptance still
includes melee facing and repeat/height/power controls, missile timing/ammunition,
spell windup/release/cancel, death/resurrection, motion links during approach,
movement prediction and correction, portal transport, swimming/jumping/falling,
item cooldowns, and interactions under network delay/loss.

The user has subsequently reported summoned creatures killing enemies, but all
summoning paths have not been accepted end to end. Earlier traces showed pet essence
use without an ensuing pet ObjectCreate. `PetDevice::ActOnUse/SummonCreature` can fail
before creation; those traces alone did not prove the cause. Capture
the client request, matching server outcome, UseDone/error/cooldown/structure
updates, and resulting pet creation before claiming it fixed. Particle/light,
weather, sound, emitter lifetime and animation-hook parity need live comparison
beyond the current fixture coverage.

### Interface and gameplay feature coverage

Audit every reachable retail panel and control, including inspection detail
formatting, equipment/preview, inventory/containers/stack splitting/merging,
shortcut binding, trade, vendors, salvage/crafting, spellbook/components,
attributes/skills, effects/vitae, fellowship/allegiance/friends/squelch, housing,
quests/contracts, books/inscriptions, options/key bindings and modal dialogs.

Owned inventory activation now follows the focused retail decision tree, but the
complete `ItemHolder` behavior is not yet transcribed. Remaining work includes
force-use, full target compatibility and target acquisition, craft/volatile-rare
confirmations, component-pack rules, ground and
vendor contexts, and the complete AutoWear/AutoWield conflict sequence. The existing
mana-stone targeting, charge flag and destruction confirmation now have native
input and packet coverage; live application/consumption remains to be accepted.
Editable text controls, popup enabled states, localization,
context actions, tooltips, drag cursors, and interaction timing are not all proven
1:1. Appraisal uses the authored layout, but layout agreement alone does not prove
that all text/content generation matches `gmExaminationUI`. In particular, the
item appraisal formatting still uses an interim summary for several fields.
Full weapon/armor/magic/requirements formatting remains on this checklist.
Character/creature inspection now has the focused layout, header, detail-row and
rendered-preview coverage recorded above; live and rare-profile acceptance is
still pending. Editable inscriptions now
have input/protocol coverage; live server persistence remains to be accepted.

### Networking and release gate

Chat editing was compared directly with retail `ChatInterface` and
`ClientCommunicationSystem` for Quest 41 / Windows 2026.09.17.51. Focused entries
now receive Up/Down before Slate consumes them, retain separate 100-line window
histories including duplicates, and expand reply shortcuts while typing. Retell
uses the outgoing recipient; patron/monarch replies retain their own last sender.
Native Quest text callbacks and the PC VR keyboard share this behavior. Actual
Slate input and isolated loopback packets cover history, reply recipient stability,
tells, channel aliases, and unknown command forwarding. See
`Quest/Validation/v41-chat-parity.md` at the repository root for sources,
test results, and native-keyboard acceptance limits.

NetworkTransport now covers real datagram parsing, CRC failures, ordering,
duplicates, malformed fragment indexes, authenticated time, world entry,
loopback fragmentation, encrypted NAKs, retry checksums and rejected recovery
requests. Still exercise live session loss/recovery, long-running encrypted
key windows, multi-queue delivery, movement timestamps, logout and
relogin, all gameplay responses, and server error paths. Large-message delivery
must remain within packet budgets for character generation and bulk operations.
The client's contiguous packet dispatcher still differs from retail's per-flow
delivery. Rejecting an unrecoverable hole allows subsequent complete messages
through but cannot recover its lost payload. Assembly storage is now bounded,
but lost reliable messages and independent gameplay queue delivery still need
further review under sustained loss.

A release can be called a retail replacement only after the account lifecycle and
all required gameplay/UI flows work against an unmodified compatible server,
with comparison evidence and no open blocking discrepancies. A numbered build
provided for testing does not meet that gate merely by compiling or passing the
current suite.


## Chess lifecycle and rating review (2026-10-09)

Compared local retail `gmMiniGameUI`, `GameBoardGrid`, `CM_Game`,
`ClientMiniGameSystem`, and `gmUIElement_MiniGameIndicator` with ACE's chess
messages, match logic, world-piece motion, and rating persistence.

- World-board use now sends retail ChessJoin (board GUID, either-color -1).
  Duplicate pending joins are suppressed and active matches cannot join again.
- The native DAT board retains its textures, piece icons, and orientation for
  each color. Input waits for StartGame, the player's turn, and confirmation of
  the preceding move. Rejected moves leave the board unchanged; unrelated-board,
  duplicate, and truncated notices do not mutate the game.
- Stalemate can be offered/retracted. Resign uses the native confirmation dialog;
  closing the panel preserves the match. The game indicator tracks participation.
  Joining reveals the native board in VR; the VR More menu can reopen Game Center.
  Game-over and logout clear the board's pending state.
- ChessRank private-property updates refresh Character Information immediately,
  including decreases and zero. Rating computation and persistence remain server-side.
- Server regression uncovered broken checkmate detection and reversible move
  search (turn, captured color, promotion, castling and shared castling rights).
  Fixed those paths, self-check validation, castling through attacked empty squares,
  and captured-rook castling rights. AI searches legal moves without corrupting
  the board. Match results are sent after the world-piece motion completes, with
  checkmate/stalemate terminating the match and the existing rating/counter policy
  applied. Duplicate joins cannot seat the same player on both sides.

Validation: Development editor build passed; `ACE.RetailParity.Chess`,
`CharacterInformation`, `Housing`, `UIScreens`, and `ACE.VR.RigAndMenus` passed
with no test warnings (five unique client tests, six server tests). The VR rig
regression confirms opening chess reveals its interactive panel.
The chess fixture checks actual loopback action packets, server-event decoding,
rendered white/black boards and confirmation, move rejection, special moves,
draw/resign/end notices, malformed notices, and live rating updates.
`ACE.Server.Tests.ChessTests` covers complete checkmate, legal/illegal moves,
search undo, special moves, AI, and actual rating/counter updates on isolated
in-memory player records, including persistence dirty flags. It does not write
real character data. Evidence: `Saved/ChessReview` and `Saved/Automation/Chess`.

Limits: no live two-client retail/Unreal match or world-piece rendering session
was performed. Server rule fixes require the updated ACE server; client fixes
also work with other servers using the existing retail chess protocol.

### Scroll learning eligibility (2026-10-09)

Retail explicitly permits level I and VII scrolls without training or skill
(June 2002 Castling announcement, “How Players Learn Spells”:
https://asheron.info/wiki/index.php/Announcements_-_2002/06_-_Castling).
ACE's `Player.CanReadScroll` extends the exemption to all power >= 300,
including VIII. Levels II–VI require a trained/specialized school and current
skill of at least 0/50/100/150/200 respectively (spell power minus 50).
Learning a spell does not guarantee being able to cast it.

UCM's unknown-scroll filter incorrectly used power minus 15 and omitted the
training check and exemptions. `ACEScrollLearning` now supplies the correct
learning policy to its inventory/corpse snapshots. Eligibility refreshes with
live skill changes. Explicit scroll Use remains server-authoritative: ACE
rejects already-known or ineligible spells before consuming the scroll, and
only adds a spell after a successful inventory consumption. NPC/admin spell
grants are separate and are not subject to scroll-only requirements.

Regression coverage includes all five magic schools, trained/specialized and
untrained states, exact II–VI thresholds, I/VII/VIII exemptions, real DAT spell
metadata in UCM inventory snapshots, live eligibility cache invalidation, and
existing unknown/known/duplicate scroll loot-and-read flows. No live server
scroll consumption session was performed.

Validation: Development editor build succeeded. ScrollLearning, LegacyAdapters, and RouteAndTellRequests passed (three tests, zero errors). The broader request fixture emits 24 missing-position JSON warnings from its existing snapshots; the new scroll snapshots initialize position. Evidence: Saved/ScrollReview/build.log and Saved/ScrollReview/Tests/index.json.

Release v105 validation: the packaged Windows client passed 15 focused suites, including chess, Olthoi previews/targeting/chat, enchantment panels, plugin requests, VR menus, and updater behavior. The packaged chess fixture now makes its loopback sender nonblocking and closes it on scope exit, matching live session socket ownership; otherwise the following subsystem tick could block on an idle test socket. One chat clipboard run failed while Windows denied clipboard access; the final run passed. See Saved/ReleaseValidation/v105.

### Aetheria during casting (2026-10-14)

Human MagicPowerUp08 (`030005A0`) creates emitters in slots 5–9, which
overlap all five Aetheria surge scripts (`3300125B`–`3300125F`). The prior
particle test accepted that windup destroying a surge. A complete cast now
has to preserve its visual through the same natural expiry as an idle body.

The available retail decompile's `ParticleManager::CreateParticleEmitter`
(`0051B6C0`) replaces occupied slots; it does not establish the reported
retail behavior of surges surviving casts. The UE renderer therefore makes
an explicit compatibility exception: each Aetheria kind owns isolated slots.
It still uses the DAT's emitters, orbit, birth intervals and lifetimes. No
timer is restarted by casting and no enchantment-duration approximation is
used. A new proc replaces only its previous visual, including when enum and
direct-script messages are mixed. Ordinary effect slots retain their existing
replacement/stop behavior.

`ACE.Rendering.ParticlePresentation` compares all five surges on local and
remote casting bodies with an idle reference, including repeated full windups,
release, cancellation, appearance refresh, explicit stop/destroy hooks and
reprocs. Before the fix, casts ended these effects after about two seconds;
their DAT-driven idle lifetimes are approximately 16–29 seconds. This is not
evidence of identical retail internals; live client comparison remains required.

Validation: Windows Development editor build succeeded. ParticlePresentation,
ParticleTiming and ParticleBatchLifetime passed with zero errors. ParticleTiming
emits two existing unresolved PlayEffect `0x41` fixture warnings. The presentation
test also samples the authored orbit during casting and verifies exact expiry
against the idle reference. Evidence: `Saved/AetheriaCastingReview/Before` and
`Saved/AetheriaCastingReview/After`.

### Desktop particle brightness and color (2026-10-14)

Retail `RenderDeviceD3D` disables both `D3DRS_SRGBWRITEENABLE` and
`D3DSAMP_SRGBTEXTURE`. `D3DPolyRender::SetSurface` (`0059C4D0`) uses
SRCALPHA/ONE for Alpha|Additive and ONE/ONE for additive-only surfaces.
These contributions accumulate in display color space, with channel saturation.
Unreal's linear scene blending followed by filmic tone mapping changes that
result, even when source texture colors and particle lifetimes are correct.

Desktop SDR views now retain additive particle contributions in the AfterDOF
buffer and composite them after tone mapping. Source alpha/fade is applied once,
pre-exposure is canceled, and overlapping glows use retail's display-space sum.
Other DAT transparency uses the world pass; world lighting and tone mapping
remain intact. The isolated pass preserves opaque depth occlusion. It requires
separate translucency and disables depth of field for these views so the engine
cannot precompose the reserved buffer. AC gameplay normally has DOF disabled.
The new shader module loads at PostConfigInit; the normal material bake must
regenerate runtime parents before packaging, and Quest source sync includes the
shader directory.

The final-output GPU regression exposed what earlier white/HDR-only checks
missed: with filmic processing, white measured approximately 0.729 instead of
1.0, and RGB (128,32,224) became (0.227,0.005,0.651). The corrected output is
approximately (0.502,0.125,0.878), including with filmic processing enabled.
Coverage includes pooled and batched sprites, colored backgrounds, overlapping
layers, alpha/fade values, foreground occlusion, FXAA/TAA/TSR, and actual DAT
surfaces for the Emissary, island portal and Tower Guardian's sword. Additive-only
surfaces ignore unused texture alpha, while Alpha|Additive preserves it.

Validation: Windows Development editor build succeeded; all 11 focused rendering,
particle, material-packaging and VR widget tests passed with zero errors. The
fixtures emitted two unresolved PlayEffect `0x41` warnings and one transient-world
actor cleanup warning. Evidence: `Saved/ParticleColorReview/final-build.log` and
`Saved/ParticleColorReview/Final/index.json`. Aetheria casting/lifetime regression
remains green. No live side-by-side retail session was performed in this pass.

Limits: this composite targets desktop SDR. HDR output, stereo VR, mobile,
raw HDR captures, debug views and views without separate translucency retain
their existing native composition. Mobile LDR already blends in display space.
Linux/Quest builds, HDR/VR visual comparisons, translucent-surface interleaving
and exact retail texture-filtering parity were not validated by these pixel tests.

### Pickup approach cancellation (2026-10-15)

Retail `ACCWeenieObject::UIAttemptPutInContainer` sends the inventory request
once. Server-directed movement then uses the normal MoveTo path and
`CommandInterpreter::SendPositionEvent` to report progress; arriving is not a
new player movement command. ACE's `GameActionMoveToState.Handle` cancels an
active `CreateMoveToChain` when a new movement command arrives.

The desktop controller protected ordinary Use approaches from those commands,
but missed inventory approaches because they do not set the UseDone busy flag.
The regression reproduced two unsolicited MoveToState packets while walking to
and arriving at a token. Those packets could cancel pickup, leaving the player
beside the item until F was pressed again. The movement-send gate now covers
every active server approach. Position reports continue normally, and explicit
movement still cancels the approach. Inventory contents still change only after
the server confirms the transfer; no pickup retry or range override was added.

`ACE.RetailParity.WorldLootPlacement` checks the real outgoing action packets,
arrival with delayed confirmation, a single pickup request, position reporting,
manual movement cancellation, and blocked approaches using retail castle data.
Evidence is under `Saved/PickupApproachReview/{Before,After}`.

Validation: Win64 Development editor build succeeded. WorldLootPlacement,
WorldEntry, InteractionRecovery, PvPCombat and EmoteTransitions passed with no
test errors. This was isolated packet/geometry testing, not a live-server run;
Linux and Quest binaries were not built in this pass.

## Portal-space interface visibility (2026-10-21)

Retail `gmSmartBoxUI::UseTime` switches the SmartBox world view to its portal
viewport; it does not hide the gameplay interface. `CPlayerSystem::SetTeleportInProgress`
increments the UI busy count, and `ClientUISystem::UpdateCursorState` selects
cursor enum 14 (DAT mapper `2500000F`, image `06004D74`, hotspot 0,0).
`ClientCombatSystem::SetCombatMode` rejects player-requested mode changes while
teleporting but accepts server changes. `ItemHolder::UseObject` uses inventory
request readiness, not a blanket interface lock.

The client now retains the existing canvas/binder, open windows, chat history
and drafts during transitions. Initial login displays the gameplay interface
after bounded texture prefetch, before the tunnel finishes. Chat, window
shortcuts and manual interface visibility remain available. Movement, world
approaches and combat requests are blocked during the transition; held/repeating
attacks and pending world-target gestures are cancelled. Server updates continue
to refresh the interface. The retail busy cursor and portal notice are restored.
This does not grant inventory actions that the server rejects.

Validation: Win64 Development editor build and all seven tests passed:
PortalInterface, LoadingTransition, PortalDrops, ChatInputAndCommands,
KeyboardBindings, UIInteractions and PvPCombat. Reports are in
`Saved/PortalInterfaceVerified`. Shared Quest source is synchronized. This was
DAT/loopback automation; live-server portal travel and Linux/Quest binaries were
not tested in this pass.

## Creature locomotion and corpse placement (2026-10-21)

The same remote motion path now covers rabbit, chicken, rooster and chick DAT
setups, alongside humanoids, carenzi, gromnies and phyntos wasps. Cycle speed
and displacement come from animation position frames plus MotionData physics;
missing explicit Velocity fields no longer substitute human walking speed.
Sidestep remains the additive motion-table modifier. Physics advances the
animation clock even when presentation evaluation is throttled.

Retail `CSequence::update_internal` consumes positive frames below High, then
`advance_to_next_animation` enters the next positive clip at Low. A zero-rate
Dead cycle adds no root movement. Reverse traversal subtracts Low on exit.
The root tracks now follow those boundaries, retain unfinished entry/exit links
on StopCompletely, and start the death motion after those links. This also
corrects the shared jump-to-run landing endpoint to 0.741333m (four 0.152m
landing steps plus the run cycle's 0.133333m entry).

ACE `Creature.CreateCorpse` uses the victim's physics position, setup, motion
table and scale. Corpses now retain that authoritative frame and use authored
body support rather than dropped-item mesh-bottom alignment. Before this fix,
the latter lifted tested corpses approximately 6–43cm. Incline support uses the
DAT spheres against the floor plane, avoiding repeated penetration corrections
that pushed small creatures sideways. Ascending jumps and gravity-disabled
actors are not forced onto the floor by the ballistic landing path.

Validation: Win64 Development editor build and all ten suites passed:
RemoteMovementReplay, WalkingStep, MovementReview, RunSpeed, RemoteSupport,
NetworkTransport, GroundItemPlacement, WorldLootPlacement, AppearancePlacement
and AvatarMotion. Evidence: `Saved/CritterCorpseReview/final/index.json` and
`final.log`; the independent ACE.Server Sequence probe and output are retained
under `Saved/CritterCorpseReview/ServerProbe` and `server-probe-zero.log`.

The replay includes 168 creature trajectories, 64 independent ACE physics death
endpoints at 30/60/144/240 FPS, and 96 corpse handoffs on flat/sloped ground and
in air, including delayed appearance construction. Maximum endpoint error was
0.00007cm; maximum corpse mesh difference was 0.563cm, including one physics
frame of falling while appearance was deferred. Shared Quest source is synced.
This verifies deterministic DAT/physics and packet fixtures; no live retail
side-by-side capture or Linux/Quest binary test was performed. It is not a
claim that all movement, collision or networking paths have complete parity.

## Area-loading texture retention (2026-10-23)

The new runtime texture-quality provider used `FCompression::CompressMemory`
with `NAME_LZ4` on every world mip during streaming. Unreal implements that
entry point with LZ4HC, including its speed-biased option. This added synchronous
compression to new object/scenery appearances. Retention now uses Core's fast
LZ4 encoder, retaining the same lossless format and decoder. Authored resolution,
alpha, generated mip colors and live quality switching are unchanged. Fast
encoding can retain somewhat more CPU memory than LZ4HC; GPU storage is unchanged
and incompressible inputs still fall back to their original bytes.

`ACE.Rendering.WorldTextureRetention` checks all uploaded bytes after both full
and partial mip reloads. Its local 37-texture/313-mip comparison measured 16.12 ms
for fast retention versus 319.16 ms for LZ4HC (5.31 versus 4.02 MiB retained from
9.81 MiB raw). These are local microbenchmark measurements, not portal-duration
claims. Loading trace scopes now distinguish retained mip compression, texture
creation, setup decoding and mesh/material installation.

In the offline outdoor scene with fresh per-run caches, game-thread compression
fell from 2.17 s to 0.10 s. During the first 15 seconds after fixture preparation,
frames over 50 ms fell from 40 to 14 and p99 frame time from 79.46 to 28.78 ms.
The full 121-landblock terrain radius still took about 5.6 s to install; this
change does not remove terrain generation or scene-construction costs. It does
not shorten the safety gates that wait for collision and nearby appearances.
The existing captured session had an 11.24-second portal transition and about
6.95 seconds in logged appearance builds. No live-server before/after portal
duration was measured. Evidence: `Saved/AreaLoadingReview/loading-comparison.json`,
both outdoor traces/logs, and `tests/index.json` (six suites successful; video
settings emitted existing console-variable priority warnings).

### Follow-up: reported sustained FPS loss

A same-executable encoder comparison at 2560x1440 did not reproduce the
reported sustained regression. After a 45-second warmup, the offline outdoor
fixture (32 synthetic NPCs and 121 terrain blocks) sampled 30 seconds per run.
At Epic quality, old/new mean frame times were 5.434/5.416 ms (184.01/184.64 FPS).
At Cinematic quality, matching the quality level recorded in the editor log,
they were 5.973/5.965 ms (167.43/167.63 FPS). Both Cinematic runs used the same
CPU/GPU tracing configuration. These small differences do not establish a
steady-state speedup. The recorded p99 times were also comparable.

World mip allocation increased from 27.81 to 35.41 MiB; RHI texture allocation
was identical within each pair (1568.17 MiB at Epic, 2054.54 MiB at Cinematic).
No texture creation/retention or setup construction occurred during either
steady-state trace. The fixture had ample free system memory. This does not
rule out pressure on lower-memory hardware, nor reproduce a live editor session
with different characters, UI, weather, camera and network activity. The cause
of the reported live-session FPS drop remains unconfirmed; no rendering quality
or movement behavior was changed speculatively.

`/acueperf` now includes runtime quality settings, frame cap, VSync, the screen
percentage console setting, RHI texture allocations and editor-play status.
The encoder comparison switch is restricted to non-Shipping automation builds.
Evidence: `Saved/FpsRegressionReview/comparison.json`, paired logs/traces and
`build-final.log`. FrameDiagnostics and WorldTextureRetention passed, with the
retention suite also passing using the legacy comparison encoder (three test
executions, no warnings/errors). Shared Quest source is synchronized; no live
server, packaged Linux or Quest FPS result is claimed.

### Live follow-up: editor cost versus arrival hitches

The two subsequent reports were matched to their originating logs rather than
compared as equivalent runs. The 2556x1354, 13.6 ms game-thread sample came from
editor PIE with the updated local module. A 2,440-frame live trace averaged
14.14 ms per engine tick: Slate drawing/prepass consumed 7.84 ms and
`ProcessLocalPlayerSlateOperations` another 2.37 ms. The latter searches the
Slate window tree for the game viewport before processing local-player replies.
These scopes include editor windows; only 1.33 ms per frame was attributed to
`Paint: Game UI`. This identifies the missing CPU time, but does not establish
a regression in the game HUD or justify changing rendering quality.

The later 2560x1440, 4.5 ms sample was emitted by the installed v131 executable,
which predates the local fast-LZ4 fix. Its log directly records the subsequent
appearance stalls: at 15:22:41-42 UTC, Iaret's appearance took 198.87 ms and the
Apprentice Alchemist's 217.96 ms; at 15:25:29 UTC an arriving player's appearance
took 679.85 ms (647.34 ms in mesh/material installation). These occur as new
objects become visible after initial area entry, not just during the portal
screen. The same NPCs in the updated editor log spent 40.06 and 22.85 ms in
mesh/material installation respectively; the different processes, cache states
and editor overhead make this supporting evidence, not a controlled speedup.

The earlier controlled loading comparison remains the validation for the
encoder fix. Remaining long frames are not all compression: in the fixed
offline trace, a 324 ms frame and its following 110 ms frame include terrain mip
uploads, task waits and editor shader/material work. No claim is made that all
arrival hitches are eliminated. Neither user-owned running process was restarted
or modified. Evidence: `Saved/FpsRegressionReview/LiveEditor/live-editor.utrace`,
its exported timers, the installed-session evidence extract in that directory,
and `Saved/AreaLoadingReview/remaining-hitches.csv`.

## Loading cadence and TurnTo target frames (2026-10-23)

Reviewed `CellManager::PreFetchCells` / `UpdateLoadPoint`,
`LScape::PreFetchCells` / `update_loadpoint`, and `CEnvCell::PreFetchCells`
against the terrain/environment presenters. Preserve the desktop residency
radius, authored models/texture mips, and the arrival collision readiness gates.
This pass changes the scheduling of repeated residency work, not the data used
to construct the scene. Unreal's looping timer previously replayed every missed
100 ms interval after a hitch, spending several application budgets in one frame.
Coalesce those retries to one per frame. The actual callback test injects 550 ms
and verifies one drain followed by normal progress on the next interval.
Restoring streaming subscriptions is now idempotent: repeated restoration used
to append the same position/entry/logout delegates again (and trigger an ensure).

Retail `MovementManager::unpack_movement` resolves a known TurnToObject target
through `CPhysicsObj::GetObjectA`. `TargetManager::SendVoyeurUpdate` supplies its
current physics position; `MoveToManager::TurnToObject_Internal` adds the relative
heading and freezes the destination for that command. An unknown target uses
the packet's absolute fallback heading. Our separate TurnTo branches instead
read sparse session positions, and could defer an unknown target's resolution
until a later tick. Use the common live target resolver at command receipt.
For the local player, use validated prediction instead of a transient server
echo; pending teleport/forced-position sequences still win. A queued network
object without a physics actor uses the packed fallback immediately. Do not
continuously track the target or add another rotation smoothing filter.

The new packet replay uses Pumpkin Buffer setup `020014E0` and motion table
`09000028`, delayed local echoes, live remote movement, relative headings,
late-spawned targets, and unapplied relocations. Before the fix, the synthetic
slow orbit differed by 6 degrees and failed the live-target assertions. After
the fix, every orbit frame is monotonic and agrees with the DAT turn rate within
0.000025 degrees at 30/60/144/240 FPS, including heading wrap. This is a controlled
protocol/DAT regression, not a recording of the user's live server.

Validation: Development Editor build; 13 automation suites, zero failures:
RemoteTurnTargets, RemoteMovementReplay, StreamingRetryBudget, ColdTerrainLoad,
StreamingCost, DeferredWorldAppearance, ResolvedAppearanceCache, ArmorDistance,
SpawnSupport, WorldEntry, StreamingRetirement, LoadingTransition, and
LandscapeTextureFidelity. Cold terrain builds cover two complete 121-block rings;
the fidelity checks compare restored mip bytes, authored LODs and palette remaps.
Reports and logs: `Saved/LoadingTurningReview/{BaselineReport,FixedReport}`.
The baseline retry fixture also exposed its first-use mesh-format invalidation;
the final fixture consumes that initialization before measuring timer cadence.
Individual synchronous uploads/collision/scene installation can still exceed a
frame budget; this pass does not establish that every loading hitch is eliminated.

## Network positions, turn poses, and emotes (2026-10-25)

Reviewed the local retail decompile's `SmartBox::HandleReceivedPosition`,
`CPhysicsObj::MoveOrTeleport` (00516330), `CMotionInterp::apply_interpreted_movement`,
`CMotionTable::GetObjectSequence`, `CSequence::update_internal` /
`advance_to_next_animation`, and `CommandInterpreter::ShouldSendPositionEvent`
(006B45E0), `SendMovementEvent` (006B4680), and `SendPositionEvent` (006B4770).
The changes retain DAT models, animation clips, collision bodies, and authored
movement speeds.

- Remote interpreted turns, TurnTo, and MoveTo steering now drive the authored
  turn animation channel as well as the physics heading. Directed turns update
  that channel once per step, avoiding repeated animation-clock resets. Stop,
  airborne motion, and tracked VR clear the desktop turn channel.
- Nearby creature corrections no longer become teleports solely because their
  error exceeds 40 AC units. Retail interpolates nearby contact updates and uses
  immediate positioning at viewer distance >=96 AC units; a new teleport epoch
  still places immediately, and ordinary non-contact updates remain ignored.
- Force-position timestamps advance monotonically across wraparound. A newer
  position carrying an older force epoch cannot roll that epoch backward and
  make a later duplicate force correction blip the player again. Forced local
  corrections immediately acknowledge corrected XYZ with the retained local
  heading, as retail does.
- Position-frame comparisons use retail's 0.0002 tolerance. Outgoing chat-emote
  MoveToState packets retain physical contact and reset the position timer just
  like other movement packets; they no longer invent grounded contact.
- Received emote/pickup rates are preserved instead of forced to 1.0. Changes
  to an active interpreted substate's speed do not replay its entry. Distinct
  stamped actions queue instead of cutting short the previous gesture.
- One-shot social actions return to the previous locomotion cycle. Persistent
  substates play their entry and then their actual DAT cycle, including nonzero
  looping cycles; they no longer all freeze at an endpoint. Already-active
  spawn states start directly in the cycle. Zero-rate poses and Dead stay still.
  The extended fixture exposed frozen cycles in 23 human chat substates.

`NetworkAnimationParity` replays F74C through the session and actor, compares
rendered part transforms with DAT evaluation for all 309 chat aliases / 74
distinct motions on both human setups at 0.5/1/2 speed, and checks completion,
continuing cycles, already-active spawn states, speed changes, and action queues.
It exercises players, human NPCs, carenzi, gromnies, wasps, rabbits, chickens,
roosters and chicks, with turns at 30/60/144 FPS and the 95/96/110 AC position
boundary. NetworkTransport adds force-epoch/replay, contact, and report-cadence
checks; MovementReview verifies immediate forced-correction acknowledgements.

The pre-change baseline already failed two older suites: landing totals counted
the positive clip's High boundary as another root step, and the LOD clock fixture
included Ready->Run entry time in its cyclic-clock measurement. Their fixtures
now integrate the retail boundary rules and isolate the cyclic clock. Existing
remote replay relocation fixtures now explicitly model fresh creation/teleport
instead of relying on the removed 40-unit heuristic. The sidestep fixture checks
continued authored substate cycles rather than requiring every pose to freeze.

The Development Editor build and all 17 final suites passed: NetworkAnimationParity,
NetworkTransport, EmoteTransitions, MovementReview, RemoteMovementReplay,
RemoteTurnTargets, TargetedEmotes, ArcheryMotion, AttackHeightMotion, AvatarMotion,
WalkingStep, RunSpeed, RemoteSupport, CombatProtocol, RuntimeActors, InteriorNetwork,
and PumpBudget. Quest shared source was synchronized and CheckOnly passed. Existing
unrelated compiler conversion/deprecation warnings remain. RuntimeActors passed
with six scalability console-variable priority warnings; the other 16 suites
passed without warnings, and there were no automation errors. Validation artifacts are under
`TestOutput/MovementPass-2026-10-25/` (baseline,
build logs, intermediate reports, and `complete`). These are isolated offline
protocol/DAT/Unreal actor tests, not a synchronized recording from a live retail
client and Unreal client. They do not certify every custom server motion or
replace live multiplayer verification.


## Dereth Lens and Third Eye plates: 2026-10-25

- Added an optional API 2 Lua companion, Dereth Lens, following SkunkVision's
  slope, impassable-water, radar-boundary and dungeon-light feature set
  (https://skunkworks.sourceforge.net/SkunkVision/SkunkVision.html). Its gold/teal
  lens-and-landscape icon uses the existing plugin vector icon system.
- Terrain classification uses retail `CPhysicsObj::get_walkable_z` /
  `PhysicsGlobals::floor_z` (0.6641741) and `CLandCell::find_env_collisions`'s
  `get_block_water_type()==2` rule. A flooded coastal cell is not open ocean.
  The visualization uses existing DAT mesh triangles, with no added DAT reads,
  physics changes or server movement requests. Radar radius is shared with
  `ACERadarVisuals::RangeAc` (CPlayerSystem::GetRadarRadius / SmartBox::is_player_outside).
- Third Eye watch rules can opt into native world plates. The snapshot retains
  active plate objects independently of selection and confirmed-cast timers.
  Background assessments refresh their server-provided metadata and vitals.
  ACE `Player::HandleActionQueryHealth` changes the server selected target, so
  it must never be used to poll companion bars. The existing background identify
  path preserves manual appraisal command context and does not change selection.
- Assessment-only data remains subject to server skill checks. Unselected health
  on stock servers updates at bounded assessment cadence. No guessed damage,
  stamina, other-player debuffs, or early dispels are presented as known facts.

Validation: Development Editor build; `ACE.Plugins.WorldPlates` (includes native
Dereth Lens geometry, material, lifecycle and Lua controls), `ThirdEye` (including
short/narrow panel renders), `Companions`, `PublicAPI`, `AppraisalCommandContext`,
and `ResourceMaintenance` passed. Tracking is checked against encoded loopback
IdentifyObject packets; no QueryHealth target changes. Artifacts are in
`TestOutput/DerethLens/`. Quest source synchronization is checked separately.

## Chat controls, vendor staging and button drags: 2026-10-30

- Main-chat expand/restore and unread controls take precedence over the legacy
  sender-click fallback. Their DAT rectangles overlap selectable chat rows;
  receiving the Slate hit alone did not prevent the canvas from starting a tell
  before dispatching the button. Buttons, scrollbars and window handles now
  retain input priority without disabling ordinary sender clicks.
- Vendor Add to List keeps the Items page and selection, matching
  `gmVendorUI::HandleButtonClicks` (0x100000C3) and
  `VendorItemsUI::AddToBuyList`. Repeated clicks accumulate the selected quantity
  without navigating away or submitting a purchase.
- `UIElement_Button::UpdateState_` only changes to a state present in its DAT
  description. Preserve the previous valid state when rollover artwork is
  absent, preventing combat/use/examine, minimize, close and logout buttons
  from going blank during a captured drag. Radar lock uses the explicit
  0x10000063/0x10000064 states from `gmRadarUI::UpdateLockedStatus`.

Validation: Development Editor build and `UIScreens`, `UIInteractions`,
`ChatInputAndCommands`, `ACE.VR.RigAndMenus` and `ACE.Mobile.TouchAndMenus`
passed. Rendered chat checks cover center/corner hit surfaces at 100%/150% UI
scale, locked/unlocked windows at the top/bottom of the viewport, native canvas
clicks with a clickable sender underneath, and preservation of unsent drafts.
Authored-button tests exercise drag out/back, release outside, capture cancel
and release inside; vendor tests perform ten consecutive native Add clicks.
The trade-note price fixture now explicitly opens Buying to inspect that page.
Evidence: `TestOutput/UIGestures-2026-10-30/` (four other suites in `report`, final
screen suite in `final-report`). These are offline DAT/Slate/loopback tests;
no live server transaction or new packaged release was performed.

## Innate caster spell and manual attack accuracy: 2026-10-30

- `gmSpellcastingUI::UpdateEndowmentIcon` and `IconData::RenderIcons` draw the
  equipped caster's spell, its transparent drag icon, then 0x06004D09 selection
  artwork. The selection frame now paints above the runtime icons. Inventory
  structure/sale decorations do not obscure the innate spell. The tooltip uses
  the item name, spell name and retail double-click instruction.
- Resolve the equipped caster before synchronizing a spell bank or casting.
  Empty banks select the innate spell, equipment changes refresh its identity,
  and unequipping falls back to an available favorite. Next/previous/first/last
  selection includes the innate slot as in `gmSpellcastingUI`'s selection notices.
  Read the session's object view without copying the entire nearby inventory.
- The normal power/accuracy thumb uses its authored width and preserves the
  pointer's grab offset. Track clicks center the thumb, pointer coordinates are
  converted consistently, and changing the requested mark does not overwrite
  the charge already being displayed.
- `gmCombatUI::RecvNotice_SetCombatMode` hides the normal combat panel when
  Advanced Combat UI is enabled. That mode now shows the floating red
  Power/Accuracy charge meter from `gmPowerbarUI::RecvNotice_BeginPowerbar`.
  Jump retains the yellow Height meter. The existing retail packet rules remain:
  normal attacks build to at least the selected mark; advanced attacks use the
  held charge at release. A visible normal slider in advanced mode previously
  suggested that quick clicks would use its mark when they did not.

Validation: Development Editor build; `UIScreens`, `UIInteractions`, `PvPCombat`,
`ACE.VR.RigAndMenus` and `ACE.Mobile.TouchAndMenus`. Native pointer drag and
attack-button tests check exactly one missile packet with 0%, 50% and 100%
accuracy at 75%, 100%, 150% and 200% Slate UI scale. Rendered checks cover innate
selection layering, empty banks, spell changes, unequipping, tooltip text and
advanced/jump meter transitions. PvPCombat separately checks normal/advanced
melee and missile release/repeat packet values. Evidence is under
`TestOutput/WandArchery-2026-10-30/`; final tooltip/selection validation is in
`final-report`. Quest and mobile share the updated source. No live-server hit
rate or user-specific wand was available for reproduction.
