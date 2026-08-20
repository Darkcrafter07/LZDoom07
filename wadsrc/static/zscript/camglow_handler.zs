// Camglow dynlight by Darkcrafter07
// Based on "Flashlight++" by SuaveSteve: https:// forum.zdoom.org/viewtopic.php?t=75585

class camglow_Handler : StaticEventHandler 
{
    // Helper method to grant the holder item to the player
    camglow_Holder setupcamglowHolder(PlayerPawn p)
    {
        camglow_Holder holder = camglow_Holder(p.GiveInventoryType("camglow_Holder"));
        if (holder) holder.Init();
        return holder;
    }

	override void WorldLoaded(WorldEvent e)
	{
	    // 1. Full purge of all "orphaned" dynlights in the map
	    ThinkerIterator it = ThinkerIterator.Create("camglow_Light");
	    camglow_Light hl;
	    while (hl = camglow_Light(it.Next())) 
	    {
	        hl.Destroy();
	    }

	    // 2. Holders state reset for players
	    for (int i = 0; i < MAXPLAYERS; i++)
	    {
	        if (!playeringame[i] || !players[i].mo) continue;
	        
	        let holder = camglow_Holder(players[i].mo.FindInventory("camglow_Holder"));
	        if (holder)
	        {
	            holder.on = false; 
	            holder.light1 = null;
	            holder.light2 = null;
	        }
	    }
	}

    override void WorldTick()
    {
        // Optimization: check once every 15 ticks (~0.4 seconds)
        if (level.time % 15 != 0) return;

        // Find the C++ Engine CVar
        let cv = CVar.FindCVar("gl_camglowlight");
        bool engineActive = (cv && cv.GetBool());

        for (int i = 0; i < MAXPLAYERS; i++)
        {
            if (!playeringame[i] || !players[i].mo) continue;

            PlayerPawn p = players[i].mo;
            camglow_Holder holder = camglow_Holder(p.FindInventory("camglow_Holder"));

            if (engineActive)
            {
                // KEY LOGIC: Re-enable the light if it's supposed to be on, 
                // but the actor is missing (e.g., after map/cluster change).
                if (!holder || !holder.on || (holder.light1 == null))
                {
                    if (!holder) holder = setupcamglowHolder(p);
                    
                    // Recreate the light actor
                    if (holder) holder.Enable(); 
                }
            }
            else
            {
                // Turn off the light if the Engine CVar is disabled
                if (holder && holder.on)
                {
                    holder.Disable();
                }
            }
        }
    }

    override void NetworkProcess(ConsoleEvent e)
    {
        // Handle full mod uninstallation
        if (e.name == "camglow_plus_uninstall")
        {
            // Clean up all light actors in the world
            ThinkerIterator it = ThinkerIterator.Create("camglow_Light");
            camglow_Light hl;
            while (hl = camglow_Light(it.Next())) hl.Destroy();
            
            // Remove holder items from all active players
            for (int i = 0; i < MAXPLAYERS; i++)
            {
                if (playeringame[i] && players[i].mo)
                {
                    let h = players[i].mo.FindInventory("camglow_Holder");
                    if (h) h.Destroy();
                }
            }
            Console.Printf("Camglow Mod: Uninstalled successfully.");
        }
    }

    // === FIX DOUBLE FLASHLIGHT ON DEATH / RESPAWN ===   
    // Instead of PlayerDied, old GZDoom 3.x uses WorldThingDied for all map casualties
    override void WorldThingDied(WorldEvent e)
    {
        // Check if the entity that died is a valid player character
        if (e.Thing && e.Thing.player)
        {
            let holder = camglow_Holder(e.Thing.FindInventory("camglow_Holder"));
            if (holder)
            {
                holder.Disable(); // Cleanly calls .destroy() on light1 and light2 instantly
                holder.on = false;
            }
        }
    }

    // Instead of PlayerRespawned, we intercept when the new PlayerPawn body spawns into the map
    override void WorldThingSpawned(WorldEvent e)
    {
        if (e.Thing && e.Thing.player)
        {
            PlayerPawn p = PlayerPawn(e.Thing);
            if (!p) return;

            // 1. Forceful sweep: clear any old left-over orphaned lights linked to this exact player pawn pointer
            ThinkerIterator it = ThinkerIterator.Create("camglow_Light");
            camglow_Light hl;
            while (hl = camglow_Light(it.Next())) 
            {
                // If a "ghost" light is floating around holding this player as master, kill it!
                if (hl.master == p) 
                {
                    hl.Destroy();
                }
            }

            // 2. Clear inventory holder internal pointers to prepare a clean slate for the new body
            let holder = camglow_Holder(p.FindInventory("camglow_Holder"));
            if (holder)
            {
                holder.on = false; 
                holder.light1 = null;
                holder.light2 = null;
            }
        }
    }
    // =================================================

}
