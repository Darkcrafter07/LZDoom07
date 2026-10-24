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
static float distgl1xsoftwarefogtable[2][256]; // light to fog conversion table for black fog for GL1x/GL2 software sector light modes

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

CUSTOM_CVAR(Int, gl_distfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	for (int i = 0; i < 256; i++)
	{
		if (i < 164)
		{
			distfogtable[0][i] = (gl_distfog >> 1) + (gl_distfog)*(164 - i) / 164;
		}
		else if (i < 230)
		{
			distfogtable[0][i] = (gl_distfog >> 1) - (gl_distfog >> 1)*(i - 164) / (230 - 164);
		}
		else distfogtable[0][i] = 0;

		if (i < 128)
		{
			distfogtable[1][i] = 6.f + (gl_distfog >> 1) + (gl_distfog)*(128 - i) / 48;
		}
		else if (i < 216)
		{
			distfogtable[1][i] = (216.f - i) / ((216.f - 128.f)) * gl_distfog / 10;
		}
		else distfogtable[1][i] = 0;
	}
}

// There's a need to have a separate fogtable for software/vanilla modes
CUSTOM_CVAR(Int, gl_distsoftwaregl1xfog, 70, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	const bool useHQtablegen = true;
	const float invMul255 = 1.0f / 255.0f;

	if (useHQtablegen)
	{
		for (int i = 0; i < 256; i++)
		{
			// Normalize index to a 0.0f - 1.0f scale
			float f = (float)i * invMul255;

			// --- SOFTWARE SECTOR LIGHT MODE #8 (ORIGINAL EXE) ---
			// We keep the heavy 1.40f multiplier to lock down rich software shadows in the dark,
			// but we modify the decay power dynamically.
			float baseDensity0 = (float)gl_distsoftwaregl1xfog * 1.40f;

			// Added an aggressive power-scaling shift (-4.10f * f - 1.65f * (f * f)).
			// In dark-to-mid zones (f < 0.4), the quadratic component is near zero, keeping the shadows heavy.
			// In bright sectors (160 to 176, f > 0.6), the (f * f) term kicks in like a booster, 
			// slamming the fog density down to absolute zero way faster. This fully stops the geometry 
			// from prematurely drowning in fog at long distances, matching the clear look of the original Dark.
			distgl1xsoftwarefogtable[0][i] = baseDensity0 * expf(-4.10f * f - 1.65f * (f * f));

			// Hard safety cushion gate right at the edge of fullbright to prevent register overflow
			if (i >= 235) distgl1xsoftwarefogtable[0][i] = 0.0f;


			// --- VANILLA SECTOR LIGHT MODE #16 ---
			// So far, the formulas may be closer to exe but it looks darker and wrong but let's have it too.
			// Mapped onto the same high-velocity clearing curve for bright layers
			float baseDensity1 = 6.0f + (float)gl_distsoftwaregl1xfog * 1.65f;
			distgl1xsoftwarefogtable[1][i] = baseDensity1 * expf(-4.60f * f - 1.95f * (f * f));

			if (i >= 220) distgl1xsoftwarefogtable[1][i] = 0.0f;
		}
	}
	else
	{
		// ---=== PIECEWISE APPROXIMATION OF OUR HQ EXP MATRIX ===---
		int baseVal = gl_distsoftwaregl1xfog;

		// PATCH: Restored heavy base multipliers back to match 1.40f and 1.65f HQ anchors.
		// This brings back pristine, pitch-black software shadow definition in dark areas!
		int sftDarkAnchor = (int)(baseVal * 1.40f);
		int vanDarkAnchor = 6 + (int)(baseVal * 1.65f);

		for (int i = 0; i < 256; i++)
		{
			// 1. --- SOFTWARE SECTOR LIGHT MODE #8 (HQ-ALIGNED PIECEWISE) ---
			if (i < 32)
			{
				distgl1xsoftwarefogtable[0][i] = sftDarkAnchor - (sftDarkAnchor * 0.15f) * i / 32;
			}
			else if (i < 64)
			{
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.85f) - (sftDarkAnchor * 0.23f) * (i - 32) / 32;
			}
			else if (i < 96)
			{
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.62f) - (sftDarkAnchor * 0.25f) * (i - 64) / 32;
			}
			else if (i < 128)
			{
				// Zone 3: Central Mid-Tones. The slope begins to take a steep dive here.
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.37f) - (sftDarkAnchor * 0.20f) * (i - 96) / 32;
			}
			else if (i < 164)
			{
				// Zone 4: Upper Mid-Tones. Accelerated drop-off completely mimics the (f * f) booster!
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.17f) - (sftDarkAnchor * 0.11f) * (i - 128) / 36;
			}
			else if (i < 196)
			{
				// Zone 5: Bright Room Baseline. Dropped sharply near zero to keep distant walls clear.
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.06f) - (sftDarkAnchor * 0.04f) * (i - 164) / 32;
			}
			else if (i < 235)
			{
				distgl1xsoftwarefogtable[0][i] = (int)(sftDarkAnchor * 0.02f) - (sftDarkAnchor * 0.02f) * (i - 196) / (235 - 196);
			}
			else
			{
				distgl1xsoftwarefogtable[0][i] = 0.0f;
			}

			// 2. --- VANILLA SECTOR LIGHT MODE #16 (HQ-ALIGNED PIECEWISE) ---
			if (i < 32)
			{
				distgl1xsoftwarefogtable[1][i] = vanDarkAnchor - (vanDarkAnchor * 0.22f) * i / 32;
			}
			else if (i < 64)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.78f) - (vanDarkAnchor * 0.26f) * (i - 32) / 32;
			}
			else if (i < 96)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.52f) - (vanDarkAnchor * 0.24f) * (i - 64) / 32;
			}
			else if (i < 128)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.28f) - (vanDarkAnchor * 0.16f) * (i - 96) / 32;
			}
			else if (i < 160)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.12f) - (vanDarkAnchor * 0.09f) * (i - 128) / 32;
			}
			else if (i < 192)
			{
				distgl1xsoftwarefogtable[1][i] = (int)(vanDarkAnchor * 0.03f) - (vanDarkAnchor * 0.03f) * (i - 160) / 32;
			}
			else if (i < 220)
			{
				distgl1xsoftwarefogtable[1][i] = 0.0f;
			}
			else
			{
				distgl1xsoftwarefogtable[1][i] = 0.0f;
			}
		}
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

int gl_CalcLightLevel(int lightlevel, int rellight, bool weapon, int blendfactor)
{
	int light;

	if (lightlevel <= 0) return 0;

	// Check for our custom legacy modes
	bool isGL1xSoftware8 = (gl.legacyMode && glset.lightmode == 8);
	bool isGL1xVanilla16 = (gl.legacyMode && glset.lightmode == 16);

	// Original software behavior activates if lightmode >= 8 and blendfactor > 0.
	// For legacy mode, we force dark lighting logic for mode 8 and 16 unconditionally.
	bool isGL1xFakeSoftwareModes = (glset.lightmode & 2) || isGL1xSoftware8 || isGL1xVanilla16 ||
		(!gl.legacyMode && glset.lightmode >= 8 && blendfactor > 0);

	if (isGL1xFakeSoftwareModes && lightlevel < 192 && !weapon)
	{
		if (lightlevel > 100)
		{
			if (isGL1xVanilla16)
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
				int divisor = isGL1xVanilla16 ? 7 : 5;
				light = 20 + (light + rellight - 20) / divisor;
			}
			else
			{
				// Soften the raw fake contrast offset by a safe scaling scalar (0.50f)
				// inside the light level rendering calculations to match fixed-pipeline hardware bounds
				int scaled_rellight = rellight;
				if (isGL1xSoftware8 || isGL1xVanilla16)
				{
					// ---=== THE DIFFERENTIAL FAKE CONTRAST MULTI-STEP MATRIX ===---
					// Instead of a rigid true/false guard, evaluate a tailored step schedule 
					// with an 8-unit stride spanning from -16 to +16 with an extra safety buffer.
					float                     contrastModifier = 1.00f;
					if      (rellight >= 16)  contrastModifier = 1.15f; // Brightness boost for bright fake contrast wall
					else if (rellight >= 8)   contrastModifier = 1.05f; // Slight brightness boost for bright fake contrast wall
					else if (rellight == 0)   contrastModifier = 1.00f; // Flats or Neutral walls stay clean
					//else if (rellight >= -8)  contrastModifier = 0.85f; // Brighten up slightly darker fake contrast walls
					//else if (rellight >= -16) contrastModifier = 0.80f; // Brighten up darker fake contrast walls
					//else                      contrastModifier = 0.75f; // Brighten up very dark fake contrast walls

					scaled_rellight = (int)(rellight * contrastModifier);
				}

				light += scaled_rellight;

				// ---=== SOLID MID-TONE FLATS CORRECTION (SMOOTH CUP CURVE) ===---
				// Gently dim the overbright anomaly on floors and ceilings around the mid-range (112 to 176).
				// This pulls down exactly the 128 mark by a smooth -10 units without causing sharp staircases.
				if (isGL1xSoftware8 || isGL1xVanilla16)
				{
					if (lightlevel >= 112 && lightlevel <= 176)
					{
						// Smooth parabola that hits 1.0f at 128, and fades out cleanly at 112 and 176
						float midFactor = 1.0f - (fabs(lightlevel - 128.0f) / 32.0f);
						if (midFactor > 0.0f)
						{
							light = light - (int)(10 * midFactor);
						}
					}
				}
			}
		}
		else
		{
			// Deep shadow handling: Mode 16 divides by 7 instead of 5, making dark sectors pitch black
			int divisor = isGL1xVanilla16 ? 7 : 5;

			int scaled_rellight = rellight;
			if (isGL1xSoftware8 || isGL1xVanilla16)
			{
				float                     contrastModifier = 1.00f;
				if      (rellight >= 16)  contrastModifier = 1.15f; // Brightness boost for bright fake contrast wall
				else if (rellight >= 8)   contrastModifier = 1.05f; // Slight brightness boost for bright fake contrast wall
				else if (rellight == 0)   contrastModifier = 1.00f; // Flats or neutral walls stay clean
				//else if (rellight >= -8)  contrastModifier = 0.85f; // Brighten up slightly darker fake contrast walls
				//else if (rellight >= -16) contrastModifier = 0.80f; // Brighten up darker fake contrast walls
				//else                      contrastModifier = 0.75f; // Brighten up very dark fake contrast walls

				scaled_rellight = (int)(rellight * contrastModifier);
			}

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
	bool isGL1xSoftwareSectorLightmode = (gl.legacyMode && (lightmode == 8 || lightmode == 16));

	if (isGL1xSoftwareSectorLightmode) lightmode = 3;

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
			// Safe baseline multiplier for our custom profiles
			int scaled_rellight = rellight;
			if (isGL1xSoftwareSectorLightmode)
			{
				scaled_rellight = (int)(rellight * 1.0f); // Pure untouched 1.0f reference contrast
			}

			// Calculate the true visual lighting environment
			int visual_lightlevel = lightlevel + scaled_rellight;

			if (isGL1xSoftwareSectorLightmode)
			{
				// ---=== THE HORIZONTAL NORTH-WALL PIPELINE SHIELD ===---
				// Negative fake contrast on northern/horizontal walls (scaled_rellight < 0)
				// pulls visual_lightlevel down. Under exponential tables, this artificial dimming
				// forces distant horizontal walls to prematurely crush and drown into dark fog.
				// We inject an active clamp here: if the wall is horizontal, we prevent the index 
				// from dropping below the sector's baseline lightlevel, fully curing the distant darkening.
				int lookup_lightlevel = visual_lightlevel;
				if (scaled_rellight < 0)
				{
					lookup_lightlevel = lightlevel; // Protect horizontal indices from diving into thick fog
				}

				int clamped_visual_light = clamp<int>(lookup_lightlevel, 0, 255);

				// Explicitly track the active sub-mode index layer (16 -> layer 1, 8 -> layer 0)
				int retroTableIndex = (glset.lightmode == 16) ? 1 : 0;

				// Feed the protected light directly into our custom exponential table
				density = distgl1xsoftwarefogtable[retroTableIndex][gl_ClampLight(clamped_visual_light)];

				// The step-wise modifier curve operates on the full visual footprint to keep near contrast intact
				float                                modifier = 1.0f;
				if      (visual_lightlevel >= 240)   modifier = 0.30f; 
				else if (visual_lightlevel >= 224)   modifier = 0.35f;
				else if (visual_lightlevel >= 208)   modifier = 0.40f;
				else if (visual_lightlevel >= 192)   modifier = 0.45f;
				else if (visual_lightlevel >= 176)   modifier = 0.50f;
				else if (visual_lightlevel >= 160)   modifier = 0.65f;
				else if (visual_lightlevel >= 144)   modifier = 0.80f;
				else if (visual_lightlevel >= 136)   modifier = 0.90f;
				else if (visual_lightlevel >= 128)   modifier = 0.95f;
				else if (visual_lightlevel >= 112)   modifier = 1.00f;

				density = density * modifier;
			}
			else
			{
				// Standard hardware baseline calculation path for regular lighting modes (fully isolated)
				int clamped_visual_light = clamp<int>(lightlevel, 30, 255);
				density = distfogtable[glset.lightmode != 0][gl_ClampLight(clamped_visual_light)];
			}
		}
		else
		{
			density = 0;
		}
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
		density = level.outsidefogdensity;
	}
	else  if (level.fogdensity != 0)
	{
		density = level.fogdensity;
	}
	else if (lightlevel < 248)
	{
		// case 5: use light level
		int scaled_rellight = rellight;
		if (isGL1xSoftwareSectorLightmode)
		{
			scaled_rellight = (int)(rellight * 1.0f);
		}

		int visual_lightlevel = lightlevel + scaled_rellight;

		if (isGL1xSoftwareSectorLightmode)
		{
			// Apply the exact same horizontal negative shielding matrix to case 5 custom fogs
			int lookup_lightlevel = visual_lightlevel;
			if (scaled_rellight < 0)
			{
				lookup_lightlevel = lightlevel;
			}

			int clamped_visual_light = clamp<int>(lookup_lightlevel, 0, 255);
			density = clamp<int>(255 - clamped_visual_light, 30, 255);

			int retroTableIndex = (glset.lightmode == 16) ? 1 : 0;
			density = distgl1xsoftwarefogtable[retroTableIndex][gl_ClampLight(clamped_visual_light)];

			float                                modifier = 1.0f;
			if      (visual_lightlevel >= 240)   modifier = 0.30f; 
			else if (visual_lightlevel >= 224)   modifier = 0.35f;
			else if (visual_lightlevel >= 208)   modifier = 0.40f;
			else if (visual_lightlevel >= 192)   modifier = 0.45f;
			else if (visual_lightlevel >= 176)   modifier = 0.50f;
			else if (visual_lightlevel >= 160)   modifier = 0.65f;
			else if (visual_lightlevel >= 144)   modifier = 0.80f;
			else if (visual_lightlevel >= 136)   modifier = 0.70f;
			else if (visual_lightlevel >= 128)   modifier = 0.70f;
			else if (visual_lightlevel >= 112)   modifier = 1.00f;

			density = density * modifier;
		}
		else
		{
			density = clamp<int>(255 - lightlevel, 30, 255);
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
