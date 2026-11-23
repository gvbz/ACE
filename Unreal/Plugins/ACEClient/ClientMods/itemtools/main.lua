-- Item Tools: one confirmed inventory transaction at a time. All game actions
-- are revalidated by the native host; this script cannot send network packets.
local pending,queue,split,baseline=nil,nil,nil,nil
local scans={}
local completed,total,next_at,cycle,stage=0,0,0,0,'open'
local operations=0
local give_retry,give_retries=nil,0
local received={}
local refresh_at=0
local chest_scan=1
local chest_seen={}
local mule_scan={mule1=1,mule2=1}
local function status(text,action) return {status=text,action=action,completed=completed,total=total,operations=operations} end
local function stop(text) return status(text,'stop') end
local count_cache
local function count(items,name,wcid)
 if not count_cache then
  count_cache={}
  for _,i in ipairs(items or {}) do
   local entry=count_cache[i.name] or {total=0,classes={}};count_cache[i.name]=entry
   entry.total=entry.total+(i.count or 1);local cls=i.wcid or 0
   entry.classes[cls]=(entry.classes[cls] or 0)+(i.count or 1)
  end
 end
 local entry=count_cache[name];if not entry then return 0 end
 return (not wcid or wcid==0) and entry.total or (entry.classes[wcid] or 0)
end
local function find(items,id) for _,i in ipairs(items or {}) do if i.id==id then return i end end end
local function usable(i) return i and i.transferable==true end
local function issue(s,p,action,item,amount,target,kind)
 pending={kind=kind or action,id=item and item.id,amount=amount or 1,before=item and item.count or 0,
  name=item and item.name,wcid=item and item.wcid,serial=s.action_serial or 0,deadline=s.time+20,
  failure_serial=s.it_failure_serial or 0,
  target=target,currency=s.it_currency or 0,
  funding=action=='it_sell' and (p.mode=='restock' or (p.mode=='shop' and (p.buy_name or '')~='')),
  owned=item and action~='it_merge' and count(s.inventory,item.name,item.wcid) or 0}
 if action=='it_give' then
  local recipient=find(s.it_objects,target)
  pending.recipient_name=recipient and recipient.name
 end
 if action=='it_merge' then pending.target_before=find(s.inventory,target).count end
 local r=status((action=='it_give' and 'Giving ' or action=='it_buy' and 'Buying ' or action=='it_sell' and 'Selling ' or action=='it_pickup' and 'Taking ' or action=='it_merge' and 'Merging ' or action=='it_split' and 'Splitting ' or action=='it_key' and 'Unlocking with ' or 'Opening ')..(item and item.name or 'chest'),action)
 r.item=item and item.id;r.amount=amount;r.target=target;r.vendor=s.vendor
 return r
end
local function evaluate(s,rules,item,scope)
 local key=(scope or "profile")..":"..item.id
 local scan=scans[key] or {index=1,state={}};scans[key]=scan
 while scan.index<=#rules do
  if not workavailable() then return 'pending' end
  local rule=rules[scan.index];local match=lootmatch(rule,item,s,scan.state)
  if match=='pending' then return 'pending' end
  if match==nil then scans[key]=nil;return 'identify' end
  if match then scans[key]=nil;return rule end
  scan.index=scan.index+1;scan.state={}
 end
 scans[key]=nil;return false
end
local identified={}
local function assess(s,item)
 local q=identified[item.id]
 if q and s.time<q then return status('Waiting for appraisal: '..item.name) end
 if q then return stop('Appraisal unavailable: '..item.name..'. No items transferred.') end
 identified[item.id]=s.time+15
 return {action='identify',item=item.id,status='Inspecting '..item.name}
end
local function keeps(rule) return rule and (rule.action or 'keep')=='keep' end
local function give_target(s,p,key)
 local id=p[key..'_id'];local t=find(s.it_objects,id)
 if t and t.recipient and t.name==p[key..'_name'] and t.distance<=5 then return id,t.player end
end
local function purchase(s,p,stock,amount)
 amount=math.min(amount,stock.limit or 0,stock.room or 0,100000)
 if amount<=0 then return stop('No inventory space or vendor stock available. Other plugins may continue.') end
 local function cost(n) return math.max(1,math.ceil((stock.sell_rate or 1)*(stock.unit_value or 0)*n-.1)) end
 local funds=s.it_currency or 0
 if cost(amount)>funds and not p.server_bank then
  local seller
  for _,i in ipairs(s.inventory or {}) do
   local chosen=p.sell_name and p.sell_name~='' and i.name==p.sell_name
   local note=p.redeem_notes~=false and s.vendor_uses_pyreals and i.trade_note
   if usable(i) and i.can_sell and (chosen or note) and i.name~=stock.name and (i.sell_unit or 0)>0 then
    if not seller or (chosen and seller.name~=p.sell_name) or i.sell_unit<seller.sell_unit then seller=i end
   end
  end
  if seller then
   local qty=math.min(seller.count,math.max(1,math.ceil((cost(amount)-funds)/seller.sell_unit)))
   if qty<seller.count then
    split={name=seller.name,wcid=seller.wcid,amount=qty,known={}}
    for _,i in ipairs(s.inventory) do split.known[i.id]=true end
    return issue(s,p,'it_split',seller,qty,nil)
   end
   return issue(s,p,'it_sell',seller,qty,s.vendor)
  end
  -- An unlimited buy uses the affordable remainder rather than discarding it.
  if p.mode=='shop' and (p.buy_limit or 0)<=0 then
   local low,high=0,amount
   while low<high do local mid=math.floor((low+high+1)/2);if cost(mid)<=funds then low=mid else high=mid-1 end end
   amount=low
  end
  if amount<=0 or cost(amount)>funds then return stop('Insufficient currency; no eligible funding items remain.') end
 end
 return issue(s,p,'it_buy',stock,amount,s.vendor)
end
return function(s,p)
 count_cache=nil
 local inv=s.inventory or {}
 local mode=p.mode or 'give'
 if not baseline and mode=='chest' then baseline={};for _,i in ipairs(inv) do baseline[i.id]=i.count or 1 end end
 if pending then
  local q=pending;local i=find(inv,q.id);local done=false
  local given=math.min(q.amount,math.max(0,q.before-(i and (i.count or 1) or 0)))
  -- Quest NPCs can consume one unit even when offered a whole stack.
  -- Confirm only the amount actually removed; keep its remainder queued.
  if q.kind=='it_give' then done=given>0
  elseif q.kind=='it_merge' then
   local target=find(inv,q.target)
   -- Both inventory receipts are required, in either order. UseDone and an
   -- unrelated stack update cannot start another merge using stale counts.
   done=target and target.wcid==q.wcid and target.count==q.target_before+q.amount
    and (not i or i.wcid==q.wcid) and (i and i.count or 0)==q.before-q.amount
  elseif q.kind=='it_sell' then done=not i or (i.count or 1)<=q.before-q.amount
  elseif q.kind=='it_buy' or q.kind=='it_pickup' then done=count(inv,q.name,q.wcid)>=q.owned+q.amount
  elseif q.kind=='it_split' then
   for _,v in ipairs(inv) do if not split.known[v.id] and v.name==split.name and v.wcid==split.wcid and v.count==split.amount then
    if split.id and split.id~=v.id then return stop('Split has ambiguous results; review inventory before continuing.') end
    split.id=v.id;done=true
   end end
  elseif q.kind=='it_open' then done=s.container==q.target
  elseif q.kind=='it_key' then
   local chest=find(s.it_objects,q.target)
   done=chest and chest.locked==false or s.container==q.target or ((s.action_serial or 0)~=q.serial and (s.action_error or 0)==0)
  end
  if done and q.funding and (s.it_currency or 0)<=q.currency then
   if s.time>=q.deadline then return stop('Sale completed, but no inventory currency arrived. If this server banks sale proceeds, enable Use server bank for purchases; no more payment items were sold.') end
   done=false
  end
  -- Bank servers debit currency outside inventory. The requested supply receipt
  -- confirms the purchase; an untagged UseDone alone never does.
  if done and q.kind=='it_buy' and not p.server_bank and (s.it_currency or 0)>=q.currency then done=false end
  if done then
   operations=operations+1
   if q.kind=='it_key' then identified[q.target]=nil;refresh_at=s.time+2;if stage=='opener' then cycle=cycle+1 end end
   if q.kind=='it_give' then
    completed=completed+given;give_retries=0
    if queue and queue[1] and queue[1].id==q.id then
     queue[1].count=queue[1].count-given
     if not i or queue[1].count<=0 then table.remove(queue,1) end
    end
    if received[q.id] then received[q.id]=math.max(0,received[q.id]-given) end
   end
   if q.kind=='it_buy' then completed=completed+q.amount end
   if q.kind=='it_pickup' then
    -- Only units received by this run may be sent to mules, including merges.
    local left=q.amount
    for _,v in ipairs(inv) do if v.name==q.name and v.wcid==q.wcid then
     local extra=math.max(0,v.count-(baseline[v.id] or 0)-(received[v.id] or 0))
     local n=math.min(left,extra);received[v.id]=(received[v.id] or 0)+n;left=left-n
    end end
   end
   local delay=q.kind=='it_merge' and (p.merge_delay_ms or 0) or (p.delay_ms or 250)
   pending=nil;next_at=s.time+math.max(0,math.min(5000,delay))/1000
  elseif (s.it_failure_serial or 0)~=q.failure_serial and (s.it_failure_item==q.id or (q.kind=='it_merge' and s.it_failure_item==q.target)) then
   local err=s.it_failure_error or 0
   -- ACE sends an item-specific rejection (often error 0) while an NPC
   -- finishes its reward emotes, without a UseDone. Retry only explicit
   -- rejections, never a timed-out/ambiguous transfer.
   if q.kind~='it_give' or (err~=0 and err~=0x1d and err~=0x1e and err~=0x4ce) then
    return stop('Server rejected '..q.kind:sub(4)..'; remaining items were not sent.')
   end
   give_retries=give_retries+1
   if give_retries>6 then return stop('Recipient repeatedly rejected the gift. Remaining items are still in your packs.') end
   pending=nil;give_retry=q;next_at=s.time+math.max(math.min(3,give_retries),math.min(5000,p.delay_ms or 250)/1000)
   return status('Gift rejected; waiting to retry '..q.name..' ('..give_retries..'/6)')
  elseif (s.action_serial or 0)~=q.serial and (s.action_error or 0)~=0 then return stop('Server rejected '..q.kind:sub(4)..'; remaining items were not sent.')
  elseif s.time>=q.deadline then return stop('Transfer was not confirmed. Check recipient, range and space before restarting.')
  else return status('Waiting for confirmation: '..(q.name or 'chest')) end
 end
 if s.time<next_at or s.ready==false or s.busy or s.portal_space then
  local wait=status(give_retry and 'Waiting to retry gift: '..give_retry.name or 'Waiting for game action')
  if mode=='merge' and s.time<next_at then wait.merge_resume_at=next_at end
  return wait
 end
 if give_retry then
  local q=give_retry;local item=find(inv,q.id);local recipient=find(s.it_objects,q.target)
  if not usable(item) or item.name~=q.name or item.wcid~=q.wcid or item.count~=q.before then return stop('Item changed during gift retry; review inventory before restarting.') end
  if not recipient or not recipient.recipient or recipient.name~=q.recipient_name or recipient.distance>5 or (recipient.player and item.attuned) then return stop('Recipient changed or moved away; remaining items are still in your packs.') end
  give_retry=nil;return issue(s,p,'it_give',item,q.amount,q.target)
 end
 if split and split.id then local i=find(inv,split.id);split=nil;if not usable(i) then return stop('Split item changed; sale cancelled.') end;return issue(s,p,'it_sell',i,i.count,s.vendor) end
 if mode=='merge' then
  -- Keep full stacks intact. Fill the fullest partial stack from the smallest
  -- matching one, then rebuild from server-confirmed inventory on the next step.
  local partial={}
  for _,i in ipairs(inv) do
   if usable(i) and (i.wcid or 0)>0 and (i.max_stack or 0)>1
    and (i.count or 0)>0 and i.count<i.max_stack then partial[#partial+1]=i end
  end
  table.sort(partial,function(a,b)
   if a.wcid~=b.wcid then return a.wcid<b.wcid end
   if a.count~=b.count then return a.count>b.count end
   return a.id<b.id
  end)
  local first=1
  while first<#partial do
   local last=first
   while last<#partial and partial[last+1].wcid==partial[first].wcid do last=last+1 end
   if last>first then
    local target,source=partial[first],partial[last]
    return issue(s,p,'it_merge',source,math.min(source.count,target.max_stack-target.count),target.id)
   end
   first=last+1
  end
  return stop('Merge complete: '..operations..' confirmed merges.')
 end
 if mode=='give' or mode=='profile' then
  local target,isplayer=give_target(s,p,'target');if not target then return stop('Select a nearby recipient (within 5 m).') end
  if not queue then
   queue={};total=0
   for _,i in ipairs(inv) do if usable(i) and (mode=='profile' or i.name==p.give_name) then queue[#queue+1]={id=i.id,count=i.count} end end
   table.sort(queue,function(a,b)return a.id<b.id end)
   if mode=='give' then for _,q in ipairs(queue) do total=total+q.count end; if (p.give_limit or 0)>0 then total=math.min(total,p.give_limit) end end
  end
  while #queue>0 do
   if not workavailable() then return status('Checking transfer profile') end
   local q=queue[1];local item=find(inv,q.id)
   if not usable(item) or (isplayer and item.attuned) then table.remove(queue,1)
   else
    local amount=math.min(q.count,item.count)
    if mode=='profile' then
     local rule=evaluate(s,p.profile_rules or {},item)
     if rule=='pending' then return status('Checking transfer profile') end
     if rule=='identify' then return assess(s,item) end
     if not keeps(rule) or (rule.keep_up_to or 0)>0 then amount=0 end
    elseif (p.give_limit or 0)>0 then amount=math.min(amount,p.give_limit-completed) end
    if amount>0 then return issue(s,p,'it_give',item,amount,target) end
    table.remove(queue,1)
   end
  end
  return stop('Complete: '..completed..' items given.')
 end
 if mode=='shop' or mode=='restock' then
  if (s.vendor or 0)==0 then return stop('Open a vendor before starting.') end
  if p.vendor_id and p.vendor_id~=s.vendor then return stop('Vendor changed; transaction stopped.') end
  if mode=='shop' then
   local stock;for _,i in ipairs(s.vendor_stock or {}) do if i.name==p.buy_name then stock=i;break end end
   if not stock then
    if p.buy_name and p.buy_name~='' then return stop('This vendor does not sell '..p.buy_name) end
    for _,i in ipairs(inv) do if usable(i) and i.can_sell and i.name==p.sell_name then return issue(s,p,'it_sell',i,i.count,s.vendor) end end
    return stop('Sale complete.')
   end
   local left=(p.buy_limit or 0)>0 and p.buy_limit-completed or 100000
   total=(p.buy_limit or 0)>0 and p.buy_limit or 0
   if left<=0 then return stop('Complete: '..completed..' items purchased.') end
   return purchase(s,p,stock,left)
  end
  local missing={};local stock_names,stock_classes={},{}
  for _,i in ipairs(s.vendor_stock or {}) do stock_names[i.name]=i;stock_classes[i.wcid]=i end
  for _,r in ipairs(p.restock or {}) do
   local needed=math.max(0,(r.quantity or 0)-count(inv,r.name,r.wcid))
   if needed>0 then
    local stock=r.wcid and stock_classes[r.wcid] or stock_names[r.name];if stock and stock.name~=r.name then stock=nil end
    if stock then return purchase(s,p,stock,needed) end
    missing[#missing+1]=r.name
   end
  end
  return stop(#missing==0 and 'Restock complete.' or 'Available supplies restocked; vendor does not stock '..table.concat(missing,', '):sub(1,600))
 end
 if mode=='chest' then
  local chest=find(s.it_objects,p.chest_id)
  if not chest or chest.name~=p.chest_name or not chest.chest or chest.distance>5 then return stop('Select a nearby chest (within 5 m).') end
  local rules=p.chest_rules or {}
  if stage=='open' then
   if (s.container or 0)~=0 and s.container~=chest.id then return stop('Close the other container before starting.') end
   if s.container==chest.id then stage='loot'
   elseif p.key_name and p.key_name~='' and chest.locked~=false then
    local key;for _,i in ipairs(inv) do if usable(i) and i.name==p.key_name then key=i;break end end
    if not key then return stop('No matching keys remain.') end
    if not chest.identified then return assess(s,chest) end
    stage=#rules==0 and 'opener' or 'open'
    return issue(s,p,'it_key',key,1,chest.id)
   elseif #rules==0 then stage='opener'
   else return issue(s,p,'it_open',chest,1,chest.id) end
  end
  if stage=='opener' then
   -- A separate looter owns opening/closing. Wait for the chest to relock.
   if chest.locked then stage='open' end
   if (p.chest_cycles or 1)>0 and cycle>=(p.chest_cycles or 1) then return stop('Chest unlocking complete.') end
   if s.time>=refresh_at then refresh_at=s.time+3;return {action='it_refresh_chest',target=chest.id,status='Checking chest lock'} end
   return status('Chest unlocked; waiting for the looting character')
  end
  if stage=='loot' then
   if s.container~=chest.id then stage='open';return status('Waiting for chest') end
   if s.contents_ready==false then return status('Waiting for chest contents') end
   for index=chest_scan,#(s.contents or {}) do
    local item=s.contents[index];chest_scan=index
    local rule=evaluate(s,rules,item,"chest")
    if rule=='pending' then return status('Checking chest loot rules') end
    if rule=='identify' then return assess(s,item) end
    if keeps(rule) and not chest_seen[item.id] then
     local have=count(inv,item.name,rule.count_by_name and 0 or item.wcid)
     local amount=(rule.keep_up_to or 0)>0 and math.min(item.count,math.max(0,rule.keep_up_to-have)) or item.count
     if amount>0 then chest_seen[item.id]=true;chest_scan=1;return issue(s,p,'it_pickup',item,amount,chest.id) end
    end
   end
   stage='mules';return {action='it_close',target=chest.id,status='Chest looted; sorting recipients'}
  end
  if stage=='mules' then
   for _,key in ipairs({'mule1','mule2'}) do
    local target,isplayer=give_target(s,p,key)
    if (p[key..'_id'] or 0)~=0 then
     if not target then return stop('Recipient '..key..' moved away; chest loot remains in your packs.') end
     for index=mule_scan[key],#inv do local item=inv[index];mule_scan[key]=index;if (received[item.id] or 0)>0 and usable(item) and not (isplayer and item.attuned) then
      local rule=evaluate(s,p[key..'_rules'] or {},item,key)
      if rule=='pending' then return status('Checking '..key..' profile') end
      if rule=='identify' then return assess(s,item) end
      if keeps(rule) and (rule.keep_up_to or 0)==0 then local amount=math.min(received[item.id],item.count);mule_scan[key]=1;return issue(s,p,'it_give',item,amount,target) end
     end end
    end
   end
   cycle=cycle+1
   if (p.chest_cycles or 1)>0 and cycle>=(p.chest_cycles or 1) then return stop('Complete: '..cycle..' chest cycles.') end
   next_at=s.time+math.max(1,p.chest_delay or 3);stage='open';chest_scan=1;chest_seen={};mule_scan={mule1=1,mule2=1};scans={};identified={};received={};baseline=nil;return {action='it_refresh_chest',target=chest.id,status='Waiting for next chest cycle'}
  end
 end
 return stop('Choose an Item Tools activity.')
end
