_G.test_mods = _G.test_mods or {}

if mcp == nil then
    return {
        name = "frame_heartbeat",
        skipped = true,
        reason = "mcp unavailable",
    }
end

local state = _G.test_mods.frame_heartbeat or {
    reloads = 0,
    frames = 0,
    logs = 0,
    callback_id = nil,
}

if state.callback_id then
    mcp.remove_callback(state.callback_id)
end

state.reloads = state.reloads + 1
state.frames = 0
state.logs = 0
state.last_dt = 0.0
state.last_frame_clock = nil

state.callback_id = mcp.on_frame(function(dt)
    state.frames = state.frames + 1
    state.last_dt = dt or 0.0
    state.last_frame_clock = os.clock()

    if state.frames % 240 == 0 then
        state.logs = state.logs + 1
        mcp.log(string.format(
            "[frame_heartbeat] frames=%d last_dt=%.4f reloads=%d",
            state.frames,
            state.last_dt,
            state.reloads
        ))
    end
end)

_G.test_mods.frame_heartbeat = state

return {
    name = "frame_heartbeat",
    callback_id = state.callback_id,
    reloads = state.reloads,
}
