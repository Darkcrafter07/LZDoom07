// Based on Flashlight++ - https:// forum.zdoom.org/viewtopic.php?t=75585

// This is the PointLight itself, what the Holder turns on and off
class camglow_Light : SpotLight
{
	double vela, velp;
	double spring, damping;
	int inertia;
	bool shouldSway;
	
	bool alertMonstersWithLight;
	double cosBeamAngle, distanceToWake;
	
	vector3 posToSet;
	double pitchToSet;
	double angleToSet;
	
	bool thisIsLight2;
	
	color baseColor;
	bool noise;
	double minNoise;
	
	bool shouldInterpolate;
	
	Default 
	{
		+NOINTERACTION;
	}
	
	enum ELocation 
	{
		LOC_HELMET = 0,
		LOC_RIGHT_SHOULDER = 1,
		LOC_LEFT_SHOULDER = 2,
		LOC_CAMERA = 3,
		LOC_GUN = 4,
		LOC_CUSTOM = 5
	};
	
	enum ESwayType 
	{
		SWT_NONE = 0,
		SWT_SPRINGY = 1,
		SWT_STIFF = 2,
		SWT_CUSTOM = 3
	};
	
	PlayerPawn toFollow;
	
	Vector3 offset;
	
	// This is run whenever the camglow is turned on.
	camglow_Light Init( PlayerPawn p, bool second )
	{
		toFollow = p;
		
		Color c = CVar.GetCVar( "cl_camglow_plus_color", toFollow.player ).GetString();
		
		if( second )
		{
			float mult = CVar.GetCVar( "cl_camglow_plus_color_2_mult", toFollow.player ).GetFloat();
			args[0] = c.r * mult;
			args[1] = c.g * mult;
			args[2] = c.b * mult;
		}
		else
        {
			args[0] = c.r;
			args[1] = c.g;
			args[2] = c.b;
		}
		
		baseColor = c;
		
		thisIsLight2 = second;
		
		noise = CVar.GetCVar( "cl_camglow_noise", toFollow.player ).GetBool();
		minNoise = CVar.GetCVar( "cl_camglow_noise_min", toFollow.player ).GetFloat();
		bATTENUATE = CVar.GetCVar( "cl_camglow_attenuated", toFollow.player ).GetBool();
		
		string suffix = second ? "_2" : "";
		
		// [gng] This is the intensity, i don't think there's a nicely named variable for it, and i'm too lazy to check
		args[3] = CVar.GetCVar( "cl_camglow_plus_intensity"..suffix, toFollow.player ).GetInt();
		
		SpotInnerAngle = CVar.GetCVar( "cl_camglow_plus_inner"..suffix, toFollow.player ).GetFloat();
		SpotOuterAngle = CVar.GetCVar( "cl_camglow_plus_outer"..suffix, toFollow.player ).GetFloat();
		
		alertMonstersWithLight = CVar.GetCVar( "cl_camglow_alertmonsters", toFollow.player ).GetBool();
		
		double zBump = toFollow.height / 15.0;
		
		shouldInterpolate = CVar.GetCVar( "cl_camglow_interpolate", toFollow.player ).GetBool();
		
		cosBeamAngle = cos( SpotOuterAngle );

		distanceToWake = args[3] / sqrt( 1.0 - cosBeamAngle );
		
		switch( CVar.GetCVar( "cl_camglow_sway_type", toFollow.player ).GetInt() )
		{
			default:
			case SWT_NONE:
				shouldSway = false;
				inertia = 1;
				spring = 1;
				damping = 1;
				break;
				
			case SWT_SPRINGY:
				shouldSway = true;
				spring = 0.25;
				damping = 0.2;
				inertia = 4;
				break;
		
			case SWT_STIFF:
				shouldSway = true;
				spring = 0.35;
				damping = 0.75;
				inertia = 2;
				break;
				
			case SWT_CUSTOM:
				shouldSway = true;
				spring = CVar.GetCVar( "cl_camglow_sway_spring", toFollow.player ).GetFloat();
				damping = CVar.GetCVar( "cl_camglow_sway_damping", toFollow.player ).GetFloat();
				inertia = CVar.GetCVar( "cl_camglow_sway_inertia", toFollow.player ).GetInt();
				break;
		}
			
		if( shouldSway )
		{
			angle = toFollow.angle;
			pitch = toFollow.pitch;
		}
		
		switch( CVar.GetCVar( "cl_camglow_plus_location", toFollow.player ).GetInt() ) 
		{
			case LOC_HELMET:
				offset = ( 0, 0, zBump );
				break;
				
			case LOC_RIGHT_SHOULDER:
				offset = ( toFollow.radius, 0, -zBump );
				break;
				
			case LOC_LEFT_SHOULDER:
				offset = ( -toFollow.radius, 0, -zBump );
				break;
				
			case LOC_CAMERA:
				offset = ( 0, 0, 0 );
				break;
				
			case LOC_GUN:
	            offset = ( 0, 0, -8 );
	            break;
	            
	        case LOC_CUSTOM:
	        	int custOffsetX = CVar.GetCVar( "cl_camglow_custpos_x", toFollow.player ).GetInt();
		        int custOffsetY = CVar.GetCVar( "cl_camglow_custpos_y", toFollow.player ).GetInt();
		        int custOffsetZ = CVar.GetCVar( "cl_camglow_custpos_z", toFollow.player ).GetInt();
		        
	            offset = ( custOffsetX, custOffsetY, custOffsetZ );
	            break;
	            
			default:
				offset = ( 0, 0, 0 );
				break;
		}

		return self;
	}



	override void Tick() 
	{
		Super.Tick();
		
		if (!toFollow || !toFollow.player)
			return;

		// 1. Optimization: read heavy Cvar not every frame but once per 5 ticks
		if (level.time % 5 == 0)
		{
			let cv = CVar.FindCVar("gl_camglowlight");
			bool engineEnabled = (cv && cv.GetBool());

			if (engineEnabled)
			{
				string suffix = thisIsLight2 ? "_2" : "";
				// Get intensity from the user CVar
				args[3] = CVar.GetCVar("cl_camglow_plus_intensity"..suffix, toFollow.player).GetInt();
			}
			else
			{
				args[3] = 0; // Turn the light off
			}
		}

		// 2. If light is off, no reason to read Sway and color
		if (args[3] <= 0) 
			return;

		// 3. Move logic (must occur every tick for smoothness)
		if (shouldSway)
		{
			if (inertia <= 0) inertia = 1;
			vel.x += DampedSpring(pos.x, toFollow.pos.x, vel.x, 1, 1);
			vel.y += DampedSpring(pos.y, toFollow.pos.y, vel.y, 1, 1);
			vel.z += DampedSpring(pos.z, toFollow.pos.z, vel.z, 1, 1);
			vela  += DampedSpring(angle, toFollow.angle, vela, spring, damping);
			velp  += DampedSpring(pitch, toFollow.pitch, velp, spring, damping);
			posToSet = pos + vel;
			angleToSet = angle + (vela / inertia);
			pitchToSet = pitch + (velp / inertia);
		}
		else
		{
			posToSet = toFollow.pos;
			angleToSet = toFollow.angle;
			pitchToSet = toFollow.pitch;
		}

		// 4. Color and flicker logic
		if (noise)
		{
			float flicker = frandom(minNoise, 1.0);
			args[0] = (baseColor.r * flicker);
			args[1] = (baseColor.g * flicker);
			args[2] = (baseColor.b * flicker);
		}
		else
		{
			args[0] = baseColor.r;
			args[1] = baseColor.g;
			args[2] = baseColor.b;
		}
		
		// 5. Angles and positions update (every tick)
		A_SetAngle(angleToSet, shouldInterpolate ? SPF_INTERPOLATE : 0);
		A_SetPitch(pitchToSet, shouldInterpolate ? SPF_INTERPOLATE : 0);
		
		// --- FIX FOR STEEP SURFACES & SPRITES ---
		// Increase the outer cone to 115 degrees. 
		// This makes the light nearly omnidirectional at the edges, preventing black artifacts on steep walls.
		SpotInnerAngle = 30.0; 
		SpotOuterAngle = 115.0; 

		// Force Attenuation on.
		// In legacy renderers, this helps the light "wrap" around corners and steep angles more naturally.
		bATTENUATE = true; 

		// Calculate view direction vector
		Vector3 viewDir = (
			cos(angleToSet) * cos(pitchToSet),
			sin(angleToSet) * cos(pitchToSet),
			-sin(pitchToSet)
		);

		// Calculate base position (Helmet location)
		// Account for view height and crouching delta
		double curViewH = toFollow.ViewHeight + toFollow.player.crouchviewdelta;
		Vector3 basePos = posToSet + (RotateVector((offset.x, offset.y * cos(toFollow.Pitch)), toFollow.angle - 90.0), 
										curViewH + offset.z + (offset.y * -sin(toFollow.Pitch)));

		// --- SMART BACKWARDS OFFSET (Collision) ---
		FLineTraceData trace;
		double maxBackDist = 1.0; 
		
		// Perform a trace backwards to check for walls immediately behind the head
		toFollow.LineTrace(
			toFollow.angle + 180.0, 
			maxBackDist, 
			-toFollow.pitch, 
			TRF_THRUACTORS, 
			curViewH + offset.z, 
			0, 0, 
			trace
		);

		double actualDist = maxBackDist;
		if (trace.HitType != TRACE_HitNone)
		{
			// Padding is crucial. The further the light source is from the wall, 
			// the less likely it is to trigger "steep surface" rendering bugs.
			actualDist = clamp(trace.Distance - 6.0, 0.0, maxBackDist);
		}

		// Apply the final offset and set the light origin
		Vector3 finalPos = basePos - (viewDir * actualDist);
		SetOrigin(finalPos, shouldInterpolate);
	}



	double DampedSpring( double p, double r, double v, double k, double d ) 
	{
		return -( d * v ) - ( k * ( p - r ) );
	}
}
