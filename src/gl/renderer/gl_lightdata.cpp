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
** gl_lightdata.cpp
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

extern GLWall* g_isCurrentlyGLWallDrawing;
extern bool isUsingVolumetric3DModelLegacyDynlight;


//==========================================================================
//
// Sets up the fog tables
//
//==========================================================================

static int s_LastComputedGLDistFog = -1;
CUSTOM_CVAR(Int, gl_distfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (s_LastComputedGLDistFog == gl_distfog) return; // skip recalculation if value not changed
	s_LastComputedGLDistFog = gl_distfog;

	const int val = gl_distfog;
	const int halfVal = val >> 1;

	for (int i = 0; i < 256; i++)
	{
		if (i < 164)
		{
			distfogtable[0][i] = halfVal + val * (164 - i) / 164;
		}
		else if (i < 230)
		{
			distfogtable[0][i] = halfVal - halfVal * (i - 164) / (230 - 164);
		}
		else distfogtable[0][i] = 0;

		if (i < 128)
		{
			distfogtable[1][i] = 6.f + halfVal + val * (128 - i) / 48;
		}
		else if (i < 216)
		{
			distfogtable[1][i] = (216.f - i) / (216.f - 128.f) * val / 10;
		}
		else distfogtable[1][i] = 0;
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

// We need to turn on fake contrast for software modes because it's how original works.
// Also, turn on camglow dynlights in the same fashion and enable gl dynlights in the menu.
bool s_WasFakeContrastForcedByGL1x = false;
bool s_WasCamglowDynlightForcedByGL1x = false;
bool s_WasCamglowDynlightForcedByShaderGL3x = false;
bool s_WereAllGLDynlightsForcedByGL1x = false;
int s_SavedFakeContrastValForcedByGL1x = 1;
int s_SavedCamglowDynlightValForcedByGL1x = 0;
int s_SavedCamglowDynlightValForcedByShaderGL3x = 0;
int s_SavedAllGLDynlightsValForcedByGL1x = 1;
CUSTOM_CVAR(Int, gl_lightmode, 8, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	int newself = self;
	if (newself > 8) newself = 16; // use 8 and 16 for software lighting to avoid conflicts with the bit mask
	else if (newself > 4) newself = 8;
	else if (newself < 0) newself = 0;
	if (self != newself) self = newself;
	glset.lightmode = newself;

	// ---=== PATHWAY A: LEGACY FIXED-PIPELINE PATH ===---
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

			// Memorize and force gl_camglowlight ON for software/vanilla/doom modes
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
			// Returning back to normal lighting modes (0, 1, 3, 4), roll back to their actual preference
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
	// ---=== PATHWAY B: MODERN SHADER PATH (GL3x+) ===---
	else
	{
		if (newself == 2 || newself == 8 || newself == 16)
		{
			// If moving into shader software modes, memorize preference and force camglow OFF
			if (!s_WasCamglowDynlightForcedByShaderGL3x)
			{
				s_SavedCamglowDynlightValForcedByShaderGL3x = gl_camglowlight;

				if (gl_camglowlight != 0)
				{
					gl_camglowlight = 0;
				}
				s_WasCamglowDynlightForcedByShaderGL3x = true;
			}
		}
		else
		{
			// Returning back to regular lighting modes, restore their native choice
			if (s_WasCamglowDynlightForcedByShaderGL3x)
			{
				gl_camglowlight = s_SavedCamglowDynlightValForcedByShaderGL3x;
				s_WasCamglowDynlightForcedByShaderGL3x = false;
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

// Gets called everywhere in the code
int gl_CalcLightLevelWrapper(int lightlevel, int rellight, bool weapon, int blendfactor)
{
	if (gl.legacyMode && (gl_lightmode == 8 || gl_lightmode == 16) )
	{
		return gl_CalcLightLevelGL1xSoftwareModes(lightlevel, rellight, weapon, blendfactor);
	}
	else
	{
		return gl_CalcLightLevelStandard(lightlevel, rellight, weapon, blendfactor);
	}
}

int gl_CalcLightLevelStandard(int lightlevel, int rellight, bool weapon, int blendfactor)
{
	int light;

	if (lightlevel <= 0) return 0;

	bool darklightmode = (glset.lightmode & 2) || (glset.lightmode >= 8 && blendfactor > 0);

	if (darklightmode && lightlevel < 192 && !weapon)
	{
		if (lightlevel > 100)
		{
			light = xs_CRoundToInt(192.f - (192 - lightlevel)* 1.87f);
			if (light + rellight < 20)
			{
				light = 20 + (light + rellight - 20) / 5;
			}
			else
			{
				light += rellight;
			}
		}
		else
		{
			light = (lightlevel + rellight) / 5;
		}

	}
	else
	{
		light = lightlevel + rellight;
	}

	return clamp(light, 0, 255);
}

int gl_CalcLightLevelGL1xSoftwareModes(int lightlevel, int rellight, bool weapon, int blendfactor)
{
	if (lightlevel <= 0) return 0;

	int light;
	int scaled_rellight = rellight;
	int divisor = gl_lightmode == 16 ? 7 : 5; // div by 7 for for Vanilla mode for darker experience

	bool isDrawingGL1xWall = false;
	if (g_isCurrentlyGLWallDrawing != nullptr) isDrawingGL1xWall = true;

	int gl1xWallTame = isDrawingGL1xWall ? 128 : 64; // subtract walls more, thus flats are brighter
	float gl1xSoftwareSectLightMul = gl_lightmode == 8 ? 1.87f : 2.15f; // div by 2.15 makes it darker

	float contrastModifier = 1.0f;
	const float invMul32 = 1.0f / 32.0f;

	if (lightlevel < 192 && !weapon)
	{
		if (lightlevel > 100)
		{
			// ---=== THE CONTINUOUS FLOATING ANCHOR SYSTEM ===---
			// To eliminate the sharp staircasing pops caused by rigid if-else blocks,
			// transition the mathematical anchor from 192.f up to 224.f continuously.
			// Normalized 't' factor links directly to the current sector brightness scale.
			float t = ((float)lightlevel - 100.0f) / (192.0f - gl1xWallTame);
			//if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;

			// Seamlessly slides the anchor point from 192.f up to 224.f based on zones setup
			float floatingAnchor = 192.0f + (224.0f - 192.0f) * t;

			// Single, continuous non-linear calculation sweep avoids single-frame register jumps
			light = xs_CRoundToInt
			(floatingAnchor - (floatingAnchor - lightlevel) * gl1xSoftwareSectLightMul);

			if (light + rellight < 20)
			{
				light = 20 + (light + rellight - 20) / divisor;
			}
			else
			{
				// ---=== THE DIFFERENTIAL FAKE CONTRAST MULTI-STEP MATRIX ===---
				//if      (rellight >= 16)  contrastModifier = 1.025f; // Slight brightness boost for very bright fake contrast wall
				//else if (rellight >= 8)   contrastModifier = 1.05f;  // Higher brightness boost for bright fake contrast wall
				//else if (rellight == 0)   contrastModifier = 1.000f; // Flats or Neutral walls stay clean
				//else if (rellight >= -8)  contrastModifier = 0.850f; // Brighten up slightly darker fake contrast walls
				//else if (rellight >= -16) contrastModifier = 0.800f; // Brighten up darker fake contrast walls
				//else                      contrastModifier = 0.750f; // Brighten up very dark fake contrast walls			
				//scaled_rellight = (int)(rellight * contrastModifier);
				//light += scaled_rellight;

				scaled_rellight = (int)(rellight * contrastModifier);
				light += scaled_rellight;

				// ---=== SOLID MID-TONE FLATS CORRECTION (SMOOTH CUP CURVE) ===---
				// Gently dim the overbright anomaly on floors and ceilings around the mid-range (112 to 176).
				// This pulls down exactly the 128 mark by a smooth -10 units without causing sharp staircases.
				if (lightlevel >= 112 && lightlevel <= 176)
				{
					// Smooth parabola that hits 1.0f at 128, and fades out cleanly at 112 and 176
					float midFactor = 1.0f - (fabsf(lightlevel - 128.0f) * invMul32);
					if (midFactor > 0.0f)
					{
						light = light - (int)(10 * midFactor);
					}
				}
			}
		}
		else
		{
			//if      (rellight >= 16)  contrastModifier = 1.025f; // Slight brightness boost for very bright fake contrast wall
			//else if (rellight >= 8)   contrastModifier = 1.05f;  // Higher brightness boost for bright fake contrast wall
			//else if (rellight == 0)   contrastModifier = 1.000f; // Flats or Neutral walls stay clean
			//else if (rellight >= -8)  contrastModifier = 0.850f; // Brighten up slightly darker fake contrast walls
			//else if (rellight >= -16) contrastModifier = 0.800f; // Brighten up darker fake contrast walls
			//else                      contrastModifier = 0.750f; // Brighten up very dark fake contrast walls
		
			scaled_rellight = (int)(rellight * contrastModifier);
			light = (lightlevel + scaled_rellight) / divisor;
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
		int hwlightlevel = gl_CalcLightLevelWrapper(sectorlightlevel, rellight, weapon, cm.BlendFactor);
		PalEntry pe = gl_CalcLightColor(hwlightlevel, cm.LightColor, cm.BlendFactor);
		gl_RenderState.SetColorAlpha(pe, alpha, cm.Desaturation);
		gl_RenderState.SetSoftLightLevel(gl_ClampLight(sectorlightlevel + rellight), cm.BlendFactor);
	}
}

//==========================================================================
//
// Calculate the current fog density
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

float gl_GetFogDensityWrapper(int lightlevel, int rellight, PalEntry fogcolor, int sectorfogdensity, int blendfactor)
{
	const bool isGL1xSoftwareSectorLightmode = gl.legacyMode && (gl_lightmode == 8 || gl_lightmode == 16);

	if (isGL1xSoftwareSectorLightmode)
	{
		return gl_GetFogDensityGL1x(lightlevel, rellight, fogcolor, sectorfogdensity, blendfactor);
	}
	else
	{
		return gl_GetFogDensityStandard(lightlevel, rellight, fogcolor, sectorfogdensity, blendfactor);
	}
}

float gl_GetFogDensityStandard(int lightlevel, int rellight, PalEntry fogcolor, int sectorfogdensity, int blendfactor)
{
	float density;

	int lightmode = glset.lightmode;
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
	}
	else
	{
		density = 0.f;
	}
	return density;
}


// Precomputed modifier LUTs for software/vanilla fog density (case 2 & case 5)
// Eliminates 10-branch else-if ladders per wall/flat/sprite draw call.
struct FSoftwareGL1xFogModifierLUT
{
	float case2[256]; float case5[256];
	FSoftwareGL1xFogModifierLUT()
	{
		for (int ll = 0; ll < 256; ++ll)
		{
			// Case 2 (black fog / diminished lighting) modifier
			float               mod2 = 1.0f;
			if      (ll >= 240) mod2 = 0.30f;
			else if (ll >= 224) mod2 = 0.35f;
			else if (ll >= 208) mod2 = 0.40f;
			else if (ll >= 192) mod2 = 0.45f;
			else if (ll >= 176) mod2 = 0.50f;
			else if (ll >= 160) mod2 = 0.65f;
			else if (ll >= 144) mod2 = 0.80f;
			else if (ll >= 136) mod2 = 0.90f;
			else if (ll >= 128) mod2 = 0.95f;
			case2[ll] = mod2;

			// Case 5 (colored fog based on light level) modifier
			float               mod5 = 1.0f;
			if      (ll >= 240) mod5 = 0.30f;
			else if (ll >= 224) mod5 = 0.35f;
			else if (ll >= 208) mod5 = 0.40f;
			else if (ll >= 192) mod5 = 0.45f;
			else if (ll >= 176) mod5 = 0.50f;
			else if (ll >= 160) mod5 = 0.65f;
			else if (ll >= 144) mod5 = 0.80f;
			else if (ll >= 136) mod5 = 0.70f;
			else if (ll >= 128) mod5 = 0.70f;
			case5[ll] = mod5;
		}
	}
};
static const FSoftwareGL1xFogModifierLUT g_SoftwareFogModLUT;

float gl_GetFogDensityGL1x(int lightlevel, int rellight, PalEntry fogcolor, int sectorfogdensity, int blendfactor)
{
	float density;

	int lightmode = 3; // Custom fake legacy software mode (8 or 16) maps to mode 3
	if (blendfactor > 0) lightmode = 2; // The blendfactor feature does not work with software-style lighting.

	if (sectorfogdensity != 0)
	{
		// case 1: Sector has an explicit fog density set.
		density = sectorfogdensity;
	}
	else if ((fogcolor.d & 0xffffff) == 0)
	{
		// case 2: black fog
		if (!(level.flags3 & LEVEL3_NOLIGHTFADE))
		{
			// Calculate the true visual lighting environment
			const int visual_lightlevel = lightlevel + rellight;

			// Feed the light directly into our custom table
			density = distfogtable[glset.lightmode != 0][gl_ClampLight(lightlevel)];

			// The step-wise modifier curve operates on the full visual footprint to keep near contrast intact
			float modifier = g_SoftwareFogModLUT.case2[clamp<int>(visual_lightlevel, 0, 255)];

			// decrease fog for bright fake contrast walls in the big distance
			if (rellight > 0) modifier *= 0.25f;

			density *= modifier;
		}
		else
		{
			density = 0.f;
		}
		density *= 0.32f; // decrease overall dimlight black fog density for GL1x software/vanilla modes
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && 
					(fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
		density = level.outsidefogdensity;
	}
	else if (level.fogdensity != 0)
	{
		density = level.fogdensity;
	}
	else if (lightlevel < 248)
	{
		// case 5: use light level
		const int visual_lightlevel = lightlevel + rellight;

		density = distfogtable[glset.lightmode != 0][gl_ClampLight(lightlevel)];

		float modifier = g_SoftwareFogModLUT.case5[clamp<int>(visual_lightlevel, 0, 255)];

		// decrease fog for bright fake contrast walls in the big distance
		if (rellight > 0) modifier *= 0.25f;

		density *= modifier;
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
		fogdensity = gl_GetFogDensityWrapper(lightlevel, rellight, fogcolor, cmap->FogDensity, cmap->BlendFactor);
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
		//if (gl.legacyMode && (check_lightmode == 8 || check_lightmode == 16)) check_lightmode = 3;

		if (cmap && (check_lightmode == 2 || (check_lightmode >= 8 && cmap->BlendFactor > 0)) && fogcolor == 0)
		{
			float light = gl_CalcLightLevelWrapper(lightlevel, rellight, false, cmap->BlendFactor);
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
