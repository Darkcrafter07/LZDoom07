// gl_lightdata.cpp
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_light.cpp
** Light level / fog management / dynamic lights
**
**/

#include "gl/system/gl_system.h"
#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/data/gl_data.h"
#include "gl/renderer/gl_colormap.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/shaders/gl_shader.h"
#include "gl/scene/gl_portal.h"
#include "c_dispatch.h"
#include "p_local.h"
#include "g_level.h"
#include "r_sky.h"
#include "g_levellocals.h"

// externally settable lighting properties
static float distfogtable[2][256];	// light to fog conversion table for black fog

CVAR(Int, gl_weaponlight, 8, CVAR_ARCHIVE);
CUSTOM_CVAR(Bool, gl_enhanced_nightvision, false, CVAR_ARCHIVE|CVAR_NOINITCALL)
{
	// The fixed colormap state needs to be reset because if this happens when
	// a shader is set to CM_LITE or CM_TORCH it won't register the change in behavior caused by this CVAR.
	if (GLRenderer != nullptr && GLRenderer->mShaderManager != nullptr)
	{
		GLRenderer->mShaderManager->ResetFixedColormap();
	}
}
CVAR(Bool, gl_brightfog, false, CVAR_ARCHIVE);
CVAR(Bool, gl_lightadditivesurfaces, false, CVAR_ARCHIVE);

EXTERN_CVAR(Int, r_fakecontrast)



//==========================================================================
//
// Sets up the fog tables
//
//==========================================================================

CUSTOM_CVAR (Int, gl_distfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	for (int i=0;i<256;i++)
	{

		if (i<164)
		{
			distfogtable[0][i]= (gl_distfog>>1) + (gl_distfog)*(164-i)/164;
		}
		else if (i<230)
		{											    
			distfogtable[0][i]= (gl_distfog>>1) - (gl_distfog>>1)*(i-164)/(230-164);
		}
		else distfogtable[0][i]=0;

		if (i<128)
		{
			distfogtable[1][i]= 6.f + (gl_distfog>>1) + (gl_distfog)*(128-i)/48;
		}
		else if (i<216)
		{											    
			distfogtable[1][i]= (216.f-i) / ((216.f-128.f)) * gl_distfog / 10;
		}
		else distfogtable[1][i]=0;
	}
}

CUSTOM_CVAR(Int,gl_fogmode,1,CVAR_ARCHIVE|CVAR_NOINITCALL)
{
	if (self>2) self=2;
	if (self<0) self=0;
}

//CUSTOM_CVAR(Int, gl_lightmode, 8 ,CVAR_ARCHIVE|CVAR_NOINITCALL)
//{
//	int newself = self;
//	if (newself > 8) newself = 16;	// use 8 and 16 for software lighting to avoid conflicts with the bit mask
//	else if (newself > 4) newself = 8;
//	else if (newself < 0) newself = 0;
//	if (self != newself) self = newself;
//	glset.lightmode = newself;
//}

// GL1x/GL2x legacy software lightmodes state tracking for r_fakecontrast.
// We need to turn on fake contrast for software modes because it's how original works.
// Also, turn on camglow dynlights in the same fashion and enable gl dynlights.
bool s_WasFakeContrastForcedByGL1x = false;
bool s_WasCamglowDynlightForcedByGL1x = false;
bool s_WereAllGLDynlightsForcedByGL1x = false;
int s_SavedFakeContrastValForcedByGL1x = 1;
int s_SavedCamglowDynlightValForcedByGL1x = 0;
int s_SavedAllGLDynlightsValForcedByGL1x = 1;
CUSTOM_CVAR(Int, gl_lightmode, 8, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	int newself = self;
	if (newself > 8) newself = 16; // use 8 and 16 for software lighting to avoid conflicts with the bit mask
	else if (newself > 4) newself = 8;
	else if (newself < 0) newself = 0;
	if (self != newself) self = newself;
	glset.lightmode = newself;

	// Handle runtime toggle optimizations for legacy fixed-pipeline paths
	if (gl.legacyMode)
	{
		if (newself == 2 || newself == 8 || newself == 16)
		{
			// If moving into software modes, memorize the original setting once
			if (!s_WasFakeContrastForcedByGL1x)
			{
				s_SavedFakeContrastValForcedByGL1x = r_fakecontrast;

				// Force it ON if it was disabled by user previously
				if (r_fakecontrast == 0)
				{
					r_fakecontrast = 1;
				}
				s_WasFakeContrastForcedByGL1x = true;
			}

			// Memorize and force gl_camglowlight ON for doom/software/vanilla modes
			if (!s_WasCamglowDynlightForcedByGL1x)
			{
				s_SavedCamglowDynlightValForcedByGL1x = gl_camglowlight;

				if (gl_camglowlight == 0)
				{
					gl_camglowlight = 1;
				}
				s_WasCamglowDynlightForcedByGL1x = true;
			}

			// Safe fallback: Memorize and force gl_lights ON so camglow script can actually render
			if (!s_WereAllGLDynlightsForcedByGL1x)
			{
				s_SavedAllGLDynlightsValForcedByGL1x = gl_lights;

				if (gl_lights == 0)
				{
					gl_lights = 1;
				}
				s_WereAllGLDynlightsForcedByGL1x = true;
			}
		}
		else
		{
			// Returning back to normal lighting modes (0-4, except 2), roll back to their actual preference
			if (s_WasFakeContrastForcedByGL1x)
			{
				r_fakecontrast = s_SavedFakeContrastValForcedByGL1x;
				s_WasFakeContrastForcedByGL1x = false;
			}

			// Restore user's previous preference for camglow light
			if (s_WasCamglowDynlightForcedByGL1x)
			{
				gl_camglowlight = s_SavedCamglowDynlightValForcedByGL1x;
				s_WasCamglowDynlightForcedByGL1x = false;
			}

			// Restore user's previous preference for global dynamic lights system
			if (s_WereAllGLDynlightsForcedByGL1x)
			{
				gl_lights = s_SavedAllGLDynlightsValForcedByGL1x;
				s_WereAllGLDynlightsForcedByGL1x = false;
			}
		}
	}
}

//==========================================================================
//
// Sets render state to draw the given render style
// includes: Texture mode, blend equation and blend mode
//
//==========================================================================

void gl_GetRenderStyle(FRenderStyle style, bool drawopaque, bool allowcolorblending,
					   int *tm, int *sb, int *db, int *be)
{
	static int blendstyles[] = { GL_ZERO, GL_ONE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
	static int renderops[] = { 0, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, -1, -1, -1, -1, 
		-1, -1, -1, -1, -1, -1, -1, -1 };

	int srcblend = blendstyles[style.SrcAlpha&3];
	int dstblend = blendstyles[style.DestAlpha&3];
	int blendequation = renderops[style.BlendOp&15];
	int texturemode = drawopaque? TM_OPAQUE : TM_MODULATE;

	if (style.Flags & STYLEF_RedIsAlpha)
	{
		texturemode = TM_REDTOALPHA;
	}
	else if (style.Flags & STYLEF_ColorIsFixed)
	{
		texturemode = TM_MASK;
	}
	else if (style.Flags & STYLEF_InvertSource)
	{
		// The only place where InvertSource is used is for inverted sprites with the infrared powerup.
		texturemode = TM_INVERSE;
	}

	if (blendequation == -1)
	{
		srcblend = GL_DST_COLOR;
		dstblend = GL_ONE_MINUS_SRC_ALPHA;
		blendequation = GL_FUNC_ADD;
	}

	if (allowcolorblending && srcblend == GL_SRC_ALPHA && dstblend == GL_ONE && blendequation == GL_FUNC_ADD)
	{
		srcblend = GL_SRC_COLOR;
	}

	// Transluscency code intercept for GL1.1 (NOT for original GL1.3+ path)
	if (gl.gl1path && gl.gl1_v1dot1)
	{
		// Hard fallback for blend equation to prevent crashes on unsupported ops
		blendequation = GL_FUNC_ADD;

		// Target transluscent styles (particles, explosions, fire balls, transluscent actors)
		if (style.SrcAlpha == STYLEALPHA_Src && style.DestAlpha == STYLEALPHA_InvSrc)
		{
			// Flag this texture mode with a special index (99) to execute alpha clamping down the pipe
			texturemode = 99; 
		}
	}

	*tm = texturemode;
	*be = blendequation;
	*sb = srcblend;
	*db = dstblend;
}


//==========================================================================
//
// Get current light level
//
//==========================================================================

int gl_CalcLightLevel(int lightlevel, int rellight, bool weapon, int blendfactor)
{
	int light;

	if (lightlevel <= 0) return 0;

	// Check for our custom legacy modes
	bool isLegacySoftware8 = (gl.legacyMode && glset.lightmode == 8);
	bool isLegacyVanilla16 = (gl.legacyMode && glset.lightmode == 16);

	// Original software behavior activates if lightmode >= 8 and blendfactor > 0.
	// For legacy mode, we force dark lighting logic for mode 8 and 16 unconditionally.
	bool darklightmode = (glset.lightmode & 2) ||
		isLegacySoftware8 || isLegacyVanilla16 ||
		(!gl.legacyMode && glset.lightmode >= 8 && blendfactor > 0);

	if (darklightmode && lightlevel < 192 && !weapon)
	{
		if (lightlevel > 100)
		{
			if (isLegacyVanilla16)
			{
				// Mode 16 drops brightness much faster (2.15f instead of 1.87f) for a darker, high-contrast look
				light = xs_CRoundToInt(192.f - (192 - lightlevel) * 2.15f);
			}
			else
			{
				// Standard Software (Mode 8) formula
				light = xs_CRoundToInt(192.f - (192 - lightlevel) * 1.87f);
			}

			if (light + rellight < 20)
			{
				// Mode 16 cuts off into deep black faster at the low-end
				int divisor = isLegacyVanilla16 ? 7 : 5;
				light = 20 + (light + rellight - 20) / divisor;
			}
			else
				light += rellight;
		}
		else
		{
			// Deep shadow handling: Mode 16 divides by 7 instead of 5, making dark sectors pitch black
			int divisor = isLegacyVanilla16 ? 7 : 5;
			light = (lightlevel + rellight) / divisor;
		}
	}
	else
	{
		light = lightlevel + rellight;
	}

	return clamp(light, 0, 255);
}

//==========================================================================
//
// Get current light color
//
//==========================================================================

static PalEntry gl_CalcLightColor(int light, PalEntry pe, int blendfactor)
{
	int r, g, b;

	if (blendfactor == 0)
	{
		// Legacy software modes (8 and 16) fall through to apply sector lighting down to the textures
		if (glset.lightmode >= 8 && !(gl.legacyMode && (glset.lightmode == 8 || glset.lightmode == 16)))
		{
			return pe;
		}
		else
		{
			r = pe.r * light / 255;
			g = pe.g * light / 255;
			b = pe.b * light / 255;
		}
	}
	else
	{
		// This is what Legacy does with colored light in 3D volumes. No, it doesn't really make sense....
		// It also doesn't translate well to software style lighting.
		int mixlight = light * (255 - blendfactor);

		r = (mixlight + pe.r * blendfactor) / 255;
		g = (mixlight + pe.g * blendfactor) / 255;
		b = (mixlight + pe.b * blendfactor) / 255;
	}
	return PalEntry(255, uint8_t(r), uint8_t(g), uint8_t(b));
}

//==========================================================================
//
// set current light color
//
//==========================================================================
void gl_SetColor(int sectorlightlevel, int rellight, bool fullbright, const FColormap &cm, float alpha, bool weapon)
{ 
	if (fullbright)
	{
		gl_RenderState.SetColorAlpha(0xffffff, alpha, 0);
		gl_RenderState.SetSoftLightLevel(255);
	}
	else
	{
		int hwlightlevel = gl_CalcLightLevel(sectorlightlevel, rellight, weapon, cm.BlendFactor);
		PalEntry pe = gl_CalcLightColor(hwlightlevel, cm.LightColor, cm.BlendFactor);
		gl_RenderState.SetColorAlpha(pe, alpha, cm.Desaturation);
		gl_RenderState.SetSoftLightLevel(gl_ClampLight(sectorlightlevel + rellight), cm.BlendFactor);
	}
}

//==========================================================================
//
// calculates the current fog density
//
//	Rules for fog:
//
//  1. If bit 4 of gl_lightmode is set always use the level's fog density. 
//     This is what Legacy's GL render does.
//	2. black fog means no fog and always uses the distfogtable based on the level's fog density setting
//	3. If outside fog is defined and the current fog color is the same as the outside fog
//	   the engine always uses the outside fog density to make the fog uniform across the level.
//	   If the outside fog's density is undefined it uses the level's fog density and if that is
//	   not defined it uses a default of 70.
//	4. If a global fog density is specified it is being used for all fog on the level
//	5. If none of the above apply fog density is based on the light level as for the software renderer.
//  6. For GL1x/GL2x legacy renderers, software light modes (including Vanilla) are based on Dark.
//     Then, they have less fog in the very good lit areas (lightlevel + fake contrast rellight).
//     For both of them, force r_fakecontrast to 1 (ON) if it's on 0(OFF) but let it be turned off later
//     in "CUSTOM_CVAR(Int, gl_lightmode, 8, CVAR_ARCHIVE | CVAR_NOINITCALL)"
//     Camglow comes as a separate zscript flashlight based dynlight mod. Read gl_20.cpp for details.
//
//==========================================================================

float gl_GetFogDensity(int lightlevel, int rellight, PalEntry fogcolor, int sectorfogdensity, int blendfactor)
{
	float density;

	int lightmode = glset.lightmode;

	// Check if this is our custom fake legacy software mode (supporting both 8 and 16)
	bool isLegacySoftware = (gl.legacyMode && (lightmode == 8 || lightmode == 16));

	if (isLegacySoftware) lightmode = 3;

	if (lightmode >= 8 && blendfactor > 0) lightmode = 2;	// The blendfactor feature does not work with software-style lighting.

	if (lightmode & 4)
	{
		// uses approximations of Legacy's default settings.
		density = level.fogdensity ? level.fogdensity : 18;
	}
	else if (sectorfogdensity != 0)
	{
		// case 1: Sector has an explicit fog density set.
		density = sectorfogdensity;
	}
	else if ((fogcolor.d & 0xffffff) == 0)
	{
		// case 2: black fog
		if ((lightmode < 8 || blendfactor > 0) && !(level.flags3 & LEVEL3_NOLIGHTFADE))
		{
			density = distfogtable[glset.lightmode != 0][gl_ClampLight(lightlevel)];

			// Dynamic fog thinning schedule for legacy software mode (accounting for fake contrast)
			if (isLegacySoftware)
			{
				int visual_lightlevel = lightlevel + rellight;
				float modifier = 1.0f; // Default to 100% original Dark mode density

				if (visual_lightlevel >= 240)   modifier = 0.30f; // Maximum visibility in full light
				else if (visual_lightlevel >= 224)   modifier = 0.32f;
				else if (visual_lightlevel >= 208)   modifier = 0.35f;
				else if (visual_lightlevel >= 192)   modifier = 0.40f;
				else if (visual_lightlevel >= 176)   modifier = 0.45f;
				else if (visual_lightlevel >= 160)   modifier = 0.50f;
				else if (visual_lightlevel >= 144)   modifier = 0.60f;
				else if (visual_lightlevel >= 136)   modifier = 0.65f;
				else if (visual_lightlevel >= 128)   modifier = 0.70f;
				else if (visual_lightlevel >= 112)   modifier = 0.80f;

				density = density * modifier;
			}
		}
		else
		{
			density = 0;
		}
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
		// case 3. outsidefogdensity has already been set as needed
		density = level.outsidefogdensity;
	}
	else  if (level.fogdensity != 0)
	{
		// case 4: level has fog density set
		density = level.fogdensity;
	}
	else if (lightlevel < 248)
	{
		// case 5: use light level
		density = clamp<int>(255 - lightlevel, 30, 255);

		// Fallback for colored/custom sector fogs if case 2 is bypassed
		if (isLegacySoftware)
		{
			int visual_lightlevel = lightlevel + rellight;
			float modifier = 1.0f;

			if (visual_lightlevel >= 240)   modifier = 0.30f;
			else if (visual_lightlevel >= 224)   modifier = 0.32f;
			else if (visual_lightlevel >= 208)   modifier = 0.35f;
			else if (visual_lightlevel >= 192)   modifier = 0.40f;
			else if (visual_lightlevel >= 176)   modifier = 0.45f;
			else if (visual_lightlevel >= 160)   modifier = 0.50f;
			else if (visual_lightlevel >= 144)   modifier = 0.60f;
			else if (visual_lightlevel >= 136)   modifier = 0.65f;
			else if (visual_lightlevel >= 128)   modifier = 0.70f;
			else if (visual_lightlevel >= 112)   modifier = 0.80f;

			density = density * modifier;
		}
	}
	else
	{
		density = 0.f;
	}
	return density;
}

//==========================================================================
//
// Check if the current linedef is a candidate for a fog boundary
//
// Requirements for a fog boundary:
// - front sector has no fog
// - back sector has fog
// - at least one of both does not have a sky ceiling.
//
//==========================================================================

bool gl_CheckFog(sector_t *frontsector, sector_t *backsector)
{
	if (frontsector == backsector) return false;	// there can't be a boundary if both sides are in the same sector.

	// Check for fog boundaries. This needs a few more checks for the sectors

	PalEntry fogcolor = frontsector->Colormap.FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
		return false;
	}
	else if (fogcolor.a != 0)
	{
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
	}
	else  if (level.fogdensity!=0 || (glset.lightmode & 4))
	{
		// case 3: level has fog density set
	}
	else 
	{
		// case 4: use light level
		if (frontsector->lightlevel >= 248) return false;
	}

	fogcolor = backsector->Colormap.FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
		return false;
	}
	else  if (level.fogdensity!=0 || (glset.lightmode & 4))
	{
		// case 3: level has fog density set
		return false;
	}
	else 
	{
		// case 4: use light level
		if (backsector->lightlevel < 248) return false;
	}

	// in all other cases this might create more problems than it solves.
	return ((frontsector->GetTexture(sector_t::ceiling)!=skyflatnum || 
			 backsector->GetTexture(sector_t::ceiling)!=skyflatnum));
}

//==========================================================================
//
// Lighting stuff 
//
//==========================================================================

void gl_SetShaderLight(float level, float olight)
{
	const float MAXDIST = 256.f;
	const float THRESHOLD = 96.f;
	const float FACTOR = 0.75f;

	if (level > 0)
	{
		float lightdist, lightfactor;
			
		if (olight < THRESHOLD)
		{
			lightdist = (MAXDIST/2) + (olight * MAXDIST / THRESHOLD / 2);
			olight = THRESHOLD;
		}
		else lightdist = MAXDIST;

		lightfactor = 1.f + ((olight/level) - 1.f) * FACTOR;
		if (lightfactor == 1.f) lightdist = 0.f;	// save some code in the shader
		gl_RenderState.SetLightParms(lightfactor, lightdist);
	}
	else
	{
		gl_RenderState.SetLightParms(1.f, 0.f);
	}
}


//==========================================================================
//
// Sets the fog for the current polygon
//
//==========================================================================

void gl_SetFog(int lightlevel, int rellight, bool fullbright, const FColormap *cmap, bool isadditive)
{
	PalEntry fogcolor;
	float fogdensity;

	if (level.flags&LEVEL_HASFADETABLE)
	{
		fogdensity = 70;
		fogcolor = 0x808080;
	}
	else if (cmap != NULL && !fullbright)
	{
		fogcolor = cmap->FadeColor;
		// rellight is now the 2nd argument
		fogdensity = gl_GetFogDensity(lightlevel, rellight, fogcolor, cmap->FogDensity, cmap->BlendFactor);
		fogcolor.a = 0;
	}
	else
	{
		fogcolor = 0;
		fogdensity = 0;
	}

	// Make fog a little denser when inside a skybox
	if (GLPortal::inskybox) fogdensity += fogdensity / 2;


	// no fog in enhanced vision modes!
	if (fogdensity == 0 || gl_fogmode == 0)
	{
		gl_RenderState.EnableFog(false);
		gl_RenderState.SetFog(0, 0);
	}
	else
	{
		int check_lightmode = glset.lightmode;
		if (gl.legacyMode && (check_lightmode == 8 || check_lightmode == 16)) check_lightmode = 3;

		if (cmap && (check_lightmode == 2 || (check_lightmode >= 8 && cmap->BlendFactor > 0)) && fogcolor == 0)
		{
			float light = gl_CalcLightLevel(lightlevel, rellight, false, cmap->BlendFactor);
			gl_SetShaderLight(light, lightlevel);
		}
		else
		{
			gl_RenderState.SetLightParms(1.f, 0.f);
		}

		// For additive rendering using the regular fog color here would mean applying it twice
		// so always use black
		if (isadditive)
		{
			fogcolor = 0;
		}
		gl_RenderState.EnableFog(true);
		gl_RenderState.SetFog(fogcolor, fogdensity);

		// Korshun: fullbright fog like in software renderer.
		if (glset.lightmode >= 8 && cmap && cmap->BlendFactor == 0 && glset.brightfog && fogdensity != 0 && fogcolor != 0)
		{
			gl_RenderState.SetSoftLightLevel(255);
		}
	}
}
