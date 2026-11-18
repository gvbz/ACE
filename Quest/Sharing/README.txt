AC:VR - QUEST SIDELOAD PACKAGE

This is a native Quest 3 test build. Once installed, the game runs on the headset;
it does not require SteamVR, Meta Link, Unreal Editor, or a gaming PC to render it.
A Windows PC is needed for this installer, and a compatible game server must be reachable
over the network while playing. Quest 3S is accepted by the installer but has not
been tested on hardware. Check manifest.json for the exact build version.

IN-HEADSET UPDATES (v73 AND LATER)

At login, open Updates, download the new version, then choose Install. Confirm
the update in the headset. If requested, allow AC:VR to install updates, return
to the game, and choose Install again. Accounts, settings, and DAT files stay
in place. The USB updater below is still available if the headset prevents
installation. Older clients need one USB update to obtain this feature.

ALREADY HAVE THE GAME INSTALLED?

Extract this new ZIP, copy your existing platform-tools folder beside the
installer (or use adb.exe from PATH), connect your Quest, and double-click
Update-Quest.cmd. It updates the app while keeping your DAT files, saved login,
and VR settings. Do not uninstall the old app first. If the original data
installation is incomplete, use Install-Quest.cmd instead.

OPTIONAL AC2 LEGIONS SKY

1. Install or update AC:VR from this full bundle. Connect your Quest by USB
   and close the game.
2. On the PC, run Install-AC2-Sky.cmd. Choose portal.dat from your own AC2
   Legions installation, or choose Saved\Skies\Legions.sky from an existing
   AC:Unreal import. The cache is the smaller transfer. For portal.dat, allow
   free space on the headset for two copies during USB transfer.
3. Open AC:VR. Under Options > VR settings > Graphics, choose "Import and use
   AC2 sky". The headset validates the file, applies the sky, and frees the
   transferred DAT space after import. Use "AC2 sky" here to switch back to
   the original sky or enable the imported sky again.

The sky and your choice survive ordinary app updates. To replace the sky,
repeat steps 2 and 3. For a single install and transfer from the command line,
pass -AC2SkyPath "C:\path to AC2\portal.dat" to Install-Quest.cmd or Update-Quest.cmd.

INSTALL

1. Extract the entire ZIP into a normal folder on your Windows PC.

2. Enable Developer Mode on your Quest using the Meta Horizon phone app. Connect
   a USB data cable and accept "Allow USB debugging" inside the headset. Select
   "Always allow from this computer". This prompt shows a computer fingerprint;
   accepting Quest Link or file access alone does not authorize ADB.
   Official setup: https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/
   On Windows, install the Oculus ADB driver if the debugging interface is missing:
   https://developers.meta.com/horizon/downloads/package/oculus-adb-drivers/
   Extract the driver download, then right-click its .inf file and choose Install.

3. Download Android SDK Platform-Tools for Windows from:
   https://developer.android.com/tools/releases/platform-tools
   Extract its platform-tools folder alongside Install-Quest.cmd. Keep adb.exe
   and its accompanying DLLs together. The installer can also use adb.exe from
   your PATH or ask you for its location. Android Studio is not required.

4. Have these files from your Asheron's Call installation in one PC folder:
     client_portal.dat
     client_cell_1.dat
     client_local_English.dat
   client_highres.dat is optional and is copied if present. DAT files are not
   included in this ZIP. Allow several GB of free headset storage for the data
   and temporary transfer. Use the same game data revision as the server owner.

5. Double-click Install-Quest.cmd and provide your DAT folder if asked. Leave the
   USB cable connected until it says installation is complete. The first copy
   can take several minutes. Every data file is verified by SHA256. You can run
   it again after an interrupted transfer; verified files are skipped.
   The installer waits up to two minutes for USB debugging authorization and
   continues automatically once the Quest is ready.
   The installer uploads the APK before asking Android to install it;
   it avoids a streamed installation and can retry once after a USB disconnect.
   Revision 3 copies DAT bytes directly into app-owned storage, verifies a
   temporary copy, then replaces the installed file. No shared-storage staging
   directory is needed. Interrupted or corrupt transfers retry up to three times.

6. Put on the headset. Open AC:VR from the Unknown Sources section of
   the app library. Choose Browse servers or Custom server in the login lobby,
   then select or add YOUR account and press Launch. Saved accounts are shared
   across your server list. The USB cable can then be disconnected.

The launcher allows this one PowerShell process to run the installer; it does
not change your system-wide execution policy. Administrator rights are normally
unnecessary. APK installation alone (including via SideQuest) does not install
the DAT files; run this helper to place them in the correct app-owned storage.

CONNECTING TO YOUR FRIEND'S SERVER

Ask the host for a reachable server address, port, and your own account.
The packaged default, 10.0.0.26:9000, is a private LAN address. It only works on
that LAN, or through a VPN route that actually reaches that LAN from the Quest.
Connecting a VPN only on the installation PC does not route the headset's Wi-Fi
traffic. A remote player needs either a routed network/VPN connection from the
Quest or the host's public IP/DNS address with the server ports forwarded.

For this server, the login port is UDP 9000 and the adjacent world port is UDP
9001. Both must be reachable. With public hosting, the host must configure their
router/firewall to forward those UDP ports to the server PC and keep the server
running. Database ports do not need to be exposed. No network settings are
changed by this installer. Only one ACE server process should own these ports.
Use this project's VR-enabled server for its custom VR combat/pose extensions.

UPDATES / TROUBLESHOOTING

- Black screen or "AC:VR isn't responding" before login: while the failure is
  still recent, connect USB and run Diagnose-Quest.cmd. Accept USB debugging if
  needed. Send the resulting Quest-Diagnostics-*.zip. It records app version,
  Horizon/Android build, free storage, DAT presence/sizes, recent game logs and
  Android's retained app-exit/ANR information, plus native thread stacks when
  available. Each read has a timeout, so an
  unresponsive headset cannot leave the helper waiting indefinitely. Logs may
  contain character/chat text; review them before sharing. This helper does not
  install, restart, stop, or reset the app, and does not collect saved logins.
  Capture before rebooting/reinstalling, which can discard the useful evidence.
  "App Name not Available" alone does not identify the reason for a launch hang.
- Press Menu for VR options, or hold X for about a second. Tapping X still opens
  inventory. The same hold-X shortcut works with Quest controllers in PC VR.
- Install a newer bundle the same way. Do not uninstall first: an update keeps
  your DAT files, saved logins, and settings. Uninstalling removes app data.
  AC:VR updates your existing installation; its package ID stays stable.
- Update after data is installed: Update-Quest.cmd
  Equivalent command: Install-Quest.cmd -SkipData
- APK installed but stuck in portal space after a failed DAT transfer: run
  Repair-Quest.cmd. This repairs/verifies the data without reinstalling the APK.
  Keep your original APK and manifest.json in this folder. The game is closed
  during data repair; saved logins and settings are preserved. Do not launch the
  game until the installer says installation is complete. If repair fails, send
  Quest-Data.txt to the host. A persistent portal stall after successful repair
  also needs the game log and a check that UDP world port 9001 is reachable.
- Custom paths: Install-Quest.cmd -AdbPath "C:\tools\platform-tools\adb.exe" -DatDirectory "D:\Games\Asheron's Call"
- More than one authorized device: add -Serial followed by the Quest serial
  shown by adb devices. Disconnect other devices if you prefer.
- Check USB debugging without installing: Install-Quest.cmd -CheckConnection
- No authorized device: the installer now reports the actual ADB state and
  waits for permission. An empty device list points to Developer Mode, the data
  cable, or the Oculus ADB driver. "unauthorized" means accept the fingerprint
  prompt in the headset. "offline" means reconnect the USB cable and keep the
  headset awake. If it times out, send Quest-Connection.txt from this folder to
  the host. It records connection details, not your game password.
- If you already accepted debugging but it stays unauthorized, reconnect USB
  and check for a new fingerprint prompt. If it still persists, close other
  ADB tools and run platform-tools\adb.exe kill-server, then restart this
  installer. This restarts the PC's shared ADB service, so other debugging
  sessions will disconnect. Do not uninstall the game or erase its data.
- Missing DAT file: select the folder that directly contains the three required
  files. Installing just the APK is insufficient.
- Connection timeout: check the server address, both UDP ports, server process,
  and the headset's network route. A USB connection does not provide LAN access.
- APK signature mismatch/downgrade: ask the host for a compatible newer build;
  the installer will not erase your existing app to force installation.
- APK installation failure: send Quest-Install.txt from this folder. It retains
  ADB's full error, including Android's INSTALL_FAILED reason when available.
  For repeated offline/disconnect errors, try a different USB data cable and a
  direct PC USB port. Keep the headset awake during the upload.
- Inspect manifest.json and run Install-Quest.cmd -ValidateOnly to verify the
  APK without connecting a device or changing an installation.

TEST BUILD STATUS

See manifest.json for the exact installed build and RELEASE-NOTES.md for changes,
validation, and remaining live checks. The lobby supports saved servers/accounts,
the community server directory, custom ACE/GDLE entries, and a configurable DAT
folder. Credentials are encrypted on the headset.
Use the host's updated VR-enabled ACE server for arc previews and combat/pose
extensions. No server installation is included in this client bundle.

This is a test build. Packaging and automated checks do not replace headset
playtesting. Dense-area frame rate, combat, movement, and headset comfort need
continued testing; consistent 90 FPS is not yet established.

This ZIP contains the APK, installer/update/repair helpers, instructions, release
notes, license, and a checksum manifest. It contains no saved player logins,
server database, DAT files, or development folder. Credentials must be entered
on the headset.
