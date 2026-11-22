-- Required custom OpenMW engine API, local SELF only.
-- World-space delta velocity in game units/second; no mass or force parameter.
local types = require('openmw.types')
local ok, addExternalVelocity = pcall(function() return types.Actor.addExternalVelocity end)
assert(ok and type(addExternalVelocity) == 'function',
    'Brutal Morrowind requires its modified OpenMW engine. Use scripts/play_brutal.sh.')
return {
    available = true,
    apply = function(actor, worldDeltaVelocity)
        addExternalVelocity(actor, worldDeltaVelocity)
    end,
}
