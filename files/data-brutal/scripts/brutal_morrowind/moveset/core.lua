-- Data-driven move sets: combos, timing windows, input buffering, cancels,
-- movement, cooldowns, resource costs and timed events.
--
-- This module is pure Lua with no OpenMW API, so it can be unit-tested outside
-- the engine (see moveset/tests). The engine layer (moveset/player.lua) feeds it
-- input and time, and performs the events it returns.
--
-- A move set is a table:
--   {
--     id = "example",
--     bufferTime = 0.25,             -- how long a press waits for a cancel window
--     start = { light = "l1", heavy = "h1" },   -- input -> first move, from neutral
--     moves = {
--       l1 = {
--         anim = "attack_l1",        -- animation group (engine layer plays it)
--         duration = 0.6,            -- total length in seconds (at speed 1)
--         speed = 1,                 -- optional playback multiplier
--         cancel = { 0.25, 0.5 },    -- window in which a buffered input chains
--         next = { light = "l2", heavy = "h1" },   -- input -> follow-up move
--         cost = { stamina = 1 },    -- spent when the move starts
--         cooldown = 0,              -- seconds before this move may start again
--         requires = { grounded = true },          -- conditions the caller provides
--         events = {                 -- fired once each when time passes `t`
--           { t = 0.10, type = "sound", id = "swing" },
--           { t = 0.20, type = "hit", piece = "right", from = 0.20, to = 0.32 },
--           { t = 0.0,  type = "move", forward = 300, up = 0, time = 0.15 },
--         },
--       },
--     },
--   }
--
-- `t`, `cancel` and `duration` are in move time (seconds at speed 1).
-- Real elapsed time is scaled by `speed`.

local M = {}

local EPS = 1e-6

local function sortedEvents(move)
    -- Sort by time; events at the same time keep their authored order.
    local list = {}
    for i, e in ipairs(move.events or {}) do
        list[i] = { e = e, i = i }
    end
    table.sort(list, function(a, b)
        if a.e.t ~= b.e.t then
            return a.e.t < b.e.t
        end
        return a.i < b.i
    end)
    for i, wrapped in ipairs(list) do
        -- A copy, so content tables are never modified (and may be shared).
        local copy = { __ordinal = wrapped.i }
        for k, v in pairs(wrapped.e) do
            copy[k] = v
        end
        list[i] = copy
    end
    return list
end

--- Checks a move set and returns a list of error strings (empty when valid).
function M.validate(set)
    local errors = {}
    local function err(fmt, ...)
        errors[#errors + 1] = string.format(fmt, ...)
    end
    if type(set) ~= "table" then
        return { "move set must be a table" }
    end
    if type(set.id) ~= "string" or set.id == "" then
        err("move set needs a string id")
    end
    if type(set.moves) ~= "table" then
        err("move set %s has no moves table", tostring(set.id))
        return errors
    end
    if set.bufferTime ~= nil and (type(set.bufferTime) ~= "number" or set.bufferTime < 0) then
        err("bufferTime must be a non-negative number")
    end
    for input, target in pairs(set.start or {}) do
        if not set.moves[target] then
            err("start input %s points to unknown move %s", tostring(input), tostring(target))
        end
    end
    for id, move in pairs(set.moves) do
        if type(move.duration) ~= "number" or move.duration <= 0 then
            err("move %s needs a positive duration", id)
        else
            if move.speed ~= nil and (type(move.speed) ~= "number" or move.speed <= 0) then
                err("move %s speed must be positive", id)
            end
            if move.cancel ~= nil then
                local a, b = move.cancel[1], move.cancel[2]
                if type(a) ~= "number" or type(b) ~= "number" or a < 0 or b < a or b > move.duration + EPS then
                    err("move %s cancel window must be {from, to} within 0..duration", id)
                end
            end
            for _, e in ipairs(move.events or {}) do
                if type(e.t) ~= "number" or e.t < 0 or e.t > move.duration + EPS then
                    err("move %s has an event outside 0..duration", id)
                end
                if type(e.type) ~= "string" then
                    err("move %s has an event without a type", id)
                end
                if e.type == "hit" and (type(e.to) ~= "number" or type(e.from) ~= "number" or e.to < e.from) then
                    err("move %s hit event needs from <= to", id)
                end
            end
        end
        for input, target in pairs(move.next or {}) do
            if not set.moves[target] then
                err("move %s input %s points to unknown move %s", id, tostring(input), tostring(target))
            end
            if not move.cancel then
                err("move %s has follow-ups but no cancel window", id)
            end
        end
        if move.cooldown ~= nil and (type(move.cooldown) ~= "number" or move.cooldown < 0) then
            err("move %s cooldown must be a non-negative number", id)
        end
    end
    table.sort(errors)
    return errors
end

--- Creates a runner for a validated move set.
-- `hooks` (all optional):
--   canAfford(cost) -> bool      resource check
--   spend(cost)                  resource spending
--   check(requires) -> bool      condition check (grounded, weapon drawn, ...)
function M.new(set, hooks)
    local errors = M.validate(set)
    if #errors > 0 then
        error("invalid move set: " .. table.concat(errors, "; "))
    end
    hooks = hooks or {}
    local self = {
        set = set,
        hooks = hooks,
        current = nil, -- { id, move, time, events, nextEvent, chain }
        buffer = nil, -- { input, age }
        cooldowns = {}, -- move id -> seconds remaining
        stats = { started = 0, chained = 0, rejected = 0 },
    }
    local sorted = {}
    for id, move in pairs(set.moves) do
        sorted[id] = sortedEvents(move)
    end

    local function allowed(id)
        local move = set.moves[id]
        if (self.cooldowns[id] or 0) > EPS then
            return false, "cooldown"
        end
        if move.requires and hooks.check and not hooks.check(move.requires) then
            return false, "requires"
        end
        if move.cost and hooks.canAfford and not hooks.canAfford(move.cost) then
            return false, "cost"
        end
        return true
    end

    local function start(id, out, chained)
        local move = set.moves[id]
        if move.cost and hooks.spend then
            hooks.spend(move.cost)
        end
        if move.cooldown and move.cooldown > 0 then
            self.cooldowns[id] = move.cooldown
        end
        local chain = chained and (self.current.chain + 1) or 1
        self.current = { id = id, move = move, time = 0, events = sorted[id], nextEvent = 1, chain = chain }
        self.stats.started = self.stats.started + 1
        if chained then
            self.stats.chained = self.stats.chained + 1
        end
        out[#out + 1] = { type = "start", move = id, anim = move.anim, speed = move.speed or 1, chain = chain }
    end

    -- Fires events up to the current move time.
    local function fireEvents(out)
        local cur = self.current
        while cur.nextEvent <= #cur.events and cur.events[cur.nextEvent].t <= cur.time + EPS do
            local e = cur.events[cur.nextEvent]
            local copy = { move = cur.id, ordinal = e.__ordinal }
            for k, v in pairs(e) do
                if k ~= "__ordinal" then
                    copy[k] = v
                end
            end
            out[#out + 1] = copy
            cur.nextEvent = cur.nextEvent + 1
        end
    end

    local function inCancelWindow()
        local cur = self.current
        local w = cur.move.cancel
        return w ~= nil and cur.time + EPS >= w[1] and cur.time <= w[2] + EPS
    end

    -- Tries to consume the buffered input. Returns true when a move started.
    local function consumeBuffer(out)
        local b = self.buffer
        if not b then
            return false
        end
        local target, chained
        if self.current then
            if not inCancelWindow() then
                return false
            end
            target = self.current.move.next and self.current.move.next[b.input]
            chained = true
        else
            target = set.start and set.start[b.input]
        end
        if not target then
            -- Not a valid follow-up: drop it now rather than let it fire later.
            self.buffer = nil
            self.stats.rejected = self.stats.rejected + 1
            out[#out + 1] = { type = "rejected", input = b.input, reason = "no move" }
            return false
        end
        local ok, reason = allowed(target)
        if not ok then
            self.buffer = nil
            self.stats.rejected = self.stats.rejected + 1
            out[#out + 1] = { type = "rejected", input = b.input, move = target, reason = reason }
            return false
        end
        self.buffer = nil
        if self.current then
            out[#out + 1] = { type = "end", move = self.current.id, reason = "cancel" }
        end
        start(target, out, chained)
        fireEvents(out)
        return true
    end

    --- Registers an input press. Returns the events produced immediately.
    function self.press(input)
        local out = {}
        self.buffer = { input = input, age = 0 }
        consumeBuffer(out)
        return out
    end

    --- Advances time by `dt` real seconds. Returns events produced, in order.
    -- Cooldowns, the input buffer and the current move all advance on one
    -- timeline, stopping at every boundary (event, cancel window opening, move
    -- end, buffer expiry, cooldown expiry). A move started mid-frame consumes
    -- the rest of the frame. The result therefore does not depend on how the
    -- elapsed time is split into frames.
    function self.update(dt)
        local out = {}
        local bufferTime = set.bufferTime or 0.25
        local remaining = dt
        local guard = 0
        while remaining > EPS do
            guard = guard + 1
            if guard > 10000 then
                error("moveset update did not converge")
            end
            local cur = self.current
            local step = remaining
            for _, left in pairs(self.cooldowns) do
                step = math.min(step, left)
            end
            if self.buffer then
                -- Stop just past the expiry so it is reported in order.
                step = math.min(step, math.max(0, bufferTime - self.buffer.age) + 2 * EPS)
            end
            if cur then
                local speed = cur.move.speed or 1
                local boundary = cur.move.duration
                local w = cur.move.cancel
                if w and cur.time + EPS < w[1] then
                    boundary = math.min(boundary, w[1])
                end
                local nextEvent = cur.events[cur.nextEvent]
                if nextEvent and nextEvent.t > cur.time + EPS then
                    boundary = math.min(boundary, nextEvent.t)
                end
                step = math.min(step, math.max(0, boundary - cur.time) / speed)
            end
            step = math.max(step, EPS)

            for id, left in pairs(self.cooldowns) do
                left = left - step
                self.cooldowns[id] = left > EPS and left or nil
            end
            if cur then
                cur.time = math.min(cur.move.duration, cur.time + step * (cur.move.speed or 1))
                fireEvents(out)
            end
            remaining = remaining - step

            local chained = self.buffer ~= nil and consumeBuffer(out)
            if not chained and self.buffer then
                self.buffer.age = self.buffer.age + step
                if self.buffer.age > bufferTime + EPS then
                    out[#out + 1] = { type = "expired", input = self.buffer.input }
                    self.buffer = nil
                end
            end
            if not chained and cur and self.current == cur and cur.time + EPS >= cur.move.duration then
                out[#out + 1] = { type = "end", move = cur.id, reason = "finished" }
                self.current = nil
            end
            if not self.current and not self.buffer and next(self.cooldowns) == nil then
                break
            end
        end
        return out
    end

    --- Interrupts the current move (stagger, death, knockdown).
    function self.interrupt(reason)
        local out = {}
        if self.current then
            out[#out + 1] = { type = "end", move = self.current.id, reason = reason or "interrupted" }
        end
        self.current = nil
        self.buffer = nil
        return out
    end

    --- Hit windows active right now: list of { move, piece, from, to }.
    function self.activeHits()
        local list = {}
        local cur = self.current
        if not cur then
            return list
        end
        for _, e in ipairs(cur.events) do
            if e.type == "hit" and cur.time + EPS >= e.from and cur.time <= e.to + EPS then
                list[#list + 1] = { move = cur.id, ordinal = e.__ordinal, piece = e.piece, from = e.from, to = e.to,
                    damage = e.damage, event = e }
            end
        end
        return list
    end

    function self.state()
        local cur = self.current
        return {
            move = cur and cur.id or nil,
            time = cur and cur.time or 0,
            chain = cur and cur.chain or 0,
            buffered = self.buffer and self.buffer.input or nil,
            canCancel = cur ~= nil and inCancelWindow(),
        }
    end

    --- Save data: only what must survive a save/load (cooldowns, stats).
    -- A move in progress is not saved; loading always resumes in neutral.
    function self.save()
        local cooldowns = {}
        for id, left in pairs(self.cooldowns) do
            cooldowns[id] = left
        end
        return {
            version = 1,
            set = set.id,
            cooldowns = cooldowns,
            stats = { started = self.stats.started, chained = self.stats.chained, rejected = self.stats.rejected },
        }
    end

    function self.load(data)
        self.current = nil
        self.buffer = nil
        self.cooldowns = {}
        if type(data) ~= "table" or data.version ~= 1 or data.set ~= set.id then
            return false
        end
        for id, left in pairs(data.cooldowns or {}) do
            if set.moves[id] and type(left) == "number" and left > 0 then
                self.cooldowns[id] = left
            end
        end
        if type(data.stats) == "table" then
            for k in pairs(self.stats) do
                if type(data.stats[k]) == "number" then
                    self.stats[k] = data.stats[k]
                end
            end
        end
        return true
    end

    return self
end

return M
