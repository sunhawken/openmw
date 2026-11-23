local self = require('openmw.self')
local types = require('openmw.types')
local core = require('openmw.core')

local phase, timer, recordId = nil, 0, nil
local SLOT = types.Actor.EQUIPMENT_SLOT.CarriedRight

return {
    eventHandlers = {
        BonePiecesTestEquip = function(data)
            recordId = data.recordId
            phase = 'equip'
        end,
    },
    engineHandlers = {
        onUpdate = function(dt)
            if not phase then return end
            timer = timer + dt
            if phase == 'equip' then
                local eq = types.Actor.getEquipment(self)
                eq[SLOT] = recordId
                types.Actor.setEquipment(self, eq)
                types.Actor.setStance(self, types.Actor.STANCE.Weapon)
                print('[bonepieces-test] equipped and drew weapon')
                phase, timer = 'drawn', 0
            elseif phase == 'drawn' and timer > 3 then
                local eq = types.Actor.getEquipment(self)
                eq[SLOT] = nil
                types.Actor.setEquipment(self, eq)
                print('[bonepieces-test] unequipped weapon')
                phase, timer = 'removed', 0
            elseif phase == 'removed' and timer > 1 then
                print('[bonepieces-test] finished')
                phase = nil
                core.quit()
            end
        end,
    },
}
