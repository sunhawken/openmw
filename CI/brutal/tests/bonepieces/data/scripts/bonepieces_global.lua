-- In-engine test for multi-piece equipment. Creates a weapon whose model
-- has AttachTo: pieces (generated boxes), gives it to the player and asks the
-- player script to equip and draw it, then unequip it. The runner checks the
-- engine log for the expected moved/detached lines.
local world = require('openmw.world')
local types = require('openmw.types')

local done = false
return {
    engineHandlers = {
        onUpdate = function()
            if done then return end
            local player = world.players[1]
            if not player then return end
            done = true
            local draft = types.Weapon.createRecordDraft({
                name = 'Bone pieces test',
                model = 'meshes/test/bonepieces.osg',
                type = types.Weapon.TYPE.ShortBladeOneHand,
                weight = 1, value = 1, health = 100, speed = 1, reach = 1,
                chopMinDamage = 1, chopMaxDamage = 2, slashMinDamage = 1, slashMaxDamage = 2,
                thrustMinDamage = 1, thrustMaxDamage = 2,
            })
            local record = world.createRecord(draft)
            local item = world.createObject(record.id)
            item:moveInto(types.Actor.inventory(player))
            print('[bonepieces-test] created weapon ' .. record.id)
            player:sendEvent('BonePiecesTestEquip', { recordId = record.id })
        end,
    },
}
