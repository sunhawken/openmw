-- OpenMW world units. These values are deliberately conservative for the first slice.
return {
    blockButton = 3, -- Right mouse; held to raise the shield (engine setting `player active shield block`).
    reach = 150,
    cone = 0.80,
    -- Dark Messiah stamina, in its own units out of `staminaMax` (player_stamina_base_max);
    -- applied as fractions of the Morrowind fatigue maximum.
    staminaMax = 9,
    kickStaminaNeeded = 3, -- mm_player_stamina_needed_for_kick
    kickStaminaNpc = 3, -- stamina_kick_npc: a kick that strikes an NPC
    kickStaminaOther = 1, -- stamina_kick_all: a kick that strikes anything else
    sprintStaminaPerSecond = 2, -- mm_player_stamina_megasprint_cost_per_second
    sprintStaminaNeeded = 3, -- needed to start; an active sprint spends down to zero
    cooldown = 0.95,
    windup = 0.375,
    -- mm_player_minangle_to_medium_kick: looking down this far plays the low kick.
    lowKickPitch = math.rad(50),
    healthDamage = 6,
    fatigueDamage = 35,
    pushSpeed = 460,
    -- Full adrenaline always reaches ragdoll; its default speed matches the ordinary push.
    -- The speed itself is the superKickSpeed option on the settings page.
    superKickLift = 0.32,
    superKickHealthDamage = 12,
    pushDrag = 850,
    wallDamage = 12,
    wallMinSpeed = 180,
    -- A kick reaching no living actor strikes a ragdoll corpse or severed head.
    ragdollKickSpeed = 420,
    ragdollKickLift = 0.35,
    -- Shield bash from the raised guard (Dark Messiah layer_parry_sword_shield_bash:
    -- HitPow 0.01, always pushes, breaks the target's guard, costs no stamina).
    bashWindup = 0.30, -- source hit event 2102
    guardChargeTime = 0.36, -- Dark Messiah's mm_player_mintime_to_bigslash, scaled: a longer press charges
    bashHealthDamage = 1,
    bashFatigueDamage = 20,
    bashPushSpeed = 300, -- weaker than the kick's pushSpeed
    -- A kick that sends a living NPC over a ledge lifts it slightly (fraction of
    -- pushSpeed). The confirmed fall fills adrenaline without changing time.
    ledgeKickLift = 0.35,
    -- A ragdoll kick during the finisher's slow motion prolongs it (real seconds).
    slowMotionKickExtension = 1.0,
    slowMotionKickExtensionMax = 2.0,
}
