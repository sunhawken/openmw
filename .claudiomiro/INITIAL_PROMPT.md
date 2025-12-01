I'm making a RTT snow deformation system for OpenMW.
The idea is to stick as close as possible from industry standard (laid out in realtimepaper.md )
OpenMW has already a RTT camera system for the water ripples, but it works with array and doesn't have depth (see investigation_report_ripples_vs_snow.md for a comparison)

The way the snow works is by first pushing the terrain vertices up by a certain amount, and then deforming them down where the character, actors, creatures etc walk. This way, we don't have to change the physics of Morrowind, which are buggy.

Right now, the terrain is fully white and deformed down in the area where the RTT camera is active. It's as if the RTT texture was fully white, I guess ?
I want you to create a step by step rock solide debug plan to make this work.