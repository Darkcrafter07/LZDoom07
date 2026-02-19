// Camglow dynlight by Darkcrafter07
// Based on "Flashlight++" by SuaveSteve: https:// forum.zdoom.org/viewtopic.php?t=75585

// Handles turning the camglow on and off while the player has a camglow item in their inventory
class camglow_Holder : Inventory 
{
	camglow_Light light1;
	camglow_Light light2;
	bool on;
	bool ifInertia;	



	void Enable()
	{
		PlayerPawn pp = PlayerPawn( owner );
		if( pp )
		{
			if( light1 )
			{
				light1.destroy();
			}
			light1 = camglow_Light( owner.Spawn( "camglow_Light" ) ).Init( pp, false );
			
			if( CVar.GetCVar( "cl_camglow_plus_second_beam", owner.player ).GetBool() )
			{
				if( light2 )
				{
					light2.destroy();
				}
				light2=camglow_Light( owner.Spawn( "camglow_Light" ) ).Init( pp, true );
			}
		}
		on = true;
	}



	void Disable()
	{
		if( light1 )
		{
			light1.destroy();
			light1 = null;
		}
		if( light2 )
		{
			light2.destroy();
			light2 = null;
		}
		on = false;
	}



	camglow_Holder Init()
	{
		light1 = null;
		light2 = null;
		on = false;
		return self;
	}



	void Togglecamglow()
	{
	    // Your regular toggle
	    if (on)
	    {
	        Disable();
	    }
	    else
	    {
	        Enable();
	    }
	}



	void FixState()
	{
		if( !owner )
		{
			destroy();
		}
		else
		{
			if( on )
				Enable();
			else
				Disable();
		}
	}
}