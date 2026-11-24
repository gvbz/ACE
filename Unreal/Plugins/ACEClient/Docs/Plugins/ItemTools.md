# Item Tools

Open **ITEM** on the plugin bar or type `/acueitems` to use the
[Virindi Item Tool](http://virindi.net/wiki/index.php/Virindi_Item_Tool) workflows.
Jobs continue when the window closes. Choose **Stop** or type `/acueitems stop` to end one.

## Give

Select a nearby player or NPC in the game, then click **Use selected** under
Recipient. Select an inventory item and do the same under Item to give. The
eligible count includes same-name items in every pack. Set a unit limit, or use
zero for all matching items, then click **Start giving**. A partial final stack
is supported. The transfer list is captured at Start so rewards from an NPC are
not automatically fed back into the same run.

Progress counts the units the server actually removes. Quest NPCs that accept
one unit from a stack are offered the remainder until the requested total is
reached. If the server explicitly rejects a gift while the recipient is busy,
Item Tools waits and retries up to six times, with increasing pauses. It never
retries an unconfirmed transfer just because it timed out. Repeated rejections
stop Item Tools with an explanation and leave UCM running.

Equipped, retained, offered items and nonempty containers are excluded. Items inside an
offered container are also excluded. Bonded quest items may be given to NPCs,
but cannot be given to players. Recipients must remain within five meters and
are revalidated before each transfer. Select them again after logging in;
transient world identifiers are not trusted across sessions.

## Buy / Sell

Open a vendor. Select its stock, then set Buy from vendor. Optionally select an
inventory item to fund the purchase. Clear Buy to sell only. The buy quantity is
the number to purchase during this run; zero spends available funding until
currency, stock or pack space runs out.

**Redeem trade notes** uses notes accepted by a pyreal vendor when more currency
is needed. Funding stacks are split to the needed quantity before selling, and
the split must be confirmed before its new object can be sold. Free a pack slot
if the server cannot split a funding stack. Custom currencies use the vendor's
advertised currency count and do not redeem pyreal notes automatically.

## Profiles (GiveVT)

Choose a `.utl` file from the shared profile folder, or copy UCM's active loot
rules. Set a recipient, then start. The first matching rule wins, using the same
appraisal and rule evaluator as UCM. As in GiveVT, only unlimited **Keep** items
are given. **Keep #**, Ignore, Sell, Salvage and Read items remain in your packs.

The selected file and rule count remain visible. Opening the profile picker
lists the files currently in the folder. Re-select a file
to apply edits made on disk. Use the separate Loot Profile Editor to edit rules,
then copy the updated UCM rules or select the saved `.utl` again. Unsupported
rules prevent applying a file and report their conversion errors.

## Chests

Set a chest and an optional key, then choose a collection profile. Keep rules
collect loot; Keep # limits how many units are retained. Two optional recipients
have separate transfer profiles, evaluated in recipient order. Only items
collected during this run are eligible for distribution, including newly acquired
units merged into older stacks. Existing inventory is not handed over.

Clear the key to use looter-only mode. Clear the collection profile to use
opener-only mode with another looting character. Set a finite number of cycles or
zero to repeat. The tool refreshes lock state between cycles. Moving recipients,
rejected actions, full inventory and unconfirmed transfers stop the job with an
explanation; remaining items stay where the server last confirmed them.

## Restock

Add selected inventory items or vendor stock and set **Keep this many** for each
supply. The tool counts all owned packs and buys only the shortage. Lists are
saved separately by character GUID and server name. **Restock automatically**
starts when a newly opened vendor stocks something in the list; enable this only
when automatic purchases are wanted. Missing stock is reported after available
supplies have been processed.

Manual command: `/acueitems restock`. The original `/it recomp` spelling is also
accepted. Other commands are `/acueitems give`, `/acueitems shop`, `/acueitems profile`,
`/acueitems chest`, and `/acueitems stop`.

## Merge

**Merge similar items** fills matching partial stacks across your carried bags.
Item Tools starts the next merge as soon as the server confirms both stack
updates. **Pause between merges** is independent of the Give tab's transfer
pause and defaults to 0 milliseconds.

## Coordination and validation

Item Tools temporarily pauses UCM's actions during a job, including inventory
combining. UCM stays enabled and resumes afterwards, clearing stale in-flight
requests. Each transfer waits for inventory/container changes from the server;
a successful UseDone alone does not confirm a give, purchase or pickup. The
native host checks ownership, quantities, vendor identity and space again before
sending a request. A transfer without confirmation stops after 20 seconds.

Regression coverage: `ACE.Plugins.ItemTools` tests the Lua policies, native
ownership/capacity checks, character persistence, loopback protocol requests and
renders all five tabs at 780 and 460 pixels wide. `ACE.Plugins.LootRules` covers
the shared rule evaluator. Live server behavior still depends on the server's
use distances, chest resets, vendor restrictions and inventory responses.
