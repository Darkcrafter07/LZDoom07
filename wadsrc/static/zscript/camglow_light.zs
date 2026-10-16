// Camglow dynlight by Darkcrafter07
// Based on "Flashlight++" by SuaveSteve: https:// forum.zdoom.org/viewtopic.php?t=75585

// This is the Light itself, controlled by the Engine CVar and player movement
class camglow_Light : SpotLightCamGlowStraight
{
	bool thisIsLight2;
	color baseColor;
	bool shouldInterpolate;
	PlayerPawn toFollow;
	Vector3 offset;

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

	// Initialize the light settings
	camglow_Light Init(PlayerPawn p, bool second)
	{
		toFollow = p;
		target = p; // Set target for native "don't light self" checks if needed
		thisIsLight2 = second;

		// 1. Get base color from the player's CVar
		Color c = CVar.GetCVar("cl_camglow_plus_color", toFollow.player).GetString();
		
		// Cache the raw base color for dynamic updates inside Tick
		baseColor = c;
		
		// 2. Set basic light properties
		bATTENUATE = CVar.GetCVar("cl_camglow_attenuated", toFollow.player).GetBool();
		
		// 3. Spotlight cone setup (will be maintained in Tick)
		SpotInnerAngle = 30.0;
		SpotOuterAngle = 115.0;
		
		shouldInterpolate = CVar.GetCVar("cl_camglow_interpolate", toFollow.player).GetBool();
		
		// 4. Location and Offset setup
		double zBump = toFollow.height * 0.067;  // 1.0 / 15.0
		int loc = CVar.GetCVar("cl_camglow_plus_location", toFollow.player).GetInt();

		switch(loc) 
		{
			case LOC_HELMET:
				offset = (0, 0, zBump);
				break;
				
			case LOC_RIGHT_SHOULDER:
				offset = (toFollow.radius, 0, -zBump);
				break;
				
			case LOC_LEFT_SHOULDER:
				offset = (-toFollow.radius, 0, -zBump);
				break;
				
			case LOC_CAMERA:
				offset = (0, 0, 0);
				break;
				
			case LOC_GUN:
				offset = (0, 0, -8);
				break;
				
			case LOC_CUSTOM:
				offset = (
					CVar.GetCVar("cl_camglow_custpos_x", toFollow.player).GetInt(),
					CVar.GetCVar("cl_camglow_custpos_y", toFollow.player).GetInt(),
					CVar.GetCVar("cl_camglow_custpos_z", toFollow.player).GetInt()
				);
				break;
				
			default:
				offset = (0, 0, 0);
				break;
		}

		return self;
	}

	override void Tick() 
	{
		Super.Tick();
		
		if (!toFollow || !toFollow.player)
			return;

		// --- Owner only visibility check ---
		if (!toFollow.CheckLocalView())
		{
			args[3] = 0;
			return; 
		}

		// 1. Optimization: read Engine CVars once per 15 ticks
		if (level.time % 15 == 0)
		{
			let cv = CVar.FindCVar("gl_camglowlight");
			let vis = CVar.FindCVar("r_visibility");
			let lm = CVar.FindCVar("gl_lightmode"); // Track current engine light mode
			
			bool engineEnabled = (cv && cv.GetBool());
			
			// --- NO CLAMP ON MULTIPLIER (Only base calculation) ---
			// Calculate visibility multiplier. Base 8.0 = 1.0x gain.
			// If r_visibility is 16.0, gain will be 2.0x.
			double visibilityMult = (vis != null) ? (vis.GetFloat() / 8.0) : 1.0;

			if (engineEnabled && visibilityMult > 0)
			{
				// Determine which intensity (RADIUS) CVar to read
				string sfx = thisIsLight2 ? "_2" : "";
				
				// Get base RADIUS directly from player's user CVar
				int baseRadius = CVar.GetCVar("cl_camglow_plus_intensity"..sfx, toFollow.player).GetInt();
				
				// Calculate final RADIUS based on visibility multiplier
				double finalRadius = baseRadius * visibilityMult;
				
				// Apply to Light RADIUS (args[3]) -> FIXED: Stays intact as radius!
				args[3] = finalRadius;

				// --- DYNAMIC TRUE INTENSITY (COLOR RGB) CALCULATION ---
				double rFactor = 1.0;
				double gFactor = 1.0;
				double bFactor = 1.0;

				// Apply multiplier ONLY to the second light source (far-field beam)
				if (thisIsLight2)
				{
					float mult = CVar.GetCVar("cl_camglow_plus_color_2_mult", toFollow.player).GetFloat();
					rFactor *= mult;
					gFactor *= mult;
					bFactor *= mult;
				}

				// --- "DOOM" SECTOR LIGHT MODE INTERCEPT (gl_lightmode == 2) ONLY ---
				// Soften the color flare by 2.5x to match the slight ambient glow requirements
				if (lm && lm.GetInt() == 2)
				{
					rFactor *= 0.40; // Drops light emission intensity down correctly
					gFactor *= 0.40;
					bFactor *= 0.40;
				}

				// Apply finalized dynamic RGB color values to args[0..2] -> FIXED: Proper arrays!
				args[0] = baseColor.r * rFactor;
				args[1] = baseColor.g * gFactor;
				args[2] = baseColor.b * bFactor;

				// DEBUG PRINT: Open console (~) to see these values
				//Console.Printf("Light %d | Radius: %d | Color: (%d, %d, %d)", thisIsLight2 ? 2 : 1, args[3], args[0], args[1], args[2]);
			}
			else
			{
				// Kill the light if engine toggle is off or visibility is zero
				args[3] = 0; 
			}
		}

		// 2. If light is off, stop processing
		if (args[3] <= 0)  return;

		// 3. Simple sync with player position (Sway removed)
		Vector3 posToSet = toFollow.pos;
		double angleToSet = toFollow.angle;
		double pitchToSet = toFollow.pitch;

		// 4. Update angles
		A_SetAngle(angleToSet, shouldInterpolate ? SPF_INTERPOLATE : 0);
		A_SetPitch(pitchToSet, shouldInterpolate ? SPF_INTERPOLATE : 0);
		
		// --- LIGHTING FIXES (Steep Surfaces & Sprites) ---
		SpotInnerAngle = 30.0; 
		SpotOuterAngle = 115.0; 
		bATTENUATE = true; 

		// Calculate view direction for offset math
		Vector3 viewDir =
		(
			cos(angleToSet) * cos(pitchToSet),
			sin(angleToSet) * cos(pitchToSet),
			-sin(pitchToSet)
		);

		// Current Eye Height (accounts for crouching)
		double curViewH = toFollow.ViewHeight + toFollow.player.crouchviewdelta;

		// Base position at Helmet/Eye level
		Vector3 basePos = posToSet + 
		(
			RotateVector((offset.x, offset.y * cos(toFollow.Pitch)), toFollow.angle - 90.0), 
			curViewH + offset.z + (offset.y * -sin(toFollow.Pitch))
		);

		// --- SMART BACKWARDS OFFSET (Collision Check) ---
		FLineTraceData trace;
		double maxBackDist = 1.0; 
		
		toFollow.LineTrace
		(
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
			// Padding to prevent light bleeding into walls
			actualDist = clamp(trace.Distance - 6.0, 0.0, maxBackDist);
		}

		// Final positioning
		Vector3 finalPos = basePos - (viewDir * actualDist);
		SetOrigin(finalPos, shouldInterpolate);
	}
}
