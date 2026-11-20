-- Install/enable this folder in each client. No received text is executed.
local recent = {}
return function(s, profile)
    local channel = profile.channel or "team"
    local bus = s.client_bus or {}
    local out = {client_channels={channel}, ui={}, status=bus.connected and "Connected" or "Connecting"}
    for _, e in ipairs(s.events or {}) do
        if e.type == "client_message" and e.channel == channel then
            recent[#recent+1] = (e.character or "Client") .. ": " .. tostring((e.data or {}).text or "")
            if #recent > 8 then table.remove(recent, 1) end
        elseif e.type == "command" or (e.type == "click" and e.id == "ping") then
            if bus.connected and (bus.channels or {})[1] == channel then
                out.client_messages = out.client_messages or {}
                if #out.client_messages < 8 then
                    out.client_messages[#out.client_messages+1] = {channel=channel, data={text=e.text or "Hello from another client"}}
                end
            else
                out.status = "Connecting; try sending again shortly"
            end
        end
    end
    local function row(id, text, action)
        out.ui[#out.ui+1] = {id=id, text=tostring(text or ""):sub(1, 512), action=action}
    end
    row("channel", "Local channel: " .. channel)
    row("status", bus.error and bus.error ~= "" and bus.error or out.status)
    row("help", "Send: /acueplugin client_channel your message")
    row("ping", "Send greeting to other clients", "event")
    for i, peer in ipairs(bus.peers or {}) do
        row("peer"..i, (peer.character or "Client") .. " — " .. table.concat(peer.channels or {}, ", "))
    end
    for i, text in ipairs(recent) do row("message"..i, text) end
    return out
end
