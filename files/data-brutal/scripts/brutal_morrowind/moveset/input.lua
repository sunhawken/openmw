-- Move set input triggers, shared by MENU and PLAYER scripts so the Controls
-- binding renderer can see them. No default keys are assigned: existing
-- bindings stay untouched until the player binds these in Options.
local input = require('openmw.input')

local L = 'BrutalMoveset'
local TRIGGERS = {
    light = 'BrutalMovesetLight',
    heavy = 'BrutalMovesetHeavy',
    skill1 = 'BrutalMovesetSkill1',
    skill2 = 'BrutalMovesetSkill2',
    skill3 = 'BrutalMovesetSkill3',
    dodge = 'BrutalMovesetDodge',
}
local ORDER = { 'light', 'heavy', 'skill1', 'skill2', 'skill3', 'dodge' }

for _, name in ipairs(ORDER) do
    local key = TRIGGERS[name]
    if not input.triggers[key] then
        input.registerTrigger({ key = key, l10n = L, name = name .. 'Binding', description = name .. 'BindingDescription' })
    end
end

return { L = L, TRIGGERS = TRIGGERS, ORDER = ORDER }
