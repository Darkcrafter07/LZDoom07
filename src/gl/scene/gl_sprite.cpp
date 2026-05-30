// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
//
// Anamorphic Forced-Perspective by Rachael Alexanderson, (C) 2025
//
// Anamorphic Forced-Perspective+ and Hybrid Forced-Perspective
// with occlusion culling code by Vadim Taranov (Darkcrafter07), (C)2025-2026
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
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


// gl_sprite.cpp
// Sprite/Particle rendering

#include "gl/system/gl_system.h"
#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "doomstat.h"
#include "gl/gl_functions.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "a_pickups.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "r_data/r_vanillatrans.h"
#include "i_time.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_scenedrawer.h"
#include "gl/scene/gl_portal.h"
#include "gl/models/gl_models.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/renderer/gl_quaddrawer.h"

#include "g_shared/a_spriteocclusion3d.h"

CVAR(Bool, gl_usecolorblending, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_sprite_blend, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_spriteclip, 1, CVAR_ARCHIVE)
CVAR(Float, gl_sclipthreshold, 10.0, CVAR_ARCHIVE)
CVAR(Float, gl_sclipfactor, 1.8, CVAR_ARCHIVE)
CVAR(Int, gl_particles_style, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // 0 = square, 1 = round, 2 = smooth
CVAR(Int, gl_billboard_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_faces_camera, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_billboard_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_enhanced_nv_stealth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, r_spriteclipanamorphicminbias, 0.6, CVAR_ARCHIVE)
CVAR(Bool, r_debug_nolimitanamorphoses, false, 0)
CUSTOM_CVAR(Int, gl_fuzztype, 6, CVAR_ARCHIVE)
{
	if (self < 0 || self > 8) self = 0;
}

EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Bool, r_debug_disable_vis_filter)

extern TArray<spritedef_t> sprites;
extern TArray<spriteframe_t> SpriteFrames;
extern uint32_t r_renderercaps;
extern int modellightindex;

enum HWRenderStyle
{
	STYLEHW_Normal,			// default
	STYLEHW_Solid,			// drawn solid (needs special treatment for sprites)
	STYLEHW_NoAlphaTest,	// disable alpha test
};


void gl_SetRenderStyle(FRenderStyle style, bool drawopaque, bool allowcolorblending)
{
	int tm, sb, db, be;

	gl_GetRenderStyle(style, drawopaque, allowcolorblending, &tm, &sb, &db, &be);
	gl_RenderState.BlendEquation(be);
	gl_RenderState.BlendFunc(sb, db);
	gl_RenderState.SetTextureMode(tm);
}

CVAR(Bool, gl_nolayer, false, 0)

static const float LARGE_VALUE = 1e19f;


//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::CalculateVertices(FVector3 *v)
{
	if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
	{
		Matrix3x4 mat;
		mat.MakeIdentity();

		// [MC] Rotate around the center or offsets given to the sprites.
		// Counteract any existing rotations, then rotate the angle.
		// Tilt the actor up or down based on pitch (increase 'somersaults' forward).
		// Then counteract the roll and DO A BARREL ROLL.

		FAngle pitch = (float)-Angles.Pitch.Degrees;
		pitch.Normalized180();

		mat.Translate(x, z, y);
		mat.Rotate(0, 1, 0, 270. - Angles.Yaw.Degrees);
		mat.Rotate(1, 0, 0, pitch.Degrees);

		if (actor->renderflags & RF_ROLLCENTER)
		{
			float cx = (x1 + x2) * 0.5;
			float cy = (y1 + y2) * 0.5;

			mat.Translate(cx - x, 0, cy - y);
			mat.Rotate(0, 1, 0, -Angles.Roll.Degrees);
			mat.Translate(-cx, -z, -cy);
		}
		else
		{
			mat.Rotate(0, 1, 0, -Angles.Roll.Degrees);
			mat.Translate(-x, -z, -y);
		}
		v[0] = mat * FVector3(x2, z, y2);
		v[1] = mat * FVector3(x1, z, y2);
		v[2] = mat * FVector3(x2, z, y1);
		v[3] = mat * FVector3(x1, z, y1);

		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -128.0f);
		return;
	}

	// [BB] Billboard stuff
	const bool drawWithXYBillboard = ((particle && gl_billboard_particles) || (!(actor && actor->renderflags & RF_FORCEYBILLBOARD)
		//&& GLRenderer->mViewActor != NULL
		&& (gl_billboard_mode == 1 || (actor && actor->renderflags & RF_FORCEXYBILLBOARD))));

	const bool drawBillboardFacingCamera = gl_billboard_faces_camera;
	// [Nash] has +ROLLSPRITE
	const bool drawRollSpriteActor = (actor != nullptr && actor->renderflags & RF_ROLLSPRITE);


	// [fgsfds] check sprite type mask
	uint32_t spritetype = (uint32_t)-1;
	if (actor != nullptr) spritetype = actor->renderflags & RF_SPRITETYPEMASK;

	// [Nash] is a flat sprite
	const bool isFlatSprite = (actor != nullptr) && (spritetype == RF_WALLSPRITE || spritetype == RF_FLATSPRITE);
	const bool useOffsets = (actor != nullptr) && !(actor->renderflags & RF_ROLLCENTER);

	// [Nash] check for special sprite drawing modes
	if (drawWithXYBillboard || drawBillboardFacingCamera || drawRollSpriteActor || isFlatSprite)
	{
		// Compute center of sprite
		float xcenter = (x1 + x2)*0.5;
		float ycenter = (y1 + y2)*0.5;
		float zcenter = (z1 + z2)*0.5;
		float xx = -xcenter + x;
		float zz = -zcenter + z;
		float yy = -ycenter + y;
		Matrix3x4 mat;
		mat.MakeIdentity();
		mat.Translate(xcenter, zcenter, ycenter); // move to sprite center

												  // Order of rotations matters. Perform yaw rotation (Y, face camera) before pitch (X, tilt up/down).
		if (drawBillboardFacingCamera && !isFlatSprite)
		{
			// [CMB] Rotate relative to camera XY position, not just camera direction,
			// which is nicer in VR
			float xrel = xcenter - r_viewpoint.Pos.X;
			float yrel = ycenter - r_viewpoint.Pos.Y;
			float absAngleDeg = RAD2DEG(atan2(-yrel, xrel));
			float counterRotationDeg = 270. - GLRenderer->mAngles.Yaw.Degrees; // counteracts existing sprite rotation
			float relAngleDeg = counterRotationDeg + absAngleDeg;

			mat.Rotate(0, 1, 0, relAngleDeg);
		}

		// [fgsfds] calculate yaw vectors
		float yawvecX = 0, yawvecY = 0, rollDegrees = 0;
		float angleRad = (270. - GLRenderer->mAngles.Yaw).Radians();
		if (actor)	rollDegrees = Angles.Roll.Degrees;
		if (isFlatSprite)
		{
			yawvecX = Angles.Yaw.Cos();
			yawvecY = Angles.Yaw.Sin();
		}

		// [fgsfds] Rotate the sprite about the sight vector (roll) 
		if (spritetype == RF_WALLSPRITE)
		{
			mat.Rotate(0, 1, 0, 0);
			if (drawRollSpriteActor)
			{
				if (useOffsets)	mat.Translate(xx, zz, yy);
				mat.Rotate(yawvecX, 0, yawvecY, rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
			}
		}
		else if (drawRollSpriteActor)
		{
			if (useOffsets) mat.Translate(xx, zz, yy);
			if (drawWithXYBillboard)
			{
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -GLRenderer->mAngles.Pitch.Degrees);
			}
			mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
			if (useOffsets) mat.Translate(-xx, -zz, -yy);
		}
		else if (drawWithXYBillboard)
		{
			// Rotate the sprite about the vector starting at the center of the sprite
			// triangle strip and with direction orthogonal to where the player is looking
			// in the x/y plane.
			mat.Rotate(-sin(angleRad), 0, cos(angleRad), -GLRenderer->mAngles.Pitch.Degrees);
		}

		mat.Translate(-xcenter, -zcenter, -ycenter); // retreat from sprite center
		v[0] = mat * FVector3(x1, z1, y1);
		v[1] = mat * FVector3(x2, z1, y2);
		v[2] = mat * FVector3(x1, z2, y1);
		v[3] = mat * FVector3(x2, z2, y2);
	}
	else // traditional "Y" billboard mode
	{
		v[0] = FVector3(x1, z1, y1);
		v[1] = FVector3(x2, z1, y2);
		v[2] = FVector3(x1, z2, y1);
		v[3] = FVector3(x2, z2, y2);
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::Draw(int pass)
{
	if (pass == GLPASS_DECALS) return;

	if (pass == GLPASS_LIGHTSONLY)
	{
		if (modelframe && !modelframe->isVoxel && !(modelframe->flags & MDL_NOPERPIXELLIGHTING))
		{
			if (RenderStyle.BlendOp != STYLEOP_Shadow)
			{
				if (gl_lights && GLRenderer->mLightCount && mDrawer->FixedColormap == CM_DEFAULT && !fullbright)
				{
					if (!particle)
					{
						dynlightindex = gl_SetDynModelLight(gl_light_sprites ? actor : nullptr, -1);
					}
				}
			}
		}
		return;
	}

	bool additivefog = false;
	bool foglayer = false;
	int rel = fullbright ? 0 : getExtraLight();

	if (pass == GLPASS_TRANSLUCENT)
	{
		// The translucent pass requires special setup for the various modes.

		// for special render styles brightmaps would not look good - especially for subtractive.
		if (RenderStyle.BlendOp != STYLEOP_Add)
		{
			gl_RenderState.EnableBrightmap(false);
		}

		gl_SetRenderStyle(RenderStyle, false,
			// The rest of the needed checks are done inside gl_SetRenderStyle
			trans > 1.f - FLT_EPSILON && gl_usecolorblending && mDrawer->FixedColormap == CM_DEFAULT && actor &&
			fullbright && gltexture && !gltexture->GetTransparent());

		if (hw_styleflags == STYLEHW_NoAlphaTest)
		{
			gl_RenderState.AlphaFunc(GL_GEQUAL, 0.f);
		}
		else if (!gltexture || !gltexture->GetTransparent()) gl_RenderState.AlphaFunc(GL_GEQUAL, gl_mask_sprite_threshold);
		else gl_RenderState.AlphaFunc(GL_GREATER, 0.f);

		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			float fuzzalpha = 0.44f;
			float minalpha = 0.1f;

			// fog + fuzz don't work well without some fiddling with the alpha value!
			if (!gl_isBlack(Colormap.FadeColor))
			{
				float dist = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y, x, y);
				int fogd = gl_GetFogDensity(lightlevel, Colormap.FadeColor, Colormap.FogDensity, Colormap.BlendFactor);

				// this value was determined by trial and error and is scale dependent!
				float factor = 0.05f + exp(-fogd * dist / 62500.f);
				fuzzalpha *= factor;
				minalpha *= factor;
			}

			gl_RenderState.AlphaFunc(GL_GEQUAL, gl_mask_sprite_threshold);
			gl_RenderState.SetColor(0.2f, 0.2f, 0.2f, fuzzalpha, Colormap.Desaturation);
			additivefog = true;
			lightlist = nullptr;	// the fuzz effect does not use the sector's light level so splitting is not needed.
		}
		else if (RenderStyle.BlendOp == STYLEOP_Add && RenderStyle.DestAlpha == STYLEALPHA_One)
		{
			additivefog = true;
		}
	}
	else if (modelframe == nullptr)
	{
		int tm, sb, db, be;

		// This still needs to set the texture mode. As blend mode it will always use GL_ONE/GL_ZERO
		gl_GetRenderStyle(RenderStyle, false, false, &tm, &sb, &db, &be);
		gl_RenderState.SetTextureMode(tm);

		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -128.0f);
	}
	if (RenderStyle.BlendOp != STYLEOP_Shadow)
	{
		if (gl_lights && GLRenderer->mLightCount && mDrawer->FixedColormap == CM_DEFAULT && !fullbright)
		{
			if (modelframe && !particle)
				dynlightindex = gl_SetDynModelLight(gl_light_sprites ? actor : NULL, dynlightindex);
			else
				gl_SetDynSpriteLight(gl_light_sprites ? actor : NULL, gl_light_particles ? particle : NULL);
		}
		sector_t *cursec = actor ? actor->Sector : particle ? particle->subsector->sector : nullptr;
		if (cursec != nullptr)
		{
			const PalEntry finalcol = fullbright
				? ThingColor
				: ThingColor.Modulate(cursec->SpecialColors[sector_t::sprites]);

			gl_RenderState.SetObjectColor(finalcol);
			gl_RenderState.SetAddColor(cursec->AdditiveColors[sector_t::sprites] | 0xff000000);
		}
		mDrawer->SetColor(lightlevel, rel, Colormap, trans);
	}


	if (gl_isBlack(Colormap.FadeColor)) foglevel = lightlevel;

	if (RenderStyle.Flags & STYLEF_FadeToBlack)
	{
		Colormap.FadeColor = 0;
		additivefog = true;
	}

	if (RenderStyle.BlendOp == STYLEOP_RevSub || RenderStyle.BlendOp == STYLEOP_Sub)
	{
		if (!modelframe)
		{
			// non-black fog with subtractive style needs special treatment
			if (!gl_isBlack(Colormap.FadeColor))
			{
				foglayer = true;
				// Due to the two-layer approach we need to force an alpha test that lets everything pass
				gl_RenderState.AlphaFunc(GL_GREATER, 0);
			}
		}
		else RenderStyle.BlendOp = STYLEOP_Fuzz;	// subtractive with models is not going to work.
	}

	if (!foglayer) mDrawer->SetFog(foglevel, rel, &Colormap, additivefog);
	else
	{
		gl_RenderState.EnableFog(false);
		gl_RenderState.SetFog(0, 0);
	}

	if (gltexture) gl_RenderState.SetMaterial(gltexture, CLAMP_XY, translation, OverrideShader, !!(RenderStyle.Flags & STYLEF_RedIsAlpha));
	else if (!modelframe) gl_RenderState.EnableTexture(false);

	//mDrawer->SetColor(lightlevel, rel, Colormap, trans);

	unsigned int iter = lightlist ? lightlist->Size() : 1;
	bool clipping = false;
	if (lightlist || topclip != LARGE_VALUE || bottomclip != -LARGE_VALUE)
	{
		clipping = true;
		gl_RenderState.EnableSplit(true);
	}

	secplane_t bottomp = { { 0, 0, -1. }, bottomclip, 1. };
	secplane_t topp = { { 0, 0, -1. }, topclip, 1. };
	for (unsigned i = 0; i < iter; i++)
	{
		if (lightlist)
		{
			// set up the light slice
			secplane_t *topplane = i == 0 ? &topp : &(*lightlist)[i].plane;
			secplane_t *lowplane = i == (*lightlist).Size() - 1 ? &bottomp : &(*lightlist)[i + 1].plane;

			int thislight = (*lightlist)[i].caster != NULL ? gl_ClampLight(*(*lightlist)[i].p_lightlevel) : lightlevel;
			int thisll = actor == nullptr ? thislight : (uint8_t)gl_CheckSpriteGlow(actor->Sector, thislight, actor->InterpolatedPosition(r_viewpoint.TicFrac));

			FColormap thiscm;
			thiscm.CopyFog(Colormap);
			thiscm.CopyFrom3DLight(&(*lightlist)[i]);
			if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				thiscm.Decolorize();
			}

			mDrawer->SetColor(thisll, rel, thiscm, trans);
			if (!foglayer)
			{
				mDrawer->SetFog(thislight, rel, &thiscm, additivefog);
			}
			gl_RenderState.SetSplitPlanes(*topplane, *lowplane);
		}
		else if (clipping)
		{
			gl_RenderState.SetSplitPlanes(topp, bottomp);
		}

		if (!modelframe)
		{
			gl_RenderState.Apply();

			FVector3 v[4];
			gl_RenderState.SetNormal(0, 0, 0);
			CalculateVertices(v);

			FShader *sh = GLRenderer->mShaderManager->GetActiveShader();
			if (gl_spriteclip == -1 || gl_spriteclip == -2 )
			{
				if (sh) sh->muAnamorphicZ.Set(nonanam_z1, nonanam_z2);
			}
			else
			{
				if (sh) sh->muAnamorphicZ.Set(0.0f, 0.0f);
			}

			FQuadDrawer qd;
			qd.Set(0, v[0][0], v[0][1], v[0][2], ul, vt);
			qd.Set(1, v[1][0], v[1][1], v[1][2], ur, vt);
			qd.Set(2, v[2][0], v[2][1], v[2][2], ul, vb);
			qd.Set(3, v[3][0], v[3][1], v[3][2], ur, vb);
			qd.Render(GL_TRIANGLE_STRIP);

			// as soon as sprite is drawn, reset brightness vars
			// so it doesn't leak to other stuff like walls etc...
			if (sh) sh->muAnamorphicZ.Set(0.0f, 0.0f);

			// --- Legacy GL1x/GL2x sprites brightmaps block start ---
			if (gl_RenderState.IsBrightmapEnabled() && gl.legacyMode && !fullbright)
			{
				FMaterial *bm = gltexture->GetBrightmapLegacy();
				if (bm)
				{
					// 1. Setup brigtmaps state
					gl_RenderState.EnableFog(false);
					gl_RenderState.BlendFunc(GL_ONE, GL_ONE); // Additive
					//gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ONE); // Multiplicative
					gl_RenderState.SetTextureMode(TM_BRIGHTMAP_LEGACY);

					// 2. Bind mask. Pass the same parameters that we do for the base texture
					gl_RenderState.SetMaterial(bm, CLAMP_XY, translation, OverrideShader, !!(RenderStyle.Flags & STYLEF_RedIsAlpha));

					// The brightmap effect remains at full strength until lightlevel >= 128.
					// After that, it linearly decreases to 15% at lightlevel 255.

					float lightFactor;
					if (lightlevel < 128)
					{
						// Full effect below 128
						lightFactor = 1.0f; 
					}
					else
					{
						// Linear decrease from 1.0 (at 128) to 0.15 (at 255)
						lightFactor = 1.0f - (float)(lightlevel - 128) / (255.0f - 128.0f);
						lightFactor = 0.15f + 0.85f * lightFactor;  // Ensure minimum 15% glow
					}

					int intensity = (int)(255 * lightFactor);
					mDrawer->SetColor(intensity, rel, Colormap, trans);

					gl_RenderState.Apply();

					// 3. Draw on the same "v" vertices
					qd.Render(GL_TRIANGLE_STRIP);

					// 4. Restore everything back for the next cycle step (or foglayer)
					gl_RenderState.SetTextureMode(TM_MODULATE);
					gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					gl_RenderState.EnableFog(!foglayer);
					// Return the base material not to break the logic below
					gl_RenderState.SetMaterial(gltexture, CLAMP_XY, translation, OverrideShader, !!(RenderStyle.Flags & STYLEF_RedIsAlpha));
				}
			}
			// --- Legacy GL1x/GL2x sprites brightmaps block finish ---

			if (foglayer)
			{
				// If we get here we know that we have colored fog and no fixed colormap.
				mDrawer->SetFog(foglevel, rel, &Colormap, additivefog);
				gl_RenderState.SetFixedColormap(CM_FOGLAYER);
				gl_RenderState.BlendEquation(GL_FUNC_ADD);
				gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				gl_RenderState.Apply();
				qd.Render(GL_TRIANGLE_STRIP);
				gl_RenderState.SetFixedColormap(CM_DEFAULT);
			}
		}
		else
		{
			gl_RenderModel(this);
		}
	}

	if (clipping)
	{
		gl_RenderState.EnableSplit(false);
	}

	if (pass == GLPASS_TRANSLUCENT)
	{
		gl_RenderState.EnableBrightmap(true);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		gl_RenderState.BlendEquation(GL_FUNC_ADD);
		gl_RenderState.SetTextureMode(TM_MODULATE);
		if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
		{
			glPolygonOffset(0.0f, 0.0f);
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
	}
	else if (modelframe == nullptr)
	{
		glPolygonOffset(0.0f, 0.0f);
		glDisable(GL_POLYGON_OFFSET_FILL);
	}

	gl_RenderState.SetObjectColor(0xffffffff);
	gl_RenderState.SetAddColor(0);
	gl_RenderState.EnableTexture(true);
	gl_RenderState.SetDynLight(0, 0, 0);

}


//==========================================================================
//
// 
//
//==========================================================================
inline void GLSprite::PutSprite(bool translucent)
{
	int list;
	// [BB] Allow models to be drawn in the GLDL_TRANSLUCENT pass.
	if (translucent || actor == nullptr || (!modelframe && (actor->renderflags & RF_SPRITETYPEMASK) != RF_WALLSPRITE))
	{
		list = GLDL_TRANSLUCENT;
	}
	else
	{
		list = GLDL_MODELS;
	}
	dynlightindex = -1;
	gl_drawinfo->drawlists[list].AddSprite(this);
}

//==========================================================================
//
// 
//
//==========================================================================
void GLSprite::SplitSprite(sector_t * frontsector, bool translucent)
{
	GLSprite copySprite(mDrawer);
	double lightbottom;
	unsigned int i;
	bool put = false;
	TArray<lightlist_t> & lightlist = frontsector->e->XFloor.lightlist;

	for (i = 0; i < lightlist.Size(); i++)
	{
		// Particles don't go through here so we can safely assume that actor is not NULL
		if (i < lightlist.Size() - 1) lightbottom = lightlist[i + 1].plane.ZatPoint(actor);
		else lightbottom = frontsector->floorplane.ZatPoint(actor);

		if (lightbottom < z2) lightbottom = z2;

		if (lightbottom < z1)
		{
			copySprite = *this;
			copySprite.lightlevel = gl_ClampLight(*lightlist[i].p_lightlevel);
			copySprite.Colormap.CopyLight(lightlist[i].extra_colormap);

			if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				copySprite.Colormap.Decolorize();
			}

			if (!gl_isWhite(ThingColor))
			{
				copySprite.Colormap.LightColor.r = (copySprite.Colormap.LightColor.r*ThingColor.r) >> 8;
				copySprite.Colormap.LightColor.g = (copySprite.Colormap.LightColor.g*ThingColor.g) >> 8;
				copySprite.Colormap.LightColor.b = (copySprite.Colormap.LightColor.b*ThingColor.b) >> 8;
			}

			z1 = copySprite.z2 = lightbottom;
			vt = copySprite.vb = copySprite.vt +
				(lightbottom - copySprite.z1)*(copySprite.vb - copySprite.vt) / (z2 - copySprite.z1);
			copySprite.PutSprite(translucent);
			put = true;
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight)
{
	const float NO_VAL = 100000000.0f;
	bool clipthing = (thing->player || thing->flags3&MF3_ISMONSTER || thing->IsKindOf(NAME_Inventory)) && (thing->flags&MF_ICECORPSE || !(thing->flags&MF_CORPSE));
	bool smarterclip = !clipthing && gl_spriteclip == 3;
	if ((clipthing || gl_spriteclip > 1) && !(thing->flags2 & MF2_FLOATBOB))
	{

		float btm = NO_VAL;
		float top = -NO_VAL;
		extsector_t::xfloor &x = thing->Sector->e->XFloor;

		if (x.ffloors.Size())
		{
			for (unsigned int i = 0; i < x.ffloors.Size(); i++)
			{
				F3DFloor * ff = x.ffloors[i];
				float floorh = ff->top.plane->ZatPoint(thingpos);
				float ceilingh = ff->bottom.plane->ZatPoint(thingpos);
				if (floorh == thing->floorz)
				{
					btm = floorh;
				}
				if (ceilingh == thing->ceilingz)
				{
					top = ceilingh;
				}
				if (btm != NO_VAL && top != -NO_VAL)
				{
					break;
				}
			}
		}
		else if (thing->Sector->GetHeightSec())
		{
			if (thing->flags2&MF2_ONMOBJ && thing->floorz ==
				thing->Sector->heightsec->floorplane.ZatPoint(thingpos))
			{
				btm = thing->floorz;
				top = thing->ceilingz;
			}
		}
		if (btm == NO_VAL)
			btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		if (top == NO_VAL)
			top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		// +/-1 to account for the one pixel empty frame around the sprite.
		float diffb = (z2 + 1) - btm;
		float difft = (z1 - 1) - top;
		if (diffb >= 0 /*|| !gl_sprite_clip_to_floor*/) diffb = 0;
		// Adjust sprites clipping into ceiling and adjust clipping adjustment for tall graphics
		if (smarterclip)
		{
			// Reduce slightly clipping adjustment of corpses
			if (thing->flags & MF_CORPSE || spriteheight > fabs(diffb))
			{
				float ratio = clamp<float>((fabs(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
				diffb *= ratio;
			}
			if (!diffb)
			{
				if (difft <= 0) difft = 0;
				if (difft >= (float)gl_sclipthreshold)
				{
					// dumb copy of the above.
					if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || difft > (float)gl_sclipthreshold)
					{
						difft = 0;
					}
				}
				if (spriteheight > fabs(difft))
				{
					float ratio = clamp<float>((fabs(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					difft *= ratio;
				}
				z2 -= difft;
				z1 -= difft;
			}
		}
		if (diffb <= (0 - (float)gl_sclipthreshold))	// such a large displacement can't be correct! 
		{
			// for living monsters standing on the floor allow a little more.
			if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || diffb < (-1.8*(float)gl_sclipthreshold))
			{
				diffb = 0;
			}
		}
		z2 -= diffb;
		z1 -= diffb;
	}
}

//==========================================================================
//
// 
//
//==========================================================================

CVAR(Float, gl_sprite_distance_cull, 4000.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

inline bool IsDistanceCulled(AActor* thing)
{
	double culldist = gl_sprite_distance_cull * gl_sprite_distance_cull;
	if (culldist <= 0.0)
		return false;

	double dist = (thing->Pos() - r_viewpoint.Pos).LengthSquared();

	if (dist > culldist)
		return true;
	return false;
}

void GLSprite::Process(AActor* thing, sector_t * sector, int thruportal, bool isSpriteShadow)
{
	sector_t rs;
	sector_t * rendersector;

	if (thing == nullptr)
		return;

	if (IsDistanceCulled(thing))
		return;

	// [ZZ] allow CustomSprite-style direct picnum specification
	bool isPicnumOverride = thing->picnum.isValid();

	// Don't waste time projecting sprites that are definitely not visible.
	if ((thing->sprite == 0 && !isPicnumOverride) || !thing->IsVisibleToPlayer() || ((thing->renderflags & RF_MASKROTATION) && !thing->IsInsideVisibleAngles()))
	{
		return;
	}

	AActor *camera = r_viewpoint.camera;

	if (thing->renderflags & RF_INVISIBLE || !thing->RenderStyle.IsVisible(thing->Alpha))
	{
		if (!(thing->flags & MF_STEALTH) || !mDrawer->FixedColormap || !gl_enhanced_nightvision || thing == camera)
			return;
	}

	// check renderrequired vs ~r_rendercaps, if anything matches we don't support that feature,
	// check renderhidden vs r_rendercaps, if anything matches we do support that feature and should hide it.
	if ((!r_debug_disable_vis_filter && !!(thing->RenderRequired & ~r_renderercaps)) ||
		(!!(thing->RenderHidden & r_renderercaps)))
		return;

	int spritenum = thing->sprite;
	DVector2 sprscale = thing->Scale;
	if (thing->player != NULL)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// If this thing is in a map section that's not in view it can't possibly be visible
	if (!thruportal && !(currentmapsection[thing->subsector->mapsection >> 3] & (1 << (thing->subsector->mapsection & 7)))) return;

	// [RH] Interpolate the sprite's position to make it look smooth
	DVector3 thingpos = thing->InterpolatedPosition(r_viewpoint.TicFrac);
	if (thruportal == 1) thingpos += Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);

	// Some added checks if the camera actor is not supposed to be seen. It can happen that some portal setup has this actor in view in which case it may not be skipped here
	if (thing == camera && !r_viewpoint.showviewer)
	{
		DVector3 thingorigin = thing->Pos();
		if (thruportal == 1) thingorigin += Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(thingorigin.X - r_viewpoint.ActorPos.X) < 2 && fabs(thingorigin.Y - r_viewpoint.ActorPos.Y) < 2) return;
	}
	// Thing is invisible if close to the camera.
	if (thing->renderflags & RF_MAYBEINVISIBLE)
	{
		if (fabs(thingpos.X - r_viewpoint.Pos.X) < 32 && fabs(thingpos.Y - r_viewpoint.Pos.Y) < 32) return;
	}

	// Too close to the camera. This doesn't look good if it is a sprite.
	if (fabs(thingpos.X - r_viewpoint.Pos.X) < 2 && fabs(thingpos.Y - r_viewpoint.Pos.Y) < 2)
	{
		if (r_viewpoint.Pos.Z >= thingpos.Z - 2 && r_viewpoint.Pos.Z <= thingpos.Z + thing->Height + 2)
		{
			// exclude vertically moving objects from this check.
			if (!thing->Vel.isZero())
			{
				if (!FindModelFrame(thing->GetClass(), spritenum, thing->frame, false))
				{
					return;
				}
			}
		}
	}

	// don't draw first frame of a player missile
	if (thing->flags&MF_MISSILE)
	{
		if (!(thing->flags7 & MF7_FLYCHEAT) && thing->target == GLRenderer->mViewActor && GLRenderer->mViewActor != NULL)
		{
			double speed = thing->Vel.Length();
			if (speed >= thing->target->radius / 2)
			{
				double clipdist = clamp(thing->Speed, thing->target->radius, thing->target->radius * 2);
				if ((thingpos - r_viewpoint.Pos).LengthSquared() < clipdist * clipdist) return;
			}
		}
		thing->flags7 |= MF7_FLYCHEAT;	// do this only once for the very first frame, but not if it gets into range again.
	}

	if (thruportal != 2 && GLRenderer->mClipPortal)
	{
		int clipres = GLRenderer->mClipPortal->ClipPoint(thingpos);
		if (clipres == GLPortal::PClip_InFront) return;
	}
	// disabled because almost none of the actual game code is even remotely prepared for this. If desired, use the INTERPOLATE flag.
	if (thing->renderflags & RF_INTERPOLATEANGLES)
		Angles = thing->InterpolatedAngles(r_viewpoint.TicFrac);
	else
		Angles = thing->Angles;

	player_t *player = &players[consoleplayer];
	FloatRect r;

	if (sector->sectornum != thing->Sector->sectornum && !thruportal)
	{
		// This cannot create a copy in the fake sector cache because it'd interfere with the main thread, so provide a local buffer for the copy.
		// Adding synchronization for this one case would cost more than it might save if the result here could be cached.
		rendersector = gl_FakeFlat(thing->Sector, mDrawer->in_area, false, &rs);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	uint32_t spritetype = (thing->renderflags & RF_SPRITETYPEMASK);
	x = thingpos.X;
	z = thingpos.Z;
	y = thingpos.Y;
	if (spritetype == RF_FACESPRITE) z -= thing->Floorclip; // wall and flat sprites are to be considered level geometry so this may not apply.

	// snap shadow Z to the floor
	if (isSpriteShadow)
	{
		z = thing->floorz;
	}
	// [RH] Make floatbobbing a renderer-only effect.
	else if (thing->flags2 & MF2_FLOATBOB)
	{
		float fz = thing->GetBobOffset(r_viewpoint.TicFrac);
		z += fz;
	}

	modelframe = isPicnumOverride ? nullptr : FindModelFrame(thing->GetClass(), spritenum, thing->frame, !!(thing->flags & MF_DROPPED));

	// don't bother drawing sprite shadows if this is a model (it will never look right)
	if (modelframe && isSpriteShadow)
	{
		return;
	}

	if (!modelframe)
	{
		bool mirror;
		DAngle ang = (thingpos - r_viewpoint.Pos).Angle();
		FTextureID patch;
		// [ZZ] add direct picnum override
		if (isPicnumOverride)
		{
			// Animate picnum overrides.
			auto tex = TexMan(thing->picnum);
			if (tex == nullptr) return;
			patch = tex->id;
			mirror = false;
		}
		else
		{
			DAngle sprangle;
			int rot;
			if (!(thing->renderflags & RF_FLATSPRITE) || thing->flags7 & MF7_SPRITEANGLE)
			{
				sprangle = thing->GetSpriteAngle(ang, r_viewpoint.TicFrac);
				rot = -1;
			}
			else
			{
				// Flat sprites cannot rotate in a predictable manner.
				sprangle = 0.;
				rot = 0;
			}
			patch = sprites[spritenum].GetSpriteFrame(thing->frame, rot, sprangle, &mirror, !!(thing->renderflags & RF_SPRITEFLIP));
		}

		if (!patch.isValid()) return;
		int type = thing->renderflags & RF_SPRITETYPEMASK;
		gltexture = FMaterial::ValidateTexture(patch, (type == RF_FACESPRITE), false);
		if (!gltexture)
			return;

		vt = gltexture->GetSpriteVT();
		vb = gltexture->GetSpriteVB();
		if (thing->renderflags & RF_YFLIP) std::swap(vt, vb);

		gltexture->GetSpriteRect(&r);

		// [SP] SpriteFlip
		if (thing->renderflags & RF_SPRITEFLIP)
			thing->renderflags ^= RF_XFLIP;

		if (mirror ^ !!(thing->renderflags & RF_XFLIP))
		{
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
			ul = gltexture->GetSpriteUL();
			ur = gltexture->GetSpriteUR();
		}
		else
		{
			ul = gltexture->GetSpriteUR();
			ur = gltexture->GetSpriteUL();
		}

		if (thing->renderflags & RF_SPRITEFLIP) // [SP] Flip back
			thing->renderflags ^= RF_XFLIP;

		r.Scale(sprscale.X, isSpriteShadow ? sprscale.Y * 0.15 : sprscale.Y);

		float SpriteOffY = thing->SpriteOffset.Y;
		float rightfac = -r.left - thing->SpriteOffset.X;
		float leftfac = rightfac - r.width;
		z1 = z - r.top - SpriteOffY;
		z2 = z1 - r.height;

		float spriteheight = sprscale.Y * r.height;

		// Tests show that this doesn't look good for many decorations and corpses
		if (spriteheight > 0 && gl_spriteclip > 0 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
		{
			PerformSpriteClipAdjustment(thing, thingpos, spriteheight);
		}

		switch (spritetype)
		{
		case RF_FACESPRITE:
		{
			float viewvecX = GLRenderer->mViewVector.X;
			float viewvecY = GLRenderer->mViewVector.Y;

			x1 = x - viewvecY * leftfac;
			x2 = x - viewvecY * rightfac;
			y1 = y + viewvecX * leftfac;
			y2 = y + viewvecX * rightfac;
			break;
		}
		case RF_FLATSPRITE:
		{
			float bottomfac = -r.top - SpriteOffY;
			float topfac = bottomfac - r.height;

			x1 = x + leftfac;
			x2 = x + rightfac;
			y1 = y - topfac;
			y2 = y - bottomfac;
			// [MC] Counteract in case of any potential problems. Tests so far haven't
			// shown any outstanding issues but that doesn't mean they won't appear later
			// when more features are added.
			z1 += SpriteOffY;
			z2 += SpriteOffY;
			break;
		}
		case RF_WALLSPRITE:
		{
			float viewvecX = Angles.Yaw.Cos();
			float viewvecY = Angles.Yaw.Sin();

			x1 = x + viewvecY * leftfac;
			x2 = x + viewvecY * rightfac;
			y1 = y - viewvecX * leftfac;
			y2 = y - viewvecX * rightfac;
			break;
		}
		}
	}
	else
	{
		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		gltexture = NULL;
	}

	depth = (float)((x - r_viewpoint.Pos.X) * r_viewpoint.TanCos + (y - r_viewpoint.Pos.Y) * r_viewpoint.TanSin);
	if (isSpriteShadow) depth += 1.f / 65536.f; // always sort shadows behind the sprite.


//==========================================================================
//
// Begin Anamorphic Forced-Perspective+ projection
//
//==========================================================================

	if (gl_spriteclip == -1 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
	{
		// ======= Anamorphic Forced-Perspective+ sprite projecting routine START =======
		// init the anamorphic sprite light correction vars with original z1, z2 coords
		nonanam_z1 = z1; nonanam_z2 = z2;
		float minbias = r_spriteclipanamorphicminbias;
		minbias = clamp(minbias, 0.3f, 1.0f);

		// Get the viewpoint from the current draw context
		const DVector3 &vp = r_viewpoint.Pos; // defining that way is closer to how GZDoom v4.14.2
		// and that helped to reduce leaks, the olny difference from original sprite cliping mode of
		// Forced-Perspective is that it's 3DFloor aware

		// Regular and 3D Floor-Aware Heights
		float btm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, thingpos, thing) - thing->Floorclip;
		float top = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, thingpos, thing);
		// Viewer's actual heights (from their sector and 3D floors)
		float vbtm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);
		float vtop = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);

		float vpx = vp.X; float vpy = vp.Y; float vpz = vp.Z;
		float tpx = thingpos.X; float tpy = thingpos.Y; float tpz = z; // Use 'z' from sprite setup

		// Radius-based bias (disable with r_debug_nolimitanamorphoses) - prevents leaks
		if (!r_debug_nolimitanamorphoses)
		{
			float objradius = thing->radius;
			float distsq = (tpx - vpx)*(tpx - vpx) + (tpy - vpy)*(tpy - vpy);
			float objradiusbias = 1.f - objradius / sqrt(distsq);
			minbias = MAX(minbias, objradiusbias);
		}

		float bintersect, tintersect;
		if (z2 < vpz && vbtm < vpz)		bintersect = MIN((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
		else							bintersect = 1.0;

		if (z1 > vpz && vtop > vpz)		tintersect = MIN((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
		else							tintersect = 1.0;

		if (thing->waterlevel >= 1 && thing->waterlevel <= 2)					bintersect = tintersect = 1.0f;

		float spbias = clamp<float>(MIN(bintersect, tintersect), minbias, 1.0f);
		float vpbias = 1.0 - spbias;

		bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, thingpos, thing->Sector, thing);

		// Apply projection distortion using original vp method
		if (!a3DfloorPlaneObstructed)
		{
			x1 = x1 * spbias + vpx * vpbias;
			y1 = y1 * spbias + vpy * vpbias;
			z1 = z1 * spbias + vpz * vpbias;
			x2 = x2 * spbias + vpx * vpbias;
			y2 = y2 * spbias + vpy * vpbias;
			z2 = z2 * spbias + vpz * vpbias;
		}

		// Anamorphic sprite overbright correction
		float sprSizeLight = (thing->radius + thing->Height) * 0.5f;
		if (sprSizeLight >= 36.0f) sprSizeLight = 36.0f; // clamp sprSizeLight
		float original_z1 = nonanam_z1;
		float original_z2 = nonanam_z2;
		// Calculate the deltas (offsets)
		// Clamp them to prevent negative depth correction
		nonanam_z1 = clamp<float>(original_z1 - z1, 0.0f, (sprSizeLight * 0.55f));
		nonanam_z2 = clamp<float>(original_z2 - z2, 0.0f, (sprSizeLight * 0.55f));
		// Output to the engine console
		// %f - float, %.2f - float with 2 decimal points
		//Printf("Sprite [%s]: Orig Z1: %.2f, Z2: %.2f | Delta Z1: %.2f, Z2: %.2f\n", 
		//	thing->GetClass()->TypeName.GetChars(), original_z1, original_z2, nonanam_z1, nonanam_z2);

		// ======= Anamorphic Forced-Perspective+ sprite projecting routine FINISH =======
	}

//==========================================================================
//
// Finish Anamorphic Forced-Perspective+ projection
//
//==========================================================================



//==========================================================================
//
// Begin Hybrid Anamorphic Forced-Perspective - Smart projection
//
//==========================================================================

	if (gl_spriteclip == -2 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
	{
		// init the anamorphic sprite light correction vars with original z1, z2 coords
		nonanam_z1 = z1; nonanam_z2 = z2;
		// Reset cache on need
		static int lastLevelMaptime = -1;
		// Level changed/reloaded
		if (level.maptime != lastLevelMaptime)
		{
			ResetAnamorphCache();
			lastLevelMaptime = level.maptime;
		}

		// Determine sprite classification
		float spriteRadius = (float)thing->radius;
		float spriteSize = (thing->radius + thing->Height) * 0.5f;
		bool isMicroSprite = (spriteSize <= 8.0f && spriteSize < 12.0f);
		bool isTinySprite = (spriteSize <= 12.0f && spriteSize < 18.0f);
		bool isSmallSprite = (spriteSize <= 18.0f);
		bool isMediumSprite = (spriteSize > 18.0f && spriteSize < 38.0f);
		bool isLargeSprite = (spriteSize > 38.0f);

		bool islegacyversionprojectile =
			(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
			(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
			(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);
		bool islegacyversionmonster =
			(thing->flags & MF_SHOOTABLE) && (thing->flags & MF_COUNTKILL) &&
			(thing->flags & MF_SOLID) && (thing->flags2 & MF2_PUSHWALL) &&
			(thing->flags4 & MF4_CANUSEWALLS) && (thing->flags2 & MF2_MCROSS) &&
			(thing->flags2 & MF2_PASSMOBJ) && (thing->flags3 & MF3_ISMONSTER);
		bool isfloatingmonster =
			(thing->flags & MF_SHOOTABLE) && (thing->flags & MF_COUNTKILL) &&
			(thing->flags & MF_SOLID) && (thing->flags2 & MF2_PUSHWALL) &&
			(thing->flags4 & MF4_CANUSEWALLS) && (thing->flags2 & MF2_MCROSS) &&
			(thing->flags2 & MF2_PASSMOBJ) && (thing->flags3 & MF3_ISMONSTER) &&
			(thing->flags & MF_FLOAT || thing->flags & MF_INFLOAT);
		bool isfloatingsprite = (thing->flags & MF_FLOAT || thing->flags & MF_INFLOAT);
		bool isactoracorpse = (thing->flags & MF_CORPSE) || (thing->flags & MF_ICECORPSE);
		bool isactorsmallbutnotcorpse = (spriteSize >= 8.0f && spriteSize <= 18.0f) && !isactoracorpse;
		bool isaregularsizedmonster = (islegacyversionmonster && (spriteSize <= 38.0f));
		bool isabonusitem = ((spriteSize >= 16.0f && spriteSize <= 25.0f) &&
			(!islegacyversionmonster || !isfloatingsprite || !isactoracorpse));


		// ~~~*** Detect if Y-axis sprite offset significant enough to cross the ground - START ***~~~
		// === LZDoom07 way - START ===================================================================
		int spriteFileOffset = 0;      // Blank rows at top of texture (from file)
		int spriteRasterXdimen = 0;    // Total texture width (including blank columns)
		int spriteRasterYdimen = 0;    // Total texture height (including blank rows)
		bool hasSignificantNegativeOffset = false;
		if (gltexture && gltexture->tex)
		{
			FTexture* tex = gltexture->tex;
			// we get scaled dim because they can be hires
			spriteRasterXdimen = tex->GetScaledWidth();
			spriteRasterYdimen = tex->GetScaledHeight();
			spriteFileOffset = tex->TopOffset;
			// Calculate visible sprite height (actual drawn pixels)
			int visibleSpriteHeight = spriteRasterYdimen - spriteFileOffset;
			hasSignificantNegativeOffset = (visibleSpriteHeight >= 1);
			// Debug output showing all measurements
			//Printf("Sprite '%s': ""FileH=%dpx | ""TopOff=%dpx | ""VisibleH=%dpx | ""SigNegOffset=%s",
			//	tex->Name.GetChars(),spriteRasterYdimen, spriteFileOffset,visibleSpriteHeight,hasSignificantNegativeOffset ? "YES" : "NO");
		}
		// === LZDoom07 way - FINISH===================================================================
		// === UZDoom way - START =====================================================================
		//int  spriteFileOffset             = 0; // Blank rows at top of texture (from file)
		//int  spriteRasterXdimen           = 0; // Total texture width (including blank columns)
		//int  spriteRasterYdimen           = 0; // Total texture height (including blank rows)
		//bool hasSignificantNegativeOffset = false;
		//FGameTexture *gtex                         = nullptr;
		//
		//	// First, check for a direct texture override (picnum)
		//if (thing->picnum.isValid())
		//{
		//	gtex = TexMan.GetGameTexture(thing->picnum);
		//}
		//else
		//{
		//	// In UZDoom, sprites are handled by the Texture Manager using their ID and frame.
		//	// We fetch the game texture using the sprite index and frame from the actor.
		//	// thing->sprite is the sprite ID, thing->frame is the frame index.
		//	gtex = TexMan.GameByIndex(thing->sprite, true); // true for animation check
		//}
		//
		//if (gtex)
		//{
		//	// Access the underlying FTexture object
		//	FTexture *tex = gtex->GetTexture();
		//
		//	if (tex)
		//	{
		//		// GetScaledWidth/Height automatically account for Scale.X/Scale.Y
		//		spriteRasterXdimen = tex->GetScaledWidth();
		//		spriteRasterYdimen = tex->GetScaledHeight();
		//		// GetScaledTopOffset replaces the old public TopOffset field
		//		spriteFileOffset = tex->GetScaledTopOffset();
		//		// Calculate visible sprite height (actual drawn pixels)
		//		int visibleSpriteHeight      = spriteRasterYdimen - spriteFileOffset;
		//		hasSignificantNegativeOffset = (visibleSpriteHeight >= 1);
		//		// Debug output to verify dimensions
		//		// Printf("Sprite Resolve: %s | RasterH: %d | TopOff: %d | Visible: %d\n",
		//		//       tex->Name.GetChars(), spriteRasterYdimen,
		//		//       spriteFileOffset, visibleSpriteHeight);
		//	}
		//}
		// === UZDoom way - FINISH ====================================================================
		// ~~~*** Detect if Y-axis sprite offset significant enough to cross the ground - FINISH ***~~~

		// =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*
		// ******* REGULAR FP sprite projecting routine for SMALL NONCORPSE sprites OPTIMIZATION START *******
		// Use simple Forced-Perspective without occlusion checks and smart clipping for sprites that
		// are smaller than 8 units, and that are not corpses, not monsters and not projectiles (explosions)
		// AND if their raster graphical size is indeed small
		const bool useRegularForcedPerspective(spriteRadius <= 8.0f &&
				         (!isactoracorpse || !islegacyversionmonster || !islegacyversionprojectile) &&
		( (spriteRasterXdimen <= 24 && spriteRasterYdimen <= 24) || !hasSignificantNegativeOffset)  );
		if (useRegularForcedPerspective)
		{
			float minbias = r_spriteclipanamorphicminbias;
			minbias = clamp(minbias, 0.3f, 1.0f);

			// Get the viewpoint from the current draw context
			const DVector3 &vp = r_viewpoint.Pos; // defining that way is closer to how GZDoom v4.14.2
			// and that helped to reduce leaks, the olny difference from original sprite cliping mode of
			// Forced-Perspective is that it's 3DFloor aware

			// Regular and 3D Floor-Aware Heights
			float btm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, thingpos, thing) - thing->Floorclip;
			float top = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, thingpos, thing);
			// Viewer's actual heights (from their sector and 3D floors)
			float vbtm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);
			float vtop = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);

			float vpx = vp.X; float vpy = vp.Y; float vpz = vp.Z;
			float tpx = thingpos.X; float tpy = thingpos.Y; float tpz = z; // Use 'z' from sprite setup

			// Radius-based bias (disable with r_debug_nolimitanamorphoses) - prevents leaks
			if (!r_debug_nolimitanamorphoses)
			{
				float objradius = thing->radius;
				float distsq = (tpx - vpx)*(tpx - vpx) + (tpy - vpy)*(tpy - vpy);
				float objradiusbias = 1.f - objradius / sqrt(distsq);
				minbias = MAX(minbias, objradiusbias);
			}

			float bintersect, tintersect;
			if (z2 < vpz && vbtm < vpz)		bintersect = MIN((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
			else							bintersect = 1.0;

			if (z1 > vpz && vtop > vpz)		tintersect = MIN((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
			else							tintersect = 1.0;

			if (thing->waterlevel >= 1 && thing->waterlevel <= 2)					bintersect = tintersect = 1.0f;

			float spbias = clamp<float>(MIN(bintersect, tintersect), minbias, 1.0f);
			float vpbias = 1.0 - spbias;

			bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, thingpos, thing->Sector, thing);

			// Apply projection distortion using original vp method
			if (!a3DfloorPlaneObstructed)
			{
				x1 = x1 * spbias + vpx * vpbias;
				y1 = y1 * spbias + vpy * vpbias;
				z1 = z1 * spbias + vpz * vpbias;
				x2 = x2 * spbias + vpx * vpbias;
				y2 = y2 * spbias + vpy * vpbias;
				z2 = z2 * spbias + vpz * vpbias;
			}

			// Anamorphic sprite overbright correction
			float sprSizeLight = (thing->radius + thing->Height) * 0.5f;
			if (sprSizeLight >= 36.0f) sprSizeLight = 36.0f; // clamp sprSizeLight
			float original_z1 = nonanam_z1;
			float original_z2 = nonanam_z2;
			// Calculate the deltas (offsets)
			// Clamp them to prevent negative depth correction
			nonanam_z1 = clamp<float>(original_z1 - z1, 0.0f, (sprSizeLight * 0.55f));
			nonanam_z2 = clamp<float>(original_z2 - z2, 0.0f, (sprSizeLight * 0.55f));
			// Output to the engine console
			// %f - float, %.2f - float with 2 decimal points
			//Printf("Sprite [%s]: Orig Z1: %.2f, Z2: %.2f | Delta Z1: %.2f, Z2: %.2f\n", 
			//	thing->GetClass()->TypeName.GetChars(), original_z1, original_z2, nonanam_z1, nonanam_z2);
			// ******* REGULAR FP sprite projecting routine for SMALL NONCORPSE sprites OPTIMIZATION FINISH*******
			// =*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*
		}
		else
		{
			// Define distance constants properly
			const float FP_CLOSER_LIMIT = 384.0f;            // Where Forced-Perspective coordinates without lift-up end (close-up)
			const float SMART_START_DISTANCE = 1200.0f;       // Where Smart-clip starts coordinates start to lift-up (far-side)
			const float TRANSITION_WIDTH = SMART_START_DISTANCE - FP_CLOSER_LIMIT;    // Length of transition

			// Get the viewpoint from the current draw context that helped to reduce leaks
			const DVector3 &vp = r_viewpoint.Pos; // defining that way is closer to GZDoom v4.14.2

			// Make sure all 1sided checks and MidTxt checks do NOT have FOV(Frustum Culling) and 2sided - HAVE them instead
			bool thingFacingBboxCrossed1sided = SpriteBboxFacingCameraCrossed1sLineCachedWrapper(thing, r_viewpoint.camera);
			bool thingCrossed1sidedLine = SpriteCrossed1sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
			bool thingCrossed1sVoidLine = SpriteCrossed1sidedVoidLinedefCachedWrapper(thing, r_viewpoint.camera, false);
			bool thingCrossed1sVoidBbox = SpriteCrossed1sidedVoidBboxFaceCachedWrapper(thing, r_viewpoint.camera, true);
			bool thingCrossed2sidedLine = SpriteCrossed2sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
			bool visible1sidesInfTallObstr = IsSpriteVisibleBehind1sidedLinesCachedWrapper(thing, r_viewpoint.camera, thingpos);
			bool visible2sideTallEnoughObstr = IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(r_viewpoint.camera, thing);
			bool visible2sideMidTex = CheckFacingMidTextureProximityWrapper(thing, r_viewpoint.camera, thingpos);
			bool visible3dfloorSides = IsSpriteVisibleBehind3DFloorSidesCachedWrapper(r_viewpoint.camera, thing);
			bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, thingpos, thing->Sector, thing);

			// Adding "AActor* viewer" to "GLSprite::Process" signature would be a pain
			// That's why get viewer from renderer context instead of function parameters
			AActor* viewer = r_viewpoint.camera;
			float EyeHeight = 41.0f;
			if (viewer->player && viewer->player->mo)
			{
				EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
			}

			float viewerBottom = viewer->Z();
			float viewerTop = viewerBottom + EyeHeight;
			float spriteBottom = thing->Z();
			float spriteTop = thing->Top();
			float sprLowerMid = (spriteBottom + (spriteTop * 0.25f));
			float sprLowerMidAdj = sprLowerMid + Ztolerance2sided;
			float viewerBottomAdj = viewerBottom - Ztolerance2sidedBot;
			float viewerTopAdj = viewerTop + Ztolerance2sided;
			float sprBottomAdj = spriteBottom + Ztolerance2sidedBot;
			float sprTopAdj = spriteTop + Ztolerance2sided;

			// -------------
			// Some mods have big looking sprites with little radius-or-height
			// we need to adjust them to have a nice anamorphosis effect (increase it)
			if ((spriteRasterXdimen >= (thing->radius)) || (spriteRasterYdimen >= (thing->Height)))
			{
				if (isMicroSprite)
				{
					if (spriteRasterXdimen >= 64.0f || spriteRasterYdimen >= 64.0f)
					{
						// handle bigger sized sprites first
						spriteSize = 4.8f;
					}
					else
					{
						spriteSize = 2.12f;
					}
				}
				else if (isTinySprite) { spriteSize += 1.0f; }
				else if (isSmallSprite) { spriteSize += 1.2f; }
				else if (isMediumSprite) { spriteSize += 2.4f; }
				else { spriteSize *= 1.1f; }
			}
			// -------------

			// -------------
			// We'll have to limit spriteSize to minimize the leaks unfortunately
			if (spriteSize >= 44.0f)
			{
				spriteSize = 44.0f;
			}
			// -------------

			// -------------
			// Too many leaks through 2sided obstructions because even default sprite sizes
			// are too big and cross the linedefs, making them invisible to detection systems.
			// In this case, we must decrease their sprite sizes to make them reasonable sizes.
			if (!visible2sideTallEnoughObstr)
			{
				spriteSize *= 0.5f;
			}
			// -------------

			// -------------
			// Some sprites like torches can still leak through
			// thin 2sided walls, especially, if they cross those linedefs
			if ((!visible2sideTallEnoughObstr || !visible3dfloorSides) && thingCrossed2sidedLine)
			{
				spriteSize *= 0.64f;
			}
			// -------------

			// Initialize spriteSize factors here for them
			// to be visible in this entire sprite clipping mode scope
			float smallsprtncrps_factor = 1.0f;
			float projectiles_factor = 1.0f;
			float regularsizmonster_factor1 = 1.0f;
			float regularsizmonster_factor2 = 1.0f;
			float extended_radius2 = 1.0f;

			float vpx = vp.X; float vpy = vp.Y; float vpz = vp.Z;
			float tpx = thingpos.X; float tpy = thingpos.Y; float tpz = z; // Use 'z' from sprite setup

			DVector3 thingpos3D(thingpos.X, thingpos.Y, z);
			float distSq = (thingpos3D - r_viewpoint.Pos).LengthSquared();
			float dist = sqrt(distSq);

			// Calculate blend properly
			float blend = 0.f;

			// With additional culling mechanism coplanar leaks already reduced
			// But we can disable Forced-Perspective for floating sprites entirely
			// but only for those whose Y-axis sprite offset doesn't cross ground at all.
			if (isfloatingsprite && !hasSignificantNegativeOffset)
			{
				// Force smart mode for floating sprites and things crossing 1sided-linedefs
				blend = 1.0f;
			}
			else
			{
				// If we're in the transition zone between forced and normal perspective
				if (dist > FP_CLOSER_LIMIT && dist < SMART_START_DISTANCE)
				{
					blend = (dist - FP_CLOSER_LIMIT) / TRANSITION_WIDTH;
				}
				// If we're beyond the transition zone, use normal perspective
				else if (dist >= SMART_START_DISTANCE)
				{
					blend = 1.0f;
				}

				// Special handling for projectiles and small sprites (except corpses): force forced-perspective
				if (islegacyversionprojectile || isactorsmallbutnotcorpse)
				{
					blend = 0.f;
				}
			}

			// Clamp to ensure it's within [0,1] range
			blend = clamp<float>(blend, 0.f, 1.f);

			// Store original coordinates
			float orig_x1 = x1, orig_y1 = y1, orig_z1 = z1;
			float orig_x2 = x2, orig_y2 = y2, orig_z2 = z2;

			// Calculate smart clipping values
			float smart_x1 = orig_x1, smart_y1 = orig_y1, smart_z1 = orig_z1;
			float smart_x2 = orig_x2, smart_y2 = orig_y2, smart_z2 = orig_z2;

			// Only calculate smart clipping for non-special cases
			if (blend > 0.f && !islegacyversionprojectile && !isactorsmallbutnotcorpse)
			{
				// Save original coordinates
				float temp_x1 = orig_x1, temp_y1 = orig_y1, temp_z1 = orig_z1;
				float temp_x2 = orig_x2, temp_y2 = orig_y2, temp_z2 = orig_z2;

				if ((isfloatingsprite && !hasSignificantNegativeOffset) || thingCrossed1sidedLine)
				{
					// Perform smart clip but don't raise for the cases above
					PerformSpriteClipAdjustment(thing, thingpos, 0.0);
				}
				else
				{
					// Perform smart clip on original coordinates
					PerformSpriteClipAdjustment(thing, thingpos, 0.0);
					smart_x1 = x1; smart_y1 = y1; smart_z1 = z1;
					smart_x2 = x2; smart_y2 = y2; smart_z2 = z2;
				}

				// Restore original coordinates for forced perspective calculation
				x1 = temp_x1; y1 = temp_y1; z1 = temp_z1;
				x2 = temp_x2; y2 = temp_y2; z2 = temp_z2;
			}

			// Only apply forced perspective if blend < 1.0 and NOT a floating sprite
			if (blend < 1.f)
			{
				float minbias = clamp<float>(r_spriteclipanamorphicminbias, 0.1f, 0.5f);

				// ======= Forced-Perspective Anamorphic sprite projecting routine START =======

				// we still got leaks when viewer is situtated at extremely steep angles to sprites
				// even through 1-sided walls, which we could try to hack by adding z-tolerance
				// but not even like that, when viewer middle is within vertical bounds of sprite

				// Regular and 3D Floor-Aware Heights
				float btm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, thingpos, thing) - thing->Floorclip;
				float top = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, thingpos, thing);
				// Viewer's actual heights (from their sector and 3D floors)
				float vbtm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);
				float vtop = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);


				//     === Dynamic anamorphosis occlusion based amount effect adjustment - START ===
				// ---===============================================================================---
				// Some HUGE leaks still occur through 2sided obstructions if we're too close. It's
				//   because we set our anamorphosis radius amounts too big for them to penetrate flats
				//   even as we step farther from them. But what to do when we're too close?
				// Pay attention that even with huge anamorphosis radiuses these leaks go away.
				// Conclusion: decrease anamorphosis radius when sprite is closer because when we close
				//   even a smaller radius anamorphosis amount is enough to provide a good effect.
				//   also pay attention we decrease radius only on viewer and sprite coplanar situtations!
				float sprPrxFctr = 1.0f;
				float sprPrxFctrProj = 1.0f;
				float sprPrxDistThresh = 674.0f;

				const float invSprPrxDistThresh = 1.0f / sprPrxDistThresh;

				// 1. Close-up coplanar mitigation loop
				if ((dist < sprPrxDistThresh) && (fabs(btm - vbtm) <= Ztolerance2sided || fabs(btm - vtop) <= Ztolerance2sided))
				{
					float distProgress = dist * invSprPrxDistThresh; // Celeron-friendly fast multiplication!
					sprPrxFctr = 0.075f + (0.25f - 0.075f) * distProgress;
					sprPrxFctrProj = sprPrxFctr; // Sync close-up behavior
				}

				// 2. DISTANT HORIZON BLIND-ZONE INTERCEPTOR (Small Items)
				if (dist >= sprPrxDistThresh && isactorsmallbutnotcorpse)
				{
					if (fabs(vpz - btm) < 128.0f)
					{
						sprPrxFctr = 0.15f; // Force-clamp the expansion hull for items on flat views
					}
				}

				smallsprtncrps_factor = 3.4f * (sprPrxFctr * 15.0f);
				if (!visible1sidesInfTallObstr || !visible2sideTallEnoughObstr || !visible3dfloorSides)
					smallsprtncrps_factor = 1.0f;
				else if (!visible2sideMidTex) smallsprtncrps_factor = 0.25f;

				projectiles_factor = 8.0f;
				if (!visible1sidesInfTallObstr || !visible2sideTallEnoughObstr || !visible3dfloorSides)
					projectiles_factor = 1.0f;
				else if (!visible2sideMidTex) projectiles_factor = 0.25f;

				regularsizmonster_factor1 = 3.64f * (sprPrxFctr * 3.0f);
				if (!visible1sidesInfTallObstr || !visible2sideTallEnoughObstr || !visible3dfloorSides)
					regularsizmonster_factor1 = 1.0f;
				else if (!visible2sideMidTex) regularsizmonster_factor1 = 0.25f;

				regularsizmonster_factor2 = (isaregularsizedmonster) ?
					regularsizmonster_factor1 :
					regularsizmonster_factor1 * 4.0f;

				float extended_radius1 = (isactorsmallbutnotcorpse) ?
					spriteSize * smallsprtncrps_factor :     // 3.25x for small noncorpsesprites and 1.0 ocl, 0.025 super occluded
					spriteSize;                              // 1x for all the rest sprites
				float extended_radius2 = (islegacyversionprojectile) ?
					extended_radius1 * projectiles_factor :  // 16x for projectiles like rockets, explosions and 6x when occluded
					extended_radius1;                        // 1x for all the rest sprites
				// ---===============================================================================---
				//     === Dynamic anamorphosis occlusion based amount effect adjustment - FINISH ===


				//					=== Anamorphosis culling pass 1 - START ===
				float radius_for_bias = 0.0f;

				// void detection is still important for "CrossedAnyWall" besides it was already used in "Anamorphosis culling pass 1"
				bool CrossedAnyWall = thingCrossed1sVoidLine || thingFacingBboxCrossed1sided || thingCrossed2sidedLine;
				// notice "isSpriteNOTObstructed" has no "! negation signs" as it means sprite is NOT obstructed and reported as visible
				bool isSpriteNOTObstructed = (visible1sidesInfTallObstr || visible2sideTallEnoughObstr || visible2sideMidTex || visible3dfloorSides);

				// Crucial for "thingCrossed1sVoidLine" to be here
				// otherwise it gets useless below (while it's also needed there as OR in "CrossedAnyWall")
				if (thingCrossed1sVoidLine || thingFacingBboxCrossed1sided || !isSpriteNOTObstructed)
				{
					// Regular Forced-Perspective way
					radius_for_bias = spriteRadius;
					spriteSize *= 0.25f;
					smallsprtncrps_factor *= 0.25f;
					projectiles_factor *= 0.25f;
					regularsizmonster_factor2 = 0.75f;
				}
				else if (isactorsmallbutnotcorpse || islegacyversionprojectile)
				{
					radius_for_bias = extended_radius2;
				}
				else
				{
					radius_for_bias = spriteSize;
				}

				if (!(r_debug_nolimitanamorphoses))
				{
					float distsqAnam = (tpx - vpx)*(tpx - vpx) + (tpy - vpy)*(tpy - vpy);
					float distAnam = sqrt(distsqAnam);

					if (distAnam > 0.1f)
					{
						// Use the monsterfactors for monster and if not - pure radius
						float currentFactor = (isaregularsizedmonster) ? regularsizmonster_factor2 : 1.0f;
						float objradiusbias = 1.f - (radius_for_bias * currentFactor) / distAnam;
						minbias = MAX(minbias, objradiusbias);
					}
				}
				//					=== Anamorphosis culling pass 1 - FINISH ===

				////		=== The pass 2 agressive culling core process - START ===
				//// this pass 2 makes stuff like "increaseAnam" to leak way less
				//if (!isSpriteNOTObstructed && !islegacyversionprojectile)
				//{
				//	const float planeProximThresh = 4.0f;
				//	float viewerTopAdjCullPass2 = viewerBottom + (EyeHeight * 0.5f);
				//	float spriteTopAdjCullPass2 = spriteTop + (EyeHeight * 0.064f);

				//	bool isFloorSprite = (spriteBottom - btm) <= planeProximThresh;
				//	bool isCeilingSprite = (top - spriteTop) <= planeProximThresh;

				//	bool viewerLookingDown = viewerTopAdjCullPass2 >= spriteTopAdjCullPass2;
				//	bool viewerLookingUp = viewerTopAdjCullPass2 <= spriteBottom;

				//	if (((isFloorSprite && viewerLookingDown) || (isCeilingSprite && viewerLookingUp)))
				//	{
				//	}
				//	else
				//	{
				//		if (!r_debug_nolimitanamorphoses)
				//		{
				//			float distsq = (tpx - vpx)*(tpx - vpx) + (tpy - vpy)*(tpy - vpy);
				//			float objradiusbias = 1.f - spriteSize / sqrt(distsq);
				//			minbias = MAX(minbias, objradiusbias);
				//		}
				//	}
				//}
				////		=== The pass 2 agressive culling core process - FINISH ===

				//					=== Anamorphosis final culling pass - START ===
				float bintersect, tintersect;
				if (z2 < vpz && vbtm < vpz) bintersect = MIN((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz));
				else bintersect = 1.0f;
				if (z1 > vpz && vtop > vpz) tintersect = MIN((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz));
				else tintersect = 1.0f;
				if (thing->waterlevel >= 1 && thing->waterlevel <= 2) bintersect = tintersect = 1.0f;

				//                |---------------------------------------------------|
				//		 ---***===|		CRAZY STEEP ANAMORPHOSIS PROCESS - START	  |===***---
				//                |---------------------------------------------------|
				// if ((dist < 1200.0f) && isonsteepsurf && isSpriteNOTObstructed && !CrossedAnyWall) increaseAnam = 0.125f;
				// 0.125 looks good but leaks farther away you go, 0.09 doesn't leak that much but looks worse when close to a sprite
				// so we need to decrease "increaseAnam" from 0.125 to 0.0 smoothly as the distance exceeds minAnamDist (384.0f)
				//
				// -------------------- COMPUTE STEEP FACTOR |-> START|
				bool  isonsteepsurf, isonsteepsurfmild;
				float steepness = 5.25f;    // detect only very steep surfaces
				float steepnessfact = pow(MAX(1.f - bintersect, 1.f - tintersect), steepness);
				isonsteepsurf = steepnessfact > 0.0001f;
				float steepnessmild = 1.5f; // detect not so steep surfaces too
				float steepnessmildfact = pow(MAX(1.f - bintersect, 1.f - tintersect), steepnessmild);
				isonsteepsurfmild = steepnessmildfact > 0.0001f;
				float viewerEyeLevelZ = viewerBottom + EyeHeight;
				bool isSprBotAtEyeLevel = fabsf(spriteBottom - viewerEyeLevelZ) <= 24.0f;
				float viewer2x5EyeLevelZ = viewerBottom + (EyeHeight * 2.5f);
				bool isSprBotAt2x5EyeLevel = fabsf(spriteBottom - viewer2x5EyeLevelZ) <= 24.0f;
				float viewerHalfEyeLevelZ = viewerBottom + (EyeHeight * 0.5f);
				bool isSprBotAtHalfEyeLevel = fabsf(spriteBottom - viewerHalfEyeLevelZ) <= 12.0f;
				// -------------------- THE MULTIPLIER SETUP |-> START|
				// -- PHASE #1 - determine maximum effect amounts
				float increaseAnam = 0.0f; // The higher the more the anamorphosis effect is but more leaks
				float incrAnamMaximum = 0.0f; // Bigger sprites need lesser "increaseAnam" amounts, otherwise they leak more
				if (CrossedAnyWall)
				{
					// Must be done this way, otherwise leaks more
					incrAnamMaximum = 0.0f; // the culled out case
				}
				else
				{
					// the visible case
					if (isabonusitem) incrAnamMaximum = 0.175f;   // Bigger amount for small but NOT smaller than bonus
					else              incrAnamMaximum = 0.075f;   // Smaller amount for all the rest sprites
				}
				// -- PHASE #2 - determine the distant effect amount fade
				if ((dist < 1200.0f) && isonsteepsurf)
				{
					const float minAnamDist = 384.0f;             // Max effect in this zone
					const float maxAnamDist = 1200.0f;            // Full effect fade here
					const float max2minAnamDistDiffInv = 1.0f / (maxAnamDist - minAnamDist);
					if (dist <= minAnamDist)
					{
						increaseAnam = incrAnamMaximum;           // Full power
					}
					else
					{
						float fadeFactor = 1.0f - ((dist - minAnamDist) * max2minAnamDistDiffInv);
						// Prevent it from becoming negative
						if (fadeFactor < 0.0f) fadeFactor = 0.0f;
						increaseAnam = incrAnamMaximum * fadeFactor;
					}
				}
				// -- PHASE #3 - setup the suppression multiplier (decreaseAnam)
				float spbias = 0.0f;       // initialize the variable
				float decreaseAnam = 0.0f; // initialize the variable
				// Decrease anamorphosis when sprites are culled by 1s+void or 2s lines,void is now facing
				if ((thingCrossed1sVoidLine || thingFacingBboxCrossed1sided) && isonsteepsurfmild)
				{
					//	//            ****** another dirty workaround hack - START ******
					//	// Tame down Project Brutality 3 invoid spawn lamps that leak through walls
					//  // A better way to tame them down - use thingCrossed1sVoidLine without bbox facing
					//	// Big decreaseAnam amounts don't just decrease anamorphosis effect - ERASE sprites
					//	// that's why you decrease decreaseAnam if their increased radii are small!
					//if (spriteSize <= 18.0f)
					//{
					//	if (isonsteepsurfmild && (isSprBotAtEyeLevel || isSprBotAt2x5EyeLevel) )
					//		                                           decreaseAnam = 0.27f;
					//	else                                           decreaseAnam = 0.05f;
					//}
					//else if (spriteSize > 32.0f && spriteSize < 43.0f) decreaseAnam = 0.15f;
					//else                                               decreaseAnam = 0.2f;
					//	//            ****** another dirty workaround hack - FINISH ******

					// Values lower aren't sufficient to suppress leaks on big radii sprites like
					// small sprites - health bonus, torches, etc with increased radii not to fade in far
					decreaseAnam = 0.075f; // that's why put it simple and do a better occlusion logic
				}
				// Still some leaks through 2s obstr. Calc isonsteepsurfmild within SprBotAtHalfEyeLev
				// Fixes some leaks on Doom2 Remake, Map12 and allows for other sprites that aren't
				// under steep angles (coplanar) to render properly.
				else if (thingCrossed2sidedLine && (isonsteepsurfmild && isSprBotAtHalfEyeLevel))
				{
					// Values lower aren't sufficient to suppress leaks on big radii sprites like
					// small sprites - health bonus, torches, etc with increased radii not to fade in far
					decreaseAnam = 0.05f;
				}
				// -------------------- THE MULTIPLIER SETUP |-> FINISH|
				// ---------PERFORM CRAZY STEEP ANAMORPHOSIS |->  START|
				// Make sure your 1s, midtxt checks do NOT have FOV check and 2s, 3df - do HAVE it
				if (CrossedAnyWall)
				{
					// Some items like torches still leak through walls if put really close.
					// Yes, it's safe to summ "decreaseAnam" here because it cuts anamorphosis anyway
					spbias = clamp<float>(MIN(bintersect, tintersect), minbias, 1.0f) + decreaseAnam;
				}
				else
				{
					// Just putting "- increaseAnam" already makes it leak SO much thus separated.
					// This mode is required to make sprites to draw through flats like crazy.
					// No it's NOT safe to subtract "increaseAnam" here on clamp but we got it CULLED
					// and leaks only occur where we need them (DONE ON PURPOSE).
					spbias = clamp<float>(MIN(bintersect, tintersect), minbias, 1.0f) - increaseAnam;
				}
				float vpbias = 1.0 - spbias;
				// --------PERFORM CRAZY STEEP ANAMORPHOSIS |->  FINISH|
				//                |---------------------------------------------------|
				//		 ---***===|		CRAZY STEEP ANAMORPHOSIS PROCESS - FINISH	  |===***---
				//                |---------------------------------------------------|

				//					=== Anamorphosis final culling pass - FINISH ===

				// Apply projection distortion using original vp method only if not obstructed by a 3DFloor above or below
				if (!a3DfloorPlaneObstructed)
				{
					x1 = x1 * spbias + vpx * vpbias;
					y1 = y1 * spbias + vpy * vpbias;
					z1 = z1 * spbias + vpz * vpbias;
					x2 = x2 * spbias + vpx * vpbias;
					y2 = y2 * spbias + vpy * vpbias;
					z2 = z2 * spbias + vpz * vpbias;
				}

				// ======= Forced-Perspective Anamorphic sprite projecting routine FINISH =======
			}

			// Store forced-perspective adjusted positions
			float fp_x1 = x1, fp_y1 = y1, fp_z1 = z1;
			float fp_x2 = x2, fp_y2 = y2, fp_z2 = z2;

			// Apply blending based on distance
			if (!islegacyversionprojectile && !isactorsmallbutnotcorpse)
			{
				if (blend > 0.f && blend < 1.f)
				{
					// Blend between forced and smart clipping for ALL coordinates
					x1 = fp_x1 * (1 - blend) + smart_x1 * blend;
					y1 = fp_y1 * (1 - blend) + smart_y1 * blend;
					z1 = fp_z1 * (1 - blend) + smart_z1 * blend;
					x2 = fp_x2 * (1 - blend) + smart_x2 * blend;
					y2 = fp_y2 * (1 - blend) + smart_y2 * blend;
					z2 = fp_z2 * (1 - blend) + smart_z2 * blend;
				}
				else if (blend >= 1.f)
				{
					// Apply full smart clipping when blend = 1.0
					x1 = smart_x1; y1 = smart_y1;
					z1 = smart_z1; x2 = smart_x2;
					y2 = smart_y2; z2 = smart_z2;
				}
			}

			// Anamorphic sprite overbright correction
			float sprSizeLight = (thing->radius + thing->Height) * 0.5f;
			if (sprSizeLight >= 36.0f) sprSizeLight = 36.0f; // clamp sprSizeLight
			float original_z1 = nonanam_z1; float original_z2 = nonanam_z2;
			// Calculate the deltas (offsets)
			// Clamp them to prevent negative depth correction - 1st val are small spr
			float sprAnamLightAmount = (isactorsmallbutnotcorpse) ? (smallsprtncrps_factor * 4.0f) : 0.4f;
			nonanam_z1 = clamp<float>(original_z1 - z1, 0.0f, (sprSizeLight * sprAnamLightAmount));
			nonanam_z2 = clamp<float>(original_z2 - z2, 0.0f, (sprSizeLight * sprAnamLightAmount));
			// Output to the engine console
			// %f - float, %.2f - float with 2 decimal points
			//Printf("Sprite [%s]: Orig Z1: %.2f, Z2: %.2f | Delta Z1: %.2f, Z2: %.2f\n", 
			//	thing->GetClass()->TypeName.GetChars(), original_z1, original_z2, nonanam_z1, nonanam_z2);
		}
	}

//==========================================================================
//
// Finish Hybrid Anamorphic Forced-Perspective - Smart projection
//
//==========================================================================

	// light calculation

	bool enhancedvision = false;

	// allow disabling of the fullbright flag by a brightmap definition
	// (e.g. to do the gun flashes of Doom's zombies correctly.
	fullbright = (thing->flags5 & MF5_BRIGHT) ||
		((thing->renderflags & RF_FULLBRIGHT) && (!gltexture || !gltexture->tex->gl_info.bDisableFullbright));

	lightlevel = fullbright ? 255 :
		gl_ClampLight(rendersector->GetTexture(sector_t::ceiling) == skyflatnum ?
			rendersector->GetCeilingLight() : rendersector->GetFloorLight());
	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255);

	lightlevel = gl_CheckSpriteGlow(rendersector, lightlevel, thingpos);

	ThingColor = (thing->RenderStyle.Flags & STYLEF_ColorIsFixed) ? thing->fillcolor : 0xffffff;
	ThingColor.a = 255;
	RenderStyle = thing->RenderStyle;

	// colormap stuff is a little more complicated here...
	if (mDrawer->FixedColormap)
	{
		if ((gl_enhanced_nv_stealth > 0 && mDrawer->FixedColormap == CM_LITE)		// Infrared powerup only
			|| (gl_enhanced_nv_stealth == 2 && mDrawer->FixedColormap >= CM_TORCH)// Also torches
			|| (gl_enhanced_nv_stealth == 3))								// Any fixed colormap
			enhancedvision = true;

		Colormap.Clear();

		if (mDrawer->FixedColormap == CM_LITE)
		{
			if (gl_enhanced_nightvision &&
				(thing->IsKindOf(NAME_Inventory) || thing->flags3&MF3_ISMONSTER || thing->flags&MF_MISSILE || thing->flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &level.sectors[rendersector->sectornum] || mDrawer->in_area != area_below)
				// under water areas keep their color for fullbright objects
			{
				// Only make the light white but keep everything else (fog, desaturation and Boom colormap.)
				Colormap.MakeWhite();
			}
			else
			{
				// Keep the color, but brighten things a bit so that a difference can be seen.
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = thing->Translation;

	OverrideShader = -1;
	trans = thing->Alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !gl.legacyMode && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;	// trans may not be 1 here
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency)
	{
		// [SP] "canonical transparency" - with the flip of a CVar, disable transparency for Doom objects,
		//   and disable 'additive' translucency for certain objects from other games.
		if (thing->renderflags & RF_ZDOOMTRANS)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
		trans = 1.f;

		if (!gl_sprite_blend || modelframe || (thing->renderflags & (RF_FLATSPRITE | RF_WALLSPRITE)) || gl_billboard_faces_camera)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((gltexture && gltexture->GetTransparent()) || (RenderStyle.Flags & STYLEF_RedIsAlpha) || (modelframe && thing->RenderStyle != DefaultRenderStyle()))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			// enhanced vision makes them more visible!
			trans = 0.5f;
			FRenderStyle rs = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
		}
		else if (thing->flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	// for sprite shadow, use a translucent stencil renderstyle
	if (isSpriteShadow)
	{
		RenderStyle = STYLE_Stencil;
		ThingColor = MAKEARGB(255, 0, 0, 0);
		trans *= 0.5f;
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (trans == 0.0f) return;

	// end of light calculation

	actor = thing;
	index = thing->SpawnOrder;

	// sprite shadows should have a fixed index of -1 (ensuring they're drawn behind particles which have index 0)
	// sorting should be irrelevant since they're always translucent
	if (isSpriteShadow)
	{
		index = -1;
	}

	particle = nullptr;

	const bool drawWithXYBillboard = (!(actor->renderflags & RF_FORCEYBILLBOARD)
		&& (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE
		&& players[consoleplayer].camera
		&& (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD));


	// no light splitting when:
	// 1. no lightlist
	// 2. any fixed colormap
	// 3. any bright object
	// 4. any with render style shadow (which doesn't use the sector light)
	// 5. anything with render style reverse subtract (light effect is not what would be desired here)
	if (thing->Sector->e->XFloor.lightlist.Size() != 0 && mDrawer->FixedColormap == CM_DEFAULT && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		if (gl.flags & RFL_NO_CLIP_PLANES)	// on old hardware we are rather limited...
		{
			lightlist = NULL;
			if (!drawWithXYBillboard && !modelframe)
			{
				SplitSprite(thing->Sector, hw_styleflags != STYLEHW_Solid);
			}
		}
		else
		{
			lightlist = &thing->Sector->e->XFloor.lightlist;
		}
	}
	else
	{
		lightlist = NULL;
	}

	PutSprite(hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}


//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::ProcessParticle(particle_t *particle, sector_t *sector)//, int shade, int fakeside)
{
	if (GLRenderer->mClipPortal)
	{
		int clipres = GLRenderer->mClipPortal->ClipPoint(particle->Pos);
		if (clipres == GLPortal::PClip_InFront) return;
	}

	player_t *player = &players[consoleplayer];

	if (particle->alpha == 0) return;

	lightlevel = gl_ClampLight(sector->GetTexture(sector_t::ceiling) == skyflatnum ?
		sector->GetCeilingLight() : sector->GetFloorLight());
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);

	if (mDrawer->FixedColormap)
	{
		Colormap.Clear();
	}
	else if (!particle->bright)
	{
		TArray<lightlist_t> & lightlist = sector->e->XFloor.lightlist;
		double lightbottom;

		Colormap = sector->Colormap;
		for (unsigned int i = 0; i < lightlist.Size(); i++)
		{
			if (i < lightlist.Size() - 1) lightbottom = lightlist[i + 1].plane.ZatPoint(particle->Pos);
			else lightbottom = sector->floorplane.ZatPoint(particle->Pos);

			if (lightbottom < particle->Pos.Z)
			{
				lightlevel = gl_ClampLight(*lightlist[i].p_lightlevel);
				Colormap.CopyLight(lightlist[i].extra_colormap);
				break;
			}
		}
		if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();	// ZDoom never applies colored light to particles.
		}
	}
	else
	{
		lightlevel = 255;
		Colormap = sector->Colormap;
		Colormap.ClearColor();
	}

	trans = particle->alpha;
	RenderStyle = STYLE_Translucent;
	OverrideShader = 0;

	ThingColor = particle->color;
	ThingColor.a = 255;

	modelframe = NULL;
	gltexture = NULL;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;
	index = 0;

	// [BB] Load the texture for round or smooth particles
	if (gl_particles_style)
	{
		FTextureID lump;
		if (gl_particles_style == 1)
		{
			lump = GLRenderer->glPart2;
		}
		else if (gl_particles_style == 2)
		{
			lump = GLRenderer->glPart;
		}
		else lump.SetNull();

		if (lump.isValid())
		{
			gltexture = FMaterial::ValidateTexture(lump, true, false);
			translation = 0;

			ul = gltexture->GetUL();
			ur = gltexture->GetUR();
			vt = gltexture->GetVT();
			vb = gltexture->GetVB();
			FloatRect r;
			gltexture->GetSpriteRect(&r);
		}
	}

	double timefrac = r_viewpoint.TicFrac;
	if (paused || level.isFrozen())
		timefrac = 0.;
	float xvf = (particle->Vel.X) * timefrac;
	float yvf = (particle->Vel.Y) * timefrac;
	float zvf = (particle->Vel.Z) * timefrac;

	x = float(particle->Pos.X) + xvf;
	y = float(particle->Pos.Y) + yvf;
	z = float(particle->Pos.Z) + zvf;

	float factor;
	if (gl_particles_style == 1) factor = 1.3f / 7.f;
	else if (gl_particles_style == 2) factor = 2.5f / 7.f;
	else factor = 1 / 7.f;
	float scalefac = particle->size * factor;

	float viewvecX = GLRenderer->mViewVector.X;
	float viewvecY = GLRenderer->mViewVector.Y;

	x1 = x + viewvecY * scalefac;
	x2 = x - viewvecY * scalefac;
	y1 = y - viewvecX * scalefac;
	y2 = y + viewvecX * scalefac;
	z1 = z - scalefac;
	z2 = z + scalefac;

	depth = (float)((x - r_viewpoint.Pos.X) * r_viewpoint.TanCos + (y - r_viewpoint.Pos.Y) * r_viewpoint.TanSin);

	actor = NULL;
	this->particle = particle;
	fullbright = !!particle->bright;

	// [BB] Translucent particles have to be rendered without the alpha test.
	if (gl_particles_style != 2 && trans >= 1.0f - FLT_EPSILON) hw_styleflags = STYLEHW_Solid;
	else hw_styleflags = STYLEHW_NoAlphaTest;

	if (sector->e->XFloor.lightlist.Size() != 0 && mDrawer->FixedColormap == CM_DEFAULT && !fullbright)
		lightlist = &sector->e->XFloor.lightlist;
	else
		lightlist = NULL;

	PutSprite(hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

//==========================================================================
//
// 
//
//==========================================================================

void GLSceneDrawer::RenderActorsInPortal(FGLLinePortal *glport)
{
	TMap<AActor*, bool> processcheck;
	if (glport->validcount == validcount) return;	// only process once per frame
	glport->validcount = validcount;
	for (auto port : glport->lines)
	{
		line_t *line = port->mOrigin;
		if (line->isLinePortal())	// only crossable ones
		{
			FLinePortal *port2 = port->mDestination->getPortal();
			// process only if the other side links back to this one.
			if (port2 != nullptr && port->mDestination == port2->mOrigin && port->mOrigin == port2->mDestination)
			{

				for (portnode_t *node = port->lineportal_thinglist; node != nullptr; node = node->m_snext)
				{
					AActor *th = node->m_thing;

					// process each actor only once per portal.
					bool *check = processcheck.CheckKey(th);
					if (check && *check) continue;
					processcheck[th] = true;

					DAngle savedangle = th->Angles.Yaw;
					DVector3 savedpos = th->Pos();
					DVector3 newpos = savedpos;
					sector_t fakesector;

					if (!r_viewpoint.showviewer && th == r_viewpoint.camera)
					{
						if (fabs(savedpos.X - r_viewpoint.ActorPos.X) < 2 && fabs(savedpos.Y - r_viewpoint.ActorPos.Y) < 2)
						{
							continue;
						}
					}

					P_TranslatePortalXY(line, newpos.X, newpos.Y);
					P_TranslatePortalZ(line, newpos.Z);
					P_TranslatePortalAngle(line, th->Angles.Yaw);
					th->SetXYZ(newpos);
					th->Prev += newpos - savedpos;

					GLSprite spr(this);

					// [Nash] draw sprite shadow
					if (R_ShouldDrawSpriteShadow(th))
					{
						spr.Process(th, gl_FakeFlat(th->Sector, in_area, false, &fakesector), 2, true);
					}

					spr.Process(th, gl_FakeFlat(th->Sector, in_area, false, &fakesector), 2);
					th->Angles.Yaw = savedangle;
					th->SetXYZ(savedpos);
					th->Prev -= newpos - savedpos;
				}
			}
		}
	}
}
