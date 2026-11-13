-- UCM policy: all effects go through the public host's validated intents.
local state, entered, point, locked_target = nil, 0, 1, nil
local targeting_status
local route_direction=1
local route_join_pending,route_join_scan=true,nil
local route_trail,route_walking,route_returning={},false,false
local route_teleport,route_revision,blocked_serial=nil,nil,nil
local route_blocks=0
local navigation_paused,door_attempts=nil,{}
local retries, until_time, corpses, item_attempts = {}, {}, {}, {}
local route_pending, loot_pending, recovery_pending, buff_item_pending = nil, nil, nil, nil
local loot_full=nil
local monster_failures,monster_blacklist,combat_serial={},{},0
local ghost_attempts,ghost_hp={},{ }
local spell_health={}
local corpse_pending=nil
local corpse_contents_wait=nil
local corpse_spawn_wait,combat_death_seen=nil,nil
local completed_corpse=nil
local confirmed_loot_notice=nil
local mana_refill_pending=nil
local mana_fill_pending,pet_refill_pending=nil,nil
local mana_sources={} -- only junk whose corpse-to-inventory receipt was confirmed
local helper_pending=nil
local random_helper_at=0
local debuff_pending=nil
local debuff_scan=nil
local purchase_pending=nil
local note_pending=nil
local pea_pending=nil
local pet_pending=nil
local dispel_pending=nil
local last_cast, unavailable_spells = nil, {}
local forced_buff=nil
local buff_skips={}
local buff_recast_until=0
local other_job=nil
local loot_scan={}
local inventory_salvage_scan={}
local inventory_salvage_blocked={}
local loot_jobs={}
local loot_job_pending=nil
local scroll_reads={}
local combine_pending=nil
local stack_pending=nil
local combine_blocked,stack_blocked={},{}
local history_cleanup_at=0
-- Retry limits belong to live objects. A hunting session continually sees new
-- GUIDs; keeping their appraisal/equipment/debuff attempts forever leaks the
-- Lua heap even after the host has removed the corresponding world objects.
local function prune_history(s)
 if s.time<history_cleanup_at then return end
 history_cleanup_at=s.time+10
 for _,timers in ipairs({until_time,corpses,buff_skips,unavailable_spells}) do
  for key,expires in pairs(timers) do if expires<=s.time then timers[key]=nil end end
 end
 local present={};if s.player then present[s.player]=true end
 for _,list in ipairs({s.inventory or {},s.contents or {},s.targets or {},s.corpses or {},s.route_objects or {},
  (s.fellowship or {}).members or {},(s.buff_request or {}).equipment or {}}) do
  for _,object in ipairs(list) do if object.id then present[object.id]=true end end
 end
 for _,blocked in ipairs({combine_blocked,stack_blocked}) do
  for id in pairs(blocked) do if not present[id] then blocked[id]=nil end end
 end
 if s.container then present[s.container]=true end
 if s.buff_request and s.buff_request.player then present[s.buff_request.player]=true end
 local function target_id(text)
  for i=1,#text do if text:sub(i,i)==':' then return tonumber(text:sub(1,i-1)) end end
  return tonumber(text)
 end
 for key in pairs(retries) do
  local id=target_id(tostring(key));if id and not present[id] then retries[key]=nil end
 end
 for key,value in pairs(item_attempts) do
  local id
  if key:sub(1,2)=='id' then id=tonumber(key:sub(3))
  elseif key:sub(1,5)=='equip' then id=target_id(key:sub(6))
  elseif key:sub(1,6)=='corpse' then id=tonumber(key:sub(7))
  elseif key:sub(1,6)=='debuff' then id=target_id(key:sub(7))
  elseif key:sub(1,6)=='helper' then id=tonumber(key:sub(7,-2)) end
  if value==0 or (id and not present[id]) then item_attempts[key]=nil end
 end
end
-- An operational failure belongs to one activity, never the whole scheduler.
-- Cooldowns prevent a failing high-priority activity starving combat/recovery.
local activity,activity_time,activity_pauses='meta',0,{}
local activity_pause_reasons={}
local function activity_ready(name)
 local until_at=activity_pauses[name]
 if until_at and activity_time<until_at then return false end
 activity_pauses[name]=nil;activity_pause_reasons[name]=nil;activity=name;return true
end
local function failed(reason)
 return {action='activity_failed',activity=activity,status=reason}
end
local follow_path,follow_target,follow_teleport={},nil,nil
local skill_categories = {[17]=45,[19]=47,[21]=3,[23]=46,[25]=5,[27]=9,[29]=10,[31]=44,[33]=12,[35]=13,
 [37]=6,[39]=7,[41]=15,[43]=31,[45]=32,[47]=33,[49]=34,[51]=16,[53]=14,[55]=29,[57]=18,[59]=30,[61]=28,
 [63]=27,[65]=20,[67]=21,[69]=22,[71]=35,[73]=23,[75]=36,[77]=24,[205]=19,[216]=39,[218]=37,[221]=38,
 [593]=41,[645]=43,[665]=52,[668]=49,[671]=50,[674]=48,[677]=51,[696]=54}
-- VT BuffController starts Creature mastery, Focus, Self/Willpower,
-- Mana Conversion and Life mastery. Finish the other magic schools before
-- ordinary buffs; category IDs preserve this order across every spell tier.
local casting_buff_order={43,9,11,51,47,45,49,645}
local casting_buff_family={};for _,category in ipairs(casting_buff_order) do casting_buff_family[category]=true end
local elements={32,4,8,64,16,2,1} -- retail damage bits: cold=8, fire=16
local damage_elements={1,2,4,8,16,32,64,1024}
local combat_school_skill={[1]=34,[2]=33,[3]=32,[4]=31,[5]=43}
local function trained_skill(s,id)
 local skill=s.skills and s.skills[tostring(math.floor(id or 0))]
 return skill and (skill.training or 0)>=2 and (skill.current or 0) or 0
end
local function pct(v,m) return m>0 and v*100/m or 0 end
local function result(a,status) return {action=a,status=status} end
local function loot_progress(status) return {status=status,continue_work=true} end
local function pause_navigation(reason)
 navigation_paused='Navigation paused: '..reason..'. UCM remains active; enable Follow route to retry'
 route_pending=nil;route_trail={};route_returning=false;route_walking=false
 route_join_pending=true;route_join_scan=nil
 return result('pause_navigation',navigation_paused)
end
local function contains(list,id) for _,v in ipairs(list or {}) do if v==id then return true end end return false end
local function vt_string(value) if type(value)=='number' then return string.format('%.15g',value) end;return tostring(value) end
local function allowed(list,id) return not list or #list==0 or contains(list,id) end
local function band(a,b) return (a or 0)&b end
local function pet_device(v)
 local q=v.int_properties or {}
 return v.pet_device or q['280']==213 or q['366']==54 or q['368']==54 or (q['362'] or 0)>0 or (q['266'] or 0)>0
end
local function equipment_selection(p,inventory)
 local selected_types,selected_ids,weapon_types,weapon_ids={},{},{},{}
 for _,id in ipairs(p.weapons or {}) do selected_types[id]=true;weapon_types[id]=true end
 for _,id in ipairs(p.weapon_items or {}) do selected_ids[id]=true;weapon_ids[id]=true end
 -- Essences share the equipment picker but are not weapons. Selecting just
 -- an essence must not exclude every wand/sword. Keep unknown selections
 -- restrictive: an absent selected weapon must not enable unrelated gear.
 for _,v in ipairs(inventory) do if pet_device(v) then
  if v.wcid then weapon_types[v.wcid]=nil end
  if v.id then weapon_ids[v.id]=nil end
 end end
 local restrict_types,restrict_ids=next(weapon_types)~=nil,next(weapon_ids)~=nil
 local automatic_pets=next(selected_types)==nil and next(selected_ids)==nil
 local function weapon_allowed(v)
  return not pet_device(v) and (not restrict_types or weapon_types[v.wcid]) and (not restrict_ids or weapon_ids[v.id])
 end
 local function pet_allowed(v) return automatic_pets or selected_types[v.wcid] or selected_ids[v.id] end
 return weapon_allowed,pet_allowed
end
local function route_kind(v)
 -- Earlier imports flattened NAV OpenVendor into Use. Only type 5 encoded
 -- both a GUID and name; preserve those saved routes without requiring import.
 if v.legacy and v.kind=='use' and v.object_id~=nil and v.object_name~=nil and not v.object_class then return 'vendor' end
 return v.kind or 'walk'
end
local function salvage_material(item)
 local material=item.material or 0
 return material>0 and material<=77 and material~=3 and material~=9 and material~=56 and material~=65 and material~=72
end
local function salvage_candidate(item)
 return not item.equipped and not item.retained and item.resource_available~=false
  and (not item.identified or item.workmanship~=nil)
  and item.object_class~=39 and item.object_class~=40 and item.object_class~=10
  and item.object_class~=38 and salvage_material(item)
end
local function salvage_protected(item)
 return ((item.int_properties or {})['171'] or 0)>0 or ((item.string_properties or {})['8'] or '')~=''
end
-- A timer or unrelated inventory update does not make a rejected combination
-- valid. Remember only the attempted items, until their relevant state changes
-- or the user explicitly restarts UCM. Removed objects are pruned above.
local function combination_state(item)
 return table.concat({vt_string(item.wcid or 0),vt_string(item.count or 1),vt_string(item.max_stack or 0),
  vt_string(item.structure or 0),vt_string(item.material or 0),vt_string(item.workmanship or -1),
  tostring(item.equipped==true),tostring(item.retained==true),tostring(item.resource_available~=false),
  tostring(salvage_protected(item))},':')
end
local function combination_rejected(blocked,item)
 local previous=blocked[item.id]
 if not previous then return false end
 if previous==combination_state(item) then return true end
 blocked[item.id]=nil;return false
end
local function world(v) local lb=math.floor(v.cell/65536);return -(math.floor(lb/256)*192+v.x),(lb%256)*192+v.y,v.z end
local function distance(a,b) local x,y,z=world(a);local X,Y,Z=world(b);return math.sqrt((x-X)^2+(y-Y)^2+(z-Z)^2) end
local function element(spell)
 local c=spell.category
 if c>=117 and c<=130 then return elements[(c-117)%7+1] end
 if c>=243 and c<=249 then return elements[c-242] end
 if c>=222 and c<=228 then return elements[c-221] end
 if c==640 or c==639 or c==636 or c==637 or c==638 or c==641 then return 1024 end
 return 0
end
local function resistance(t,damage) return t and t.resists and t.resists[tostring(math.floor(damage or 0))] or 1 end
-- Standard ACE appraisals omit elemental resistances. Like VT's GameInfoDB,
-- use named-monster/species preferences only when the server supplied none.
local function element_preference(t,damage)
 if not damage or damage==0 then return 0 end
 if t.resists and next(t.resists) then return resistance(t,damage) end
 for index,e in ipairs(t.preferred_elements or {}) do if e==damage then return 16-index end end
 return 0
end
-- VT ga.a resolves elemental rends, then cleaves; neither is the weapon's
-- displayed physical damage mask. Keep these separate from actual attack damage.
local function equipment_known(v)
 return v.identified or v.combat_identified
end
local function weapon_traits(v)
 local ints,floats=v.combat_int_properties or v.int_properties or {},v.combat_float_properties or v.float_properties or {}
 local imbue=ints['179'] or 0
 local rend=({[8]=1,[16]=2,[32]=4,[64]=32,[128]=8,[256]=64,[512]=16})[band(imbue,1023)] or 0
 if rend==0 then
  for _,e in ipairs({32,4,8,16,64,2,1}) do if band(ints['263'],e)~=0 then rend=e;break end end
 end
 local bonus=(band(imbue,4)~=0 and 4000 or 0)+(band(imbue,1)~=0 and 2000 or 0)+(band(imbue,2)~=0 and 1000 or 0)
  +((floats['155'] or 0)>0 and 400 or 0)+((floats['136'] or 0)>0 and 200 or 0)+((floats['147'] or 0)>0 and 100 or 0)
 return rend,bonus
end
local function preferred_damage(t,mask)
 local damage,value,preference=0,-1,-1
 for _,e in ipairs(damage_elements) do if band(mask,e)~=0 then
  local rank=element_preference(t,e);local resist=resistance(t,e)
  if rank>preference or (rank==preference and resist>value) then damage=e;value=resist;preference=rank end
 end end
 return damage,value,preference
end
local function cast(s,v,target,status)
 last_cast={id=v.id,category=v.category,target=target,serial=s.action_serial or 0,started=s.time}
 return {action='cast',spell=v.id,target=target,status=status}
end
local function action_receipt(s,kind,serial,started,id,target)
 local receipt
 for _,e in ipairs(s.action_events or {}) do
  if e.type==kind and (e.action_serial or 0)>serial and (not e.requested_at or e.requested_at>=started)
   and (kind=='cast' and e.spell==id or kind=='use' and e.source==id) and (not target or e.target==target)
   and (not receipt or e.action_serial>receipt.action_serial) then receipt=e end
 end
 return receipt
end
local function cast_receipt(s,q)
 if s.action_events then return action_receipt(s,'cast',q.serial,q.started,q.id or q.spell,q.target) end
 -- Compatibility with older snapshots. Current hosts retain the exact cast
 -- receipt even when another action completes before the next policy step.
 if (s.action_serial or 0)~=q.serial and s.last_spell==(q.id or q.spell) then
  return {error=s.action_error or 0,confirmed=s.last_spell_confirmed~=false}
 end
end
local function inventory_failure(s,q,ids)
 return (s.inventory_failure_serial or 0)>(q.failure_serial or 0) and ids[s.inventory_failure_item or 0]==true
end
-- Retail portal.dat puts Heal Self (including Adja's Intervention) in category
-- 67, also used by the timed Healing skill enchantment. Duration separates them.
-- Conversion categories identify the consumed vital, not the restored vital;
-- tier VII has distinct names and tier VIII adds "Incantation of".
local conversion_aliases={['Meditative Trance']='Stamina to Mana',['Rushed Recovery']='Stamina to Health',
 ['Energize Vitality']='Mana to Health',['Energize Vigor']='Mana to Stamina',['Cannibalize']='Health to Mana',['Self Sacrifice']='Health to Stamina'}
local vital_ids={Health=2,Stamina=4,Mana=6}
-- VT he.a/dg compare the expected conversion gain to a regular Heal Self.
-- These are selection estimates; server results remain authoritative.
local heal_amounts={17,25,32,45,67,87,115,135}
local transfer_ratios={.9,1,1.1,1.2,1.35,1.5,1.75,1.75}
local transfer_caps={50,100,150,200}
local function health_gain(v,source)
 local level=math.max(1,math.min(8,v.level or 1))
 return source and math.min(transfer_caps[level] or math.huge,math.floor(source*transfer_ratios[level])) or heal_amounts[level]
end
local function recovery_spell(v)
 if not v.caster_target or (v.duration or 0)~=0 then return end
 local c=v.category
 if c==67 or c==79 or c==257 then return 2,0 end
 if c==81 or c==407 then return 4,0 end
 if c==83 or c==259 then return 6,0 end
 local name=conversion_aliases[v.name] or v.name or ''
 if name:sub(1,15)=='Incantation of ' then name=name:sub(16) end
 for source,from in pairs(vital_ids) do for destination,to in pairs(vital_ids) do if from~=to then
  local prefix=source..' to '..destination
  if name==prefix or name:sub(1,#prefix+1)==prefix..' ' then return to,from end
 end end end
end
-- Host regex matching has bounded input, stack, match time and per-tick work.
local function literal_pattern(text,pattern) return regextest(text,pattern) end
local catalog_snapshot,catalog_margin,catalog_by_id,catalog_supplied,catalog_usable,catalog_groups
local function index_spells(s,margin)
 if catalog_snapshot~=s or (margin and catalog_margin~=margin) then
  catalog_snapshot=s;catalog_margin=margin or 30
  catalog_by_id,catalog_supplied,catalog_usable,catalog_groups=spellindex(s.spells or {},s.inventory or {},s.time,catalog_margin,unavailable_spells,s.components_required~=false)
 end
 return catalog_by_id,catalog_usable,catalog_groups
end
local function spell_supplied(s,spell)
 index_spells(s)
 return catalog_supplied[spell.id]==true
end
local function buff_element_enabled(p,spell)
 local c=spell.category;local letter,kind
 if c>=101 and c<=113 and c%2==1 then letter=({'A','B','C','L','F','P','S'})[(c-101)//2+1];kind='protection'
 elseif c>=162 and c<=174 and c%2==0 then letter=({'A','B','C','L','F','P','S'})[(c-162)//2+1];kind='bane'
 elseif c>=176 and c<=188 and c%2==0 then letter=({'B','S','P','L','C','F','A'})[(c-176)//2+1];kind='bane' end
 if not kind then return true end
 local profiles=kind=='protection' and {'CUSTOM','ALL','NONE','B','BPS','BPSA','ALFC','BPSAC'} or {'CUSTOM','ALL','NONE','B','BPS','BPSA','BPSAC','ALFC'}
 local selection=profiles[p[kind..'_profile'] or 2] or 'ALL'
 if selection=='CUSTOM' then selection=string.upper(p[kind..'_custom'] or 'ALL') end
 return selection=='ALL' or (selection~='NONE' and selection:contains(letter))
end
-- Zero means automatic; explicit tiers use the retail formula level, not
-- difficulty (special spells can have different difficulty at the same tier).
local function stronger_spell(v,other)
 if not other then return true end
 if (v.level or 0)~=(other.level or 0) then return (v.level or 0)>(other.level or 0) end
 return v.power>other.power
end
local function buff_tier(v,p)
 if v.school==3 and p.item_buffs_enabled==false then return false end
 local key=v.school==4 and 'creature_buff_level' or v.school==2 and 'life_buff_level' or v.school==3 and 'item_buff_level'
 local level=key and (p[key] or 0) or 0
 return level==0 or v.level==level
end
local function buff_skipped(s,target,category)
 local key=tostring(target)..':'..category
 return s.time<(buff_skips[key] or 0) or (forced_buff and forced_buff.skipped and forced_buff.skipped[key])
end
-- VT eq.b/g maintain ordinary attributes, trained skills, regeneration,
-- protections and configured equipment buffs, not every beneficial spell in
-- the spellbook (which also includes fellowship and special-purpose spells).
local automatic_buff_families={}
for _,c in ipairs({1,3,5,7,9,11,93,95,97,101,103,105,107,109,111,113,115,
 152,154,156,158,160,162,164,166,168,170,172,174,195,695}) do automatic_buff_families[c]=true end
local function buff_refresh_window(s,p,duration)
 local refresh=math.max(10,p.refresh_seconds or 300)
 -- VT fz/gw: after starting a buff, include near-expiring neighbours for 30s.
 if s.time<buff_recast_until then refresh=refresh+30 end
 return duration and duration>0 and math.min(refresh,duration*.5) or refresh
end
local function timed_buff(e)
 -- VT gz.cs sends only positive-duration enchantments to BuffController.
 -- Wield effects disappear with the buff wand; counting them here alternates
 -- the wand and combat weapon forever without casting the lasting self buffs.
 -- Older snapshot fixtures omitted duration and supplied only remaining time.
 return e.duration==nil or e.duration>0
end
-- A side-effect-free buff plan is shared by NeedBuff and the casting policy.
local function item_enchantable(item)
 -- Retail appraisal's ResistMagic (36), also used by ACE's IsEnchantable.
 -- Covenant is one example; never infer eligibility from an item's name.
 return item and ((item.int_properties or {})['36'] or 0)<9999
end
local function compute_buff(s,p)
 local trained={};for _,id in ipairs(s.trained_skills or {}) do trained[id]=true end
 -- Index selections once. Scanning long imported buff/exclusion lists for every
 -- spell made a fully learned spellbook exhaust the sandbox's tick budget.
 local selected,excluded={},{}
 for _,id in ipairs(p.buffs or {}) do selected[id]=true end
 for _,id in ipairs(p.excluded_buffs or {}) do excluded[id]=true end
 local excluded_ids={};for _,id in ipairs(p.excluded_buff_spells or {}) do excluded_ids[id]=true end
 local by_id=index_spells(s)
 local families={}
 for id in pairs(excluded_ids) do local v=by_id[id];if v then excluded[v.category]=true end end
 for id in pairs(selected) do local v=by_id[id];if v then families[v.category]=true end end
 local best,order,blocked,active,confirmed,item_best,item_blocked={},{},{},{},{},{},{}
 local eligibility={}
 local item_targets=p.item_buff_targets and #p.item_buff_targets>0
 local margin=p.buff_skill_margin or p.skill_margin or 30
 local tiers={[2]=p.life_buff_level or 0,[3]=p.item_buff_level or 0,[4]=p.creature_buff_level or 0}
 for _,v in ipairs(catalog_groups.buffs) do
  if item_targets and v.known~=false and v.beneficial and v.school==3 and (v.duration or 0)>0 and (v.skill or 0)>=v.power+margin and buff_tier(v,p) and not excluded[v.category] then
   if not item_best[v.category] or item_best[v.category].power<v.power then
    if s.time<(unavailable_spells[v.id] or 0) or not catalog_supplied[v.id] then item_blocked[v.category]=true
    else item_best[v.category]=v end
   end
  end
  if v.known~=false and (v.school~=3 or p.item_buffs_enabled~=false) and (v.self_buff or (v.beneficial and v.school==3 and (v.duration or 0)>0
   and (v.caster_target or band(v.target_type,0x8107)~=0))) then
  local eligible=eligibility[v.category]
  if eligible==nil then
   local skill=skill_categories[v.category]
   eligible=((p.auto_buffs~=false and (skill and trained[skill] or automatic_buff_families[v.category]) and buff_element_enabled(p,v)) or families[v.category])==true
   eligibility[v.category]=eligible
  end
  -- Automatic maintenance uses sustained buffs. Burst spells such as Tusker
  -- Leap (10 seconds) are only included when explicitly selected.
  if eligible and (band(v.flags,0x2000)==0 or (families[v.category] and s.in_fellowship))
   and ((v.duration or 300)>=300 or selected[v.id]) and not excluded[v.category] and (not best[v.category] or v.power>best[v.category].power) then
   local tier=tiers[v.school] or 0
   if (tier~=0 and v.level~=tier) or (v.skill or 0)<v.power+margin
    or s.time<(unavailable_spells[v.id] or 0) or not catalog_supplied[v.id] then blocked[v.category]=v
   else
    if not best[v.category] then order[#order+1]=v.category end
    best[v.category]=v
   end
  end
  end
 end
 -- Order only the chosen families, not the entire spellbook. Keep the
 -- remaining families stable and preserve exclusions/skill/component checks.
 local ordered={}
 for _,category in ipairs(casting_buff_order) do if best[category] then ordered[#ordered+1]=category end end
 for _,category in ipairs(order) do if not casting_buff_family[category] then ordered[#ordered+1]=category end end
 order=ordered
 -- A configured refresh window must never make a freshly cast short buff stale.
 local function refresh_window(duration) return buff_refresh_window(s,p,duration) end
 for _,e in ipairs(s.enchantments or {}) do local v=by_id[e.id] or best[e.category]
  if timed_buff(e) and e.remaining>refresh_window(e.duration or (v and v.duration)) then active[e.category]=math.max(active[e.category] or 0,e.power) end
 end
 for _,e in ipairs(s.item_buffs or {}) do local v=best[e.category] or item_best[e.category]
  if e.expires-s.time>refresh_window(e.duration or (v and v.duration)) then local k=tostring(e.target)..':'..e.category;confirmed[k]=math.max(confirmed[k] or 0,e.power) end
 end
 -- Retail equipment-compatible item spells accept the player. Banes/impen
 -- spread to worn armor, and weapon auras apply to the character. Do not cast
 -- the same family on every carried weapon or armor piece. Other-player banes
 -- intentionally retain their separate equipment targeting below.
 local id=s.player or 0
 local owned,usable_names,current_weapon={},{},nil
 for _,v in ipairs(s.inventory or {}) do owned[v.id]=v;if v.usable and v.name then usable_names[v.name]=v end;if v.equipped and band(v.type,0x8101)~=0 then current_weapon=v end end
 local missing={}
 for _,e in ipairs(s.item_buffs or {}) do
  local v=best[e.category] or item_best[e.category]
  if v and e.target==id and confirmed[tostring(id)..':'..e.category] then
   for _,item in ipairs(e.missing_equipment or {}) do
    if item_enchantable(owned[item]) and (confirmed[tostring(item)..':'..e.category] or -1)<v.power and not buff_skipped(s,item,e.category) then
     -- Worn armor is covered by one player-targeted bane, including after a
     -- gear swap or a partial timer restored at login. A shield taken off to
     -- hold the wand still needs a direct cast while it is in the backpack.
     local worn=owned[item].equipped and band(owned[item].slots or 0,0x200000)==0
     if worn then missing[e.category]=id else missing[e.category]=missing[e.category] or item end
    end
   end
  end
 end
 for _,category in ipairs(order) do local v=best[category]
  if not buff_skipped(s,id,category) then
  local refreshed=forced_buff and (forced_buff.done[tostring(id)..':'..v.category] or -1)>=v.power
  if forced_buff and not refreshed then return v,id end
  if not forced_buff and not ((active[v.category] or -1)>=v.power or (confirmed[tostring(id)..':'..v.category] or -1)>=v.power) then return v,id end
  if not forced_buff and missing[category] and not buff_skipped(s,missing[category],category) then return v,missing[category] end
  end
 end
 for _,binding in ipairs(p.item_buff_targets or {}) do
  local item=binding.item==0 and current_weapon or owned[binding.item];local exemplar=by_id[binding.spell]
  local v=exemplar and item_best[exemplar.category]
  if item_enchantable(item) and v then
   -- Keep UCM's player-targeted armor/weapon auras. Explicit non-redirectable
   -- item enchantments (for example a jewelry target) retain their own target.
   local target=(v.caster_target or band(v.target_type,0x8107)~=0) and id or item.id
   if (target==id or band(v.target_type,item.type or 0)~=0) and not buff_skipped(s,target,v.category) then
    local key=tostring(target)..':'..v.category
    if forced_buff and (forced_buff.done[key] or -1)<v.power then return v,target end
    if not forced_buff and (confirmed[key] or -1)<v.power and (target~=id or (active[v.category] or -1)<v.power) then return v,target end
    if not forced_buff and target==id and missing[v.category] then return v,missing[v.category] end
   end
  elseif item and exemplar and item_blocked[exemplar.category] then blocked[exemplar.category]=exemplar
  end
 end
 local incomplete=false
 for category,v in pairs(blocked) do if not best[category] then
  if forced_buff and not forced_buff.done[tostring(id)..':'..category] or (not forced_buff and not (active[category] or confirmed[tostring(id)..':'..category])) then incomplete=true end
 end end
 for _,expires in pairs(buff_skips) do if s.time<expires then incomplete=true;break end end
 for _,b in ipairs(p.buff_items or {}) do
  local v=by_id[b.spell]
  if v and (v.school~=3 or p.item_buffs_enabled~=false) and (band(v.flags,0x2000)==0 or s.in_fellowship) and (active[v.category] or -1)<v.power then
   if usable_names[b.name] and (item_attempts['buffitem'..b.name] or 0)<3 then return nil,nil,incomplete,true end
  end
 end
 return nil,nil,incomplete
end
-- NeedBuff may occur in many rules in the same meta tick. Reuse the plan only
-- within that snapshot and identical options; never cache server confirmations
-- or inventory across ticks.
local buff_plan
local function next_buff(s,p)
 local keys={'auto_buffs','item_buffs_enabled','buffs','excluded_buffs','excluded_buff_spells','item_buff_targets','buff_items','skill_margin','buff_skill_margin','refresh_seconds','creature_buff_level','life_buff_level','item_buff_level','protection_profile','bane_profile','protection_custom','bane_custom'}
 local same=buff_plan and buff_plan.snapshot==s
 if same then for _,k in ipairs(keys) do if buff_plan.options[k]~=p[k] then same=false;break end end end
 if not same then
  local v,id,blocked,consumable=compute_buff(s,p)
  local options={};for _,k in ipairs(keys) do options[k]=p[k] end
  buff_plan={snapshot=s,options=options,spell=v,target=id,blocked=blocked,consumable=consumable}
 end
 return buff_plan.spell,buff_plan.target,buff_plan.blocked,buff_plan.consumable
end
-- Legacy meta state is session-local; imported options never overwrite saved profiles.
local meta={name='Default',fired={},stack={},options={},route_revision=0,queue={}}
local function meta_state(name,s)
 meta.name=name;meta.entered=s.time;meta.persistent_entered=s.time;meta.fired={};meta.watchdog=nil
 meta.chat_start=s.chat_serial or 0;meta.portal_start=s.portal_serial or 0
end
local meta_expression
local function meta_command(c,s)
 if c.op=='stop' then return 'Stopped by UCM command'
 elseif c.op=='start' or c.op=='noop' then return -- Already executing within an explicitly started run.
 elseif c.op=='option' then
  meta.options[c.key]=c.value
  -- An explicit meta recovery command can retry navigation, just like the
  -- player's switch. Merely continuing the old meta must not clear a pause.
  if c.key=='navigation' and c.value and navigation_paused then
   navigation_paused=nil;route_blocks=0;door_attempts={};route_join_pending=true;route_join_scan=nil
  end
 elseif c.op=='reverse_route' then route_direction=-route_direction;meta.notice='Nav backwards is: '..tostring(route_direction<0)
 elseif c.op=='reverse_query' then meta.notice='Nav backwards is: '..tostring(route_direction<0)
 elseif c.op=='state' then meta_state(c.state,s)
 elseif c.op=='expression' then meta_expression(c.expression,s)
 elseif c.op=='forcebuff' then
  meta.forcebuff=c.value
  if c.value then if not forced_buff then forced_buff={request=0,done={}};buff_plan=nil end
  elseif forced_buff and forced_buff.request==0 then forced_buff=nil;buff_plan=nil end
 elseif c.op=='echo' then meta.notice=c.text
 elseif c.op=='plugin' or c.op=='use' or c.op=='say' or c.op=='give' or c.op=='select' or c.op=='use_selected' or c.op=='combat_mode' or c.op=='logout' or c.op=='jump' or c.op=='load' or c.op=='recall' or c.op=='face' or c.op=='fellowship' or c.op=='confirm' or c.op=='attack_bar' then
  if #meta.queue>=64 then return 'Meta command queue exceeds 64 actions' end
  meta.queue[#meta.queue+1]=c
  if c.op=='load' then meta.awaiting_load=true end
 else return 'Unsupported meta command' end
end
local meta_vars={}
local object_refs,object_snapshot={},nil
local meta_active_profile={}
local species_names={}
local function truth(v) return v~=nil and v~=false and v~=0 and v~='' end
-- Lists are reference objects, with zero-based public indexes. Keep their
-- identity and reject cycles before insertion, including indirect cycles.
local function list_value(items) return {vt_list=true,items=items or {}} end
local function check_list(v)
 if type(v)~='table' or not v.vt_list then error('Expected meta list') end
 return v.items
end
local function list_cycle(value,target,seen,depth)
 if value==target then return true end
 if type(value)~='table' or not value.vt_list then return false end
 if depth>32 then error('Meta list nesting exceeds 32') end
 if seen[value] then return false end;seen[value]=true
 for _,child in ipairs(value.items) do if list_cycle(child,target,seen,depth+1) then return true end end
 return false
end
meta_expression=function(e,s)
 local op=e.op
 if op=='literal' then return e.value end
 if op=='monster_var' then
  local t=s.monster or {};local n=e.name
  if n=='name' then return t.name or '' elseif n=='typeid' then return t.wcid or 0
  elseif n=='range' then return t.distance or 0 elseif n=='metastate' then return meta.name end
  if n=='hasshield' then return t.has_shield and 1 or 0 end
  local catalog=((s.profile or {}).monster_catalog or {})[t.name or ''] or {}
  if n=='maxhp' then return (t.max_health and t.max_health>0) and t.max_health or catalog.max_health or -1 end
  if n=='species' then
   local id=(t.creature_type and t.creature_type>0) and t.creature_type or catalog.species
   return id and species_names[tostring(math.floor(id))] or ''
  end
  error('Unknown monster variable: '..tostring(n))
 end
 if op=='call' then
  local args={};for i,a in ipairs(e.args) do args[i]=meta_expression(a,s) end
  local n=e.name;local key=vt_string(args[1] or '')
  if n=='isfalse' then return args[1]==0 or args[1]==false end
  if n=='istrue' then return args[1]==true or type(args[1])=='number' and args[1]~=0 end
  if n=='iif' then if args[1]==true or type(args[1])=='number' and args[1]~=0 then return args[2] end;return args[3] end
  if n=='strlen' then
   if type(args[1])~='string' then error('strlen requires a string') end
   local length=0;for _,code in utf8.codes(args[1]) do length=length+(code>0xffff and 2 or 1) end;return length
  end
  if n=='cnumber' then
   if type(args[1])~='string' then error('cnumber requires a string') end
   -- The legacy parser accepts decimal grouping and exponents, not Lua hex.
   local chars={};for i=1,#args[1] do local char=args[1]:sub(i,i)
    if char=='x' or char=='X' or char=='p' or char=='P' then return 0 end
    if char~=',' then chars[#chars+1]=char end
   end
   local text=table.concat(chars)
   local number=tonumber(text);return number and number==number and math.abs(number)<math.huge and number or 0
  end
  if n=='floor' or n=='ceiling' or n=='round' or n=='abs' or n=='randint' then
   local value=type(args[1])=='boolean' and (args[1] and 1 or 0) or args[1]
   if type(value)~='number' or value~=value or math.abs(value)==math.huge then error(n..' requires a finite number') end
   if n=='floor' then return math.floor(value) elseif n=='ceiling' then return math.ceil(value) elseif n=='abs' then return math.abs(value) end
   if n=='round' then
    local lower=math.floor(value);local fraction=value-lower
    if fraction==.5 then return lower%2==0 and lower or lower+1 end -- .NET midpoint-to-even.
    return fraction<.5 and lower or lower+1
   end
   local upper=type(args[2])=='boolean' and (args[2] and 1 or 0) or args[2]
   if type(upper)~='number' or upper~=upper or value< -2147483648 or value>2147483647 or upper< -2147483648 or upper>2147483647 then error('randint requires 32-bit bounds') end
   value=math.modf(value);upper=math.modf(upper)
   if value>upper then error('randint minimum exceeds maximum') end
   return value==upper and value or math.random(value,upper-1)
  end
  if n=='listcreate' then return list_value(args) end
  if n:sub(1,4)=='list' then
   local values=check_list(args[1]);local count=#values
   local function index(v,insert)
    if type(v)~='number' or v%1~=0 or v<0 or v>=count+(insert and 1 or 0) then error('Meta list index out of range') end
    return v+1
   end
   if n=='listcount' then return count end
   if n=='listgetitem' then return values[index(args[2])] end
   if n=='listclear' then args[1].items={};return args[1] end
   if n=='listcopy' or n=='listreverse' then local copy={};for i=1,count do copy[i]=values[n=='listreverse' and count-i+1 or i] end;return list_value(copy) end
   if n=='listadd' or n=='listinsert' then
    if count>=1024 then error('Meta list exceeds 1024 items') end
    if list_cycle(args[2],args[1],{},0) then error('Meta lists cannot contain themselves') end
    table.insert(values,n=='listinsert' and index(args[3],true) or count+1,args[2]);return args[1]
   end
   if n=='listremoveat' then table.remove(values,index(args[2]));return args[1] end
   local found=-1
   for i,v in ipairs(values) do if v==args[2] then found=i-1;if n~='listlastindexof' then break end end end
   if n=='listcontains' then return found>=0 and 1 or 0 end
   if n=='listindexof' or n=='listlastindexof' then return found end
   if n=='listremove' then if found>=0 then table.remove(values,found+1) end;return args[1] end
   error('Unsupported list operation: '..n)
  end
  if n=='getworldname' then return s.world_name or '' end
  if n:sub(1,9)=='getfellow' then
   local f=s.fellowship or {};local members=f.members or {};local active=f.valid==true
   local leader=active and f.leader==s.player
   if n=='getfellowshipstatus' then return active and 1 or 0 end
   if n=='getfellowshipname' then return active and f.name or '' end
   if n=='getfellowshipcount' then return active and #members or 0 end
   if n=='getfellowshipleaderid' then return active and f.leader or 0 end
   if n=='getfellowshiplocked' then return active and f.locked and 1 or 0 end
   if n=='getfellowshipisleader' then return leader and 1 or 0 end
   if n=='getfellowshipisopen' then return active and f.open and 1 or 0 end
   if n=='getfellowshipisfull' then return active and #members>=9 and 1 or 0 end
   if n=='getfellowshipcanrecruit' then return active and #members<9 and (leader or f.open) and 1 or 0 end
   if n=='getfellownames' or n=='getfellowids' then local values={};if active then for _,m in ipairs(members) do values[#values+1]=n=='getfellownames' and m.name or m.id end end;table.sort(values);return list_value(values) end
  end
  if n=='getvar' then if meta_vars[key]==nil then return 0 end;return meta_vars[key] end
  if n=='testvar' then return meta_vars[key]~=nil end
  if n=='clearallvars' then meta_vars={};return true end
  if n=='clearvar' then meta_vars[key]=nil;return true end
  if n=='setvar' or n=='touchvar' then
   if n=='touchvar' and meta_vars[key]~=nil then return 1 end
   if #key>256 then error('Meta variable name exceeds 256 bytes') end
   local count=0;for _ in pairs(meta_vars) do count=count+1 end
   if count>=256 and meta_vars[key]==nil then error('Meta exceeds 256 variables') end
   meta_vars[key]=n=='touchvar' and 0 or args[2];return meta_vars[key]
  end
  if n=='getobjectinternaltype' then local t=type(args[1]);return t=='table' and 7 or t=='string' and 3 or (t=='number' or t=='boolean') and 1 or 0 end
  if n=='cstr' then local v=args[1];if type(v)=='boolean' then v=v and 1 or 0 end;if type(v)~='number' then error('cstr requires a number') end;return vt_string(v) end
  if n=='getisspellknown' then
   if type(args[1])~='number' then error('getisspellknown requires a spell ID') end
   if s.known_spells then return contains(s.known_spells,args[1]) end
   for _,spell in ipairs(s.spells or {}) do if spell.id==args[1] then return spell.known~=false end end
   return false
  end
  if n=='cstrf' then
   local format=tostring(args[2]);local digits=tonumber(format:sub(2));if format:sub(1,1)~='F' or not digits or digits<0 or digits>9 then error('Only F0 through F9 numeric formatting is supported') end
   return string.format('%.'..digits..'f',args[1])
  end
  local function object(id)
   if type(id)=='table' then if not id.vt_object then return nil end;id=id.id end
   if id==s.player then return {id=id,name=(s.char_strings or {})['1'] or '',object_class=24,cell=s.position.cell,x=s.position.x,y=s.position.y,z=s.position.z} end
   for _,list in ipairs({s.inventory or {},s.route_objects or {},s.targets or {},s.corpses or {}}) do for _,o in ipairs(list) do if o.id==id then return o end end end
  end
  if n=='getcharintprop' or n=='getcharquadprop' or n=='getchardoubleprop' or n=='getcharboolprop' or n=='getcharstringprop' then
   local id=args[1];if type(id)~='number' or id~=id or id< -2147483648 or id>2147483647 then error('Character property requires a numeric 32-bit ID') end
   id=id<0 and math.ceil(id) or math.floor(id)
   local field=({getcharintprop='char_ints',getcharquadprop='char_quads',getchardoubleprop='char_doubles',getcharboolprop='char_bools',getcharstringprop='char_strings'})[n]
   local values=s[field]
   return (values or {})[string.format('%.0f',id)] or 0
  end
  if n:sub(1,13)=='getcharskill_' then
   if type(args[1])~='number' then error('Skill query requires a numeric skill ID') end
   local skill=(s.skills or {})[key] or {};return skill[n=='getcharskill_base' and 'base' or n=='getcharskill_buffed' and 'current' or 'training'] or 0
  end
  if n:sub(1,13)=='getcharvital_' then
   if type(args[1])~='number' then error('Vital query requires a numeric vital ID') end
   local vital=({'health','stamina','mana'})[math.floor(args[1])]
   return math.max(1,vital and s[(n=='getcharvital_base' and 'base_' or n=='getcharvital_buffedmax' and 'max_' or '')..vital] or 1)
  end
  local function object_ref(o)
   if object_snapshot~=s then
    object_snapshot=s;local present={[s.player or 0]=true}
    for _,list in ipairs({s.inventory or {},s.route_objects or {},s.targets or {},s.corpses or {}}) do for _,v in ipairs(list) do present[v.id]=true end end
    for id in pairs(object_refs) do if not present[id] then object_refs[id]=nil end end
   end
   if not o then return 0 end
   object_refs[o.id]=object_refs[o.id] or {vt_object=true,id=o.id};return object_refs[o.id]
  end
  if n=='getplayerlandcell' then return s.position.cell end
  if n=='wobjectgetselection' then return object_ref(object(s.selected)) end
  if n=='wobjectgetplayer' then return object_ref(object(s.player)) end
  if n=='wobjectgetobjectclass' or n=='wobjectgettemplatetype' then
   local o=object(args[1]);if not o then error('World object is unavailable') end
   return o[n=='wobjectgetobjectclass' and 'object_class' or 'wcid'] or 0
  end
  if n=='wobjectfindininventorybytemplatetype' then
   for _,o in ipairs(s.inventory or {}) do if o.wcid==args[1] then return object_ref(o) end end;return 0
  end
  if n=='actiontryselect' then
   if not object(args[1]) then return 0 end
   meta.queue[#meta.queue+1]={op='select',item_id=object(args[1]).id};return 1
  end
  if n=='actiontryequipanywand' or n=='actiontrycastbyid' or n=='actiontrycastbyidontarget' or n=='getcancastspell_hunt' or n=='getcancastspell_buff' then
   local p=meta_active_profile;local spell
   local function setting(k,fallback) local v=meta.options[k];if v==nil then v=p[k] end;if v==nil then return fallback end;return v end
   if n~='actiontryequipanywand' then
    if type(args[1])~='number' then error('Spell expression requires numeric spell ID') end
    for _,v in ipairs(s.spells or {}) do if v.id==args[1] then spell=v;break end end
    local eligible=spell and spell.known~=false and spell.skill>=spell.power+setting(n=='getcancastspell_buff' and 'buff_skill_margin' or 'skill_margin',n=='getcancastspell_buff' and 5 or 30)
    eligible=eligible and spell_supplied(s,spell)
    if n=='getcancastspell_hunt' or n=='getcancastspell_buff' then return eligible and 1 or 0 end
    local untargeted=spell and (spell.caster_target or spell.target_type==0)
    if not eligible or (n=='actiontrycastbyid' and not untargeted) or (n=='actiontrycastbyidontarget' and (untargeted or not object(args[2]))) then return 2 end
   end
   if s.busy or s.ready==false or meta.pending or #meta.queue>0 or s.jumping then return 0 end
   local wand,unknown
   local weapon_allowed=equipment_selection(p,s.inventory or {})
   for _,v in ipairs(s.inventory or {}) do if band(v.type,0x8000)~=0 then
    if v.equipped then wand=v;break end
    if weapon_allowed(v) then
     if equipment_known(v) and v.can_wield then wand=wand or v elseif not equipment_known(v) then unknown=unknown or v end
    end
   end end
   if not wand then if unknown then meta.queue[#meta.queue+1]={op='identify',item_id=unknown.id} end;return 0 end
   if not wand.equipped then meta.queue[#meta.queue+1]={op='equip',item_id=wand.id};return 0 end
   if s.combat_mode~=8 then meta.queue[#meta.queue+1]={op='combat_mode',mode=8};return 0 end
   if n=='actiontryequipanywand' then return 1 end
   meta.queue[#meta.queue+1]={op='cast',spell=spell.id,target_id=n=='actiontrycastbyid' and s.player or object(args[2]).id};return 1
  end
  if n=='wobjectfindininventorybyname' then for _,o in ipairs(s.inventory or {}) do if string.lower(o.name)==string.lower(key) then return object_ref(o) end end;return 0 end
  if n=='wobjectfindininventorybynamerx' then
   if type(args[1])~='string' then error('Inventory name regex must be a string') end
   for _,o in ipairs(s.inventory or {}) do if literal_pattern(o.name or '',args[1]) then return object_ref(o) end end;return 0
  end
  if n=='wobjectfindnearestmonster' then
   local best;for _,o in ipairs(s.targets or {}) do
    if s.time>=(monster_blacklist[o.id] or 0) and (not best or o.distance<best.distance) then best=o end
   end;return object_ref(best)
  end
  if n=='wobjectfindnearestdoor' or n=='wobjectfindnearestbyobjectclass' or n=='wobjectfindnearestbynameandobjectclass' then
   local cls=n=='wobjectfindnearestdoor' and 26 or args[1];local best
   if type(cls)~='number' or (n=='wobjectfindnearestbynameandobjectclass' and type(args[2])~='string') then error('World lookup requires a numeric class and string pattern') end
   for _,list in ipairs({s.route_objects or {},s.targets or {},s.corpses or {}}) do for _,o in ipairs(list) do
    if o.id~=s.player and o.object_class==cls and (n~='wobjectfindnearestbynameandobjectclass' or literal_pattern(o.name or '',args[2])) and (not best or o.distance<best.distance) then best=o end
   end end
   return object_ref(best)
  end
  if n=='wobjectgetname' then local o=object(args[1]);return o and o.name or '' end
  if n=='wobjectgetisdooropen' then local o=object(args[1]);return o and o.door_open or false end
  if n=='getplayercoordinates' then return s.position end
  if n=='wobjectgetphysicscoordinates' then local o=object(args[1]);if not o or not o.cell then error('Object position is unavailable') end;return o end
  if n=='coordinateparse' then
   if type(args[1])~='string' then error('coordinateparse requires text') end
   local groups=regexmatch(args[1],'([0-9]+(?:[.][0-9]+)?)\\s*([nNsS])[\\s,]*([0-9]+(?:[.][0-9]+)?)\\s*([eEwW])')
   if not groups then return 0 end
   local ns,a,ew,b=groups['1'],groups['2'],groups['3'],groups['4']
   ns,ew=tonumber(ns),tonumber(ew);if ns>1000 or ew>1000 then return 0 end
   if a:lower()=='s' then ns=-ns end;if b:lower()=='w' then ew=-ew end
   -- A synthetic landblock zero coordinate preserves signed global locations.
   return {cell=0,x=24468+ew*240,y=24468+ns*240,z=0}
  end
  if n=='coordinategetns' or n=='coordinategetwe' or n=='coordinategetz' then
   local x,y,z=world(args[1]);return n=='coordinategetns' and (y-24468)/240 or n=='coordinategetwe' and (-x-24468)/240 or z/240
  end
  if n=='coordinatedistancewithz' then return distance(args[1],args[2]) end
  if n=='coordinatedistanceflat' then local x,y=world(args[1]);local X,Y=world(args[2]);return math.sqrt((x-X)^2+(y-Y)^2) end
  if n=='coordinatetostring' then local x,y,z=world(args[1]);return string.format('%.4f%s, %.4f%s, %.4fZ',math.abs((y-24468)/240),y>=24468 and 'N' or 'S',math.abs((-x-24468)/240),-x>=24468 and 'E' or 'W',z/240) end
  if n=='actiontryuseitem' or n=='actiontryapplyitem' or n=='actiontrygiveitem' then
   local item=object(args[1]);local target=object(args[2]);if not item or (n~='actiontryuseitem' and not target) or s.busy or s.ready==false or meta.pending or #meta.queue>0 then return false end
   if n=='actiontrygiveitem' then local owned=false;for _,i in ipairs(s.inventory or {}) do if i.id==item.id and not i.equipped then owned=true;break end end;if not owned then return false end end
   meta.queue[#meta.queue+1]={op=n=='actiontrygiveitem' and 'give' or 'use',item_id=item.id,target_id=target and target.id};return true
  end
  if n=='stopwatchcreate' then return {stopwatch=true,elapsed=0} end
  local w=args[1]
  if type(w)~='table' or not w.stopwatch then error('Expected stopwatch') end
  if n=='stopwatchstart' then w.started=w.started or s.time;return w end
  if n=='stopwatchstop' then if w.started then w.elapsed=w.elapsed+s.time-w.started;w.started=nil end;return w end
  if n=='stopwatchelapsedseconds' then return w.elapsed+(w.started and s.time-w.started or 0) end
  error('Unsupported expression function: '..n)
 end
 local a=meta_expression(e.left,s)
 if op=='not' then return not truth(a) end
 if op=='negative' then return -a end
 -- VT evaluates both operands, including function side effects.
 local b=meta_expression(e.right,s)
 if op==';' then return a end
 if type(a)=='boolean' then a=a and 1 or 0 end
 if type(b)=='boolean' then b=b and 1 or 0 end
 if type(a)~=type(b) then error('Meta operator requires operands of the same type') end
 if type(a)=='table' then error('Meta operators do not accept internal objects; use object queries') end
 if type(a)=='string' then
  if op=='&&' or op=='||' or op=='^' then error('Meta logical operators require numbers') end
  if op=='+' or op=='-' then local text=a..(op=='-' and '-' or '')..b;if #text>4096 then error('Meta string exceeds 4096 bytes') end;return text end
  if op=='#' then return literal_pattern(a,'(?i)'..b) and 1 or 0 end
  a=string.lower(a);b=string.lower(b)
 end
 if op=='==' then return a==b and 1 or 0 elseif op=='!=' then return a~=b and 1 or 0
 elseif op=='&&' then return truth(a) and truth(b) and 1 or 0
 elseif op=='||' then return (truth(a) or truth(b)) and 1 or 0
 elseif op=='<' then return a<b and 1 or 0 elseif op=='>' then return a>b and 1 or 0
 elseif op=='<=' then return a<=b and 1 or 0 elseif op=='>=' then return a>=b and 1 or 0
 elseif op=='+' then return a+b elseif op=='-' then return a-b elseif op=='*' then return a*b
 elseif op=='^' then return math.tointeger(math.modf(a)) ~ math.tointeger(math.modf(b))
 elseif op=='/' or op=='%' then
  if b==0 then error('Meta division by zero') end
  if op=='/' then return a/b end
  a=math.modf(a);b=math.modf(b);if b==0 then error('Meta division by zero') end;return math.fmod(a,b)
 end
 error('Unsupported expression operator: '..op)
end
local function monster_rule(t,p)
 local rule,defaults,best_match={},{},-1
 for _,r in ipairs(p.monsters or {}) do if r.default then defaults=r;break end end
 rule=defaults
 for _,r in ipairs(p.monsters or {}) do
  local matches
  if r.expression then
   local value=meta_expression(r.expression,{monster=t,profile=p})
   matches=type(value)=='string' and string.lower(t.name)==string.lower(value) or (type(value)~='string' and truth(value))
  end
  if not r.default then
   if matches then rule=r;break end -- Imported expressions retain their ordering.
   if not r.expression then
    local name=string.lower(r.name or '');local target_name=string.lower(t.name)
    local match=r.exact and target_name==name or not r.exact and target_name:sub(1,#name)==name
    local score=#name*2+(r.exact and 1 or 0)
    if match and score>best_match then rule=r;best_match=score end
   end
  end
 end
 -- Inherit without mutating saved rules; an explicit zero priority is valid.
 local effective={}
 -- Preserve legacy explicit inheritance until the native editor materializes
 -- it. New native rows copy Default once; imported rows remain independent.
 if rule.inherit_default then for k,v in pairs(defaults) do effective[k]=v end end
 for k,v in pairs(rule) do effective[k]=v end
 -- Older USD imports encoded disabled attack families as Ignore. That must not
 -- exclude those monsters from essence summoning. Explicit Ignore still does.
 if effective.ignore and effective.combat~='ignore' and effective.bolt==false and effective.streak==false and effective.ring==false then effective.ignore=false end
 if effective.priority==nil then effective.priority=p.monster_default_priority or 0 end
 -- VT f7 rejects a negative priority before equipment/attack evaluation.
 -- Apply at runtime too so already imported profiles gain the same behavior.
 if effective.priority<0 then effective.ignore=true end
 if (effective.range or 0)<=0 then effective.range=p.monster_default_range or 0 end
 return effective
end
local function monster_damage_attacks(rule,p)
 if rule.ignore or rule.combat=='ignore' or rule.combat=='off' then return false end
 if rule.bolt~=false or rule.streak~=false then return true end
 for _,key in ipairs({'ring','debuff_corruption','debuff_destructive','debuff_corrosion'}) do
  local value=rule[key];if value==nil then value=p[key] end
  if value then return true end
 end
 return false
end
local function monster_attacks(rule,p)
 if rule.ignore or rule.combat=='ignore' or rule.combat=='off' then return false end
 if monster_damage_attacks(rule,p) then return true end
 for _,key in ipairs({'debuff_imperil','debuff_vuln','debuff_yield','debuff_fester','debuff_broadside','debuff_gravity','debuff_weakening','debuff_festering'}) do
  local value=rule[key];if value==nil then value=p[key] end
  if value then return true end
 end
 return (rule.secondary_vuln or 0)~=0
end
local function meta_condition(c,s,p)
 local t,v,q=c.type,c.value,c.properties or {}
 if t==0 then return false elseif t==1 then return true end
 if t==26 then return truth(meta_expression(c.expression,s)) end
 if t==2 then for _,n in ipairs(c.children) do if not meta_condition(n,s,p) then return false end end;return true end
 if t==3 then for _,n in ipairs(c.children) do if meta_condition(n,s,p) then return true end end;return false end
 if t==21 then return not meta_condition(c.children[1],s,p) end
 if t==4 or t==28 then
  for _,e in ipairs(s.chat_events or {}) do if e.serial>meta.chat_start then
   local colors=';'..(q.c or '')..';';local colorok=t==4 or (q.c or '')=='' or colors:contains(';'..vt_string(e.color or 0)..';')
   local groups=colorok and regexmatch(e.text,t==4 and v or q.p)
   if groups then
    if t==28 then for key in pairs(meta_vars) do if key:sub(1,13)=='capturegroup_' then meta_vars[key]=nil end end;for k,text in pairs(groups) do meta_vars['capturegroup_'..k]=text end;meta_vars.capturecolor=e.color or 0 end
    return true
   end
  end end;return false
 end
 if t==5 then return (s.pack_slots or 0)<=v end
 if t==8 then return (s.health or 0)<=0 end
 if t==9 then return (s.vendor or 0)~=0 end
 if t==10 then return (s.vendor or 0)==0 end
 if t==15 then local v,_,blocked,consumable=next_buff(s,p);return v~=nil or consumable==true end
 if t==23 then for _,e in ipairs(s.enchantments or {}) do if e.id==q.sid and e.remaining>=q.sec then return true end end;return false end
 if t==24 then return (s.burden_percent or 0)>=v end
 if t==25 then
  if #(p.route or {})==0 then return false end
  for _,pt in ipairs(p.route) do local d=distance(s.position,pt);if d>0.00001 and d<=q.dist then return false end end;return true
 end
 if t==14 then local count=0;for _,m in ipairs(s.targets or {}) do
  local priority=monster_rule(m,p).priority or 0
  if m.distance<=q.r and priority==q.p then count=count+1 end
 end;return count>=q.c end
 if t==6 or t==22 then return s.time-(t==22 and meta.persistent_entered or meta.entered)>=v end
 if t==7 then return #(p.route or {})==0 or (not p.loop_route and not p.reverse_route and point>#p.route) end
 if t==11 or t==12 then
  local count=0;for _,i in ipairs(s.inventory or {}) do if string.lower(i.name)==string.lower(q.n) then count=count+(i.count or 1) end end
  return t==11 and count<=q.c or t==12 and count>=q.c
 end
 if t==13 or t==16 then
  local count=0;for _,m in ipairs(s.targets or {}) do if m.distance<=(q.r or 0) and (t==16 or literal_pattern(m.name,q.n)) then count=count+1 end end
  return t==16 and count==0 or t==13 and count>=q.c
 end
 if t==17 then return (math.floor(s.position.cell)&0xffff0000)==(math.floor(v)&0xffff0000) end
 if t==18 then return math.floor(s.position.cell)==(math.floor(v)&0xffffffff) end
 if t==19 or t==20 then for _,e in ipairs(s.portal_events or {}) do if e.serial>meta.portal_start and e.entered==(t==19) then return true end end;return false end
 error('Unsupported meta condition: '..t)
end
local function meta_action(a,s,p)
 local t,q=a.type,a.properties or {}
 if t==0 then return end
 if t==1 then meta_state(a.value,s)
 elseif t==2 then return meta_command(a.command,s)
 elseif t==3 then for _,c in ipairs(a.children) do local e=meta_action(c,s,p);if e then return e end end
 elseif t==4 then
  meta.route=a.route;meta.route_profile=a.route_profile;point=1;route_direction=1;route_pending=nil;route_join_pending=true;route_join_scan=nil;meta.route_revision=meta.route_revision+1;meta.route_changed=true
  route_trail={};route_returning=false;route_walking=false
 elseif t==5 then
  if #meta.stack>=128 then return 'Meta call stack exceeds 128 entries' end
  meta.stack[#meta.stack+1]=q.ret;meta_state(q.st,s)
 elseif t==6 then
  if #meta.stack==0 then return 'Meta return has no caller' end
  local name=meta.stack[#meta.stack];meta.stack[#meta.stack]=nil;meta_state(name,s)
 elseif t==7 then meta_expression(a.expression,s)
 elseif t==8 then return meta_command(ucmcommand(tostring(meta_expression(a.expression,s))),s)
 elseif t==9 then meta.watchdog={state=q.s,radius=q.r,seconds=q.t,next=s.time+q.t/10,samples={}}
 elseif t==10 then meta.watchdog=nil
 elseif t==11 then
  local defaults={auto_craft=false,auto_fellowship=false,craft_health_kits=0,craft_health_food=0,craft_stamina_kits=0,craft_stamina_food=0,craft_mana_kits=0,craft_mana_food=0,combat='off',buffing=true,navigation=false,looting=false,autostack=false,meta_enabled=true,idle_peace=false,open_doors=false,stop_on_death=false,autocram=false,nav_priority=false,radius=30,approach_range=60,waypoint_radius=.8,refresh_seconds=300,height=2,mana_threshold=45,health_threshold=65,stamina_threshold=50,door_range=2,
   item_mana=true,item_mana_threshold=20,read_unknown_scrolls=false,buff_skill_margin=p.skill_margin or 30,skill_margin=30,
   loot_fellow_corpses=false,loot_all_corpses=false,auto_attack_power=false,use_recklessness=false,idle_buff_topoff=false,idle_buff_seconds=1200,
   idle_health_threshold=0,idle_stamina_threshold=0,idle_mana_threshold=0,loot_range=15,loot_priority=false,loot_only_rare=false,salvage_combine=false,follow_corners=false,target_lock=true,
   kit_min_success=0,kits_in_magic=true,kit_peace=false,random_helper_buffs=false,random_helper_interval=5,helper_health_threshold=0,helper_stamina_threshold=0,helper_mana_threshold=0,
   helper_health_range=74.5,helper_stamina_range=74.5,helper_mana_range=40,ring_range=5,ring_min_targets=4,debuff_refresh=5,debuff_fallback=false,debuff_selection=2,
   split_peas=true,component_critical=4,component_normal=20,component_idle=20,fast_cast_buffs=true,cast_jiggle=false,jump_out_wand=false,switch_debuff_wand=true,
   summon_pets=false,pet_range_mode=0,pet_range=5,pet_min_targets=1,refill_summons=false,summon_refill_charges=5,fill_mana_stones=false,mana_tank_minimum=1000,
   protection_profile=2,bane_profile=2,protection_custom='ALL',bane_custom='ALL',target_select=1,target_angle_range=5,minimum_range=0,monster_attempts=4,monster_blacklist_seconds=120,corpse_attempts=30,corpse_blacklist_seconds=200,use_arcs=1,arc_range=5,debuff_each_first=1}
  local v=meta.options[a.key];if v==nil then v=p[a.key] end;if v==nil then v=defaults[a.key] end
  if a.key=='combat' then v=v~='off' end
  meta_vars[q.v]=a.boolean and (v and 1 or 0) or v/a.scale
 elseif t==12 then
  local value=meta_expression(a.expression,s)
  if a.boolean then value=truth(value) and 'true' or 'false' end
  return meta_command(ucmcommand('/acueucm opt set '..q.o..' '..tostring(value)),s)
 else return 'Unsupported meta action '..t end
end
local function meta_profile(s,p)
 local effective={};for k,v in pairs(p) do effective[k]=v end
 for k,v in pairs(meta.options) do effective[k]=v end
 if meta.route then for k,v in pairs(meta.route) do if k~='navigation' then effective[k]=v end end end
 if meta.forcebuff then effective.buffing=true end
 if p.vt_meta then effective.states=nil;effective.initial_state='Default' end
 return effective
end
-- Diagnostics must not evaluate conditions a second time: expressions can
-- mutate variables or queue game actions. Describe the next rules instead.
local function meta_condition_text(c,s,depth)
 if depth>5 or not workavailable() then return 'additional conditions' end
 local t,v,q=c.type,c.value,c.properties or {}
 if t==2 or t==3 or t==21 then
  local parts={};for i,n in ipairs(c.children or {}) do
   if i>4 then parts[#parts+1]='...';break end
   parts[#parts+1]=meta_condition_text(n,s,depth+1)
  end
  return (t==21 and 'not ' or '')..'('..table.concat(parts,t==3 and ' OR ' or ' AND ')..')'
 end
 if t==4 or t==28 then return 'chat matching '..tostring(t==4 and v or q.p):sub(1,180) end
 if t==11 or t==12 then
  local count=0;for _,i in ipairs(s.inventory or {}) do if string.lower(i.name or '')==string.lower(q.n or '') then count=count+(i.count or 1) end end
  return string.format('%s %s %d (have %d)',q.n or 'item',t==11 and '<=' or '>=',q.c or 0,count)
 end
 if t==6 or t==22 then return tostring(v)..' seconds in this state' end
 if t==13 then return string.format('%d monsters matching %s within %.1f m',q.c or 0,q.n or '',q.r or 0) end
 if t==14 then return string.format('%d monsters of priority %d within %.1f m',q.c or 0,q.p or 0,q.r or 0) end
 if t==16 then return string.format('no monsters within %.1f m',q.r or 0) end
 if t==17 or t==18 then return string.format('%s 0x%08X',t==17 and 'landblock' or 'cell',math.floor(v)&0xffffffff) end
 local names={[0]='never',[1]='always',[5]='free pack slots',[7]='route complete',[8]='death',[9]='vendor open',[10]='vendor closed',
  [15]='buffs needed',[19]='enter portal space',[20]='leave portal space',[23]='spell duration',[24]='burden',[25]='distance from route',[26]='expression'}
 return names[t] or ('condition '..tostring(t))
end
local function meta_status(s,p)
 if p.meta_enabled==false then return 'Meta paused; enable Run imported meta to process its rules.' end
 if meta.loading or meta.awaiting_load then return 'Loading referenced profile before continuing meta actions.' end
 local pending={};local total=0
 for i,r in ipairs(p.vt_meta or {}) do
  if not workavailable() then return 'Meta '..meta.name..': waiting for its next rule.' end
  if r.state==meta.name and not meta.fired[i] then
  total=total+1;if #pending<3 then pending[#pending+1]=meta_condition_text(r.condition,s,0) end
 end end
 if #pending==0 then return 'No pending rules in '..meta.name..'.' end
 return ('Waiting in '..meta.name..': '..table.concat(pending,'; OR ')..(total>3 and '; ...' or '')):sub(1,1600)
end
-- VT applies load commands synchronously. Native profile I/O is acknowledged
-- on the next snapshot, so suspend the remaining All actions at that boundary.
local function meta_actions(a,s,p)
 meta.actions=meta.actions or {}
 if a then meta.actions[#meta.actions+1]=a end
 while #meta.actions>0 do
  local next_action=table.remove(meta.actions,1)
  if next_action.type==3 then
   for i=#next_action.children,1,-1 do table.insert(meta.actions,1,next_action.children[i]) end
  else
   local err=meta_action(next_action,s,meta_profile(s,p));if err then return err end
   if meta.awaiting_load then return end
  end
 end
end
local function meta_tick(s,p)
 meta_active_profile=p
 -- Profile controls edit the base profile while imported commands keep a
 -- session-local overlay. An explicit UI change must supersede an older
 -- command, rather than leaving a working-looking checkbox with no effect.
 if meta.saved_options and meta.saved_revision~=(p.loot_revision or 0) then for key in pairs(meta.options) do
  if meta.saved_options[key]~=p[key] then meta.options[key]=nil end
 end end
 meta.saved_options={};for key,value in pairs(p) do
  if type(value)~='table' then meta.saved_options[key]=value end
 end
 meta.saved_revision=p.loot_revision or 0
 if meta.profile_revision~=(p.vt_revision or 0) then
  local first=meta.profile_revision==nil;meta.profile_revision=p.vt_revision or 0
  if not first and p.vt_loaded_kind=='usd' then
   meta.options={}
   recovery_pending=nil;buff_item_pending=nil;mana_refill_pending=nil;helper_pending=nil;random_helper_at=0;debuff_pending=nil;debuff_scan=nil;pea_pending=nil;pet_pending=nil;dispel_pending=nil;buff_plan=nil;last_cast=nil
   until_time={};retries={};item_attempts={}
  end
  if not first and p.vt_loaded_kind=='met' then meta={name='Default',fired={},stack={},options=meta.options,queue={},profile_revision=p.vt_revision,route_revision=0};meta_vars={};meta_state('Default',s) end
  if not first and (p.vt_loaded_kind=='nav' or p.vt_loaded_kind=='met') then
   point=1;route_direction=1;route_pending=nil;route_join_pending=true;route_join_scan=nil;meta.route=nil;meta.route_changed=false
   route_trail={};route_returning=false;route_walking=false
  end
  meta.loading=false
  meta.awaiting_load=false
  meta.profile_load=nil
 end
 if meta.loading or meta.awaiting_load then return end
 -- Recompute after an acknowledged USD clears the previous option overrides.
 p=meta_profile(s,p);meta_active_profile=p
 if not meta.entered then meta_state('Default',s) end
 if p.stop_on_death and (s.health or 0)<=0 then return 'Stopped on death' end
 if not p.vt_meta then return end
 if meta.actions and #meta.actions>0 then
  local err=meta_actions(nil,s,p);if err then return err end
  if meta.awaiting_load then return end
 end
 if meta_profile(s,p).meta_enabled==false then return end
 local w=meta.watchdog
 if w and s.time>=w.next then
  w.next=s.time+w.seconds/10;w.samples[#w.samples+1]=s.position
  if #w.samples>10 then table.remove(w.samples,1) end
  local trapped=#w.samples==10
  for _,pos in ipairs(w.samples) do if distance(pos,s.position)>w.radius then trapped=false end end
  if trapped then
   if #meta.stack>=128 then return 'Meta watchdog call stack overflow' end
   meta.stack[#meta.stack+1]=meta.name;meta_state(w.state,s);return
  end
 end
 local before=meta.name
 for i,r in ipairs(p.vt_meta) do
  if r.state==meta.name and not meta.fired[i] and meta_condition(r.condition,s,meta_profile(s,p)) then
   meta.fired[i]=true;local e=meta_actions(r.action,s,p);if e then return e end
   -- State changes, including a transition to the same state, rearm on the next tick.
   if meta.awaiting_load or meta.name~=before or not meta.fired[i] then break end
  end
 end
end
local function fellowship_done(s,q)
 local f=s.fellowship or {};local op=q.operation
 if op=='quit' or op=='disband' then return not f.valid end
 if not f.valid then return false end
 if op=='create' then return string.lower(f.name or '')==string.lower(q.name or '') end
 if op=='open' then return f.open==true end
 if op=='close' then return f.open==false end
 if op=='leader' then return f.leader==q.target end
 local member=false
 for _,m in ipairs(f.members or {}) do if m.id==q.target then member=true;break end end
 if op=='recruit' then return member end
 if op=='dismiss' then return not member end
 return false
end
local function meta_work(s,p)
 if not activity_ready('meta') then return end
 if meta.notice then local text=meta.notice;meta.notice=nil;return {action='notice',text=text,status=text} end
 if meta.turn then
  local delta=math.abs(((s.heading or 0)-meta.turn.heading+180)%360-180)
  if delta<2 then meta.turn=nil
  elseif s.time>meta.turn.deadline then return failed('Unable to face route heading')
  else return {action='face',heading=meta.turn.heading,status='Facing route heading'} end
 end
 if (s.health or 0)<=0 then meta.pending=nil;return result(nil,'Waiting for resurrection') end
 -- A NAV use/portal can wait for confirmation before it can complete.
 -- Respond to its immediately following confirmation command, then skip only
 -- that already executed command when the original server action completes.
 if route_pending and not route_pending.confirmed then
  local route=p.route or {};local current=route[point];local nextpoint=route[point+route_direction]
  local dialog=(s.confirmations or {})[1]
  if current and (current.kind=='use' or current.kind=='portal') and nextpoint and nextpoint.kind=='command' and nextpoint.command and nextpoint.command.op=='confirm' and dialog then
   if s.ready==false then return result(nil,'Waiting to respond to route confirmation') end
   route_pending.confirmed=true
   return {action='confirm',type=dialog.type,context=dialog.context,accept=nextpoint.command.accept,status='Confirming route interaction'}
  end
 end
 -- Confirmation is allowed while the original use is waiting for the server.
 local confirmation=meta.queue[1]
 if confirmation and confirmation.op=='confirm' then
  local dialog=(s.confirmations or {})[1]
  if s.ready==false then return result(nil,'Waiting to respond to confirmation') end
  if dialog then table.remove(meta.queue,1);return {action='confirm',type=dialog.type,context=dialog.context,accept=confirmation.accept,status='Responding to server confirmation'} end
  confirmation.deadline=confirmation.deadline or s.time+10
  if s.time>=confirmation.deadline then return failed('Expected server confirmation did not appear') end
  return result(nil,'Waiting for server confirmation')
 end
 if meta.pending then
  local q=meta.pending
  local receipt=not q.portal and q.kind~='give' and q.kind~='fellowship' and action_receipt(s,q.kind,q.serial,q.started,q.id,q.target)
  local fellow_done=false
  if q.kind=='fellowship' then
   local f=s.fellowship or {}
   if f.valid and q.fellowship_name and f.name~=q.fellowship_name then return failed('Fellowship changed while waiting for meta command') end
   fellow_done=fellowship_done(s,q)
  end
  local given=false
  if q.kind=='give' then
   local current
   for _,v in ipairs(s.inventory or {}) do if v.id==q.id then current=v;break end end
   -- A capped inventory snapshot cannot prove that a missing source was given.
   given=(not current and #(s.inventory or {})<1024) or (current and (current.count or 1)<q.count)
   if (s.inventory_failure_serial or 0)>q.failure_serial and s.inventory_failure_item==q.id then
    return failed('Meta give failed; check recipient, range and item requirements')
   end
  end
  if receipt and (receipt.error or 0)~=0 then return failed('Meta interaction failed; check target, range and item requirements') end
  if given or fellow_done or (receipt and (q.kind~='cast' or receipt.confirmed~=false))
   or (q.kind~='give' and q.kind~='fellowship' and (s.teleport_sequence or 0)~=q.teleport and not s.portal_space) then meta.pending=nil
  elseif s.time>=q.deadline then return failed('Meta interaction timed out')
  else
   -- Keep command order and the navigation hold, but let the normal scheduler
   -- recover/combat once the native action gate has released. Unrelated uses
   -- (including that recovery) cannot complete this pending command.
   if s.busy or s.ready==false or s.jumping then return result(nil,'Waiting for meta interaction') end
   return
  end
 end
 if meta.loading then return result(nil,'Waiting for profile load') end
 if #meta.queue==0 then return end
 if s.busy or s.ready==false or s.jumping then return result(nil,'Waiting to run meta command') end
 local c=meta.queue[1]
 local function immediate(intent) table.remove(meta.queue,1);return intent end
 if c.op=='load' then meta.loading=true;meta.profile_load=c;return immediate({action='profile_load',profile=c.profile,kind=c.kind,status='Loading '..c.profile}) end
 if c.op=='recall' then meta.pending={serial=s.action_serial or 0,teleport=s.teleport_sequence or 0,deadline=s.time+60,portal=true};return immediate({action='recall',destination=c.destination,status='Recalling to '..c.destination}) end
 if c.op=='say' then return immediate({action='say',text=c.text,channel=c.channel,status='Meta chat'}) end
 if c.op=='plugin' then return immediate({action='plugin_command',text=c.text,status='Meta plugin command'}) end
 if c.op=='attack_bar' then meta.options.power_percent=c.power*100;return immediate({action='attack_bar',power=c.power,status='Attack power updated'}) end
 if c.op=='combat_mode' then return immediate({action='combat_mode',mode=c.mode,status='Changing combat mode'}) end
 if c.op=='equip' or c.op=='identify' then return immediate({action=c.op,item=c.item_id,status='Meta: '..c.op}) end
 if c.op=='cast' then
  meta.pending={kind='cast',id=c.spell,target=c.target_id,started=s.time,serial=s.action_serial or 0,teleport=s.teleport_sequence or 0,deadline=s.time+30}
  return immediate({action='cast',spell=c.spell,target=c.target_id,status='Meta spell cast'})
 end
 if c.op=='logout' then return immediate({action='logout',status='Meta logout'}) end
 if c.op=='jump' then return immediate({action='jump',charge=c.charge,forward=c.forward or 0,strafe=c.strafe or 0,walk_jump=c.walk_jump,heading=c.heading,current_heading=c.current_heading~=false,status='Meta jump'}) end
 if c.op=='face' then meta.turn={heading=c.heading,deadline=s.time+10};return immediate({action='face',heading=c.heading,status='Facing route heading'}) end
 if c.op=='fellowship' then
  local f=s.fellowship or {};local op=c.operation;local id=c.target_id
  if op=='recruit' or op=='dismiss' or op=='leader' then
   for _,m in ipairs(f.members or {}) do if (id and m.id==id) or (not id and string.lower(m.name or '')==string.lower(c.name or '')) then id=m.id;break end end
   if not id and op=='recruit' then for _,o in ipairs(s.route_objects or {}) do if o.object_class==24 and string.lower(o.name or '')==string.lower(c.name or '') then id=o.id;break end end end
   if not id then return failed('Fellowship target is unavailable: '..(c.name or '')) end
  end
  local q={kind='fellowship',operation=op,name=c.name,target=id,deadline=s.time+30,fellowship_name=f.valid and f.name or nil}
  if fellowship_done(s,q) then return immediate(result(nil,'Fellowship already updated')) end
  if op=='create' and f.valid then return failed('Already in another fellowship') end
  if op~='create' and not f.valid then return failed('Not in a fellowship') end
  meta.pending=q
  return immediate({action='fellowship',operation=op,name=c.name,target=id,status='Fellowship: '..op})
 end
 local function find(list,name,id,partial)
  name=string.lower(name or '')
  for _,o in ipairs(list or {}) do if (id and o.id==id) or (not id and string.lower(o.name or '')==name) then return o end end
  if partial and c.prefix and not id then for _,o in ipairs(list or {}) do if string.lower(o.name or ''):contains(name) then return o end end end
 end
 if c.op=='use_selected' then c.item_id=meta.selected or s.selected end
 if c.op=='select' and c.item_id==s.player then meta.selected=s.player;return immediate({action='select',target=s.player,status='Selected self'}) end
 local item,action
 for _,partial in ipairs({false,true}) do
  if c.scope~='world' then item=find(s.inventory,c.item,c.item_id,partial);if item then action='use_item';break end end
  if c.scope~='inventory' then item=find(s.route_objects,c.item,c.item_id,partial) or find(s.targets,c.item,c.item_id,partial) or find(s.corpses,c.item,c.item_id,partial);if item then action='use_world';break end end
 end
 if not item then return failed('Meta item or object is unavailable: '..(c.item or tostring(c.item_id))) end
 if c.op=='select' then meta.selected=item.id;return immediate({action='select',target=item.id,status='Selected '..item.name}) end
 local target
 if c.target or c.target_id then
  if action~='use_item' then return failed('Meta use-on/give requires an owned item') end
  if c.target_id==s.player then target={id=s.player} end
  for _,partial in ipairs({false,true}) do target=target or find(s.route_objects,c.target,c.target_id,partial) or find(s.inventory,c.target,c.target_id,partial) or find(s.targets,c.target,c.target_id,partial);if target then break end end
  if not target then return failed('Meta target is unavailable: '..(c.target or tostring(c.target_id))) end
  action=c.op=='give' and 'give' or 'apply_item'
 end
 table.remove(meta.queue,1);meta.pending={kind=action=='give' and 'give' or 'use',id=item.id,target=target and target.id,
  count=item.count or 1,failure_serial=s.inventory_failure_serial or 0,started=s.time,serial=s.action_serial or 0,teleport=s.teleport_sequence or 0,deadline=s.time+45}
 return {action=action,item=item.id,target=target and target.id,amount=1,status='Meta: '..c.op..' '..item.name}

end
local function corpse_owner_name(name)
 name=string.lower(name or '')
 while name:sub(1,1)=='+' do name=name:sub(2) end
 return name
end
local rule_match=lootmatch
-- VT hv classifies ordinary loot first, then dy accepts otherwise unwanted
-- magical loot with workmanship and ManaTankMinimumMana (default 1000).
-- It queues only items it actually looted, never arbitrary carried equipment.
local function mana_source_candidate(v,p,option)
 local ints=v.int_properties or {}
 if not v.identified or v.equipped or v.retained or salvage_protected(v)
  or band(v.type,0x80200)~=0 or band(v.icon_effects or ints['218103824'],1)==0
  or (v.workmanship or ints['105'] or 0)<=0 or (v.max_mana or 0)<=0
  or (v.mana or 0)<math.max(1,option('mana_tank_minimum',1000)) then return false end
 if contains(p.weapon_items,v.id) or contains(p.weapons,v.wcid) or contains(p.consumables,v.wcid)
  or contains(p.item_buff_targets,v.id) or v.id==p.melee_weapon or v.id==p.melee_secondary_equip then return false end
 for _,b in ipairs(p.buff_items or {}) do if b.name==v.name then return false end end
 for _,a in ipairs(p.assist_items or {}) do if a.name==v.name then return false end end
 return true
end
local function empty_mana_stone(v,p)
 return v.identified and v.usable and not v.equipped and v.mana_supply_available~=false and v.mana_empty and band(v.type,0x80000)~=0
end
local function reserved_mana_source(v,q,p,option)
 -- Pickup can recreate the object and invalidate its corpse appraisal. Keep
 -- that exact pickup reserved while reassessing it; unknown is not unsuitable.
 return q and q.wcid==v.wcid and q.count==(v.count or 1) and v.resource_available~=false
  and (not v.identified or mana_source_candidate(v,p,option))
end
local function mana_source_demand(inventory,p,option)
 if not option('fill_mana_stones',false) or mana_fill_pending then return 0 end
 local empty,reserved=0,0
 for _,v in ipairs(inventory) do
  if empty_mana_stone(v,p) then empty=empty+1 end
  local q=mana_sources[v.id]
  if reserved_mana_source(v,q,p,option) then reserved=reserved+1 end
 end
 return math.max(0,empty-reserved)
end
local function equipment_mana(s,p,inventory,option,assess)
 if not activity_ready('item_mana') then return end
 if mana_fill_pending then
  local q=mana_fill_pending;local stone
  local receipt=action_receipt(s,'use',q.serial,q.started,q.stone,q.source)
  for _,v in ipairs(inventory) do if v.id==q.stone then stone=v;break end end
  if stone and stone.identified and (stone.mana or 0)>0 then mana_fill_pending=nil
  elseif (receipt and (receipt.error or 0)~=0) or s.time>=q.deadline then
   mana_fill_pending=nil;until_time.mana_fill=s.time+30
  else return end -- Keep combat/recovery available while appraisal catches up.
 end
 -- Inventory mana is an item resource, not the player's mana vital.
 if mana_refill_pending then
  local q=mana_refill_pending
  local receipt=action_receipt(s,'use',q.serial,q.started,q.item)
  local improved=false
  for _,v in ipairs(inventory) do local before=mana_refill_pending.before[v.id];if before and (v.mana or 0)>before then improved=true;break end end
  if improved then mana_refill_pending=nil;item_attempts.mana_refill=0
  elseif (receipt and (receipt.error or 0)~=0) or s.time>=q.expires then
   mana_refill_pending=nil;until_time.mana_stone=s.time+30
  else return end -- The native use gate owns animation waits; appraisal must not block buffs/navigation.
 end
 if option('item_mana',true) then
  local low=false;for _,v in ipairs(inventory) do if v.equipped and (v.max_mana or 0)>0 and pct(v.mana or 0,v.max_mana)<math.min(99,option('item_mana_threshold',20)) then low=true end end
  if low and s.time>=(until_time.mana_stone or 0) then
   local supply,priority
   for _,v in ipairs(inventory) do
    local rank=p.assist_items==nil and ((v.workmanship or 0)>0 and 0 or 1) or nil
    for _,a in ipairs(p.assist_items or {}) do if (a.type==6 or a.type==8) and a.name==v.name then rank=a.type==8 and 0 or 1 end end
    local cooldown=tostring(math.floor((v.int_properties or {})['280'] or 0))
    if rank and v.identified and v.usable and not v.equipped and v.mana_supply_available~=false and (v.count or 1)>0
     and ((s.cooldowns or {})[cooldown] or 0)<=0 and band(v.type,0x80000)~=0 and (v.mana or 0)>0
     and (not v.workmanship or v.workmanship==0 or (v.mana or 0)>=option('mana_tank_minimum',0)) and allowed(p.consumables,v.wcid)
     and (not supply or rank<priority) then supply=v;priority=rank end
   end
   if supply then
    if (item_attempts.mana_refill or 0)>=3 then return failed('Equipment mana refill failed repeatedly; check charges and item requirements') end
    item_attempts.mana_refill=(item_attempts.mana_refill or 0)+1
    local before={};for _,gear in ipairs(inventory) do if gear.equipped then before[gear.id]=gear.mana or 0 end end
    mana_refill_pending={before=before,item=supply.id,serial=s.action_serial or 0,started=s.time,expires=s.time+10}
    until_time.mana_stone=s.time+30;return {action='use_item',item=supply.id,status='Recharging equipment: '..supply.name}
   end
  end
 end
 if option('fill_mana_stones',false) and s.time>=(until_time.mana_fill or 0) then
  local stone,source,unassessed;local present={}
  for _,v in ipairs(inventory) do
   if empty_mana_stone(v,p) then stone=stone or v end
   local q=mana_sources[v.id]
   if q then
    present[v.id]=true
    if reserved_mana_source(v,q,p,option) then
     if v.identified then source=source or v else unassessed=unassessed or v end
    else mana_sources[v.id]=nil end -- A player changed/protected the item after pickup.
   end
  end
  for id in pairs(mana_sources) do if not present[id] then mana_sources[id]=nil end end
  if stone and source and stone.id~=source.id then
   mana_fill_pending={stone=stone.id,source=source.id,serial=s.action_serial or 0,started=s.time,deadline=s.time+15}
   return {action='apply_item',item=stone.id,target=source.id,status='Filling '..stone.name..' from '..source.name..' (consumes item)'}
  end
  if stone and unassessed and s.time>=(until_time['id'..unassessed.id] or 0) then return assess(unassessed) end
 end
end

local function pet_eligible(s,v)
 local q=v.int_properties or {};local skills=s.skills or {}
 if not v.identified or not v.usable or not pet_device(v) then return false end
 if ((v.max_structure or 0)>0 or q['92']~=nil) and (v.structure or 0)<=0 then return false end
 if (q['369'] or 0)>(s.level or 0) or ((q['362'] or 0)~=0 and q['362']~=(s.char_ints or {})['362']) then return false end
 for _,requirement in ipairs({{'366',2},{'368',3}}) do
  local id=q[requirement[1]] or 0
  if id~=0 then local skill=skills[tostring(math.floor(id))] or {}
   if (skill.training or 0)<requirement[2] or (skill.current or 0)<(q['367'] or 0) then return false end
  end
 end
 return ((s.cooldowns or {})[tostring(math.floor(q['280'] or 0))] or 0)<=0
end
local function refill_pet(s,p,inventory,option,eligible)
 if not activity_ready('pets') then return end
 if pet_refill_pending then
  local q=pet_refill_pending;local device
  local receipt=action_receipt(s,'use',q.serial,q.started,q.source,q.id)
  for _,v in ipairs(inventory) do if v.id==q.id then device=v;break end end
  if device and (device.structure or 0)>q.before then pet_refill_pending=nil
  elseif (receipt and (receipt.error or 0)~=0) or s.time>=q.deadline then
   pet_refill_pending=nil;until_time.pet_refill=s.time+30
  else return end
 end
 if not option('refill_summons',false) or s.time<(until_time.pet_refill or 0) then return end
 local spirit,device
 for _,v in ipairs(inventory) do
  -- ACE's Encapsulated Spirit recipe explicitly uses WCID 49485.
  if v.wcid==49485 and v.usable and not v.equipped and v.resource_available~=false then spirit=spirit or v end
  local props=v.int_properties or {}
  if eligible(v) and v.identified and pet_device(v) and (v.max_structure or 0)>0
   and (v.structure or 0)<v.max_structure and (v.structure or 0)<=option('summon_refill_charges',5) then device=device or v end
 end
 if spirit and device then
  pet_refill_pending={id=device.id,source=spirit.id,started=s.time,before=device.structure or 0,serial=s.action_serial or 0,deadline=s.time+15}
  return {action='apply_item',item=spirit.id,target=device.id,status='Refilling summon: '..device.name}
 end
end

local function vendor_work(s,p,inventory,option)
 -- Restock exact saved stock identities at an open vendor; never guess by GUID
 -- across logins. Wait for server inventory replication before buying again.
 -- Recovery may run while waiting. Its untagged UseDone/errors cannot confirm
 -- or reject a vendor transaction; quantities and deadlines are authoritative.
 local stock_counts
 local function stock_count(wcid,name)
  if not stock_counts then
   stock_counts={};for _,v in ipairs(inventory) do if v.wcid and v.name then
    local names=stock_counts[v.wcid] or {};stock_counts[v.wcid]=names;names[v.name]=(names[v.name] or 0)+(v.count or 1)
   end end
  end
  return stock_counts[wcid] and stock_counts[wcid][name] or 0
 end
 if activity_ready('vendors') then
 if note_pending then
  local q=note_pending
  if not option('vendor_restock',false) or s.vendor~=q.vendor or not s.vendor_uses_pyreals then
   note_pending=nil;return result(nil,'Trade note redemption cancelled: vendor or restocking changed')
  end
  if s.time>=q.deadline then
   return failed('Trade note redemption not confirmed: check inventory space and vendor messages')
  end
  local source
  for _,v in ipairs(inventory) do if v.id==q.id then source=v;break end end
  if not source then for _,v in ipairs(s.vendor_trade_notes or {}) do if v.id==q.id then source=v;break end end end
  if q.phase=='split' then
   local candidate
   for _,v in ipairs(s.vendor_trade_notes or {}) do
    if not q.known[v.id] and v.wcid==q.wcid and v.name==q.name and v.count==q.count and v.unit_value==q.unit_value then
     if candidate then return failed('Trade note split is ambiguous; no notes sold') end
     candidate=v
    end
   end
   if source and source.count==q.before-q.count and candidate then
    q.phase='sell';q.id=candidate.id;q.deadline=s.time+15;q.serial=s.action_serial or 0;q.known=nil
    q.money=s.pyreals or 0
    return {action='sell_note',vendor=q.vendor,item=q.id,count=q.count,status='Redeeming '..q.count..' '..q.name..' for supplies'}
   end
   return result(nil,'Waiting for trade note split')
  end
  -- Require both sides of the transaction. Replication may deliver the money
  -- before the sold object disappears, or vice versa.
  if not source and (s.pyreals or 0)>=q.money+q.count*q.unit_value then note_pending=nil
  else return result(nil,'Waiting for trade note proceeds') end
 end
 if purchase_pending then
  local q=purchase_pending
  if not option('vendor_restock',false) or (s.vendor or 0)~=q.vendor then
   purchase_pending=nil;return result(nil,'Purchase wait cancelled: vendor or restocking changed')
  end
  if stock_count(q.wcid,q.name)>=q.expected then purchase_pending=nil
  elseif s.time>=q.deadline then
   return failed('Purchase not confirmed: check currency, pack space and vendor stock')
  else return result(nil,'Waiting for purchased '..q.name) end
 end
 if option('vendor_restock',false) and (s.vendor or 0)~=0 then
  local stock={}
  for _,v in ipairs(s.vendor_stock or {}) do if v.limit>0 then local names=stock[v.wcid] or {};stock[v.wcid]=names;if not names[v.name] or v.limit>names[v.name].limit then names[v.name]=v end end end
  for _,r in ipairs(p.vendor_rules or {}) do
   if r.enabled~=false and r.server==s.world_name and r.vendor_wcid==s.vendor_wcid and r.vendor_name==s.vendor_name then
    local have=stock_count(r.item_wcid,r.item_name)
    if have<r.quantity then
     local v=stock[r.item_wcid] and stock[r.item_wcid][r.item_name]
     if v then
      local count=math.min(r.quantity-have,v.limit)
      local cost=v.unit_value and math.max(1,math.ceil(v.unit_value*(v.sell_rate or 1)*count-.1))
      if cost and s.vendor_uses_pyreals and (s.pyreals or 0)<cost then
       local deficit=cost-(s.pyreals or 0);local candidates,total={},0;local reserved={}
       for _,rule in ipairs(p.vendor_rules or {}) do
        if rule.enabled~=false and rule.server==s.world_name and rule.vendor_wcid==s.vendor_wcid and rule.vendor_name==s.vendor_name then reserved[rule.item_wcid]=true end
       end
       for _,note in ipairs(s.vendor_trade_notes or {}) do
        if not reserved[note.wcid] and note.unit_value>0 and note.count>0 then
         candidates[#candidates+1]=note;total=total+note.unit_value*note.count
        end
       end
       if total<deficit then return failed('Not enough pyreals or redeemable trade notes for '..r.item_name) end
       -- Prefer a denomination covering the shortfall with the least change.
       -- Otherwise redeem the largest available contribution and reassess.
       table.sort(candidates,function(a,b)
        local ac=math.min(a.count,math.ceil(deficit/a.unit_value))*a.unit_value
        local bc=math.min(b.count,math.ceil(deficit/b.unit_value))*b.unit_value
        if (ac>=deficit)~=(bc>=deficit) then return ac>=deficit end
        if ac~=bc then if ac>=deficit then return ac<bc else return ac>bc end end
        return a.id<b.id
       end)
       local note=candidates[1];local amount=math.min(note.count,math.ceil(deficit/note.unit_value))
       note_pending={phase=amount<note.count and 'split' or 'sell',id=note.id,wcid=note.wcid,name=note.name,
        unit_value=note.unit_value,count=amount,before=note.count,money=s.pyreals or 0,vendor=s.vendor,deadline=s.time+15,serial=s.action_serial or 0}
       if amount<note.count then
        note_pending.known={};for _,item in ipairs(inventory) do note_pending.known[item.id]=true end
        for _,item in ipairs(s.vendor_trade_notes or {}) do note_pending.known[item.id]=true end
        return {action='split_note',vendor=s.vendor,item=note.id,count=amount,status='Splitting '..amount..' '..note.name..' for supplies'}
       end
       return {action='sell_note',vendor=s.vendor,item=note.id,count=amount,status='Redeeming '..amount..' '..note.name..' for supplies'}
      end
      purchase_pending={wcid=r.item_wcid,name=r.item_name,expected=have+count,deadline=s.time+15,serial=s.action_serial or 0,vendor=s.vendor}
      return {action='buy',vendor=s.vendor,item=v.id,count=count,status='Restocking '..r.item_name..' ('..have..' / '..r.quantity..')'}
     end
     return failed('Saved restock item is not available: '..r.item_name)
    end
   end
  end
 end
 end -- vendor activity
end

-- VT bf/g5: tell-driven recruitment, FIFO reservations and member votes.
-- State is session-local. Never derive membership from chat or UseDone.
local fellow_manager
local fellow_last_serial=0
local function fellowship_work(s,p)
 local f=s.fellowship or {};local now=s.time
 if not p.auto_fellowship or not f.valid then
  for _,e in ipairs(s.fellowship_tells or {}) do fellow_last_serial=math.max(fellow_last_serial,e.serial) end
  fellow_manager=nil;return
 end
 if not activity_ready('fellowship') then return end
 local leader=f.leader==s.player
 if not fellow_manager or fellow_manager.name~=f.name then
  fellow_manager={name=f.name,leader=leader,serial=fellow_last_serial,queue={},banned={},rates={},cooldown={},votes={},next_vote=0,replies={},operations={},open=p.fellowship_open~=false,configured_open=p.fellowship_open~=false}
 end
 local m=fellow_manager;local members,byname={},{}
 for _,v in ipairs(f.members or {}) do members[v.id]=v;byname[string.lower(v.name or '')]=v end
 local function reply(name,text)
  if #m.replies<64 then m.replies[#m.replies+1]={action='say',recipient=name,text='[UCM Fellow Manager] '..text,channel=not name and 2048 or nil,status='Fellowship management'} end
 end
 if m.leader and not leader then
  for _,q in ipairs(m.queue) do reply(q.name,'Waiting list cancelled: leadership changed.') end
  m.queue={};m.votes={};m.banned={};m.operations={};m.pending=nil
 end
 m.leader=leader
 if m.configured_open~=(p.fellowship_open~=false) then m.configured_open=p.fellowship_open~=false;m.open=m.configured_open end
 if p.fellowship_votes==false then m.votes={} end
 local capacity=math.max(2,math.min(100,p.fellowship_capacity or 9))
 local count=0;for _ in pairs(members) do count=count+1 end
 local free=math.max(0,capacity-count)
 if free==0 and leader then m.holding=true end
 for i=#m.queue,1,-1 do
  local q=m.queue[i]
  if members[q.id] then table.remove(m.queue,i)
  elseif q.expires and now>=q.expires then reply(q.name,'Your reserved spot expired; send xp to join the list again.');table.remove(m.queue,i) end
 end
 local function remove(id) for i=#m.queue,1,-1 do if m.queue[i].id==id then table.remove(m.queue,i) end end end
 if m.pending then
  local q=m.pending
  if fellowship_done(s,q) then m.pending=nil
  elseif now>=q.deadline then
   if q.operation=='recruit' then remove(q.target);reply(q.name,'Recruitment was not confirmed. Check range and invitations, then send xp again.') end
   m.pending=nil;m.backoff=now+30
  end
 end
 for id,v in pairs(m.votes) do
  for voter in pairs(v.ballots) do if not members[voter] then v.ballots[voter]=nil end end
  if now>=v.expires then
   local yes,no=0,0;for _,choice in pairs(v.ballots) do if choice then yes=yes+1 else no=no+1 end end
   reply(nil,string.format('Vote %d %s (%d/%d).',id,yes>no and 'passed' or 'failed',yes,no))
   if leader and yes>no then
    if v.kind=='setopen' then m.open=v.value=='true'
    elseif members[v.target] and v.target~=s.player then
     if v.kind=='ban' then m.banned[string.lower(members[v.target].name)]=true;remove(v.target) end
     m.operations[#m.operations+1]={operation=v.kind=='giveleader' and 'leader' or 'dismiss',target=v.target}
    end
   end
   m.votes[id]=nil
  end
 end
 for id,r in pairs(m.rates) do if now-r.time>=180 then m.rates[id]=nil end end
 for id,t in pairs(m.cooldown) do if now>=t then m.cooldown[id]=nil end end
 for _,e in ipairs(s.fellowship_tells or {}) do if e.serial>m.serial then
  m.serial=e.serial;fellow_last_serial=e.serial
  local name=e.name or '';local lower=string.lower(name);local text=string.lower(e.text or '');local member=members[e.id]
  local words={};local first
  for i=1,#text+1 do local ch=text:sub(i,i);if ch=='' or ch==' ' or ch=='\t' or ch=='\r' or ch=='\n' then if first then words[#words+1]=text:sub(first,i-1);first=nil end elseif not first then first=i end end
  text=table.concat(words,' ')
  local local_owner=e['local']==true and e.id==s.player and leader
  local recognized=text=='xp' or text=='remove' or text=='line' or text=='list' or text=='status' or text=='leader' or text=='location' or text=='help' or text:sub(1,5)=='help ' or text:sub(1,10)=='startvote ' or text:sub(1,5)=='vote ' or local_owner
  local rate=m.rates[e.id] or {time=now,count=0}
  if recognized then
   if not m.rates[e.id] then local n=0;for _ in pairs(m.rates) do n=n+1 end;if n>=256 then recognized=false else m.rates[e.id]=rate end end
   rate.count=rate.count+1
  end
  if recognized and now-(e.time or now)<180 and (rate.count<=8 or local_owner) then
   local q,index;for i,v in ipairs(m.queue) do if v.id==e.id then q=v;index=i;break end end
   if text=='clear' and local_owner then m.queue={};m.votes={};m.operations={};m.pending=nil
   elseif text:sub(1,6)=='unban ' and local_owner then m.banned[text:sub(7)]=nil
   elseif text=='xp' then
    if member then reply(name,'You are already in the fellowship.')
    elseif m.banned[lower] then reply(name,'You are banned from this managed fellowship.')
    elseif not leader and (not f.open or free==0) then reply(name,'Ask the fellowship leader: '..((members[f.leader] or {}).name or 'unknown'))
    elseif q then if q.expires then q.ready=true end;reply(name,'Waiting list position '..index..' of '..#m.queue..'. Please wait nearby.')
    elseif #m.queue>=64 then reply(name,'Waiting list is full. Try again later.')
    else
     m.queue[#m.queue+1]={id=e.id,name=name,ready=not m.holding,expires=not m.holding and now+300 or nil}
     reply(name,'Added to waiting list at position '..#m.queue..'. '..(m.holding and 'I will tell you when a spot opens.' or 'Please stand within 10 metres.'))
    end
   elseif text=='remove' then remove(e.id);reply(name,'Removed from the waiting list.')
   elseif text=='line' or text=='list' or text=='status' then reply(name,q and ('Waiting list position '..index..' of '..#m.queue..'.') or ('Fellowship: '..count..' members; '..#m.queue..' waiting.'))
   elseif text=='leader' then reply(name,'Leader: '..((members[f.leader] or {}).name or 'unknown')..'. Fellowship is '..(f.open and 'open.' or 'closed.'))
   elseif text=='location' then reply(name,member and string.format('Current landcell: %08X',(s.position or {}).cell or 0) or 'Location is available to fellowship members only.')
   elseif text:sub(1,4)=='help' then reply(name,'Commands: xp, line/list/status, remove, leader, location; startvote kick/ban/giveleader NAME or setopen true/false; vote ID yes/no. Votes last 2 minutes.')
   elseif text:sub(1,10)=='startvote ' and leader and member and not m.banned[lower] and p.fellowship_votes~=false then
    local kind,value=words[2],table.concat(words,' ',3);local target=byname[value or ''];local n=0;local duplicate=false
    for _,v in pairs(m.votes) do n=n+1;if v.kind==kind and v.value==value then duplicate=true end end
    if (m.cooldown[e.id] or 0)>now then reply(name,'Wait four minutes between starting votes.')
    elseif n>=8 or duplicate then reply(name,'Vote already exists or the vote limit was reached.')
    elseif (kind=='setopen' and (value=='true' or value=='false')) or ((kind=='kick' or kind=='ban' or kind=='giveleader') and target and target.id~=s.player) then
     m.next_vote=m.next_vote+1;m.votes[m.next_vote]={kind=kind,value=value,target=target and target.id,expires=now+120,ballots={[e.id]=true}};m.cooldown[e.id]=now+240
     reply(nil,string.format('%s started vote %d: %s %s. Tell me vote %d yes/no within 2 minutes.',name,m.next_vote,kind,value,m.next_vote))
    else reply(name,'Invalid vote type or target; choose another current member.') end
   elseif text:sub(1,5)=='vote ' and member and leader and p.fellowship_votes~=false then
    local id,choice=tonumber(words[2]),#words==3 and words[3];local v=id and m.votes[id]
    if v and choice=='abort' and local_owner then m.votes[id]=nil;reply(nil,'Vote '..id..' cancelled by leader.')
    elseif v and (choice=='yes' or choice=='no') and not m.banned[lower] then
     v.ballots[e.id]=choice=='yes';local yes,no=0,0;for _,b in pairs(v.ballots) do if b then yes=yes+1 else no=no+1 end end;reply(nil,string.format('Vote %d: %d yes / %d no.',id,yes,no))
    else reply(name,'Invalid vote. Use vote ID yes/no.') end
   end
  end
 end end
 -- Reservations are FIFO, occupy real free slots, and require a new xp tell.
 if leader and m.holding then
  for i,q in ipairs(m.queue) do
   if i>free and q.expires then q.expires=nil;q.ready=false;reply(q.name,'Your spot was taken; returned to waiting list position '..i..'.')
   elseif i<=free and not q.expires then q.expires=now+300;q.ready=false;reply(q.name,'A spot is open. Come nearby and tell me xp within 5 minutes.') end
  end
 end
 if #m.queue==0 then m.holding=false end
 local status=string.format('%d members; %d waiting; ',count,#m.queue)
 local vote_text={};for id,v in pairs(m.votes) do vote_text[#vote_text+1]=string.format('Vote %d: %s %s',id,v.kind,v.value) end;table.sort(vote_text)
 status=status..(#vote_text>0 and table.concat(vote_text,'; ') or 'no active votes')
 local intent
 if #m.replies>0 then intent=table.remove(m.replies,1)
 elseif not m.pending and now>=(m.backoff or 0) then
  local op=table.remove(m.operations,1)
  local desired=m.open and not m.holding
  if not op and leader and not f.locked and f.open~=desired then op={operation=desired and 'open' or 'close'} end
  if not op and (leader or f.open) and free>0 then
   for i,q in ipairs(m.queue) do if i<=free and q.ready then
    for _,v in ipairs(s.route_objects or {}) do if v.object_class==24 and v.id==q.id and (v.distance or math.huge)<=10 then op={operation='recruit',target=q.id,name=q.name};break end end
    if op then break end
   end end
  end
  if op then m.pending={operation=op.operation,target=op.target,name=op.name,deadline=now+30};op.action='fellowship';op.status='Fellowship: '..op.operation;intent=op end
 end
 if intent then intent.activity='fellowship';intent.fellowship_status=status;return intent end
 -- Metadata accompanies the next ordinary activity without claiming its turn.
 m.status=status
end

local function craft_work(s,p,phase)
 if not p.auto_craft or not activity_ready('crafting') then return end
 local c=s.crafting or {}
 if c.pending then return {action='craft',activity='crafting',status='Crafting '..(c.name or 'supplies')} end
 for _,v in ipairs(c.candidates or {}) do if v.phase==phase then return {action='craft',activity='crafting',recipe=v.recipe,status='Preparing '..v.name} end end
end

local function tick(s,p)
 if not state then state=p.initial_state or 'Default';entered=s.time end
 local hp,sp,mp=pct(s.health,s.max_health),pct(s.stamina or 0,s.max_stamina or 1),pct(s.mana,s.max_mana)
 local current;for _,v in ipairs(p.states or {}) do if v.name==state then current=v end end
 if p.usd_keys then state='Default';current={} end -- Imported character settings replace native state overrides.
 if not p.states and state=='Default' then current={} end
 if not current then
  local invalid=state;state='Default';current={};entered=s.time
  if activity_ready('meta') then return failed('Unknown activity state: '..invalid..'; using base settings') end
 end
 local function option(k,default) if k=='navigation' and navigation_paused then return false end;if current[k]~=nil then return current[k] end;if p[k]~=nil then return p[k] end;return default end
 -- ACE creates a corpse only after the death animation. Do not start retracing
 -- a combat excursion in that gap. Only the host's confirmed combat death can
 -- arm this wait; disappearing, ignored or out-of-sight targets cannot.
 local death=s.combat_death
 if death and death.time~=combat_death_seen then
  combat_death_seen=death.time;corpse_spawn_wait=death
 end
 if corpse_spawn_wait then
  local elapsed=s.time-corpse_spawn_wait.time
  if elapsed<0 or elapsed>=8 or corpse_spawn_wait.teleport_sequence~=s.teleport_sequence
   or not option('looting',false) or not s.position or distance(s.position,corpse_spawn_wait)>option('loot_range',15) then
   corpse_spawn_wait=nil
  else
   -- Any newly arrived visible corpse within loot range can be considered by
   -- the normal ownership/rule checks now. Old corpses do not end the wait.
   for _,c in ipairs(s.corpses or {}) do
    if c.observed_age and c.observed_age<=elapsed+.05 and c.distance<=option('loot_range',15) and c.line_of_sight~=false then
     corpse_spawn_wait=nil;break
    end
   end
  end
 end
 -- Failure counts come from server combat feedback, not from elapsed attack time
 -- or evades. Match VT's strict count > limit and let the route run past failures.
 local present={};for _,t in ipairs(s.targets or {}) do present[t.id]=true end
 -- VT es tracks remaining HP and the last single-target spell's actual damage.
 -- Fresh health samples supersede estimates; damage before that sample must
 -- not be subtracted twice. Unknown HP never implies a nearly dead target.
 for id in pairs(spell_health) do if not present[id] then spell_health[id]=nil end end
 for _,t in ipairs(s.targets or {}) do
  local h=spell_health[t.id] or {};spell_health[t.id]=h
  local maximum=t.max_health
  if not maximum or maximum<=0 then maximum=((p.monster_catalog or {})[t.name] or {}).max_health end
  local observed=t.health or (t.health_fraction and maximum and maximum>0 and math.floor(t.health_fraction*maximum+.5))
  local stamp=t.health_observed_at
  if observed and ((stamp and stamp>(h.observed_at or -1)) or (not stamp and observed~=h.observed)) then
   h.hp=observed;h.observed=observed;h.observed_at=stamp
  elseif h.hp==nil and maximum and maximum>0 then h.hp=maximum end
 end
 for id in pairs(monster_failures) do if not present[id] then monster_failures[id]=nil end end
 for id,expiry in pairs(monster_blacklist) do if not present[id] or s.time>=expiry then monster_blacklist[id]=nil end end
 for _,e in ipairs(s.combat_events or {}) do if e.serial>combat_serial then
  combat_serial=e.serial
  if present[e.target] then
   if e.unhittable then monster_blacklist[e.target]=s.time+option('monster_blacklist_seconds',120);monster_failures[e.target]=nil
   elseif e.hit then
    monster_failures[e.target]=nil;ghost_attempts[e.target]=nil;ghost_hp[e.target]={time=s.time}
    if e.spell and (e.damage or 0)>0 then
     local h=spell_health[e.target];h.damage=e.damage
     if h.hp and (not e.time or not h.observed_at or e.time>h.observed_at) then h.hp=math.max(0,h.hp-e.damage) end
    end
   else
    local failures=(monster_failures[e.target] or 0)+1
    if failures>option('monster_attempts',4) then monster_blacklist[e.target]=s.time+option('monster_blacklist_seconds',120);monster_failures[e.target]=nil
    else monster_failures[e.target]=failures end
   end
  end
 end end
 -- Keep server-owned objects intact; suppress stale combat targets locally.
 for id in pairs(ghost_attempts) do if not present[id] then ghost_attempts[id]=nil end end
 for id in pairs(ghost_hp) do if not present[id] then ghost_hp[id]=nil end end
 for _,t in ipairs(s.targets or {}) do
  local h=ghost_hp[t.id]
  if not h or (t.health_fraction and t.health_fraction~=h.health) then h={time=s.time,health=t.health_fraction};ghost_hp[t.id]=h end
  if (option('delete_ghost_monsters',false) and (ghost_attempts[t.id] or 0)>option('ghost_spell_attempts',20))
   or (option('delete_ghost_hp',false) and t.id==locked_target and s.time-h.time>=option('ghost_hp_seconds',600)) then
   monster_blacklist[t.id]=s.time+option('monster_blacklist_seconds',120);ghost_attempts[t.id]=nil;h.time=s.time
  end
 end
 local function ring_enabled(rule) if rule and rule.ring~=nil then return rule.ring end;return option('ring',false) end
 local target,target_rule,priority=nil,nil,-1e9
 local function nearer(a,b)
  if not b then return true end
  if option('target_lock',true) then if a.id==locked_target then return true elseif b.id==locked_target then return false end end
  local method=option('target_select',1);local range=option('target_angle_range',5)
  local angles=method==2 or (method==3 and a.distance<range and b.distance<range)
  if angles and (a.angle or 180)~=(b.angle or 180) then return (a.angle or 180)<(b.angle or 180) end
  if a.distance~=b.distance then return a.distance<b.distance end
  return (a.angle or 180)<(b.angle or 180)
 end
 -- Acquisition is independent of the distance at which a weapon can fire.
 -- Imported VT ApproachDistance=0 retains its stationary attack-radius mode.
 local acquisition=option('approach_range',60)
 if acquisition<=0 then acquisition=option('radius',30) end
 local ring_count=0;local target_candidates={};local nearby_monsters=false
 for _,t in ipairs(s.targets or {}) do
  if t.distance<=acquisition and t.line_of_sight~=false and (t.health_fraction or 1)>0 then nearby_monsters=true;break end
 end
 for _,t in ipairs(s.targets or {}) do if not monster_blacklist[t.id] and t.line_of_sight~=false and t.distance>=option('minimum_range',0) and t.distance<=acquisition and not contains(p.ignore_monsters,t.name) then
  local rule=monster_rule(t,p)
  local attacks=monster_attacks(rule,p)
  if attacks and ring_enabled(rule) and t.distance<option('ring_range',5) then ring_count=ring_count+1 end
  local rank=rule.priority or 0
  if attacks then target_candidates[#target_candidates+1]={target=t,rule=rule,rank=rank} end
  if attacks and (rank>priority or (rank==priority and nearer(t,target))) then target=t;target_rule=rule;priority=rank end
 end end
 if not target and s.nearest~=0 and s.targets==nil then
  local candidate={id=s.nearest,name='',distance=s.distance or 0,resists={}}
  local rule=monster_rule(candidate,p)
  if monster_attacks(rule,p) then target=candidate;target_rule=rule end
 end
 targeting_status=string.format('Search: %.0f m. %s',acquisition,target and string.format('%s: %.1f m.',target.name or 'Target',target.distance) or 'No visible eligible monsters.')
 if not option('approach',true) then targeting_status=targeting_status..' Pursuit is off; only attacks within range.' end
 if option('nav_priority',false) and option('navigation',false) then targeting_status=targeting_status..' Route has priority over combat.' end
 for _,r in ipairs(not forced_buff and current.transitions or {}) do
  local match=(r.when=='health_below' and hp<r.value) or (r.when=='mana_below' and mp<r.value) or (r.when=='stamina_below' and sp<r.value)
   or (r.when=='elapsed' and s.time-entered>=r.value) or (r.when=='no_targets' and not target)
   or (r.when=='target_available' and target~=nil) or (r.when=='route_complete' and point>#(p.route or {}))
  if match then state=r.next;entered=s.time;return result(nil,'State: '..state) end
 end
 -- The host exposes only UCM's cancellable corpse approach here, never an
 -- inventory transfer or a cast. Reuse normal target filtering (LOS, rules,
 -- range and blacklists) before interrupting it for combat.
 if s.loot_approaching and not s.jumping and target and option('combat','off')~='off'
  and not option('loot_priority',false) and activity_ready('combat') then
  if corpse_pending then
   local key='corpse'..corpse_pending.id;item_attempts[key]=math.max(0,(item_attempts[key] or 1)-1);corpse_pending=nil
  end
  return result('pause_loot_approach','Pausing corpse approach for combat')
 end
 if s.crafting and s.crafting.pending and (s.busy or (s.confirmations or {})[1]) then
  local craft=craft_work(s,p,'critical');if craft then return craft end
 end
 if s.busy or s.ready==false or s.jumping then return result(nil,s.jumping and 'Following jump arc' or 'Waiting for game action') end
 if locked_target and (not target or option('combat','off')=='off') then
  locked_target=nil;return {action='cancel_attack',status='No unobstructed combat target; continuing other activities',continue_work=true}
 end
 local cast_result=last_cast and cast_receipt(s,last_cast)
 if cast_result then
  local confirmed=(cast_result.error or 0)==0 and cast_result.confirmed==true
  if confirmed and forced_buff and last_cast.force_cycle==forced_buff then
   forced_buff.done[tostring(last_cast.target)..':'..last_cast.category]=last_cast.power;buff_plan=nil
  end
  if not last_cast.request then
   -- The server finished this attempt, so the twelve-second missing-reply
   -- watchdog no longer applies. Fizzles retry promptly but retain the bounded
   -- attempt count; they must not mark a forced family complete.
   local key=tostring(last_cast.target)..':'..last_cast.category;until_time[key]=nil
   if confirmed then
    retries[key]=nil
    -- Result chat and the enchantment update may be in different packets.
    -- Allow the authoritative timer to arrive instead of immediately recasting.
    if last_cast.buff then until_time['buffack'..last_cast.target..':'..last_cast.id]=s.time+2 end
   end
  end
  if cast_result.error==0x400 then
   if last_cast.request and other_job and other_job.id==last_cast.request then
    other_job.unavailable[last_cast.id]=s.time+120
   else
    unavailable_spells[last_cast.id]=s.time+120
    catalog_snapshot=nil;buff_plan=nil
    local key=tostring(last_cast.target)..':'..last_cast.category;retries[key]=nil;until_time[key]=nil
   end
  end
  last_cast=nil
 end
 local route_wait
 if route_pending and route_pending.kind~='pause' and not forced_buff then
   activity='navigation'
   local route=p.route or {};local v=route[point]
   if not v then return failed('Route changed during an action') end
   local kind=route_kind(v)
   if kind=='vendor' then
    -- VT OpenVendor tests VendorId, not the unrelated UseDone stream. ID 0
    -- is Close Vendor, and closing is confirmed by the same observed state.
    if (s.vendor or 0)==route_pending.vendor then route_pending=nil;point=point+route_direction
    elseif s.time-route_pending.time>30 then return pause_navigation('vendor did not open or close')
    else route_wait='Waiting for vendor' end
   elseif kind=='jump' then
    if s.time-route_pending.time>2 and not s.jumping then route_pending=nil;point=point+route_direction end
   elseif kind=='portal' or kind=='recall' then
    local nextpoint=route[point+route_direction]
    if s.teleport_sequence~=route_pending.teleport and not s.portal_space and (v.legacy or (nextpoint and distance(s.position,nextpoint)<30)) then local step=route_pending.confirmed and 2 or 1;route_pending=nil;point=point+route_direction*step
    elseif s.time-route_pending.time>45 then return pause_navigation('portal did not reach the recorded destination') end
   else
    local receipt
    if s.action_events then
     for _,e in ipairs(s.action_events) do
      if (e.action_serial or 0)>(route_pending.serial or 0) and e.type=='use' and e.source==route_pending.item
       and (not e.requested_at or e.requested_at>=route_pending.time) then receipt=e end
     end
    elseif s.action_serial~=route_pending.serial then receipt={error=s.action_error} end
    if receipt then
     if (receipt.error or 0)~=0 then return pause_navigation('route interaction failed') end
     local step=route_pending.confirmed and 2 or 1;route_pending=nil;point=point+route_direction*step
    elseif s.time-route_pending.time>30 then return pause_navigation('route action timed out') end
   end
   -- Portal arrival is confirmed by teleport sequence/position, never by a
   -- generic UseDone. While the host is free, VT's recharge/combat rules still
   -- run ahead of navigation. Do not let a missing portal monopolize them.
   -- Ordinary use waypoints also yield after the native action lock clears.
   -- Recovery/combat completions cannot acknowledge the recorded route item.
  end
 local by_id,spells,spell_groups=index_spells(s,option('skill_margin',30));local trained={}
 for _,id in ipairs(s.trained_skills or {}) do trained[id]=true end
 for _,id in ipairs(s.usable_skills or {}) do trained[id]=true end
 local inventory=s.inventory or {}

 local usable_names={};for _,v in ipairs(inventory) do if v.usable and v.name then usable_names[v.name]=v end end
 local weapon_allowed,pet_allowed=equipment_selection(p,inventory)
 for _,v in ipairs(inventory) do
  if not v.equipped then item_attempts['equip'..v.id..':remove']=nil
  elseif band(v.equipped_slot,0x200000)~=0 then item_attempts['equip'..v.id..':offhand']=nil
  else item_attempts['equip'..v.id]=nil end
 end
 local function equip(v,offhand,unequip)
  local key='equip'..v.id..(offhand and ':offhand' or unequip and ':remove' or '')
  if s.time<(until_time[key] or 0) then return result(nil,'Waiting for equipment') end
  if (item_attempts[key] or 0)>=3 then return failed('Could not equip '..v.name) end
  until_time[key]=s.time+5;item_attempts[key]=(item_attempts[key] or 0)+1
  return {action='equip',item=v.id,offhand=offhand,unequip=unequip,status=(unequip and 'Removing ' or 'Equipping ')..v.name}
 end
 local function assess(v)
  local key='id'..v.id
  if (item_attempts[key] or 0)>=3 then return failed('Unable to assess '..v.name..'; check appraisal skills') end
  if s.time<(until_time[key] or 0) then return result(nil,'Waiting for appraisal: '..v.name) end
  item_attempts[key]=(item_attempts[key] or 0)+1;until_time[key]=s.time+30
  return {action='identify',item=v.id,status='Assessing '..v.name}
 end
 local function caster()
  if not s.inventory then return end -- API v1 fixtures / hosts.
  local best,unknown
  for _,v in ipairs(inventory) do if band(v.type,0x8000)~=0 then
   if v.equipped then return end
   if not equipment_known(v) then unknown=v
   elseif v.can_wield and (not best or (v.element_mod or 1)>(best.element_mod or 1)) then best=v end
  end end
  if best then return equip(best) end
  if unknown then return assess(unknown) end
  return failed('No usable casting equipment; add a wand, staff or orb')
 end
 local function idle_stance()
  if not target and not nearby_monsters and option('idle_peace',false) and (s.combat_mode or 1)~=1 then
   return {action='combat_mode',mode=1,status='Resting in peace mode',continue_work=true}
  end
 end
 local function route_door(destination)
  if not option('open_doors',true) then return end
  for id,attempt in pairs(door_attempts) do
   if s.time-attempt.time>30 then door_attempts[id]=nil end
  end
  local px,py=world(s.position);local tx,ty=world(destination)
  local dx,dy=tx-px,ty-py;local length=dx*dx+dy*dy
  if length==0 then return end
  for _,door in ipairs(s.route_objects or {}) do
   if door.object_class==26 then
    if door.door_open then door_attempts[door.id]=nil
    elseif door.distance<=option('door_range',2) and s.time>=(until_time['door'..door.id] or 0) then
     local x,y=world(door);local along=((x-px)*dx+(y-py)*dy)/length
     if along>=0 and along<=1 and (x-px-along*dx)^2+(y-py-along*dy)^2<2.25 then
      local attempt=door_attempts[door.id]
      -- A used door can close again. Only bound retries at the same obstruction;
      -- opening it, moving away, or returning later starts a fresh attempt.
      if not attempt or distance(s.position,attempt.position)>2 or s.time-attempt.time>30 then
       attempt={count=0,position=s.position};door_attempts[door.id]=attempt
      end
      if door.door_identified==false then
       attempt.ids=(attempt.ids or 0)+1;attempt.time=s.time;until_time['door'..door.id]=s.time+5
       if attempt.ids>3 then return pause_navigation('cannot assess route door') end
       return {action='identify',item=door.id,status='Inspecting route door'}
      end
      local pick
      if door.door_locked then
       local skill=(s.skills or {})['23'] or {}
       if (skill.training or 0)<2 or (skill.current or 0)<(door.lock_difficulty or 0)+option('door_lockpick_margin',0) then
        return pause_navigation('route door requires a higher Lockpick skill')
       end
       local function named(item)
        if not p.assist_items then return true end
        for _,a in ipairs(p.assist_items) do if a.type==10 and a.name==item.name then return true end end
        return false
       end
       for _,item in ipairs(inventory) do
        if item.object_class==30 and item.usable and item.lockpick_available~=false and not item.equipped
         and (item.structure or 0)>0 and named(item) and (not pick or item.structure<pick.structure) then pick=item end
       end
       if not pick then return pause_navigation('no usable configured lockpick for route door') end
      end
      if attempt.count>=3 then return pause_navigation('door remains closed after three attempts') end
      attempt.count=attempt.count+1;attempt.time=s.time
      until_time['door'..door.id]=s.time+5
      if pick then return {action='apply_item',item=pick.id,target=door.id,status='Unlocking route door'} end
      return {action='use_world',item=door.id,status='Opening route door'}
     end
    end
   end
  end
 end
 local function navigate()
 if meta.pending then return end
 if not activity_ready('navigation') then return end
 if option('navigation',false) then
  -- VT's timed pause stops route movement, not the higher-priority combat and
  -- recharge rules. It has no server acknowledgement to monopolize the loop.
  if route_pending and route_pending.kind=='pause' then
   if s.time<route_pending.deadline then return end
   route_pending=nil;point=point+route_direction
   return result(nil,'Route pause complete')
  end
  -- A pending portal/recall has already been submitted. Yield without issuing
  -- it again, including when navigation priority is enabled.
  if route_pending then return end
  local idle=idle_stance();if idle then return idle end
  if (p.follow_name or '')~='' or (p.follow_id or 0)~=0 then
   local follow
   for _,o in ipairs(s.route_objects or {}) do
    if ((p.follow_name or '')~='' and o.name==p.follow_name) or ((p.follow_name or '')=='' and o.id==p.follow_id) then follow=o;break end
   end
   if not follow then return result(nil,'Waiting for follow target to enter range') end
   -- Being close through a wall is not the same as reaching the next footstep.
   if (not option('follow_corners',false) or #follow_path<=1) and follow.distance<=option('follow_distance',3) then return result(nil,'Following '..follow.name) end
   local aim=option('follow_corners',false) and follow_path[1] or follow
   aim=aim or follow
   local door=route_door(aim);if door then return door end
   return {action='move',cell=aim.cell,x=aim.x,y=aim.y,z=aim.z,arrival_radius=option('follow_corners',false) and .15 or nil,status='Following '..follow.name}
  end
  if meta.route_changed then return result(nil,'Loading route geometry') end
  local route=p.route or {};if #route==0 then return result(nil,'Record a route to begin navigation') end
  -- Return along positions actually traversed during combat/looting. Picking
  -- the nearest waypoint after a chase can cut across an adjacent room wall.
  if route_walking then route_trail={} end
  -- Loot can be directly ahead on the leg already being followed. Once back
  -- on that same segment, continue toward its pending waypoint instead of
  -- walking backwards to the departure point. The tight 3D corridor excludes
  -- parallel rooms/floors; off-route returns still retain their observed bends.
  local destination=route[point]
  if not route_join_pending and #route_trail>0 and destination and (not destination.legacy or destination.walk_first) then
   local ax,ay,az=world(route_trail[1]);local bx,by,bz=world(destination);local x,y,z=world(s.position)
   local dx,dy,dz=bx-ax,by-ay,bz-az;local length=dx*dx+dy*dy+dz*dz
   local t=length>0 and ((x-ax)*dx+(y-ay)*dy+(z-az)*dz)/length or -1
   local radius=math.min(option('waypoint_radius',.8),.8)
   if t>=0 and t<=1 and (x-ax-t*dx)^2+(y-ay-t*dy)^2+(z-az-t*dz)^2<=radius^2 then route_trail={} end
  end
  while #route_trail>0 do
   local back=route_trail[#route_trail]
   local radius=math.min(option('waypoint_radius',.8),.8)
   if distance(s.position,back)<=radius then table.remove(route_trail)
   else route_returning=true;local door=route_door(back);if door then return door end
    return {action='move',cell=back.cell,x=back.x,y=back.y,z=back.z,arrival_radius=radius,status='Returning to route'} end
  end
  route_returning=false;route_walking=true
  if route_join_pending then
   -- The host checks nearest entries in bounded batches. An untested entry is
   -- not an obstruction, and choosing a farther entry mid-scan can skip actions.
   if s.route_visible_pending then route_join_scan=nil;return {status='Finding nearest route waypoint',continue_work=true} end
   -- Join once, then retain the ordered cursor through combat, looting and
   -- portal/jump acknowledgements. Camera heading must not affect the choice.
   -- Imported NAV action coordinates can be placeholders; only walk-first
   -- entries are spatial anchors. Keep action-only routes in their original order.
   local scan=route_join_scan or {index=1,position=s.position,visible=s.route_visible};route_join_scan=scan
   local here=scan.position;local cell=math.floor(here.cell)
   while scan.index<=#route do
    if not workavailable() then return result(nil,'Finding nearest route waypoint') end
    local i=scan.index;local candidate=route[i];scan.index=i+1
    local spatial=not candidate.legacy or candidate.walk_first
    if spatial then
     scan.first=scan.first or i
     local target=math.floor(candidate.cell)
     local connected=candidate.legacy or (cell>>16)==(target>>16) or ((cell&0xffff)<0x100 and (target&0xffff)<0x100)
     if connected and (not scan.visible or scan.visible[tostring(i)]~=false) then
      local d=distance(here,candidate)
      if not scan.distance or d<scan.distance then scan.point=i;scan.distance=d end
     end
    end
   end
   -- Native routes in another dungeon cannot be joined by walking to their
   -- unrelated coordinates. Do not silently charge towards waypoint one.
   if not scan.point and scan.first then route_join_scan=nil;return result(nil,'No reachable route waypoint in this area') end
   -- Retain a NAV's leading setup/use/confirmation actions when joining its
   -- first spatial anchor. They are not independent destinations to skip.
   point=(scan.point==scan.first) and 1 or (scan.point or 1);route_join_pending=false;route_join_scan=nil
  end
  -- VT advances reached walk points in the same decision. Leaving the old
  -- steering target active for an idle tick can turn back toward that point.
  -- Bound duplicate/looping points; route actions still execute individually.
  for _=1,math.min(#route+1,64) do
  if point>#route or point<1 then
   if p.reverse_route and #route>1 then route_direction=-route_direction;point=point>#route and #route-1 or 2
   elseif p.loop_route then point=point<1 and #route or 1
   elseif p.reverse_route then point=1
   else return result(nil,'Route complete') end
  end
  local v=route[point];local kind=route_kind(v)
  if distance(s.position,v)<=option('waypoint_radius',.8) then route_blocks=0 end
  if (not v.legacy or v.walk_first) and distance(s.position,v)>option('waypoint_radius',.8) then
   local door=route_door(v);if door then return door end
   return {action='move',cell=v.cell,x=v.x,y=v.y,z=v.z,arrival_radius=option('waypoint_radius',.8),coordinate_route=v.legacy,status='Waypoint '..point..' / '..#route}
  end
  if kind=='checkpoint' and (not s.server_position or distance(s.server_position,v)>option('waypoint_radius',.8)) then return result(nil,'Checkpoint: waiting for server position') end
  if kind=='pause' then
   route_pending={kind='pause',time=s.time,deadline=s.time+(v.seconds or 5)}
   route_trail={{cell=s.position.cell,x=s.position.x,y=s.position.y,z=s.position.z}}
   return result(nil,'Pausing on route')
  end
  if kind=='recall' then
   if not by_id[v.spell] or by_id[v.spell].known==false then return failed('Route recall spell is not known') end
   local equipment=caster();if equipment then return equipment end
   if not v.legacy and not route[point+route_direction] then return failed('Record a destination point after the recall') end
   route_pending={kind=kind,time=s.time,serial=s.action_serial,teleport=s.teleport_sequence};return cast(s,by_id[v.spell],s.player,'Recalling on route')
  end
  if kind=='command' then local e=meta_command(v.command,s);if e then if e=='Stopped by UCM command' then return result('stop',e) end;return failed(e) end;point=point+route_direction;return result(nil,'Route command completed') end
  if kind=='vendor' then
   local id=v.object_id or 0
   if (s.vendor or 0)~=id then
    if id==0 then
     route_pending={kind=kind,time=s.time,vendor=0}
     return {action='close_vendor',vendor=s.vendor,status='Closing vendor'}
    end
    local object
    for _,o in ipairs(s.route_objects or {}) do if o.id==id and o.object_class==25 then object=o;break end end
    if not object then return pause_navigation('recorded vendor is not visible') end
    route_pending={kind=kind,time=s.time,vendor=id}
    return {action='use_world',item=id,status='Opening '..object.name}
   end
  end
  if kind=='jump' then route_pending={kind=kind,time=s.time};return {action='jump',heading=v.heading or 0,current_heading=v.current_heading,charge=v.charge or .5,forward=v.forward or 1,strafe=v.strafe or 0,walk_jump=v.walk_jump,status='Charging route jump'} end
  if kind=='portal' or kind=='use' then
   if kind=='portal' and not v.legacy and not route[point+route_direction] then return failed('Record a destination point after the portal') end
   local object
   for _,o in ipairs(s.route_objects or {}) do
    local match=(v.object_id and o.id==v.object_id) or (not v.object_id and ((v.wcid and o.wcid==v.wcid) or o.name==v.object_name))
    if v.object_class and o.object_class~=v.object_class then match=false end
    if match and v.object_coordinates then
     local x,y,z=world(o);local c=v.object_coordinates
     match=math.sqrt((-x-(c.ew*240+24468))^2+(y-(c.ns*240+24468))^2+(z-c.z*240)^2)<=2.5
    end
    if match and (not object or o.distance<object.distance) then object=o end
   end
   if not object then return failed('Route object is not visible; select it and record Use again') end
   route_pending={kind=kind,time=s.time,item=object.id,serial=s.action_serial or 0,teleport=s.teleport_sequence};return {action='use_world',item=object.id,status='Using '..object.name}
  end
  point=point+route_direction
  end
  return {status='Advancing reached waypoints',continue_work=true}
 end
 end
 -- Confirm the vital actually improved. A failed kit or fizzle must not loop forever.
 local function split_peas(minimum)
  if not activity_ready('components') then return end
  if s.components_required==false then pea_pending=nil;return end
  if not pea_pending and (not option('split_peas',true) or #(p.pea_recipes or {})==0) then return end
  local counts,items={},{};for _,v in ipairs(inventory) do if not v.equipped and v.name then counts[v.name]=(counts[v.name] or 0)+(v.count or 1);items[v.name]=v end end
  if pea_pending then local q=pea_pending
   local receipt=action_receipt(s,'use',q.serial,q.started,q.tool,q.input)
   if (counts[q.output] or 0)>q.before then pea_pending=nil;item_attempts['pea'..q.output]=0
   elseif s.time>=q.deadline or (receipt and (receipt.error or 0)~=0) then return failed('Pea splitting was not confirmed; check supplies and pack space')
   else return end -- Await inventory replication without postponing recovery.
  end
  if not option('split_peas',true) then return end
  local selected,all={},false
  for _,v in ipairs(p.assist_items or {}) do if v.type==9 then selected[v.name]=true elseif v.type==11 and v.name=='[All Peas]' then all=true end end
  for _,recipe in ipairs(p.pea_recipes or {}) do
   if (all or selected[recipe.output]) and (counts[recipe.output] or 0)<minimum then
    local tool,input=items[recipe.tool],items[recipe.input]
    if tool and input and (input.count or 1)>0 then
     if (s.combat_mode or 1)~=1 then return {action='combat_mode',mode=1,status='Preparing to split '..recipe.input} end
     if (item_attempts['pea'..recipe.output] or 0)>=3 then return failed('Pea splitting exceeded retry limit') end
     item_attempts['pea'..recipe.output]=(item_attempts['pea'..recipe.output] or 0)+1
     pea_pending={output=recipe.output,before=counts[recipe.output] or 0,tool=tool.id,input=input.id,started=s.time,serial=s.action_serial or 0,deadline=s.time+15}
     return {action='apply_item',item=tool.id,target=input.id,status='Splitting '..recipe.input}
    end
   end
  end
 end
 local critical_components=split_peas(option('component_critical',4));if critical_components then return critical_components end
 for v,q in pairs(recovery_pending or {}) do
  local value=v==2 and s.health or v==4 and s.stamina or s.mana
  local failed,confirmed=false,false
  if s.action_events then
   for _,e in ipairs(s.action_events) do
    if (e.action_serial or 0)>q.serial and (not e.requested_at or e.requested_at>=q.started)
     and ((q.spell and e.type=='cast' and e.spell==q.spell and e.target==s.player)
       or (q.item and e.type=='use' and e.source==q.item)) then
     failed=(e.error or 0)~=0 or (q.spell and e.confirmed==false)
     confirmed=q.spell and e.confirmed==true and not failed
    end
   end
  else
   -- Older hosts expose only the last cast. Never attribute an unrelated item
   -- or spell completion to a recovery whose acknowledgement is still pending.
   local spell_done=q.spell and (s.action_serial or 0)~=q.serial and s.last_spell==q.spell
   failed=spell_done and ((s.action_error or 0)~=0 or s.last_spell_confirmed==false)
   confirmed=spell_done and s.last_spell_confirmed==true and not failed
  end
  -- Vital replication and UseDone may arrive in either order. The authoritative
  -- vital increase is enough; do not accumulate failures across successful uses.
  -- A confirmed heal can be smaller than damage received in the same network
  -- batch. Net HP is not a cast-success test, nor a reason to disable healing.
  if not failed and (confirmed or value>q.before) then recovery_pending[v]=nil
  elseif failed or s.time>=q.expires then
   -- A rejected or unconfirmed preferred method yields to the other kind.
   -- Otherwise a bad kit could consume all retries without ever trying a heal.
   if q.spell then
    -- Back off only this failed tier briefly; other heals/conversions remain
    -- eligible immediately. Missing components retain the explicit longer ban.
    unavailable_spells[q.spell]=math.max(unavailable_spells[q.spell] or 0,s.time+1)
    catalog_snapshot=nil;by_id,spells,spell_groups=index_spells(s,option('skill_margin',30))
   elseif q.item then until_time['recover_supply'..v]=s.time+15 end
   if q.handler and not q.spell then until_time[q.handler]=s.time+15 end
   recovery_pending[v]=nil
  end
 end
 local recovery_missing={}
 local function recovery(vital,percent,threshold)
  if not activity_ready('recovery'..vital) then return end
  for v in pairs(recovery_pending or {}) do if v<=vital then return end end
  -- The conversion slider is an independent health trigger, not a subset of
  -- the regular-heal slider. Keep explicit imported handler order authoritative.
  local conversion_threshold=option('emergency_health_threshold',35)
  if vital==2 and not (p.recharge_handlers and option('use_imported_recovery_order',true)) then
   threshold=math.max(threshold,conversion_threshold)
  end
  if percent>=threshold then return end
  local function pending(handler)
   recovery_pending=recovery_pending or {}
   recovery_pending[vital]={before=vital==2 and s.health or vital==4 and s.stamina or s.mana,expires=s.time+8,started=s.time,serial=s.action_serial or 0,handler=handler}
  end
  local function can_prepare_spell()
   if s.time<(until_time['recover_spell'..vital] or 0) then return false end
   if not s.inventory then return true end
   for _,item in ipairs(inventory) do
    if band(item.type,0x8000)~=0 and (item.equipped or (equipment_known(item) and item.can_wield)) then return true end
   end
   return false
  end
  local function recover_cast(spell,handler)
   if spell.implement then
    local item=spell.implement
    if not item.equipped then return equip(item) end
    pending(handler);recovery_pending[vital].item=item.id
    return {action='cast_item',item=item.id,spell=spell.id,target=s.player,status='Recovering with '..item.name}
   end
   local equipment=caster();if equipment then return equipment end
   pending(handler);recovery_pending[vital].spell=spell.id
   local action=cast(s,spell,s.player,'Recovering with '..(spell.name or 'spell'));last_cast.recovery=true
   return action
  end
  local function innate_recovery(best,matches)
   if s.time<(until_time['recover_supply'..vital] or 0) then return best end
   local weapon_allowed=equipment_selection(p,inventory)
   for _,item in ipairs(inventory) do
    local v=item.innate_recovery
    local cooldown=tostring(math.floor((item.int_properties or {})['280'] or 0))
    if v and item.innate_recovery_available and ((s.cooldowns or {})[cooldown] or 0)<=0 and equipment_known(item) and (item.equipped or item.can_wield)
     and weapon_allowed(item) and matches(v) and (not best or v.power>best.power) then
     best={};for key,value in pairs(v) do best[key]=value end;best.implement=item
    end
   end
   return best
  end
  local function named_supply(item,kind)
   if not p.assist_items then return true end
   for _,a in ipairs(p.assist_items) do if a.type==kind and a.name==item.name then return true end end
   return false
  end
  local function assess_supply(handlers)
   for _,v in ipairs(inventory) do
    local candidate=not v.identified and v.usable and not v.equipped and allowed(p.consumables,v.wcid)
     and ((v.healing_kit and trained[21] and (v.structure or 0)>0) or band(v.type,32)~=0 or contains(p.consumables,v.wcid))
    if candidate and handlers then
     candidate=false
     for _,h in ipairs(handlers) do
      local kit=h.handler=='Kit Recharge'
      if h.vital==vital and h.stance==(s.combat_mode==8 and 1 or 2) and (kit or h.handler=='Recharge With Food')
       and (v.healing_kit==true)==kit and named_supply(v,vital-2+(kit and 0 or 1)) then candidate=true;break end
     end
    end
    if candidate and (item_attempts['id'..v.id] or 0)<3 then return assess(v) end
   end
  end
  local function source_ready(source)
   if source==0 then return true end
   local percent=source==2 and hp or source==4 and sp or mp
   local reserve=source==2 and option('health_threshold',65) or source==4 and option('stamina_threshold',50) or option('mana_threshold',45)
   return percent>math.max(source==2 and 50 or 25,reserve)
  end
  local function kit_ready(v)
   if not trained[21] or (v.structure or 0)<=0 or (vital~=4 and (s.stamina or 0)<15) then return false end
   if s.combat_mode==8 and not option('kits_in_magic',true) then return false end
   local minimum=option('kit_min_success',0)
   if minimum<=0 then return true end
   local healing=(s.skills or {})['21'];if not healing then return false end
   local missing=vital==2 and s.max_health-s.health or vital==4 and s.max_stamina-s.stamina or s.max_mana-s.mana
   -- VT's kit estimate uses current Healing + kit bonus against twice the
   -- missing vital, with a 10% combat difficulty penalty. This is a prediction;
   -- only the server's subsequent vital update confirms a successful heal.
   local difficulty=math.ceil(math.max(0,missing)*(s.combat_mode==1 and 2 or 2.2))
   local chance=100/(1+math.exp(math.max(-700,math.min(700,-.03*((healing.current or 0)+(v.boost or 0)-difficulty)))))
   return chance>=minimum
  end
  local function better_kit(v,best)
   return not best or (v.heal_mod or 1)>(best.heal_mod or 1)
    or ((v.heal_mod or 1)==(best.heal_mod or 1) and v.structure<best.structure)
  end
  local function use_supply(best,handler)
   if best.healing_kit and option('kit_peace',false) and s.combat_mode~=1 then return {action='combat_mode',mode=1,status='Entering peace to use healing kit'} end
   pending(handler);recovery_pending[vital].item=best.id;return {action='use_item',item=best.id,status='Recovering with '..best.name}
  end
  -- Health emergencies may spend stamina below its normal recovery reserve.
  -- Choose the lowest eligible transfer tier that reaches the health threshold;
  -- otherwise prefer the largest gain. Never override an imported handler order
  -- or an explicit preference for supplies.
  if vital==2 and percent<option('emergency_health_threshold',35) and (s.stamina or 0)>10
   and not (p.recharge_handlers and option('use_imported_recovery_order',true))
   and not option('recovery_supplies_first',false) and can_prepare_spell() then
   local conversion,gain=nil,0
   local needed=math.max(1,math.ceil((s.max_health or 0)*threshold/100-(s.health or 0)))
   for _,v in ipairs(spell_groups.instant_self) do
    local destination,from=recovery_spell(v)
    if destination==2 and from==4 then
     local amount=health_gain(v,s.stamina)
     if not conversion or (amount>=needed and gain<needed) or (amount>=needed and gain>=needed and v.power<conversion.power)
      or (amount<needed and gain<needed and amount>gain) then conversion=v;gain=amount end
    end
   end
   if conversion and gain>0 then return recover_cast(conversion) end
  end
  if p.recharge_handlers and option('use_imported_recovery_order',true) then
   local stance=s.combat_mode==8 and 1 or 2;local last
   for i,h in ipairs(p.recharge_handlers) do if h.vital==vital and h.stance==stance then last=i end end
   for i,h in ipairs(p.recharge_handlers) do
    local key='handler'..i
    if h.vital==vital and h.stance==stance and (i==last or (math.floor(percent)>=h.min and math.floor(percent)<=h.max)) and s.time>=(until_time[key] or 0) then
     local best,best_rank;local kind=h.handler
     if kind=='Recharge With Food' or kind=='Kit Recharge' then
      local kit=kind=='Kit Recharge'
      for _,v in ipairs(inventory) do
       if s.time>=(until_time['recover_supply'..vital] or 0) and (v.count or 1)>0 and v.identified and v.usable and v.boost_vital==vital and (v.healing_kit==true)==kit and allowed(p.consumables,v.wcid)
        and named_supply(v,vital-2+(kit and 0 or 1)) and (not kit or kit_ready(v))
        and ((kit and better_kit(v,best)) or (not kit and (not best or (v.boost or 0)>(best.boost or 0)))) then best=v end
      end
      if best then return use_supply(best,key) end
     else
      local conversion=kind~='Regular Spell'
      local source=kind:sub(1,6)=='Health' and hp or kind:sub(1,7)=='Stamina' and sp or mp
      local reserve=kind:sub(1,6)=='Health' and option('health_threshold',65) or kind:sub(1,7)=='Stamina' and option('stamina_threshold',50) or option('mana_threshold',45)
      if not conversion or (vital==2 and source>0) or (vital~=2 and source>=reserve) then
       for _,v in ipairs(spell_groups.instant_self) do
        local destination,from=recovery_spell(v)
        local regular=destination==vital and (from==0 or (vital==6 and from==4 and source_ready(from)))
        local handler_source=kind:sub(1,6)=='Health' and 2 or kind:sub(1,7)=='Stamina' and 4 or 6
        local matches=conversion and destination==vital and from==handler_source or (not conversion and regular)
        local rank=not conversion and vital==6 and from==4 and 2 or 1
        if matches and v.caster_target and (v.duration or 0)==0 and (not best or rank>best_rank or (rank==best_rank and stronger_spell(v,best))) then best=v;best_rank=rank end
       end
       best=innate_recovery(best,function(v)
        local destination,from=recovery_spell(v)
        local handler_source=kind:sub(1,6)=='Health' and 2 or kind:sub(1,7)=='Stamina' and 4 or 6
        return destination==vital and (conversion and from==handler_source or not conversion and from==(vital==6 and 4 or 0))
       end)
      end
      if best and conversion and vital==2 then
       local healing,heal_power=0,-1
       local amounts={['Heal Self I']=17,['Heal Self II']=25,['Heal Self III']=32,['Heal Self IV']=45,['Heal Self V']=67,['Heal Self VI']=87,["Adja's Intervention"]=115,['Incantation of Heal Self']=135}
       for _,v in ipairs(spell_groups.instant_self) do local destination,from=recovery_spell(v);if destination==2 and from==0 and v.power>heal_power then heal_power=v.power;healing=amounts[v.name] or 10 end end
       local multiplier=kind=='Stamina to Health' and option('stamina_health_multiplier',1) or option('mana_health_multiplier',1)
       local source_value=kind=='Stamina to Health' and s.stamina or math.max(0,s.mana-30)
       local ratio,cap=1.75,math.huge
       local levels={{' I',.9,50},{' II',1,100},{' III',1.1,150},{' IV',1.2,200},{' V',1.35,math.huge},{' VI',1.5,math.huge}}
       for _,v in ipairs(levels) do if best.name:sub(-#v[1])==v[1] then ratio=v[2];cap=v[3];break end end
       local missing=s.max_health-s.health;local gain=math.min(missing,cap,math.floor(source_value*ratio))
       if healing>missing or healing*multiplier>=missing or healing*multiplier>=gain then best=nil end
      end
      if best and (best.implement or can_prepare_spell()) then return recover_cast(best,key) end
     end
    end
   end
   local appraisal=assess_supply(p.recharge_handlers);if appraisal then return appraisal end
   recovery_missing[#recovery_missing+1]=vital
   return -- An explicit handler list must not fall back to unrelated supplies.
  end
  local best
  for _,v in ipairs(inventory) do
   if s.time>=(until_time['recover_supply'..vital] or 0) and (v.count or 1)>0 and v.identified and v.usable and v.boost_vital==vital and allowed(p.consumables,v.wcid)
    and (not v.healing_kit or kit_ready(v))
    and (not best or (v.boost or 0)>(best.boost or 0)) then best=v end
  end
  local supply=best;best=nil
  if supply and option('recovery_supplies_first',false) then return use_supply(supply) end
  local best_rank=0;local casting_available=can_prepare_spell()
  for _,v in ipairs(spell_groups.instant_self) do
   local destination,from=recovery_spell(v)
   if destination==vital and source_ready(from) and casting_available then
    -- VT Regular Spell uses Stamina to Mana; direct Mana Boost is fallback.
    local rank=vital==6 and from==4 and 3 or from==0 and 2 or 1
    if not best or rank>best_rank or (rank==best_rank and stronger_spell(v,best)) then best=v;best_rank=rank end
   end
  end
  best=innate_recovery(best,function(v)
   local destination,from=recovery_spell(v)
   return destination==vital and from==(vital==6 and 4 or 0) and source_ready(from)
  end)
  if best then return recover_cast(best) end
  if supply then return use_supply(supply) end
  local appraisal=assess_supply();if appraisal then return appraisal end
  recovery_missing[#recovery_missing+1]=vital
 end
 if option('recovery',true) then
  local action=recovery(2,hp,not target and math.max(option('idle_health_threshold',0),option('health_threshold',65)) or option('health_threshold',65))
   or recovery(4,sp,not target and math.max(option('idle_stamina_threshold',0),option('stamina_threshold',50)) or option('stamina_threshold',50))
   or recovery(6,mp,not target and math.max(option('idle_mana_threshold',0),option('mana_threshold',45)) or option('mana_threshold',45))
  if action then return action end
 end
 if recovery_pending and next(recovery_pending) then return result(nil,'Waiting for recovery result') end
 recovery_pending=nil
 local craft=craft_work(s,p,'critical');if craft then return craft end
 -- Vendor waits never outrank VT's vital recovery. Keep the route at the
 -- vendor until restocking finishes, so a later Close Vendor cannot race Buy.
 if route_wait then return result(nil,route_wait) end
 local vendor=vendor_work(s,p,inventory,option);if vendor then return vendor end
 -- Even without a ready heal, keep fighting; stopping here abandons the player.
 local health_critical=hp<option('stop_health',15)
 -- VT c4/c8/cx only dispel temporary elemental vulnerabilities, not every
 -- negative enchantment. A UseDone acknowledgement alone is not success.
 activity='dispel'
 local vulnerabilities={};local lowest_vulnerability
 for _,e in ipairs(s.harmful_enchantments or {}) do
  if e.category>=102 and e.category<=114 and e.category%2==0 and (e.remaining or 0)>0 then
   vulnerabilities[e.id]=true;lowest_vulnerability=math.min(lowest_vulnerability or math.huge,e.power or math.huge)
  end
 end
 if dispel_pending then
  local q=dispel_pending;local removed=false;for id in pairs(q.before) do if not vulnerabilities[id] then removed=true;break end end
  local receipt=action_receipt(s,q.kind,q.serial,q.started,q.id,q.target)
  if removed then dispel_pending=nil
  elseif s.time>=q.expires or (receipt and (receipt.error or 0)~=0) then
   dispel_pending=nil;until_time.dispel=s.time+15
  end
 end
 if not dispel_pending and activity_ready('dispel') and lowest_vulnerability and s.time>=(until_time.dispel or 0) and (option('dispel_self',false) or option('dispel_items',false)) then
  local function pending_dispel(kind,id,target)
   dispel_pending={before=vulnerabilities,expires=s.time+8,serial=s.action_serial or 0,started=s.time,kind=kind,id=id,target=target}
  end
  if option('dispel_self',false) then
   local chorizite=false;for _,v in ipairs(inventory) do if v.name=='Chorizite' and (v.count or 1)>0 then chorizite=true;break end end
   local best
   if chorizite then for _,v in ipairs(spell_groups.by_category[250] or {}) do
    if v.category==250 and v.school==2 and v.caster_target and v.power>=lowest_vulnerability and (not best or v.power>best.power) then best=v end
   end end
   if best then local equipment=caster();if equipment then return equipment end;pending_dispel('cast',best.id,s.player);return cast(s,best,s.player,'Dispelling elemental vulnerabilities') end
  end
  if option('dispel_items',false) then
   for _,choice in ipairs({{'Rune of Dispel',400},{'Society Gem of Dispelling',400},{'Black Market Gem of Dispelling',400},{'Chocolate Gromnie',350},{'Condensed Dispel Potion',350},{'Gem of Stillness',350}}) do
    if lowest_vulnerability<=choice[2] then for _,v in ipairs(inventory) do
     local cooldown=string.format('%.0f',(v.int_properties or {})['280'] or 0)
     if v.name==choice[1] and v.usable and not v.equipped and (v.count or 1)>0 and ((s.cooldowns or {})[cooldown] or 0)<=0 then
      pending_dispel('use',v.id);return {action='use_item',item=v.id,status='Dispelling with '..v.name}
     end
    end end
   end
  end
 end
 local fellows=s.fellowship or {}
 if helper_pending then
  local q=helper_pending;local member
  local receipt=cast_receipt(s,q)
  for _,m in ipairs(fellows.members or {}) do if m.id==q.target then member=m;break end end
  if member and (member.vitals_age or 1e9)<10 and (member[q.vital] or 0)>q.before then
   helper_pending=nil;item_attempts[q.key]=0
  elseif not fellows.valid or not member or s.time>=q.expires or (receipt and (receipt.error or 0)~=0) then
   helper_pending=nil;until_time[q.key]=s.time+15
  end
 end
 if not helper_pending and activity_ready('helper') and not forced_buff and option('recovery',true) and fellows.valid then
  for index,vital in ipairs({'health','stamina','mana'}) do
   local threshold=option('helper_'..vital..'_threshold',0)
   local recipient,lowest=nil,threshold
   for _,m in ipairs(fellows.members or {}) do
    local key='helper'..tostring(m.id)..vital
    if m.id~=s.player and (m.health or 0)>0 and (m['max_'..vital] or 0)>0 and (m.vitals_age or 1e9)<10
     and m.line_of_sight==true and (m.distance or 1e9)<=option('helper_'..vital..'_range',index==3 and 40 or 74.5)
     and s.time>=(until_time[key] or 0) and (item_attempts[key] or 0)<3 then
     local percent=math.floor(pct(m[vital] or 0,m['max_'..vital]));if percent<lowest then recipient=m;lowest=percent end
    end
   end
   if recipient then
    local best
    for _,spell in ipairs(spell_groups.by_category[77+index*2] or {}) do
     if spell.category==77+index*2 and not spell.caster_target and spell.beneficial and (spell.duration or 0)==0 and band(spell.flags,0x2000)==0
      and (not best or spell.power>best.power) then best=spell end
    end
    if best then
     local equipment=caster();if equipment then return equipment end
     local key='helper'..tostring(recipient.id)..vital;item_attempts[key]=(item_attempts[key] or 0)+1
     helper_pending={target=recipient.id,spell=best.id,started=s.time,vital=vital,before=recipient[vital],serial=s.action_serial or 0,expires=s.time+10,key=key}
     return cast(s,best,recipient.id,'Restoring '..recipient.name..'\'s '..vital)
    end
   end
  end
 end
 if option('fill_mana_stones',false) then
  for _,v in ipairs(inventory) do
   if not v.identified and v.usable and band(v.type,0x80000)~=0 and (item_attempts['id'..v.id] or 0)<3
    and s.time>=(until_time['id'..v.id] or 0) then activity='item_mana';return assess(v) end
  end
 end
 local mana_action=equipment_mana(s,p,inventory,option,assess);if mana_action then return mana_action end
 local function buffs(refresh)
 if health_critical or not activity_ready('buffs') then return end
 if forced_buff or option('buffing',true) then
  -- Consumable buffs require an observed enchantment, not merely UseDone.
  -- Failed uses rotate to the next item instead of consuming a whole stack.
  if buff_item_pending then
   local q=buff_item_pending
   local receipt=action_receipt(s,'use',q.serial,q.started,q.item)
   local confirmed=false
   for _,e in ipairs(s.enchantments or {}) do if timed_buff(e) and e.category==buff_item_pending.category and e.power>=buff_item_pending.power and e.remaining>buff_item_pending.before then confirmed=true;break end end
   if confirmed then
    if forced_buff then forced_buff.done['item'..buff_item_pending.name]=true end
    item_attempts['buffitem'..buff_item_pending.name]=0;buff_item_pending=nil
   elseif not (receipt and (receipt.error or 0)~=0) and s.time<buff_item_pending.expires then return result(nil,'Waiting for consumable buff confirmation')
   else until_time['buffitem'..buff_item_pending.name]=s.time+30;buff_item_pending=nil end
  end
  local settings={};for k,val in pairs(p) do settings[k]=val end;for k,val in pairs(current) do settings[k]=val end
  if refresh then settings.refresh_seconds=refresh end
  local v,id,blocked=next_buff(s,settings)
  -- Missing components or rejected families must not stop the rest of a run.
  if v then
   local key=tostring(id)..':'..v.category
   -- Assess direct equipment targets before changing weapons or attempting a
   -- cast. Unknown gear can be unenchantable even though it can be wielded.
   if id~=(s.player or 0) then
    for _,item in ipairs(inventory) do if item.id==id and item.identified==false then
     if (item_attempts['id'..id] or 0)>=3 then
      buff_skips[key]=s.time+120;buff_plan=nil
      return result(nil,'Skipping unassessed buff target: '..item.name)
     end
     return assess(item)
    end end
   end
   if not forced_buff and s.time<(until_time['buffack'..id..':'..v.id] or 0) then return result(nil,'Waiting for buff timer update') end
   if s.time<(until_time[key] or 0) then return result(nil,'Waiting for buff confirmation') end
   if mp<option('stop_mana',10) then return failed('Mana too low to buff; configure recovery') end
   local equipment=caster()
   if (retries[key] or 0)>=3 or (equipment and equipment.action=='activity_failed') then
    buff_skips[key]=s.time+120;retries[key]=nil;until_time[key]=nil;buff_plan=nil
    if forced_buff then forced_buff.skipped=forced_buff.skipped or {};forced_buff.skipped[key]=true end
    return result(nil,'Skipping failed buff: '..(v.name or tostring(v.id))..'; continuing cycle')
   end
   if equipment then return equipment end
   retries[key]=(retries[key] or 0)+1;until_time[key]=s.time+12
   buff_recast_until=s.time+30;buff_plan=nil
   local action=cast(s,v,id,(forced_buff and 'Force Buff: ' or 'Buffing: ')..(v.name or tostring(v.id)))
   last_cast.buff=true
   if forced_buff then last_cast.force_cycle=forced_buff;last_cast.power=v.power end
   return action
  end
  for _,b in ipairs(p.buff_items or {}) do
   local spell=by_id[b.spell];local item=usable_names[b.name]
   if spell and item and (spell.school~=3 or p.item_buffs_enabled~=false) and (band(spell.flags,0x2000)==0 or s.in_fellowship) then
    local active,before=false,0;for _,e in ipairs(s.enchantments or {}) do if timed_buff(e) and e.category==spell.category and e.power>=spell.power then before=math.max(before,e.remaining);if e.remaining>buff_refresh_window(s,settings,e.duration or spell.duration) then active=true end end end
    local needed=forced_buff and not forced_buff.done['item'..b.name] or (not forced_buff and not active)
    if needed and (item_attempts['buffitem'..b.name] or 0)<3 then
     local key='buffitem'..b.name
     local cooldown=tostring(math.floor((item.int_properties or {})['280'] or 0))
     if s.time>=(until_time[key] or 0) and ((s.cooldowns or {})[cooldown] or 0)<=0 then
      item_attempts[key]=(item_attempts[key] or 0)+1
      buff_item_pending={name=b.name,item=item.id,category=spell.category,power=spell.power,before=before,serial=s.action_serial or 0,started=s.time,expires=s.time+12}
      return {action='use_item',item=item.id,status='Buffing with '..b.name}
     elseif forced_buff then
      forced_buff.skipped=forced_buff.skipped or {};forced_buff.skipped[key]=true
     end -- A failed/cooling-down consumable must not hold up the remaining buffs.
    end
   end
  end
  meta.forcebuff=false
  if forced_buff then
   local skipped=blocked or (forced_buff.skipped and next(forced_buff.skipped))
   for _,b in ipairs(p.buff_items or {}) do if (item_attempts['buffitem'..b.name] or 0)>=3 then skipped=true end end
   local status=skipped and 'Force Buff finished; unavailable or failed buffs skipped' or 'Force Buff complete'
   local request=forced_buff.request;forced_buff=nil;buff_plan=nil
   if request~=0 then return {action='force_buff_done',request=request,status=status} end
   return result(nil,status)
  end
 end
 end
 local buff_action=buffs();if buff_action then return buff_action end
 local normal_components=split_peas(option('component_normal',20));if normal_components then return normal_components end
 -- Tell requests use Other spells, never redirect Self spells to the requester.
 local request=s.buff_request
 if request and activity_ready('buff_others') then
  local function finish(message)
   other_job=nil;return {action='buff_request_done',request=request.id,status=message}
  end
  if not option('buffing',true) or not option('buff_others',false) then return finish('Buff request cancelled: buffing is disabled') end
  if not other_job or other_job.id~=request.id then other_job={id=request.id,done={},failed={},attempts={},unavailable={},count=0} end
  local job=other_job
  if s.time>(request.expires or math.huge) then return finish('Buff request expired: '..request.name) end
  if not request.present or request.distance>option('buff_other_range',20) then
   return finish('Buff request cancelled: '..request.name..' is not nearby; continuing to the next request')
  end
  if request.started==false then return {action='buff_request_start',request=request.id,status='Starting buffs for '..request.name} end
  if job.pending then
   local pending=job.pending
   local receipt=cast_receipt(s,pending)
   if not receipt and s.time<pending.deadline then return result(nil,'Waiting for requested buff confirmation') end
   if receipt and (receipt.error or 0)==0 and receipt.confirmed==true then job.done[pending.key]=true;job.count=job.count+1
   elseif (job.attempts[pending.key] or 0)>=3 then job.failed[pending.key]=pending.name end
   job.pending=nil
  end
  local common={1,3,5,7,9,11,37,39,41,51,53,67,69,77,93,95,97,101,103,105,107,109,111,113,115}
  local roles={mage={43,45,47,49,645},heavy={31,593},light={17},unarmed={17},finesse={23},twohanded={593},melee={17,23,31,593},missile={19,677}}
  local wanted={};for _,c in ipairs(common) do wanted[c]=true end
  for _,c in ipairs(roles[request.role] or {}) do wanted[c]=true end
  -- Modern retail uses Light Weapons for unarmed attacks. Every melee role
  -- includes Dual Wield, Shield, Dirty Fighting, Recklessness and Sneak Attack.
  if request.role~='mage' and request.role~='missile' then for _,c in ipairs({668,674,665,671,677}) do wanted[c]=true end end
  -- These are retail item buff families; DAT target masks determine which
  -- of the requester's server-known equipped items can receive each spell.
  for _,c in ipairs({152,154,156,158,160,162,164,166,168,170,172,174,195}) do wanted[c]=true end
  local best,excluded={},{ }
  for _,category in ipairs(p.excluded_buffs or {}) do excluded[category]=true end
  for _,spell in ipairs(p.excluded_buff_spells or {}) do if by_id[spell] then excluded[by_id[spell].category]=true end end
  local _,_,buff_groups=index_spells(s,option('buff_skill_margin',option('skill_margin',30)))
  for _,v in ipairs(buff_groups.other_buffs) do
   if wanted[v.category] and buff_tier(v,p) and v.beneficial and not v.caster_target and not v.self_buff and (v.duration or 0)>0 and s.time>=(job.unavailable[v.id] or 0)
    and band(v.flags,0x2000)==0 and not excluded[v.category] and buff_element_enabled(p,v)
    and (not best[v.category] or v.power>best[v.category].power) then best[v.category]=v end
  end
  local categories={};for c in pairs(wanted) do categories[#categories+1]=c end;table.sort(categories)
  for _,category in ipairs(categories) do
   local v=best[category]
   if not v and not excluded[category] and buff_element_enabled(p,{category=category}) then job.failed['family'..category]='unavailable family '..category
   elseif v then
    job.failed['family'..category]=nil
    local targets={request.player}
    if v.school==3 then
     targets={}
     for _,item in ipairs(request.equipment or {}) do
      if band(item.type,v.target_type or 0)~=0 and (item.count or 1)==1 then targets[#targets+1]=item.id end
     end
     if #targets==0 then job.failed['family'..category]='equipment unavailable for '..(v.name or category) end
    end
    for _,id in ipairs(targets) do
     local key=tostring(id)..':'..category
     if not job.done[key] and not job.failed[key] then
      if mp<option('stop_mana',10) then return finish('Buff request incomplete: mana too low for '..request.name) end
      local equipment=caster();if equipment then return equipment end
      job.attempts[key]=(job.attempts[key] or 0)+1
      job.pending={key=key,spell=v.id,target=id,serial=s.action_serial or 0,started=s.time,name=v.name or tostring(v.id),deadline=s.time+15}
      local intent=cast(s,v,id,'Buffing '..request.name..': '..(v.name or tostring(v.id)))
      last_cast.request=request.id;return intent
     end
    end
   end
  end
  local missing=0;for _ in pairs(job.failed) do missing=missing+1 end
  return finish(string.format('%s: %d buffs confirmed%s',request.name,job.count,missing>0 and (', '..missing..' unavailable or failed (skills, components or visible equipment)') or '; requested set complete'))
 else other_job=nil end
 local craft=craft_work(s,p,'normal');if craft then return craft end
 local fellow=fellowship_work(s,p);if fellow then return fellow end
 local combat=activity_ready('combat') and option('combat','off') or 'off'
 if combat~='off' and not option('manual_combat',false) then combat='auto' end
 activity='pets'
 if option('summon_pets',false) then
  for _,v in ipairs(inventory) do
   if not v.identified and v.usable and pet_allowed(v) and band(v.type,128)~=0
    and (item_attempts['id'..v.id] or 0)<3 and s.time>=(until_time['id'..v.id] or 0) then return assess(v) end
  end
 end
 local refill=refill_pet(s,p,inventory,option,pet_allowed);if refill then return refill end
 if pet_pending then
  local receipt=action_receipt(s,'use',pet_pending.serial,pet_pending.started,pet_pending.item)
  local cooldown=((s.cooldowns or {})[tostring(math.floor(pet_pending.cooldown))] or 0)>0
  if s.owned_pet or cooldown then pet_pending=nil
  elseif (receipt and (receipt.error or 0)~=0) or s.time>=pet_pending.deadline then
   until_time['pet'..pet_pending.item]=s.time+30;pet_pending=nil
  end -- Continue combat/recovery while waiting; do not submit another summon.
 end
 -- A combat loadout retry pauses attacking, not the independent pet activity.
 if activity_ready('pets') and not pet_pending and not pet_refill_pending and option('summon_pets',false) and not s.owned_pet then
   local pet_target,pet_rule,count=nil,nil,0
   local range=option('pet_range_mode',0)==1 and option('pet_range',5) or option('radius',30)
   for _,t in ipairs(s.targets or {}) do
    local rule=monster_rule(t,p)
    if t.distance<range and t.line_of_sight~=false and not rule.ignore and rule.pet_damage_type~=98 and not contains(p.ignore_monsters,t.name) then
     count=count+1
     if not pet_target or (rule.priority or 0)>(pet_rule.priority or 0) or ((rule.priority or 0)==(pet_rule.priority or 0) and t.distance<pet_target.distance) then pet_target=t;pet_rule=rule end
    end
   end
   if pet_target and count>=option('pet_min_targets',1) then
    local best,best_rank,best_level=nil,-1,-1
    local preferred=({[0]=2,4,1,32,64,8,16})[pet_rule.pet_damage_type]
    local attack_element=pet_rule.damage_type or option('damage_type',0)
    if attack_element==0 then
     local score=-1
     for _,v in ipairs(spell_groups.offense) do local e=element(v);local rank=(v.power or 0)*resistance(pet_target,e)
      if e~=0 and not v.beneficial and rank>score then attack_element=e;score=rank end
     end
     if combat~='magic' then for _,v in ipairs(inventory) do if v.equipped and (band(v.type,1)~=0 or band(v.slots,0x800000)~=0) then
      local rank=-1;for _,e in ipairs(damage_elements) do if band(v.damage_type,e)~=0 and resistance(pet_target,e)>rank then attack_element=e;rank=resistance(pet_target,e) end end
     end end end
    end
    for _,v in ipairs(inventory) do local props=v.int_properties or {}
     -- VT's equipment list explicitly opts devices in. A native profile can
     -- instead select device classes through its equipment picker.
     if pet_allowed(v) and pet_eligible(s,v) and s.time>=(until_time['pet'..v.id] or 0) then
      local damage=({[1]=4,[32]=16,[64]=64,[128]=8,[256]=32})[v.icon_effects] or 4
      local rank=resistance(pet_target,damage)
      if damage==preferred then rank=rank+1000000
      elseif (preferred or (pet_rule.pet_damage_type or 101)==101) and damage==attack_element then rank=rank+10000 end
      local level=props['369'] or 0
      if rank>best_rank or (rank==best_rank and level>best_level) then best=v;best_rank=rank;best_level=level end
     end
    end
    if best then
     -- Summoning devices can be used while armed. Keep the current stance;
     -- dropping to peace here unnecessarily removes combat defenses.
     pet_pending={item=best.id,cooldown=(best.int_properties or {})['280'] or 0,started=s.time,serial=s.action_serial or 0,deadline=s.time+15}
     return {action='use_item',item=best.id,status='Summoning with '..best.name}
    end
   end
 end
 if combat=='off' then debuff_pending=nil;debuff_scan=nil;if locked_target then locked_target=nil;return result('cancel_attack','Combat disabled') end end
 local function loot()
 if not activity_ready('loot') then return end
 local learned={}
 for _,spell in ipairs(s.spells or {}) do if spell.known~=false then learned[spell.id]=true end end
 for _,id in ipairs(s.known_spells or {}) do learned[id]=true end
 local present_scrolls={};for _,item in ipairs(inventory) do present_scrolls[item.id]=true end
 for id in pairs(scroll_reads) do if not present_scrolls[id] then scroll_reads[id]=nil end end
 if corpse_pending then
  local receipt=action_receipt(s,'use',corpse_pending.serial,corpse_pending.started,corpse_pending.id)
  if s.container==corpse_pending.id then item_attempts['corpse'..corpse_pending.id]=0;corpse_pending=nil
  elseif s.time<corpse_pending.deadline and not (receipt and (receipt.error or 0)~=0) then return result(nil,'Approaching / waiting for corpse to open')
  else
   local key='corpse'..corpse_pending.id;local blacklisted=(item_attempts[key] or 0)>=option('corpse_attempts',30)
   corpses[corpse_pending.id]=s.time+(blacklisted and option('corpse_blacklist_seconds',200) or 10)
   if blacklisted then item_attempts[key]=0 end
   corpse_pending=nil
  end
 end
 -- A corpse can close/disappear with its final transfer. Confirm ownership
 -- independently of the UI container, before moving on or applying loot jobs.
   if loot_pending then
    local present=false;for _,v in ipairs(s.contents or {}) do if s.container==loot_pending.container and v.id==loot_pending.id and v.count>=loot_pending.count then present=true end end
    local owned,received_exact=0,false
    for _,v in ipairs(inventory) do
     if v.wcid==loot_pending.wcid then owned=owned+(v.count or 1) end
     if v.id==loot_pending.id and (v.count or 1)>=loot_pending.amount then received_exact=true end
    end
    if not received_exact and (present or owned<loot_pending.expected) then
     if s.time<loot_pending.expires then return result(nil,'Waiting for loot transfer') end
     -- A timeout does not prove full packs or lost ownership. Release this
     -- corpse so combat/recovery continue; retry it later without flooding Use.
     return failed('Loot transfer unconfirmed; leaving items untouched')
    end
    if loot_pending.rule then
     confirmed_loot_notice=string.format('Picked up %.0f x %s (rule "%s", %s).',loot_pending.amount,loot_pending.name,loot_pending.rule,loot_pending.after or 'keep')
     loot_pending.rule=nil
    end
    if loot_pending.after and loot_pending.after~='keep' then
     if #loot_jobs>=128 then return failed('Too many pending loot actions; visit a vendor') end
     local received
     for _,i in ipairs(inventory) do if i.wcid==loot_pending.wcid and not loot_pending.before[i.id] and (i.count or 1)==loot_pending.amount then
      if received then return failed('Loot transfer has multiple matching new items; review inventory before continuing') end
      received=i
     end end
     if not received then return failed('Loot was merged or changed; review inventory before applying a destructive rule') end
     if loot_pending.after=='mana_source' then
      mana_sources[received.id]={count=loot_pending.amount,wcid=loot_pending.wcid}
     else loot_jobs[#loot_jobs+1]={id=received.id,count=loot_pending.amount,action=loot_pending.after,wcid=loot_pending.wcid} end
    end
    if s.container~=loot_pending.container then
     corpses[loot_pending.container]=s.time+30
     if loot_pending.empties then completed_corpse=loot_pending.container end
    end
    loot_pending=nil
   end
 local need_mana_source=mana_source_demand(inventory,p,option)>0
 if not loot_full and option('looting',false) and (#(p.loot_rules or {})>0 or option('read_unknown_scrolls',false) or option('fill_mana_stones',false)) then
  if s.container and s.container~=0 then
   -- A player-opened pack/chest must not block combat, recovery or navigation.
   -- Only corpse looting owns the contents of a corpse.
   if s.container_is_corpse==false then corpse_contents_wait=nil;return end
   if s.contents_ready==false then
    if not corpse_contents_wait or corpse_contents_wait.id~=s.container then corpse_contents_wait={id=s.container,deadline=s.time+20} end
    if s.time<corpse_contents_wait.deadline then return result(nil,'Waiting for corpse item descriptions') end
    corpses[s.container]=s.time+30;corpse_contents_wait=nil
    return {action='close_corpse',status='Corpse contents incomplete; retrying later'}
   end
   corpse_contents_wait=nil
   -- Charge actual work, not all the conditions/spells a rule might examine.
   -- Stable ordering prevents snapshot enumeration changes restarting the scan.
   local rules=p.loot_rules or {};local contents=s.contents or {}
   table.sort(contents,function(a,b)return a.id<b.id end)
   local signature=tostring(s.container)..':'..#contents..':'..#rules..':'..(p.vt_revision or 0)..':'..(p.loot_revision or 0)
   for _,v in ipairs(contents) do signature=signature..':'..v.id..':'..v.count..':'..tostring(v.identified) end
   if loot_scan.signature~=signature then loot_scan={signature=signature,item=1,rule=1} end
   local owned,owned_names={},{};for _,v in ipairs(inventory) do owned[v.wcid]=(owned[v.wcid] or 0)+(v.count or 1);local name=v.name or '';owned_names[name]=(owned_names[name] or 0)+(v.count or 1) end
   local carried_scrolls={}
   if option('read_unknown_scrolls',false) then
    for _,v in ipairs(inventory) do if v.object_class==42 and (v.spell or 0)>0 then carried_scrolls[v.spell]=true end end
   end
   local pending_appraisals=0
   for _,item in ipairs(contents) do if not item.identified and s.time<(until_time['id'..item.id] or 0) then pending_appraisals=pending_appraisals+1 end end
   local function progress()
    local r=result(nil,'Checking loot rules: item '..loot_scan.item..'/'..#contents..', rule '..loot_scan.rule..'/'..#rules)
    r.continue_work=true;return r
   end
   while loot_scan.item<=#contents do
    local item=contents[loot_scan.item]
    local item_rules=rules;local resolved=true
    -- VT considers learnable unknown scrolls before the user's loot rules.
    -- Carried copies suppress duplicates while a new scroll waits to be read.
    if option('read_unknown_scrolls',false) and item.object_class==42 and item.scroll_can_learn and (item.spell or 0)>0 and not learned[item.spell] and not carried_scrolls[item.spell] then
     item_rules={{action='read',label='Learn unknown spell'}}
    end
    if band(item.type,0x80000)~=0 and (p.mana_stone_loot_count or 0)>0 then
     for _,a in ipairs(p.assist_items or {}) do if a.type==8 and a.name==item.name then
      item_rules={{action='keep',name=item.name,name_mode='exact',keep_up_to=p.mana_stone_loot_count,count_by_name=true,label='Mana stone supplies'}};break
     end end
    end
    while loot_scan.rule<=#item_rules do
     local rule=item_rules[loot_scan.rule]
     if not workavailable() then return progress() end
     loot_scan.evaluation=loot_scan.evaluation or {}
     local match=rule_match(rule,item,s,loot_scan.evaluation)
     if match=='pending' then return progress() end
     loot_scan.evaluation=nil
     if match==nil then
      local key='id'..item.id
      if (item_attempts[key] or 0)>=3 and s.time>=(until_time[key] or 0) then return failed('Unable to appraise loot; check appraisal skills') end
      -- VT's ID queue advances while earlier replies are pending. Preserve
      -- first-match rule order for this item, but keep examining other items.
      loot_scan.waiting=true;resolved=false
      if pending_appraisals<4 and s.time>=(until_time[key] or 0) then
       item_attempts[key]=(item_attempts[key] or 0)+1;until_time[key]=s.time+10
       loot_scan.item=loot_scan.item+1;loot_scan.rule=1
       return {action='identify',item=item.id,status='Inspecting '..item.name..' for loot rule'}
      end
      break
     elseif match then
      -- First match wins, including Skip and a satisfied quantity limit.
      local have=rule.count_by_name and (owned_names[item.name] or 0) or (owned[item.wcid] or 0)
      local amount=rule.keep_up_to and rule.keep_up_to>0 and math.min(item.count,math.max(0,rule.keep_up_to-have)) or item.count
      if rule.action~='skip' and amount>0 then
       local before={};for _,v in ipairs(inventory) do before[v.id]=true end
       loot_scan={};loot_pending={container=s.container,empties=#contents==1 and amount==item.count,before=before,amount=amount,id=item.id,count=item.count,wcid=item.wcid,expected=(owned[item.wcid] or 0)+amount,expires=s.time+10,after=rule.action,name=item.name or 'Item',rule=rule.label or 'Matching rule'}
       return {action='loot',item=item.id,amount=amount,status='Looting '..item.name..' ('..(rule.label or 'matching rule')..')'}
      end
      break
     end
     loot_scan.rule=loot_scan.rule+1
    end
    -- NoLoot/Skip and satisfied KeepUpTo rules are mana-donor candidates in
    -- VT, after normal loot and scroll handling. One junk item per empty stone.
    if resolved and need_mana_source and band(item.icon_effects or (item.int_properties or {})['218103824'],1)~=0
     and not item.retained and not item.equipped and band(item.type,0x80200)==0 then
     if not item.identified then
      local key='id'..item.id;loot_scan.waiting=true
      if (item_attempts[key] or 0)>=3 and s.time>=(until_time[key] or 0) then return failed('Unable to appraise mana-source loot') end
      if pending_appraisals<4 and s.time>=(until_time[key] or 0) then
       item_attempts[key]=(item_attempts[key] or 0)+1;until_time[key]=s.time+10
       loot_scan.item=loot_scan.item+1;loot_scan.rule=1
       return {action='identify',item=item.id,status='Inspecting '..item.name..' for mana stone filling'}
      end
     elseif mana_source_candidate(item,p,option) then
      local before={};for _,v in ipairs(inventory) do before[v.id]=true end
      loot_scan={};loot_pending={container=s.container,empties=#contents==1 and item.count==1,before=before,amount=1,id=item.id,count=item.count,wcid=item.wcid,expected=(owned[item.wcid] or 0)+1,expires=s.time+10,after='mana_source'}
      return {action='loot',item=item.id,amount=1,status='Looting '..item.name..' to fill an empty mana stone'}
     end
    end
    loot_scan.item=loot_scan.item+1;loot_scan.rule=1
   end
   local waiting=loot_scan.waiting;loot_scan={}
   if waiting then return result(nil,'Waiting for loot appraisal') end
   corpses[s.container]=s.time+300;return {action='close_corpse',completed=true,continue_work=true,status='Loot complete'}
  end
 end
 -- Only the explicit unknown-scroll option authorizes reading carried scrolls.
 -- This also recovers a pickup interrupted by a busy reply, restart or logout.
 -- Imported Read/Salvage/Sell rules alone never act on existing possessions.
 if option('looting',false) and option('read_unknown_scrolls',false) and (not s.container or s.container==0) then
  local queued={};for _,job in ipairs(loot_jobs) do queued[job.id]=true end
  for _,item in ipairs(inventory) do
   local retry=scroll_reads[item.id]
   if item.object_class==42 and item.scroll_can_learn and (item.spell or 0)>0 and not learned[item.spell]
    and not item.equipped and not item.retained and item.resource_available~=false and not queued[item.id]
    and (not retry or (retry.attempts<3 and s.time>=retry.after)) and #loot_jobs<128 then
    loot_jobs[#loot_jobs+1]={id=item.id,count=item.count or 1,action='read',wcid=item.wcid}
    queued[item.id]=true
   end
  end
 end
 if loot_job_pending then
  local job=loot_job_pending;local exists=false;for _,i in ipairs(inventory) do if i.id==job.id then exists=true end end
  local receipt=job.spell and action_receipt(s,'use',job.serial,job.started,job.id)
  local rejected=inventory_failure(s,job,{[job.id]=true}) or (receipt and (receipt.error or 0)~=0)
  if job.spell and receipt and not s.busy then job.completed_at=job.completed_at or s.time end
  if not exists or (job.spell and learned[job.spell]) then table.remove(loot_jobs,job.index);loot_job_pending=nil
  elseif job.spell and (rejected or s.time>=job.deadline or (job.completed_at and s.time>=job.completed_at+1)) then
   -- Scroll Use is safe to retry after completion: the server rejects known
   -- spells before consuming a second copy. Never infer learning from UseDone.
   local retry=scroll_reads[job.id];retry.after=s.time+2*retry.attempts
   loot_jobs[job.index].retry_after=retry.after;loot_job_pending=nil
   if retry.attempts>=3 then
    table.remove(loot_jobs,job.index)
    return failed('Scroll reading was not confirmed after three attempts; keeping the scroll and continuing other activities')
   end
   return result(nil,'Scroll reading was not confirmed; retrying after the current action')
  elseif rejected then return failed('Loot '..job.action..' was rejected by the server')
  elseif s.time>=job.deadline then return failed('Loot '..job.action..' did not complete; item remains in inventory')
  else return result(nil,'Waiting for loot '..job.action) end
 end
 if not s.container or s.container==0 then
  for index,job in ipairs(loot_jobs) do if (job.action~='sell' or (s.vendor or 0)~=0) and s.time>=(job.retry_after or 0) then
   local item;for _,i in ipairs(inventory) do if i.id==job.id and i.wcid==job.wcid then item=i;break end end
   if not item then table.remove(loot_jobs,index);return loot_progress('Queued loot item is no longer present') end
   if item.equipped or (item.count or 1)~=job.count then return failed('Queued loot item was equipped or its stack changed; review loot rules') end
   local scroll_spell=job.action=='read' and (item.spell or 0)>0 and item.spell or nil
   if scroll_spell and (learned[scroll_spell] or item.scroll_can_learn==false) then
    table.remove(loot_jobs,index);return loot_progress('Keeping known or currently unlearnable scroll: '..item.name)
   end
   local tool
   if job.action=='salvage' then
    if not salvage_candidate(item) or salvage_protected(item) then table.remove(loot_jobs,index);return loot_progress('Keeping protected or unsalvageable item: '..item.name) end
    if not item.identified then return assess(item) end
    for _,i in ipairs(inventory) do if (i.salvage_tool_available==true or (i.salvage_tool_available==nil and i.object_class==40 and i.resource_available~=false)) then tool=i.id;break end end
    if not tool then return failed('Salvage rule requires an Ust') end
   end
   if scroll_spell then
    local retry=scroll_reads[item.id] or {attempts=0,after=0};retry.attempts=retry.attempts+1;scroll_reads[item.id]=retry
   end
   loot_job_pending={index=index,id=item.id,action=job.action,spell=scroll_spell,started=s.time,failure_serial=s.inventory_failure_serial or 0,serial=s.action_serial or 0,deadline=s.time+(scroll_spell and 45 or 15)}
   return {action=job.action,item=item.id,tool=tool,status=job.action..': '..item.name}
  end end
 end
 if option('salvage_inventory',false) and (not s.container or s.container==0) then
  local scan=inventory_salvage_scan;local rules=p.loot_rules or {}
  local revision=tostring(s.inventory_revision)..':'..tostring(s.appraisal_revision)..':'..(p.loot_revision or 0)..':'..(p.vt_revision or 0)
  if scan.revision~=revision then scan={revision=revision,item=1,rule=1};inventory_salvage_scan=scan end
  if not scan.done then
  local has_salvage=false;for _,rule in ipairs(rules) do if rule.enabled~=false and rule.action=='salvage' then has_salvage=true;break end end
  if not has_salvage then scan.done=true else
  local ordered,present={},{};for _,v in ipairs(inventory) do ordered[#ordered+1]=v;present[v.id]=true end;table.sort(ordered,function(a,b)return a.id<b.id end)
  for id in pairs(inventory_salvage_blocked) do if not present[id] then inventory_salvage_blocked[id]=nil end end
  local function progress() local r=result(nil,'Checking inventory salvage rules: item '..scan.item..'/'..#ordered);r.continue_work=true;return r end
  while scan.item<=#ordered do
   local item=ordered[scan.item]
   if salvage_candidate(item) and not inventory_salvage_blocked[item.id] then
    if not item.identified then
     if (item_attempts['id'..item.id] or 0)>=3 then inventory_salvage_blocked[item.id]=true
     else return assess(item) end
    elseif not salvage_protected(item) then
     while scan.rule<=#rules do
      if not workavailable() then return progress() end
      scan.evaluation=scan.evaluation or {};local match=rule_match(rules[scan.rule],item,s,scan.evaluation)
      if match=='pending' then return progress() end
      scan.evaluation=nil
      if match==nil then inventory_salvage_blocked[item.id]=true;break end
      if match then
       if rules[scan.rule].action=='salvage' then
        -- Freeze identity and count; execute through the same validated queue
        -- as newly looted salvage, never Sell/Read a pre-existing possession.
        inventory_salvage_blocked[item.id]=true
        loot_jobs[#loot_jobs+1]={id=item.id,count=item.count or 1,action='salvage',wcid=item.wcid}
        scan.item=scan.item+1;scan.rule=1
        return loot_progress('Queued inventory salvage: '..item.name)
       end
       break -- First match wins, including Keep, Skip and disabled quantities.
      end
      scan.rule=scan.rule+1
     end
    end
   end
   scan.item=scan.item+1;scan.rule=1
  end
  scan.done=true
  end
  end
 else inventory_salvage_scan={} end
 if not loot_full and option('looting',false) and (#(p.loot_rules or {})>0 or option('read_unknown_scrolls',false) or need_mana_source) then
  for _,c in ipairs(s.corpses or {}) do if not c.looted and c.line_of_sight~=false and c.distance<=option('loot_range',15) and s.time>=(corpses[c.id] or 0) then
   if (c.ownership_available or option('loot_only_rare',false)) and not c.identified then return assess(c) end
   local permitted=true
   if c.ownership_available then
    local name=corpse_owner_name((s.char_strings or {})['1']);local killer=corpse_owner_name(c.killer)
    permitted=killer==name and name~=''
    if not permitted and not c.rare then
     local fellow
     for _,m in ipairs((s.fellowship or {}).members or {}) do if killer~='' and corpse_owner_name(m.name)==killer then fellow=m;break end end
     if fellow then permitted=option('loot_fellow_corpses',false) and (fellow.share_loot or (c.observed_age or 0)>=100)
     else permitted=option('loot_all_corpses',false) and (c.observed_age or 0)>=100 end
    end
   end
   if permitted and (not option('loot_only_rare',false) or c.rare) then
   item_attempts['corpse'..c.id]=(item_attempts['corpse'..c.id] or 0)+1
   corpse_pending={id=c.id,started=s.time,deadline=s.time+20,serial=s.action_serial or 0};return {action='open_corpse',item=c.id,status='Opening '..c.name} end end end
 end
 end
 -- VT cLogic puts corpse approach and loot actions before attack only with
 -- LootPriorityBoost. An open corpse or pending receipt must not enable it.
 -- Keep loot_pending intact across combat so the server transfer is reconciled
 -- once, rather than requesting the item again or losing its follow-up rule.
 -- Idle peace is a precondition of ordinary loot in VT, not an activity that
 -- must wait until all corpses are emptied. Nearby living monsters retain
 -- combat stance even when combat is disabled or loot priority is enabled.
 local idle=idle_stance();if idle then return idle end
 if option('loot_priority',false) then local action=loot();if action then return action end end
 if option('nav_priority',false) then
  -- Keep clear-area looting from being starved by a continuous route, without
  -- implicitly enabling LootPriorityBoost while a combat target is present.
  if not target or combat=='off' then local action=loot();if action then return action end end
  local nav=navigate();if nav then return nav end
 end
 activity='combat'
 local attack_spells,combat_inventory,best_debuffs,attack_indices,ammo_by_kind,proc_ammo={},{},{},{},{},{}
 local debuff_by_skill=option('debuff_selection',2)==2
 local debuff_first,debuff_second=debuff_by_skill and 'skill' or 'power',debuff_by_skill and 'power' or 'skill'
 local function choose_debuff(v,item,kind)
  if v.beneficial or v.caster_target or (v.duration or 0)<=0 then return end
  local old=best_debuffs[v.category]
  local first,second=debuff_first,debuff_second
  if old then
   if (v[first] or 0)<(old[first] or 0) then return end
   if (v[first] or 0)==(old[first] or 0) then
    if (v[second] or 0)<(old[second] or 0) then return end
    -- VT dz resolves equal quality/skill in favor of learned spells. Among
    -- items, avoid a needless swap or prefer the smallest grenade stack.
    if (v[second] or 0)==(old[second] or 0) then
     if not old.item or (item and (old.item.equipped or (kind=='grenade' and (old.item.count or 1)<=(item.count or 1)))) then return end
    end
   end
  end
  if not item then best_debuffs[v.category]=v;return end
  local candidate={};for k,value in pairs(v) do candidate[k]=value end
  candidate.item=item;candidate.source_kind=kind;best_debuffs[v.category]=candidate
 end
 if target and combat~='off' then
 for _,v in ipairs(spell_groups.offense) do
  if element(v)~=0 and not v.beneficial and v.category~=636 and v.category~=637 and v.category~=638 then
   local key=tostring(v.category)..':'..tostring(band(v.flags,0x1000))
   -- Keep other elements available for monster overrides, while preserving
   -- an explicit lower-tier spell within its own family.
    local index=attack_indices[key]
    if not index then attack_spells[#attack_spells+1]=v;attack_indices[key]=#attack_spells
    elseif v.id==p.attack_spell or (attack_spells[index].id~=p.attack_spell and v.power>attack_spells[index].power) then attack_spells[index]=v end
  else choose_debuff(v) end
 end
 for _,v in ipairs(inventory) do
  if band(v.type,0x8101)~=0 or band(v.slots,0x800000)~=0 then combat_inventory[#combat_inventory+1]=v end
  if band(v.slots,0x800000)~=0 then local kind=v.ammo_type or 0;ammo_by_kind[kind]=ammo_by_kind[kind] or {};table.insert(ammo_by_kind[kind],v) end
  if v.innate_spell_available and v.innate_spell and v.can_wield and weapon_allowed(v) then choose_debuff(v.innate_spell,v,'wand') end
 end
 -- VT ga/dz also equips physical proc weapons and attacks until the debuff
 -- lands. A launcher is eligible only with usable ammunition of its own kind.
 for _,v in ipairs(combat_inventory) do
  if v.proc_debuff_available and v.proc_debuff and v.can_wield and not v.auto_wield_left and weapon_allowed(v) then
   local ammo,score=nil,-1
   for _,a in ipairs(ammo_by_kind[v.ammo_type] or {}) do
    if equipment_known(a) and a.can_wield and (a.count or 1)>0 and a.resource_available~=false then
     local value=(a.damage or 0)*(1-(a.variance or 0)*.5)
     if value>score or value==score and a.equipped then ammo=a;score=value end
    end
   end
   if (v.ammo_type or 0)==0 or ammo then
    proc_ammo[v.id]=ammo;choose_debuff(v.proc_debuff,v,'proc')
   end
  end
 end
 local owned={};for _,v in ipairs(inventory) do owned[v.id]=v end
 for _,choice in ipairs(s.grenade_debuffs or {}) do local item=owned[choice.item];if item and choice.spell then choose_debuff(choice.spell,item,'grenade') end end
 end
 local effect_index,active_vulnerabilities={},{}
 for _,e in ipairs(s.debuffs or {}) do
 if e.expires-s.time>.5 then
  local active=active_vulnerabilities[e.target] or {};active_vulnerabilities[e.target]=active;active[e.category]=true
 end
 local refresh=(e.category==636 or e.category==637 or e.category==638) and 0 or option('debuff_refresh',5)
 if e.expires-s.time>refresh then
  local effects=effect_index[e.target] or {};effect_index[e.target]=effects
  effects[e.category]=math.max(effects[e.category] or 0,e.power)
 end end
 local vulns={[32]=102,[4]=104,[8]=106,[64]=108,[16]=110,[2]=112,[1]=114}
 local function wanted(rule,key) if rule and rule[key]~=nil then return rule[key] end;return option(key,false) end
 local function debuff_categories(rule,element)
  local categories={}
  if wanted(rule,'debuff_yield') then categories[#categories+1]=42 end
  if wanted(rule,'debuff_weakening') then categories[#categories+1]=642 end
  if wanted(rule,'debuff_festering') then categories[#categories+1]=643 end
  if wanted(rule,'debuff_corruption') then categories[#categories+1]=638 end
  if wanted(rule,'debuff_destructive') then categories[#categories+1]=636 end
  if wanted(rule,'debuff_corrosion') then categories[#categories+1]=637 end
  if wanted(rule,'debuff_imperil') then categories[#categories+1]=116 end
  if wanted(rule,'debuff_vuln') and vulns[element] then categories[#categories+1]=vulns[element] end
  local secondary=rule and vulns[rule.secondary_vuln]
  if secondary and (not wanted(rule,'debuff_vuln') or secondary~=vulns[element]) then categories[#categories+1]=secondary end
  if wanted(rule,'debuff_gravity') then categories[#categories+1]=78 end
  if wanted(rule,'debuff_broadside') then categories[#categories+1]=40 end
  if wanted(rule,'debuff_fester') then categories[#categories+1]=96 end
  return categories
 end
 local function missing_debuff(target,categories)
  local confirmed=effect_index[target.id] or {}
  for _,category in ipairs(categories) do
   local spell=best_debuffs[category]
   if (not spell and not option('debuff_fallback',false)) or (spell and (confirmed[category] or 0)<spell.power and (not option('debuff_fallback',false) or (item_attempts['debuff'..target.id..':'..category] or 0)<3)) then return true end
  end
  return false
 end
 local function combat_plan(target,target_rule)
  local target_combat=target_rule and target_rule.combat or combat
  local requested=target_rule and target_rule.weapon
  local explicit
  if requested and requested>0 then for _,v in ipairs(combat_inventory) do if v.id==requested and not v.auto_wield_left then explicit=v;break end end end
  -- VT falls back to automatic selection when an explicit object is no longer
  -- owned. Zero means automatic casting equipment, not the first melee item.
  if requested==0 then target_combat='magic' end
  local damage_override=target_rule and target_rule.damage_type or p.damage_type
  if damage_override==128 then damage_override=1024 end -- previous UCM Nether setting
  local strict_damage=damage_override and damage_override~=0
  local selected_spell=p.attack_spell
  if target_rule and (target_rule.damage_type or 0)~=0 then
   local selected=by_id[selected_spell]
   if not selected or element(selected)~=damage_override then selected_spell=nil end
  end
  local spell_plans={}
  local function spell_plan(requested_element)
   local key=requested_element or 0
   if spell_plans[key] then return spell_plans[key] end
   local damage_override=requested_element
   local attack,best_score=nil,-1
   local ring,ring_score,streak_spell,streak_score=nil,-1,nil,-1
   local arc,arc_score=nil,-1
   local use_ring=ring_enabled(target_rule)
   -- Void damage-over-time spells are attacks in their own right. Requiring a
   -- bolt/streak first rejected the wand before the enabled curse pass could run.
   local dot_attack,dot_score,dot_skill=nil,-1,-1
   if not damage_override or damage_override==0 or damage_override==1024 then
    for _,family in ipairs({{636,'debuff_destructive'},{637,'debuff_corrosion'},{638,'debuff_corruption'}}) do
     local v=best_debuffs[family[1]]
     if v and wanted(target_rule,family[2]) then
      local rank=trained_skill(s,combat_school_skill[v.school]);local score=v.power*resistance(target,1024)
      if rank>dot_skill or (rank==dot_skill and score>dot_score) then dot_attack=v;dot_score=score;dot_skill=rank end
     end
    end
   end
   local magic_skill=-1
   if target_combat=='auto' then
    magic_skill=dot_skill
    for _,v in ipairs(attack_spells) do
     local e=element(v)
     if e~=0 and not v.beneficial and (not damage_override or damage_override==0 or damage_override==e)
      and (not selected_spell or selected_spell==0 or selected_spell==v.id) then magic_skill=math.max(magic_skill,trained_skill(s,combat_school_skill[v.school])) end
    end
   end
   if not damage_override or damage_override==0 then
    local preference,score=-1,-1
    for _,v in ipairs(attack_spells) do local e=element(v)
     local is_ring=(v.category>=222 and v.category<=228) or v.category==641
     local ring_usable=use_ring and (ring_count>=option('ring_min_targets',4) or (target_rule and target_rule.bolt==false and target_rule.streak==false and ring_count>0))
     if (not is_ring or ring_usable) and (not selected_spell or selected_spell==0 or selected_spell==v.id) and trained_skill(s,combat_school_skill[v.school])>=magic_skill then
      local rank=element_preference(target,e);local value=v.power*resistance(target,e)
      if rank>preference or (rank==preference and value>score) then damage_override=e;preference=rank;score=value end
     end
    end
   end
   local bolt_enabled=not target_rule or target_rule.bolt~=false
   local streak_enabled=not target_rule or target_rule.streak~=false
   local bolt_fallback=bolt_enabled or streak_enabled or use_ring
   for _,v in ipairs(attack_spells) do local e=element(v)
    local streak=(v.category>=243 and v.category<=249) or v.category==639
    local is_ring=(v.category>=222 and v.category<=228) or v.category==641
    if e~=0 and not v.beneficial and (not selected_spell or selected_spell==0 or selected_spell==v.id) and trained_skill(s,combat_school_skill[v.school])>=magic_skill then
     local score=v.power*resistance(target,e);if damage_override and damage_override~=0 and e~=damage_override then score=-1 end
     if is_ring then if use_ring and score>ring_score then ring=v;ring_score=score end
     elseif streak then if streak_enabled and score>streak_score then streak_spell=v;streak_score=score end
     elseif band(v.flags,0x1000)~=0 then if bolt_fallback and score>arc_score then arc=v;arc_score=score end
     elseif bolt_fallback and score>best_score then attack=v;best_score=score end
    end
   end
   -- VT chooses the higher-quality eligible family first. The arc preference
   -- breaks ties; it does not discard the only usable family.
   local arc_mode=option('use_arcs',1)
   if arc and (arc_score>best_score or (arc_score==best_score and (arc_mode==3 or (arc_mode==2 and target.distance>=option('arc_range',5))))) then attack=arc;best_score=arc_score end
   -- VT keeps ordinary spells as fallback if a requested streak/ring is not
   -- usable. Ring-only rules cast with one nearby eligible target, otherwise
   -- the configured minimum applies (counted only from ring-enabled rules).
   local health=spell_health[target.id] or {}
   local finishing=streak_spell and health.hp and health.hp>0
    and health.hp<math.floor(((health.damage or 0)>0 and health.damage or streak_spell.power)/7)
   if streak_spell and (not bolt_enabled or not attack or finishing) then attack=streak_spell;best_score=streak_score end
   if ring and (ring_count>=option('ring_min_targets',4) or (not bolt_enabled and not streak_enabled and ring_count>0)) then attack=ring;best_score=ring_score end
   local casting_attack=attack or dot_attack
   local casting_score=attack and best_score or dot_score
   local value={attack=attack,dot=dot_attack,score=casting_score,casting=casting_attack}
   spell_plans[key]=value;return value
  end
  local initial_spells=spell_plan(damage_override)
  local attack,dot_attack,casting_attack=initial_spells.attack,initial_spells.dot,initial_spells.casting
  local weapon,best,best_spells,best_ammo,best_element
  local usable_caster=false
  local unassessed,unassessed_ammo
  local blocked,blocked_reason
  local function rejected(v,reason)
   if not blocked or (explicit and v.id==explicit.id) then blocked=v;blocked_reason=reason end
  end
  local function better(a,b)
   if not b then return true end
   for i=1,#a do if a[i]~=b[i] then return a[i]>b[i] end end
   return false
  end
  local function tier(v,e,rend,bonus,launcher,caster)
   local species=target.creature_type or ((p.monster_catalog or {})[target.name] or {}).species
   if (v.slayer or 0)>0 and v.slayer==species then return 8 end
   local preference=element_preference(target,e)
   if strict_damage then preference=e==damage_override and 1 or 0 end
   local confirmed=(active_vulnerabilities[target.id] or {})[vulns[e]]
   if bonus>0 and preference>0 and confirmed then return 7 end
   if rend~=0 and rend==e and preference>0 then return 6 end
   if caster and e==1024 then return 5 end
   if bonus>0 and preference>0 then return 4 end
   if caster and bonus>0 and (v.damage_type or 0)==0 then return 3 end
   if preference>0 then return 2 end
   if launcher then return 1 end
   return 0
  end
  -- One ammunition choice belongs to the entire loadout. Reusing it for
  -- equip/debuff/attack avoids independently choosing a different element later.
  local ammo_cache={}
  local function ammunition(v,requested)
   local base_damage=(v.damage_type or 0)~=0 and v.damage_type or 2
   local key=tostring(v.ammo_type)..':'..tostring(requested or 0)..':'..tostring(base_damage)
   if ammo_cache[key] then return table.unpack(ammo_cache[key]) end
   local best_ammo,best_rank,best_e,unknown
   for _,a in ipairs(ammo_by_kind[v.ammo_type] or {}) do
    if not equipment_known(a) then unknown=a
    elseif a.can_wield then
     -- Prismatic (retail Base bit) inherits the launcher's damage, not the
     -- monster's preferred element. A neutral bow still fires piercing prisms.
     local mask=a.damage_type==0x10000000 and base_damage or a.damage_type
     if not requested or requested==0 or band(mask,requested)~=0 then
      local e=requested and requested~=0 and requested or preferred_damage(target,mask)
      local rank={element_preference(target,e),(a.damage or 0)*resistance(target,e),a.equipped and 1 or 0}
      if better(rank,best_rank) then best_ammo=a;best_rank=rank;best_e=e end
     end
    end
   end
   ammo_cache[key]={best_ammo or false,best_e or 0,unknown or false}
   return best_ammo,best_e,unknown
  end
  for _,v in ipairs(combat_inventory) do
   local melee=band(v.type,1)~=0;local missile=band(v.type,0x100)~=0 and band(v.slots,0x800000)==0;local caster=band(v.type,0x8000)~=0
   local allowed=explicit and v.id==explicit.id or (not explicit or strict_damage) and weapon_allowed(v)
   local mode_allowed=target_combat=='auto' or (target_combat=='melee' and melee) or (target_combat=='missile' and missile) or (target_combat=='magic' and caster)
   local main_hand=option('melee_weapon',-1)
   if melee then
    if not explicit and main_hand>0 and not strict_damage then allowed=v.id==main_hand end
    if v.auto_wield_left then allowed=false end
   end
   if not caster and target_rule and target_rule.bolt==false then allowed=false end
   if allowed and not mode_allowed then
    if explicit and v.id==explicit.id then rejected(v,'selected weapon does not match '..target_combat..' mode') end
    allowed=false
   end
   if caster and allowed and equipment_known(v) and v.can_wield then usable_caster=true end
   if not equipment_known(v) and allowed and (melee or missile or caster) then unassessed=v end
   if equipment_known(v) and allowed and not v.can_wield then rejected(v,v.wield_reason or 'wield requirements are not met or unavailable') end
   if equipment_known(v) and v.can_wield and allowed and (melee or missile or caster) then
    local damage=v.damage_type or 0
    local rend,bonus=weapon_traits(v)
    local launcher=missile and (v.ammo_type or 0)~=0
    local e=strict_damage and damage_override or rend~=0 and rend or preferred_damage(target,damage)
    if not strict_damage and not caster and not launcher and band(damage,e)==0 then e=preferred_damage(target,damage) end
    local spells=initial_spells
    local ammo,unknown
    if caster then
     if not strict_damage and rend==0 and damage==0 then e=casting_attack and element(casting_attack) or 0 end
     -- A deliberately selected spell remains authoritative. Otherwise an
     -- elemental wand must select its corresponding spell, never an unrelated
     -- highest-tier bolt chosen before the wand was considered.
     if not strict_damage and selected_spell and selected_spell~=0 and by_id[selected_spell] then e=element(by_id[selected_spell]) end
     spells=spell_plan(e)
     if not spells.casting and initial_spells.casting then
      spells=initial_spells;e=element(spells.casting)
     end
    elseif launcher then
     local requested=strict_damage and damage_override or rend~=0 and rend or nil
     ammo,e,unknown=ammunition(v,requested)
     unassessed_ammo=unknown or unassessed_ammo
    end
    local resist=resistance(target,e)
    local wrong_caster_element=caster and strict_damage and damage~=0 and band(damage,damage_override)==0 and rend~=damage_override
    local value=caster and spells.score or ((v.damage or 0)*(1-(v.variance or 0)*.5)*math.max(1,v.damage_mod or 1)*resist/(1+(v.weapon_time or 0)/100))
    if caster then
     if not spells.casting then value=-1
     elseif wrong_caster_element then value=-1
     elseif band(damage,e)~=0 then value=value*math.max(1,v.element_mod or 1) end
    elseif launcher then
     value=ammo and (ammo.damage or 0)*resist*math.max(1,v.damage_mod or 1)/(1+(v.weapon_time or 0)/100) or -1
    elseif strict_damage and band(damage,damage_override)==0 then value=-1 end
    if value<0 then rejected(v,wrong_caster_element and 'casting device does not match the selected damage type'
      or caster and 'no usable attack spell for the selected element and spell settings'
      or launcher and 'no eligible ammunition for the selected damage type' or 'weapon does not match the selected damage type') end
    if (v.slayer or 0)>0 and v.slayer==target.creature_type then value=value*math.max(1,v.slayer_mod or 1) end
    local skill_id=caster and spells.casting and combat_school_skill[spells.casting.school] or v.weapon_skill
    local skill=target_combat=='auto' and trained_skill(s,skill_id) or 0
    local requested=explicit and v.id==explicit.id or not explicit and melee and main_hand>0 and v.id==main_hand or false
    local rank={requested and 1 or 0,tier(v,e,rend,bonus,launcher,caster),element_preference(target,e),bonus,skill,value,
     v.equipped and band(v.equipped_slot,0x200000)==0 and 1 or 0}
    if value>=0 and better(rank,best) then weapon=v;best=rank;best_spells=spells;best_ammo=ammo;best_element=e end
   end
  end
  if best_spells then attack=best_spells.attack;dot_attack=best_spells.dot;casting_attack=best_spells.casting end
  local mode=weapon and (band(weapon.type,0x8000)~=0 and 'magic' or band(weapon.type,0x100)~=0 and 'missile' or 'melee') or target_combat
  local debuff_element=best_element or damage_override
  local categories=debuff_categories(target_rule,debuff_element)
  local confirmed=effect_index[target.id] or {}
  local needs=missing_debuff(target,categories)
  return {attack=attack,dot_only=not attack and dot_attack~=nil,missing_attack=usable_caster and not casting_attack and (target_combat=='auto' or target_combat=='magic'),weapon=weapon,unassessed=unassessed,unassessed_ammo=unassessed_ammo,mode=mode,categories=categories,confirmed=confirmed,needs=needs,damage=damage_override,attack_element=best_element,ammo=best_ammo,priority=tier,
   blocked_reason=blocked and ((blocked.name or tostring(blocked.id))..': '..blocked_reason) or nil}
 end
 -- Keep an in-flight debuff on its recipient until the server answers. A new
 -- higher-priority spawn must not make UseDone look like successful debuffing.
 if debuff_pending then local q=debuff_pending
  local recipient_present=s.targets==nil
  for _,candidate in ipairs(s.targets or {}) do if candidate.id==q.target then recipient_present=true;break end end
  local reported=(s.debuff_revision or 0)~=q.revision and s.debuff_target==q.target and s.debuff_spell==q.spell
  local receipt;if q.item then receipt=action_receipt(s,'use',q.serial,q.started,q.item,q.target) elseif not q.physical then receipt=cast_receipt(s,q) end
  local failed=receipt and (receipt.error or 0)~=0
  local confirmed=false;for _,e in ipairs(s.debuffs or {}) do if e.target==q.target and e.category==q.category and e.power>=q.power and e.expires>s.time then confirmed=true;break end end
  if not recipient_present or reported or failed or confirmed or s.time>=q.deadline then debuff_pending=nil
   if q.physical then return result('cancel_attack','Debuff weapon attempt finished') end
  else return result(nil,'Waiting for debuff confirmation') end
 end
 local debuff_mode=option('debuff_each_first',1)
 if target and combat~='off' and debuff_mode>1 then
  -- Large crowds are evaluated in bounded slices. Spells are compacted by
  -- family above, and every slice uses current positions and server effects.
  local candidates={}
  for _,candidate in ipairs(target_candidates) do if debuff_mode==3 or candidate.rank==priority then
   candidates[#candidates+1]=candidate
  end end
  table.sort(candidates,function(a,b)
   if a.target.id==b.target.id then return false end
   if a.rank~=b.rank then return a.rank>b.rank end
   if nearer(a.target,b.target) then return true elseif nearer(b.target,a.target) then return false end
   return a.target.id<b.target.id
  end)
  local signature=tostring(debuff_mode)..':'..priority
  for _,candidate in ipairs(candidates) do signature=signature..':'..candidate.target.id end
  if not debuff_scan or debuff_scan.signature~=signature then debuff_scan={signature=signature,index=1} end
  local work=0
  while debuff_scan.index<=#candidates do
   local candidate=candidates[debuff_scan.index]
   local element=candidate.rule and candidate.rule.damage_type or p.damage_type
   local dynamic=wanted(candidate.rule,'debuff_vuln') and (not element or element==0)
   -- Fixed debuff families do not depend on choosing a weapon. Index confirmed
   -- effects once, and only calculate a loadout for an automatic vulnerability.
   local needs=dynamic and combat_plan(candidate.target,candidate.rule).needs or missing_debuff(candidate.target,debuff_categories(candidate.rule,element))
   if needs then target=candidate.target;target_rule=candidate.rule;break end
   debuff_scan.index=debuff_scan.index+1;work=work+(dynamic and math.max(16,#attack_spells+#combat_inventory*16) or 16)
   if work>=2048 and debuff_scan.index<=#candidates then return result(nil,'Evaluating group debuffs') end
  end
  debuff_scan=nil
 else debuff_scan=nil end
 if target and combat~='off' then
  locked_target=target.id
  if not target.identified and not target.appraisal_recent and s.targets and s.time>=(until_time['id'..target.id] or 0) then
   until_time['id'..target.id]=s.time+30;return {action='identify',item=target.id,status='Assessing '..target.name,continue_work=true} end
  local plan=combat_plan(target,target_rule)
  local attack,weapon,mode,damage_override=plan.attack,plan.weapon,plan.mode,plan.damage
  local categories,confirmed=plan.categories,plan.confirmed
  local missing_attack_status='No usable attack spell; check learned spells, skill buffer, components, selected element and enabled Void curses/rings'
  for _,category in ipairs(categories) do
   local spell=best_debuffs[category]
   if not spell then if not option('debuff_fallback',false) then return failed(plan.missing_attack and monster_damage_attacks(target_rule or {},p) and missing_attack_status or 'Required debuff unavailable; check learned spells, components, debuff items and activation requirements') end
   elseif (confirmed[category] or 0)<spell.power then
    local key='debuff'..target.id..':'..category
    if (item_attempts[key] or 0)>=3 then if not option('debuff_fallback',false) then return failed('Debuff resisted or unconfirmed after three attempts') end
    elseif target.distance<=option('radius',30) then
     if spell.item then
      if spell.source_kind=='proc' and band(spell.item.type,1)~=0 and target.distance>option('melee_range',2) then
       if option('approach',true) and target.distance<=option('approach_range',60) and target.cell then
        return {action='move',target=target.id,cell=target.cell,x=target.x,y=target.y,z=target.z,status='Approaching for proc debuff'}
       end
       return result(nil,'Proc debuff requires melee range')
      end
      if not spell.item.equipped or band(spell.item.equipped_slot,0x200000)~=0 then return equip(spell.item) end
      local ammo=proc_ammo[spell.item.id];if ammo and not ammo.equipped then return equip(ammo) end
     else
      if option('switch_debuff_wand',true) and mode=='magic' and weapon and not weapon.equipped then return equip(weapon) end
      local equipment=caster();if equipment then return equipment end
     end
     item_attempts[key]=(item_attempts[key] or 0)+1
     debuff_pending={target=target.id,spell=spell.id,category=category,power=spell.power,started=s.time,deadline=s.time+10,serial=s.action_serial or 0,revision=s.debuff_revision or 0}
     if spell.item then
      if spell.source_kind=='grenade' or spell.source_kind=='proc' then
       debuff_pending.physical=true
       local melee=band(spell.item.type,1)~=0
       return {action='attack',item=spell.item.id,debuff_spell=spell.id,target=target.id,mode=melee and 2 or 4,power=melee and 0 or 1,height=2,status='Debuffing with '..spell.item.name}
      end
      debuff_pending.item=spell.item.id
      return {action='cast_item',item=spell.item.id,spell=spell.id,target=target.id,status='Debuffing with '..spell.item.name}
     end
     return cast(s,spell,target.id,'Debuffing: '..(spell.name or 'spell'))
    end
   else item_attempts['debuff'..target.id..':'..category]=0 end
  end
  if not monster_damage_attacks(target_rule or {},p) then return result(nil,'Debuffs active; ordinary attacks disabled for this mob') end
  if not weapon and plan.unassessed then return assess(plan.unassessed) end
  if not weapon and plan.unassessed_ammo then return assess(plan.unassessed_ammo) end
  if not weapon and plan.missing_attack then return failed(missing_attack_status) end
  if not weapon and plan.blocked_reason then return failed('No eligible combat loadout; '..plan.blocked_reason) end
  if not weapon and s.inventory then return failed('No eligible combat loadout; check weapon requirements, damage type and ammunition') end
  if weapon and (not weapon.equipped or band(weapon.equipped_slot,0x200000)~=0) then return equip(weapon) end
  local secondary_request=target_rule and target_rule.secondary_equip
  if secondary_request==nil then secondary_request=option('melee_secondary_equip',nil) end
  if damage_override and damage_override~=0 then
   for _,v in ipairs(inventory) do
    if band(v.type,1)~=0 and band(v.damage_type,damage_override)==0
     and (secondary_request==v.id or (secondary_request==nil and v.equipped and band(v.equipped_slot,0x200000)~=0)) then secondary_request=2;break end
   end
  end
  -- With no explicit offhand rule, Left-hand Tether is the user's assignment.
  -- Do not rank this weapon as a main hand or repeatedly swap the two hands.
  local tether_only=secondary_request==nil
  if tether_only then for _,v in ipairs(inventory) do if v.auto_wield_left and weapon_allowed(v) then secondary_request=2;break end end end
  if weapon and (mode=='melee' or mode=='missile' and (weapon.ammo_type or 0)==0)
   and secondary_request~=nil and band(weapon.slots,0x2000000)==0 then
   local requested=secondary_request
   local current,shield,secondary,unknown
   local rank,best_preference,best_priority,best_bonus=-1,-1,-1,-1
   for _,v in ipairs(inventory) do
    if v.equipped and band(v.equipped_slot,0x200000)~=0 then current=v end
    local melee=band(v.type,1)~=0 and band(v.slots,0x2000000)==0
    local armor=band(v.type,2)~=0 and band(v.slots,0x200000)~=0
    if v.id~=weapon.id and (melee or armor) and (not tether_only or v.auto_wield_left)
     and (requested>3 and requested==v.id or requested<=2 and weapon_allowed(v)) then
     if not equipment_known(v) then unknown=v
     elseif v.can_wield and (not melee or not damage_override or damage_override==0 or band(v.damage_type,damage_override)~=0) then
      if requested>3 then secondary=v
      elseif armor and not shield then shield=v
      elseif melee then
       local rend,bonus=weapon_traits(v)
       local e=damage_override and damage_override~=0 and damage_override or rend~=0 and rend or preferred_damage(target,v.damage_type)
       local resistance_score,preference=resistance(target,e),element_preference(target,e)
       local priority=plan.priority(v,e,rend,bonus,false,false)
       local value=(v.damage or 0)*(1-(v.variance or 0)*.5)*math.max(1,v.damage_mod or 1)*resistance_score/(1+(v.weapon_time or 0)/100)
       if (v.slayer or 0)>0 and v.slayer==target.creature_type then value=value*math.max(1,v.slayer_mod or 1) end
       if not secondary or (v.auto_wield_left and not secondary.auto_wield_left) or ((v.auto_wield_left==true)==(secondary.auto_wield_left==true)
        and (priority>best_priority or priority==best_priority and (preference>best_preference or preference==best_preference and (bonus>best_bonus or bonus==best_bonus and value>rank)))) then
        secondary=v;rank=value;best_preference=preference;best_priority=priority;best_bonus=bonus end
      end
     end
    end
   end
   if requested==1 or requested==0 and not (secondary and secondary.auto_wield_left) and (trained[48] or not trained[49]) and shield then secondary=shield
   elseif requested==3 then secondary=nil end
   if not secondary and unknown and requested~=3 then return assess(unknown) end
   if secondary then
    if not current or current.id~=secondary.id then return equip(secondary,true) end
    item_attempts['equip'..secondary.id..':offhand']=0
   elseif current then return equip(current,false,true) end
  end
  if mode=='missile' and weapon and (weapon.ammo_type or 0)~=0 then
   if not plan.ammo then return failed('No usable ammunition for this weapon') end
   if not plan.ammo.equipped then return equip(plan.ammo) end
  end
  local range=mode=='melee' and option('melee_range',2) or option('radius',30)
  if target_rule and (target_rule.range or 0)>0 then range=target_rule.range end
  if target.distance>range then
   if target.distance<=option('approach_range',60) and option('approach',true) and target.cell then
    return {action='move',target=target.id,cell=target.cell,x=target.x,y=target.y,z=target.z,status='Approaching '..(target.name or 'target')}
   end
  elseif mode=='magic' and attack then
   local equipment=caster();if equipment then return equipment end
   ghost_attempts[target.id]=(ghost_attempts[target.id] or 0)+1
   return cast(s,attack,target.id,'Casting '..(attack.name or 'attack'))
  elseif mode=='magic' and plan.dot_only then
   return result(nil,'Void curses active; waiting for their timers before refreshing')
  elseif mode=='melee' or mode=='missile' then
   local power=option('power_percent',nil)
   power=power and power/100 or option('power',.5)
   if option('auto_attack_power',true) and weapon then
    power=1
    if mode=='melee' then
     local damage=plan.attack_element or preferred_damage(target,weapon.damage_type)
     local secondary='none';for _,v in ipairs(inventory) do if v.equipped and band(v.equipped_slot,0x200000)~=0 then secondary=band(v.type,1)~=0 and 'melee' or 'shield';break end end
     local dual=weapon.damage_type==3;local multi=band(weapon.attack_type,0x40)~=0
     if weapon.weapon_type==1 and secondary~='melee' then power=damage==1 and dual and .5 or 0
     elseif damage==2 and dual then power=not multi and .2 or secondary=='melee' and .49 or secondary=='shield' and 1 or .2 end
    end
    if option('use_recklessness',false) and trained[50] then power=math.max(.11,math.min(.9,power)) end
   end
   return {action='attack',target=target.id,mode=mode=='melee' and 2 or 4,power=power,height=option('manual_attack_height',false) and option('height',2) or (target.attack_height or 2),status='Combat: '..mode}
  elseif s.inventory then return failed('No eligible combat equipment or attack spell; review Combat setup') end
 end
 local loot_action=loot();if loot_action then return loot_action end
 if not target then local components=split_peas(option('component_idle',20));if components then return components end end
 if not target then local craft=craft_work(s,p,'idle');if craft then return craft end end
 if not target and option('idle_buff_topoff',false) then local action=buffs(option('idle_buff_seconds',1200));if action then return action end end
 if activity_ready('inventory') and option('autocram',false) and (s.main_pack_slots or 0)<2 then
  for _,bag in ipairs(s.packs or {}) do if bag.free>0 then for _,item in ipairs(inventory) do
   if item.container==s.player and item.object_class~=10 and item.object_class~=38 and not item.equipped then
    return {action='store_item',item=item.id,target=bag.id,status='Making room in the main pack'}
   end
  end end end
 end
 if activity_ready('combine') and option('salvage_combine',false) and (not s.container or s.container==0) then
  if combine_pending then
   local remains=false;for _,item in ipairs(inventory) do if combine_pending.ids[item.id] then remains=true end end
   if not remains then combine_pending=nil
   elseif inventory_failure(s,combine_pending,combine_pending.ids) or s.time>=combine_pending.deadline then return failed('Salvage combine did not complete; review bags and Ust')
   else return result(nil,'Waiting for salvage combination') end
  end
  local policy=p.salvage_policy or {default={{min=1,max=6},{min=7,max=8},{min=9,max=9},{min=10,max=10}}}
  local groups,tool={},nil
  for _,item in ipairs(inventory) do
   if (item.salvage_tool_available==true or (item.salvage_tool_available==nil and item.object_class==40 and item.resource_available~=false)) then tool=item.id end
   if item.object_class==39 and not item.equipped and not item.retained and item.resource_available~=false
    and salvage_material(item) and not salvage_protected(item) and not combination_rejected(combine_blocked,item)
    and (item.structure or 0)>0 and item.structure<100 and item.workmanship then
    local ranges=(policy.materials or {})[vt_string(item.material)] or policy.default or {};local index=#ranges+1
    for n,range in ipairs(ranges) do if range.min>item.workmanship then index=n-1;break elseif item.workmanship<=range.max then index=n;break end end
    local key=tostring(item.material)..':'..index;groups[key]=groups[key] or {};groups[key][#groups[key]+1]=item
   end
  end
  local keys={};for key in pairs(groups) do keys[#keys+1]=key end;table.sort(keys)
  for _,key in ipairs(keys) do local bags=groups[key];if #bags>=2 then
   -- VT sorts by material/workmanship before selecting bags. Stable GUID ties
   -- keep server enumeration order from changing the selected combination.
   table.sort(bags,function(a,b)return a.workmanship==b.workmanship and a.id<b.id or a.workmanship<b.workmanship end)
   local selected,units,value={},0,0;local threshold=(policy.values or {})[vt_string(bags[1].material)]
   if threshold and threshold<=0 then threshold=nil end
   for _,bag in ipairs(bags) do selected[#selected+1]=bag;units=units+bag.structure;value=value+(bag.value or 0);if #selected==64 or (not threshold and units>=100) then break end end
   if threshold and value<threshold then
    table.sort(bags,function(a,b) return a.structure<b.structure end)
    selected=bags[1].structure+bags[2].structure<100 and {bags[1],bags[2]} or {}
   end
   if #selected>=2 then
    if not tool then return failed('Salvage combining requires an Ust') end
    local ids,text,states={},{},{};for _,bag in ipairs(selected) do ids[bag.id]=true;states[bag.id]=combination_state(bag);text[#text+1]=string.format('%d',bag.id) end
    combine_pending={ids=ids,states=states,deadline=s.time+15,failure_serial=s.inventory_failure_serial or 0}
    return {action='combine_salvage',tool=tool,items=table.concat(text,','),status='Combining salvage workmanship group'}
   end
  end end
 elseif not option('salvage_combine',false) then combine_pending=nil end
 if activity_ready('inventory') and option('autostack',false) and (not s.container or s.container==0) then
  if stack_pending then
   local source,target
   for _,v in ipairs(inventory) do
    if v.id==stack_pending.source then source=v end
    if v.id==stack_pending.target then target=v end
   end
   -- Wait for both inventory updates, not UseDone or an unrelated stack change.
   if (not source or (source.count or 1)<stack_pending.source_count)
    and target and (target.count or 1)>stack_pending.target_count then stack_pending=nil
   elseif inventory_failure(s,stack_pending,{[stack_pending.source]=true,[stack_pending.target]=true})
    or s.time>=stack_pending.deadline then return failed('Stack combination was not confirmed; skipping unchanged items')
   else return result(nil,'Waiting for stack combination') end
  end
  local stacks={}
  for _,v in ipairs(inventory) do
   -- Full stacks are already compact. Moving one into a partial stack just
   -- swaps which stack is full and can repeat forever.
   if (v.max_stack or 0)>1 and (v.count or 1)<v.max_stack and (v.wcid or 0)>0
    and v.object_class~=39 and band(v.type,0x4000000)==0
    and not v.equipped and not v.retained and v.resource_available~=false and not combination_rejected(stack_blocked,v) then
    local target=stacks[v.wcid];local source=v
    if target then
     stack_pending={source=source.id,target=target.id,source_count=source.count or 1,target_count=target.count or 1,
      states={[source.id]=combination_state(source),[target.id]=combination_state(target)},failure_serial=s.inventory_failure_serial or 0,deadline=s.time+15}
     return {action='merge',item=source.id,target=target.id,status='Combining stacks: '..(v.name or 'item')}
    end
    stacks[v.wcid]=v
   end
  end
 elseif not option('autostack',false) then stack_pending=nil end
 if corpse_spawn_wait and not option('nav_priority',false) and option('navigation',false)
  and not loot_full and (#(p.loot_rules or {})>0 or option('read_unknown_scrolls',false) or need_mana_source)
  and activity_ready('loot') then
  return result(nil,'Waiting for defeated creature\'s corpse')
 end
 local nav=navigate();if nav then return nav end
 -- VT ba/cLogic schedules these after recovery, combat, loot and navigation.
 -- Pick one eligible family first, so learning more tiers does not bias its
 -- probability. Recipient buff state is unknown: this is explicitly a random
 -- courtesy buff, not a claim that the player needs it.
 if not target and not health_critical and activity_ready('random_helper') and option('random_helper_buffs',false) and s.time>=random_helper_at then
  local players={}
  for _,v in ipairs(s.route_objects or {}) do
   if v.object_class==24 and v.id~=s.player and (v.distance or 1e9)<=18 and v.line_of_sight==true then players[#players+1]=v end
  end
  local choices={}
  for _,category in ipairs({3,93,95,115,101,103,105,107,109,111,113}) do
   local best
   for _,v in ipairs(spell_groups.by_category[category] or {}) do
    if v.beneficial and not v.caster_target and (v.duration or 0)>0 and band(v.flags,0x2000)==0 and stronger_spell(v,best) then best=v end
   end
   if best then choices[#choices+1]=best end
  end
  if #players>0 and #choices>0 then
   activity='random_helper'
   local equipment=caster();if equipment then return equipment end
   local recipient=players[math.random(#players)];local spell=choices[math.random(#choices)]
   random_helper_at=s.time+math.max(1,option('random_helper_interval',5))
   return cast(s,spell,recipient.id,'Helping '..recipient.name..': '..spell.name)
  end
 end
 local idle=idle_stance();if idle then return idle end
 if #recovery_missing>0 then
  local names={};for _,v in ipairs(recovery_missing) do names[#names+1]=v==2 and 'health' or v==4 and 'stamina' or 'mana' end
  return result(nil,'Recovery unavailable for '..table.concat(names,', ')..'; check skill buffer, components, supplies and imported handlers')
 end
 if target and option('combat','off')~='off' and activity_pauses.combat and activity_pause_reasons.combat then
  return result(nil,'Combat paused: '..activity_pause_reasons.combat..'. Retrying in '..math.max(1,math.ceil(activity_pauses.combat-s.time))..' seconds')
 end
 return result(nil,navigation_paused or (meta.pending and 'Waiting for meta interaction') or (route_pending and (route_pending.kind=='pause' and 'Pausing on route' or 'Waiting for route action')) or (health_critical and 'Health critical; recovery unavailable, UCM remains active') or (loot_full and 'Looting paused: inventory full; other activities remain active') or 'Ready - waiting for enabled activities')
end

local function pause_activity(s,name,reason)
 if name=='navigation' then return pause_navigation(reason) end
 local rejected=name=='combine' and combine_pending or name=='inventory' and stack_pending
 if rejected then
  local blocked=name=='combine' and combine_blocked or stack_blocked
  for id,state in pairs(rejected.states or {}) do blocked[id]=state end
 end
 -- Invalid meta execution requires explicit restart; do not skip ahead through
 -- a script whose later commands may depend on the rejected operation.
 local delay=name=='meta' and math.huge or (name=='combat' or name:sub(1,8)=='recovery') and 5 or 30
 activity_pauses[name]=s.time+delay
 activity_pause_reasons[name]=reason
 if name=='vendors' then note_pending=nil;purchase_pending=nil
 elseif name=='loot' then
  if loot_job_pending and loot_job_pending.action=='salvage' then inventory_salvage_blocked[loot_job_pending.id]=true end
  if loot_pending then corpses[loot_pending.container]=s.time+30 end
  if corpse_pending then corpses[corpse_pending.id]=s.time+30 end
  if s.container and s.container~=0 then corpses[s.container]=s.time+30 end
  -- Never retry ambiguous destructive jobs or reclassify existing inventory.
  loot_pending=nil;corpse_pending=nil;loot_scan={};loot_jobs={};loot_job_pending=nil
 elseif name=='components' then pea_pending=nil
 elseif name=='item_mana' then mana_refill_pending=nil;mana_fill_pending=nil;item_attempts.mana_refill=0
 elseif name=='pets' then pet_pending=nil;pet_refill_pending=nil
 elseif name=='combine' then combine_pending=nil
 elseif name=='inventory' then stack_pending=nil
 elseif name=='fellowship' and fellow_manager then
  local q=fellow_manager.pending
  if q and q.operation=='recruit' then for i=#fellow_manager.queue,1,-1 do if fellow_manager.queue[i].id==q.target then table.remove(fellow_manager.queue,i) end end end
  fellow_manager.pending=nil;fellow_manager.backoff=s.time+30
 elseif name=='helper' then helper_pending=nil
 elseif name=='dispel' then dispel_pending=nil
 elseif name=='buffs' then buff_item_pending=nil
 elseif name=='buff_others' then other_job=nil
 elseif name=='combat' then locked_target=nil;debuff_pending=nil;debuff_scan=nil
 elseif name:sub(1,8)=='recovery' then
  local vital=tonumber(name:sub(9));if vital and recovery_pending then recovery_pending[vital]=nil else recovery_pending=nil end
 elseif name=='meta' then
  meta.pending=nil;meta.queue={};meta.turn=nil;meta.options={}
  meta.actions={};meta.loading=false;meta.awaiting_load=false;meta.profile_load=nil
  meta.forcebuff=false;if forced_buff and forced_buff.request==0 then forced_buff=nil end
 end
 -- Shared preparation can fail inside several activities. Release exhausted
 -- appraisal/equipment attempts, while the activity cooldown prevents a spin.
 for key in pairs(item_attempts) do
  local text=tostring(key)
  if text:sub(1,5)=='equip' or text:sub(1,2)=='id' or (name=='components' and text:sub(1,3)=='pea') then item_attempts[key]=nil end
 end
 local labels={random_helper='Idle helper buffs',crafting='Crafting',fellowship='Fellowship management',loot='Looting',vendors='Vendor restocking',components='Component preparation',item_mana='Equipment mana',pets='Pet summoning',combine='Salvage combining',helper='Fellowship recovery',dispel='Dispelling',buffs='Buffing',buff_others='Buff requests',combat='Combat',recovery2='Health recovery',recovery4='Stamina recovery',recovery6='Mana recovery',meta='Meta',inventory='Inventory management'}
 return {action='activity_failed',activity=name,status=(labels[name] or name)..' paused: '..reason..
  (rejected and '. Unchanged items will be skipped until they change or UCM restarts; other activities remain active'
   or delay==math.huge and '. Restart UCM to retry; other activities remain active' or '. Retrying in '..delay..' seconds; other activities remain active')}
end

local function run(s,p)
 if p.ucm_activity_failure then
  local failure=p.ucm_activity_failure
  if failure.inventory_full and failure.activity=='loot' then
   loot_full={revision=s.inventory_revision or 0,slots=s.pack_slots or 0}
   -- No transfer was sent. Preserve already received salvage/sell jobs, which
   -- can free space, and leave combat, recovery, buffs and navigation running.
   loot_pending=nil;corpse_pending=nil;loot_scan={}
   return result(nil,'Looting paused: inventory full; other activities remain active')
  end
  return pause_activity(s,failure.activity,failure.status)
 end
 if loot_full and (loot_full.revision~=(s.inventory_revision or 0) or (s.pack_slots or 0)>loot_full.slots) then loot_full=nil end
 -- The host switches navigation off when it receives pause_navigation. An
 -- explicit UI toggle or a new meta enable command can retry; stale overrides
 -- from the previous state must not silently resume the blocked route.
 if navigation_paused and p.navigation then
   navigation_paused=nil;route_blocks=0;door_attempts={}
   route_join_pending=true;route_join_scan=nil;meta.options.navigation=nil
 end
 if p.ucm_mana_only then
  if s.busy or s.ready==false or s.jumping then return result(nil,'Waiting for game action') end
  return equipment_mana(s,p,s.inventory or {},function(k,d) if p[k]==nil then return d end;return p[k] end) or result(nil,'Equipment mana ready')
 end
 species_names=s.species_names or {}
 meta_active_profile=p
 if not meta.entered then meta_state('Default',s) end
 -- Chat and imported routes use the same bounded command executor.
 -- Host drains this array once; it is never saved in a user profile.
 for _,command in ipairs(p.ucm_commands or {}) do
  local command_error=meta_command(command,s)
  if command_error then
   if command_error=='Stopped by UCM command' then return result('stop',command_error) end
   return failed(command_error)
  end
 end
 if p.ucm_command_only then
  local command_intent=meta_work(s,p)
  if command_intent then return command_intent end
  if meta.pending or s.jumping or s.busy or s.ready==false then return result(nil,'Waiting for UCM command completion') end
  return result('stop','UCM command completed')
 end
 local request=s.force_buff_request or 0
 if request~=0 and (not forced_buff or forced_buff.request~=request) then
  forced_buff={request=request,done={}};buff_plan=nil;retries={};buff_skips={}
  -- Force Buff is an explicit new attempt, including families skipped by a
  -- previous pass. Preserve component/skill eligibility and fresh failures.
  unavailable_spells={};catalog_snapshot=nil;activity_pauses.buffs=nil
  for key in pairs(item_attempts) do if tostring(key):sub(1,8)=='buffitem' then item_attempts[key]=nil end end
  for key in pairs(until_time) do
   local text=tostring(key)
   if text:sub(1,8)=='buffitem' then until_time[key]=nil end
   for i=1,#text do if text:sub(i,i)==':' then until_time[key]=nil;break end end
  end
 elseif request==0 and forced_buff and forced_buff.request~=0 then forced_buff=nil;buff_plan=nil end
 local effective=meta_profile(s,p)
 activity='meta'
 local error=activity_ready('meta') and (not forced_buff or forced_buff.request==0) and meta_tick(s,p)
 if error then
  if error=='Stopped by UCM command' then local out=result('stop',error);out.preserve_meta=true;return out end
  return failed(error)
 end
 effective=meta_profile(s,p)
 if navigation_paused then effective.navigation=false end
 -- Retain the followed player's observed path even while combat/actions pause walking.
 if effective.follow_corners and effective.navigation then
  local target
  for _,o in ipairs(s.route_objects or {}) do
   if ((effective.follow_name or '')~='' and o.name==effective.follow_name) or ((effective.follow_name or '')=='' and o.id==effective.follow_id) then target=o;break end
  end
  if not target or target.id~=follow_target or follow_teleport~=s.teleport_sequence then follow_path={} end
  follow_target=target and target.id;follow_teleport=s.teleport_sequence
  if target then
   local function remember(observation)
    local last=follow_path[#follow_path]
    if last and distance(last,observation)>50 then follow_path={} end
    if #follow_path>0 and distance(follow_path[#follow_path],observation)<.096 then return end
    local at={cell=observation.cell,x=observation.x,y=observation.y,z=observation.z}
    -- Compress only collinear observations; preserve turns, stairs and loops.
    local replace=false
    if #follow_path>=2 then
     local ax,ay,az=world(follow_path[#follow_path-1]);local bx,by,bz=world(follow_path[#follow_path]);local cx,cy,cz=world(at)
     local dx,dy,dz=cx-ax,cy-ay,cz-az;local length=dx*dx+dy*dy+dz*dz
     local t=length>0 and ((bx-ax)*dx+(by-ay)*dy+(bz-az)*dz)/length or 0
     replace=length<=36 and t>0 and t<1 and (bx-ax-t*dx)^2+(by-ay-t*dy)^2+(bz-az-t*dz)^2<=.02^2
    end
    if replace then follow_path[#follow_path]=at
    else
     if #follow_path>=512 then return pause_navigation('follow path exceeded 512 points') end
     follow_path[#follow_path+1]=at
    end
   end
   -- Network observations are collected even while the host is waiting for a
   -- cast/equipment acknowledgment and deliberately does not run the policy.
   if s.follow_motion_target==target.id then
    if s.follow_motion_overflow then return pause_navigation('follow movement history exceeded its limit') end
    for _,at in ipairs(s.follow_motion or {}) do local limit=remember(at);if limit then return limit end end
   end
   local limit=remember(target);if limit then return limit end
   -- Never search backwards for a nearby later segment: adjacent hallways and
   -- returning leaders can be close in space but far apart along the path.
   while #follow_path>1 and distance(s.position,follow_path[1])<=.15 do table.remove(follow_path,1) end
  end
 else follow_path={};follow_target=nil end
 local route=effective.route or {}
 local route_changed=route_teleport~=s.teleport_sequence or route_revision~=(effective.vt_revision or 0)
 if not effective.navigation or route_changed then
  route_trail={};route_returning=false;route_walking=false
 end
 if route_teleport~=s.teleport_sequence or route_revision~=(effective.vt_revision or 0) then route_blocks=0 end
 route_teleport=s.teleport_sequence;route_revision=effective.vt_revision or 0
 if effective.navigation and #route>0 and s.position and (not route_pending or route_pending.kind=='pause') then
  -- Combat may win the very first decision, before any waypoint is selected.
  -- Keep that departure position too; join-pending must not erase a chase.
  if route_walking or (#route_trail==0 and not route_returning) then
   route_trail={{cell=s.position.cell,x=s.position.x,y=s.position.y,z=s.position.z}}
  elseif not route_returning and #route_trail>0 then
   -- A delayed combat/loot acknowledgment can span more than 15m of ordinary
   -- travel. Only an actual teleport/revision invalidates the return trail.
   local function remember(at)
    if distance(at,route_trail[#route_trail])<.6 then return end
    -- Coalesce only short, nearly straight runs of the observed path. Keep
    -- bends and elevation changes rather than cutting to a nearby waypoint
    -- through a dungeon wall. Dense host samples otherwise cause a stop at
    -- every 60cm breadcrumb on the way back from combat.
    if #route_trail>=2 then
     local a,b=route_trail[#route_trail-1],route_trail[#route_trail]
     local ax,ay,az=world(a);local bx,by,bz=world(b);local cx,cy,cz=world(at)
     local dx,dy,dz=cx-ax,cy-ay,cz-az;local length=dx*dx+dy*dy+dz*dz
     local t=length>0 and ((bx-ax)*dx+(by-ay)*dy+(bz-az)*dz)/length or 0
     if length<=36 and t>0 and t<1 and (bx-ax-t*dx)^2+(by-ay-t*dy)^2+(bz-az-t*dz)^2<=.02^2 then
      route_trail[#route_trail]={cell=at.cell,x=at.x,y=at.y,z=at.z};return
     end
    end
    if #route_trail>=2048 then return pause_navigation('combat detour is too long') end
    route_trail[#route_trail+1]={cell=at.cell,x=at.x,y=at.y,z=at.z}
   end
   if not route_changed then
    if s.route_motion_overflow then return pause_navigation('movement history exceeded its limit') end
    for _,at in ipairs(s.route_motion or {}) do local limit=remember(at);if limit then return limit end end
   end
   local limit=remember(s.position);if limit then return limit end
  end
 end
 local blocked=s.movement_blocked_serial or 0
 if blocked_serial==nil then blocked_serial=blocked end
 if blocked~=blocked_serial then
  blocked_serial=blocked
  if effective.navigation then
   route_blocks=route_blocks+1
   if route_blocks>2 then return pause_navigation('route obstructed after two recovery attempts') end
   if route_returning then return pause_navigation('return path is blocked') end
  end
  if locked_target then monster_blacklist[locked_target]=s.time+30;locked_target=nil end
  if #route_trail==0 then route_join_pending=true;route_join_scan=nil end
 end
 if not effective.auto_fellowship then fellow_manager=nil end
 local intent=(not forced_buff and not (s.crafting and s.crafting.pending) and meta_work(s,effective)) or tick(s,effective)
 -- Empty embedded routes clear native navigation even when the resulting
 -- configuration has no combat, movement or inventory action to emit.
 if not intent and meta.route_changed then intent=result(nil,'Navigation route updated') end
 if intent then
  intent.services_profile={}
  for _,key in ipairs({'auto_craft','auto_fellowship','craft_confirm','assist_items','buff_items','craft_targets','craft_health_kits','craft_health_food','craft_stamina_kits','craft_stamina_food','craft_mana_kits','craft_mana_food'}) do if effective.auto_craft then intent.services_profile[key]=effective[key] end end
  intent.services_profile.random_helper_buffs=effective.random_helper_buffs==true
  intent.services_profile.grenade_catalog=effective.grenade_catalog
  intent.services_profile.assist_items=effective.assist_items
  intent.services_profile.auto_craft=effective.auto_craft==true;intent.services_profile.auto_fellowship=effective.auto_fellowship==true
  if fellow_manager then intent.fellowship_status=intent.fellowship_status or fellow_manager.status end
  intent.targeting_status=targeting_status
  route_walking=intent.status and (intent.status:sub(1,9)=='Waypoint ' or intent.status=='Route complete') or false
  if intent.status~='Returning to route' then route_returning=false end
  if intent.action=='cast' then intent.fast_cast=effective.fast_cast_buffs~=false end
  if intent.action=='cast' or intent.action=='cast_item' then intent.cast_jiggle=effective.cast_jiggle==true;intent.jump_out_wand=effective.jump_out_wand==true end
  intent.helper_updates=effective.recovery~=false and ((effective.helper_health_threshold or 0)>0 or (effective.helper_stamina_threshold or 0)>0 or (effective.helper_mana_threshold or 0)>0)
  intent.route_point=point
  intent.route_join_pending=route_join_pending
  intent.route_track_movement=effective.navigation and #route>0 and not route_walking and not route_returning and (not route_pending or route_pending.kind=='pause') or false
  if p.vt_meta then
   intent.meta_state=meta.name;intent.meta_status=meta_status(s,effective)
   if intent.status=='Record a route to begin navigation' then
    intent.status=effective.meta_enabled==false and 'Meta paused; enable Run imported meta' or ('Meta '..meta.name..': waiting for a route or trigger')
   end
  end
  if meta.route_changed then
   intent.runtime_route=meta.route.route;intent.runtime_route_profile=meta.route_profile;intent.runtime_loop_route=effective.loop_route==true;intent.runtime_reverse_route=effective.reverse_route==true
   meta.route_changed=false
  end
 end
 return intent
end

return function(s,p)
 completed_corpse=nil
 confirmed_loot_notice=nil
 targeting_status=nil
 activity_time=s.time;activity='meta'
 if p.ucm_resume then
  fellow_manager=nil;fellow_last_serial=0
  -- VT resets the ordinary state timer on Start, retaining the persistent timer,
  -- variables, fired rules, call stack and selected route. In-flight requests
  -- are abandoned: inventory/targets may have changed while stopped.
  if meta.entered then meta.entered=s.time end
  entered=s.time;locked_target=nil;route_blocks=0;blocked_serial=nil
  route_join_pending=true;route_join_scan=nil
  route_trail={};route_returning=false;route_walking=false
  local loads={}
  if meta.profile_load and (p.vt_revision or 0)==meta.profile_revision then loads[#loads+1]=meta.profile_load end
  for _,command in ipairs(meta.queue) do if command.op=='load' then loads[#loads+1]=command end end
  meta.pending=nil;meta.queue=loads;meta.loading=false;meta.awaiting_load=#loads>0;meta.profile_load=nil;meta.forcebuff=false
  if meta.route then meta.route_changed=true end
  route_pending=nil;loot_pending=nil;loot_full=nil;recovery_pending=nil;buff_item_pending=nil
  corpse_pending=nil;corpse_contents_wait=nil;corpse_spawn_wait=nil;combat_death_seen=s.combat_death and s.combat_death.time;purchase_pending=nil;note_pending=nil;mana_refill_pending=nil;mana_fill_pending=nil;pet_refill_pending=nil;helper_pending=nil;random_helper_at=0;debuff_pending=nil;debuff_scan=nil;pea_pending=nil;pet_pending=nil;dispel_pending=nil
  last_cast=nil;unavailable_spells={};forced_buff=nil;buff_skips={};buff_recast_until=0;other_job=nil
  loot_scan={};loot_jobs={};loot_job_pending=nil;mana_sources={};scroll_reads={};combine_pending=nil;stack_pending=nil;combine_blocked={};stack_blocked={};inventory_salvage_scan={};inventory_salvage_blocked={}
  retries={};until_time={};item_attempts={};corpses={};monster_failures={};monster_blacklist={};ghost_attempts={};ghost_hp={};spell_health={}
  follow_path={};follow_target=nil;follow_teleport=nil
  activity_pauses={};activity_pause_reasons={}
  history_cleanup_at=0
 end
 prune_history(s)
 -- Count consecutive unresolved requests, not successful assessments over the
 -- whole session. Loot receipt and real item changes may legitimately require
 -- a fresh appraisal later, including the currently configured weapon/ammo.
 for _,list in ipairs({s.inventory or {},s.contents or {},(s.buff_request or {}).equipment or {}}) do
  for _,item in ipairs(list) do if item.identified then
   local key='id'..item.id;item_attempts[key]=nil;until_time[key]=nil
  end end
 end
 buff_plan=nil
 local intent=run(s,p)
 if intent then
  intent.activity=intent.activity or activity
  -- Host rejections arrive already formatted; ordinary policy failures are
  -- isolated here once, after the deciding function has unwound.
  if intent.action=='activity_failed' and not p.ucm_activity_failure then
   local original=intent
   intent=pause_activity(s,intent.activity,intent.status)
   -- Isolating a failed combat/recovery action must not discard a meta's
   -- already-decided route update (especially an empty route) or live status.
   for _,key in ipairs({'runtime_route','runtime_route_profile','runtime_loop_route','runtime_reverse_route','meta_state','meta_status','route_point','route_join_pending','route_track_movement','targeting_status','helper_updates'}) do
    if original[key]~=nil then intent[key]=original[key] end
   end
  end
 end
 if completed_corpse then intent=intent or result(nil,'Loot complete');intent.completed_corpse=completed_corpse end
 if confirmed_loot_notice and p.loot_notify_pickup~=false then intent=intent or result(nil,'Loot complete');intent.loot_notification=confirmed_loot_notice end
 buff_plan=nil -- do not retain the previous full snapshot across callbacks
 catalog_snapshot=nil;catalog_by_id=nil;catalog_supplied=nil;catalog_usable=nil;catalog_groups=nil
 object_snapshot=nil -- references keep IDs; the full world snapshot can be collected
 -- Expression helpers only need these during a callback. Retaining the profile
 -- kept an entire LootSnob rule tree alive while the host marshalled the next
 -- copy, exhausting the VM heap on the second looting decision.
 meta_active_profile=nil;species_names=nil
 return intent
end
