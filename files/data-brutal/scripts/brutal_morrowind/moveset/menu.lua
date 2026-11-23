-- Options > Scripts > Brutal Morrowind move sets: on/off, combo display and
-- the six move set inputs (all unbound by default).
local I = require('openmw.interfaces')
local Input = require('scripts.brutal_morrowind.moveset.input')

local L = Input.L
I.Settings.registerPage({ key = 'BrutalMoveset', l10n = L, name = 'PageName', description = 'PageDescription' })

local settings = {
    { key = 'enabled', renderer = 'checkbox', name = 'enabled', description = 'enabledDescription', default = true },
    { key = 'hud', renderer = 'checkbox', name = 'hud', description = 'hudDescription', default = false },
}
for _, name in ipairs(Input.ORDER) do
    settings[#settings + 1] = {
        key = name .. 'Binding', renderer = 'inputBinding', name = name .. 'Binding',
        description = name .. 'BindingDescription', default = 'BrutalMoveset_' .. name,
        argument = { key = Input.TRIGGERS[name], type = 'trigger' },
    }
end

I.Settings.registerGroup({
    key = 'SettingsBrutalMoveset',
    page = 'BrutalMoveset',
    l10n = L,
    name = 'PageName',
    permanentStorage = true,
    settings = settings,
})
return {}
