// Based on Flashlight++ - https:// forum.zdoom.org/viewtopic.php?t=75585
class camglow_Handler : StaticEventHandler 
{
	override void WorldTick()
	{
	    // to optimize CPU usage, only check once per 15 ticks (0.4 sec)
	    if (level.time % 15 != 0) 
	        return;
	
	    let cv = CVar.FindCVar("gl_camglowlight");
	    // If CVar not found, turn everything OFF
	    bool engineActive = (cv && cv.GetBool());
	
	    for (int i = 0; i < MAXPLAYERS; i++)
	    {
	        // Check that the game has the player and its object exists
	        if (!playeringame[i] || !players[i].mo) 
	            continue;
	
	        PlayerPawn p = players[i].mo;
	        camglow_Holder holder = camglow_Holder(p.FindInventory("camglow_Holder"));
	
	        if (engineActive)
	        {
	            // If the C++ CVAR is ON but camglow is absent or it's in the "OFF" state
	            if (!holder || !holder.on)
	            {
	                if (!holder) 
	                {
	                    holder = camglow_Holder(p.GiveInventoryType("camglow_Holder"));
	                    holder.Init(); // Crucial to initialize a new holder
	                }
	                holder.Enable();
	            }
	        }
	        else
	        {
	            // If the C++ CVAR is OFF but the holder still treats it "ON"
	            if (holder && holder.on)
	            {
	                holder.Disable();
	            }
	        }
	    }
	}

    override void NetworkProcess(ConsoleEvent e)
    {
        // Now react only on command of full mod removal from the game
        if (e.name == "camglow_plus_uninstall")
        {
            // Iterate through all players and remove their item-holder
            for (int i = 0; i < MAXPLAYERS; i++)
            {
                if (!playeringame[i]) continue;
                PlayerPawn p = players[i].mo;
                
                if (p)
                {
                    Inventory holder = p.FindInventory("camglow_Holder");
                    if (holder) 
                    {
                        p.RemoveInventory(holder);
                        // Call Destroy explicitly, to free the memory
                        holder.Destroy();
                    }
                }
            }
            
            // just in case, force clear all remaining light actors in the world
            ThinkerIterator it = ThinkerIterator.Create("camglow_Light");
            camglow_Light hl;
            while (hl = camglow_Light(it.Next()))
            {
                hl.Destroy();
            }

            // ... and all "orphaned" holders
            ThinkerIterator itH = ThinkerIterator.Create("camglow_Holder");
            camglow_Holder h;
            while (h = camglow_Holder(itH.Next()))
            {
                h.Destroy();
            }
            
            //Console.Printf("Flashlight++: Uninstall complete.");
        }
    }
}