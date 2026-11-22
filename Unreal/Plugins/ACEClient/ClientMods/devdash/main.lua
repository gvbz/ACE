-- Development Dashboard: in-game test controls for the client's personal. console
-- variables, through the dev service (permission dev_tools). Those values are not
-- saved: the variables live as long as the client runs.
-- The Admin tab sends server admin commands through the chat service (permission
-- say), one per second. Only its saved locations are kept, with state. It reads the
-- server's place list from chat (permission chat, one subscribed line) for suggestions.

-- Tabs and controls. min, max, step and default come from each variable's clamp and
-- default in the client source; the default here is only shown while the client
-- has not answered yet, Reset tab uses the default the client reports.
local follow={"Follow option","Off","On"}
local follow_values={-1,0,1}

-- Fog tab (personal.FogColor.*). Selects carry their option names and values. Per-weather
-- controls have weather (shown only for the weather picked in Edit weather), set ("palettes"
-- or "tables": shown only with that color set) and custom (shown only while that window's
-- palette is Custom).
local fog_weathers={"Clear","Sunny","Cloudy","Rainy"}
local presets={"AC2","dawn and dusk","Dawn to Dusk","Dusk and Dawn","From Dusk to Dawn","Custom"}
local preset_values={0,1,2,3,4,5}
local fog_current_name="personal.FogColor.Current"
local fog_controls={
    {id="fc_draw",text="Fog colors",name="personal.FogColor.Draw",kind="select",default=-1},
    {id="fc_strength",text="Strength",name="personal.FogColor.Strength",kind="slider",min=0,max=1,step=0.05,default=1},
    -- Saturation: -1 follows the Fog Color Saturation option; the slider then shows the value in use
    -- (from Current, else follow). Moving it sets an override; Reset tab sets -1 again.
    {id="fc_saturation",text="Saturation",name="personal.FogColor.Saturation",kind="slider",min=0,max=1.5,step=0.05,default=-1,follow=0.35},
    {id="fc_window",text="Window",name="personal.FogColor.Window",kind="slider",min=0.5,max=2,step=0.05,default=1.5},
    {id="fc_storm_fade",text="Storm fade",name="personal.FogColor.StormFade",kind="slider",min=0,max=1,step=0.05,default=0.35},
    {id="fc_variation",text="Day variation",name="personal.FogColor.Variation",kind="slider",min=0,max=1,step=0.05,default=0.5},
    {id="fc_day_offset",text="Preview day",name="personal.FogColor.DayOffset",kind="slider",min=-10,max=10,step=1,default=0},
    {id="fc_weather",text="Weather",name="personal.FogColor.Weather",kind="select",default=-1,
        options={"Follow server","Clear","Sunny","Cloudy","Rainy"},values={-1,0,1,2,3}},
    {id="fc_set",text="Color set",name="personal.FogColor.Set",kind="select",default=1,
        options={"Palettes","Atmospheric tables"},values={0,1}},
    {id="fc_column",text="Column",name="personal.FogColor.Column",kind="select",default=0,set="tables",
        options={"Horizon","Mid-sky","Zenith"},values={0,1,2}},
    {id="fc_table_brightness",text="Use table brightness",name="personal.FogColor.TableBrightness",kind="checkbox",default=0,set="tables"},
    {id="fc_split_haze",text="Separate horizon band",name="personal.FogColor.SplitHaze",kind="checkbox",default=1,set="tables"},
    {id="fc_haze_column",text="Band column",name="personal.FogColor.HazeColumn",kind="select",default=1,set="tables",split=true,
        options={"Horizon","Mid-sky","Zenith"},values={0,1,2}}}
local fog_defaults={Clear={1,4,1,0.85,0.8},Sunny={3,1,3,0.9,0.85},Cloudy={4,2,2,0.85,0.8},Rainy={2,2,2,0.75,0.75}} -- table, sunrise palette, sunset palette, sunrise and sunset brightness
for _,w in ipairs(fog_weathers) do
    local p,lw="personal.FogColor."..w..".","fc_"..string.lower(w).."_"
    local d=fog_defaults[w]
    for _,c in ipairs({
        {id=lw.."sunrise_palette",text="Sunrise palette",name=p.."SunrisePalette",kind="select",default=d[2],set="palettes",options=presets,values=preset_values},
        {id=lw.."sunrise_hue",text="Sunrise hue",name=p.."SunriseHue",kind="slider",min=0,max=360,step=1,default=15,set="palettes",custom="sunrise"},
        {id=lw.."sunrise_sat",text="Sunrise saturation",name=p.."SunriseSat",kind="slider",min=0,max=1,step=0.01,default=0.45,set="palettes",custom="sunrise"},
        {id=lw.."sunset_palette",text="Sunset palette",name=p.."SunsetPalette",kind="select",default=d[3],set="palettes",options=presets,values=preset_values},
        {id=lw.."sunset_hue",text="Sunset hue",name=p.."SunsetHue",kind="slider",min=0,max=360,step=1,default=20,set="palettes",custom="sunset"},
        {id=lw.."sunset_sat",text="Sunset saturation",name=p.."SunsetSat",kind="slider",min=0,max=1,step=0.01,default=0.7,set="palettes",custom="sunset"},
        {id=lw.."table",text="Table",name=p.."Table",kind="select",default=d[1],set="tables",
            options={"Clear sky","Overcast and rainy","Hazy","Foggy"},values={1,2,3,4}},
        {id=lw.."sunrise_bright",text="Sunrise brightness",name=p.."SunriseBright",kind="slider",min=0.5,max=1.5,step=0.05,default=d[4]},
        {id=lw.."sunset_bright",text="Sunset brightness",name=p.."SunsetBright",kind="slider",min=0.5,max=1.5,step=0.05,default=d[5]}}) do
        c.weather=w;fog_controls[#fog_controls+1]=c
    end
end
-- Lighting tab (personal.LightDir.*): smooth light direction with the imported AC2 sky, and its status line.
local light_current_name="personal.LightDir.Current"
local light_controls={
    {id="ld_smooth",text="Smooth light direction",name="personal.LightDir.Smooth",kind="checkbox",default=1},
    {id="ld_horizon_fade",text="Horizon fade",name="personal.LightDir.HorizonFade",kind="slider",min=5,max=45,step=1,default=20},
    {id="ld_night_turn",text="Night turn",name="personal.LightDir.NightTurn",kind="slider",min=0.01,max=0.25,step=0.01,default=0.1}}
local pages={
    {id="far_terrain",text="Far Terrain",controls={
        {id="ft_wireframe",text="Wireframe",name="personal.FarTerrain.Wireframe",kind="checkbox",default=0},
        {id="ft_freeze",text="Freeze",name="personal.FarTerrain.Freeze",kind="checkbox",default=0}}},
    {id="far_scenery",text="Far Scenery",controls={
        {id="fs_kinds",text="Test colors",name="personal.FarWindows.Kinds",kind="checkbox",default=0},
        {id="fs_fade",text="Fade",name="personal.FarScenery.Fade",kind="checkbox",default=1},
        {id="fs_size",text="Size factor",name="personal.FarScenery.SizeFactor",kind="slider",min=1,max=2000,step=1,default=400},
        {id="fs_detail",text="Detail levels",name="personal.FarScenery.Detail",kind="checkbox",default=1},
        {id="fs_middle",text="Middle ring",name="personal.FarScenery.MiddleRing",kind="slider",min=0,max=24,step=1,default=4},
        {id="fs_simplest",text="Simplest ring",name="personal.FarScenery.SimplestRing",kind="slider",min=0,max=24,step=1,default=5},
        {id="fs_group",text="Group",name="personal.FarScenery.Group",kind="slider",min=1,max=8,step=1,default=1},
        {id="fs_budget",text="Budget ms",name="personal.FarScenery.BudgetMs",kind="slider",min=0.1,max=100,step=0.1,default=1},
        {id="fs_radius",text="Radius override",name="personal.FarScenery.Radius",kind="slider",min=0,max=24,step=1,default=0}}},
    {id="far_rooms",text="Far Rooms",controls={
        {id="fr_rooms",text="Rooms",name="personal.FarWindows.Rooms",kind="select",default=-1},
        {id="fr_draw",text="Draw",name="personal.FarWindows.Draw",kind="select",default=-1},
        {id="fr_fog_near",text="Room fog near",name="personal.FarWindows.RoomFogNear",kind="slider",min=0,max=5000,step=10,default=200},
        {id="fr_fog_far",text="Room fog far",name="personal.FarWindows.RoomFogFar",kind="slider",min=0,max=5000,step=10,default=600},
        {id="fr_lift",text="Room lift",name="personal.FarWindows.RoomLift",kind="slider",min=0,max=100,step=0.5,default=3},
        {id="fr_door",text="Door distance",name="personal.FarWindows.DoorDistance",kind="slider",min=0,max=600,step=1,default=96},
        {id="fr_always",text="Rooms always",name="personal.FarWindows.RoomsAlways",kind="checkbox",default=0}},
     commands={
        {id="fr_mark_door",text="Mark door",name="personal.FarWindows.Mark",args="door"},
        {id="fr_mark_open",text="Mark open",name="personal.FarWindows.Mark",args="open"},
        {id="fr_mark_clear",text="Clear mark",name="personal.FarWindows.Mark",args="clear"}}},
    {id="fog",text="Fog",controls=fog_controls,fog=true},
    {id="lighting",text="Lighting",controls=light_controls,current="ld_current"},
    {id="shadows",text="Shadows"},
    {id="performance",text="Performance"},
    {id="admin",text="Admin",admin=true}}
local frame_rate={id="log_frame_rate",text="Log frame rate",name="personal.FarScenery.LogFrameRate"}

-- Admin tab. The ACE server runs a Talk line as a command only when it starts with @
-- (GameActionTalk.cs); the chat box turns / into @, the chat service sends text as is.
local weather_buttons={{"adm_w_clear","Clear","@weather clear"},{"adm_w_sunny","Sunny","@weather sunny"},{"adm_w_cloudy","Cloudy","@weather cloudy"},
    {"adm_w_rainy","Rainy","@weather rainy"},{"adm_w_storm","Storm","@weather passing"}}
local time_buttons={{"adm_t_dawn","Dawn","@time set dawn"},{"adm_t_day","Day","@time set day"},{"adm_t_dusk","Dusk","@time set dusk"},{"adm_t_night","Night","@time set night"}}
local fixed={adm_sky_status="@weather status",adm_stop_timelapse="@timelapse stop",adm_home="@home",adm_myloc="@myloc",
    adm_list_places="@telepoi list",adm_god="@god",adm_ungod="@ungod",adm_heal="@heal",adm_buff="@buff",adm_smite="@smite"}
for _,b in ipairs(weather_buttons) do fixed[b[1]]=b[3] end
for _,b in ipairs(time_buttons) do fixed[b[1]]=b[3] end
local no_locations="No saved locations"
local admin,locations,state_dirty={},{},false -- locations: {name,cell,x,y,z}, sorted by name
local chat_queue,last_chat={},-1e9
local places,places_asked,shown=nil,false,{} -- names from @telepoi list (this session); suggestions on screen

local controls,buttons,resets={},{},{}
for _,page in ipairs(pages) do
    for _,c in ipairs(page.controls or {}) do controls[c.id]=c end
    for _,b in ipairs(page.commands or {}) do buttons[b.id]=b end
    if page.controls then resets["reset_"..page.id]=page end
end
buttons[frame_rate.id]=frame_rate

local info=nil          -- name -> entry from list; nil until the client answers
local values={}         -- name -> value shown (a number)
local pending,pending_count={},0 -- name -> value for the next set
local queue={}          -- list, get and run requests waiting to be sent
local inflight={}       -- request id -> the request, for replies and retries
local last_set={}       -- name -> id of the last set that carried it
local serial,next_send,player=0,0,false
local status="Reading the client's values..."
local edit_weather="Clear" -- the weather whose Fog controls are shown
local fog_current=nil       -- personal.FogColor.Current, read every two seconds
local light_current=nil     -- personal.LightDir.Current, read with it
local next_poll=0

-- A number in Current after key (for example "saturation " -> 0.30); nil if Current is not read or has none.
local function current_number(key)
    if not fog_current then return nil end
    local at=string.findplain(fog_current,key)
    if not at then return nil end
    return tonumber(string.sub(fog_current,at+#key,at+#key+3))
end
local function number(v)
    if type(v)=="boolean" then return v and 1 or 0 end
    if v=="true" then return 1 elseif v=="false" then return 0 end
    return tonumber(v)
end
local function value_of(c)
    local v=values[c.name];if v==nil then v=c.default end;return v
end
-- A select's value for an option name, and the option name for a value (the first if none matches).
local function select_value(c,label)
    local options,list=c.options or follow,c.values or follow_values
    for i,o in ipairs(options) do if o==label then return list[i] end end
    return nil
end
local function select_label(c,v)
    local options,list=c.options or follow,c.values or follow_values
    for i,x in ipairs(list) do if x==v then return options[i] end end
    return options[1]
end
local function usable(name,kind)
    local e=info and info[name]
    return e~=nil and e.kind==kind and not (kind=="variable" and e.read_only)
end
local function tip(name)
    local e=info and info[name]
    if e then return e.help or "" end
    return info and ("Not in this client: "..name) or name
end
local function clean(c,v)
    v=number(v);if v==nil then return nil end
    local e=info and info[c.name]
    if e and (e.type=="int" or e.type=="bool") then v=math.floor(v+0.5) end
    return v
end
local function put(name,v)
    if pending[name]==nil then pending_count=pending_count+1 end
    pending[name]=v;values[name]=v
end
local function request(r)
    r.service=r.service or "dev";serial=serial+1;r.id=r.service.."_"..r.operation.."_"..serial;return r
end
local function enqueue(r)
    if #queue<16 then queue[#queue+1]=r else status="Too many requests waiting; try again." end
end
-- note: the status line when it is sent, instead of "Sent: <text>".
local function say(text,note)
    if #chat_queue<16 then chat_queue[#chat_queue+1]={text=text,note=note} else status="Too many commands waiting; try again." end
end
local function trim(t)
    t=tostring(t or "");local a,b=1,#t
    while a<=b and string.byte(t,a)<=32 do a=a+1 end
    while b>=a and string.byte(t,b)<=32 do b=b-1 end
    return string.sub(t,a,b)
end
local function sort_locations() table.sort(locations,function(a,b) return a.name<b.name end) end
local function load_locations(st)
    locations={}
    local list=type(st)=="table" and st.locations
    if type(list)~="table" then return end
    for _,l in ipairs(list) do
        if #locations<50 and type(l)=="table" and type(l.name)=="string" and l.name~="" and tonumber(l.cell)
            and tonumber(l.x) and tonumber(l.y) and tonumber(l.z) then
            locations[#locations+1]={name=l.name,cell=math.floor(tonumber(l.cell)),x=tonumber(l.x),y=tonumber(l.y),z=tonumber(l.z)}
        end
    end
    sort_locations()
end
local function start(s)
    info=nil;values={};pending={};pending_count=0;queue={};inflight={};last_set={}
    status="Reading the client's values..."
    edit_weather="Clear";fog_current=nil;light_current=nil;next_poll=0
    enqueue({operation="list"})
    admin={speed=4,place="",place_serial=0,name="",name_serial=0,location=nil,attackable=false,cloaked=false}
    chat_queue={};load_locations(s.state);state_dirty=false;places,places_asked,shown=nil,false,{}
end
local function on_list(e)
    info={}
    for _,entry in ipairs(e.entries or {}) do
        if type(entry)=="table" and entry.name then info[entry.name]=entry end
    end
    local missing={}
    for _,c in pairs(controls) do
        local entry=info[c.name]
        if entry and entry.kind=="variable" then values[c.name]=number(entry.value)
        else missing[#missing+1]=c.name end
    end
    for _,b in pairs(buttons) do
        if not usable(b.name,"command") then missing[#missing+1]=b.name end
    end
    if info[fog_current_name] then fog_current=tostring(info[fog_current_name].value) end
    if info[light_current_name] then light_current=tostring(info[light_current_name].value) end
    if #missing>0 then
        table.sort(missing);local seen,list={},{}
        for _,n in ipairs(missing) do if not seen[n] then seen[n]=true;list[#list+1]=n end end
        status="Not in this client (shown disabled): "..table.concat(list,", ")
    else
        status="Ready. Changes apply at once; test values are not saved."
    end
end
local function on_values(e,id)
    for name,v in pairs(e.values or {}) do
        -- A newer change of the same variable wins over an older reply.
        if name==fog_current_name then fog_current=tostring(v)
        elseif name==light_current_name then light_current=tostring(v)
        elseif pending[name]==nil and (id==nil or last_set[name]==nil or last_set[name]==id) then values[name]=number(v) end
    end
end
local function failed(r,e)
    local err=tostring(e.error or "Request failed.")
    if r.service=="chat" then
        -- A rate-limited batch did not run; the chat service refuses a second send within one second.
        if string.contains(err,"rate limit") then table.insert(chat_queue,1,{text=r.text,retried=r.retried});return end
        status="Refused: "..r.text.." ("..err..")"
        if string.contains(err,"one send per second") and not r.retried then
            table.insert(chat_queue,1,{text=r.text,retried=true});status=status.." Sending it again."
        end
        return
    end
    if string.contains(err,"rate limit") then
        -- The request did not run: send it again.
        if r.operation=="set" then
            for name,v in pairs(r.values) do if pending[name]==nil then put(name,v) end end
        else table.insert(queue,1,{operation=r.operation,names=r.names,text=r.text}) end
        return
    end
    status=err
    if r.operation=="list" then return end
    if r.operation=="set" then
        -- Nothing was set: show what the client really uses.
        local names={}
        for name in pairs(r.values) do names[#names+1]=name end
        enqueue({operation="get",names=names})
    end
end
local function reset(page)
    local count=0
    for _,c in ipairs(page.controls) do
        if usable(c.name,"variable") then
            local v=clean(c,info[c.name].default)
            if v~=nil then put(c.name,v);count=count+1 end
        end
    end
    status=count>0 and (page.text..": "..count.." values back to their defaults.") or (page.text..": nothing to reset.")
end

local function selected_location()
    for _,l in ipairs(locations) do if l.name==admin.location then return l end end
    return locations[1]
end
-- Same form as the server's own example: @teleloc 0x7F0401AD 12.319900 -28.482000 0.005000
local function teleloc(l)
    return string.format("@teleloc 0x%08X %.6f %.6f %.6f",l.cell,l.x,l.y,l.z)
end
local function save_location(pos)
    local name=trim(admin.name)
    if name=="" then status="Type a name first, then Save location.";return end
    if #name>64 then status="Names are at most 64 characters.";return end
    if type(pos)~="table" or not tonumber(pos.cell) or tonumber(pos.cell)==0 then status="No position yet; try again in a moment.";return end
    local entry={name=name,cell=math.floor(tonumber(pos.cell)),x=tonumber(pos.x) or 0,y=tonumber(pos.y) or 0,z=tonumber(pos.z) or 0}
    local found
    for i,l in ipairs(locations) do if l.name==name then found=i end end
    if not found and #locations>=50 then status="50 locations saved: delete one first.";return end
    if found then locations[found]=entry else locations[#locations+1]=entry end
    sort_locations();state_dirty=true
    -- A new Name input (new ID) is empty, even while the old one has focus.
    admin.location=name;admin.name="";admin.name_serial=admin.name_serial+1
    status=(found and "Replaced " or "Saved ")..name.."."
end
local function delete_location()
    local l=selected_location();if not l then return end
    for i,v in ipairs(locations) do if v==l then table.remove(locations,i);break end end
    admin.location=nil;state_dirty=true;status="Deleted "..l.name.."."
end
-- Map coordinates as typed or pasted, such as 83.0S, 93.2E: "<number>N|S", spaces or a comma,
-- "<number>E|W". Returns "83.0S 93.2E" for @tele (CommandParameterHelpers.cs reads that form), or nil.
local function coordinates(text)
    local t=string.upper(trim(text));local i,n,parts=1,#t,{}
    local function digit(c) return c and ((c>=48 and c<=57) or c==46) end
    for k,letters in ipairs({"NS","EW"}) do
        if k==2 then while i<=n and (string.byte(t,i)==32 or string.byte(t,i)==44) do i=i+1 end end
        local a=i
        while i<=n and digit(string.byte(t,i)) do i=i+1 end
        if i==a or i>n or not string.contains(letters,string.sub(t,i,i)) then return nil end
        parts[k]=string.sub(t,a,i);i=i+1
    end
    if i<=n or not tonumber(string.sub(parts[1],1,-2)) or not tonumber(string.sub(parts[2],1,-2)) then return nil end
    return parts[1].." "..parts[2]
end
-- The server's reply to @telepoi list: "All POIs: <name>, <name>, ..." (AdminCommands.cs).
local function read_places(text)
    local prefix="All POIs: "
    if type(text)~="string" or string.sub(text,1,#prefix)~=prefix then return end
    local list,at=string.sub(text,#prefix+1),1;places={}
    while at<=#list do
        local stop=string.findplain(list,", ",at)
        local name=trim(string.sub(list,at,(stop or #list+1)-1))
        if name~="" then places[#places+1]=name end
        if not stop then break end
        at=stop+2
    end
end
-- Up to six names for the typed text: names starting with it, then names containing it, case ignored.
local function suggestions()
    local q=string.lower(string.sub(trim(admin.place),1,100))
    if q=="" or not places or coordinates(admin.place) then return {} end
    local starts,contains={},{}
    for _,name in ipairs(places) do
        local l=string.lower(name)
        if l==q then return {} end
        if string.sub(l,1,#q)==q then starts[#starts+1]=name elseif string.contains(l,q) then contains[#contains+1]=name end
    end
    local function by_name(a,b) return string.lower(a)<string.lower(b) end
    table.sort(starts,by_name);table.sort(contains,by_name)
    local result={}
    for _,list in ipairs({starts,contains}) do
        for _,name in ipairs(list) do if #result<6 then result[#result+1]=name end end
    end
    return result
end
local function admin_change(id,v)
    if id=="adm_speed" and tonumber(v) then admin.speed=math.max(1,math.min(256,math.floor(tonumber(v)+0.5)))
    elseif id=="adm_location" then admin.location=v
    elseif id=="adm_place_"..admin.place_serial then
        admin.place=tostring(v or "")
        if not places and not places_asked and trim(admin.place)~="" and not coordinates(admin.place) then
            places_asked=true;say(fixed.adm_list_places,"Getting the list of places.")
        end
    elseif id=="adm_name_"..admin.name_serial then admin.name=tostring(v or "")
    elseif id=="adm_attackable" then admin.attackable=v==true;say("@attackable "..(admin.attackable and "on" or "off"))
    elseif id=="adm_cloaked" then admin.cloaked=v==true;say("@cloak "..(admin.cloaked and "on" or "off"))
    end
end
local function admin_click(id,pos)
    if fixed[id] then say(fixed[id])
    elseif id=="adm_start_timelapse" then say(string.format("@timelapse %d",admin.speed))
    elseif id=="adm_reset_sky" then say("@weather reset");say("@time reset");say("@timelapse reset")
    elseif id=="adm_go_place" then
        local place=string.sub(trim(admin.place),1,100);local where=coordinates(place)
        if place=="" then status="Type a place first, for example Arwic, or coordinates such as 83.0S, 93.2E."
        elseif where then say("@tele "..where) else say("@telepoi "..place) end
    elseif string.sub(id,1,9)=="adm_sugg_" then
        -- A new Place input (new ID) shows the picked name, even while the old one has focus.
        local name=shown[tonumber(string.sub(id,10))]
        if name then admin.place=name;admin.place_serial=admin.place_serial+1 end
    elseif id=="adm_save" then save_location(pos)
    elseif id=="adm_go" then local l=selected_location();if l then say(teleloc(l)) end
    elseif id=="adm_delete" then delete_location()
    end
end
-- Layout. The host insets row children by 3 px (ACEPluginCompanionPanel.cpp), so every control
-- sits in a row: then all controls of a tab, or of a section, start at the same left edge.
local function row(id,children,extra)
    local r={id=id,type="row",children=children}
    for k,v in pairs(extra or {}) do r[k]=v end
    return r
end
local function line(c) return row(c.id.."_line",{c}) end
-- Three equal columns; empty text controls fill the unused ones. A button in a column of its
-- own width never wraps its text (a row sized to its content can).
local function thirds(id,children,extra)
    local list={}
    for i=1,3 do list[i]=children[i] or {id=id.."_fill"..i,text=""} end
    return row(id,list,extra)
end
local function admin_ui()
    local names={}
    for _,l in ipairs(locations) do names[#names+1]=l.name end
    local current=selected_location();local has=current~=nil
    local function b(id,text,tooltip,enabled) return {id=id,type="button",text=text,tooltip=tooltip,enabled=enabled~=false} end
    -- A section in a row would pass the output's 12 nesting levels (a select's options), so it
    -- gets padding 7 instead: the row's 2, its slot's 3 and the control's own 2.
    local function section(id,text,children) return {id=id,type="section",text=text,expanded=true,padding=7,children=children} end
    local weather,times={},{}
    for _,w in ipairs(weather_buttons) do weather[#weather+1]=b(w[1],w[2],w[3]) end
    for _,t in ipairs(time_buttons) do times[#times+1]=b(t[1],t[2],t[3]) end
    shown=suggestions()
    local poi={row("adm_place_row",{{id="adm_place_"..admin.place_serial,type="input",text="Place",value=admin.place,
        placeholder="Arwic, or 83.0S, 93.2E",live=true},b("adm_go_place","Go to place","@telepoi <place>, or @tele <coordinates>")},{valign="bottom"})}
    for r=0,1 do
        local cells={}
        for c=1,3 do local i=r*3+c;if shown[i] then cells[c]=b("adm_sugg_"..i,shown[i],"Puts "..shown[i].." in Place.") end end
        if #cells>0 then poi[#poi+1]=thirds("adm_sugg_row"..(r+1),cells) end
    end
    poi[#poi+1]=line(b("adm_list_places","List places",fixed.adm_list_places))
    return {
        line({id="adm_note",text="Admin commands work only for an admin character (local server)."}),
        section("adm_sky","Sky",{
            line({id="adm_weather_label",text="Weather"}),
            row("adm_weather_row",weather),
            line({id="adm_time_label",text="Time of day"}),
            row("adm_time_row",times),
            line({id="adm_speed",type="slider",text="Timelapse speed",min=1,max=256,step=1,value=admin.speed}),
            row("adm_timelapse_row",{b("adm_start_timelapse","Start timelapse",string.format("@timelapse %d",admin.speed)),
                b("adm_stop_timelapse","Stop timelapse",fixed.adm_stop_timelapse)}),
            row("adm_sky_row",{b("adm_sky_status","Sky status",fixed.adm_sky_status),
                b("adm_reset_sky","Reset sky","@weather reset, @time reset, @timelapse reset")})}),
        section("adm_saved","Saved locations",{
            row("adm_location_row",{{id="adm_location",type="select",text="Location",options=has and names or {no_locations},
                value=has and current.name or no_locations,enabled=has},
                b("adm_go","Go",has and teleloc(current) or "@teleloc",has),b("adm_delete","Delete","Removes the selected location.",has)},{valign="bottom"}),
            row("adm_name_row",{{id="adm_name_"..admin.name_serial,type="input",text="Name",value=admin.name,live=true},
                b("adm_save","Save location","Saves the character's cell and position under the name.")},{valign="bottom"}),
            row("adm_home_row",{b("adm_home","Home",fixed.adm_home),b("adm_myloc","My location",fixed.adm_myloc)})}),
        section("adm_poi","Points of interest",poi),
        section("adm_character","Character",{
            row("adm_god_row",{b("adm_god","God mode",fixed.adm_god),b("adm_ungod","Normal mode",fixed.adm_ungod)}),
            row("adm_heal_row",{b("adm_heal","Heal",fixed.adm_heal),b("adm_buff","Buff",fixed.adm_buff)}),
            row("adm_flags_row",{{id="adm_attackable",type="checkbox",text="Attackable",tooltip="@attackable on / off",value=admin.attackable},
                {id="adm_cloaked",type="checkbox",text="Cloaked",tooltip="@cloak on / off",value=admin.cloaked}})}),
        section("adm_targets","Targets",{line(b("adm_smite","Smite target",fixed.adm_smite))})}
end
-- Fog tab: the status line, the shared controls, Edit weather, then that weather's controls of
-- the color set in use. Reset tab covers every Fog variable, shown or not.
local function fog_ui(control)
    local tables=value_of(controls.fc_set)==1
    local children={line({id="fc_current",text="Now: "..(fog_current or "waiting for the client.")})}
    local function shown(c)
        if c.set and (c.set=="tables")~=tables then return false end
        if c.weather and c.weather~=edit_weather then return false end
        if c.split and value_of(controls.fc_split_haze)~=1 then return false end
        if c.custom then
            local palette=controls["fc_"..string.lower(c.weather).."_"..c.custom.."_palette"]
            if value_of(palette)~=5 then return false end
        end
        return true
    end
    local edit_added=false
    for _,c in ipairs(fog_controls) do
        if c.weather and not edit_added then
            edit_added=true
            children[#children+1]=line({id="fc_edit_weather",type="select",text="Edit weather",options=fog_weathers,value=edit_weather,
                tooltip="Which weather's colors are shown below."})
        end
        if shown(c) then children[#children+1]=line(control(c)) end
    end
    return children
end

return function(s)
    if s.player~=player then player=s.player;start(s) end
    for _,c in ipairs(s.chat_events or {}) do read_places(c.raw_text) end
    for _,e in ipairs(s.events or {}) do
        if e.type=="result" and inflight[e.id] then
            local r=inflight[e.id];inflight[e.id]=nil
            if not e.ok then failed(r,e)
            elseif r.service=="chat" then -- sent; the status line already shows it
            elseif r.operation=="list" then on_list(e)
            elseif r.operation=="get" then on_values(e,nil)
            elseif r.operation=="set" then on_values(e,e.id)
            elseif r.operation=="run" then
                status=r.text==frame_rate.name and "Frame rate written to the log." or ("Ran "..r.text..".")
            end
        elseif e.type=="change" and controls[e.id] then
            local c=controls[e.id]
            if usable(c.name,"variable") then
                local v
                if c.kind=="select" then v=select_value(c,e.value) else v=clean(c,e.value) end
                if v~=nil then put(c.name,v) end
            end
        elseif e.type=="change" and e.id=="fc_edit_weather" then
            for _,w in ipairs(fog_weathers) do if w==e.value then edit_weather=w end end
        elseif e.type=="change" and type(e.id)=="string" and string.sub(e.id,1,4)=="adm_" then admin_change(e.id,e.value)
        elseif e.type=="click" then
            local b=buttons[e.id]
            if b and usable(b.name,"command") then
                enqueue({operation="run",text=b.args and (b.name.." "..b.args) or b.name})
            elseif resets[e.id] then reset(resets[e.id])
            elseif type(e.id)=="string" and string.sub(e.id,1,4)=="adm_" then admin_click(e.id,s.position) end
        end
    end

    -- Send: one admin command (at least 1 s after the last), all changes of this callback
    -- in one set, plus waiting requests, four at most.
    local requests
    local now=s.time or 0
    -- The Fog and Lighting tabs' status lines: personal.FogColor.Current and personal.LightDir.Current
    -- every two seconds, in one get.
    if info and now>=next_poll then
        local names={}
        for _,n in ipairs({fog_current_name,light_current_name}) do if info[n] then names[#names+1]=n end end
        if #names>0 then
            next_poll=now+2
            local waiting=false
            for _,r in ipairs(queue) do if r.operation=="get" and r.names and r.names[1]==names[1] and #r.names==#names then waiting=true end end
            if not waiting then enqueue({operation="get",names=names}) end
        end
    end
    local chat_ready=#chat_queue>0 and now-last_chat>=1
    if now>=next_send and (pending_count>0 or #queue>0 or chat_ready) then
        requests={}
        if chat_ready then
            local c=table.remove(chat_queue,1)
            local r=request({service="chat",operation="send",text=c.text})
            inflight[r.id]={service="chat",text=c.text,retried=c.retried};requests[#requests+1]=r;last_chat=now
            status=(c.note or ("Sent: "..c.text))..(#chat_queue>0 and (" ("..#chat_queue.." waiting)") or "")
        end
        if pending_count>0 then
            local r=request({operation="set",values=pending})
            for name in pairs(pending) do last_set[name]=r.id end
            pending,pending_count={},0
            inflight[r.id]=r;requests[#requests+1]=r
        end
        while #queue>0 and #requests<4 do
            local r=request(table.remove(queue,1));inflight[r.id]=r;requests[#requests+1]=r
        end
        next_send=now+0.5
    end

    local function control(c)
        local enabled=usable(c.name,"variable")
        local base={id=c.id,text=c.text,tooltip=tip(c.name),enabled=enabled}
        local v=value_of(c)
        if c.kind=="checkbox" then base.type="checkbox";base.value=(v or 0)~=0
        elseif c.kind=="select" then
            base.type="select";base.options=c.options or follow
            base.value=select_label(c,v)
        else
            base.type="slider";base.min=c.min;base.max=c.max;base.step=c.step
            if c.follow and (v==nil or v<0) then v=current_number("saturation ") or c.follow end
            base.value=math.max(c.min,math.min(c.max,v or c.default))
        end
        return base
    end
    local function button(b)
        return {id=b.id,type="button",text=b.text,tooltip=tip(b.name),enabled=usable(b.name,"command")}
    end
    local tabs={}
    for _,page in ipairs(pages) do
        local children={}
        if page.admin then children=admin_ui()
        elseif page.controls then
            if page.fog then children=fog_ui(control)
            else
                if page.current then children[1]=line({id=page.current,text="Now: "..(light_current or "waiting for the client.")}) end
                for _,c in ipairs(page.controls) do children[#children+1]=line(control(c)) end
            end
            if page.commands then
                local list={}
                for _,b in ipairs(page.commands) do list[#list+1]=button(b) end
                children[#children+1]=row(page.id.."_commands",list)
            end
            children[#children+1]=thirds(page.id.."_reset_row",{
                {id="reset_"..page.id,type="button",text="Reset tab",tooltip="Sets every variable of this tab back to the client's default.",enabled=info~=nil}})
        else
            children[1]=line({id=page.id.."_later",text="Added by a later step."})
        end
        tabs[#tabs+1]={id=page.id,type="column",text=page.text,children=children}
    end
    -- The tabs control and a tab's column put its rows 6 px further in than a row of the
    -- window (2 px padding each, plus the column's 2 px slot); padding 8 instead of 2 matches it.
    local ui={
        {id="status",text=status},
        {id="tabs",type="tabs",children=tabs},
        thirds("shared",{button(frame_rate)},{padding=8})}
    local out={ui=ui,requests=requests,status=status}
    if state_dirty then
        local list={}
        for i,l in ipairs(locations) do list[i]={name=l.name,cell=l.cell,x=l.x,y=l.y,z=l.z} end
        out.state={locations=list};state_dirty=false
    end
    return out
end
