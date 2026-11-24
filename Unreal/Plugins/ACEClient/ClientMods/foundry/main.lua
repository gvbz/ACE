-- Dereth Foundry: Lua owns the workshop; the host exposes bounded content
-- file browsing and ACE's existing administrator commands, never a database.
local c,root="", ""
local files,total,page={},0,0
local queue,serial={},0
local status="Choose content or select a world object to begin."
local history,pending={},nil
local defaults={wcid="",landblock="",quest="",recipe="",generator="",child="",parent="",
    kind="weenies",format="json",filter="",file="",distance=.1,heading=0}
local types={weenies="weenie",landblocks="landblock",quests="quest",recipes="recipe"}
local function trim(v)
    local t=tostring(v or "");local a,b=1,#t
    while a<=b and t:byte(a)<=32 do a=a+1 end
    while b>=a and t:byte(b)<=32 do b=b-1 end
    return t:sub(a,b)
end
local function token(v)
    local t=trim(v);if t=="" or #t>128 then return nil end
    for i=1,#t do local ch=t:byte(i);if ch<=32 or ch==127 then return nil end end
    return t
end
local transfers_by_id={}
for kind in pairs(types) do for _,verb in ipairs({"import","export"}) do for _,format in ipairs({"json","sql"}) do
    transfers_by_id[verb.."_"..format.."_"..kind]={verb,format,kind}
end end end
local function file_id(path)
    local start=1;for i=1,#path do if path:sub(i,i)=="/" then start=i+1 end end
    local name=path:sub(start);local stop=#name
    for i=1,#name-2 do if name:sub(i,i+2)==" - " then return name:sub(1,i-1) end end
    for i=#name,1,-1 do if name:sub(i,i)=="." then stop=i-1;break end end
    return name:sub(1,stop)
end
local function hex(v) return string.format("%08X",tonumber(v) or 0) end
local function note(t) status=t;table.insert(history,1,t);while #history>8 do table.remove(history) end end
local function req(op,data)
    serial=serial+1;data=data or {};data.service="content";data.operation=op;data.id="content_"..serial
    if op=="list" then for i=#queue,1,-1 do if queue[i].operation=="list" then table.remove(queue,i) end end end
    if #queue<16 then queue[#queue+1]=data else note("Finish the pending requests first.") end
end
local function scan() req("list",{kind=c.kind,format=c.format,filter=c.filter,offset=page*128}) end
local function command(text,target,danger)
    if danger then pending={text=text,target=target};note("Review the command below, then confirm.");return end
    req("command",{text=text,target=target})
end
local function required(value,label)
    local v=token(value);if not v then note("Enter "..label.." (one ID or classname).");return nil end;return v
end
return function(s)
    if type(c)~="table" then
        c={};for k,v in pairs(defaults) do c[k]=type((s.state or {})[k])==type(v) and s.state[k] or v end
        if not types[c.kind] then c.kind="weenies" end
        if c.format~="json" and c.format~="sql" then c.format="json" end
        c.distance=math.max(.001,math.min(100,tonumber(c.distance) or .1));c.heading=math.max(0,math.min(360,tonumber(c.heading) or 0))
    end
    local selection=s.selection or {};local selected=tonumber(selection.id) or 0
    local function target() if selected==0 or selection.object_type=="player" or selected==s.player then note("Select a non-player object first.");return nil end;return selected end
    local dirty=false
    if pending and pending.target and pending.target~=selected then pending=nil;note("Selection changed; command cancelled.") end
    for _,e in ipairs(s.events or {}) do
        if e.type=="content_root" then root=e.root or "";page=0;c.file="";files={};scan()
        elseif e.type=="result" and tostring(e.id):sub(1,8)=="content_" then
            if not e.ok then note(e.error or "Request failed.")
            elseif e.files then files=e.files;total=e.total or #files;root=e.root or root;note(total.." matching files.")
            elseif e.command then note("Sent "..e.command.." — see chat for the server result.")
            else note("Opened in your text editor.") end
        elseif e.type=="change" and defaults[e.id]~=nil then
            c[e.id]=e.value;dirty=true
            if e.id=="kind" or e.id=="format" or e.id=="filter" then page=0;c.file="";files={};scan() end
            if e.id=="file" then
                local prefix=file_id(c.file)
                local field=({weenies="wcid",landblocks="landblock",quests="quest",recipes="recipe"})[c.kind]
                c[field]=prefix
            end
        elseif e.type=="click" then
            local id=e.id
            if id=="confirm" and pending then command(pending.text,pending.target);pending=nil
            elseif id=="cancel" then pending=nil;note("Command cancelled.")
            elseif id=="scan" then scan()
            elseif id=="prev" then page=math.max(0,page-1);scan()
            elseif id=="next" then page=page+1;scan()
            elseif id=="edit" then if c.file~="" then req("edit",{name=c.file}) else note("Choose a file first.") end
            elseif id=="use_selected" then
                if selected~=0 then c.wcid=selection.wcid and string.format("%.0f",selection.wcid) or "";dirty=true else note("Select an object first.") end
            elseif id=="get_parent" then if target() then c.parent="0x"..hex(selected);dirty=true end
            elseif id=="current_block" then c.landblock=string.format("%04X",math.floor(((s.position or {}).cell or 0)/65536));dirty=true
            elseif id=="create" or id=="ci" or id=="createinst" then
                local v=required(c.wcid,"a WCID");if v then command("/"..id.." "..v,nil,id=="createinst") end
            elseif id=="generator_create" then local v=required(c.generator,"a generator WCID");if v then command("/createinst "..v,nil,true) end
            elseif id=="link" then
                local parent=required(c.parent,"a parent GUID");local child=required(c.child,"a child WCID")
                if parent and child and parent:sub(1,2):lower()=="0x" and #parent<=10 and tonumber(parent:sub(3),16) then command("/createinst -p "..parent.." -c "..child,nil,true)
                elseif parent then note("Parent GUID must use 0x followed by hexadecimal digits.") end
            elseif id=="addenc" then local v=required(c.child,"an encounter WCID");if v then command("/addenc "..v,nil,true) end
            elseif id=="removeinst" or id=="removeenc" or id=="delete" or id=="getinfo" then
                local t=target();if t then command("/"..id,t,id~="getinfo") end
            elseif id:sub(1,6)=="nudge_" then local t=target();if t then command("/nudge "..id:sub(7).." "..tostring(c.distance),t) end
            elseif id=="rotate" then local t=target();if t then command("/rotate "..tostring(c.heading),t) end
            elseif id=="rotate_here" then local t=target();if t then command("/rotate here",t) end
            elseif id=="myloc" or id=="clearcache" or id=="reload-landblock" then command("/"..id,nil,id~="myloc")
            elseif id=="server_folder" then command("/fetchstring content_folder")
            else
                local transfer=transfers_by_id[id]
                if transfer then
                    local verb,format,kind=table.unpack(transfer)
                    local field=({weenies="wcid",landblocks="landblock",quests="quest",recipes="recipe"})[kind]
                    local value=required(c[field],"the "..types[kind].." ID")
                    if value then command("/"..verb.."-"..format.." "..types[kind].." "..value,nil,verb=="import") end
                end
            end
        end
    end
    local function text(id,t) return {id=id,text=t} end
    local function button(id,t,enabled) return {id=id,type="button",text=t,enabled=enabled~=false} end
    local function input(id,t) return {id=id,type="input",text=t,value=c[id]} end
    local function row(id,children) return {id=id,type="row",children=children} end
    local function tab(id,t,children) return {id=id,type="column",text=t,children=children} end
    local function transfers(kind)
        return row(kind.."_transfers",{button("import_json_"..kind,"Import JSON"),button("import_sql_"..kind,"Import SQL"),button("export_json_"..kind,"Export JSON"),button("export_sql_"..kind,"Export SQL")})
    end
    local selected_text=selected~=0 and (selection.name or "Object").." · WCID "..(selection.wcid and string.format("%.0f",selection.wcid) or "?").." · 0x"..hex(selected) or "No object selected"
    local tabs={
        tab("content","Items",{input("wcid","WCID or classname"),button("use_selected","Use selected WCID"),
            row("spawn",{button("create","Create in world"),button("ci","Create in inventory"),button("createinst","Create instance")}),transfers("weenies"),
            row("object",{button("getinfo","Get info"),button("removeinst","Remove instance"),button("delete","Delete selected")}),
            text("content_help","Create instance saves the placement in the world database. Server replies appear in chat.")}),
        tab("placement","Move",{{id="distance",type="number",text="Nudge distance (m)",value=c.distance,min=.001,max=100,step=.001},
            row("north",{button("nudge_nw","Northwest"),button("nudge_n","North"),button("nudge_ne","Northeast")}),
            row("middle",{button("nudge_w","West"),button("nudge_here","Move here"),button("nudge_e","East")}),
            row("south",{button("nudge_sw","Southwest"),button("nudge_s","South"),button("nudge_se","Southeast")}),
            row("height",{button("nudge_up","Up"),button("nudge_down","Down")}),
            {id="heading",type="number",text="Heading (0 = north, 90 = west)",value=c.heading,min=0,max=360,step=1},
            row("rotation",{button("rotate","Rotate"),button("rotate_here","Face my direction")}),
            text("placement_help","Select a persistent instance to move or rotate it.")}),
        tab("landblocks","Land",{input("landblock","Landblock ID (hex)"),button("current_block","Use current landblock"),transfers("landblocks"),
            row("land_tools",{button("reload-landblock","Reload landblock"),button("myloc","My location"),button("clearcache","Clear server cache")})}),
        tab("quests","Quests",{input("quest","Quest name"),transfers("quests"),input("recipe","Recipe ID"),transfers("recipes")}),
        tab("generators","Spawns",{input("generator","Generator WCID or classname"),button("generator_create","Create generator instance"),
            input("parent","Parent GUID (0x...)"),button("get_parent","Use selected parent"),input("child","Child / encounter WCID"),
            button("link","Link child instance"),row("encounters",{button("addenc","Add encounter"),button("removeenc","Remove encounter")}),
            text("generator_help","Create or select a generator, capture its GUID, then add a child. Encounters use your current outdoor cell.")}),
        tab("files","Files",{text("folder",root~="" and root or "Default folder: ClientMods/Content"),
            {id="choose_folder",type="button",text="Choose local folder",action="content_folder"},button("server_folder","Show server folder"),
            {id="kind",type="select",text="Content type",value=c.kind,options={"weenies","landblocks","quests","recipes"}},
            {id="format",type="select",text="Format",value=c.format,options={"json","sql"}},input("filter","Filter filenames"),button("scan","Refresh files"),
            {id="file",type="select",text="File",value=c.file,options=files},
            row("file_page",{button("prev","Previous",page>0),button("next","Next",(page+1)*128<total),button("edit","Edit file",c.file~="")}),
            text("file_help","Selecting a local file fills its ID on the matching tab. To import on a remote server, copy the edited file to its content folder first.")}),
        tab("help","Help",{text("guide","1. Choose an ACE content folder in Files.\n2. Export an existing object or choose a JSON/SQL file, then edit it.\n3. Import the matching ID.\n4. Create a live preview or a persistent instance.\n5. Position it and export the landblock."),
            text("access","Foundry commands require developer or admin access on the server."),
            text("results",table.concat(history,"\n"))})}
    local ui={text("selection",selected_text),{id="tabs",type="tabs",children=tabs},text("status",status)}
    if pending then
        table.insert(ui,{id="review",type="section",text="Review content change",children={text("review_command",pending.text..(pending.target and " on "..selected_text or "")),
            row("review_buttons",{button("confirm","Confirm"),button("cancel","Cancel")})}})
    end
    local requests
    if #queue>0 and (s.time or 0)>=(c.next_send or 0) then
        requests={table.remove(queue,1)};c.next_send=(s.time or 0)+1.05
    end
    -- Idle callbacks contain only the small selection and no inventories/spellbooks.
    return {state=dirty and c or nil,ui=ui,requests=requests,status=status}
end
