// Flashlight++ - https:// forum.zdoom.org/viewtopic.php?t=75585

// This is the Light itself, controlled by the Engine CVar and player movement
class camglow_Light : SpotLight
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
		target = p; 
		thisIsLight2 = second;

		// Get base color from CVar
		Color c = CVar.GetCVar("cl_camglow_plus_color", toFollow.player).GetString();
		string suffix = second ? "_2" : "";

		if (second)
		{
			float mult = CVar.GetCVar("cl_camglow_plus_color_2_mult", toFollow.player).GetFloat();
			args[0] = (c.r * mult);
			args[1] = (c.g * mult);
			args[2] = (c.b * mult);
		}
		else
		{
			args[0] = c.r;
			args[1] = c.g;
			args[2] = c.b;
		}

		baseColor = c;
		args[3] = CVar.GetCVar("cl_camglow_plus_intensity"..suffix, toFollow.player).GetInt();
		shouldInterpolate = CVar.GetCVar("cl_camglow_interpolate", toFollow.player).GetBool();
		
		// Set initial SpotLight cones (Values will be maintained in Tick for steep angles)
		SpotInnerAngle = 30.0; 
		SpotOuterAngle = 115.0; 
		bATTENUATE = true; 

		// Location setup
		double zBump = toFollow.height / 15.0;
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

		// 1. Optimization: read Engine CVar every 15 ticks
		if (level.time % 15 == 0)
		{
			let cv = CVar.FindCVar("gl_camglowlight");
			bool engineEnabled = (cv && cv.GetBool());
			// Console.Printf("Engine Light Status: %d", engineEnabled);

			if (engineEnabled)
			{
				string suffix = thisIsLight2 ? "_2" : "";
				args[3] = CVar.GetCVar("cl_camglow_plus_intensity"..suffix, toFollow.player).GetInt();
			}
			else
			{
				args[3] = 0; 
			}
		}

		// 2. If light is off, stop processing
		if (args[3] <= 0) 
			return;

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
		Vector3 viewDir = (
			cos(angleToSet) * cos(pitchToSet),
			sin(angleToSet) * cos(pitchToSet),
			-sin(pitchToSet)
		);

		// Current Eye Height (accounts for crouching)
		double curViewH = toFollow.ViewHeight + toFollow.player.crouchviewdelta;

		// Base position at Helmet/Eye level
		Vector3 basePos = posToSet + (
			RotateVector((offset.x, offset.y * cos(toFollow.Pitch)), toFollow.angle - 90.0), 
			curViewH + offset.z + (offset.y * -sin(toFollow.Pitch))
		);

		// --- SMART BACKWARDS OFFSET (Collision Check) ---
		FLineTraceData trace;
		double maxBackDist = 1.0; 
		
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
			// Padding to prevent light bleeding into walls
			actualDist = clamp(trace.Distance - 6.0, 0.0, maxBackDist);
		}

		// Final positioning
		Vector3 finalPos = basePos - (viewDir * actualDist);
		SetOrigin(finalPos, shouldInterpolate);
	}
}