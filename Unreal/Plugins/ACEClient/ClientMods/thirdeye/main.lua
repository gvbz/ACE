-- Passive, bounded observer. Only configuration is saved; world GUIDs and alert
-- suppression belong to this login. Matching is literal, never a regular expression.
local config, draft, exact, feedback = nil, "", false, ""
local cache, seen = {}, {}
local rule_page, detected_page, filter = 1, 1, ""
local alerts, alert_inflight, next_alert = {}, false, 0
local sound_pending, preview_pending, sound_inflight = false, false, nil
local next_sound, sound_sequence, sound_feedback = 0, 0, ""
local rule_size, detected_size, MAX_RULES = 6, 8, 128

local function trim(text)
    local first, last = 1, #text
    while first <= last and string.byte(text, first) <= 32 do first = first + 1 end
    while last >= first and string.byte(text, last) <= 32 do last = last - 1 end
    return string.sub(text, first, last)
end
local function short(text)
    local pos = utf8.offset(text, 65)
    return pos and string.sub(text, 1, pos - 1).."..." or text
end
local function button(id, text, enabled)
    return {id=id, type="button", text=text, enabled=enabled ~= false, auto_width=true, wrap=false}
end
local function page_controls(id, page, total, size)
    local prev, next = button(id.."_prev", "<", page > 1), button(id.."_next", ">", page*size < total)
    prev.tooltip, next.tooltip = "Previous page", "Next page"
    return {id=id, type="row", compact=true, children={prev,
        {id=id.."_label", text=page.." / "..math.max(1, math.ceil(total/size)), wrap=false}, next}}
end

return function(s)
    -- The host reports available content space in logical UI units. Keep every
    -- list row a single line (full name in its tooltip) so pages fit on resize.
    local height = s.panel and s.panel.height or 620
    local next_rules = math.max(1, math.min(16, math.floor((height-330)/54)))
    local next_detected = math.max(1, math.min(16, math.floor((height-230)/54)))
    rule_page = math.floor((rule_page-1)*rule_size/next_rules)+1
    detected_page = math.floor((detected_page-1)*detected_size/next_detected)+1
    rule_size, detected_size = next_rules, next_detected
    local migrated = false
    if not config then
        local saved = s.state or {}
        config = {rules={}, next_id=1, watching=saved.watching ~= false,
            chat_alerts=saved.chat_alerts ~= false, world_markers=saved.world_markers == true,
            map_markers=saved.map_markers ~= false, range=math.max(5, math.min(240, tonumber(saved.range) or 240)),
            sound_alerts=saved.sound_alerts == true,
            sound_source=saved.sound_source == "custom" and "custom" or "bundled",
            sound_asset=type(saved.sound_asset)=="string" and saved.sound_asset or "alert.wav",
            sound_label=type(saved.sound_label)=="string" and saved.sound_label or "Default chime",
            sound_volume=math.max(0, math.min(1, tonumber(saved.sound_volume) or .7))}
        if config.sound_source=="bundled" or config.sound_asset=="" then
            config.sound_source, config.sound_asset, config.sound_label = "bundled", "alert.wav", "Default chime"
        end
        migrated = saved.opacity ~= nil or saved.background_opacity ~= nil or saved.vr_opacity ~= nil or saved.vr_background_opacity ~= nil
        for _, r in ipairs(type(saved.rules)=="table" and saved.rules or {}) do
            if #config.rules == MAX_RULES then break end
            if type(r)=="table" and type(r.name)=="string" and #r.name>0 and #r.name<=256 then
                config.rules[#config.rules+1] = {id=config.next_id, name=r.name,
                    exact=r.exact==true, enabled=r.enabled~=false}
                migrated = migrated or r.plate ~= nil
                config.next_id = config.next_id + 1
            end
        end
    end
    local changed, rules_changed = migrated, false
    local selected
    for _, o in ipairs(s.objects or {}) do if o.id == s.selected then selected=o end end
    local function add(name)
        name=trim(name)
        if name=="" then feedback="Enter a name or select an object in the world."; return end
        if #name>256 then feedback="The name is too long (256 UTF-8 bytes maximum)."; return end
        for _, r in ipairs(config.rules) do
            if string.lower(r.name)==string.lower(name) and r.exact==exact then
                feedback="That match is already in your watch list."; return
            end
        end
        if #config.rules>=MAX_RULES then feedback="The watch list is full (128 entries)."; return end
        config.rules[#config.rules+1]={id=config.next_id, name=name, exact=exact, enabled=true}
        config.next_id=config.next_id+1
        rule_page=math.ceil(#config.rules/rule_size)
        draft=""; feedback="Added "..name; changed=true; rules_changed=true
    end
    for _, e in ipairs(s.events or {}) do
        if e.type=="result" and e.id=="detected_alert" then
            alert_inflight=false
            if e.ok or not string.contains(e.error or "","rate limit") then table.remove(alerts,1) end
            if not e.ok then feedback=e.error or "Could not display alert." end
        elseif e.type=="result" and sound_inflight and e.id==sound_inflight.id then
            local was_preview, cancelled, started=sound_inflight.preview, sound_inflight.cancelled, sound_inflight.started
            sound_inflight=nil
            if not e.ok and string.contains(e.error or "","rate limit") then
                if was_preview then preview_pending=true
                elseif not cancelled and config.watching and config.sound_alerts then sound_pending=true end
            elseif e.ok then
                next_sound=math.max(next_sound,started+math.max(0,math.min(10,tonumber(e.duration) or 0)))
                if was_preview then sound_feedback="Preview played." end
            else sound_feedback=e.error or "Could not play the alert sound." end
        elseif e.type=="sound_file" then
            if e.ok and type(e.asset)=="string" and e.asset~="" then
                config.sound_source, config.sound_asset = "custom", e.asset
                config.sound_label=type(e.name)=="string" and e.name or "Custom sound"
                sound_feedback="Custom sound selected. Use Test to preview it."
                changed=true
            elseif not e.cancelled then sound_feedback=e.error or "Could not select that sound." end
        elseif e.type=="change" then
            if e.id=="draft" then draft=tostring(e.value)
            elseif e.id=="exact" then exact=e.value==true
            elseif e.id=="filter" then filter=tostring(e.value); detected_page=1
            elseif e.id=="range" then config.range=math.max(5,math.min(240,tonumber(e.value) or 240)); changed=true
            elseif e.id=="sound_volume" then
                config.sound_volume=math.max(0,math.min(1,(tonumber(e.value) or 70)/100)); changed=true
            elseif e.id=="watching" or e.id=="chat_alerts" or e.id=="world_markers" or e.id=="map_markers" or e.id=="sound_alerts" then
                config[e.id]=e.value==true; changed=true
                if (e.id=="watching" or e.id=="sound_alerts") and not e.value then
                    sound_pending=false
                    if sound_inflight and not sound_inflight.preview then sound_inflight.cancelled=true end
                end
            elseif string.sub(e.id or "",1,8)=="enabled_" then
                local id=tonumber(string.sub(e.id,9))
                for _, r in ipairs(config.rules) do if r.id==id then r.enabled=e.value==true; changed=true; rules_changed=true end end
            end
        elseif e.type=="click" then
            if e.id=="add" then add(draft)
            elseif e.id=="add_selected" then add(selected and selected.name or "")
            elseif e.id=="rules_prev" then rule_page=rule_page-1
            elseif e.id=="rules_next" then rule_page=rule_page+1
            elseif e.id=="detected_prev" then detected_page=detected_page-1
            elseif e.id=="detected_next" then detected_page=detected_page+1
            elseif e.id=="test_sound" then preview_pending=true; sound_feedback="Preview requested..."
            elseif e.id=="default_sound" then
                config.sound_source, config.sound_asset, config.sound_label = "bundled", "alert.wav", "Default chime"
                sound_feedback="Default chime selected."; changed=true
            elseif string.sub(e.id or "",1,7)=="remove_" then
                local id=tonumber(string.sub(e.id,8))
                for i, r in ipairs(config.rules) do
                    if r.id==id then table.remove(config.rules,i); changed=true; rules_changed=true; feedback="Removed "..r.name; break end
                end
            end
        end
    end
    if rules_changed then cache={} end
    local exact_names, partial_names = {}, {}
    for _, r in ipairs(config.rules) do
        if r.enabled then
            local name=string.lower(r.name)
            if r.exact then exact_names[name]=true
            else partial_names[#partial_names+1]=name end
        end
    end
    local matches, markers, new_names, next_cache, next_seen = {}, {}, {}, {}, {}
    local new_count, scanning = 0, false
    local now=s.time or 0
    -- Brief awareness/range flicker must not repeatedly announce the same spawn.
    local retained=0
    for id, record in pairs(seen) do
        if now-record.time<30 and retained<1024 then next_seen[id]=record; retained=retained+1 end
    end
    if config.watching then
        for _, o in ipairs(s.objects or {}) do
            if not o.dead and o.distance<=config.range then
                local name=string.lower(o.name)
                local match=cache[name]
                if match==nil and workavailable() then
                    match=exact_names[name] or false
                    for _, rule in ipairs(partial_names) do
                        if match then break end
                        if string.contains(name,rule) then match=true end
                    end
                end
                if match==nil then scanning=true else next_cache[name]=match end
                if match then
                    matches[#matches+1]=o
                    local previous=seen[o.id]
                    if not previous or previous.name~=o.name or previous.wcid~=o.wcid then
                        new_count=new_count+1
                        if #new_names<3 then new_names[#new_names+1]=short(o.name).." ("..math.floor(o.distance).." m)" end
                    end
                    next_seen[o.id]={name=o.name, wcid=o.wcid, time=now}
                    if #markers<32 and (config.world_markers or config.map_markers) then
                        markers[#markers+1]={id=o.id,shape="diamond",color="green",world=config.world_markers,map=config.map_markers}
                    end
                end
            end
        end
    end
    cache, seen = next_cache, next_seen
    local requests={}
    if not config.watching or not config.chat_alerts then alerts={}; alert_inflight=false end
    if new_count>0 and config.chat_alerts then
        local text="Matching spawn detected: "..table.concat(new_names,", ")
        if new_count>#new_names then text=text.."; +"..(new_count-#new_names).." more (see Detected)." end
        if #alerts<8 then alerts[#alerts+1]=text else alerts[8]="More matching spawns detected; see Detected for the current list." end
    end
    if #alerts>0 and not alert_inflight and now>=next_alert then
        requests[1]={id="detected_alert",service="notice",operation="show",text=alerts[1]}
        alert_inflight=true; next_alert=now+1
    end
    -- A single chime represents a detection burst. Keep it independent of chat,
    -- and retry only a host rate limit, not an invalid or unavailable sound.
    if not config.watching or not config.sound_alerts then sound_pending=false
    elseif new_count>0 then sound_pending=true end
    if not sound_inflight and now>=next_sound and (preview_pending or sound_pending) then
        local preview=preview_pending
        preview_pending, sound_pending = false, false
        sound_sequence=sound_sequence+1
        local id=(preview and "preview_sound_" or "detected_sound_")..sound_sequence
        requests[#requests+1]={id=id,service="sound",operation="play",source=config.sound_source,
            asset=config.sound_asset,volume=config.sound_volume}
        sound_inflight={id=id,preview=preview,started=now}; next_sound=now+1
    end
    local visible={}
    for _, o in ipairs(matches) do
        if filter=="" or string.contains(string.lower(o.name),string.lower(string.sub(filter,1,256))) then visible[#visible+1]=o end
    end
    detected_page=math.max(1,math.min(detected_page,math.ceil(#visible/detected_size)))
    rule_page=math.max(1,math.min(rule_page,math.ceil(#config.rules/rule_size)))
    local detected={
        {id="count",text=config.watching and (#matches.." matching objects nearby") or "Watching paused"},
        {id="filter",type="input",text="Filter detected names",value=filter,live=true},
        page_controls("detected",detected_page,#visible,detected_size)
    }
    if #config.rules==0 then detected[#detected+1]={id="empty",text="Add names on Setup to begin watching."}
    elseif #visible==0 then detected[#detected+1]={id="empty",text="No matching objects in current awareness."} end
    if scanning then detected[#detected+1]={id="scanning",text="Scanning remaining names..."} end
    if s.objects_truncated then detected[#detected+1]={id="truncated",text="Crowded area: checking the nearest 512 known objects."} end
    local nav=s.plugins and s.plugins.waypoint and s.plugins.waypoint.enabled
    for i=(detected_page-1)*detected_size+1,math.min(#visible,detected_page*detected_size) do
        local o=visible[i]
        local key=string.format("%.0f",o.id)
        detected[#detected+1]={id="object_"..key,type="row",children={
            {id="select_"..key,type="button",text=o.name.." - "..string.format("%.1f m",o.distance),wrap=false,action="select",target=o.id,tooltip="Select "..o.name},
            {id="nav_"..key,type="button",text="Arrow",auto_width=true,wrap=false,action="waypoint",target=o.id,enabled=nav==true,tooltip="Point Waypoint at this object (requires Waypoint)"}
        }}
    end
    local setup={
        {id="draft",type="input",text="Name to watch",tooltip="Enter a literal name or part of a name.",value=draft,live=true},
        {id="exact",type="checkbox",text="Exact name",value=exact,tooltip="Names ignore case. Turn off for partial matches."},
        {id="add_row",type="row",children={button("add","Add name"),button("add_selected","Add selected",selected~=nil)}},
        {id="feedback",text=feedback,wrap=false,tooltip=feedback},
        page_controls("rules",rule_page,#config.rules,rule_size)
    }
    local options={
        {id="range",type="slider",text="Detection range (m)",min=5,max=240,step=5,value=config.range},
        {id="chat_alerts",type="checkbox",text="Green chat alerts",value=config.chat_alerts},
        {id="world_markers",type="checkbox",text="Mark matches in the world",value=config.world_markers},
        {id="map_markers",type="checkbox",text="Mark matches on Waypoint map",value=config.map_markers},
        {id="limits",text="Up to 32 nearby matches can be marked.",size=12}
    }
    local choose_sound=button("choose_sound","Choose WAV")
    choose_sound.action="sound_file"; choose_sound.tooltip="Choose a notification WAV: PCM16 mono/stereo, 8-48 kHz, up to 10 seconds and 1 MiB"
    local default_sound=button("default_sound","Use default",config.sound_source~="bundled")
    default_sound.tooltip="Use the included chime"
    local test_sound=button("test_sound","Test",sound_inflight==nil and not preview_pending and now>=next_sound)
    test_sound.tooltip="Preview this sound, even when automatic alerts are off"
    local audio={
        {id="sound_alerts",type="checkbox",text="Play a sound for new matches",value=config.sound_alerts},
        {id="sound_name",text="Sound: "..config.sound_label,wrap=false,ellipsis=true,tooltip=config.sound_label},
        {id="sound_buttons",type="row",children={choose_sound,default_sound,test_sound}},
        {id="sound_volume",type="slider",text="Alert volume (%)",min=0,max=100,step=5,value=math.floor(config.sound_volume*100+.5)},
        {id="sound_feedback",text=sound_feedback,wrap=false,ellipsis=true,tooltip=sound_feedback},
        {id="sound_help",text="Matches found together play one alert.",size=12}
    }
    for i=(rule_page-1)*rule_size+1,math.min(#config.rules,rule_page*rule_size) do
        local r=config.rules[i]
        setup[#setup+1]={id="rule_"..r.id,type="row",children={
            {id="enabled_"..r.id,type="checkbox",text=(r.exact and "Equals: " or "Contains: ")..r.name,wrap=false,tooltip=r.name,value=r.enabled},
            button("remove_"..r.id,"Remove")
        }}
    end
    local ui={{id="watching",type="checkbox",text="Watch for matches",value=config.watching},
        {id="tabs",type="tabs",children={{id="detected_tab",type="column",text="Detected",children=detected},
            {id="setup_tab",type="column",text="Setup",children=setup},
            {id="options_tab",type="column",text="Options",children=options},
            {id="audio_tab",type="column",text="Audio",children=audio}}}}
    if s.error and s.error~="" then ui[#ui+1]={id="error",text=s.error,color="red"} end
    -- Clear any retained plates from older Third Eye versions; other addons own
    -- their plate lists and opacity preferences independently.
    return {status=#matches.." matches",ui=ui,markers=markers,world_plates={},requests=requests,state=changed and config or nil}
end
