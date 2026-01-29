// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
//
// Anamorphic Hybrid-Forced-Perspective code by Vadim Taranov, (C) 2025-2026
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

#include "p_3dmidtex.h"

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

			FQuadDrawer qd;
			qd.Set(0, v[0][0], v[0][1], v[0][2], ul, vt);
			qd.Set(1, v[1][0], v[1][1], v[1][2], ur, vt);
			qd.Set(2, v[2][0], v[2][1], v[2][2], ul, vb);
			qd.Set(3, v[3][0], v[3][1], v[3][2], ur, vb);
			qd.Render(GL_TRIANGLE_STRIP);

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

//==========================================================================
//
// Anamorphic "Forced-Perspective" sprite clipping occlusion culling - start
//
//==========================================================================

bool enableAnamorphCache = true; // gives it a really considerable speed-up

//         ---===      ***************************************        ===---
// ******* Anamorphic "Forced-Pespective" common early exit checks - start *******
// Distance cull checks on big maps with lots of 3D-floors to speed up
bool IsAnamorphicDistanceCulled(AActor* thing, float gl_anamorphic_spriteclip_distance_cull)
{
	if (!thing) return true; // Handle null pointers
	if (gl_anamorphic_spriteclip_distance_cull <= 0.0f)
		return false;

	const float cullDist = gl_anamorphic_spriteclip_distance_cull;
	const float cullDistSq = cullDist * cullDist;

	return (thing->Pos() - r_viewpoint.Pos).LengthSquared() > cullDistSq;
}

line_t* GetClosestLineInSector(const DVector3 &thingpos, sector_t* sector)
{
	if (!sector || sector->Lines.Size() == 0) return nullptr;

	line_t* closestLine = nullptr;
	float minDistSq = FLT_MAX;

	for (unsigned i = 0; i < sector->Lines.Size(); i++)
	{
		line_t* testLine = sector->Lines[i];
		float distSq1 = (testLine->v1->fPos() - thingpos.XY()).LengthSquared();
		float distSq2 = (testLine->v2->fPos() - thingpos.XY()).LengthSquared();
		float avgDistSq = (distSq1 + distSq2) * 0.5f;

		if (avgDistSq < minDistSq)
		{
			minDistSq = avgDistSq;
			closestLine = testLine;
		}
	}
	return closestLine;
}

// Frustum culling
static bool CheckFrustumCulling(AActor* thing)
{
	const DVector3 viewerPos = r_viewpoint.Pos;
	const DVector3 thingPos = thing->Pos();

	// Extract the ACTUAL current FOV from the viewpoint
	// r_viewpoint.FOV is already in degrees, so we don't need to convert
	float currentFOV = r_viewpoint.FieldOfView.Degrees;
	float currentFOV2x = currentFOV * 2.0;

	// Calculate frustum based on tilt
	float tilt = fabs(static_cast<float>(r_viewpoint.Angles.Pitch.Degrees));
	if (tilt > 46.0f) return false; // Don't cull at extreme angles

	// Use the actual FOV instead of hardcoded 90
	float floatangle = 2.0 + (45.0 + (tilt / 1.9)) * currentFOV2x * 48.0 / AspectMultiplier(r_viewwindow.WidescreenRatio) / currentFOV;
	angle_t frustumAngle = DAngle(floatangle).BAMs();

	if (frustumAngle < ANGLE_180)
	{
		DVector2 spriteVec(thingPos.X - viewerPos.X, thingPos.Y - viewerPos.Y);
		angle_t spriteAng = spriteVec.Angle().BAMs();
		angle_t viewerAng = r_viewpoint.Angles.Yaw.BAMs();

		angle_t diff1 = spriteAng > viewerAng ? spriteAng - viewerAng : viewerAng - spriteAng;
		angle_t angleDiff = (diff1 > ANGLE_MAX / 2) ? ANGLE_MAX - diff1 : diff1;

		if (angleDiff > frustumAngle / 2)
		{
			return true; // Culled by frustum
		}
	}

	return false; // Not culled by frustum
}

struct LineSegmentCommon
{
	float x1, y1, x2, y2;

	LineSegmentCommon(float x1, float y1, float x2, float y2) : x1(x1), y1(y1), x2(x2), y2(y2) {}

	bool IntersectsCommon(const LineSegmentCommon& other) const
	{
		auto cross = [](float ax, float ay, float bx, float by)
		{
			return ax * by - ay * bx;
		};

		// Direction vectors
		float dx1 = x2 - x1;
		float dy1 = y2 - y1;
		float dx2 = other.x2 - other.x1;
		float dy2 = other.y2 - other.y1;

		// Relative vectors
		float relAX = other.x1 - x1;
		float relAY = other.y1 - y1;
		float relBX = other.x2 - x1;
		float relBY = other.y2 - y1;

		float cross1 = cross(dx1, dy1, relAX, relAY);
		float cross2 = cross(dx1, dy1, relBX, relBY);
		if (cross1 * cross2 >= 0) return false;

		float relCX = x1 - other.x1;
		float relCY = y1 - other.y1;
		float relDX = x2 - other.x1;
		float relDY = y2 - other.y1;

		float cross3 = cross(dx2, dy2, relCX, relCY);
		float cross4 = cross(dx2, dy2, relDX, relDY);
		return (cross3 * cross4 < 0);
	}
};

// Function to check if a wall is thin (less than 12 units thick)
// how to call: bool thisisathinwall = IsThinWallCommon(thing, r_viewpoint.camera, thingpos);
static bool IsThinWallCommon(AActor* viewer, AActor* thing, DVector3& thingpos)
{
	// Early exit for invalid inputs
	if (!viewer || !thing) return false;

	sector_t* viewSector = viewer->Sector;
	sector_t* thingSector = P_PointInSector(thingpos.X, thingpos.Y);

	// Only check if in different sectors
	if (viewSector == thingSector) return false;

	DVector3 viewerPos = viewer->Pos();
	LineSegmentCommon sight(viewerPos.X, viewerPos.Y, thingpos.X, thingpos.Y);

	// Precompute perpendicular direction for thickness checks
	const DVector2 perpDirs[2] =
	{
		{0, 1},   // Positive perpendicular
		{0, -1}   // Negative perpendicular
	};

	// Check both sectors
	for (auto* sector : { viewSector, thingSector })
	{
		for (auto& line : sector->Lines)
		{
			LineSegmentCommon wall(line->v1->fX(), line->v1->fY(), line->v2->fX(), line->v2->fY());

			if (wall.IntersectsCommon(sight))
			{
				// Get the other sector
				sector_t* otherSector = (sector == viewSector) ? thingSector : viewSector;

				// Wall direction vector
				DVector2 wallDir(line->v2->fX() - line->v1->fX(), line->v2->fY() - line->v1->fY());
				float wallLength = wallDir.Length();

				if (wallLength <= 0) continue;

				// Normalize wall direction
				wallDir /= wallLength;

				// Sample points along the wall (3 points)
				for (float t : {0.2f, 0.5f, 0.8f})
				{
					DVector2 wallPoint(
						line->v1->fX() + wallDir.X * wallLength * t,
						line->v1->fY() + wallDir.Y * wallLength * t
					);

					// Check thickness in both perpendicular directions
					for (const auto& perp : perpDirs)
					{
						DVector2 testPerp(perp.Y * wallDir.X - perp.X * wallDir.Y,
							perp.X * wallDir.Y - perp.Y * wallDir.X);

						// Ray cast for thickness
						for (float rayDist = 1.0f; rayDist <= 15.0f; rayDist += 1.0f)
						{
							DVector2 testPos = wallPoint + testPerp * rayDist;
							if (P_PointInSector(testPos.X, testPos.Y) == otherSector)
							{
								if (rayDist < 12.0f) // Thin wall found
									return true;
								break;
							}
						}
					}
				}
			}
		}
	}

	return false;
}

// Wrapper function - to call struct LineSegment1sided from any other place
// bool thingCrossed1sidedLine = SpriteIntersectsLinedef(viewerPos.X, viewerPos.Y, edgeX, edgeY, line->v1->fX(), line->v1->fY(), line->v2->fX(), line->v2->fY(), ix, iy);
static bool SpriteIntersectsLinedef(float x1, float y1, float x2, float y2, float ox1, float oy1, float ox2, float oy2, float& ix, float& iy)
{
	LineSegmentCommon seg1(x1, y1, x2, y2);
	LineSegmentCommon seg2(ox1, oy1, ox2, oy2);

	if (!seg1.IntersectsCommon(seg2))
		return false;

	// Calculate intersection point
	float denom = (x1 - x2) * (oy1 - oy2) - (y1 - y2) * (ox1 - ox2);
	float t = ((x1 - ox1) * (oy1 - oy2) - (y1 - oy1) * (ox1 - ox2)) / denom;
	ix = x1 + t * (x2 - x1);
	iy = y1 + t * (y2 - y1);

	return true;
}

// === CACHED version of SpriteIntersectsLinedef - start === UNUSED
struct SpriteIntersectsLinedefCacheKey
{
	float segmentX;
	float segmentY;

	SpriteIntersectsLinedefCacheKey(float sx, float sy) : segmentX(sx), segmentY(sy) {}

	bool operator==(const SpriteIntersectsLinedefCacheKey& other) const
	{
		return segmentX == other.segmentX && segmentY == other.segmentY;
	}

	bool operator!=(const SpriteIntersectsLinedefCacheKey& other) const
	{
		return !(*this == other);
	}

	bool operator<(const SpriteIntersectsLinedefCacheKey& other) const
	{
		if (segmentX != other.segmentX)
			return segmentX < other.segmentX;
		return segmentY < other.segmentY;
	}
};

template<> struct THashTraits<SpriteIntersectsLinedefCacheKey>
{
	// Custom hash function for our key type
	hash_t Hash(const SpriteIntersectsLinedefCacheKey key)
	{
		// Mix both floats into the hash
		hash_t xhash, yhash;
		memcpy(&xhash, &key.segmentX, sizeof(xhash));
		memcpy(&yhash, &key.segmentY, sizeof(yhash));
		return xhash ^ yhash;
	}

	// Compare two keys (required)
	int Compare(const SpriteIntersectsLinedefCacheKey left, const SpriteIntersectsLinedefCacheKey right)
	{
		return !(left == right);
	}
};

struct spriteIntersectsLineCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cachedSpriteIntersectsLine = false;
	bool cachedSpriteIntersectsLineValid = false;
	float cached_ix = 0.0f, cached_iy = 0.0f;
};

static TMap<line_t*, TMap<SpriteIntersectsLinedefCacheKey, spriteIntersectsLineCacheEntry>> spriteIntersectsLineCache;

bool spriteIntersectsLineCachedWrapper(line_t* line, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float& ix, float& iy)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;

		// Generate a unique key for this segment (using start and end points)
		SpriteIntersectsLinedefCacheKey key(x1 * 1000.0f + x2, y1 * 1000.0f + y2);

		spriteIntersectsLineCacheEntry& entry = spriteIntersectsLineCache[line][key];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7 && entry.cachedSpriteIntersectsLineValid)
		{
			ix = entry.cached_ix;
			iy = entry.cached_iy;
			return entry.cachedSpriteIntersectsLine;
		}

		// Compute fresh result and cache it
		entry.cachedSpriteIntersectsLine = SpriteIntersectsLinedef(x1, y1, x2, y2, x3, y3, x4, y4, entry.cached_ix, entry.cached_iy);
		entry.cachedSpriteIntersectsLineValid = true;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;

		// Set output values
		ix = entry.cached_ix;
		iy = entry.cached_iy;

		return entry.cachedSpriteIntersectsLine;
	}
	else
	{
		// Original uncached behavior
		return SpriteIntersectsLinedef(x1, y1, x2, y2, x3, y3, x4, y4, ix, iy);
	}
}
// === CACHED version of SpriteIntersectsLinedef - finish === UNUSED ===

// Alternative version that checks the actor's position directly
static bool IsActorInVoid(AActor *actor)
{
	// Check if the actor's position is in a valid sector
	sector_t* sector = P_PointInSector(actor->X(), actor->Y());

	// If sector is nullptr, actor is outside the map
	if (sector == nullptr)
	{
		return true;
	}
	else
	{
		return false;
	}
}


// ******* Anamorphic "Forced-Pespective" common early exit checks - finish *******
//         ---===      ***************************************        ===---



//         ---===      ***************************************        ===---
// ******* 1-sided-linedef culling block start *******

bool SpriteCrossed1sidedLinedef(AActor* thing, AActor* viewer)
{
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// ONLY check sprite's sector, never viewer's sector
	sector_t* thingSector = thing->Sector;
	if (!thingSector || thingSector->Lines.Size() == 0) return false;

	// Calculate viewer->thing line geometry
	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	// Calculate sprite size with YOUR original classification logic
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;

	// YOUR EXACT sprite size thresholds (unchanged)
	const bool isMicroSprite = (spriteSize <= 8.0f);
	const bool isTinySprite = (spriteSize <= 12.0f);
	const bool isSmallSprite = (spriteSize <= 18.0f);
	const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 38.0f);
	const bool isLargeSprite = (spriteSize > 38.0f && spriteSize < 39.0f);
	const bool isHugeSprite = (spriteSize >= 39.0f);

	// Scale adjustments based on YOUR original logic
	float spriteScale = 0.25f;  // Default: micro/tiny/small sprites
	if (isMediumSprite) { spriteScale = 0.2f; }
	else if (isLargeSprite) { spriteScale = 0.15f; }
	else if (isHugeSprite) { spriteScale = 0.1f; }

	// Calculate scaled test points based on radius + size
	float adjustedRadius = thing->radius * spriteScale;

	// Generate 4 orthogonal test points around the sprite center
	float testPoints[4][2] =
	{
		{thingX + adjustedRadius, thingY},           // Right
		{thingX - adjustedRadius, thingY},           // Left  
		{thingX, thingY + adjustedRadius},           // Up
		{thingX, thingY - adjustedRadius}            // Down
	};

	for (auto testLine : thingSector->Lines)
	{
		if (!testLine->sidedef[0] || testLine->sidedef[1])
			continue; // Skip non-1-sided lines

		float lineX1 = (float)testLine->v1->fX();
		float lineY1 = (float)testLine->v1->fY();
		float lineX2 = (float)testLine->v2->fX();
		float lineY2 = (float)testLine->v2->fY();

		// Check each test point against the viewer line
		for (int i = 0; i < 4; i++)
		{
			float ix, iy;
			if (SpriteIntersectsLinedef
			(
				viewerX, viewerY,
				testPoints[i][0], testPoints[i][1],
				lineX1, lineY1,
				lineX2, lineY2,
				ix, iy))
			{
				return true; // Sprite crosses a 1-sided line visible to viewer
			}
		}

		// Also check the core viewer->sprite line itself
		float ix, iy;
		if (SpriteIntersectsLinedef(viewerX, viewerY, thingX, thingY, lineX1, lineY1, lineX2, lineY2, ix, iy))
		{
			return true;
		}
	}
	return false;
}

struct SpriteCrossed1SidedLineCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached1sidedResult = false;  // Default: assume NO 1-sided crossing
};

static TMap<AActor*, SpriteCrossed1SidedLineCacheEntry> SpriteCrossed1sidedLineCache;

bool SpriteCrossed1sidedLinedefCachedWrapper(AActor* thing, AActor* viewer)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		SpriteCrossed1SidedLineCacheEntry& entry = SpriteCrossed1sidedLineCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached1sidedResult;
		}

		// Compute and cache fresh result from non-cached function
		bool result = SpriteCrossed1sidedLinedef(thing, viewer);
		entry.cached1sidedResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Original uncached behavior
		return SpriteCrossed1sidedLinedef(thing, viewer);
	}
}




static bool CheckLineOfSight1sided(AActor* viewer, const DVector3& thingpos, float spriteRadius)
{
	DVector3 viewerPos = viewer->Pos();
	sector_t* viewSector = viewer->Sector;
	sector_t* thingSector = P_PointInSector(thingpos.X, thingpos.Y);

	// Early exit for same position
	if (viewerPos.XY() == thingpos.XY()) return true;

	// Direction vector (non-normalized)
	float dx = thingpos.X - viewerPos.X;
	float dy = thingpos.Y - viewerPos.Y;

	// Perpendicular vector
	float px = -dy;
	float py = dx;

	// Precompute test points (4 orthogonal directions)
	DVector2 testPoints[4];
	float scale = spriteRadius / sqrt(dx*dx + dy * dy + 1e-8f);

	testPoints[0] = { thingpos.X + px * scale, thingpos.Y + py * scale };
	testPoints[1] = { thingpos.X - px * scale, thingpos.Y - py * scale };
	testPoints[2] = { thingpos.X + dx * scale, thingpos.Y + dy * scale };
	testPoints[3] = { thingpos.X - dx * scale, thingpos.Y - dy * scale };

	// Check both sectors
	for (auto* sector : { viewSector, thingSector })
	{
		for (auto& line : sector->Lines)
		{
			// Only check 1-sided lines
			if (!line->sidedef[0] || line->sidedef[1]) continue;

			LineSegmentCommon wall(line->v1->fX(), line->v1->fY(), line->v2->fX(), line->v2->fY());

			for (const auto& pt : testPoints)
			{
				LineSegmentCommon sight(viewerPos.X, viewerPos.Y, pt.X, pt.Y);

				if (wall.IntersectsCommon(sight))  // Now correct - single argument
					return false; // Blocked by 1-sided line
			}
		}
	}

	return true; // No blocking 1-sided lines found
}

// Cache structure for 1-sided checks
struct Visibility1sidedCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached1sidedResult = true;
};

// Global cache storage for 1-sided checks
static TMap<AActor*, Visibility1sidedCacheEntry> Visibility1sidedCache;

static bool IsSpriteVisibleBehind1sidedLinesCachedWrapper(AActor* thing, AActor* viewer, const DVector3& thingpos)
{
	// Fast escape checks
	if (!thing || !viewer) return true;

	// We call the function with "!" - that's why return "true" when culled
	if (CheckFrustumCulling(thing)) return true;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return true;

	float spriteScale = 0.15f;

	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		Visibility1sidedCacheEntry& entry = Visibility1sidedCache[thing];

		// Return cached result if valid
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached1sidedResult;
		}

		// Compute and cache fresh result
		bool result = CheckLineOfSight1sided(viewer, thingpos, ((thing->radius) * spriteScale));
		entry.cached1sidedResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Original uncached behavior
		return CheckLineOfSight1sided(viewer, thingpos, ((thing->radius) * spriteScale));
	}
}// ******* 1-sided-linedef culling block finish *******
//         ---===      ***************************************        ===---



//         ---===      ***************************************        ===---
// ******* 2-sided-linedef tall enough sector obstructions culling block start *******
constexpr float MINCOORD2SIDED = -32768.0;
constexpr float MAXCOORD2SIDED = 32767.9999847;
constexpr float EPSILON2SIDED = 0.001;
float Ztolerance2sided = 2.0f;

// Optimized position getters that avoid implicit conversions - again
static FVector2 GetVertexPosition(const vertex_t* vert)
{
	return FVector2(static_cast<float>(vert->fX()), static_cast<float>(vert->fY()));
}

static FVector2 GetActorPosition(const AActor* actor)
{
	return FVector2(static_cast<float>(actor->X()), static_cast<float>(actor->Y()));
}

static FVector3 GetActorPosition3D(const AActor* actor)
{
	return FVector3(static_cast<float>(actor->X()), static_cast<float>(actor->Y()), static_cast<float>(actor->Z()));
}



bool SpriteCrossed2sidedLinedef(AActor* thing, AActor* viewer)
{
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// Only check thing's sector - no other changes
	sector_t* currentSector = thing->Sector;
	if (!currentSector || currentSector->Lines.Size() == 0) return false;

	float vpx = (float)viewer->X();
	float vpy = (float)viewer->Y();
	float tpx = (float)thing->X();
	float tpy = (float)thing->Y();

	for (unsigned i = 0; i < currentSector->Lines.Size(); i++)
	{
		line_t* testLine = currentSector->Lines[i];

		// only check 2-sided linedefs (has both front and back sides)
		if (testLine->sidedef[0] && testLine->sidedef[1])
		{
			float ix, iy;

			// it's slower to call cached version from here, thus unused. If possible, call "SpriteCrossed2sidedLinedefCachedWrapper" as it's going to be faster
			//if (spriteIntersectsLineCachedWrapper(testLine, vpx, vpy, tpx, tpy, testLine->v1->fX(), testLine->v1->fY(), testLine->v2->fX(), testLine->v2->fY(), ix, iy))
			if (SpriteIntersectsLinedef(vpx, vpy, tpx, tpy, testLine->v1->fX(), testLine->v1->fY(), testLine->v2->fX(), testLine->v2->fY(), ix, iy))
			{
				return true;
			}
		}
	}
	return false;
}

struct SpriteCrossed2SidedLineCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached2sidedSpriteCrossedResult = false;  // Default: assume NO 1-sided crossing
};

static TMap<AActor*, SpriteCrossed2SidedLineCacheEntry> SpriteCrossed2sidedLineCache;

bool SpriteCrossed2sidedLinedefCachedWrapper(AActor* thing, AActor* viewer)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// Use caching if enabled
	if (enableAnamorphCache)  // Renamed variable but same concept!
	{
		const int currentMapTimeTick = level.maptime;
		SpriteCrossed2SidedLineCacheEntry& entry = SpriteCrossed2sidedLineCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached2sidedSpriteCrossedResult;
		}

		// Compute and cache fresh result from non-cached function
		bool result = SpriteCrossed2sidedLinedef(thing, viewer);
		entry.cached2sidedSpriteCrossedResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Original uncached behavior
		return SpriteCrossed2sidedLinedef(thing, viewer);
	}
}



struct ObstructionData2Sided
{
	// Floor - track both lowest and highest
	float minFloor = MAXCOORD2SIDED;    // Start high, find lowest
	float maxFloor = MINCOORD2SIDED;    // Start low, find highest

	// Ceiling - track both lowest and highest
	float minCeiling = MAXCOORD2SIDED;  // Start high, find lowest
	float maxCeiling = MINCOORD2SIDED;  // Start low, find highest

	bool valid = false;


	void Update2sidedTallObstructions(AActor* thing, AActor* viewer, const sector_t* sector, const FVector2& point)
	{
		// Determine sprite classification
		// Must be done via "AND" but "OR" works better
		const bool isLegacyProjectile =
			(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
			(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
			(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);

		const float spriteSize = (thing->radius + thing->Height) * 0.5f;
		const bool isSmallSprite = (spriteSize <= 18.0f);
		const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 40.0f);
		const bool isLargeSprite = (spriteSize > 40.0f);
		const bool isRegularMonster = !isSmallSprite && !isLegacyProjectile;

		float EyeHeight = 41.0f;
		if (viewer->player && viewer->player->mo)
		{
			EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
			//Printf("EyeHeight1: %.2f (ViewHeight: %.2f + crouchdelta: %.2f)\n", EyeHeight1, viewer->player->mo->FloatVar(NAME_ViewHeight), viewer->player->crouchviewdelta);
		}
		else
		{
			//Printf("EyeHeight1: %.2f (default value, no player object)\n", EyeHeight1);
		}
		float viewerBottom = viewer->Z();
		float viewerTop = viewerBottom + EyeHeight;
		float viewerBottomAdj = viewerBottom - Ztolerance2sided;
		float viewerTopAdj = viewerTop + Ztolerance2sided;
		float spriteBottom = thing->Z();
		float spriteTop = (thing->Z()) + (thing->Height);
		float sprBottomAdj = spriteBottom - Ztolerance2sided;
		float sprTopAdj = spriteTop + Ztolerance2sided;
		const float cullsmallspriteslessthresh = 32.0f;
		if (isSmallSprite)
		{
			//sprTopAdj -= cullsmallspriteslessthresh;
			//sprBottomAdj -= cullsmallspriteslessthresh;
			EyeHeight *= 2.0f;
		}

		const FVector2 clamped =
		{
			clamp<float>(point.X, MINCOORD2SIDED, MAXCOORD2SIDED),
			clamp<float>(point.Y, MINCOORD2SIDED, MAXCOORD2SIDED)
		};

		float ceilingHeightInitial = sector->ceilingplane.ZatPoint(clamped.X, clamped.Y);
		float floorHeightInitial = sector->floorplane.ZatPoint(clamped.X, clamped.Y);


		// we need to cull large sprites and projectiles with explosions a bit harder
		// so we multiply by a larger number (large sprite expansion in anamorphosis projection)
		float tallestGameStep = 24.0f;
		float diffOftalleststepAndHorizon = EyeHeight - tallestGameStep;
		bool isObstructionTallEnough = (sprTopAdj - diffOftalleststepAndHorizon + EPSILON2SIDED) <= floorHeightInitial ||
			(sprBottomAdj + diffOftalleststepAndHorizon + EPSILON2SIDED) >= ceilingHeightInitial;


		// determine which sprites are located slightly above on elevated platforms
		bool isViewerBottomLowerThanSpriteBottom = viewerBottomAdj <= sprBottomAdj;  // we need some Z tolerance here
		bool isThatElevatedSpriteStillSeen = isViewerBottomLowerThanSpriteBottom && (viewerTopAdj <= sprBottomAdj);
		// sprites that are located slightly above cut suspiciously too much so we raise ceilings by current viewerTopAdj Z coordinates
		float spritesHigherThanViewerOnElevPlatf = (isThatElevatedSpriteStillSeen) ? 1.25f : 1.0f;

		// the same we're doing when determinig which sprites are located slightly below but when observed from the top of elevations
		bool isViewerBottomHigherThanSpriteTop1 = viewerBottomAdj >= sprTopAdj;  // we need some Z tolerance here
		bool isThatLoweredSpriteStillSeen = isViewerBottomHigherThanSpriteTop1 && (viewerTopAdj >= sprTopAdj);
		// sprites that are located below and observed from the top of elevations, lower floors by current viewerTopAdj Z coordinates
		float spritesLowerThanViewerObservedFromElevPlatf = (isThatLoweredSpriteStillSeen) ? 0.8f : 1.0f;

		float ceilingHeightAdj1 = ceilingHeightInitial * spritesHigherThanViewerOnElevPlatf;
		float floorHeightAdj1 = floorHeightInitial * spritesLowerThanViewerObservedFromElevPlatf;


		// let's now try to occlude large sprites and projectiles behind tall enough obstructions
		float floorHeightMultiplier1 = ((isLegacyProjectile || isLargeSprite) && isObstructionTallEnough) ? 2.5f : 1.0f;
		float ceilingHeightMultiplier1 = ((isLegacyProjectile || isLargeSprite) && isObstructionTallEnough) ? 2.75f : 1.0f;
		// Calculate distance between viewer and sprite
		const FVector2 viewerPos = GetActorPosition(viewer);
		const FVector2 thingPos = GetActorPosition(thing);
		const float distanceSquared = (viewerPos - thingPos).LengthSquared();
		const float distance = sqrt(distanceSquared);

		// Enhanced obstruction check for large sprites/projectiles
		if (isLegacyProjectile || isLargeSprite)
		{
			// Calculate obstruction proximity factor (1.0 at 0 distance, 0.0 at 256 units)
			const float proximityFactor = clamp(1.0f - (distance / 256.0f), 0.0f, 1.0f);

			// Dynamic multipliers that increase as sprite gets closer to obstruction
			const float baseFloorMultiplier = 2.5f;
			const float baseCeilingMultiplier = 2.75f;

			// Apply proximity-based scaling
			floorHeightMultiplier1 = 1.0f + (baseFloorMultiplier - 1.0f) * proximityFactor;
			ceilingHeightMultiplier1 = 1.0f + (baseCeilingMultiplier - 1.0f) * proximityFactor;

			// Additional height-based adjustment for projectiles
			if (isLegacyProjectile)
			{
				// Projectiles get extra culling when near floor/ceiling
				// where 384.0f is a permittable view height range to allow culling
				const float heightFactor = 1.0f - fabs(thing->Z() - floorHeightInitial) / 384.0f;
				floorHeightMultiplier1 *= 1.0f + (0.5f * heightFactor);
			}
		}
		else
		{
			// Default behavior for regular sprites
			floorHeightMultiplier1 = 1.0f;
			ceilingHeightMultiplier1 = 1.0f;
		}

		// Apply the calculated multipliers
		float ceilingHeightAdj2 = ceilingHeightAdj1 * ceilingHeightMultiplier1;
		float floorHeightAdj2 = floorHeightAdj1 * floorHeightMultiplier1;

		minCeiling = MIN(minCeiling, ceilingHeightAdj2);
		maxCeiling = MAX(maxCeiling, ceilingHeightAdj2);
		minFloor = MIN(minFloor, floorHeightAdj2);
		maxFloor = MAX(maxFloor, floorHeightAdj2);

		valid = true;
	}

	bool IsSpriteVisible2sided(AActor* viewer, AActor* thing, const FVector3& pos, float height) const
	{
		if (!valid) return true;

		// Determine sprite classification again
		// Must be done via "AND" but "OR" works better
		const bool isLegacyProjectile =
			(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
			(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
			(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);

		const float spriteSize = (thing->radius + thing->Height) * 0.5f;
		const bool isSmallSprite = (spriteSize <= 24.0f);
		const bool isMediumSprite = (spriteSize > 24.0f && spriteSize <= 40.0f);
		const bool isLargeSprite = (spriteSize > 40.0f);
		const bool isRegularMonster = !isSmallSprite && !isLegacyProjectile;

		// Default doom view height
		float EyeHeight = 41.0f;
		if (viewer->player && viewer->player->mo)
		{
			EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
		}
		float viewerBottom = viewer->Z();
		float viewerTop = viewerBottom + EyeHeight;
		float viewerBottomAdj = viewerBottom - Ztolerance2sided;
		float viewerTopAdj = viewerTop - Ztolerance2sided;
		// large sprites have height 16 times than usual, so we must divide 1 by 16 for them
		float spriteBottom = thing->Z();
		float spriteTop = (thing->Z()) + (thing->Height);
		float sprBottomAdj = spriteBottom - Ztolerance2sided;
		float sprTopAdj = spriteTop + Ztolerance2sided;

		bool isViewerTopHigerThanSpriteTop = viewerTop >= spriteTop;
		if (isViewerTopHigerThanSpriteTop && !isSmallSprite)
		{
			EyeHeight += 16.0f;
		}

		bool isViewerBottomLowerThanSpriteBottom = viewerBottomAdj < sprBottomAdj;
		float cullSmallSpritesAboveHarderCoeff = (isViewerBottomLowerThanSpriteBottom) ? 0.15f : 1.0f;

		bool isViewerBottomHigherThanSpriteBottom = viewerBottom >= spriteBottom;
		float MoreTolerantInvCullAmountRestsized = (isViewerBottomHigherThanSpriteBottom) ? 1.0f : 0.15f;
		float MoreTolerantInvCullAmountSmallSpr = (isViewerBottomHigherThanSpriteBottom) ? cullSmallSpritesAboveHarderCoeff : 0.15f;
		float invCullAmount = (isSmallSprite) ? MoreTolerantInvCullAmountSmallSpr : MoreTolerantInvCullAmountRestsized;
		float viewerTopAdjCulled = viewerTop * invCullAmount;

		bool floorBlocks = false;
		float floorVariation = maxFloor - minFloor;
		if (floorVariation >= viewerTopAdjCulled)
		{
			floorBlocks = (sprBottomAdj <= maxFloor + EPSILON2SIDED);
		}

		bool ceilingBlocks = false;
		float ceilingVariation = maxCeiling - minCeiling;
		if (ceilingVariation >= viewerTopAdjCulled)
		{
			ceilingBlocks = (sprTopAdj >= minCeiling - EPSILON2SIDED);
		}

		return !floorBlocks && !ceilingBlocks;
	}
};

// Line-intersection test
static bool LineIntersectsSegment2sided(const FVector2& seg1Start, const FVector2& seg1End, const FVector2& seg2Start, const FVector2& seg2End, FVector2& intersectionPoint)
{
	// Cross product helper - same as LineSegmentCommon implementation
	const auto cross = [](const FVector2& a, const FVector2& b)
	{
		return a.X*b.Y - a.Y*b.X;
	};

	const FVector2 d1 = seg1End - seg1Start;
	const FVector2 d2 = seg2End - seg2Start;

	// Relative vectors (vectors from seg1Start to seg2 points)
	const FVector2 relA = seg2Start - seg1Start;
	const FVector2 relB = seg2End - seg1Start;

	// Cross products for segment 1 straddle check
	const float cross1 = cross(d1, relA);
	const float cross2 = cross(d1, relB);

	// If same sign (not straddling) - no intersection
	if (cross1 * cross2 > EPSILON2SIDED) return false;

	// Relative vectors (vectors from seg2Start to seg1 points)
	const FVector2 relC = seg1Start - seg2Start;
	const FVector2 relD = seg1End - seg2Start;

	// Cross products for segment 2 straddle check
	const float cross3 = cross(d2, relC);
	const float cross4 = cross(d2, relD);

	// If same sign (not straddling) - no intersection
	if (cross3 * cross4 > EPSILON2SIDED) return false;

	// Calculate intersection point (now that we know they intersect)
	// Division happens only once per intersection
	const float det = cross(d1, d2);
	if (fabs(det) < EPSILON2SIDED) return false; // Parallel case

	const float t = cross(relA, d2) / det;
	intersectionPoint = seg1Start + d1 * t;

	return true;
}

// Sides of the world helper.
// Helper template to get bit count from a bitmask.
// For example, if mask is 0b100101, this returns 3.
inline int CountBits2sidedObstr(unsigned int mask)
{
	int count = 0;
	for (; mask; count++)
	{
		mask &= mask - 1; // clear the least significant bit set
	}
	return count;
}

// Helper function for distance calculation
static float SegmentPointDistanceSqForCheckLineOfSight2Sided(const FVector2& segA, const FVector2& segB, const FVector2& point)
{
	FVector2 segVec = segB - segA;
	FVector2 ptVec = point - segA;

	float segLengthSq = segVec.LengthSquared();
	float t = (ptVec | segVec) / segLengthSq;
	t = clamp(t, 0.0f, 1.0f);

	FVector2 projection = segA + segVec * t;
	return (point - projection).LengthSquared();
}

// Precomputed direction vectors (E, S, W, N)
static const FVector2 directionVectors[4] =
{
	{1.0f, 0.0f},   // East
	{0.0f, 1.0f},   // South
	{-1.0f, 0.0f},  // West
	{0.0f, -1.0f}   // North
};

// Optimized Implementation of CheckLineOfSight2sided with extended radius
static bool CheckLineOfSight2sided(AActor* viewer, AActor* thing)
{
	// No obstructions found means true
	if (CheckFrustumCulling(thing)) return true;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return true;

	// Eye height calculation
	float EyeHeight2 = 41.0f;
	if (viewer->player && viewer->player->mo)
	{
		EyeHeight2 = viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta;
	}

	// Determine sprite classification again
	// Must be done via "AND" but "OR" works better
	const bool isLegacyProjectile =
		(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
		(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
		(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);

	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool isSmallSprite = (spriteSize <= 18.0f);
	const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 40.0f);
	const bool isLargeSprite = (spriteSize > 40.0f);
	const bool isRegularMonster = !isSmallSprite && !isLegacyProjectile;

	const bool isProjectileLargeSprite = (isLegacyProjectile || isLargeSprite);

	// Viewer positions
	float viewerBottom = viewer->Z();
	float viewerTop = viewerBottom + EyeHeight2;
	float viewerBottomAdj = viewerBottom - Ztolerance2sided;
	float viewerTopAdj = viewerTop + Ztolerance2sided;

	// ==================================================================================================
	// THIS IS THE PLACE WHERE YOU CONFIGURE SPRITES CULLING AMOUNT PER TYPE FOR 2SIDED TALL OBSTRUCTIONS
	// 1st val is large spr, 2nd is small sprites, third is all the rest sprites, higher vals cull more
	const float RadiusExpansionFactor = isProjectileLargeSprite ? (isSmallSprite ? 8.5f : 1.75f) : 6.0f;
	const float HeightExpansionFactor = isProjectileLargeSprite ? (isSmallSprite ? 12.0f : 4.0f) : 1.64f;

	// Core sprite positions (using expansion factors)
	const FVector2 viewerPos = GetActorPosition(viewer);
	const FVector2 thingCenterPos = GetActorPosition(thing);
	float spriteBottom = thing->Z();
	float spriteTop = thing->Z() + thing->Height * HeightExpansionFactor;
	float sprBottomAdj = spriteBottom - Ztolerance2sided;
	float sprTopAdj = spriteTop + Ztolerance2sided;
	float spriteMidHeight = spriteBottom + ((spriteTop - spriteBottom) * 0.5f);

	// Direction checks setup
	unsigned int directionMask = isProjectileLargeSprite ? 0xF : 0x0;
	int numDirectionChecks = CountBits2sidedObstr(directionMask);
	bool potentialObstructionFound = false;

	// Loop through which end of the viewer's eye line to test
	for (int EyeLevel = 0; EyeLevel < 2; EyeLevel++)
	{
		const float camHeight = (EyeLevel == 0) ? viewerBottomAdj : viewerTopAdj;

		// Initialize the test point to the sprite's center. We only deviate for large sprites.
		FVector2 testPoint = thingCenterPos;

		// We only do the detailed check if we have directions to test.
		for (int testIdx = 0; testIdx <= numDirectionChecks; ++testIdx)
		{
			if (testIdx > 0)
			{
				// if this isn't the initial center test
				int shift = testIdx - 1;
				// Get the actual direction from the mask
				unsigned int currentDir = (1 << shift);
				if (!(directionMask & currentDir)) continue;

				// Use precomputed direction vector instead of trig functions
				testPoint = thingCenterPos + directionVectors[shift] * (thing->radius * RadiusExpansionFactor);
			}

			// Check both sectors (viewer & sprite)
			for (int SectorCheck = 0; SectorCheck < 2; SectorCheck++)
			{
				const sector_t* sector = (SectorCheck == 0) ? viewer->Sector : thing->Sector;

				for (auto line : sector->Lines)
				{
					LineSegmentCommon sight(viewerPos.X, viewerPos.Y, testPoint.X, testPoint.Y);

					LineSegmentCommon wall(line->v1->fX(), line->v1->fY(), line->v2->fX(), line->v2->fY());

					if (!sight.IntersectsCommon(wall))
					{
						continue;
					}

					// Calculate intersection point if needed
					FVector2 intersectionPoint;
					if (!LineIntersectsSegment2sided(viewerPos, testPoint, GetVertexPosition(line->v1), GetVertexPosition(line->v2), intersectionPoint))
					{
						continue;
					}
				}
			}

			// Check both sectors (viewer & sprite)
			for (int SectorCheck = 0; SectorCheck < 2; SectorCheck++)
			{
				const sector_t* sector = (SectorCheck == 0) ? viewer->Sector : thing->Sector;

				for (auto line : sector->Lines)
				{
					const FVector2 lineStart = GetVertexPosition(line->v1);
					const FVector2 lineEnd = GetVertexPosition(line->v2);
					FVector2 intersectionPoint;

					if (!LineIntersectsSegment2sided(viewerPos, testPoint, lineStart, lineEnd, intersectionPoint))
					{
						continue;
					}

					// When we're on top of elevation and shoot a rocket from above to below
					// We can observe it culls too much because our elevation cuts it, thus
					// Enhanced back-face culling (checks if line faces towards the sprite's center)
					//const DVector2 lineVec = lineEnd - lineStart;
					//DVector2 lineNormal(-lineVec.Y, lineVec.X);
					//lineNormal = lineNormal.Unit();
					//const DVector2 viewToIntersect = intersectionPoint - viewerPos;
					//DVector2 thingToIntersect = intersectionPoint - thingCenterPos;


					//	// ===============================================================
					//	// *** FACE CHECK SKIPPING LOGIC - START ***    WIP
					//
					//	// Track potential obstructions
					//potentialObstructionFound = true;
					//
					//	// Vertical separation check with tolerance buffer
					//const float verticalTolerance = 24.0f;
					//bool spriteIsBelow = (sprTopAdj + verticalTolerance) < viewerBottom;
					//bool spriteIsAbove = (sprBottomAdj - verticalTolerance) > viewerTop;
					//
					//if (spriteIsBelow || spriteIsAbove)
					//{
					//	//Printf("Z-SKIP: Sprite (%.1f-%.1f) vertical separation from viewer (%.1f-%.1f)", sprBottomAdj, sprTopAdj, viewerBottom, viewerTop);
					//	continue; // Skip facing check for vertically separated sprites
					//}
					//
					//	// Perform facing direction check
					//const DVector2 lineDelta = line->Delta();
					//DVector2 lineNormal(-lineDelta.Y, lineDelta.X);
					//lineNormal = lineNormal.Unit();
					//
					//const DVector2 viewToIntersect = intersectionPoint - viewerPos;
					//
					//if (viewToIntersect.Length() > EPSILON2SIDED)
					//{
					//	const DVector2 viewDir = viewToIntersect.Unit();
					//	double dot = (viewDir | lineNormal);
					//
					//	if (dot > 0.25) // More lenient facing threshold
					//	{
					//		//Printf("OCCLUDED: Line %d faces viewer (dot=%.2f). Sprite (%.1f-%.1f) blocked.", line->Index(), dot, spriteBottom, spriteTop);
					//		return false; // Immediate occlusion
					//	}
					//	else
					//	{
					//		//Printf("Line %d faces away (dot=%.2f) - no occlusion", line->Index(), dot);
					//	}
					//}
					//	// *** FACE CHECK SKIPPING LOGIC - FINISH ***WIP
					//	// ===============================================================

					// Clamp intersection point
					const FVector2 clamped =
					{
						clamp<float>(intersectionPoint.X, MINCOORD2SIDED, MAXCOORD2SIDED),
						clamp<float>(intersectionPoint.Y, MINCOORD2SIDED, MAXCOORD2SIDED)
					};

					ObstructionData2Sided obsData;

					if (line->flags & ML_TWOSIDED)
					{
						if (line->frontsector) obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
						if (line->backsector)  obsData.Update2sidedTallObstructions(thing, viewer, line->backsector, clamped);
					}
					else
					{
						if (line->frontsector) obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
					}

					// Final visibility check with expanded height
					if (!obsData.IsSpriteVisible2sided(viewer, thing, FVector3{ testPoint.X, testPoint.Y, spriteMidHeight }, spriteTop))
					{
						return false; // Sprite is occluded
					}
				}

				if (viewer->Sector == thing->Sector) break; // Skip duplicate sector check
			}
		}
	}

	return true; // No obstructions found
}

// this function determines visibility of sprites behind tall enough 2-sided-linedef based obstructions, call it like that:
// bool visible2sideTallEnoughObstr = IsSpriteVisibleBehind2sidedLinedefbasedSectorObstructions(r_viewpoint.camera, thing);
bool IsSpriteVisibleBehind2sidedLinedefbasedSectorObstructions(AActor* viewer, AActor* thing)
{
	if (!viewer || !thing || viewer == thing) return true;

	// We call the function with "!" - that's why return "true" when culled
	if (CheckFrustumCulling(thing)) return true;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return true;

	// 3. Full occlusion test
	return CheckLineOfSight2sided(viewer, thing);
}



// Cache structure - stores only what we need
struct Visibility2sidedObstrCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached2sidedObstrResult = true;
};

// Global cache storage
static TMap<AActor*, Visibility2sidedObstrCacheEntry> Visibility2sidedObstrCache;

// this function determines visibility of sprites behind tall enough 2-sided-linedef based obstructions, call it like that:
// bool visible2sideTallEnoughObstr = IsSpriteVisibleBehind2sidedLinedefbasedSectorObstructions(r_viewpoint.camera, thing);
bool IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(AActor* viewer, AActor* thing)
{
	if (!viewer || !thing || viewer == thing) return true;

	// We call the function with "!" - that's why return "true" when culled
	if (CheckFrustumCulling(thing)) return true;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return true;

	// 3. Occlusion test - choose implementation based on toggle
	if (enableAnamorphCache)
	{
		// CACHED VERSION
		const int currentMapTimeTick = level.maptime;
		Visibility2sidedObstrCacheEntry& entry = Visibility2sidedObstrCache[viewer, thing];

		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached2sidedObstrResult;
		}

		entry.cached2sidedObstrResult = CheckLineOfSight2sided(viewer, thing);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return entry.cached2sidedObstrResult;
	}
	else
	{
		// UNCACHED VERSION
		return CheckLineOfSight2sided(viewer, thing);
	}
}
// ******* 2-sided-linedef tall enough sector obstructions culling block finish *******
//         ---===      ***************************************        ===---



//         ---===      ***************************************        ===---
// ******* 2-sided-linedef-based mid-texture in front of a sprite facing camera - start *******
//		"CheckFacingMidTextureProximity" - finds out whether a mid texture facing camera
//		lies on viewdirection between viewer and sprite behind that mid texture and returns
//		a proximity value from 0 to 1, where 1 is fully visible. Large sprites must come at
//		least 32 units to the texture to be fully occluded, all the rest sized ones - 64u.
//		Also as soon as viewer comes very close to the sprite behind the mid texture, that
//		immediately returns proximity of 1.0f which means no more occlusion, because sprites
//		don't leak through mid textures on such low distances due to how GL depth works, so
//		the viewer would have less chance seeing sprite chopped.
//							I'd recommed to call the function like this:
//		float behindFacingMidTxtProximity = CheckFacingMidTextureProximity(thing, r_viewpoint.camera, thingpos);
//		bool visbible2sideMidTex = (behindFacingMidTxtProximity <= 0.55f) ? false : true;
static float CheckFacingMidTextureProximity(AActor* thing, const AActor* viewer, TVector3<double>& thingpos)
{
	//Printf("===== TEXTURE PROXIMITY CHECK START =====\n");
	//Printf("Thing: %s at (%.1f, %.1f, %.1f)\n", thing->GetClass()->TypeName.GetChars(), thingpos.X, thingpos.Y, thingpos.Z);
	//Printf("Camera: (%.1f, %.1f, %.1f) facing %.1f degrees\n", camera->X(), camera->Y(), camera->Z(), camera->Angles.Yaw.Degrees());

	// 1. Quick out: Frustum culling
	if (CheckFrustumCulling(thing))
	{
		//Printf("Skipped: Frustum culled\n");
		return 0.0f;
	}

	// 2. Distance culling (far planes)
	if (IsAnamorphicDistanceCulled(thing, 2048.0f))
	{
		//Printf("Skipped: Distance culled (2048+ units away)\n");
		return 0.0f;
	}

	// 3. Actor classification flags and thresholds
	const bool isLegacyVersionProjectile = thing->flags & (MF_MISSILE | MF_NOBLOCKMAP | MF_NOGRAVITY) || thing->flags2 & (MF2_IMPACT | MF2_NOTELEPORT | MF2_PCROSS);
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool isLargeSprite = (spriteSize > 40.0f);

	// Get vertical positioning info
	float EyeHeight = 41.0f;
	if (viewer->player && viewer->player->mo)
	{
		EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
		//Printf("EyeHeight1: %.2f (ViewHeight: %.2f + crouchdelta: %.2f)\n", EyeHeight1, viewer->player->mo->FloatVar(NAME_ViewHeight), viewer->player->crouchviewdelta);
	}
	else
	{
		//Printf("EyeHeight1: %.2f (default value, no player object)\n", EyeHeight1);
	}
	float viewerBottom = viewer->Z();
	float viewerTop = viewerBottom + EyeHeight;
	float viewerBottomAdj = viewerBottom - Ztolerance2sided;
	float viewerTopAdj = viewerTop + Ztolerance2sided;
	float spriteBottom = thing->Z();
	float spriteTop = (thing->Z()) + (thing->Height);
	float sprBottomAdj = spriteBottom - Ztolerance2sided;
	float sprTopAdj = spriteTop + Ztolerance2sided;

	// pretty useless
	//bool viewerFeetLowerThanSpriteFeetAndStillSeen = (viewerBottomAdj <= sprBottomAdj) <= EyeHeight;
	//bool viewerFeetHigherThanSpriteHeadAndStillSeen = viewerBottomAdj <= sprTopAdj >= EyeHeight;
	// pretty useless and safer for compilation - how to remove leaking through mid tex when we're below it? I don't know yet
	bool viewerFeetLowerThanSpriteFeetAndStillSeen = (viewerBottomAdj <= sprBottomAdj) && ((sprBottomAdj - viewerBottomAdj) <= EyeHeight);
	bool viewerFeetHigherThanSpriteHeadAndStillSeen = (viewerBottomAdj >= sprTopAdj) && ((viewerBottomAdj - sprTopAdj) <= EyeHeight);


	//Printf("  Actor type: %s%s (size: %.1f)\n", isLegacyProjectile ? "Projectile " : "", isLargeSprite ? "Large" : "Standard", spriteSize);

	// 4. Proximity thresholds where occlusion is disabled (to prevent popping)
	const float CLOSE_DIST_SMALL = 64.0f;        // Standard avoidance distance
	const float CLOSE_DIST_LARGE = 96.0f;        // Extended for large entities

	// 5. Measured distance from camera to thing
	float distToCamSq = (thingpos - TVector3<double>(viewer->X(), viewer->Y(), viewer->Z())).LengthSquared();

	bool isViewerVerticallyAligned = (viewerBottomAdj <= spriteBottom) || (viewerBottom >= spriteTop);
	float minDist = (isLegacyVersionProjectile || isLargeSprite) ? CLOSE_DIST_LARGE : CLOSE_DIST_SMALL;

	// 6. If very close and vertically aligned - full visibility (disable occlusion)
	// because sprites don't override walls depth that hard when close and viewer is not under or above the sprite
	if (isViewerVerticallyAligned && distToCamSq <= minDist * minDist)
	{
		//Printf("Close proximity (%.1f units): Full visibility (factor=1.0)\n", sqrt(distToCamSq));
		return 1.0f;
	}
	//Printf("Distance to camera: %.1f units\n", sqrt(distToCamSq));

	// 7. Set detection radii based on actor type
	float MAX_DIST_SOLID = 64.0f;    // Standard solid wall max distance
	float MAX_DIST_MASKED = 96.0f;   // Masked textures have see-through parts
	const float EDGE_BUFFER = 32.0f; // Safety margin from texture edges

	// Adaptive adjustments for special actor types
	if (isLegacyVersionProjectile)
	{
		MAX_DIST_SOLID = 80.0f;
		MAX_DIST_MASKED = 112.0f;
		//Printf("  Using projectile detection ranges: SOLID=%.1f, MASKED=%.1f\n", MAX_DIST_SOLID, MAX_DIST_MASKED);
	}
	else if (isLargeSprite)
	{
		MAX_DIST_SOLID = 96.0f;
		MAX_DIST_MASKED = 128.0f;
		//Printf("Using large sprite detection ranges: SOLID=%.1f, MASKED=%.1f\n", MAX_DIST_SOLID, MAX_DIST_MASKED);
	}
	//else
	//{
	//    Printf("Using standard detection ranges: SOLID=%.1f, MASKED=%.1f\n", MAX_DIST_SOLID, MAX_DIST_MASKED);
	//}

	float proximity_factor = 1.0f;

	// 8. Calculate view direction vector
	float yawRadians = viewer->Angles.Yaw.Radians();
	TVector2<float> viewDir(cos(yawRadians), sin(yawRadians));
	//Printf("  View direction: (%.3f, %.3f)\n", viewDir.X, viewDir.Y);

	// 9. Iterate sector lines (with comprehensive checks)
	sector_t* sector = thing->Sector;
	//Printf("Checking %u lines in sector %d\n", sector->Lines.Size(), sector->Index());
	for (auto line : sector->Lines)
	{
		//Printf("\n  --- Checking Line %d (flags: %04X) ---\n", line->Index(), line->flags);

		// Minimal line validation - must be two-sided (game logic requirement)
		if (!line->sidedef[0] || !line->sidedef[1])
		{
			//Printf("Skipped: Missing front/back sidedef\n");
			continue;
		}

		// 10. MIDTEXTURE EXISTENCE CHECK
		bool hasValidMidTexture = false;
		for (int sideno = 0; sideno < 2; sideno++)
		{
			FTextureID midtex = line->sidedef[sideno]->GetTexture(side_t::mid);
			if (midtex.isValid() && TexMan[midtex])
			{
				hasValidMidTexture = true;
				//Printf("Side %d has valid MID texture: %s\n", sideno, TexMan[midtex]->Name.GetChars());
				break;
			}
		}
		if (!hasValidMidTexture)
		{
			//Printf("Skipped: No valid MID textures on either side\n");
			continue;
		}

		// 11. PRECISION OCCLUSION DETECTION
		TVector2<float> viewerPos(viewer->X(), viewer->Y());
		TVector2<float> thingPos2D(thingpos.X, thingpos.Y);

		// Create camera->thing visibility ray
		TVector2<float> rayVec = thingPos2D - viewerPos;
		float rayLen = rayVec.Length();
		TVector2<float> rayDir = rayVec.Unit();

		//Printf("Ray: from (%.1f,%.1f) to (%.1f,%.1f) len=%.1f dir(%.3f,%.3f)\n", cameraPos.X, cameraPos.Y, thingPos2D.X, thingPos2D.Y, rayLen, rayDir.X, rayDir.Y);

		// Line geometry data
		TVector2<float> lineA(line->v1->fX(), line->v1->fY());
		TVector2<float> lineB(line->v2->fX(), line->v2->fY());
		TVector2<float> lineV = lineB - lineA;

		//Printf("Line: (%.1f,%.1f)->(%.1f,%.1f) vec(%.1f,%.1f)\n",lineA.X, lineA.Y, lineB.X, lineB.Y, lineV.X, lineV.Y);

		// Ray-line intersection math
		float denom = rayDir.X * lineV.Y - rayDir.Y * lineV.X;
		if (fabs(denom) < 1e-10)
		{
			//Printf("Skipped: Ray parallel to line (denom=%.4f)\n", denom);
			continue;
		}

		TVector2<float> delta = lineA - viewerPos;
		float t = (delta.X * lineV.Y - delta.Y * lineV.X) / denom;
		float u = (delta.X * rayDir.Y - delta.Y * rayDir.X) / denom;

		//Printf("Intersection params: t=%.4f, u=%.4f\n", t, u);

		// Validate segment intersection
		if (t < 0.0 || t > rayLen || u < -1e-5 || u > 1.00001)
		{
			//Printf("Skipped: Intersection outside segments (t=%.1f on ray, u=%.1f on line)\n", t, u);
			continue;
		}

		// Surface orientation check
		TVector2<float> lineNormal(-lineV.Y, lineV.X);
		lineNormal = lineNormal.Unit();
		if ((rayDir | lineNormal) < 0)
		{
			//Printf("Skipped: Facing back side of line (normal dot=%.2f)\n", rayDir | lineNormal);
			continue;
		}

		TVector2<float> hitPoint = viewerPos + rayDir * t;
		float distToIntersect = (thingPos2D - hitPoint).Length();
		//Printf("Valid intersection at (%.1f,%.1f), %.1f units from thing\n", hitPoint.X, hitPoint.Y, distToIntersect);

		// 12. VERTICAL MIDTEXTURE RANGE CHECK
		double textop_d, texbot_d; // Doubles to call the function
		if (P_GetMidTexturePosition(line, 0, &textop_d, &texbot_d))
		{
			// Convert results to floats immediately after the call
			const float textop = static_cast<float>(textop_d);
			const float texbot = static_cast<float>(texbot_d);

			//Printf("Vertical check: thingZ=%.1f-%.1f, tex=%.1f-%.1f\n", thingpos.Z, thingTopZ, texbot, textop);

			if (viewerBottomAdj <= texbot || sprTopAdj >= textop)
			{
				//Printf("Skipped: Sprite outside midtexture vertical range\n");
				continue;
			}
		}
		//else
		//{
		//    Printf("No vertical midtexture position data\n");
		//}

		// 13. DISTANCE TO LINE SEGMENT CALCULATION
		DVector2 v1(line->v1->fX(), line->v1->fY());
		DVector2 v2(line->v2->fX(), line->v2->fY());
		DVector2 lineVec = v2 - v1;
		double lineLenSq = lineVec.LengthSquared();
		if (lineLenSq < 1e-6)
		{
			//Printf("Skipped: Degenerate line (length squared=%.4f)\n", lineLenSq);
			continue;
		}

		DVector2 toSprite(thingpos.X - v1.X, thingpos.Y - v1.Y);
		double dot = lineVec.X * toSprite.X + lineVec.Y * toSprite.Y;
		float t_segment = clamp(float(dot / lineLenSq), 0.0f, 1.0f);

		DVector2 closest(v1.X + t_segment * lineVec.X, v1.Y + t_segment * lineVec.Y);
		float dist = float((thingpos - DVector3(closest, 0)).XY().Length());

		//Printf("Closest point on line: (%.1f,%.1f), distance=%.1f\n", closest.X, closest.Y, dist);

		// 14. ADAPTIVE TEXTURE TYPE HANDLING
		bool isSolid = false;
		for (int sideno = 0; !isSolid && sideno < 2; sideno++)
		{
			FTexture* tex = TexMan[line->sidedef[sideno]->GetTexture(side_t::mid)];
			if (tex && !tex->bMasked)
			{
				isSolid = true;
				//Printf("Found solid texture on side %d: %s\n", sideno, tex->Name.GetChars());
			}
		}

		float maxDist = isSolid ? MAX_DIST_SOLID : MAX_DIST_MASKED;
		float threshold = maxDist + EDGE_BUFFER;

		//Printf("Texture type: %s, maxDist=%.1f, threshold=%.1f\n",
		//       isSolid ? "SOLID" : "MASKED", maxDist, threshold);

		if (dist > threshold)
		{
			//Printf("Skipped: Too far from line (%.1f > %.1f)\n", dist, threshold);
			continue;
		}

		// 15. PROXIMITY FACTOR CALCULATION
		float distFromEdge = MAX(0.0f, dist - EDGE_BUFFER);
		float factor = lerp(0.25f, 1.0f, distFromEdge / maxDist);
		factor = clamp(factor, 0.25f, 1.0f);
		proximity_factor = MIN(proximity_factor, factor);

		//Printf("Proximity factor: %.2f (current min: %.2f)\n", factor, proximity_factor);
	}

	//Printf("===== FINAL PROXIMITY FACTOR: %.2f =====\n", proximity_factor);

	return proximity_factor;
	// if our feet are lower than sprite feet behind a mid-tex mirror for elevated case - cull it
	if (viewerFeetLowerThanSpriteFeetAndStillSeen || viewerFeetHigherThanSpriteHeadAndStillSeen)
	{
		return proximity_factor = 1.0f; // cull sprite
	}
}

struct MidTextureProximityCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	float cachedProximityFactor = 1.0f;  // Changed to float
};

static TMap<AActor*, MidTextureProximityCacheEntry> MidTextureProximityCache;

bool CheckFacingMidTextureProximityWrapper(AActor* thing, AActor* viewer, TVector3<double>& thingpos)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		MidTextureProximityCacheEntry& entry = MidTextureProximityCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			// Use cached proximity factor to compute boolean
			return entry.cachedProximityFactor >= 0.55f;
		}

		// Compute fresh proximity factor and cache it
		entry.cachedProximityFactor = CheckFacingMidTextureProximity(thing, viewer, thingpos);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;

		// Convert cached proximity to boolean
		return entry.cachedProximityFactor > 0.55f;
	}
	else
	{
		// Original uncached behavior
		float proximity = CheckFacingMidTextureProximity(thing, viewer, thingpos);
		return proximity >= 0.55f;
	}
}

//         ---===      ***************************************        ===---
// ******* 2-sided-linedef-based mid-texture in front of a sprite facing camera - finish *******



//         ---===      ***************************************        ===---
// ******* closed doors in front of sprites detection - start *******
//                      NOT IMPLEMENTED YET BUT EVEN NEEDED?
// ******* closed doors in front of sprites detection - start *******
//         ---===      ***************************************        ===---



//         ---===      ***************************************        ===---
// ******* 3DFloor-planar - floor and ceiling culling block start *******
static DVector3 GetSpriteOcclusionPoint3DFloors(AActor* thing, DVector3& pos)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return DVector3(0, 0, 0);

	DVector3 occlusionPos = pos;  // Start with original position

	// Only adjust RF_FACESPRITE types
	if ((thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
	{
		// Could be "and" but "or" works better
		const bool islegacyversionprojectile =
			(thing->flags & MF_MISSILE) ||
			(thing->flags & MF_NOBLOCKMAP) ||
			(thing->flags & MF_NOGRAVITY) ||
			(thing->flags2 & MF2_IMPACT) ||
			(thing->flags2 & MF2_NOTELEPORT) ||
			(thing->flags2 & MF2_PCROSS);

		const float base_radius = thing->radius;
		const float base_height = thing->Height;
		const float base_dimen = (base_radius + base_height) * 0.5;

		const bool islittlesprite = (base_dimen <= 18.0f);
		const bool isactoracorpse = (thing->flags & MF_CORPSE) || (thing->flags & MF_ICECORPSE);
		const bool isactorsmallbutnotcorpse = islittlesprite && !isactoracorpse;

		if (isactorsmallbutnotcorpse)
		{
			// Expand virtual height dramatically for sprites with extended radius to block more agressively
			occlusionPos.Z += thing->Height * 2.0;
			// Expand horizontally by 1600%
			occlusionPos += DVector3(thing->radius * 16.0, thing->radius * 16.0, 0);
		}
	}

	return occlusionPos;
}

// Shared filter to determine relevant 3D floors
static bool IsRelevantFFloor3DFloors(F3DFloor* rover, AActor* thing)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return 0;

	return (rover->flags & FF_EXISTS) &&
		(rover->flags & FF_SOLID) &&
		(rover->flags & FF_RENDERPLANES) &&
		!(rover->flags & FF_TRANSLUCENT)

		//      no need to check those yet

		//		&& (rover->alpha >= 200)
		;
}

// Modified to use absolute sprite bottom
static float GetActualSpriteFloorZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return pos.Z;

	float height = s->floorplane.ZatPoint(pos);
	float spriteBottom = thing->Z(); // Absolute bottom position

	for (auto rover : s->e->XFloor.ffloors)
	{
		if (!IsRelevantFFloor3DFloors(rover, thing)) continue;

		float rover_z = rover->top.plane->ZatPoint(pos);
		if (rover_z > height && rover_z <= spriteBottom + 2.0f) // 2-unit tolerance
			height = rover_z;
	}
	return height;
}

// Modified to use absolute sprite top
static float GetActualSpriteCeilingZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0)) return pos.Z;

	float height = s->ceilingplane.ZatPoint(pos);
	float spriteTop = thing->Z() + thing->Height; // Absolute top position

	for (auto rover : s->e->XFloor.ffloors)
	{
		if (!IsRelevantFFloor3DFloors(rover, thing)) continue;

		float rover_z = rover->bottom.plane->ZatPoint(pos);
		if (rover_z < height && rover_z >= spriteTop - 2.0f) // 2-unit tolerance
			height = rover_z;
	}
	return height;
}

// Helper function: Find intersection between plane and line
static bool PlaneLineIntersection3DFloors(AActor* thing, secplane_t* plane, DVector3& start, DVector3& end, DVector3& out)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return true;

	const DVector3& n = plane->Normal();
	float planeD = plane->fD();

	DVector3 dir = end - start;
	float denominator = n | dir;                 // Dot product

	if (fabs(denominator) < 1e-8) return false;  // Line is parallel to plane

	float t = -(planeD + (n | start)) / denominator;
	if (t < 0.0 || t > 1.0) return false;        // Intersection point not between start and end

	out = start + t * dir;
	return true;
}

// Helper to get sector bounds
static void GetSectorBounds3DFloors(const sector_t* sec, DVector2 &minPoint, DVector2 &maxPoint)
{
	const float MINCOORD2SIDED3DFLOOR = -32768.0;
	const float MAXCOORD2SIDED3DFLOOR = 32767.9999847;

	minPoint = { MAXCOORD2SIDED3DFLOOR, MAXCOORD2SIDED3DFLOOR };
	maxPoint = { -MINCOORD2SIDED3DFLOOR, -MINCOORD2SIDED3DFLOOR };

	for (auto line : sec->Lines)
	{
		const DVector2 v1(line->v1->fX(), line->v1->fY());
		const DVector2 v2(line->v2->fX(), line->v2->fY());

		minPoint.X = MIN(minPoint.X, MIN(v1.X, v2.X));
		minPoint.Y = MIN(minPoint.Y, MIN(v1.Y, v2.Y));
		maxPoint.X = MAX(maxPoint.X, MAX(v1.X, v2.X));
		maxPoint.Y = MAX(maxPoint.Y, MAX(v1.Y, v2.Y));
	}
}

bool IsSpriteBehind3DFloorPlane(DVector3& cameraPos, DVector3& spritePos, sector_t* sector, AActor* thing)
{
	if (!sector || !sector->e)
		return false;

	// We call the function with "!" - that's why return "true" when culled
	if (CheckFrustumCulling(thing)) return true;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return true;

	const float TOLERANCE = 16.0f;    // Vertex-aligned sector tolerance
	const float EPSILON = 8.0f;       // Vertical comparison safety
	const float HORIZ_SAFETY = 32.0f; // Horizontal expansion

	AActor* viewer = players[consoleplayer].camera;
	const float viewerBottom = viewer->Z();  // Absolute bottom position
	const float viewerTop = viewerBottom + viewer->Height;  // Absolute top position

	for (auto rover : sector->e->XFloor.ffloors)
	{
		if (!IsRelevantFFloor3DFloors(rover, thing))
			continue;

		// 1. Get the 3D floor's target sector
		sector_t* target = rover->target;

		// 2. Calculate expanded bounds
		DVector2 minBound, maxBound;
		GetSectorBounds3DFloors(target, minBound, maxBound);
		minBound -= HORIZ_SAFETY;
		maxBound += HORIZ_SAFETY;

		// 3. Check if both camera and sprite are within bounds
		DVector2 camXY = cameraPos.XY();
		DVector2 sprXY = spritePos.XY();

		bool camInBounds = camXY.X >= minBound.X && camXY.X <= maxBound.X && camXY.Y >= minBound.Y && camXY.Y <= maxBound.Y;
		bool sprInBounds = sprXY.X >= minBound.X && sprXY.X <= maxBound.X && sprXY.Y >= minBound.Y && sprXY.Y <= maxBound.Y;

		if (!camInBounds || !sprInBounds) continue;

		// 4. Get 3D floor plane heights at both positions
		const float floorTop = rover->top.plane->ZatPoint(cameraPos);
		const float floorBottom = rover->bottom.plane->ZatPoint(cameraPos);

		// 5. Special case: Camera is standing on this 3D floor
		bool cameraOnFloor = fabs(viewerBottom - floorTop) < EPSILON;

		// 6. Check vertical relationships
		if (cameraOnFloor)
		{
			// When standing on the 3D floor, only cull if sprite is below
			if (spritePos.Z < floorBottom - EPSILON) return true;
		}
		else
		{
			// Normal case: camera above floor, sprite below
			if (viewerBottom > floorTop + EPSILON && spritePos.Z < floorTop - EPSILON) return true;

			// Camera below floor, sprite above
			if (viewerTop < floorBottom - EPSILON && spritePos.Z > floorBottom + EPSILON) return true;
		}
	}

	return false;
}

// Cache structure for 3D floor plane checks
struct a3DFloorPlaneCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached3DFloorPlaneResult = false;
};

// Global cache storage
static TMap<AActor*, a3DFloorPlaneCacheEntry> a3DFloorPlaneCache;

bool IsSpriteBehind3DFloorPlaneWrapper(DVector3& cameraPos, DVector3& spritePos, sector_t* sector, AActor* thing)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		a3DFloorPlaneCacheEntry& entry = a3DFloorPlaneCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached3DFloorPlaneResult;
		}

		// Compute fresh result and cache it
		entry.cached3DFloorPlaneResult = IsSpriteBehind3DFloorPlane(cameraPos, spritePos, sector, thing);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return entry.cached3DFloorPlaneResult;
	}
	else
	{
		// Original uncached behavior
		return IsSpriteBehind3DFloorPlane(cameraPos, spritePos, sector, thing);
	}
}

// ******* 3DFloor-planar - floor and ceiling culling block finish *******
//         ---===      ***************************************        ===---




//         ---===      ***************************************        ===---
// ******* 3DFloor-sides culling block start *******
// Utility: get plane height at a 2D point
static inline float GetPlaneHeight(secplane_t* plane, const DVector2& pt)
{
	return plane->ZatPoint(pt);
}

bool IsSpriteVisibleBehind3DFloorSides(AActor* viewer, AActor* thing)
{
	constexpr float RAY_EPS = 1e-4;     // tolerance for ray-intersection tests
	constexpr float VERT_EPS = 1e-5;    // tolerance for vertical visibility checks

	// 1) Preliminary checks
	if (!viewer || !thing) return true;
	if (viewer->Sector == thing->Sector) return true;   // same sector – no occlusion

	// Distance culling – keep your existing method
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// 2) Ray geometry
	const TVector2<float> vd(viewer->Pos().X, viewer->Pos().Y);   // viewer
	const TVector2<float> sd(thing->Pos().X, thing->Pos().Y);     // sprite

	const TVector2<float> rayVec = sd - vd;
	const float rayLen = rayVec.Length();

	// Guard against degenerate rays (viewer==sprite)
	if (rayLen == 0.0) return true;

	const TVector2<float> rayDir = rayVec.Unit();                 // <-- Use Unit()

	const float spriteHeight = thing->Height;

	// 3) Helper: test a single sector for blocking 3D floors
	auto checkSectorForFloor = [&](sector_t* sector) -> bool
	{
		if (!sector || sector->e->XFloor.ffloors.Size() == 0)
			return true;                // no 3D floors here

		// Mid-point on the ray – we use this point to sample plane heights
		TVector2<float> midPt = (sd + vd) * 0.5;

		for (auto& floor : sector->e->XFloor.ffloors)
		{
			// Only solid 3D floors can occlude a sprite
			// EDIT - REALLY?
			// if (!(floor->flags & FF_SOLID)) continue;

			// Check that the pointer to a plane exists
			if (!floor->bottom.plane || !floor->top.plane) continue;

			// get the floor / ceiling heights at our sample point
			float floorHeight = floor->bottom.plane->ZatPoint(midPt.X, midPt.Y);
			float ceilingHeight = floor->top.plane->ZatPoint(midPt.X, midPt.Y);

			// Check line-of-sight crossing this 3D floor
			// Parametric t for intersection with floor & ceiling planes
			float tFloor = (floorHeight - viewer->Pos().Z) / (thing->Pos().Z - viewer->Pos().Z);
			float tCeil = (ceilingHeight - viewer->Pos().Z) / (thing->Pos().Z - viewer->Pos().Z);

			auto testIntersection = [&](float t)->bool
			{
				// Intersection must lie on the segment
				if (t < -RAY_EPS || t > 1.0 + RAY_EPS) return true;

				// Intersection point in 2D
				const TVector2<float> inter = vd + rayDir * (t * rayLen);

				// Height of the floor / ceiling at this intersection point
				float intFloor = floor->bottom.plane->ZatPoint(inter.X, inter.Y);
				float intCeiling = floor->top.plane->ZatPoint(inter.X, inter.Y);

				// Is the sprite higher than the ceiling?
				bool spriteAboveCeiling = thing->Pos().Z >= intCeiling;

				// Is the sprite lower than the floor?
				bool spriteBelowFloor = (thing->Pos().Z + spriteHeight) <= intFloor;

				// If neither, the sprite is trapped between the floor & ceiling
				return !(spriteAboveCeiling || spriteBelowFloor);
			};

			if (!testIntersection(tFloor) || !testIntersection(tCeil))
				return false;   // 3D floor blocks the view
		}

		return true;    // No blocking 3D floor found in this sector
	};

	// 4) Test viewer and sprite sectors
	if (!checkSectorForFloor(viewer->Sector) || !checkSectorForFloor(thing->Sector))
		return false;   // at least one sector blocks

	return true;        // No 3-D floor occlusion
}
// ******* 3DFloor-sides culling block finish *******
//         ---===      ***************************************        ===---

static int resetCounter = -1;
void ResetAnamorphCache()
{
	// Only reset 1-2 caches per frame to spread out the cost
	switch (resetCounter % 7)
	{
	case 0:
		spriteIntersectsLineCache.Clear(0);
		SpriteCrossed1sidedLineCache.Clear(0);
		break;
	case 1:
		SpriteCrossed2sidedLineCache.Clear(0);
		Visibility1sidedCache.Clear(0);
		break;
	case 2:
		Visibility2sidedObstrCache.Clear(0);
		MidTextureProximityCache.Clear(0);
		a3DFloorPlaneCache.Clear(0);
		break;
	}
	resetCounter++;
}

//==========================================================================
//
// Anamorphic "Forced-Perspective" sprite clipping occlusion culling - finish
//
//==========================================================================




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

		bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlane(r_viewpoint.Pos, thingpos, thing->Sector, thing);

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
		// Reset cache on need
		static int lastLevelMaptime = -1;
		// Level changed/reloaded
		if (level.maptime != lastLevelMaptime)
		{
			ResetAnamorphCache();
			lastLevelMaptime = level.maptime;
		}

		// Define distance constants properly
		const float FP_CLOSER_LIMIT = 384.0f;            // Where Forced-Perspective coordinates without lift-up end (close-up)
		const float SMART_START_DISTANCE = 1400.0f;      // Where Smart-clip starts coordinates start to lift-up (far-side)
		const float TRANSITION_WIDTH = SMART_START_DISTANCE - FP_CLOSER_LIMIT;    // Length of transition

		// Get the viewpoint from the current draw context that helped to reduce leaks
		const DVector3 &vp = r_viewpoint.Pos; // defining that way is closer to GZDoom v4.14.2

		// Determine sprite classification
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


		// An attempt to detect whether Y-axis sprite offset significant enough to cross the ground
		int spriteFileOffset = 0;      // Blank rows at top of texture (from file)
		int spriteRasterXdimen = 0;    // Total texture width (including blank columns)
		int spriteRasterYdimen = 0;    // Total texture height (including blank rows)
		bool hasSignificantNegativeOffset = false;
		if (gltexture && gltexture->tex)
		{
			FTexture* tex = gltexture->tex;
			spriteRasterXdimen = tex->GetWidth();
			spriteRasterYdimen = tex->GetHeight();
			spriteFileOffset = tex->TopOffset;
			// Calculate visible sprite height (actual drawn pixels)
			int visibleSpriteHeight = spriteRasterYdimen - spriteFileOffset;
			hasSignificantNegativeOffset = (visibleSpriteHeight >= 1);
			// Debug output showing all measurements
			//Printf("Sprite '%s': ""FileH=%dpx | ""TopOff=%dpx | ""VisibleH=%dpx | ""SigNegOffset=%s",
			//	tex->Name.GetChars(),spriteRasterYdimen, spriteFileOffset,visibleSpriteHeight,hasSignificantNegativeOffset ? "YES" : "NO");
		}


		float spriteSize = (thing->radius + thing->Height) * 0.5f;
		bool isMicroSprite = (spriteSize <= 8.0f);
		bool isTinySprite = (spriteSize <= 12.0f);
		bool isSmallSprite = (spriteSize <= 18.0f);
		bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 38.0f);
		bool isLargeSprite = (spriteSize > 38.0f);
		bool isactoracorpse = (thing->flags & MF_CORPSE) || (thing->flags & MF_ICECORPSE);
		bool isactorsmallbutnotcorpse = isSmallSprite && !isactoracorpse;
		bool isaregularsizedmonster = (islegacyversionmonster && (spriteSize <= 38.0f));

		// -------------
		// Some mods have big looking sprites with little radius-or-height
		// we need to adjust them to have a nice anamorphosis effect (increase it)
		if ((spriteRasterXdimen >= (thing->radius)) || (spriteRasterYdimen >= (thing->Height)))
		{
			if (isMicroSprite) { spriteSize *= 2.75f; }
			else if (isTinySprite) { spriteSize *= 2.0f; }
			else if (isSmallSprite) { spriteSize *= 1.5f; }
			else if (isMediumSprite) { spriteSize *= 1.32f; }
			else { spriteSize *= 1.2f; }
		}
		// -------------

		float vpx = vp.X; float vpy = vp.Y; float vpz = vp.Z;
		float tpx = thingpos.X; float tpy = thingpos.Y; float tpz = z; // Use 'z' from sprite setup

		//bool thingCrossed2sidedLine = SpriteCrossed2sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
		//float forceTinySpriteSize = (thingCrossed2sidedLine) ? spriteSize : (spriteSize * 0.32f);
		//float spriteSize = forceTinySpriteSize;

		bool thingCrossed1sidedLine = SpriteCrossed1sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
		bool thingCrossed2sidedLine = SpriteCrossed2sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
		bool visible1sidesInfTallObstr = IsSpriteVisibleBehind1sidedLinesCachedWrapper(thing, r_viewpoint.camera, thingpos);
		bool visible2sideTallEnoughObstr = IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(r_viewpoint.camera, thing);
		bool visible2sideMidTex = CheckFacingMidTextureProximityWrapper(thing, r_viewpoint.camera, thingpos);
		bool visible3dfloorSides = IsSpriteVisibleBehind3DFloorSides(r_viewpoint.camera, thing);
		bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlaneWrapper(r_viewpoint.Pos, thingpos, thing->Sector, thing);
		bool actorInVoid = IsActorInVoid(thing);

		DVector3 thingpos3D(thingpos.X, thingpos.Y, z);
		float distSq = (thingpos3D - r_viewpoint.Pos).LengthSquared();
		float dist = sqrt(distSq);

		// Calculate blend properly
		float blend = 0.f;

		// With additional culling mechanism coplanar leaks already reduced
		// But we can disable Forced-Perspective for floating sprites entirely
		// but only for those whose Y-axis sprite offset doesn't cross ground at all.
		// Turn off anamorphosis for things that crossed 1sided linedefs.
		if ((isfloatingsprite && !hasSignificantNegativeOffset) || thingCrossed1sidedLine || actorInVoid)
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


			//			=== Dynamic anamorphosis occlusion based amount effect adjustment - START ===
			float extremeCull1 = !visible2sideMidTex ? 0.025f : 1.0f;
			float smallsprtncrps_factor = (!visible1sidesInfTallObstr || !visible2sideTallEnoughObstr || !visible2sideMidTex || !visible3dfloorSides) ? extremeCull1 : 3.25f;
			float extremeCull2 = !visible2sideMidTex ? 2.0f : 6.0f;
			float projectiles_factor = (!visible1sidesInfTallObstr || !visible2sideTallEnoughObstr || !visible2sideMidTex || !visible3dfloorSides) ? extremeCull2 : 14.0f;
			float extremeCull3 = !visible2sideMidTex ? 0.025f : 1.0f;
			float regularsizmonster_factor1 = (!visible1sidesInfTallObstr || !visible2sideTallEnoughObstr || !visible2sideMidTex || !visible3dfloorSides) ? extremeCull3 : 3.25f;

			float regularsizmonster_factor2 = (isaregularsizedmonster) ?
				regularsizmonster_factor1 :
				regularsizmonster_factor1 * 4.0f;

			float extended_radius1 = (isactorsmallbutnotcorpse) ?
				spriteSize * smallsprtncrps_factor :     // 3.25x for small noncorpsesprites and 0.25 when occluded
				spriteSize;                              // 1x for all the rest sprites
			float extended_radius2 = (islegacyversionprojectile) ?
				extended_radius1 * projectiles_factor :  // 16x for projectiles like rockets, explosions and 6x when occluded
				extended_radius1;                        // 1x for all the rest sprites
			//			=== Dynamic anamorphosis occlusion based amount effect adjustment - FINISH ===


			//					=== Anamorphosis culling pass 1 - START ===
			// This type of culling is adequate only to cover for very obvious leaks
			// but keep some extra space for stuff that don't need even that much like big explosions

			// Only apply extended radius bias for specific actor types
			float radius_for_bias = thing->radius;
			if (isactorsmallbutnotcorpse || islegacyversionprojectile)
			{
				radius_for_bias = extended_radius2;
			}

			if (!(r_debug_nolimitanamorphoses))
			{
				// this should help prevent clipping through walls ...
				if (isaregularsizedmonster)
				{
					// this is the only place we can use to increase regular sized monster sprites anamorphic effect correctly
					// 3.25x for regular sized sprites and 0.1x when occluded
					float objradiusbias = 1.f - (radius_for_bias * regularsizmonster_factor2) / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
					minbias = MAX(minbias, objradiusbias);
				}
				else
				{
					// keep original formula for small-noncorpse sprites and projectiles
					float objradiusbias = 1.f - radius_for_bias / sqrt((vpx - tpx) * (vpx - tpx) + (vpy - tpy) * (vpy - tpy));
					minbias = MAX(minbias, objradiusbias);
				}
			}
			//					=== Anamorphosis culling pass 1 - FINISH ===


			//					=== Anamorphosis culling pass 2 - START ===
			// This one culls agressively and prevents coplanar leaks but chops too much
			// when we look at the sprite from above, in this case we don't do this cull at all
			// Detect viewer middle ground is still higher than sprite bottom

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

			// Get actual floor/ceiling heights (including 3D floors) thingpos
			float actualFloor = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, thingpos, thing);
			float actualCeiling = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, thingpos, thing);
			const float planeProximThresh = 32.0f;

			// Adjusted viewer/sprite positions
			float viewerTopAdj = viewerBottom + (EyeHeight * 0.5f);  // Mid-eye position
			float spriteTopAdj = spriteTop + (EyeHeight * 0.064f);   // Small leeway threshold

			// Proximity detection
			bool isFloorSprite = (spriteBottom - actualFloor) <= planeProximThresh;
			bool isCeilingSprite = (actualCeiling - spriteTop) <= planeProximThresh;

			// Viewer angle detection
			bool viewerLookingDown = viewerTopAdj >= spriteTopAdj;
			bool viewerLookingUp = viewerTopAdj <= spriteBottom;

			// Skip culling when:
			// - Looking down on floor sprites OR
			// - Looking up at ceiling sprites
			if (((isFloorSprite && viewerLookingDown) || (isCeilingSprite && viewerLookingUp)))
			{
				// Skip aggressive culling
			}
			else // Otherwise apply culling
			{
				if (!r_debug_nolimitanamorphoses)
				{
					float distsq = (tpx - vpx)*(tpx - vpx) + (tpy - vpy)*(tpy - vpy);
					float objradiusbias = 1.f - spriteSize / sqrt(distsq);
					minbias = MAX(minbias, objradiusbias);
				}
			}
			//		--- The pass 2 agressive culling core process - finish ---

			float epsZtolOfspriteSize = spriteSize * 0.01f; // add this to bintersect and tintersect to reduce coplanar leaks

			float bintersect, tintersect;
			if (z2 < vpz && vbtm < vpz) bintersect = MIN((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz)) + epsZtolOfspriteSize;
			else bintersect = 1.0f;

			if (z1 > vpz && vtop > vpz) tintersect = MIN((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz)) + epsZtolOfspriteSize;
			else tintersect = 1.0f;

			if (thing->waterlevel >= 1 && thing->waterlevel <= 2) bintersect = tintersect = 1.0f;

			float spbias = clamp<float>(MIN(bintersect, tintersect), minbias, 1.0f);
			float vpbias = 1.0 - spbias;
			//					=== Anamorphosis culling pass 2 - FINISH ===


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
				x1 = smart_x1;
				y1 = smart_y1;
				z1 = smart_z1;
				x2 = smart_x2;
				y2 = smart_y2;
				z2 = smart_z2;
			}
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

	// anamorphic Forced-Perspective overbright darkening start
	if (((gl_spriteclip == -1) || (gl_spriteclip == -2)) && !fullbright && (gl_lightmode == 8 || gl_lightmode == 16) )
	{
		float base_radius = thing->radius;
		float base_height = thing->Height;
		float base_dimen = (base_radius + base_height) * 0.5;
		bool islittlesprite = (base_dimen <= 18.0f);
		bool isactoracorpse = (thing->flags & MF_CORPSE) || (thing->flags & MF_ICECORPSE);

		bool isactorclose = !IsAnamorphicDistanceCulled(thing, 128.0);

		float minbias = r_spriteclipanamorphicminbias;
		minbias = clamp<float>(minbias, 0.1f, 0.5f);

		// Regular and 3D Floor-Aware Heights
		float btm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, thingpos, thing) - thing->Floorclip;
		float top = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, thingpos, thing);
		// Viewer's actual heights (from their sector and 3D floors)
		float vbtm = GetActualSpriteFloorZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);
		float vtop = GetActualSpriteCeilingZ3DfloorsAndOther(thing->Sector, r_viewpoint.Pos, thing);

		// Viewer/sprite positions
		float vpx = r_viewpoint.Pos.X;
		float vpy = r_viewpoint.Pos.Y;
		float vpz = r_viewpoint.Pos.Z;

		// Calculate intersections
		float bintersect = (z2 < vpz && vbtm < vpz) ? MIN((btm - vpz) / (z2 - vpz), (vbtm - vpz) / (z2 - vpz)) : 1.f;
		float tintersect = (z1 > vpz && vtop > vpz) ? MIN((top - vpz) / (z1 - vpz), (vtop - vpz) / (z1 - vpz)) : 1.f;

		// Blend factor
		float spbias = clamp<float>(MIN(bintersect, tintersect), minbias, 1.0f);
		float vpbias = 1.0f - spbias;

		// Compute steep factor
		bool isonsteepsurf;
		const float STEEPNESS = 1.0f;  //detect only very steep surfaces
		float steepnessfact = pow(MAX(1.f - bintersect, 1.f - tintersect), STEEPNESS);
		isonsteepsurf = steepnessfact > 0.0001f;

		float ANAMORPHIC_DARKEN;
		if (isonsteepsurf && islittlesprite && !isactoracorpse && isactorclose)
		{
			ANAMORPHIC_DARKEN = 0.989f;
		}
		else
		{
			ANAMORPHIC_DARKEN = 0.989f;
		}

		float adjusted = (lightlevel * ANAMORPHIC_DARKEN);
		lightlevel = clamp<float>(adjusted, 0, 255);
	}
	// anamorphic Forced-Perspective sprite brightness darkening finish

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
