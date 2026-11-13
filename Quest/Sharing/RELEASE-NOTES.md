# AC:Unreal / AC:VR release 157

Windows desktop, PC VR, native Linux x86_64, standalone Quest, and the Android
phone/tablet preview share **2026.11.13.157**. Android version code: 157.
Quest installer revision: 10. These notes cover changes since v156.

## UCM recovery, combat and automation

- Improve buff and equipment-mana recovery on ACE and GDLE. Delayed or unrelated
  action replies no longer complete or cancel the wrong activity, and appraisal
  waits yield so other work can continue.
- Read GDLE equipment mana changes immediately and prefer usable mana stones
  before configured charges. Failed buff items no longer block the entire cycle.
- Support innate wand recovery spells and choose debuff sources across learned
  spells, wand activation, grenades and weapon procs. Wait for confirmed effects.
- Improve thrown-weapon offhand selection and preserve damage choices when
  saving and reloading profiles.
- Add optional automatic fellowship management with tell-based recruitment,
  waiting lists, reservations and member votes.
- Integrate automatic supply crafting into UCM, including intermediate recipes,
  stock targets, protected ingredients and recovery priority. Custom servers can
  import their own validated recipe catalogs through Auto Craft.
- Add configured lockpick support for route doors, optional idle helper buffs,
  and optional vTank-style casting movement.
- Improve meta sequencing and item-specific result tracking for giving, looting,
  salvage, summoning and crafting; unrelated failures no longer interrupt jobs.

## Profiles, plugins and vendors

- Expand vTank profile and regex compatibility, including conditional and
  balancing groups, named captures and lookbehind; fix case-sensitive matching
  caches and preserve Classic expression metadata during import/export.
- Update Lua developer documentation for the added actions and snapshot fields.
- Restore retail affordability checks in normal vendor windows. Single-item and
  cart purchases require sufficient carried coin value; rejected purchases keep
  the cart and do not start the hourglass. Item Tools' explicit bank option remains.

## Verification and scope

Validation covers automated Editor and packaged Windows/native Linux regressions,
APK contents/signatures, and an isolated Windows upgrade. Live-server acceptance
and physical Quest/phone testing remain unverified for this release. Optional new
automation defaults off in native profiles; imported vTank settings retain their
own values. Custom recipe/grenade data and server permissions remain requirements.
The Quest installer includes Diagnose-Quest.cmd for startup diagnostics.

Packages exclude retail DATs, personal settings, server data and generated
MeshCache files. Android phones/tablets remain a preview APK.
