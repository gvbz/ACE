-- Lua controls the optional aid; the host draws cached terrain in both eyes.
local config
local defaults={enabled=true,slopes=true,water=true,radar=false,dungeon=false,
    opacity=.45,range=120,brightness=.3,
    slope_color={1,.08,.02},water_color={.15,.25,1},radar_color={.1,.15,.25},light_color={1,1,1}}
local limits={opacity={0,.8,.05},range={24,192,12},brightness={0,1,.05}}
local function clamp(v,lo,hi) return math.max(lo,math.min(hi,v)) end
return function(s)
    if not config then
        config={}
        local saved=s.state or {}
        for key,value in pairs(defaults) do
            if type(value)=="table" then
                config[key]={}
                for i=1,3 do config[key][i]=clamp(tonumber(type(saved[key])=="table" and saved[key][i]) or value[i],0,1) end
            elseif type(value)=="boolean" then
                if type(saved[key])=="boolean" then config[key]=saved[key] else config[key]=value end
            else config[key]=clamp(tonumber(saved[key]) or value,limits[key][1],limits[key][2]) end
        end
    end
    local changed=false
    for _,e in ipairs(s.events or {}) do
        if e.type=="change" then
            if type(defaults[e.id])=="boolean" then config[e.id]=e.value==true;changed=true
            elseif limits[e.id] then config[e.id]=clamp(tonumber(e.value) or defaults[e.id],limits[e.id][1],limits[e.id][2]);changed=true
            else
                for _,key in ipairs({"slope_color","water_color","radar_color","light_color"}) do
                    for i=1,3 do if e.id==key..i then config[key][i]=clamp(tonumber(e.value) or defaults[key][i],0,1);changed=true end end
                end
            end
        end
    end
    local function check(key,label) return {id=key,type="checkbox",text=label,value=config[key]} end
    local function slider(key,label) local l=limits[key];return {id=key,type="slider",text=label,value=config[key],min=l[1],max=l[2],step=l[3]} end
    local function colors(key,label)
        local rows={}
        for i,name in ipairs({"Red","Green","Blue"}) do rows[i]={id=key..i,type="slider",text=name,min=0,max=1,step=.05,value=config[key][i]} end
        return {id=key,type="column",text=label,children=rows}
    end
    local tabs={
        {id="terrain",type="column",text="Terrain",children={
            check("slopes","Highlight unwalkable slopes"),check("water","Highlight impassable water"),
            slider("opacity","Overlay opacity"),slider("range","Terrain range (m)"),
            {id="legend",text="Highlights outdoor terrain. Wadeable water stays clear."}}},
        {id="extras",type="column",text="Extras",children={
            check("radar","Show radar boundary"),{id="radar_help",text="Radar radius is 75 m outdoors and 25 m indoors."},
            check("dungeon","Dungeon light"),slider("brightness","Dungeon brightness"),
            {id="light_help",text="Your usual lighting returns outdoors."}}},
        colors("slope_color","Slopes"),colors("water_color","Water"),colors("radar_color","Radar"),colors("light_color","Light")}
    return {state=changed and config or nil,status=config.enabled and "Lens active" or "Lens paused",
        ui={check("enabled","Enable Dereth Lens"),{id="tabs",type="tabs",children=tabs}},
        terrain_overlay={slopes=config.enabled and config.slopes,water=config.enabled and config.water,
            radar=config.enabled and config.radar,dungeon=config.enabled and config.dungeon,
            opacity=config.opacity,range=config.range,brightness=config.brightness,
            slope_color=config.slope_color,water_color=config.water_color,radar_color=config.radar_color,light_color=config.light_color}}
end
