// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2016 Christoph Oelckers
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

#include "gl/system/gl_system.h"
#include "p_local.h"
#include "p_lnspec.h"
#include "a_sharedglobal.h"
#include "g_levellocals.h"
#include "actor.h"
#include "actorinlines.h"
#include "gl/gl_functions.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/data/gl_data.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/dynlights/gl_lightbuffer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/scene/gl_scenedrawer.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_templates.h"
#include "gl/renderer/gl_quaddrawer.h"

#include "gl/compatibility/gl_20.h"

EXTERN_CVAR(Bool, gl_seamless)

//==========================================================================
//
// Collect lights for shader
//
//==========================================================================
FDynLightData lightdata;


void GLWall::SetupLights()
{
	if (RenderStyle == STYLE_Add && !glset.lightadditivesurfaces) return;	// no lights on additively blended surfaces.

	// check for wall types which cannot have dynamic lights on them (portal types never get here so they don't need to be checked.)
	switch (type)
	{
	case RENDERWALL_FOGBOUNDARY:
	case RENDERWALL_MIRRORSURFACE:
	case RENDERWALL_COLOR:
		return;
	}

	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;

	lightdata.Clear();
	p.Set(&glseg);

	/*
	if (!p.ValidNormal()) 
	{
		return;
	}
	*/
	FLightNode *node;
	if (seg->sidedef == NULL)
	{
		node = NULL;
	}
	else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
	{
		node = seg->sidedef->lighthead;
	}
	else if (sub)
	{
		// Polobject segs cannot be checked per sidedef so use the subsector instead.
		node = sub->lighthead;
	}
	else node = NULL;

	// Iterate through all dynamic lights which touch this wall and render them
	while (node)
	{
		if (node->lightsource->IsActive())
		{
			iter_dlight++;

			DVector3 posrel = node->lightsource->PosRelative(seg->frontsector->PortalGroup);
			float x = posrel.X;
			float y = posrel.Y;
			float z = posrel.Z;
			float dist = fabsf(p.DistToPoint(x, z, y));
			float radius = node->lightsource->GetRadius();
			float scale = 1.0f / ((2.f * radius) - dist);
			FVector3 fn, pos;

			if (radius > 0.f && dist < radius)
			{
				FVector3 nearPt, up, right;

				pos = { x, z, y };
				fn = p.Normal();

				fn.GetRightUp(right, up);

				FVector3 tmpVec = fn * dist;
				nearPt = pos + tmpVec;

				FVector3 t1;
				int outcnt[4]={0,0,0,0};
				texcoord tcs[4];

				// do a quick check whether the light touches this polygon
				for(int i=0;i<4;i++)
				{
					t1 = FVector3(&vtx[i*3]);
					FVector3 nearToVert = t1 - nearPt;
					tcs[i].u = ((nearToVert | right) * scale) + 0.5f;
					tcs[i].v = ((nearToVert | up) * scale) + 0.5f;

					if (tcs[i].u<0) outcnt[0]++;
					if (tcs[i].u>1) outcnt[1]++;
					if (tcs[i].v<0) outcnt[2]++;
					if (tcs[i].v>1) outcnt[3]++;

				}
				if (outcnt[0]!=4 && outcnt[1]!=4 && outcnt[2]!=4 && outcnt[3]!=4) 
				{
					gl_GetLight(seg->frontsector->PortalGroup, p, node->lightsource, true, lightdata);
				}
			}
		}
		node = node->nextLight;
	}

	dynlightindex = GLRenderer->mLights->UploadLights(lightdata);
}

//==========================================================================
//
// build the vertices for this wall
//
//==========================================================================

void GLWall::MakeVertices(bool nosplit)
{
	if (vertcount == 0)
	{
		bool split = (gl_seamless && !nosplit && seg->sidedef != NULL && !(seg->sidedef->Flags & WALLF_POLYOBJ) && !(flags & GLWF_NOSPLIT));

		FFlatVertex *ptr = GLRenderer->mVBO->GetBuffer();

		ptr->Set(glseg.x1, zbottom[0], glseg.y1, tcs[LOLFT].u, tcs[LOLFT].v);
		ptr++;
		if (split && glseg.fracleft == 0) SplitLeftEdge(ptr);
		ptr->Set(glseg.x1, ztop[0], glseg.y1, tcs[UPLFT].u, tcs[UPLFT].v);
		ptr++;
		if (split && !(flags & GLWF_NOSPLITUPPER)) SplitUpperEdge(ptr);
		ptr->Set(glseg.x2, ztop[1], glseg.y2, tcs[UPRGT].u, tcs[UPRGT].v);
		ptr++;
		if (split && glseg.fracright == 1) SplitRightEdge(ptr);
		ptr->Set(glseg.x2, zbottom[1], glseg.y2, tcs[LORGT].u, tcs[LORGT].v);
		ptr++;
		if (split && !(flags & GLWF_NOSPLITLOWER)) SplitLowerEdge(ptr);
		vertcount = GLRenderer->mVBO->GetCount(ptr, &vertindex);
	}
}


//==========================================================================
//
// General purpose wall rendering function
// everything goes through here
//
//==========================================================================

void GLWall::RenderWall(int textured)
{
	gl_RenderState.Apply();
	gl_RenderState.ApplyLightIndex(dynlightindex);
	if (gl.buffermethod != BM_DEFERRED)
	{
		MakeVertices(!!(textured&RWF_NOSPLIT));
	}
	else if (vertcount == 0)
	{
		// This should never happen but in case it actually does, use the quad drawer as fallback (without edge splitting.)
		// This way it at least gets drawn.
		FQuadDrawer qd;
		qd.Set(0, glseg.x1, zbottom[0], glseg.y1, tcs[LOLFT].u, tcs[LOLFT].v);
		qd.Set(1, glseg.x1, ztop[0], glseg.y1, tcs[UPLFT].u, tcs[UPLFT].v);
		qd.Set(2, glseg.x2, ztop[1], glseg.y2, tcs[UPRGT].u, tcs[UPRGT].v);
		qd.Set(3, glseg.x2, zbottom[1], glseg.y2, tcs[LORGT].u, tcs[LORGT].v);
		qd.Render(GL_TRIANGLE_FAN);
		vertexcount += 4;
		return;
	}
	GLRenderer->mVBO->RenderArray(GL_TRIANGLE_FAN, vertindex, vertcount);
	vertexcount += vertcount;
}

//==========================================================================
//
// 
//
//==========================================================================

void GLWall::RenderFogBoundary()
{
	if (gl_fogmode && mDrawer->FixedColormap == 0)
	{
		if (!gl.legacyMode)
		{
			int rel = rellight + getExtraLight();
			mDrawer->SetFog(lightlevel, rel, &Colormap, false);
			gl_RenderState.EnableDrawBuffers(1);
			gl_RenderState.SetEffect(EFF_FOGBOUNDARY);
			gl_RenderState.AlphaFunc(GL_GEQUAL, 0.f);
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -128.0f);
			RenderWall(RWF_BLANK);
			glPolygonOffset(0.0f, 0.0f);
			glDisable(GL_POLYGON_OFFSET_FILL);
			gl_RenderState.SetEffect(EFF_NONE);
			gl_RenderState.EnableDrawBuffers(gl_RenderState.GetPassDrawBufferCount());
		}
		else
		{
			RenderFogBoundaryCompat();
		}
	}
}


//==========================================================================
//
// 
//
//==========================================================================
void GLWall::RenderMirrorSurface()
{
	if (!GLRenderer->mirrorTexture.isValid()) return;

	// For the sphere map effect we need a normal of the mirror surface,
	FVector3 v = glseg.Normal();

	if (!gl.legacyMode)
	{
		// we use texture coordinates and texture matrix to pass the normal stuff to the shader so that the default vertex buffer format can be used as is.
		tcs[LOLFT].u = tcs[LORGT].u = tcs[UPLFT].u = tcs[UPRGT].u = v.X;
		tcs[LOLFT].v = tcs[LORGT].v = tcs[UPLFT].v = tcs[UPRGT].v = v.Z;

		gl_RenderState.EnableTextureMatrix(true);
		gl_RenderState.mTextureMatrix.computeNormalMatrix(gl_RenderState.mViewMatrix);
	}
	else
	{
		glNormal3fv(&v[0]);
	}

	// Use sphere mapping for this
	gl_RenderState.SetEffect(EFF_SPHEREMAP);

	mDrawer->SetColor(lightlevel, 0, Colormap ,0.1f);
	mDrawer->SetFog(lightlevel, 0, &Colormap, true);
	gl_RenderState.BlendFunc(GL_SRC_ALPHA,GL_ONE);
	gl_RenderState.AlphaFunc(GL_GREATER,0);
	glDepthFunc(GL_LEQUAL);

	FMaterial * pat=FMaterial::ValidateTexture(GLRenderer->mirrorTexture, false, false);
	gl_RenderState.SetMaterial(pat, CLAMP_NONE, 0, -1, false);

	flags &= ~GLWF_GLOW;
	RenderWall(RWF_BLANK);

	gl_RenderState.EnableTextureMatrix(false);
	gl_RenderState.SetEffect(EFF_NONE);

	// Restore the defaults for the translucent pass
	gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl_RenderState.AlphaFunc(GL_GEQUAL, gl_mask_sprite_threshold);
	glDepthFunc(GL_LESS);

	// This is drawn in the translucent pass which is done after the decal pass
	// As a result the decals have to be drawn here.
	if (seg->sidedef->AttachedDecals)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -128.0f);
		glDepthMask(false);
		DoDrawDecals();
		glDepthMask(true);
		glPolygonOffset(0.0f, 0.0f);
		glDisable(GL_POLYGON_OFFSET_FILL);
		gl_RenderState.SetTextureMode(TM_MODULATE);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
}

//==========================================================================
//
// 
//
//==========================================================================

static const uint8_t renderwalltotier[] =
{
	side_t::none,
	side_t::top,
	side_t::mid,
	side_t::mid,
	side_t::bottom,
	side_t::none,
	side_t::none,
	side_t::mid,
	side_t::none,
	side_t::mid,
};

void GLWall::RenderTextured(int rflags)
{
	int tmode = gl_RenderState.GetTextureMode();
	int rel = rellight + getExtraLight();

	if (flags & GLWF_GLOW)
	{
		gl_RenderState.EnableGlow(true);
		gl_RenderState.SetGlowParams(topglowcolor, bottomglowcolor);
		gl_RenderState.SetGlowPlanes(frontsector->ceilingplane, frontsector->floorplane);
	}
	gl_RenderState.SetMaterial(gltexture, flags & 3, 0, -1, false);

	if (flags & GLT_CLAMPY && (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF))
	{
		if (tmode == TM_MODULATE) gl_RenderState.SetTextureMode(TM_CLAMPY);
	}

	if (type == RENDERWALL_M2SNF)
	{
		mDrawer->SetFog(255, 0, NULL, false);
	}
	if (type != RENDERWALL_COLOR && seg->sidedef != nullptr)
	{
		auto side = seg->sidedef;
		auto tierndx = renderwalltotier[type];
		auto &tier = side->textures[tierndx];
		PalEntry color1 = side->GetSpecialColor(tierndx, side_t::walltop, frontsector);
		PalEntry color2 = side->GetSpecialColor(tierndx, side_t::wallbottom, frontsector);
		gl_RenderState.SetObjectColor(color1);
		gl_RenderState.SetObjectColor2(color2);
		gl_RenderState.SetAddColor(side->GetAdditiveColor(tierndx, frontsector));
		if (color1 != color2)
		{
			// Do gradient setup only if there actually is a gradient.

			gl_RenderState.EnableGradient(true);
			if ((tier.flags & side_t::part::ClampGradient) && backsector)
			{
				if (tierndx == side_t::top)
				{
					gl_RenderState.SetGradientPlanes(frontsector->ceilingplane, backsector->ceilingplane);
				}
				else if (tierndx == side_t::mid)
				{
					gl_RenderState.SetGradientPlanes(backsector->ceilingplane, backsector->floorplane);
				}
				else // side_t::bottom:
				{
					gl_RenderState.SetGradientPlanes(backsector->floorplane, frontsector->floorplane);
				}
			}
			else
			{
				gl_RenderState.SetGradientPlanes(frontsector->ceilingplane, frontsector->floorplane);
			}
		}
	}

	float absalpha = fabsf(alpha);
	if (lightlist == nullptr)
	{
		if (type != RENDERWALL_M2SNF) mDrawer->SetFog(lightlevel, rel, &Colormap, RenderStyle == STYLE_Add);
		mDrawer->SetColor(lightlevel, rel, Colormap, absalpha);
		RenderWall(rflags);
	}
	else
	{
		gl_RenderState.EnableSplit(true);

		for (unsigned i = 0; i < lightlist->Size(); i++)
		{
			secplane_t &lowplane = i == (*lightlist).Size() - 1 ? frontsector->floorplane : (*lightlist)[i + 1].plane;
			// this must use the exact same calculation method as GLWall::Process etc.
			float low1 = lowplane.ZatPoint(vertexes[0]);
			float low2 = lowplane.ZatPoint(vertexes[1]);

			if (low1 < ztop[0] || low2 < ztop[1])
			{
				int thisll = (*lightlist)[i].caster != nullptr ? gl_ClampLight(*(*lightlist)[i].p_lightlevel) : lightlevel;
				FColormap thiscm;
				thiscm.FadeColor = Colormap.FadeColor;
				thiscm.FogDensity = Colormap.FogDensity;
				thiscm.CopyFrom3DLight(&(*lightlist)[i]);
				mDrawer->SetColor(thisll, rel, thiscm, absalpha);
				if (type != RENDERWALL_M2SNF) mDrawer->SetFog(thisll, rel, &thiscm, RenderStyle == STYLE_Add);
				gl_RenderState.SetSplitPlanes((*lightlist)[i].plane, lowplane);
				RenderWall(rflags);
			}
			if (low1 <= zbottom[0] && low2 <= zbottom[1]) break;
		}

		gl_RenderState.EnableSplit(false);
	}
	gl_RenderState.SetObjectColor(0xffffffff);
	gl_RenderState.SetObjectColor2(0);
	gl_RenderState.SetAddColor(0);
	gl_RenderState.SetTextureMode(tmode);
	gl_RenderState.EnableGlow(false);
	gl_RenderState.EnableGradient(false);
}

//==========================================================================
//
// 
//
//==========================================================================

void GLWall::RenderTranslucentWall()
{
	if (gltexture)
	{
		if (mDrawer->FixedColormap == CM_DEFAULT && gl_lights && gl.lightmethod == LM_DIRECT)
		{
			SetupLights();
		}
		if (!gltexture->GetTransparent()) gl_RenderState.AlphaFunc(GL_GEQUAL, gl_mask_threshold);
		else gl_RenderState.AlphaFunc(GL_GEQUAL, 0.f);
		if (RenderStyle == STYLE_Add) gl_RenderState.BlendFunc(GL_SRC_ALPHA,GL_ONE);
		RenderTextured(RWF_TEXTURED | RWF_NOSPLIT);
		if (RenderStyle == STYLE_Add) gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
	{
		gl_RenderState.AlphaFunc(GL_GEQUAL, 0.f);
		mDrawer->SetColor(lightlevel, 0, Colormap, fabsf(alpha));
		mDrawer->SetFog(lightlevel, 0, &Colormap, RenderStyle == STYLE_Add);
		gl_RenderState.EnableTexture(false);
		RenderWall(RWF_NOSPLIT);
		gl_RenderState.EnableTexture(true);
	}
}

//==========================================================================
//
// 
//
//==========================================================================
void GLWall::Draw(int pass)
{
	gl_RenderState.SetNormal(glseg.Normal());
	switch (pass)
	{
	case GLPASS_LIGHTSONLY:
		SetupLights();
		break;

	case GLPASS_ALL:
		SetupLights();
		// fall through
	case GLPASS_PLAIN:
		RenderTextured(RWF_TEXTURED);
		break;

	case GLPASS_TRANSLUCENT:

		switch (type)
		{
		case RENDERWALL_MIRRORSURFACE:
			RenderMirrorSurface();
			break;

		case RENDERWALL_FOGBOUNDARY:
			RenderFogBoundary();
			break;

		default:
			RenderTranslucentWall();
			break;
		}
		break;

	case GLPASS_LIGHTTEX:
	case GLPASS_LIGHTTEX_ADDITIVE:
	case GLPASS_LIGHTTEX_FOGGY:
		RenderLightsCompat(pass);
		break;

	case GLPASS_TEXONLY:
		gl_RenderState.SetMaterial(gltexture, flags & 3, 0, -1, false);
		RenderWall(RWF_TEXTURED);
		break;

	case GLPASS_BRIGHTEN_LEGACY_LIGHTTEX:
		if (seg->sidedef != nullptr && seg->v1 != nullptr && seg->v2 != nullptr)
		{
			// Define local viewport culling radius bubble for open-space landscape sectors
			float cullDist = 16000.0f;
			if (cullDist > 0.0f)
			{
				float maxAllowedDistSq = cullDist * cullDist;

				// Calculate squared distance from camera viewpoint to both vertices of the current wall segment
				float dist1 = (float)(seg->v1->fPos() - r_viewpoint.Pos).LengthSquared();
				float dist2 = (float)(seg->v2->fPos() - r_viewpoint.Pos).LengthSquared();

				// If both vertices escape the culling bubble, skip the entire heavy overbright pass instantly!
				if (dist1 > maxAllowedDistSq && dist2 > maxAllowedDistSq)
				{
					break;
				}
			}
			gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ONE);
			glDepthFunc(GL_EQUAL);
			glDepthMask(false);
			gl_RenderState.SetMaterial(gltexture, CLAMP_NONE, 0, -1, false);

			FLightNode *node = (!(seg->sidedef->Flags & WALLF_POLYOBJ)) ?
				seg->sidedef->lighthead : (sub ? sub->lighthead : nullptr);

			if (node == nullptr)
			{
				break;
			}

			texcoord save[4];
			memcpy(save, tcs, sizeof(tcs)); // Save original wall coordinates

			float baseFactor = clamp((float)gl_legacy_dynlight_overbright_walls, 0.0f, 0.2f);

			float passIntensityMultiplier = 1.5f;
			if (gl_drawinfo && gl_drawinfo->gl_IsFoggyPass)
			{
				passIntensityMultiplier = 1.5f * 3.0f; // Tripple the punch for foggy walls!
			}

			float x1 = (float)glseg.x1; float y1 = (float)glseg.y1;
			float x2 = (float)glseg.x2; float y2 = (float)glseg.y2;

			float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
			float zt1 = (float)ztop[1]; float zb1 = (float)zbottom[1];

			while (node)
			{
				FDynamicLight *light = node->lightsource;
				if (light && light->IsActive() && light->GetRadius() > 0.0f)
				{
					gl_RenderState.EnableFog(false);
					gl_RenderState.BlendEquation(GL_FUNC_ADD);

					float radius = light->GetRadius();
					float radiusSq = radius * radius;
					float lx = (float)light->X();
					float ly = (float)light->Y();
					float lz = (float)light->Z();

					float vtx[] = { x1, zb0, y1, x1, zt0, y1, x2, zt1, y2, x2, zb1, y2 };

					gl_RenderState.Apply();
					glEnable(GL_TEXTURE_2D);
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

					// --- SINGLE FAST PASS WITH DOUBLE INTENSITY ---
					// We completely removed the second glBegin/glEnd pass loop!
					// By multiplying intensity by 2.0f inside the first pass, we get the EXACT same
					glBegin(GL_QUADS);
					for (int i = 0; i < 4; i++)
					{
						float vx = vtx[i * 3]; float vz = vtx[i * 3 + 1]; float vy = vtx[i * 3 + 2];
						float dx = vx - lx; float dy = vy - ly; float dz = vz - lz;
						float dist2 = (dx * dx) + (dy * dy) + (dz * dz);

						float intensity = 0.0f;
						if (dist2 < radiusSq)
						{
							float dist = sqrtf(dist2);
							// Doubled intensity multiplier gives the same power boost instantly!
							intensity = (1.0f - (dist / radius)) * baseFactor * passIntensityMultiplier * 2.0f;
						}

						if (intensity > 1.0f) intensity = 1.0f;
						if (intensity < 0.0f) intensity = 0.0f;

						glTexCoord2f(tcs[i].u, tcs[i].v);
						glColor4f(intensity, intensity, intensity, 1.0f);
						glVertex3f(vx, vz, vy);
					}
					glEnd();
				}
				node = node->nextLight;
			}

			memcpy(tcs, save, sizeof(tcs)); // Restore layout
			vertcount = 0;

			// CLEAN ARCHITECTURAL FIX: SYNC STATE MACHINE AND PREVENT BLEND LEAKS INTO SKY
			// Force-update everything through Graf Zahl's cache layer to prevent data corruption
			gl_RenderState.EnableFog(true);
			gl_RenderState.BlendFunc(GL_ONE, GL_ZERO);
			gl_RenderState.SetTextureMode(TM_MODULATE);

			// If LZDoom's FRenderState has a depth function cacher, use it here, 
			// otherwise we force it natively right before Apply()
			glDepthFunc(GL_LESS);
			glDepthMask(true);

			// Force the state machine to flush all changes directly into the GPU hardware registers.
			// This completely clears the left-over GL_DST_COLOR, GL_ONE blending state!
			gl_RenderState.Apply();

			// Secondary fallback clean for raw OpenGL 1.1 hardware registers
			glEnable(GL_FOG);
			glBlendFunc(GL_ONE, GL_ZERO);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		}
		break;

	case GLPASS_BRIGHTMAP_LEGACY:
	{
		// Use brightmap texture mode for glowing flats too to save CPU on sector iterations cycle
		const float overallGlowIntensity = 0.9f;

		// 1. Compute the native base brightmap asset light attenuation factor.
		int wallLight = this->lightlevel;
		float intensityFactor;
		if (wallLight < 96)
		{
			intensityFactor = 1.0f; // Maintain maximum power in dark ambient configurations.
		}
		else
		{
			const float rangeFactorInv = 1.0f / (255.0f - 96.0f);
			float factor = 1.0f - ((float)(wallLight - 96) * rangeFactorInv);
			factor = factor + 0.1f * factor * (1.0f - factor); // Apply subtle mid-range parabola boost.
			intensityFactor = (factor * 0.99f) + 0.01f;
		}
		if (intensityFactor > 1.0f) intensityFactor = 1.0f;
		if (intensityFactor < 0.0f) intensityFactor = 0.0f;

		// Initialize local vector blocks to safely extract sector floor/ceiling parameters.
		float topglow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		float bottomglow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		// Extract hardware color variables and specific Glow Heights from the source sector.
		bool hasWallGlow = (frontsector != nullptr) && gl_GetWallGlow(frontsector, topglow, bottomglow);
		FMaterial *bm = gltexture ? gltexture->GetBrightmapLegacy() : nullptr;

		// Early culling fallback: If the sector has no brightmap and no active glow emitters, skip rendering.
		if (!bm && !hasWallGlow) break;

		// Extract local wall segment horizontal vectors and standard texture heights.
		float x1 = (float)glseg.x1; float y1 = (float)glseg.y1;
		float x2 = (float)glseg.x2; float y2 = (float)glseg.y2;
		float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
		float zt1 = (float)ztop[1]; float zb1 = (float)zbottom[1];

		// Synchronize texture state pipeline via Graf Zahl's cache layer.
		gl_RenderState.SetTextureMode(TM_BRIGHTMAP_LEGACY);
		glEnable(GL_TEXTURE_2D);

		if (bm) // PASS A: RENDER NATIVE BRIGHTMAP TEXTURE MASK ASSET
		{
			// Bind the custom black-masked fullbright texture asset.
			gl_RenderState.SetMaterial(bm, flags & 3, 0, -1, false);
			gl_RenderState.Apply();

			glBegin(GL_QUADS);
			for (int i = 0; i < 4; i++)
			{
				// Standard non-clipped vertex projection pass for the texture emitters.
				glTexCoord2f(tcs[i].u, tcs[i].v);
				glColor4f(intensityFactor, intensityFactor, intensityFactor, 1.0f);

				float vx = (i == 0 || i == 1) ? x1 : x2;
				float vy = (i == 0 || i == 1) ? y1 : y2;
				float vz = (i == 0) ? zb0 : (i == 1) ? zt0 : (i == 2) ? zt1 : zb1;
				glVertex3f(vx, vz, vy);
			}
			glEnd();
		}

		if (hasWallGlow) // PASS B: RENDER PROCEDURAL SECTOR GLOW GRADIENTS (FIXED-FUNCTION EXTRACTION)
		{
			// Re-bind the diffuse wall texture so that our glow multiplies correctly over the wall patterns.
			gl_RenderState.SetMaterial(gltexture, flags & 3, 0, -1, false);
			gl_RenderState.Apply();

			// HARDWARE ANTI-Z-FIGHTING PIPELINE SECURE:
			// 1. Force LEQUAL testing to allow the sub-pass geometry to overlap pixel-perfect layouts.
			glDepthFunc(GL_LEQUAL);
			// 2. Clear depth mask to lock writing operations and isolate alpha blending overhead.
			glDepthMask(false);
			// 3. Offset projection values slightly closer to camera viewpoint to eliminate shimmering.
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-0.5f, -0.5f);

			// Extract designated heights limits from index [3] of the verified float arrays.
			float floorGlowHeight = bottomglow[3];
			float ceilGlowHeight = topglow[3];

			// --- SUB-PASS B1: REPLICATE FLOOR GLOW GRADIENT ---
			if (floorGlowHeight > 0.0f)
			{
				// SECURE PORTAL HEIGHT FIXED BINDING:
				// Calculate absolute physics level of fluid by querying sector planes directly.
				// This completely stops the gradient from jumping up to upper portal/door arch archways.
				float trueFloorZ0 = (float)frontsector->floorplane.ZatPoint(seg->v1->fPos());
				float trueFloorZ1 = (float)frontsector->floorplane.ZatPoint(seg->v2->fPos());
				// Compute target ceiling boundaries where procedural light falloff drops to zero.
				float splitZ0 = trueFloorZ0 + floorGlowHeight;
				float splitZ1 = trueFloorZ1 + floorGlowHeight;
				// Clamp final execution spaces strictly inside the physical rendering coordinates of the current wall.
				float rBottomZ0 = trueFloorZ0; if (rBottomZ0 < zb0) rBottomZ0 = zb0;
				float rBottomZ1 = trueFloorZ1; if (rBottomZ1 < zb1) rBottomZ1 = zb1;
				float rTopZ0 = splitZ0; if (rTopZ0 > zt0) rTopZ0 = zt0;
				float rTopZ1 = splitZ1; if (rTopZ1 > zt1) rTopZ1 = zt1;

				// Only execute vertex pipe if the calculated glow slice intersects visible wall space.
				if (rTopZ0 > rBottomZ0 && rTopZ1 > rBottomZ1)
				{
					// HARDWARE STABILITY / TEXTURE SQUASHING FIX:
					// Query full raw dimensions of texture mappings (Top minus Bottom) across both sides.
					float v_range0 = tcs[1].v - tcs[0].v; float v_range1 = tcs[2].v - tcs[3].v;
					float h_total0 = zt0 - zb0; float h_total1 = zt1 - zb1;

					// Interpolate native V coordinate points proportionally to secure precise 1:1 pixel alignment.
					// This completely stops texture crunching when drawing clipped sub-geometry on high walls.
					float sliceBottomV0 = tcs[0].v + (h_total0 > 0.0f ? ((rBottomZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceBottomV1 = tcs[3].v + (h_total1 > 0.0f ? ((rBottomZ1 - zb1) / h_total1) * v_range1 : 0.0f);
					float sliceTopV0 = tcs[0].v + (h_total0 > 0.0f ? ((rTopZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceTopV1 = tcs[3].v + (h_total1 > 0.0f ? ((rTopZ1 - zb1) / h_total1) * v_range1 : 0.0f);

					// Determine individual vertex lighting factors depending on true distance from liquid floor.
					float intensityBottom0 = 1.0f - ((rBottomZ0 - trueFloorZ0) / floorGlowHeight);
					float intensityBottom1 = 1.0f - ((rBottomZ1 - trueFloorZ1) / floorGlowHeight);
					float intensityTop0 = 1.0f - ((rTopZ0 - trueFloorZ0) / floorGlowHeight);
					float intensityTop1 = 1.0f - ((rTopZ1 - trueFloorZ1) / floorGlowHeight);

					glBegin(GL_QUADS);
					// Vertex 0: Bottom-Left Boundary
					glTexCoord2f(tcs[0].u, sliceBottomV0);
					glColor4f(bottomglow[0] * intensityBottom0 * overallGlowIntensity, bottomglow[1] * intensityBottom0 *
						overallGlowIntensity, bottomglow[2] * intensityBottom0 * overallGlowIntensity, 1.0f);
					glVertex3f(x1, rBottomZ0, y1);
					// Vertex 1: Top-Left Upper Slice Edge (Approaching 0% lighting brightness)
					glTexCoord2f(tcs[1].u, sliceTopV0);
					glColor4f(bottomglow[0] * intensityTop0 * overallGlowIntensity, bottomglow[1] * intensityTop0 *
						overallGlowIntensity, bottomglow[2] * intensityTop0 * overallGlowIntensity, 1.0f);
					glVertex3f(x1, rTopZ0, y1);
					// Vertex 2: Top-Right Upper Slice Edge (Approaching 0% lighting brightness)
					glTexCoord2f(tcs[2].u, sliceTopV1);
					glColor4f(bottomglow[0] * intensityTop1 * overallGlowIntensity, bottomglow[1] * intensityTop1 *
						overallGlowIntensity, bottomglow[2] * intensityTop1 * overallGlowIntensity, 1.0f);
					glVertex3f(x2, rTopZ1, y2);
					// Vertex 3: Bottom-Right Boundary
					glTexCoord2f(tcs[3].u, sliceBottomV1);
					glColor4f(bottomglow[0] * intensityBottom1 * overallGlowIntensity, bottomglow[1] * intensityBottom1 *
						overallGlowIntensity, bottomglow[2] * intensityBottom1 * overallGlowIntensity, 1.0f);
					glVertex3f(x2, rBottomZ1, y2);
					glEnd();
				}
			}

			// --- SUB-PASS B2: REPLICATE CEILING GLOW GRADIENT ---
			if (ceilGlowHeight > 0.0f)
			{
				// Extract absolute ceiling heights by processing sector planes directly.
				float trueCeilZ0 = (float)frontsector->ceilingplane.ZatPoint(seg->v1->fPos());
				float trueCeilZ1 = (float)frontsector->ceilingplane.ZatPoint(seg->v2->fPos());
				float splitZ0 = trueCeilZ0 - ceilGlowHeight;
				float splitZ1 = trueCeilZ1 - ceilGlowHeight;
				float rTopZ0 = trueCeilZ0; if (rTopZ0 > zt0) rTopZ0 = zt0;
				float rTopZ1 = trueCeilZ1; if (rTopZ1 > zt1) rTopZ1 = zt1;
				float rBottomZ0 = splitZ0; if (rBottomZ0 < zb0) rBottomZ0 = zb0;
				float rBottomZ1 = splitZ1; if (rBottomZ1 < zb1) rBottomZ1 = zb1;

				if (rTopZ0 > rBottomZ0 && rTopZ1 > rBottomZ1)
				{
					float v_range0 = tcs[1].v - tcs[0].v; float v_range1 = tcs[2].v - tcs[3].v;
					float h_total0 = zt0 - zb0; float h_total1 = zt1 - zb1;
					float sliceBottomV0 = tcs[0].v + (h_total0 > 0.0f ? ((rBottomZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceBottomV1 = tcs[3].v + (h_total1 > 0.0f ? ((rBottomZ1 - zb1) / h_total1) * v_range1 : 0.0f);
					float sliceTopV0 = tcs[0].v + (h_total0 > 0.0f ? ((rTopZ0 - zb0) / h_total0) * v_range0 : 0.0f);
					float sliceTopV1 = tcs[3].v + (h_total1 > 0.0f ? ((rTopZ1 - zb1) / h_total1) * v_range1 : 0.0f);
					float intensityBottom0 = 1.0f - ((trueCeilZ0 - rBottomZ0) / ceilGlowHeight);
					float intensityBottom1 = 1.0f - ((trueCeilZ1 - rBottomZ1) / ceilGlowHeight);
					float intensityTop0 = 1.0f - ((trueCeilZ0 - rTopZ0) / ceilGlowHeight);
					float intensityTop1 = 1.0f - ((trueCeilZ1 - rTopZ1) / ceilGlowHeight);

					glBegin(GL_QUADS);

					// Vertex 0: Lower Split Cut-off Edge
					glTexCoord2f(tcs[0].u, sliceBottomV0);
					glColor4f(topglow[0] * intensityBottom0 * overallGlowIntensity, topglow[1] * intensityBottom0 *
						overallGlowIntensity, topglow[2] * intensityBottom0 * overallGlowIntensity, 1.0f);
					glVertex3f(x1, rBottomZ0, y1);
					// Vertex 1: True Top-Left Boundary
					glTexCoord2f(tcs[1].u, sliceTopV0);
					glColor4f(topglow[0] * intensityTop0 * overallGlowIntensity, topglow[1] * intensityTop0 *
						overallGlowIntensity, topglow[2] * intensityTop0 * overallGlowIntensity, 1.0f);
					glVertex3f(x1, rTopZ0, y1);
					// Vertex 2: True Top-Right Boundary
					glTexCoord2f(tcs[2].u, sliceTopV1);
					glColor4f(topglow[0] * intensityTop1 * overallGlowIntensity, topglow[1] * intensityTop1 *
						overallGlowIntensity, topglow[2] * intensityTop1 * overallGlowIntensity, 1.0f);
					glVertex3f(x2, rTopZ1, y2);
					// Vertex 3: Lower Split Cut-off Edge
					glTexCoord2f(tcs[3].u, sliceBottomV1);
					glColor4f(topglow[0] * intensityBottom1 * overallGlowIntensity, topglow[1] * intensityBottom1 *
						overallGlowIntensity, topglow[2] * intensityBottom1 * overallGlowIntensity, 1.0f);
					glVertex3f(x2, rBottomZ1, y2);
					glEnd();
				}
			}
			// Comprehensive symmetric hardware rollback execution.
			glDisable(GL_POLYGON_OFFSET_FILL);
			glDepthMask(true);
			glDepthFunc(GL_EQUAL);
		}
		// Revert the texture environment state to safe modulation parameters.
		gl_RenderState.SetTextureMode(TM_MODULATE);
	}
	break;

	}
}





	//case GLPASS_BRIGHTMAP_LEGACY:
	//	FMaterial *bm = gltexture->GetBrightmapLegacy();
	//	if (bm)
	//	{
	//		// Get the current wall lighting level
	//		int wallLight = this->lightlevel;

	//		float intensityFactor;
	//		if (wallLight < 96)
	//		{
	//			// Full effect below 96 light level to prevent early dimming
	//			intensityFactor = 1.0f;
	//		}
	//		else
	//		{
	//			// Optimized inverse light factor for the 96-255 range using multiplication
	//			const float rangeFactorInv = 1.0f / (255.0f - 96.0f);
	//			float factor = 1.0f - ((float)(wallLight - 96) * rangeFactorInv);

	//			// Add a subtle ~2.5% parabola bump to mid-range without overbrightening high values
	//			factor = factor + 0.1f * factor * (1.0f - factor);

	//			// Scale intensity to smoothly drop from 1.0 (at 96) down to 0.01 (at 255)
	//			intensityFactor = (factor * 0.99f) + 0.01f;
	//		}

	//		if (intensityFactor > 1.0f) intensityFactor = 1.0f;
	//		if (intensityFactor < 0.0f) intensityFactor = 0.0f;

	//		// Apply calculated intensity as modulation color
	//		gl_RenderState.SetColor(intensityFactor, intensityFactor, intensityFactor, 1.0f);

	//		gl_RenderState.SetMaterial(bm, flags & 3, 0, -1, false);
	//		RenderWall(RWF_TEXTURED);
	//	}
	//	break;



	//case GLPASS_BRIGHTEN_LEGACY_LIGHTTEX_SLOW:
	//	if (seg->sidedef != nullptr && seg->v1 != nullptr && seg->v2 != nullptr)
	//	{
	//		float cullDist = 2048.0f;
	//		if (cullDist > 0.0f)
	//		{
	//			float maxAllowedDistSq = cullDist * cullDist;
	//			// Calculate squared distance from camera viewpoint to both vertices of the current wall segment
	//			float dist1 = (float)(seg->v1->fPos() - r_viewpoint.Pos).LengthSquared();
	//			float dist2 = (float)(seg->v2->fPos() - r_viewpoint.Pos).LengthSquared();
	//			// If both vertices escape the culling bubble, skip the entire heavy overbright pass instantly!
	//			if (dist1 > maxAllowedDistSq && dist2 > maxAllowedDistSq)
	//			{
	//				break;
	//			}
	//		}

	//		// Proceed with the lightning-fast textureless overbright pipeline for nearby geometry
	//		glDisable(GL_TEXTURE_2D);

	//		// Synchronize hardware registers into the multiplicative overbright window
	//		gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ONE);
	//		glDepthFunc(GL_EQUAL);
	//		glDepthMask(false);

	//		FLightNode *node = (!(seg->sidedef->Flags & WALLF_POLYOBJ)) ?
	//			seg->sidedef->lighthead : (sub ? sub->lighthead : nullptr);

	//		if (node == nullptr)
	//		{
	//			glEnable(GL_TEXTURE_2D);
	//			break;
	//		}

	//		texcoord save[4];
	//		memcpy(save, tcs, sizeof(tcs)); // Restore missing index array tracking boundaries

	//		float baseFactor = clamp((float)gl_legacy_dynlight_overbright_walls, 0.0f, 0.2f);
	//		float passIntensityMultiplier = (gl_drawinfo && gl_drawinfo->gl_IsFoggyPass) ? 4.5f : 1.5f;
	//		float totalMultiplier = baseFactor * passIntensityMultiplier * 2.0f;

	//		float x1 = (float)glseg.x1; float y1 = (float)glseg.y1;
	//		float x2 = (float)glseg.x2; float y2 = (float)glseg.y2;

	//		float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
	//		float zt1 = (float)ztop[1];    float zb1 = (float)zbottom[1]; // Restored safe index array tracking bounds

	//		while (node)
	//		{
	//			FDynamicLight *light = node->lightsource;
	//			if (light && light->IsActive() && light->GetRadius() > 0.0f)
	//			{
	//				gl_RenderState.EnableFog(false);
	//				gl_RenderState.BlendEquation(GL_FUNC_ADD);

	//				float radius = light->GetRadius();
	//				float radiusSq = radius * radius;
	//				float lx = (float)light->X(); float ly = (float)light->Y(); float lz = (float)light->Z();

	//				float vtx[] = {
	//					x1, zb0, y1,
	//					x1, zt0, y1,
	//					x2, zt1, y2,
	//					x2, zb1, y2
	//				};

	//				gl_RenderState.Apply();

	//				// --- SINGLE FAST PASS WITH DOUBLE INTENSITY ---
	//				glBegin(GL_QUADS);
	//				for (int i = 0; i < 4; i++)
	//				{
	//					float vx = vtx[i * 3]; float vz = vtx[i * 3 + 1]; float vy = vtx[i * 3 + 2];
	//					float dx = vx - lx; float dy = vy - ly; float dz = vz - lz;
	//					float dist2 = (dx * dx) + (dy * dy) + (dz * dz);

	//					float intensity = 0.0f;
	//					if (dist2 < radiusSq)
	//					{
	//						intensity = (1.0f - (sqrtf(dist2) / radius)) * totalMultiplier;
	//					}

	//					if (intensity > 1.0f) intensity = 1.0f;
	//					if (intensity < 0.0f) intensity = 0.0f;

	//					float r = (light->GetRed() / 255.0f) * intensity;
	//					float g = (light->GetGreen() / 255.0f) * intensity;
	//					float b = (light->GetBlue() / 255.0f) * intensity;

	//					glColor4f(r, g, b, 1.0f);
	//					glVertex3f(vx, vz, vy);
	//				}
	//				glEnd();
	//			}
	//			node = node->nextLight;
	//		}

	//		memcpy(tcs, save, sizeof(tcs));
	//		vertcount = 0;

	//		// ==============================================================================
	//		// TWIN-PASS CONTEXT PURGE: RESET PIPELINE SPECIFICALLY FOR THE DRAWWALLS2X LOOP
	//		// ==============================================================================
	//		glEnable(GL_TEXTURE_2D);

	//		gl_RenderState.EnableFog(true);
	//		gl_RenderState.BlendFunc(GL_ONE, GL_ONE);
	//		gl_RenderState.SetTextureMode(TM_MODULATE);

	//		glDepthFunc(GL_EQUAL);
	//		glDepthMask(false);

	//		gl_RenderState.Apply();

	//		glEnable(GL_FOG);
	//		glBlendFunc(GL_ONE, GL_ONE);
	//		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	//	}
	//	break;

	//case GLPASS_BRIGHTEN_LEGACY_LIGHTTEX_PROJECT_LIGHT_LIKE_A_TEXTURE:
	//	if (seg->sidedef != nullptr)
	//	{
	//		// Force-bind the correct dynamic light attenuation filter mask (glLight) via Graf Zahl's system 
	//		if (!gl_SetupLightTexture())
	//		{
	//			break;
	//		}

	//		// Synchronize hardware registers into the multiplicative overbright window over the light mask
	//		gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ONE);
	//		glDepthFunc(GL_EQUAL);
	//		glDepthMask(false);

	//		FLightNode *node = (!(seg->sidedef->Flags & WALLF_POLYOBJ)) ?
	//			seg->sidedef->lighthead : (sub ? sub->lighthead : nullptr);

	//		if (node == nullptr)
	//		{
	//			break;
	//		}

	//		// PERFORMANCE MAGIC: We COMPLETELY removed the duplicate "PrepareLight" function call!
	//		// Since wall->Draw(GLPASS_LIGHTTEX) has ALREADY executed right before this step inside DrawWalls2x,
	//		// the master 'tcs[i]' array already contains the mathematically perfect spotlight projection UVs.
	//		// Re-calling PrepareLight was corrupting internal matrix transforms, throwing the mask into random coordinates!

	//		float baseFactor = clamp((float)gl_legacy_dynlight_overbright_walls, 0.0f, 0.2f);
	//		float passIntensityMultiplier = (gl_drawinfo && gl_drawinfo->gl_IsFoggyPass) ? 4.5f : 1.5f;
	//		float totalMultiplier = baseFactor * passIntensityMultiplier * 2.0f;

	//		float x1 = (float)glseg.x1; float y1 = (float)glseg.y1;
	//		float x2 = (float)glseg.x2; float y2 = (float)glseg.y2;

	//		float zb0 = (float)zbottom[0]; float zt0 = (float)ztop[0];
	//		float zt1 = (float)ztop[1];    float zb1 = (float)zbottom[1];

	//		while (node)
	//		{
	//			FDynamicLight *light = node->lightsource;
	//			if (light && light->IsActive() && light->GetRadius() > 0.0f)
	//			{
	//				gl_RenderState.EnableFog(false);
	//				gl_RenderState.BlendEquation(GL_FUNC_ADD);

	//				float radius = light->GetRadius();
	//				float radiusSq = radius * radius;
	//				float lx = (float)light->X(); float ly = (float)light->Y(); float lz = (float)light->Z();

	//				float vtx[] = {
	//					x1, zb0, y1,
	//					x1, zt0, y1,
	//					x2, zt1, y2,
	//					x2, zb1, y2
	//				};

	//				gl_RenderState.Apply();
	//				glEnable(GL_TEXTURE_2D);
	//				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	//				// --- SINGLE FAST PASS WITH DOUBLE INTENSITY ---
	//				glBegin(GL_QUADS);
	//				for (int i = 0; i < 4; i++)
	//				{
	//					float vx = vtx[i * 3]; float vz = vtx[i * 3 + 1]; float vy = vtx[i * 3 + 2];
	//					float dx = vx - lx; float dy = vy - ly; float dz = vz - lz;
	//					float dist2 = (dx * dx) + (dy * dy) + (dz * dz);

	//					float intensity = 0.0f;
	//					if (dist2 < radiusSq)
	//					{
	//						intensity = (1.0f - (sqrtf(dist2) / radius)) * totalMultiplier;
	//					}

	//					if (intensity > 1.0f) intensity = 1.0f;
	//					if (intensity < 0.0f) intensity = 0.0f;

	//					// Safely feed the pre-calculated texture projection coordinates straight into OpenGL
	//					glTexCoord2f(tcs[i].u, tcs[i].v);
	//					glColor4f(intensity, intensity, intensity, 1.0f);
	//					glVertex3f(vx, vz, vy);
	//				}
	//				glEnd();
	//			}
	//			node = node->nextLight;
	//		}

	//		// Reset texture mapping state parameters cleanly
	//		vertcount = 0;

	//		// ==============================================================================
	//		// TWIN-PASS CONTEXT PURGE: RESET PIPELINE SPECIFICALLY FOR THE DRAWWALLS2X LOOP
	//		// ==============================================================================
	//		gl_RenderState.EnableFog(true);
	//		gl_RenderState.BlendFunc(GL_ONE, GL_ONE); // Revert back to baseline additive illumination mode
	//		gl_RenderState.SetTextureMode(TM_MODULATE);

	//		glDepthFunc(GL_EQUAL);
	//		glDepthMask(false);

	//		// Force-flush the updated state manager block directly into the hardware context
	//		gl_RenderState.Apply();

	//		// Fallback hardware safety clean overrides
	//		glEnable(GL_FOG);
	//		glBlendFunc(GL_ONE, GL_ONE);
	//		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	//	}
	//	break;

