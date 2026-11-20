# AC:Unreal / AC:VR release 163

Windows desktop, PC VR, native Linux, standalone Quest, and the Android phone and tablet preview share version **2026.11.20.163**.

## UCM target acquisition

- Fix a stationary combat stall when a new monster appears as the previous target dies. A delayed movement instruction toward the defeated or removed target can no longer leave UCM waiting indefinitely; a navigation route or Stop/Start is no longer needed to release that stale approach.
- Preserve active casting, inventory transactions, and approaches to living targets. Show a waiting status when a real pending action would otherwise leave the panel displaying Ready.

## Vendor restocking with banked currency

- Use the same spendable balance shown in the vendor window, including banked pyreals and server-managed alternate currencies advertised by custom servers.
- Redeem trade notes only for the remaining pyreal shortfall. Recognize proceeds credited to the vendor balance without requiring a physical coin stack, and still wait for both the funds and note removal.
- Recheck funds for each purchase and wait for inventory confirmation before buying again.
- Expose and document `vendor_purchase_funds` for Lua plugin authors while retaining the existing physical `pyreals` count.

Also resolve helper-name collisions in clean unity builds. These are build fixes with no intended change to client messaging.

Players supply their own retail data. No saved profiles, account credentials, or retail DAT files are included. Automated regression and package checks do not replace live custom-server or physical-headset testing.
