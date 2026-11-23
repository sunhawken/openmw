-- Runs on an NPC using a generated rig. Checks exact bone names, clip groups
-- and that playing each clip moves its bone. Results go to the log.
local self = require('openmw.self')
local types = require('openmw.types')
local animation = require('openmw.animation')
local core = require('openmw.core')

local model = nil
local CUSTOM = {
    bones = { 'rig_root', 'pelvis', 'spine.upper', 'arm_l', 'arm l', 'hand_r' },
    clips = { { 'wave_hello', 'arm_l' }, { 'slash combo.2', 'hand_r' }, { 'sway', 'spine.upper' } },
}
local BIP01 = {
    bones = { 'Bip01', 'Bip01 Pelvis', 'Bip01 Spine1', 'Bip01 L Clavicle', 'Bip01 L UpperArm', 'Bip01 R Hand' },
    clips = { { 'wave_hello', 'Bip01 L Clavicle' }, { 'slash combo.2', 'Bip01 R Hand' } },
}
local spec, tag
local timer, phase, clipIndex = 0, 'wait', 1
local before, step, control0, control

local function check(ok, what)
    print('[rig-test] ' .. tag .. ' ' .. (ok and 'PASS ' or 'FAIL ') .. what)
end

local function rotationOf(bone)
    local t = animation.getBoneTransform(self, bone)
    if not t then return nil end
    -- Rotation part: apply to unit axes, relative to the bone's position.
    local o = t:apply(require('openmw.util').vector3(0, 0, 0))
    return { t:apply(require('openmw.util').vector3(10, 0, 0)) - o, t:apply(require('openmw.util').vector3(0, 10, 0)) - o }
end

local function angleChange(a, b)
    if not a or not b then return 0 end
    return math.max((a[1] - b[1]):length(), (a[2] - b[2]):length())
end

return {
    engineHandlers = {
        onInit = function(data)
            model = data.model
            spec = (model:find('bip01') or model:find('legacy')) and BIP01 or CUSTOM
            tag = '[' .. model:match('([^/]+)%.dae$') .. ']'
        end,
        onUpdate = function(dt)
            if not spec then return end
            timer = timer + dt
            if phase == 'wait' and timer > 2 then
                check(animation.hasAnimation(self), 'actor has animation')
                for _, b in ipairs(spec.bones) do
                    check(animation.hasBone(self, b), 'bone "' .. b .. '" present under its exact name')
                end
                for _, c in ipairs(spec.clips) do
                    check(animation.hasGroup(self, c[1]), 'clip group "' .. c[1] .. '" available')
                end
                phase, timer = 'clip', 0
            elseif phase == 'clip' then
                local c = spec.clips[clipIndex]
                if not c then
                    print('[rig-test] ' .. tag .. ' finished')
                    phase = 'done'
                    return
                end
                -- Control: how much the bone moves in 0.3 s without the clip
                -- (stock animations also drive Bip01-named bones). Then the clip.
                if step == nil then
                    step, timer, control0 = 'control', 0, rotationOf(c[2])
                elseif step == 'control' and timer > 0.3 then
                    control = angleChange(control0, rotationOf(c[2]))
                    before = rotationOf(c[2])
                    if animation.hasGroup(self, c[1]) then
                        animation.playBlended(self, c[1], { priority = animation.PRIORITY.Scripted,
                            blendMask = animation.BLEND_MASK.All, startKey = 'start', stopKey = 'stop', loops = 0 })
                    end
                    step, timer = 'clip', 0
                elseif step == 'clip' and timer > 0.3 then
                    local moved = angleChange(before, rotationOf(c[2]))
                    print(string.format('[rig-test] %s debug %s playing=%s time=%s completion=%s',
                        tag, c[1], tostring(animation.isPlaying(self, c[1])),
                        tostring(animation.getCurrentTime(self, c[1])), tostring(animation.getCompletion(self, c[1]))))
                    check(moved > control + 3, string.format('clip "%s" moves bone "%s" (%.2f, without clip %.2f)',
                        c[1], c[2], moved, control))
                    animation.cancel(self, c[1])
                    step, clipIndex, timer = nil, clipIndex + 1, 0
                end
            end
        end,
    },
}
