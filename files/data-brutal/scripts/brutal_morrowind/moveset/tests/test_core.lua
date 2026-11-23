-- Unit tests for moveset/core.lua. Pure Lua; run outside the engine:
--   luajit files/data-brutal/scripts/brutal_morrowind/moveset/tests/test_core.lua
-- Exits non-zero on failure.

local here = debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./"
package.path = here .. "../?.lua;" .. package.path
local core = dofile(here .. "../core.lua")

local passed, failed = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        passed = passed + 1
    else
        failed = failed + 1
        io.stderr:write("FAIL ", name, ": ", tostring(err), "\n")
    end
end
local function eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. " got " .. tostring(a), 2)
    end
end
local function near(a, b, msg)
    if math.abs(a - b) > 1e-4 then
        error((msg or "") .. " expected " .. tostring(b) .. " got " .. tostring(a), 2)
    end
end
local function types(events)
    local t = {}
    for i, e in ipairs(events) do
        t[i] = e.type .. (e.move and (":" .. e.move) or "")
    end
    return table.concat(t, ",")
end

-- A small generic test set: two light attacks chaining, a heavy finisher,
-- a dash with movement and a cooldown, and a move that needs a condition.
local function testSet()
    return {
        id = "test",
        bufferTime = 0.2,
        start = { light = "l1", heavy = "h1", dash = "dash", air = "air" },
        moves = {
            l1 = {
                anim = "a1",
                duration = 0.6,
                cancel = { 0.3, 0.5 },
                next = { light = "l2", heavy = "h1" },
                cost = { stamina = 1 },
                events = {
                    { t = 0.1, type = "sound", id = "swing" },
                    { t = 0.2, type = "hit", piece = "right", from = 0.2, to = 0.3, damage = 5 },
                },
            },
            l2 = {
                anim = "a2",
                duration = 0.5,
                speed = 2,
                cancel = { 0.2, 0.4 },
                next = { heavy = "h1" },
                events = { { t = 0.1, type = "hit", piece = "left", from = 0.1, to = 0.2 } },
            },
            h1 = { anim = "a3", duration = 1.0, cost = { stamina = 3 } },
            dash = {
                anim = "a4",
                duration = 0.3,
                cooldown = 1.0,
                events = { { t = 0, type = "move", forward = 400, time = 0.2 } },
            },
            air = { anim = "a5", duration = 0.4, requires = { airborne = true } },
        },
    }
end

test("valid set has no errors", function()
    eq(#core.validate(testSet()), 0)
end)

test("validation reports broken references and windows", function()
    local set = testSet()
    set.moves.l1.next.light = "missing"
    set.moves.l2.cancel = { 0.4, 0.2 }
    set.moves.h1.events = { { t = 2, type = "sound" } }
    set.moves.dash.next = { light = "l1" } -- follow-ups without a cancel window
    local errors = core.validate(set)
    eq(#errors, 4, table.concat(errors, " | "))
end)

test("new() refuses an invalid set", function()
    local set = testSet()
    set.start.light = "nope"
    local ok = pcall(core.new, set)
    eq(ok, false)
end)

test("press from neutral starts the start move and fires t=0 events", function()
    local r = core.new(testSet())
    local ev = r.press("dash")
    eq(types(ev), "start:dash,move:dash")
    eq(ev[2].forward, 400)
end)

test("events fire once each, in time order", function()
    local r = core.new(testSet())
    r.press("light")
    eq(types(r.update(0.05)), "")
    eq(types(r.update(0.1)), "sound:l1")
    eq(types(r.update(0.1)), "hit:l1")
    eq(types(r.update(0.1)), "")
end)

test("move ends when its duration elapses", function()
    local r = core.new(testSet())
    r.press("light")
    local ev = r.update(1.0)
    eq(types(ev), "sound:l1,hit:l1,end:l1")
    eq(ev[3].reason, "finished")
    eq(r.state().move, nil)
end)

test("a press inside the cancel window chains immediately", function()
    local r = core.new(testSet())
    r.press("light")
    r.update(0.35)
    local ev = r.press("light")
    eq(types(ev), "end:l1,start:l2")
    eq(ev[2].chain, 2)
    eq(r.stats.chained, 1)
end)

test("a buffered press chains exactly when the window opens", function()
    local r = core.new(testSet())
    r.press("light")
    r.update(0.15) -- t = 0.15, window opens at 0.3
    eq(types(r.press("light")), "") -- buffered
    eq(r.state().buffered, "light")
    -- One big step that jumps past the window opening still chains at 0.3,
    -- and the rest of the frame (0.35 s) plays l2 (0.25 s at speed 2) through.
    local ev = r.update(0.5)
    eq(types(ev), "hit:l1,end:l1,start:l2,hit:l2,end:l2")
end)

test("a buffered press expires after bufferTime", function()
    local r = core.new(testSet())
    r.press("light")
    r.update(0.01)
    r.press("light") -- too early: window opens at 0.3, buffer lasts 0.2
    local ev = r.update(0.25)
    eq(types(ev), "sound:l1,hit:l1,expired")
    eq(r.state().buffered, nil)
end)

test("speed scales move time", function()
    local r = core.new(testSet())
    r.press("light")
    r.update(0.35)
    r.press("light") -- l2, speed 2, duration 0.5 -> 0.25 s real
    local ev = r.update(0.26)
    eq(types(ev), "hit:l2,end:l2")
end)

test("a follow-up not listed in next is rejected, not queued", function()
    local r = core.new(testSet())
    r.press("light")
    r.update(0.35)
    local ev = r.press("dash")
    eq(types(ev), "rejected")
    eq(r.state().move, "l1")
    eq(r.state().buffered, nil)
end)

test("cooldown blocks a restart until it runs out", function()
    local r = core.new(testSet())
    r.press("dash")
    r.update(0.5) -- dash finished, cooldown 0.5 left
    local ev = r.press("dash")
    eq(ev[1].type, "rejected")
    eq(ev[1].reason, "cooldown")
    r.update(0.6)
    eq(types(r.press("dash")), "start:dash,move:dash")
end)

test("cost hooks are checked and spent", function()
    local stamina = 3
    local r = core.new(testSet(), {
        canAfford = function(c) return (c.stamina or 0) <= stamina end,
        spend = function(c) stamina = stamina - (c.stamina or 0) end,
    })
    r.press("light")
    eq(stamina, 2)
    r.update(0.35)
    local ev = r.press("heavy") -- needs 3
    eq(ev[1].type, "rejected")
    eq(ev[1].reason, "cost")
    eq(stamina, 2)
end)

test("requires hook gates moves", function()
    local airborne = false
    local r = core.new(testSet(), {
        check = function(req) return not req.airborne or airborne end,
    })
    eq(r.press("air")[1].reason, "requires")
    airborne = true
    eq(types(r.press("air")), "start:air")
end)

test("activeHits reports open hit windows per piece", function()
    local r = core.new(testSet())
    r.press("light")
    r.update(0.25)
    local hits = r.activeHits()
    eq(#hits, 1)
    eq(hits[1].piece, "right")
    eq(hits[1].damage, 5)
    r.update(0.1)
    eq(#r.activeHits(), 0)
end)

test("interrupt ends the move and clears the buffer", function()
    local r = core.new(testSet())
    r.press("light")
    r.press("light") -- buffered
    local ev = r.interrupt("stagger")
    eq(types(ev), "end:l1")
    eq(ev[1].reason, "stagger")
    eq(r.state().move, nil)
    eq(r.state().buffered, nil)
end)

test("save and load keep cooldowns and stats, not the move in progress", function()
    local r = core.new(testSet())
    r.press("dash")
    r.update(0.4)
    r.press("light")
    local data = r.save()
    near(data.cooldowns.dash, 0.6)
    local r2 = core.new(testSet())
    eq(r2.load(data), true)
    near(r2.cooldowns.dash, 0.6)
    eq(r2.stats.started, 2)
    eq(r2.state().move, nil)
    eq(r2.press("dash")[1].reason, "cooldown")
end)

test("load rejects data for another set or version", function()
    local r = core.new(testSet())
    eq(r.load({ version = 1, set = "other" }), false)
    eq(r.load({ version = 99, set = "test" }), false)
    eq(r.load(nil), false)
end)

-- Regressions from code review (partition invariance, cooldown timing,
-- authored order of same-time events).

local function chainSet()
    return {
        id = "chain",
        bufferTime = 1,
        start = { light = "a" },
        moves = {
            a = { duration = 1, cancel = { 0.1, 0.2 }, next = { light = "b", skill = "s" } },
            b = { duration = 1, speed = 2, events = { { t = 0.3, type = "sound", id = "b03" } } },
            s = { duration = 0.5, cooldown = 1 },
        },
    }
end

local function runSplit(steps)
    local r = core.new(chainSet())
    r.press("light")
    r.press("light") -- buffered; window opens at 0.1
    local all = {}
    for _, dt in ipairs(steps) do
        for _, e in ipairs(r.update(dt)) do all[#all + 1] = e end
    end
    return r, types(all)
end

test("remaining frame time carries into the chained move", function()
    local r = runSplit({ 0.3 })
    eq(r.state().move, "b")
    -- 0.1 s in a, then 0.2 s real in b at speed 2 = 0.4 move time.
    near(r.state().time, 0.4)
end)

test("result does not depend on how the frame is split", function()
    local r1, ev1 = runSplit({ 0.3 })
    local r2, ev2 = runSplit({ 0.1, 0.1, 0.1 })
    local r3, ev3 = runSplit({ 0.05, 0.2, 0.05 })
    near(r1.state().time, r2.state().time)
    near(r1.state().time, r3.state().time)
    eq(ev1, ev2)
    eq(ev1, ev3)
    eq(ev1, "end:a,start:b,sound:b")
end)

test("a cooldown expiring after the cancel window does not allow the chain", function()
    local r = core.new(chainSet())
    r.press("light") -- a
    r.update(0.05)
    r.cooldowns.s = 0.2 -- expires at 0.25, after the window (0.1 .. 0.2)
    r.press("skill") -- buffered until the window opens at 0.1
    local ev = r.update(0.3)
    eq(r.state().move, "a", "a should keep running, not chain into s")
    local found = false
    for _, e in ipairs(ev) do
        if e.type == "rejected" and e.reason == "cooldown" then found = true end
    end
    eq(found, true, "the chain should be rejected for cooldown: " .. types(ev))
end)

test("a cooldown expiring before the window opens allows the chain", function()
    local r = core.new(chainSet())
    r.press("light")
    r.update(0.02)
    r.cooldowns.s = 0.05 -- expires at 0.07, before the window opens at 0.1
    r.press("skill")
    r.update(0.2)
    eq(r.state().move, "s")
end)

test("same-time events keep their authored order", function()
    local set = {
        id = "order",
        start = { light = "m" },
        moves = {
            m = {
                duration = 1,
                events = {
                    { t = 0.2, type = "sound", id = "first" },
                    { t = 0.2, type = "effect", model = "second" },
                    { t = 0.2, type = "hit", piece = "third", from = 0.2, to = 0.3 },
                    { t = 0.1, type = "sound", id = "earliest" },
                },
            },
        },
    }
    local r = core.new(set)
    r.press("light")
    local ev = r.update(0.5)
    eq(types(ev), "sound:m,sound:m,effect:m,hit:m")
    eq(ev[1].id, "earliest")
    eq(ev[2].id, "first")
    eq(ev[3].model, "second")
end)

test("random frame splits give identical results (200 trials)", function()
    math.randomseed(12345)
    local function run(steps)
        local r = core.new(testSet())
        local all = {}
        local function add(ev) for _, e in ipairs(ev) do all[#all + 1] = e end end
        add(r.press("light"))
        add(r.press("light")) -- buffered, chains at 0.3
        for _, dt in ipairs(steps) do add(r.update(dt)) end
        local st = r.state()
        return types(all) .. "|" .. tostring(st.move) .. "|" .. string.format("%.4f", st.time)
    end
    local reference = run({ 0.7 })
    for _ = 1, 200 do
        local steps, left = {}, 0.7
        while left > 1e-9 do
            local dt = math.min(left, math.random() * 0.2)
            steps[#steps + 1] = dt
            left = left - dt
        end
        eq(run(steps), reference)
    end
end)

io.write(string.format("moveset core: %d passed, %d failed\n", passed, failed))
os.exit(failed == 0 and 0 or 1)
