-- Attached to the test target: keep it passive so it neither attacks the
-- player nor runs away during the test.
local self = require('openmw.self')
local types = require('openmw.types')
types.Actor.stats.ai.fight(self).base = 0
types.Actor.stats.ai.flee(self).base = 0
return {}
