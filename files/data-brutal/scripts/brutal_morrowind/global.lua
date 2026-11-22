local world = require('openmw.world')
local types = require('openmw.types')
local util = require('openmw.util')
local C = require('scripts.brutal_morrowind.config')
local S = require('scripts.brutal_morrowind.settings')
local storage = require('openmw.storage')
assert(type(world.applyObjectImpulse) == 'function', 'Brutal Morrowind requires native dynamic props')
return {
    engineHandlers = {onPlayerAdded = function(player)
        player:sendEvent('BrutalPhysicsAvailable', {available = true,
            superKick = type(world.applyNpcSuperKick) == 'function'})
    end},
    eventHandlers = {BrutalSettingsChanged = function(data)
        if data.player ~= world.players[1] or type(data.values) ~= 'table' then return end
        storage.globalSection(S.SYNC):set('values', data.values)
    end, BrutalPropImpulse = function(data)
        if not S.get('dynamicProps') then return end
        local item, attacker = data.item, data.attacker
        if not item or not item:isValid() or not item.enabled or not item.cell
            or not attacker or not attacker:isValid() or types.Actor.isDead(attacker)
            or item.cell ~= attacker.cell or not types.Item.objectIsInstance(item)
            or types.Light.objectIsInstance(item) or (item.position-attacker.position):length() > C.reach+20 then return end
        local direction = util.vector3(data.direction.x, data.direction.y, 0)
        if direction:length() < 0.1 then return end
        -- Morrowind weight is converted to a tuned gameplay mass, not claimed SI data.
        local record = item.type.record(item)
        local mass = math.max(0.25, math.min(50, (record.weight or 1) * 0.5))
        local velocity = direction:normalize() * math.min(700, 350/mass)
            + util.vector3(0, 0, math.min(180, 90/mass))
        world.applyObjectImpulse(item, velocity * mass, mass)
    end},
}
