# AC:VR: Rift through SteamVR

Build **2026.09.13.11** includes the OpenXR VR client and accompanying ACE server extension. This is a Development build for headset acceptance testing. Automated tests do not certify controller alignment, live combat, or headset performance.

This update adds a saved Walk/Run toggle, full held jump charging, corrected forearm roll, deliberate melee swing detection, offhand bows with equipped arrow visuals, and wand-tip spell origins. Window dragging and closing no longer open the keyboard; inventory double clicks have more time and tolerate controller jitter. Use the included updated server for the revised melee validation and wand-tip reach.

This follow-up corrects the actual elbow landmark inside the retail forearm mesh, keeps the bow in the offhand even while peaceful, and preserves equipped-weapon information when public object descriptions refresh. Two-hand bow draws no longer depend on the support controller's pointing ray. The updated server restores equipment activation effects, validates melee against the creature's full body, and clears stale projectile target filters. Terrain comfort smoothing stays within 3 cm of the character's floor height; jumps follow it immediately.

Spell icons retain the retail power-level background, beneficial/harmful mask coloring, and target overlays. The central and wrist hotbars render all blue empty-slot numbers 1–9 before any spell is added.

## Start the client

Extension discovery uses the standard ACE PingRequest with an optional eight-byte
trailer (`ACUE`, followed by version 1 as a little-endian uint32). Ordinary ACE
servers ignore the trailer and return their normal ping response; no custom
action is sent until the server advertises capabilities. Discovery runs once per
character entry. Servers using the older VR extension must update the
`GameActionPingRequest` handler from this repository to support this discovery.
The existing capability response, subscriptions and VR action formats are unchanged.

1. Connect the Rift and both Touch controllers in the Meta PC software.
2. Start SteamVR and complete its room setup. Select **SteamVR as the current OpenXR runtime** in SteamVR settings. Wait until SteamVR actually detects the headset and controllers; having SteamVR open or selecting the runtime alone is insufficient. If using a Quest through the Meta PC runtime, enter its PC/Link connection before launching the game.
3. Keep the complete `Unreal/PackagedVR/Windows` directory together. Double-click the adjacent **`Launch-VR.bat`**, which handles PowerShell's script policy for this process only and checks the startup log for VR activation. The package requires the existing retail DAT installation; it does not include DAT files.
4. Point either controller at the login card. Pull its trigger to click a field. Use the virtual keyboard below the card, then log in and select or create a character.
5. Click the left stick to open VR settings. Use **Recenter / Calibrate height** while looking forward. Choose seated mode when playing seated.
6. Connect to an ACE server. Extended servers report **VR combat ready**. Standard servers support retail-targeted spell casting and item spells from the VR controls; free-aim combat requires the extension described below.

For development from source, run `Unreal/Launch-VR.ps1`. It builds the editor target, checks the selected runtime, and starts `UnrealEditor` with `-game -vr`. `-BuildOnly` compiles without launching. Desktop play remains available through the existing launcher; use `-nohmd` for desktop play when a headset runtime is running.

The renderer uses UE 5.8 OpenXR and instanced stereo. SteamVR supplies the OpenXR runtime; the old Unreal SteamVR plugin is not required. See [Epic's SteamVR development guide](https://dev.epicgames.com/documentation/unreal-engine/developing-for-steamvr-in-unreal-engine).

## Touch controls

- **Left stick:** move, relative to your head by default, with a gradual speed ramp. Click the left stick for VR settings and select **Movement: Run / Walk**. Run is the default and uses your character's current Run skill and burden. The saved toggle is independent of grip buttons. Movement speed adjusts the chosen mode from 25% to 100%; the default is 100%.
- **Right stick left/right:** 30-degree snap turns. Smooth turning is an option. Turns pivot around the actual headset position.
- **Right stick up/down over a menu:** scroll.
- **Right stick click:** hold for about one second to fully charge a jump, then release. A temporary yellow retail **Height** bar appears below the center of your view while charging. Height uses your character's Jump skill and burden; a short press produces a smaller jump.
- **X:** dismiss an active chat keyboard; otherwise show or hide inventory. Movement and attacks pause while typing or using the inventory panel.
- **Y:** switch between peace and the equipped combat stance. The settings panel also has direct Peace, Melee, Missile, and Magic buttons.
- **Left stick click:** open or close VR settings.
- **A:** select the pointed UI control; in magic stance, otherwise advance the selected spell.
- **B:** select the previous spell.
- **Either trigger over UI:** click, or hold and move to drag.
- **Login pointers:** visible hand markers and rays appear before you hit a button. Aim either ray at a text field and squeeze/release that trigger, then select keyboard keys. The keyboard sits below the login card, tilted toward you.
- **Weapon-hand trigger in peace:** select the pointed world object and use or pick it up when within its normal use radius. It does not walk toward distant objects automatically.

The weapon hand defaults to right. Left-handed mode swaps weapon and support hands; the sticks, letter buttons, and left wrist hotbar stay on their physical controllers. Server vendor, external container, and trade responses reveal the retail panel automatically.

## Magic

Equip a wand, orb, or staff and enter magic stance with Y. Look toward the left wrist to reveal the original retail hotbar, including its bank tabs, icons, and selection highlight. Looking away hides it after a short grace period to avoid flicker. The explicit View pin setting keeps it in view instead. A short status strip below it shows casting feedback and server errors. The default size is 33% larger than build .7; use Hotbar scale to adjust it.

Open the spellbook through the retail menus or the VR settings shortcut. Hold a trigger on a spell, move the controller ray to a hotbar slot on the central interface, and release. Use the central hotbar to reorder spells too. The wrist hotbar selects only: dragging over it cannot reorder spells or accept drops. A press and release selects a spell without closing the window. Use X to close the main menus when ready to cast. A selects the pointed control or advances the spell when pointing into the world; B selects the previous spell.

Pull the weapon-hand trigger while aiming away from UI to cast. Selecting a UI control never also casts. The wrist uses the actual active retail bank; if it is empty, A/B can select known spells until you populate it. The wielded item's built-in spell is also available through A/B.

On extended servers, projectile spells originate at the visible wand/staff tip, directed outward along its rendered shaft, sampled when the trigger is pressed. They retain ACE windup, mana, component, proficiency, damage, and recoil logic; they travel without homing toward a selected creature. Self buffs and other non-projectile spells use ACE's normal target rules. A teleport or weapon/stance change during windup invalidates the pending aimed cast. On standard servers, select a valid retail target before casting an offensive spell.
## Melee and missiles

For melee, equip a weapon and enter melee stance. Swing through a creature deliberately: at least 18 cm of net weapon travel at 1.4 m/s or faster. Movement comes from the controller in tracking space; body/weapon animation, locomotion, and small tracking jitter cannot produce a hit. Continuous motion registers at most one contact; briefly rest the weapon before another swing. Retail auto attack is disabled in VR. The server checks the sweep, target visibility/reach, equipment, and attack timing, then runs normal ACE damage and stamina logic. A contact can still miss or deal reduced damage under ACE combat rules.

For bows, equip the bow and compatible ammunition, then enter missile stance. The bow stays in the support hand even while peaceful; the equipped arrow appears at its string in missile stance. Bring the weapon hand within about 30 cm of that arrow, hold the grip or trigger to nock, pull back, and release the same button to fire. The line from the drawing hand through the bow defines the shot direction. The default full draw is 60 cm between hands and is adjustable. Very short draws, brief taps, and unreachable tracking jumps do not fire. Feedback reports nocking, missing ammo, release, and server rejection reasons. The server consumes ammunition and applies draw strength, accuracy, stamina, and reload timing. Arrows travel into the world and collide with the creature actually struck without requiring a selected target.

Crossbows use the same grip-to-draw gesture to load, then the weapon-hand trigger fires. Thrown weapons use grip hold/release and controller aim; they do not yet derive launch speed from throwing velocity. Tracking loss, menus, equipment changes, death, loading, and recentering cancel pending gestures.

The current swing detector uses the primary melee weapon. Independent offhand attacks, physical shield blocking, articulated finger tracking, and replication of tracked arm poses to other clients are not implemented. Equipped shields/offhand items follow the local hands visually; ordinary ACE defensive statistics still apply.

## Settings and comfort

The VR menu has a **Movement: Run / Walk** toggle above the sliders for **Menu scale**, **Menu distance**, **Hotbar scale**, **Movement speed**, and **Wrist stabilization**. The first four pin controls let you attach the main menus or hotbar to the camera view and keep vitals/chat visible in the view. Main menus default to a fixed world position; the hotbar defaults to the left wrist. Head-pinned panels participate in the headset's late pose update.

**Inventory recenter radius** in VR settings adjusts the body-anchored menu's minimum follow angle from 30 to 120 degrees (default 55). Wider or offset panels expand the angle further so you can inspect their edges. Changes apply in the headset and save when you release the slider. The setting is disabled for head and world anchors.

The desktop-style panel has an **Unlock retail windows** control below it. Use that to move and resize individual retail windows; the neighboring panel lock controls the whole floating surface. Either controller can activate inventory items, and equipped items support inspection and removal.

**Arms only** hides the torso, legs, head, and non-arm accessories from your own view while retaining all parts for shadows. The regular body option keeps the local head hidden and still casts its shadow. Recenter while upright to calibrate body scale; select seated mode before calibrating when seated.

**Move vitals:** select the lock strip beneath the health/stamina/mana bar to unlock it. Hold either trigger on the bar, drag it to the desired place in your view, and release. Select the strip again to lock it. The position and lock state save across restarts. VR settings also includes a Vitals placement lock control.

**Chat:** click a chat entry with either trigger. A keyboard appears in front of you, with the unsent text shown above its keys. Enter submits through the normal chat control and closes the keyboard. X, the keyboard's Close button, or a trigger click outside the keyboard dismisses text entry and retains any unsent draft. The first outside click only dismisses the keyboard. Main menus and pinned windows return after dismissal.

To rearrange items, hold a trigger, drag, and release. Double clicks allow 0.85 seconds between presses and 24 pixels of movement before becoming a drag. Unlock the original UI to drag window title bars; title bars and close buttons do not open the keyboard. Vendor cart drops stage items for sale; use the existing Sell control to commit. To give an item to a nearby NPC/player, drag out of inventory and release while pointing at that actor. Use the inventory's explicit Drop control for dropping items onto the ground. Missing a target cancels the transfer. X closes the main menus; selecting an item or spell keeps them open.

The remaining settings cover snap/smooth turning, snap angle, handedness, movement direction, posture, body visibility, haptics, bow draw, and render resolution. Changes save to the generated ACEVR.ini; existing saved preferences take priority over defaults.

Defaults are in `Unreal/Config/DefaultACEVR.ini`. User changes save to the platform's generated `ACEVR.ini` under the game's saved config directory. Additional values there include stick dead zone, smooth turn speed, seated eye height, panel distance, wrist size, and minimum swing speed. Values are bounded when loaded/saved.

Room movement is reconciled through the existing character collision path. Loading uses a fade while retaining headset camera ownership. Head obstruction and lost tracking/focus also fade the view and inhibit gameplay input. Teleport locomotion and a peripheral motion vignette are not included; stick movement, room movement, and charged jumping are available.

## Start the extended server

`Unreal/PackagedVR/Server` contains the matching Release server build. It requires the existing ACE database and DAT configuration, and the .NET 10 runtime unless you publish a self-contained server. Configure `Config.js` from `Config.js.example` for your own test server, following the project's normal server setup. No database migration is added by this extension. Run `ACE.Server.exe` from that directory after configuration.

Source build: `dotnet build Source/ACE.Server/ACE.Server.csproj -c Release`. Deployment should use the complete server output with its dependencies and configuration, not a single replacement DLL.

The client negotiates private protocol version 1 after entering the world. Unmodified servers do not provide VR combat. Head tracking, menus, and locomotion do not depend on that handshake, but the client will not send physical combat actions before the server acknowledges support. Existing desktop clients continue using their original actions.

## Headset acceptance checklist

Run this against a disposable test character on the extended server before using the build for normal play:

- Login with upper/lowercase and symbols, select/create a character, and confirm the initial panel is at head height.
- Verify standing/seated height, recentering, both controllers, hands, armor, weapon alignment, and left-handed mode. Check a bow, crossbow, wand, shield, and melee weapon separately.
- Walk, strafe, turn, physically move, climb stairs, collide with walls, charge a jump, enter interiors, and use portals. Verify no forced camera rotation or tunnel FOV animation.
- Open/close inventory and settings; scroll, drag/equip items, type chat, pick up nearby loot, use a vendor/container, and open trade. Verify distant use never moves the player.
- Select different spell banks; cast self buffs and aimed bolts, arcs, and multi-projectile spells. Check mana/components, resistances, misses, walls, damage, and invalid PK targets on the server.
- Swing slowly/quickly; check one hit per swing, server cooldowns, armor, stamina, and blocked/rejected targets. Walking with a stationary weapon must not attack.
- Nock/draw/release arrows; check partial draw, cancellation, reload, final ammunition, target impact, crossbow load/trigger, and gear switches with projectiles in flight.
- Lose/reacquire controller tracking, remove the headset, open the SteamVR dashboard, die, portal, and log out during a draw/cast. No stale attacks should appear afterward.
- Restart and verify saved VR settings. Use SteamVR frame timing to assess CPU/GPU performance at the Rift's configured refresh rate in towns, combat, interiors, and portal transitions. No headset frame-rate target has been measured yet.

See `VR_PORT_REVIEW.md` for the implementation review and automated validation evidence.

## If the headset shows a flat desktop window

Close the game, make sure SteamVR detects the headset, then relaunch with `Launch-VR.bat`. The first live attempt started before SteamVR detected the headset: its OpenXR log reported `VRInitError_Init_HmdNotFound`, and Unreal disabled XR with `Instance is not viable`. Connecting the headset after that point does not convert the running desktop client into VR. Relaunching after detection successfully created both eye projection views and activated the ACE VR rig.

The desktop mirror and world-space login/inventory panels are still flat surfaces in a successful VR session. Close the SteamVR desktop/dashboard overlay and return to the game to view the immersive scene. Launcher logs are saved under `%LOCALAPPDATA%/ACUnreal/Saved/Logs/VR-launch-*.log`.

## Quest Link shows passthrough while the desktop mirror tracks the headset

Keep the game running and return to the active Link session inside the headset. Exit the headset's passthrough view and close the Meta and SteamVR dashboards. On Quest 3, double-tapping the side of the headset toggles passthrough. Stay inside the configured boundary. This is separate from selecting SteamVR as the PC OpenXR runtime.

The Quest 3 test reached an OpenXR focused session and submitted both eye projection views to SteamVR; the tester subsequently confirmed the login screen is visible in the headset. A moving desktop mirror alone does not establish that Link is displaying the submitted frames. If passthrough recurs, first check whether the headset can show the Link dashboard or another SteamVR scene. If those are also missing, reconnect Link before investigating the game's menus or server. If other VR scenes display correctly, preserve the game and SteamVR logs for a presentation-path comparison.

## Login is visible but hands, pointers, or keyboard are missing

Use client build `2026.09.12.4` or later. The login and loading flows previously disabled the pawn's collision, including the attached VR panel, while rays were hidden until a clickable widget was hit. VR visuals and panels now use a separate actor, controller components activate explicitly, fallback hands/rays use an unlit material, and menu rays stay visible while aiming. Both hands retain their shared UI input when one controller loses tracking. Login retries no longer attach a fullscreen duplicate over the VR scene. The keyboard is raised and tilted toward the user.

After launching, put on the headset, close the dashboards, hold both controllers, and click a login text field with either trigger. `ACE VR tracking` lines in the launcher log report head/focus, each grip/aim pose, and panel collision. `ACE VR trigger` lines report whether a trigger reached a widget; they do not log entered text or credentials. The tester confirmed login, keyboard input, character entry, and equipping in build 2026.09.12.4.
