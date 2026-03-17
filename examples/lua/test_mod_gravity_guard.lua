_G.test_mods = _G.test_mods or {}

if mcp == nil then
    return {
        name = "gravity_guard",
        skipped = true,
        reason = "mcp unavailable",
    }
end

local state = _G.test_mods.gravity_guard or {
    reloads = 0,
    callback_id = nil,
    target_gravity = 0.35,
}

if state.callback_id then
    mcp.remove_callback(state.callback_id)
end

state.reloads = state.reloads + 1
state.frames = 0
state.pawn_seen = 0
state.read_failures = 0
state.write_failures = 0
state.writes = 0
state.last_gravity = nil
state.original_gravity = nil
state.last_movement_mode = nil
state.movement_addr = 0
state.last_write_clock = nil

state.callback_id = mcp.on_frame(function(_dt)
    state.frames = state.frames + 1

    local pawn = uevr.api.get_local_pawn()
    if not pawn then
        return
    end

    state.pawn_seen = state.pawn_seen + 1

    local move = mcp.read_property(pawn:get_address(), "CharacterMovement")
    if not move or move == 0 then
        state.read_failures = state.read_failures + 1
        return
    end

    state.movement_addr = move

    local gravity = mcp.read_property(move, "GravityScale")
    if type(gravity) ~= "number" then
        state.read_failures = state.read_failures + 1
        return
    end

    state.last_gravity = gravity
    state.last_movement_mode = mcp.read_property(move, "MovementMode")

    if state.original_gravity == nil then
        state.original_gravity = gravity
    end

    if math.abs(gravity - state.target_gravity) > 0.001 then
        if mcp.write_property(move, "GravityScale", state.target_gravity) then
            state.writes = state.writes + 1
            state.last_write_clock = os.clock()
            state.last_gravity = state.target_gravity
        else
            state.write_failures = state.write_failures + 1
        end
    end

    if state.frames % 300 == 0 then
        mcp.log(string.format(
            "[gravity_guard] frames=%d gravity=%.3f writes=%d read_failures=%d write_failures=%d",
            state.frames,
            state.last_gravity or -1.0,
            state.writes,
            state.read_failures,
            state.write_failures
        ))
    end
end)

_G.test_mods.gravity_guard = state

return {
    name = "gravity_guard",
    callback_id = state.callback_id,
    reloads = state.reloads,
    target_gravity = state.target_gravity,
}
