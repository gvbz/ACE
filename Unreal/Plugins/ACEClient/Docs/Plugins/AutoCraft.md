# Auto Craft

Enable **Auto Craft** in **Plugin Manager**, then open its mortar-and-pestle icon
on the plugin bar. It is independent of UCM and Item Tools. It uses the same
movable, resizable, pinnable window and background-opacity settings as the other
plugins. Disabling its manager checkbox also stops the job.

Choose a **Category**, then an **Item**. Every dropdown has a search field: type
words in any order to narrow its results, then click a result or press Enter for
the first match. **All categories** searches the entire catalog. Items sharing a
name are grouped; **Recipe / ingredients** lists their different ingredient pairs
and output quantities. Selection initially favors ingredients already owned.

For example, Cooking includes Hearty Carrot Soup; Fletching includes Prismatic
Arrow and its larger wrapped-ingredient recipe. The preview shows the chosen
yield, difficulty, material counts and manufacturing steps.

**Inventory check** runs automatically when the recipe, quantity or inventory
changes, across the main backpack and all carried packs. **Check inventory**
refreshes the preview immediately. The notice above the selectors names missing
supplies; the materials panel lists every shortage and shows **owned / usable /
needed** for the recipe inputs. Owned items that cannot be consumed (retained,
equipped, offered in trade or nonempty containers) are explained separately.
With **Make missing ingredients** enabled, shortages include raw materials and
reusable tools for an ingredient route; shared stock is reserved across its steps.
Different ingredient recipes may require different supplies. When **Max
available** cannot make anything, the checklist describes the first whole batch.
These inventory checks do not use or move items, and Make remains disabled until
the executable plan has enough eligible materials.

## Quantities and ingredients

The quantity means **additional finished items**, not attempts or desired total
inventory. A recipe producing 100 arrows rounds a request for 101 up to 200 and
shows that before starting. **Max available** estimates the output from current
ingredients, including intermediate recipes if enabled, assuming successful
attempts. Failures can reduce that estimate. Jobs are bounded at 100,000 requested
items and 10,000 operations; recipes without a finite material limit require an
explicit quantity.

**Make missing ingredients** recursively prepares ordinary ingredients from
materials and tools already in the character's packs. It never buys materials or
withdraws from external chests. Dependencies run before the finished recipe, and
shared materials are reserved once across branches. The planner stops on missing
tools, missing materials, cycles or excessively complex chains. Complex quest
requirements and item-modification recipes are not silently inserted into an
ingredient chain.

Equipped items, items offered in trade, contents of offered packs, nonempty
containers and retained ingredients are excluded. A retained tool may be used
when the recipe cannot consume or modify it on either success or failure.
Matching stacks are split or combined when the recipe requires it, and those
transfers must be confirmed before crafting proceeds. A free slot is required for
a split. Result space is checked before using the ingredients.

## Running a job

**Make**, **Pause / Resume** and **Stop** control the job. Pause and Stop cannot
undo an action already sent to the server. Closing the window leaves the job
running. UCM pauses while Auto Craft runs and resumes afterwards. A competing
inventory automation is stopped when the crafting job starts.

The client enters peace mode and sends the source/target pair in the cookbook's
original direction. It waits for the matching inventory changes, not just
UseDone, before advancing. Optional ingredient consumption and failures have
separate expected results. Partial or delayed packets cannot trigger repeated
craft requests. A rejected or unconfirmed action stops with an explanation; the
timeout is 60 seconds and does not retry an uncertain action.

The confirmation option applies only to a single crafting confirmation for the
current outstanding job action. Other prompts stop the job. Turn this option off
to answer crafting confirmations manually. **Stop after a failed attempt** is
optional; progress always counts successful output rather than attempts.

The category, recipe, quantity and checkboxes are saved separately for each
character and server. Window placement and enablement use normal plugin settings.
Logging out, changing character, dying or disabling the plugin stops crafting;
jobs never automatically restart at login.

## Ammunition in UCM

In UCM, go to **Recovery → Automatic crafting** and turn on **Craft ammunition
from bundles**. Set **Craft when ammunition is below** to the number of rounds
you want available before UCM makes another batch. The default is 25. The
checkbox works independently of the general supply crafting checkbox.

When selecting a missile loadout, UCM counts compatible ammunition across the
equipped stack and packs. It combines matching regular or wrapped bundles for
standard arrows, quarrels and atlatl darts, including elemental variants. It
checks Fletching, waits for peace mode and the server's inventory result, then
resumes the loadout. Keep the matching shafts and arrowheads in inventory.

## Catalog scope and provenance

The bundled catalog is generated from
[ACE World Database v0.9.296](https://github.com/ACEmulator/ACE-World-16PY-Patches/releases/tag/v0.9.296):
44,755 cookbook pairs examined, 11,199 production pairs and 4,744 distinct output
item IDs. All pairs with a defined success output and named source, target and
output are retained, including alternate ingredients and batch sizes. The other
33,556 pairs produce no new item; these are predominantly existing-item changes
such as tailoring/tinkering and are outside this manufacturing interface.

This is standard ACE database coverage, not a claim that every custom server's
recipes or hardcoded server-only interactions are known. Server skill checks,
quest flags, workmanship, requirements, burden, success chance and results remain
authoritative. Recipes with extra requirements or item modifications are marked
in the interface. Unknown/custom results stop rather than being guessed.

The source data is from ACEmulator's **ACE-World-16PY** and
**ACE-World-16PY-Patches** projects, distributed under AGPL-3.0. See the staged
`ClientMods/autocraft/LICENSE` and `NOTICE.txt`. The transformed JSON stores its
upstream version and SQL SHA-256; the generator is maintained in this repository.

To regenerate from a downloaded SQL release (no running database required):

```text
python Unreal/Tools/Generate-CraftingCatalog.py world.sql Unreal/Plugins/ACEClient/ClientMods/autocraft/catalog.json --version v0.9.296
```

`ACE.Plugins.AutoCraft.Production` tests the actual shipped catalog, recursive
planning, quantities, protected items, network transactions, split/merge receipts,
timeouts, confirmation isolation, persistence and manager enablement. With a
rendering RHI it also tests real Slate dropdown filtering, selection and window
retention, and writes a temporary review image in `Saved/AutoCraftTests`.
