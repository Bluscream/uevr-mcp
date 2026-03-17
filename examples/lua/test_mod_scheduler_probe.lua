_G.test_mods = _G.test_mods or {}

if mcp == nil then
    return {
        name = "scheduler_probe",
        skipped = true,
        reason = "mcp unavailable",
    }
end

local state = _G.test_mods.scheduler_probe or {
    reloads = 0,
    timer_id = nil,
    async_id = nil,
    timer_fires = 0,
    async_resumes = 0,
    phase = "cold",
}

if state.timer_id then
    mcp.clear_timer(state.timer_id)
end

if state.async_id then
    mcp.cancel_async(state.async_id)
end

state.reloads = state.reloads + 1
state.timer_fires = 0
state.async_resumes = 0
state.phase = "armed"
state.completed_at = nil
state.last_timer_clock = nil

state.timer_id = mcp.set_timer(0.25, function()
    state.timer_fires = state.timer_fires + 1
    state.last_timer_clock = os.clock()

    if state.timer_fires % 4 == 0 then
        mcp.log(string.format(
            "[scheduler_probe] timer_fires=%d phase=%s",
            state.timer_fires,
            state.phase
        ))
    end
end, true)

state.async_id = mcp.async(function()
    state.async_resumes = state.async_resumes + 1
    state.phase = "after_spawn"
    mcp.wait(0.20)

    state.async_resumes = state.async_resumes + 1
    state.phase = "after_wait"
    mcp.wait_until(function()
        return state.timer_fires >= 3
    end)

    state.async_resumes = state.async_resumes + 1
    state.phase = "complete"
    state.completed_at = os.clock()
    mcp.log(string.format(
        "[scheduler_probe] complete timer_fires=%d async_resumes=%d reloads=%d",
        state.timer_fires,
        state.async_resumes,
        state.reloads
    ))
end)

_G.test_mods.scheduler_probe = state

return {
    name = "scheduler_probe",
    timer_id = state.timer_id,
    async_id = state.async_id,
    reloads = state.reloads,
}
