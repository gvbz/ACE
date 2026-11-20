# AC:Unreal / AC:VR release 162

Windows desktop, PC VR, native Linux, standalone Quest, and the Android phone and tablet preview share version **2026.11.19.162**.

## Faster UCM combat and looting

- Switch targets and retry attacks as soon as the client receives completion, rejection, target death, or removal. Remove the extra physical-attack timer and post-cast pauses while retaining server-controlled action completion and active auto-repeat.
- Appraise corpse items concurrently, with bounded outstanding requests and retries. Loot rules still wait for the required item properties; transfers remain confirmed before moving to the next item.
- Wake looting immediately when inventory, corpse contents, or appraisal replies arrive, instead of waiting for a polling delay.
- Recovery-only UCM preserves your combat stance when using food, healing kits, and mana supplies with **Peace when idle** disabled. The explicit healing-kit peace option remains available.
- Check the complete spell formula before changing casting equipment, including tapers and foci/augmentation requirements. Missing components no longer make unusable spells compete with your available weapons.
- Open route doors promptly after appraisal or unlocking, without an extra five-second wait. Hold position while a closed door is being opened instead of continuing to run into it.

## Portal arrivals and fellowship

- Resolve indoor portal and login destinations from their position when the supplied cell ID names the wrong room. Keep the server's coordinates and rotation, and load the resolved room before revealing the world.
- Positions outside all rooms can request the existing one-time lifestone recovery after the arrival timeout. Intentional drops remain valid; missing DAT files still report a file error instead of recalling.
- Show each fellowship member's nominal retail XP share alongside their level, with support for even and level-weighted splits. Server bonuses, distance, and custom rules can change actual awards.

## Desktop and plugin improvements

- Cap desktop login, character selection, and loading screens at 30 FPS. Gameplay retains its configured limit; active headset rendering keeps its own pacing.
- Use the clean **AC:Unreal** desktop window title without development-build suffixes.
- Add an opt-in local client messaging API for Lua plugins on Windows and Linux. Plugins can discover peers and exchange bounded messages across clients on the same computer and server. Includes API documentation and an example plugin; this is not the retail UtilityBelt protocol or automatic command execution.
- Document the opt-in corpse-appraisal pipeline for policy plugins.

Experimental water and item/model treatments are excluded from this release. Players supply their own retail data. No saved profiles, account credentials, or retail DAT files are included.
