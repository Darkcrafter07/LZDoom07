// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
// Copyright(C) 2026 Vadim Taranov
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
** gl_20.cpp
**
** Fallback code for ancient hardware
** This file collects everything larger that is only needed for
** OpenGL v1.1 is required (1997+ cards?), the same file for GL2x path.
** The difference GL2 makes is no blurry textures thanks to NPOT support.
**
*/

#include "gl_20.h"
#include "gl/dynlights/gl_dynlightcache.h"

//FGLBSPCache g_BSPRenderCache; 

CVAR(Bool, gl_lights_additive, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_legacy_mode, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOSET)

//CVAR(Bool, gl_legacy_dynlight_baked_huge, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_legacy_dynlight_compress_range, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_legacy_dynlight_brightness, 1.8f, CVAR_ARCHIVE)
CVAR(Int, gl_legacy_dynlight_saturation_thresh, 90, CVAR_ARCHIVE)     // Dark surface cutoff ceiling (Range: 32 to 192)
CVAR(Float, gl_legacy_dynlight_saturation_dark, 0.2, CVAR_ARCHIVE)   // Color saturation modifier applied to dim/dark surfaces
CVAR(Float, gl_legacy_dynlight_saturation_bright, 0.5f, CVAR_ARCHIVE) // Color saturation modifier applied to bright surfaces
CVAR(Float, gl_legacy_dynlight_hue_shift, 22.4f, CVAR_ARCHIVE) // Shift dynamic light hue (-180.0 to 180.0). 0 = Disabled.

CVAR(Bool, gl_legacy_dynlight_overbright, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // Dynlight overbright global switch used in gl_scene.cpp
CVAR(Float, gl_legacy_dynlight_overbright_flats, 0.125f, CVAR_ARCHIVE) // Intensity multiplier for flats
CVAR(Float, gl_legacy_dynlight_overbright_walls, 0.125f, CVAR_ARCHIVE) // Intensity multiplier for walls

//==========================================================================
//
// Do some tinkering with the menus so that certain options only appear
// when they are actually valid.
//
//==========================================================================

void gl_PatchMenu()
{
	// Radial fog and Doom lighting are not available without full shader support.
	if (!gl_legacy_mode) return;

	FOptionValues **opt = OptionValues.CheckKey("LightingModes");
	if (opt != NULL)
	{
		for (int i = (*opt)->mValues.Size() - 1; i >= 0; i--)
		{
			// Delete 'Doom' lighting mode
			if ((*opt)->mValues[i].Value == 2.0 || (*opt)->mValues[i].Value == 8.0 || (*opt)->mValues[i].Value == 16.0)
			{
				(*opt)->mValues.Delete(i);
			}
		}
	}

	opt = OptionValues.CheckKey("FogMode");
	if (opt != NULL)
	{
		for (int i = (*opt)->mValues.Size() - 1; i >= 0; i--)
		{
			// Delete 'Radial' fog mode
			if ((*opt)->mValues[i].Value == 2.0)
			{
				(*opt)->mValues.Delete(i);
			}
		}
	}

	// disable features that don't work without shaders.
	if (gl_lightmode == 2 || gl_lightmode == 8 || gl_lightmode == 16) gl_lightmode = 3;
	if (gl_fogmode == 2) gl_fogmode = 1;

	// remove more unsupported stuff like postprocessing options.
	// This cannot be done with a menu filter because the renderer gets initialized long after the menu is set up.
	DMenuDescriptor **desc = MenuDescriptors.CheckKey("OpenGLOptions");
	if (desc != nullptr && (*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor)))
	{
		auto md = static_cast<DOptionMenuDescriptor*>(*desc);
		for (int i = md->mItems.Size() - 1; i >= 0; i--)
		{
			if (!stricmp(md->mItems[i]->mAction.GetChars(), "gl_multisample") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_tonemap") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_bloom") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_lens") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_ssao") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_ssao_portals") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_fxaa") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_paltonemap_powtable") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "vr_mode") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "vr_enable_quadbuffered") ||
				!stricmp(md->mItems[i]->mAction.GetChars(), "gl_paltonemap_reverselookup"))
			{
				md->mItems.Delete(i);
			}
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================

void gl_SetTextureMode(int type)
{
	if (type == TM_MASK)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_OPAQUE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_INVERSE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_INVERTOPAQUE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_ONE_MINUS_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	}
	else if (type == TM_BRIGHTMAP_LEGACY)
	{
		// Turn modulation on: brightmap texture will be multiplied by glColor
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		// Make sure that scale has reset to 1, otherwise overbright
		glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, 1);
	}

	//else if (type == TM_SHADEDLIGHT_LEGACY)
	//{
	//	// Switch the pipeline to advanced layer combining mode
	//	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);

	//	// RGB CHANNEL: Multiply the spotlight texture (Texture0) by the vertex color (Primary Color).
	//	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
	//	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE0);
	//	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PRIMARY_COLOR);
	//	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
	//	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);

	//	// HARD ALPHA PROTECTION: Completely replace spotlight alpha with geometry alpha.
	//	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
	//	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_PRIMARY_COLOR);
	//	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);

	//	// Enable hardware generation of eye space coordinates
	//	glEnable(GL_TEXTURE_GEN_S);
	//	glEnable(GL_TEXTURE_GEN_T);
	//	glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	//	glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
	//}

	else // if (type == TM_MODULATE)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	}
}

//==========================================================================
//
//
//
//==========================================================================

static int ffTextureMode;
static bool ffTextureEnabled;
static bool ffFogEnabled;
static PalEntry ffFogColor;
static int ffSpecialEffect;
static float ffFogDensity;
static bool currentTextureMatrixState;
static bool currentModelMatrixState;

extern int modellightindex; // to distinguish between different dynlight types
extern bool isUsingVolumetric3DModelLegacyDynlight;
void FRenderState::ApplyFixedFunction()
{
	int thistm = mTextureMode == TM_MODULATE && mTempTM == TM_OPAQUE ? TM_OPAQUE : mTextureMode;
	if (thistm != ffTextureMode)
	{
		ffTextureMode = thistm;
		if (ffTextureMode == TM_CLAMPY) ffTextureMode = TM_MODULATE;
		gl_SetTextureMode(ffTextureMode);
	}
	if (mTextureEnabled != ffTextureEnabled)
	{
		if ((ffTextureEnabled = mTextureEnabled)) glEnable(GL_TEXTURE_2D);
		else glDisable(GL_TEXTURE_2D);
	}
	if (mFogEnabled != ffFogEnabled)
	{
		if ((ffFogEnabled = mFogEnabled))
		{
			glEnable(GL_FOG);
		}
		else glDisable(GL_FOG);
	}
	if (mFogEnabled)
	{
		if (ffFogColor != mFogColor)
		{
			ffFogColor = mFogColor;
			GLfloat FogColor[4] = { mFogColor.r / 255.0f,mFogColor.g / 255.0f,mFogColor.b / 255.0f,0.0f };
			glFogfv(GL_FOG_COLOR, FogColor);
		}
		if (ffFogDensity != mLightParms[2])
		{
			// Check if the incoming fog color is black or sits within the strict 5-unit tolerance threshold.
			// If it's a software shadow simulation fog, fallback to standard soft legacy attenuation coefficients.
			// If it's a true environmental map colored fog (brighter color fog), unlock dense multipliers.
			if (mFogColor.r <= 5 && mFogColor.g <= 5 && mFogColor.b <= 5)
			{
				// Stock soft factory decay curve to protect shadows from blinding pitch black voids
				glFogf(GL_FOG_DENSITY, mLightParms[2] * -0.6931471f);
			}
			else
			{
				// High-intensity vivid color fog injection cloned straight from the GL3 pipeline specs
				glFogf(GL_FOG_DENSITY, mLightParms[2] * -1.12f);
			}
			ffFogDensity = mLightParms[2];
		}
	}
	if (mSpecialEffect != ffSpecialEffect)
	{
		switch (ffSpecialEffect)
		{
		case EFF_SPHEREMAP:
			glDisable(GL_TEXTURE_GEN_T);
			glDisable(GL_TEXTURE_GEN_S);
		default:
			break;
		}
		switch (mSpecialEffect)
		{
		case EFF_SPHEREMAP:
			glEnable(GL_TEXTURE_GEN_T);
			glEnable(GL_TEXTURE_GEN_S);
			glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
			glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
			break;
		default:
			break;
		}
		ffSpecialEffect = mSpecialEffect;
	}

	FStateVec4 col = mColor;

	// Legacy sprites and 3D models dynamic light (flat) - START
	if (modellightindex == -2 && isUsingVolumetric3DModelLegacyDynlight)
	{
		// Disable flat coloring for legacy VOLUMETRIC dynamic lights
		//col.vec[0] += mDynColor.vec[0];
		//col.vec[1] += mDynColor.vec[1];
		//col.vec[2] += mDynColor.vec[2];
	}
	else
	{
		// Activate flat coloring for sprites and legacy FLAT dynamic lights
		col.vec[0] += mDynColor.vec[0];
		col.vec[1] += mDynColor.vec[1];
		col.vec[2] += mDynColor.vec[2];
	}
	// Legacy sprites and 3D models dynamic light (flat) - FINISH

	col.vec[0] = clamp(col.vec[0], 0.f, 1.f);
	col.vec[1] = clamp(col.vec[1], 0.f, 1.f);
	col.vec[2] = clamp(col.vec[2], 0.f, 1.f);
	col.vec[3] = clamp(col.vec[3], 0.f, 1.f);

	col.vec[0] *= (mObjectColor.r / 255.f);
	col.vec[1] *= (mObjectColor.g / 255.f);
	col.vec[2] *= (mObjectColor.b / 255.f);
	col.vec[3] *= (mObjectColor.a / 255.f);
	glColor4fv(col.vec); // Applies clean baseline sector shadow level

	glEnable(GL_BLEND);
	if (mAlphaThreshold > 0)
	{
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, mAlphaThreshold * col.vec[3]);
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}

	// Protect texture matrix allocations during mode 13 projection pass
	if (mTextureMatrixEnabled && ffTextureMode != 13)
	{
		glMatrixMode(GL_TEXTURE);
		glLoadMatrixf(mTextureMatrix.get());
		currentTextureMatrixState = true;
	}
	else if (currentTextureMatrixState && ffTextureMode != 13)
	{
		glMatrixMode(GL_TEXTURE);
		glLoadIdentity();
		currentTextureMatrixState = false;
	}

	if (mModelMatrixEnabled)
	{
		VSMatrix mult = mViewMatrix;
		mult.multMatrix(mModelMatrix);
		glMatrixMode(GL_MODELVIEW);
		glLoadMatrixf(mult.get());
		currentModelMatrixState = true;
	}
	else if (currentModelMatrixState)
	{
		glMatrixMode(GL_MODELVIEW);
		glLoadMatrixf(mViewMatrix.get());
		currentModelMatrixState = false;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void gl_FillScreen();

void FRenderState::DrawColormapOverlay()
{
	float r, g, b;
	if (mColormapState > CM_DEFAULT && mColormapState < CM_MAXCOLORMAP)
	{
		FSpecialColormap *scm = &SpecialColormaps[mColormapState - CM_FIRSTSPECIALCOLORMAP];
		float m[] = { scm->ColorizeEnd[0] - scm->ColorizeStart[0],
			scm->ColorizeEnd[1] - scm->ColorizeStart[1], scm->ColorizeEnd[2] - scm->ColorizeStart[2], 0.f };

		if (m[0] < 0 && m[1] < 0 && m[2] < 0)
		{
			gl_RenderState.SetColor(1, 1, 1, 1);
			gl_RenderState.BlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO);
			gl_FillScreen();

			r = scm->ColorizeStart[0];
			g = scm->ColorizeStart[1];
			b = scm->ColorizeStart[2];
		}
		else
		{
			r = scm->ColorizeEnd[0];
			g = scm->ColorizeEnd[1];
			b = scm->ColorizeEnd[2];
		}
	}
	else if (mColormapState == CM_LITE)
	{
		if (gl_enhanced_nightvision)
		{
			r = 0.375f, g = 1.0f, b = 0.375f;
		}
		else
		{
			return;
		}
	}
	else if (mColormapState >= CM_TORCH)
	{
		int flicker = mColormapState - CM_TORCH;
		r = (0.8f + (7 - flicker) / 70.0f);
		if (r > 1.0f) r = 1.0f;
		b = g = r;
		if (gl_enhanced_nightvision) b = g * 0.75f;
	}
	else return;

	gl_RenderState.SetColor(r, g, b, 1.f);
	gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ZERO);
	gl_FillScreen();
	gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

//==========================================================================
//
// Sets up the parameters to render one dynamic light onto one plane
//
//==========================================================================

struct FLightColorContext
{
	float r, g, b;
	float maxColor;
	float minColor;
	float chroma;

	FLightColorContext(FDynamicLight* light)
	{
		r = light->GetRed() / 255.0f;
		g = light->GetGreen() / 255.0f;
		b = light->GetBlue() / 255.0f;

		maxColor = r; if (g > maxColor) maxColor = g; if (b > maxColor) maxColor = b;
		minColor = r; if (g < minColor) minColor = g; if (b < minColor) minColor = b;
		chroma = maxColor - minColor;
	}
};

void gl_dynlightSaturateLegacy(float &r, float &g, float &b, float current_boost, float radius, const FLightColorContext &ctx)
{
	// Prevent logic executions on zero/dead light nodes
	if (ctx.maxColor > 0.001f)
	{
		float avgColor = (r + g + b) * 0.3333f;

		// Amplificate channels relative to how much they exceed the baseline average color
		r += (r - avgColor) * current_boost;
		g += (g - avgColor) * current_boost;
		b += (b - avgColor) * current_boost;

		// Re-evaluate limits after saturation boost locally
		float localMax = r; if (g > localMax) localMax = g; if (b > localMax) localMax = b;
		float localMin = r; if (g < localMin) localMin = g; if (b < localMin) localMin = b;
		float localChroma = localMax - localMin;

		// HARD CRITERIA: Execute hue shifting ONLY on colored lights larger than 768 units radius
		if (ctx.chroma >= 0.10f && localChroma > 0.05f && radius >= 768.0f)
		{
			// --- STEP-BY-STEP CASCADE HUE SHIFT CALCULATION (+5 per size tier) ---
			float                                             sizeShiftModifier = 0.0f; // Base level
			if      (radius >= 384.0f && radius <= 800.0f)    sizeShiftModifier = 3.2f;
			else if (radius >= 800.0f && radius <= 1600.0f)   sizeShiftModifier = 4.5f;
			else if (radius >= 1600.0f && radius <= 3000.0f)  sizeShiftModifier = 5.0f;
			else if (radius >= 3000.0f && radius <= 6000.0f)  sizeShiftModifier = 5.5f;
			else if (radius >= 6000.0f && radius <= 12000.0f) sizeShiftModifier = 6.5f;
			else if (radius >= 12000.0f)                      sizeShiftModifier = 7.0f;

			float raw_shift = gl_legacy_dynlight_hue_shift + sizeShiftModifier;
			float final_shift = 0.0f;

			// Guard condition: inside [-20; 20] range hue modulation remains untouched
			if (fabsf(raw_shift) > 20.0f)
			{
				if (raw_shift > 0.0f) final_shift = raw_shift - 20.0f;
				else final_shift = raw_shift + 20.0f;
			}

			if (final_shift < 0.0f) final_shift = 360.0f + fmodf(final_shift, 360.0f);
			if (final_shift >= 360.0f) final_shift = fmodf(final_shift, 360.0f);

			// Execute chromatic spectrum translation if final rotation angle is valid
			if (final_shift > 0.001f)
			{
				float h = 0.0f;
				if (localMax == r) { h = (g - b) / localChroma; if (h < 0.0f) h += 6.0f; }
				else if (localMax == g) { h = (b - r) / localChroma + 2.0f; }
				else { h = (r - g) / localChroma + 4.0f; }
				h *= 60.0f;

				h += final_shift;
				if (h >= 360.0f) h -= 360.0f;

				float hPrime = h / 60.0f;
				float x = localChroma * (1.0f - fabsf(fmodf(hPrime, 2.0f) - 1.0f));
				float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;

				if (hPrime >= 0.0f && hPrime < 1.0f) { r1 = localChroma; g1 = x; }
				else if (hPrime >= 1.0f && hPrime < 2.0f) { r1 = x; g1 = localChroma; }
				else if (hPrime >= 2.0f && hPrime < 3.0f) { g1 = localChroma; b1 = x; }
				else if (hPrime >= 3.0f && hPrime < 4.0f) { g1 = x; b1 = localChroma; }
				else if (hPrime >= 4.0f && hPrime < 5.0f) { r1 = x; b1 = localChroma; }
				else { r1 = localChroma; b1 = x; }

				float m = localMin;
				r = r1 + m;
				g = g1 + m;
				b = b1 + m;
			}
		}

		if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
		if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
		if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
	}
}

bool gl_SetupLightWall(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive)
{
	FVector3 fn, pos;
	const float invMul64 = 1.0f / 64.0f;
	const float invMul255 = 1.0f / 255.0f;
	const float invMul286 = 1.0f / 286.0f;
	const float invMul322 = 1.0f / 322.0f;
	const float invMul424 = 1.0f / 424.0f;

	DVector3 lpos = light->PosRelative(group);

	float dist = fabsf(p.DistToPoint(lpos.X, lpos.Z, lpos.Y));
	float radius = light->GetRadius();

	if (radius <= 0.f) return false;
	if (dist > radius) return false;
	if (checkside && gl_lights_checkside && p.PointOnSide(lpos.X, lpos.Z, lpos.Y))
	{
		return false;
	}
	if (!light->visibletoplayer)
	{
		return false;
	}

	// 1. INITIALIZE LIGHT COLOR CONTEXT ONCE ON THE CPU
	FLightColorContext colorCtx(light);

	// Decrease distance to dynlight to have a richer color saturation in the far
	float                                             distFactor = 1.0f;
	if      (radius >= 384.0f && radius <= 800.0f)    distFactor = 0.88f;
	else if (radius >= 800.0f && radius <= 1600.0f)   distFactor = 0.77f;
	else if (radius >= 1600.0f && radius <= 3000.0f)  distFactor = 0.74f;
	else if (radius >= 3000.0f && radius <= 6000.0f)  distFactor = 0.72f;
	else if (radius >= 6000.0f && radius <= 12000.0f) distFactor = 0.67f;
	else if (radius >= 12000.0f)                      distFactor = 0.64f;

	scale = 1.0f / ((2.25f * radius) - (dist * distFactor));

	pos = { (float)lpos.X, (float)lpos.Z, (float)lpos.Y };
	fn = p.Normal();
	fn.GetRightUp(right, up);

	FVector3 tmpVec = fn * dist;
	nearPt = pos + tmpVec;

	float cs = 0.0f;

	// Compress range ONLY if CVAR is enabled AND dynlight is at least 10% colorful
	// That can get you richer color saturation on farther surfaces and some ledges
	if (gl_legacy_dynlight_compress_range && colorCtx.chroma >= 0.10f)
	{
		// Compress range and make darker lit surfaces brighter and saturated
		float lightRatio = dist / radius;
		if      (lightRatio > 1.0f) lightRatio = 1.0f;
		else if (lightRatio < 0.0f) lightRatio = 0.0f;
		cs = 0.75f - powf(lightRatio, 2.75f);
	}
	else
	{
		cs = 1.0f - (dist / radius);
	}
	if (additive) cs *= 0.2f;
	if (colorCtx.chroma >= 0.10f) cs *= gl_legacy_dynlight_brightness;

	// Dynamically look up the subsector at light source position to pull map light level
	int surface_lightlevel = 255;
	subsector_t *light_subsector = P_PointInSubsector(lpos.X, lpos.Y);
	if (light_subsector && light_subsector->sector)
	{
		surface_lightlevel = light_subsector->sector->lightlevel;
	}

	float r, g, b;
	float current_boost;

	// Calculate strict bounds for the smooth 64-unit transition window around the threshold
	float lowerBound = (float)gl_legacy_dynlight_saturation_thresh - 32.0f;
	float upperBound = (float)gl_legacy_dynlight_saturation_thresh + 32.0f;

	if ((float)surface_lightlevel <= lowerBound)
	{
		// 1. PURE DARK ZONE: 100% full original formula and full dark saturation boost
		r = colorCtx.r * cs;
		g = colorCtx.g * cs;
		b = colorCtx.b * cs;

		current_boost = gl_legacy_dynlight_saturation_dark;
	}
	else if ((float)surface_lightlevel >= upperBound)
	{
		// 2. PURE BRIGHT ZONE: 100% tamed formulas and full bright saturation modifier
		if (radius >= 384.0f && radius <= 800.0f)
		{
			r = light->GetRed() * invMul286 * cs;
			g = light->GetGreen() * invMul286 * cs;
			b = light->GetBlue() * invMul286 * cs;
		}
		else if (radius >= 800.0f && radius <= 1600.0f)
		{
			r = light->GetRed() * invMul322 * cs;
			g = light->GetGreen() * invMul322 * cs;
			b = light->GetBlue() * invMul322 * cs;
		}
		else if (radius >= 1600.0f)
		{
			r = light->GetRed() * invMul424 * cs;
			g = light->GetGreen() * invMul424 * cs;
			b = light->GetBlue() * invMul424 * cs;
		}
		else
		{
			r = colorCtx.r * cs;
			g = colorCtx.g * cs;
			b = colorCtx.b * cs;
		}

		current_boost = gl_legacy_dynlight_saturation_bright;
	}
	else
	{
		// 3. SMOOTH TRANSITION WINDOW (64 units span centered exactly at thresh)
		float factor = ((float)surface_lightlevel - lowerBound) * invMul64; // 0.0 at lowerBound, 1.0 at upperBound

		// Interpolate the saturation boost factor between dark and bright settings
		current_boost = gl_legacy_dynlight_saturation_dark + (gl_legacy_dynlight_saturation_bright - gl_legacy_dynlight_saturation_dark) * factor;

		// Calculate both configurations to perform a seamless blend
		float r_full = colorCtx.r * cs;
		float g_full = colorCtx.g * cs;
		float b_full = colorCtx.b * cs;

		float r_tame, g_tame, b_tame;
		if (radius >= 384.0f && radius <= 800.0f)
		{
			r_tame = light->GetRed() * invMul286 * cs; g_tame = light->GetGreen() * invMul286 * cs; b_tame = light->GetBlue() * invMul286 * cs;
		}
		else if (radius >= 800.0f && radius <= 1600.0f)
		{
			r_tame = light->GetRed() * invMul322 * cs; g_tame = light->GetGreen() * invMul322 * cs; b_tame = light->GetBlue() * invMul322 * cs;
		}
		else if (radius >= 1600.0f)
		{
			r_tame = light->GetRed() * invMul424 * cs; g_tame = light->GetGreen() * invMul424 * cs; b_tame = light->GetBlue() * invMul424 * cs;
		}
		else
		{
			r_tame = r_full; g_tame = g_full; b_tame = b_full;
		}

		// Blend them: closer to lowerBound means more full power color intensity
		r = r_full + (r_tame - r_full) * factor;
		g = g_full + (g_tame - g_full) * factor;
		b = b_full + (b_tame - b_full) * factor;
	}

	// Route final pipeline colors with pre-calculated context and radius constraints
	gl_dynlightSaturateLegacy(r, g, b, current_boost, radius, colorCtx);

	if (light->IsSubtractive())
	{
		gl_RenderState.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
		float length = float(FVector3(r, g, b).Length());
		r = length - r; g = length - g; b = length - b;
	}
	else
	{
		gl_RenderState.BlendEquation(GL_FUNC_ADD);
	}
	gl_RenderState.SetColor(r, g, b);
	return true;
}

bool gl_SetupLightFlat(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive)
{
	FVector3 fn, pos;
	const float invMul64 = 1.0f / 64.0f;
	const float invMul255 = 1.0f / 255.0f;
	const float invMul284 = 1.0f / 284.0f;
	const float invMul322 = 1.0f / 322.0f;
	const float invMul444 = 1.0f / 444.0f;

	DVector3 lpos = light->PosRelative(group);

	float dist = fabsf(p.DistToPoint(lpos.X, lpos.Z, lpos.Y));
	float radius = light->GetRadius();

	if (radius <= 0.f) return false;
	if (dist > radius) return false;
	if (checkside && gl_lights_checkside && p.PointOnSide(lpos.X, lpos.Z, lpos.Y))
	{
		return false;
	}
	if (!light->visibletoplayer)
	{
		return false;
	}

	// 1. INITIALIZE LIGHT COLOR CONTEXT ONCE ON THE CPU
	FLightColorContext colorCtx(light);

	// Decrease distance to dynlight to have a richer color saturation in the far
	float                                             distFactor = 1.0f;
	if      (radius >= 384.0f && radius <= 800.0f)    distFactor = 0.88f;
	else if (radius >= 800.0f && radius <= 1600.0f)   distFactor = 0.77f;
	else if (radius >= 1600.0f && radius <= 3000.0f)  distFactor = 0.74f;
	else if (radius >= 3000.0f && radius <= 6000.0f)  distFactor = 0.72f;
	else if (radius >= 6000.0f && radius <= 12000.0f) distFactor = 0.67f;
	else if (radius >= 12000.0f)                      distFactor = 0.64f;

	scale = 1.0f / ((2.25f * radius) - (dist * distFactor));

	pos = { (float)lpos.X, (float)lpos.Z, (float)lpos.Y };
	fn = p.Normal();
	fn.GetRightUp(right, up);

	FVector3 tmpVec = fn * dist;
	nearPt = pos + tmpVec;

	float cs = 0.0f;
	// Compress range ONLY if CVAR is enabled AND dynlight is at least 10% colorful
	// That can get you richer color saturation on farther surfaces and some ledges
	if (gl_legacy_dynlight_compress_range && colorCtx.chroma >= 0.10f)
	{
		// Compress range and make darker lit surfaces brighter and saturated
		float    lightRatio = dist / radius;
		if      (lightRatio > 1.0f) lightRatio = 1.0f;
		else if (lightRatio < 0.0f) lightRatio = 0.0f;
		cs = 0.75f - powf(lightRatio, 2.75f);
	}
	else
	{
		cs = 1.0f - (dist / radius);
	}
	if (additive) cs *= 0.2f;
	if (colorCtx.chroma >= 0.10f) cs *= gl_legacy_dynlight_brightness;

	// Dynamically look up the subsector at light source position to pull map light level
	int surface_lightlevel = 255;
	subsector_t *light_subsector = P_PointInSubsector(lpos.X, lpos.Y);
	if (light_subsector && light_subsector->sector)
	{
		surface_lightlevel = light_subsector->sector->lightlevel;
	}

	float r, g, b;
	float current_boost;

	// Calculate strict bounds for the smooth 64-unit transition window around the threshold
	float lowerBound = (float)gl_legacy_dynlight_saturation_thresh - 32.0f;
	float upperBound = (float)gl_legacy_dynlight_saturation_thresh + 32.0f;

	if ((float)surface_lightlevel <= lowerBound)
	{
		// 1. PURE DARK ZONE: 100% full original formula and full dark saturation boost
		r = colorCtx.r * cs;
		g = colorCtx.g * cs;
		b = colorCtx.b * cs;

		current_boost = gl_legacy_dynlight_saturation_dark;
	}
	else if ((float)surface_lightlevel >= upperBound)
	{
		// 2. PURE BRIGHT ZONE: 100% tamed formulas and full bright saturation modifier
		if (radius >= 384.0f && radius <= 800.0f)
		{
			r = light->GetRed() * invMul284 * cs;
			g = light->GetGreen() * invMul284 * cs;
			b = light->GetBlue() * invMul284 * cs;
		}
		else if (radius >= 800.0f && radius <= 1600.0f)
		{
			r = light->GetRed() * invMul322 * cs;
			g = light->GetGreen() * invMul322 * cs;
			b = light->GetBlue() * invMul322 * cs;
		}
		else if (radius >= 1600.0f)
		{
			r = light->GetRed() * invMul444 * cs;
			g = light->GetGreen() * invMul444 * cs;
			b = light->GetBlue() * invMul444 * cs;
		}
		else
		{
			r = colorCtx.r * cs;
			g = colorCtx.g * cs;
			b = colorCtx.b * cs;
		}

		current_boost = gl_legacy_dynlight_saturation_bright;
	}
	else
	{
		// 3. SMOOTH SMOOTH TRANSITION WINDOW (64 units span centered exactly at thresh)
		float factor = ((float)surface_lightlevel - lowerBound) * invMul64; // 0.0 at lowerBound, 1.0 at upperBound

		// Interpolate the saturation boost factor between dark and bright settings
		current_boost = gl_legacy_dynlight_saturation_dark + (gl_legacy_dynlight_saturation_bright - gl_legacy_dynlight_saturation_dark) * factor;

		// Calculate both configurations to perform a seamless blend
		float r_full = colorCtx.r * cs;
		float g_full = colorCtx.g * cs;
		float b_full = colorCtx.b * cs;

		float r_tame, g_tame, b_tame;
		if (radius >= 384.0f && radius <= 800.0f)
		{
			r_tame = light->GetRed() * invMul284 * cs; g_tame = light->GetGreen() * invMul284 * cs; b_tame = light->GetBlue() * invMul284 * cs;
		}
		else if (radius >= 800.0f && radius <= 1600.0f)
		{
			r_tame = light->GetRed() * invMul322 * cs; g_tame = light->GetGreen() * invMul322 * cs; b_tame = light->GetBlue() * invMul322 * cs;
		}
		else if (radius >= 1600.0f)
		{
			r_tame = light->GetRed() * invMul444 * cs; g_tame = light->GetGreen() * invMul444 * cs; b_tame = light->GetBlue() * invMul444 * cs;
		}
		else
		{
			r_tame = r_full; g_tame = g_full; b_tame = b_full;
		}

		// Blend them: closer to lowerBound means more full power color intensity
		r = r_full + (r_tame - r_full) * factor;
		g = g_full + (g_tame - g_full) * factor;
		b = b_full + (b_tame - b_full) * factor;
	}

	// Route final pipeline colors into the custom saturation encapsulation pass with radius constraints
	gl_dynlightSaturateLegacy(r, g, b, current_boost, radius, colorCtx);

	if (light->IsSubtractive())
	{
		gl_RenderState.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
		float length = float(FVector3(r, g, b).Length());
		r = length - r;
		g = length - g;
		b = length - b;
	}
	else
	{
		gl_RenderState.BlendEquation(GL_FUNC_ADD);
	}
	gl_RenderState.SetColor(r, g, b);
	return true;
}

// Way less advanced code:
//bool gl_SetupLightWall(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive)
//{
//	FVector3 fn, pos;
//
//	DVector3 lpos = light->PosRelative(group);
//
//	float dist = fabsf(p.DistToPoint(lpos.X, lpos.Z, lpos.Y));
//	float radius = light->GetRadius();
//	const float invMul255 = 1.0f / 255.0f;
//	const float invMul286 = 1.0f / 286.0f;
//	const float invMul322 = 1.0f / 322.0f;
//	const float invMul424 = 1.0f / 424.0f;
//
//	//if (gl.legacyMode && (light->IsAttenuated()))
//	//{
//	//	radius *= 0.66f;
//	//}
//
//	if (radius <= 0.f) return false;
//	if (dist > radius) return false;
//	if (checkside && gl_lights_checkside && p.PointOnSide(lpos.X, lpos.Z, lpos.Y))
//	{
//		return false;
//	}
//	if (!light->visibletoplayer)
//	{
//		return false;
//	}
//
//	//scale = 1.0f / ((2.f * radius) - dist);
//	scale = 1.0f / ((2.25f * radius) - dist);
//
//	// project light position onto plane (find closest point on plane)
//
//
//	pos = { (float)lpos.X, (float)lpos.Z, (float)lpos.Y };
//	fn = p.Normal();
//
//	fn.GetRightUp(right, up);
//
//	FVector3 tmpVec = fn * dist;
//	nearPt = pos + tmpVec;
//
//	float cs = 1.0f - (dist / radius);
//	if (additive) cs *= 0.2f;	// otherwise the light gets too strong.
//
//	//float r, g, b;
//	//r = light->GetRed() * invMul255 * cs;
//	//g = light->GetGreen() * invMul255 * cs;
//	//b = light->GetBlue() * invMul255 * cs;
//
//	// GL1x/GL2x fixed function pipeline 1 pass dynligts clamp to 1.0.
//	// That means that the brigther the ligtlevel of a surface, the
//	// less saturated color it will get as it's going to get whiter.
//	// That's why we tame down dynligt intensities on brigter walls.
//	// The bigger the dynlight radius the less brighter it should get
//	float r, g, b;
//	if (radius >= 384.0f && radius <= 800.0f)
//	{
//		r = light->GetRed() * invMul286 * cs;
//		g = light->GetGreen() * invMul286 * cs;
//		b = light->GetBlue() * invMul286 * cs;
//	}
//	else if (radius >= 800.0f && radius <= 1600.0f)
//	{
//		r = light->GetRed() * invMul322 * cs;
//		g = light->GetGreen() * invMul322 * cs;
//		b = light->GetBlue() * invMul322 * cs;
//	}
//	else if (radius >= 1600.0f)
//	{
//		r = light->GetRed() * invMul424 * cs;
//		g = light->GetGreen() * invMul424 * cs;
//		b = light->GetBlue() * invMul424 * cs;
//	}
//	else
//	{
//		r = light->GetRed() * invMul255 * cs;
//		g = light->GetGreen() * invMul255 * cs;
//		b = light->GetBlue() * invMul255 * cs;
//	}
//
//	if (light->IsSubtractive())
//	{
//		gl_RenderState.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
//		float length = float(FVector3(r, g, b).Length());
//		r = length - r;
//		g = length - g;
//		b = length - b;
//	}
//	else
//	{
//		gl_RenderState.BlendEquation(GL_FUNC_ADD);
//	}
//	gl_RenderState.SetColor(r, g, b);
//	return true;
//}
//
//bool gl_SetupLightFlat(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive)
//{
//	// we need to get flats darker, that's why we have a separate gl_SetupLightFlat function
//
//	FVector3 fn, pos;
//
//	DVector3 lpos = light->PosRelative(group);
//
//	float dist = fabsf(p.DistToPoint(lpos.X, lpos.Z, lpos.Y));
//	float radius = light->GetRadius();
//	const float invMul255 = 1.0f / 255.0f;
//	const float invMul284 = 1.0f / 284.0f;
//	const float invMul322 = 1.0f / 322.0f;
//	const float invMul444 = 1.0f / 444.0f;
//
//	//if (gl.legacyMode && (light->IsAttenuated()))
//	//{
//	//	radius *= 0.66f;
//	//}
//
//	if (radius <= 0.f) return false;
//	if (dist > radius) return false;
//	if (checkside && gl_lights_checkside && p.PointOnSide(lpos.X, lpos.Z, lpos.Y))
//	{
//		return false;
//	}
//	if (!light->visibletoplayer)
//	{
//		return false;
//	}
//
//	//scale = 1.0f / ((2.f * radius) - dist);
//	scale = 1.0f / ((2.25f * radius) - dist);
//
//	// project light position onto plane (find closest point on plane)
//
//
//	pos = { (float)lpos.X, (float)lpos.Z, (float)lpos.Y };
//	fn = p.Normal();
//	fn.GetRightUp(right, up);
//
//	FVector3 tmpVec = fn * dist;
//	nearPt = pos + tmpVec;
//
//	float cs = 1.0f - (dist / radius);
//	if (additive) cs *= 0.2f;	// otherwise the light gets too strong.
//
//	// GL1x/GL2x fixed function pipeline 1 pass dynligts clamp to 1.0.
//	// That means that the brigther the ligtlevel of a surface, the
//	// less saturated color it will get as it's going to get whiter.
//	// That's why we tame down dynligt intensities on brigter flats.
//	// The bigger the dynlight radius the less brighter it should get
//
//	float r, g, b;
//	// we need to get flats darker, that's why we increase divisors twice from 255.0f to 510.0f
//	if (radius >= 384.0f && radius <= 800.0f)
//	{
//		r = light->GetRed() * invMul284 * cs;
//		g = light->GetGreen() * invMul284 * cs;
//		b = light->GetBlue() * invMul284 * cs;
//	}
//	else if (radius >= 800.0f && radius <= 1600.0f)
//	{
//		r = light->GetRed() * invMul322 * cs;
//		g = light->GetGreen() * invMul322 * cs;
//		b = light->GetBlue() * invMul322 * cs;
//	}
//	else if (radius >= 1600.0f)
//	{
//		r = light->GetRed() * invMul444 * cs;
//		g = light->GetGreen() * invMul444 * cs;
//		b = light->GetBlue() * invMul444 * cs;
//	}
//	else
//	{
//		r = light->GetRed() * invMul255 * cs;
//		g = light->GetGreen() * invMul255 * cs;
//		b = light->GetBlue() * invMul255 * cs;
//	}
//
//	if (light->IsSubtractive())
//	{
//		gl_RenderState.BlendEquation(GL_FUNC_REVERSE_SUBTRACT);
//		float length = float(FVector3(r, g, b).Length());
//		r = length - r;
//		g = length - g;
//		b = length - b;
//	}
//	else
//	{
//		gl_RenderState.BlendEquation(GL_FUNC_ADD);
//	}
//	gl_RenderState.SetColor(r, g, b);
//	return true;
//}

//==========================================================================
//
//
//
//==========================================================================

bool gl_SetupLightTexture()
{
	if (!GLRenderer->glLight.isValid()) return false;
	FMaterial * pat = FMaterial::ValidateTexture(GLRenderer->glLight, false, false);
	gl_RenderState.SetMaterial(pat, CLAMP_XY_NOMIP, 0, -1, false);
	return true;
}

//==========================================================================
//
// Project dynlights onto BRIGHTMAP_LEGACY cases.
// (5% FPS boost on walls, MINUS 20%! if flats are enabled
// kept for further works)
//
//==========================================================================
//bool gl_GetWallStaticLightmaps(seg_t *seg, float ztop, float zbottom, float *topLightmapColor, float *bottomLightmapColor)
//{
//	// STRICT CHECK: Run ONLY in legacy engine mode to protect shader pipelines
//	if (!gl.legacyMode || !seg || !seg->sidedef || !gl_lights || !gl_legacy_dynlight_baked_huge) return false;
//
//	topLightmapColor[0] = topLightmapColor[1] = topLightmapColor[2] = 0.0f;
//	bottomLightmapColor[0] = bottomLightmapColor[1] = bottomLightmapColor[2] = 0.0f;
//
//	bool hasStaticLight = false;
//	FLightNode *node = seg->sidedef->lighthead;
//
//	while (node)
//	{
//		FDynamicLight *light = node->lightsource;
//
//		// Bake only large static/environmental emitters (radius >= 512)
//		// Don't forget to turn off the "PrepareLight" method
//		if (light && light->IsActive() && light->GetRadius() >= 512.0f)
//		{
//			float radius = light->GetRadius();
//
//			// Extract map positions using native DVector2 getters (.fX() and .fY())
//			float wallMidX = (float)(seg->v1->fX() + seg->v2->fX()) * 0.5f;
//			float wallMidY = (float)(seg->v1->fY() + seg->v2->fY()) * 0.5f;
//
//			DVector3 lpos = light->PosRelative(seg->frontsector->PortalGroup);
//
//			// Calculate 3D distance to Top Center of the wall segment
//			float dxTop = wallMidX - (float)lpos.X;
//			float dyTop = wallMidY - (float)lpos.Y;
//			float dzTop = ztop - (float)lpos.Z;
//			float distTop = sqrtf(dxTop * dxTop + dyTop * dyTop + dzTop * dzTop);
//
//			// Calculate 3D distance to Bottom Center of the wall segment
//			float dxBot = wallMidX - (float)lpos.X;
//			float dyBot = wallMidY - (float)lpos.Y;
//			float dzBot = zbottom - (float)lpos.Z;
//			float distBot = sqrtf(dxBot * dxBot + dyBot * dyBot + dzBot * dzBot);
//
//			if (distTop < radius || distBot < radius)
//			{
//				hasStaticLight = true;
//
//				float r_base = light->GetRed() / 255.0f;
//				float g_base = light->GetGreen() / 255.0f;
//				float b_base = light->GetBlue() / 255.0f;
//
//				if (distTop < radius)
//				{
//					float factorTop = 1.0f - (distTop / radius);
//					topLightmapColor[0] += r_base * factorTop;
//					topLightmapColor[1] += g_base * factorTop;
//					topLightmapColor[2] += b_base * factorTop;
//				}
//
//				if (distBot < radius)
//				{
//					float factorBot = 1.0f - (distBot / radius);
//					bottomLightmapColor[0] += r_base * factorBot;
//					bottomLightmapColor[1] += g_base * factorBot;
//					bottomLightmapColor[2] += b_base * factorBot;
//				}
//			}
//		}
//		node = node->nextLight;
//	}
//
//	// Dynamic hard-clamping to prevent fixed-function color overflows
//	for (int c = 0; c < 3; c++)
//	{
//		if (topLightmapColor[c] > 1.0f) topLightmapColor[c] = 1.0f;
//		if (bottomLightmapColor[c] > 1.0f) bottomLightmapColor[c] = 1.0f;
//	}
//
//	return hasStaticLight;
//}
//
//bool gl_GetFlatStaticLightmaps(subsector_t *sub, const GLSectorPlane &secPlane, float *lightmapColor)
//{
//	if (!gl.legacyMode || !sub || !sub->sector || !gl_lights || !gl_legacy_dynlight_baked_huge) return false;
//
//	lightmapColor[0] = lightmapColor[1] = lightmapColor[2] = 0.0f;
//	bool hasStaticLight = false;
//
//	FLightNode *node = sub->sector->lighthead;
//	while (node)
//	{
//		FDynamicLight *light = node->lightsource;
//
//		// Bake only large static/environmental emitters (radius >= 512)
//		// Don't forget to turn off "DrawSubsectorLights" method
//		if (light && light->IsActive() && light->GetRadius() >= 512.0f)
//		{
//			float radius = light->GetRadius();
//			float radiusSq = radius * radius;
//
//			// Extract subsector center coordinate points natively
//			float flatMidX = (float)sub->sector->centerspot.X;
//			float flatMidY = (float)sub->sector->centerspot.Y;
//
//			// Calculate the exact vertical height of the floor/ceiling plane 
//			// using engine's native secplane_t via the centerspot DVector2 structure!
//			float flatMidZ = (float)sub->sector->floorplane.ZatPoint(sub->sector->centerspot);
//
//			DVector3 lpos = light->PosRelative(sub->sector->PortalGroup);
//
//			// Calculate 3D distance from large light center to flat polygon center spot
//			float dx = flatMidX - (float)lpos.X;
//			float dy = flatMidY - (float)lpos.Y;
//			float dz = flatMidZ - (float)lpos.Z;
//			float distSq = (dx * dx) + (dy * dy) + (dz * dz);
//
//			if (distSq < radiusSq)
//			{
//				hasStaticLight = true;
//
//				float r_base = light->GetRed() / 255.0f;
//				float g_base = light->GetGreen() / 255.0f;
//				float b_base = light->GetBlue() / 255.0f;
//
//				float dist = sqrtf(distSq);
//				float factor = 1.0f - (dist / radius);
//
//				lightmapColor[0] += r_base * factor;
//				lightmapColor[1] += g_base * factor;
//				lightmapColor[2] += b_base * factor;
//			}
//		}
//		node = node->nextLight;
//	}
//
//	// Dynamic hard-clamping to prevent fixed-function color overflows
//	for (int c = 0; c < 3; c++)
//	{
//		if (lightmapColor[c] > 1.0f) lightmapColor[c] = 1.0f;
//	}
//
//	return hasStaticLight;
//}

//==========================================================================
//
// Early crappy attempts to do shadowmaps
//
//==========================================================================

//bool gl_IntersectionLineToWall(float x1, float y1, float x2, float y2,
//	float lx1, float ly1, float lx2, float ly2,
//	float &ix, float &iy)
//{
//	// Calculate direction vectors of both lines
//	float qx = x2 - x1;   float qy = y2 - y1;
//	float wx = lx2 - lx1; float wy = ly2 - ly1;
//
//	// Determinant of the lines matrix coefficient system (Cross Product)
//	float det = qx * wy - qy * wx;
//
//	// If determinant is close to zero, the lines are parallel or collinear!
//	if (fabsf(det) < 0.0001f) return false;
//
//	// Linear system solver parameters using Cramer's rule multipliers
//	float dx = lx1 - x1;
//	float dy = ly1 - y1;
//
//	float t = (dx * wy - dy * wx) / det;
//	float u = (dx * qy - dy * qx) / det;
//
//	// For a true segment intersection, both t and u parameters must sit within [0.0; 1.0] bounds!
//	if (t >= 0.0f && t <= 1.0f && u >= 0.0f && u <= 1.0f)
//	{
//		// Intersected coordinates calculation pass
//		ix = x1 + t * qx;
//		iy = y1 + t * qy;
//		return true;
//	}
//
//	return false; // No intersection found inside the segment bounds
//}
//
//bool gl_CalculateLegacyShadow(float vx, float vy, float vz, FDynamicLight *light, int portalGroup)
//{
//	if (!light || !gl_lights) return false;
//
//	// Extract raw 3D position vector from the dynamic light source
//	DVector3 lpos = light->PosRelative(portalGroup);
//
//	// Trajectory line boundaries: from the geometry vertex point STRAIGHT to the light source center
//	float x1 = vx;             float y1 = vy;             float z1 = vz;
//	float x2 = (float)lpos.X;  float y2 = (float)lpos.Y;  float z2 = (float)lpos.Z;
//
//	// Establish fast integer bounding box limits around the ray path across blockmap blocks
//	int minBX = level.blockmap.GetBlockX(x1 < x2 ? x1 : x2) - 1;
//	int maxBX = level.blockmap.GetBlockX(x1 > x2 ? x1 : x2) + 1;
//	int minBY = level.blockmap.GetBlockY(y1 < y2 ? y1 : y2) - 1;
//	int maxBY = level.blockmap.GetBlockY(y1 > y2 ? y1 : y2) + 1;
//
//	// --- BLOCKMAP GRID PROTECTION ---
//	if (minBX < 0) minBX = 0; if (maxBX >= level.blockmap.bmapwidth) maxBX = level.blockmap.bmapwidth - 1;
//	if (minBY < 0) minBY = 0; if (maxBY >= level.blockmap.bmapheight) maxBY = level.blockmap.bmapheight - 1;
//
//	// Sweep the horizontal sectors geometry partitions loop
//	for (int bx = minBX; bx <= maxBX; ++bx)
//	{
//		for (int by = minBY; by <= maxBY; ++by)
//		{
//			// Verify if the block coordinates match internal map indices
//			if (!level.blockmap.isValidBlock(bx, by)) continue;
//
//			// Direct access to flat linear block lines offset pointer index array
//			int *list = level.blockmap.GetLines(bx, by);
//			if (!list) continue;
//
//			for (int i = 0; list[i] != -1; ++i)
//			{
//				line_t *testLine = &level.lines[list[i]];
//				if (!testLine || !testLine->v1 || !testLine->v2) continue;
//
//				float lx1 = (float)testLine->v1->fX(); float ly1 = (float)testLine->v1->fY();
//				float lx2 = (float)testLine->v2->fX(); float ly2 = (float)testLine->v2->fY();
//
//				float ix, iy;
//				if (gl_IntersectionLineToWall(x1, y1, x2, y2, lx1, ly1, lx2, ly2, ix, iy))
//				{
//					// CRITERIA A: HARD INTERCEPT FOR 1-SIDED GEOMETRY (SOLID BLIND WALLS)
//					// If the line has no backsector, it's a solid void barrier! Light drops instantly!
//					if (!(testLine->flags & ML_TWOSIDED) || !testLine->backsector)
//					{
//						return true; // HARD BLOCK! VERTEX IS TRAPPED IN TOTAL SHADOW!
//					}
//
//					// CRITERIA B: DYNAMIC INTERPOLATION FOR 2-SIDED GEOMETRY (LEDGES & PORTALS)
//					// Extract absolute sector heights at the ray crossing point (ix, iy)
//					float frontFloorZ = (float)testLine->frontsector->floorplane.ZatPoint(ix, iy);
//					float frontCeilZ = (float)testLine->frontsector->ceilingplane.ZatPoint(ix, iy);
//					float backFloorZ = (float)testLine->backsector->floorplane.ZatPoint(ix, iy);
//					float backCeilZ = (float)testLine->backsector->ceilingplane.ZatPoint(ix, iy);
//
//					// Determine physical bounding steps gaps for the fluid light beam
//					float blockFloorZ = frontFloorZ > backFloorZ ? frontFloorZ : backFloorZ;
//					float blockCeilZ = frontCeilZ < backCeilZ ? frontCeilZ : backCeilZ;
//
//					// Linearly calculate the exact vertical Z position of the light ray at (ix, iy) coordinate
//					float distTotal = sqrtf((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
//					if (distTotal > 0.001f)
//					{
//						float distIntersect = sqrtf((ix - x1) * (ix - x1) + (iy - y1) * (iy - y1));
//						float factor = distIntersect / distTotal;
//
//						// Ray absolute height calculation pass
//						float rayZ = z1 + (z2 - z1) * factor;
//
//						// If the ray hits the physical step ledge or lower/upper door frame structure:
//						if (rayZ <= blockFloorZ || rayZ >= blockCeilZ)
//						{
//							return true; // SHADOW TRIGGERED!
//						}
//					}
//				}
//			}
//		}
//	}
//
//	return false; // Ray path is completely pristine! Light illuminates the surface.
//}

//==========================================================================
//
// Check fog in current sector for placing into the proper draw list.
//
//==========================================================================

static bool gl_CheckFog(FColormap *cm, int lightlevel)
{
	bool frontfog;

	PalEntry fogcolor = cm->FadeColor;

	if ((fogcolor.d & 0xffffff) == 0)
	{
		frontfog = false;
	}
	else if (level.outsidefogdensity != 0 && APART(level.info->outsidefog) != 0xff && (fogcolor.d & 0xffffff) == (level.info->outsidefog & 0xffffff))
	{
		frontfog = true;
	}
	else  if (level.fogdensity != 0 || (glset.lightmode & 4) || cm->FogDensity > 0)
	{
		// case 3: level has fog density set
		frontfog = true;
	}
	else
	{
		// case 4: use light level
		frontfog = lightlevel < 248;
	}
	return frontfog;
}

//==========================================================================
//
//
//
//==========================================================================

// -------------------   UNUSED   ------------------------------------------
//bool GLWall::PutWallCompat_original_problematic_unused(int passflag)
//{
//	static int list_indices[2][2] =
//	{ { GLLDL_WALLS_PLAIN, GLLDL_WALLS_FOG },{ GLLDL_WALLS_MASKED, GLLDL_WALLS_FOGMASKED } };
//
//	// are lights possible?
//	if (mDrawer->FixedColormap != CM_DEFAULT || !gl_lights || seg->sidedef == nullptr || type == RENDERWALL_M2SNF || !gltexture) return false;
//
//	// multipassing these is problematic.
//	if ((flags&GLWF_SKYHACK && type == RENDERWALL_M2S)) return false;
//
//	// Any lights affecting this wall?
//	
//	//	// we need to comment out this check because then foggy-dynlight
//	//	// walls unaffected by a huge dynlight start to brighten up entirely while another dynlight affecting it
//	//if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
//	//{
//	//	if (seg->sidedef->lighthead == nullptr) return false;
//	//}
//
//	if (sub)
//	{
//		if (sub->lighthead == nullptr) return false;
//	}
//
//	bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE) || gl_lights_additive;
//	bool masked = passflag == 2 && gltexture->isMasked();
//
//	int list = list_indices[masked][foggy];
//	gl_drawinfo->dldrawlists[list].AddWall(this);
//	return true;
//}
// -------------------   UNUSED   ------------------------------------------

bool GLWall::PutWallCompat(int passflag)
{
	static int list_indices[2][2] =
	{ { GLLDL_WALLS_PLAIN, GLLDL_WALLS_FOG },{ GLLDL_WALLS_MASKED, GLLDL_WALLS_FOGMASKED } };

	// ALLOW M2SNF and M2S to have lights!
	if (mDrawer->FixedColormap != CM_DEFAULT || !gl_lights || seg->sidedef == nullptr || !gltexture) return false;

	// Block ONLY specific Skyhack if it's really a hurdle
	if ((flags & GLWF_SKYHACK) && type == RENDERWALL_M2S) return false;

	// Check if this wall has any lights affecting it
	bool hasLights = false;

	// FORCE: If it's a Mid-Texture, we ALWAYS want it in the list for the multipass
	// This bypasses any logic that might skip it due to "no lights in range"
	if (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF)
	{
		hasLights = true;
	}

	// First check if there are any lights at all
	if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		if (seg->sidedef->lighthead == nullptr)
		{
			// No lights on this wall
			// Only skip lighting passes, not other passes like base or fog
			if (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE)
				return false;
		}
		else
		{
			// Check if any lights are actually within range
			for (FLightNode * ln = seg->sidedef->lighthead; ln; ln = ln->nextLight)
			{
				FDynamicLight * light = ln->lightsource;

				// Get wall vertices
				vertex_t *v1 = seg->sidedef->V1();
				vertex_t *v2 = seg->sidedef->V2();

				// Create a plane from the wall
				// The plane equation is: ax + by + cz + d = 0
				// We need to calculate a, b, c, d from the wall's vertices
				double dx = v2->fX() - v1->fX();
				double dy = v2->fY() - v1->fY();

				// Normal is perpendicular to the wall (in 2D, just rotate 90 degrees)
				double nx = -dy;
				double ny = dx;

				// Normalize the normal vector
				double len = sqrt(nx*nx + ny * ny);
				if (len > 0)
				{
					nx /= len;
					ny /= len;
				}

				// Calculate d using one of the vertices
				double d = -(nx * v1->fX() + ny * v1->fY());

				// Now create a plane with these values
				secplane_t wallPlane;
				wallPlane.set(nx, ny, 0, d); // Set z-component to 0 as we're working in 2D

				// Get light position and calculate distance
				// Initialize group to a default value (0 is usually the default portal group)
				int group = 0;
				DVector3 lpos = light->PosRelative(group);

				// Calculate distance from light to wall plane using the correct method
				// PointToDist returns the actual distance value
				float dist = fabsf(wallPlane.PointToDist(DVector2(lpos.X, lpos.Y), lpos.Z));
				float radius = light->GetRadius();

				// Check if light is within range
				if (dist <= radius)
				{
					hasLights = true;
					break; // Found at least one light that affects this wall
				}
			}

			// If no lights are in range, skip lighting passes
			if (!hasLights && (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE))
				return false;
		}
	}
	else if (sub)
	{
		// Similar logic for polyobj walls
		if (sub->lighthead == nullptr)
		{
			if (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE)
				return false;
		}
		else
		{
			for (FLightNode * ln = sub->lighthead; ln; ln = ln->nextLight)
			{
				FDynamicLight * light = ln->lightsource;

				// For subsectors, we need to find the closest wall
				// This is more complex, so for now we'll just check if any light is in range
				// Initialize group to a default value (0 is usually the default portal group)
				int group = 0;
				DVector3 lpos = light->PosRelative(group);
				float radius = light->GetRadius();

				// Simple check - if light radius overlaps with subsector bounds
				// You might want to make this more precise
				if (fabs(lpos.X - sub->sector->centerspot.X) < radius + 128 &&
					fabs(lpos.Y - sub->sector->centerspot.Y) < radius + 128)
				{
					hasLights = true;
					break;
				}
			}

			if (!hasLights && (passflag == GLPASS_LIGHTTEX || passflag == GLPASS_LIGHTTEX_ADDITIVE))
				return false;
		}
	}

	bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE);
	bool masked = passflag == 2 && gltexture->isMasked();

	int list = list_indices[masked][foggy];
	gl_drawinfo->dldrawlists[list].AddWall(this);
	return true;
}

//==========================================================================
//
//
//
//==========================================================================

bool GLFlat::PutFlatCompat(bool fog)
{
	// Are lights possible?
	if (mDrawer->FixedColormap != CM_DEFAULT || !gl_lights || !gltexture || renderstyle != STYLE_Translucent || 
											alpha < 1.f - FLT_EPSILON || sector->lighthead == NULL) return false;

	static int list_indices[2][2] =
	{ { GLLDL_FLATS_PLAIN, GLLDL_FLATS_FOG },{ GLLDL_FLATS_MASKED, GLLDL_FLATS_FOGMASKED } };

	bool masked = gltexture->isMasked() && ((renderflags&SSRF_RENDER3DPLANES) || stack);
	bool foggy = gl_CheckFog(&Colormap, lightlevel) || (level.flags&LEVEL_HASFADETABLE) || gl_lights_additive;

	int list = list_indices[masked][foggy];
	gl_drawinfo->dldrawlists[list].AddFlat(this);
	return true;
}


//==========================================================================
//
// Fog boundary without any shader support
//
//==========================================================================

void GLWall::RenderFogBoundaryCompat()
{
	// without shaders some approximation is needed. This won't look as good
	// as the shader version but it's an acceptable compromise.
	float fogdensity = gl_GetFogDensity(lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

	float dist1 = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y, glseg.x1, glseg.y1);
	float dist2 = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y, glseg.x2, glseg.y2);

	// these values were determined by trial and error and are scale dependent!
	float fogd1 = (0.95f - exp(-fogdensity * dist1 / 62500.f)) * 1.05f;
	float fogd2 = (0.95f - exp(-fogdensity * dist2 / 62500.f)) * 1.05f;

	float fc[4] = { Colormap.FadeColor.r / 255.0f,Colormap.FadeColor.g / 255.0f,Colormap.FadeColor.b / 255.0f,fogd2 };

	gl_RenderState.EnableTexture(false);
	gl_RenderState.EnableFog(false);
	gl_RenderState.AlphaFunc(GL_GEQUAL, 0);
	gl_RenderState.Apply();
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(-1.0f, -128.0f);
	glDepthFunc(GL_LEQUAL);
	glColor4f(fc[0], fc[1], fc[2], fogd1);
	glBegin(GL_TRIANGLE_FAN);
	glTexCoord2f(tcs[LOLFT].u, tcs[LOLFT].v);
	glVertex3f(glseg.x1, zbottom[0], glseg.y1);
	glTexCoord2f(tcs[UPLFT].u, tcs[UPLFT].v);
	glVertex3f(glseg.x1, ztop[0], glseg.y1);
	glColor4f(fc[0], fc[1], fc[2], fogd2);
	glTexCoord2f(tcs[UPRGT].u, tcs[UPRGT].v);
	glVertex3f(glseg.x2, ztop[1], glseg.y2);
	glTexCoord2f(tcs[LORGT].u, tcs[LORGT].v);
	glVertex3f(glseg.x2, zbottom[1], glseg.y2);
	glEnd();
	glDepthFunc(GL_LESS);
	glPolygonOffset(0.0f, 0.0f);
	glDisable(GL_POLYGON_OFFSET_FILL);
	gl_RenderState.EnableFog(true);
	gl_RenderState.AlphaFunc(GL_GEQUAL, 0.5f);
	gl_RenderState.EnableTexture(true);
}

//==========================================================================
//
// Flats 
//
//==========================================================================

// Doesn't need to boost dynlight overbright intensity unlike walls
void GLFlat::DrawSubsectorLights(subsector_t * sub, int pass)
{
	Plane p;
	FVector3 nearPt, up, right, t1;
	float scale;

	FLightNode * node = sub->lighthead;
	while (node)
	{
		FDynamicLight * light = node->lightsource;

		//	// No reason to draw it like usual if we bake huge radius lights
		//if (gl_legacy_dynlight_baked_huge && light->GetRadius() >= 512.0f)
		//{
		//	if (pass == GLPASS_LIGHTTEX || pass == GLPASS_LIGHTTEX_ADDITIVE || pass == GLPASS_LIGHTTEX_FOGGY)
		//	{
		//		node = node->nextLight;
		//		continue; // Skip standard multi-pass accumulation to avoid double lighting math!
		//	}
		//}

		if (!(light->IsActive()) ||
			(pass == GLPASS_LIGHTTEX && light->IsAdditive()) ||
			(pass == GLPASS_LIGHTTEX_ADDITIVE && !light->IsAdditive()))
		{
			node = node->nextLight;
			continue;
		}

		// We must do the side check here because gl_SetupLight needs the correct plane orientation
		// which we don't have for Legacy-style 3D-floors
		float planeh = plane.plane.ZatPoint(light->Pos);
		if (gl_lights_checkside && ((planeh < light->Z() && ceiling) || (planeh > light->Z() && !ceiling)))
		{
			node = node->nextLight;
			continue;
		}

		p.Set(plane.plane);
		if (!gl_SetupLightFlat(sub->sector->PortalGroup, p, light, nearPt, up, right, scale, false, pass != GLPASS_LIGHTTEX))
		{
			node = node->nextLight;
			continue;
		}

		// --- FIX: OVERRIDE FORCED DARKNESS WITH CVAR INTENSITY CONTROL FOR BRIGHTEN PASS ---
		if (pass == GLPASS_BRIGHTEN_LEGACY_LIGHTTEX)
		{
			// Safe bounds check for the external overbright intensity CVAR
			float overbrightFactor = clamp((float)gl_legacy_dynlight_overbright_flats, 0.0f, 0.2f);
			if (overbrightFactor > 1.0f) overbrightFactor = 1.0f;
			if (overbrightFactor < 0.0f) overbrightFactor = 0.0f;

			// Force standard additive blending and inject your custom brightness scale factor
			gl_RenderState.BlendEquation(GL_FUNC_ADD);
			gl_RenderState.SetColor(overbrightFactor, overbrightFactor, overbrightFactor, 1.0f);
			gl_RenderState.SetObjectColor(0xffffffff);
		}

		gl_RenderState.Apply();

		FFlatVertex *ptr = GLRenderer->mVBO->GetBuffer();
		FFlatVertex *startPtr = ptr; // Keep a pointer to the start of the fan polygon for the second pass

		for (unsigned int k = 0; k < sub->numlines; k++)
		{
			vertex_t *vt = sub->firstline[k].v1;
			ptr->x = vt->fX();
			ptr->z = plane.plane.ZatPoint(vt) + dz;
			ptr->y = vt->fY();
			t1 = { ptr->x, ptr->z, ptr->y };
			FVector3 nearToVert = t1 - nearPt;

			// --- FIX: LINEAR COMPENSATED TEXTURE PROJECTION FOR GLPASS_BRIGHTEN_LEGACY_LIGHTTEX ---
			if (pass == GLPASS_BRIGHTEN_LEGACY_LIGHTTEX)
			{
				float radius = light->GetRadius();
				if (radius <= 0.0f) radius = 1.0f;

				// Core embedded float constants (Scale = 2.0f, Offset = 0.0f)
				// Hardcoded directly inside the function to completely replace old CVARs
				const float staticScaleX = 2.0f;
				const float staticScaleY = 2.0f;
				const float staticOffsetX = 0.0f;
				const float staticOffsetY = 0.0f;

				float customScaleX = 1.0f / (radius * staticScaleX);
				float customScaleY = 1.0f / (radius * staticScaleY);

				// Map vertices pixel-to-pixel relative to PivotPoint nearPt using perfect 0.5f base layout centring
				ptr->u = ((nearToVert | right) * customScaleX) + 0.5f + staticOffsetX;
				ptr->v = ((nearToVert | up) * customScaleY) + 0.5f + staticOffsetY;
			}
			else
			{
				// Keep native Graf Zahl texture space projection mapping for standard passes
				ptr->u = ((nearToVert | right) * scale) + 0.5f;
				ptr->v = ((nearToVert | up) * scale) + 0.5f;
			}
			ptr++;
		}

		// FIRST PASS: Multiplies existing floor texture by the projected light mask shape
		GLRenderer->mVBO->RenderCurrent(ptr, GL_TRIANGLE_FAN);

		// FIXED OVERBRIGHT MULTI-PASS BOOSTER: If we are in the brightening pass, 
		// we immediately push the exact same VBO fan buffer onto the GPU a SECOND time!
		// This guarantees that the overbright intensity stacks per-each-light, perfectly matching the walls!
		if (pass == GLPASS_BRIGHTEN_LEGACY_LIGHTTEX)
		{
			GLRenderer->mVBO->RenderCurrent(startPtr, GL_TRIANGLE_FAN);
		}

		node = node->nextLight;
	}
}

//==========================================================================
//
//
//
//==========================================================================

void GLFlat::DrawLightsCompat(int pass)
{
	// Set fog and global coloring for this pass
	if (pass == GLPASS_BRIGHTEN_LEGACY_LIGHTTEX || pass == GLPASS_LIGHTTEX_ADDITIVE)
	{
		// Force-disable fixed hardware fog to fully eliminate the lipstick effect
		gl_RenderState.EnableFog(false);

		// Force vertices to pure white to avoid colored grease lines on floors/ceilings
		gl_RenderState.SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		gl_RenderState.SetObjectColor(0xffffffff);
	}
	else
	{
		mDrawer->SetFog(lightlevel, gl_GetFogDensity(lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor), &Colormap, true);
	}

	// Draw the subsectors belonging to this sector
	for (int i = 0; i < sector->subsectorcount; i++)
	{
		subsector_t * sub = sector->subsectors[i];
		if (sub)
		{
			// Bypass ss_renderflags filter completely for our overbright pass!
			if (pass == GLPASS_BRIGHTEN_LEGACY_LIGHTTEX || (gl_drawinfo->ss_renderflags[sub->Index()] & renderflags))
			{
				DrawSubsectorLights(sub, pass);
			}
		}
	}

	// Draw the subsectors assigned to it due to missing textures / render hacks
	if (!(renderflags & SSRF_RENDER3DPLANES))
	{
		gl_subsectorrendernode * node = (renderflags & SSRF_RENDERFLOOR) ?
			gl_drawinfo->GetOtherFloorPlanes(sector->sectornum) :
			gl_drawinfo->GetOtherCeilingPlanes(sector->sectornum);

		while (node)
		{
			if (node->sub)
			{
				DrawSubsectorLights(node->sub, pass);
			}
			node = node->next;
		}
	}
}

//==========================================================================
//
// Sets up the texture coordinates for one light to be rendered
//
//==========================================================================
bool GLWall::PrepareLight(FDynamicLight * light, int pass)
{
	float vtx[] = { glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2 };
	Plane p;
	FVector3 nearPt, up, right;
	float scale;

	p.Set(&glseg);

	// TO DO: this causes some walls to not receieve dynlights at all, try to fix gl_geometric.h and gl_wall.h
	//if (!p.ValidNormal())
	//{
	//	return false;
	//}

	//	// No reason to draw it like usual if we bake huge radius lights
	//if (gl_legacy_dynlight_baked_huge && light->GetRadius() >= 512.0f)
	//{
	//	return false;
	//}

	if (!gl_SetupLightWall(seg->frontsector->PortalGroup, p, light, nearPt, up, right, scale, true, pass != GLPASS_LIGHTTEX))
	{
		return false;
	}

	FVector3 t1;
	int outcnt[4] = { 0,0,0,0 };

	// This sets the coordinates to project a dynlight texture onto a polygon
	for (int i = 0; i < 4; i++)
	{
		t1 = &vtx[i * 3];
		FVector3 nearToVert = t1 - nearPt;
		tcs[i].u = ((nearToVert | right) * scale) + 0.5f;
		tcs[i].v = ((nearToVert | up) * scale) + 0.5f;

		// quick check whether the light touches this polygon
		if (tcs[i].u < 0) outcnt[0]++;
		if (tcs[i].u > 1) outcnt[1]++;
		if (tcs[i].v < 0) outcnt[2]++;
		if (tcs[i].v > 1) outcnt[3]++;

	}
	// The light doesn't touch this polygon
	if (outcnt[0] == 4 || outcnt[1] == 4 || outcnt[2] == 4 || outcnt[3] == 4) return false;

	draw_dlight++;
	return true;
}


void GLWall::RenderLightsCompat(int pass)
{

	FLightNode * node;

	// black fog is diminishing light and should affect lights less than the rest!
	if (pass == GLPASS_LIGHTTEX) mDrawer->SetFog((255 + lightlevel) >> 1, 0, NULL, false);
	else mDrawer->SetFog(lightlevel, 0, &Colormap, true);

	if (seg->sidedef == NULL)
	{
		return;
	}
	else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		// Iterate through all dynamic lights which touch this wall and render them
		node = seg->sidedef->lighthead;
	}
	else if (sub)
	{
		// To avoid constant rechecking for polyobjects use the subsector's lightlist instead
		node = sub->lighthead;
	}
	else
	{
		return;
	}

	texcoord save[4];
	memcpy(save, tcs, sizeof(tcs));
	while (node)
	{
		FDynamicLight * light = node->lightsource;

		if (!(light->IsActive()) ||
			(pass == GLPASS_LIGHTTEX && light->IsAdditive()) ||
			(pass == GLPASS_LIGHTTEX_ADDITIVE && !light->IsAdditive()))
		{
			node = node->nextLight;
			continue;
		}
		if (PrepareLight(light, pass))
		{
			vertcount = 0;
			RenderWall(RWF_TEXTURED);
		}
		node = node->nextLight;
	}
	memcpy(tcs, save, sizeof(tcs));
	vertcount = 0;
}

//==========================================================================
//
//
//
//==========================================================================

void GLSceneDrawer::RenderMultipassStuff()
{
	// First pass: empty background with sector light only

	// Part 1: solid geometry
	gl_RenderState.EnableTexture(false);
	gl_RenderState.EnableBrightmap(false);
	gl_RenderState.Apply();
	gl_drawinfo->dldrawlists[GLLDL_WALLS_PLAIN].DrawWalls(GLPASS_PLAIN);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_PLAIN].DrawFlats(GLPASS_PLAIN);

	// Part 2: masked geometry
	gl_RenderState.EnableTexture(true);
	gl_RenderState.SetTextureMode(TM_MASK);
	gl_RenderState.EnableBrightmap(true);
	gl_RenderState.AlphaFunc(GL_GEQUAL, gl_mask_threshold);
	gl_drawinfo->dldrawlists[GLLDL_WALLS_MASKED].DrawWalls(GLPASS_PLAIN);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_MASKED].DrawFlats(GLPASS_PLAIN);

	// Part 3: The base of fogged surfaces (without fixed fog)
	gl_RenderState.EnableFog(false);  // Disable fixed fog for foggy surfaces
	gl_RenderState.EnableBrightmap(false);
	gl_RenderState.SetTextureMode(TM_MODULATE);
	gl_RenderState.AlphaFunc(GL_GEQUAL, 0);
	gl_drawinfo->dldrawlists[GLLDL_WALLS_FOG].DrawWalls(GLPASS_PLAIN);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_FOG].DrawFlats(GLPASS_PLAIN);
	gl_RenderState.AlphaFunc(GL_GEQUAL, gl_mask_threshold);
	gl_drawinfo->dldrawlists[GLLDL_WALLS_FOGMASKED].DrawWalls(GLPASS_PLAIN);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_FOGMASKED].DrawFlats(GLPASS_PLAIN);

	// Second pass: draw lights (including foggy surfaces)
	glDepthMask(false);
	if (GLRenderer->mLightCount && !FixedColormap)
	{
		if (gl_SetupLightTexture())
		{
			gl_RenderState.BlendFunc(GL_ONE, GL_ONE);
			glDepthFunc(GL_EQUAL);
			if (glset.lightmode >= 8) gl_RenderState.SetSoftLightLevel(255);

			// Apply regular lights to ALL surfaces
			gl_drawinfo->dldrawlists[GLLDL_WALLS_PLAIN].DrawWalls(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_WALLS_MASKED].DrawWalls(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_WALLS_FOG].DrawWalls(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_WALLS_FOGMASKED].DrawWalls(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_PLAIN].DrawFlats(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_MASKED].DrawFlats(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_FOG].DrawFlats(GLPASS_LIGHTTEX);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_FOGMASKED].DrawFlats(GLPASS_LIGHTTEX);

			gl_RenderState.BlendEquation(GL_FUNC_ADD);
		}
	}

	// Third pass: modulated texture (all surfaces)
	gl_RenderState.SetColor(0xffffffff);
	gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ZERO);
	gl_RenderState.EnableFog(false);  // Keep fog disabled
	gl_RenderState.AlphaFunc(GL_GEQUAL, 0);
	// Dynlights in front of a transparent txt make all black behind it
	glDepthFunc(GL_EQUAL);  // CRITICAL: Use GL_EQUAL instead of GL_LEQUAL
	gl_drawinfo->dldrawlists[GLLDL_WALLS_PLAIN].DrawWalls(GLPASS_TEXONLY);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_PLAIN].DrawFlats(GLPASS_TEXONLY);
	gl_RenderState.AlphaFunc(GL_GREATER, gl_mask_threshold);
	gl_drawinfo->dldrawlists[GLLDL_WALLS_MASKED].DrawWalls(GLPASS_TEXONLY);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_MASKED].DrawFlats(GLPASS_TEXONLY);
	gl_RenderState.AlphaFunc(GL_GEQUAL, 0);
	gl_drawinfo->dldrawlists[GLLDL_WALLS_FOG].DrawWalls(GLPASS_TEXONLY);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_FOG].DrawFlats(GLPASS_TEXONLY);
	gl_RenderState.AlphaFunc(GL_GREATER, gl_mask_threshold);
	gl_drawinfo->dldrawlists[GLLDL_WALLS_FOGMASKED].DrawWalls(GLPASS_TEXONLY);
	gl_drawinfo->dldrawlists[GLLDL_FLATS_FOGMASKED].DrawFlats(GLPASS_TEXONLY);

	// Fourth pass: apply fog as translucent overlay for foggy surfaces
	gl_RenderState.EnableFog(false);  // Ensure fog is disabled
	gl_RenderState.BlendFunc(GL_ONE, GL_ONE); // must be a faster way to do it
	//gl_RenderState.BlendFunc(GL_SRC_ALPHA, 1); // another way
	glDepthMask(false);

	// Fifth pass: apply fog overlay to foggy surfaces
	if (gl_drawinfo->dldrawlists[GLLDL_WALLS_FOG].drawitems.Size() > 0)
	{
		gl_drawinfo->dldrawlists[GLLDL_WALLS_FOG].DrawWalls(GLPASS_PLAIN);
	}
	if (gl_drawinfo->dldrawlists[GLLDL_FLATS_FOG].drawitems.Size() > 0)
	{
		gl_drawinfo->dldrawlists[GLLDL_FLATS_FOG].DrawFlats(GLPASS_PLAIN);
	}
	if (gl_drawinfo->dldrawlists[GLLDL_WALLS_FOGMASKED].drawitems.Size() > 0)
	{
		gl_drawinfo->dldrawlists[GLLDL_WALLS_FOGMASKED].DrawWalls(GLPASS_PLAIN);
	}
	if (gl_drawinfo->dldrawlists[GLLDL_FLATS_FOGMASKED].drawitems.Size() > 0)
	{
		gl_drawinfo->dldrawlists[GLLDL_FLATS_FOGMASKED].DrawFlats(GLPASS_PLAIN);
	}

	// Sixth pass: there are still special dynamic lights that are SET to be ADDITIVE
	// These lights must be rendered AFTER fog to remain bright and visible
	if (GLRenderer->mLightCount && !FixedColormap)
	{
		if (gl_SetupLightTexture())
		{
			gl_RenderState.EnableFog(true); // Let additive lights blend with fog color if needed
			gl_RenderState.BlendFunc(GL_ONE, GL_ONE);
			glDepthFunc(GL_EQUAL);
			glDepthMask(false);

			gl_drawinfo->dldrawlists[GLLDL_WALLS_PLAIN].DrawWalls(GLPASS_LIGHTTEX_ADDITIVE);
			gl_drawinfo->dldrawlists[GLLDL_WALLS_MASKED].DrawWalls(GLPASS_LIGHTTEX_ADDITIVE);
			gl_drawinfo->dldrawlists[GLLDL_WALLS_FOG].DrawWalls(GLPASS_LIGHTTEX_ADDITIVE);
			gl_drawinfo->dldrawlists[GLLDL_WALLS_FOGMASKED].DrawWalls(GLPASS_LIGHTTEX_ADDITIVE);

			gl_drawinfo->dldrawlists[GLLDL_FLATS_PLAIN].DrawFlats(GLPASS_LIGHTTEX_ADDITIVE);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_MASKED].DrawFlats(GLPASS_LIGHTTEX_ADDITIVE);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_FOG].DrawFlats(GLPASS_LIGHTTEX_ADDITIVE);
			gl_drawinfo->dldrawlists[GLLDL_FLATS_FOGMASKED].DrawFlats(GLPASS_LIGHTTEX_ADDITIVE);
		}
	}

	glDepthMask(true);

	// Cleanup
	glDepthFunc(GL_LESS);
	gl_RenderState.AlphaFunc(GL_GEQUAL, 0.f);
	gl_RenderState.EnableFog(true);
	gl_RenderState.BlendFunc(GL_ONE, GL_ZERO);
	glDepthMask(true);
}