//-----------------------------------------------------------------------------
//
// Copyright 1993-1996 id Software
// Copyright 1999-2016 Randy Heit
// Copyright 2016 Magnus Norddahl
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//-----------------------------------------------------------------------------
//

#include <stdlib.h>
#include <float.h>
#include "templates.h"
#include "i_system.h"
#include "w_wad.h"
#include "doomdef.h"
#include "doomstat.h"
#include "r_sky.h"
#include "stats.h"
#include "v_video.h"
#include "a_sharedglobal.h"
#include "c_console.h"
#include "cmdlib.h"
#include "d_net.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "swrenderer/scene/r_opaque_pass.h"
#include "r_flatplane.h"
#include "swrenderer/scene/r_3dfloors.h"
#include "v_palette.h"
#include "r_data/colormaps.h"
#include "swrenderer/drawers/r_draw_rgba.h"
#include "a_dynlight.h"
#include "swrenderer/segments/r_clipsegment.h"
#include "swrenderer/segments/r_drawsegment.h"
#include "swrenderer/scene/r_portal.h"
#include "swrenderer/scene/r_scene.h"
#include "swrenderer/scene/r_light.h"
#include "swrenderer/plane/r_visibleplane.h"
#include "swrenderer/viewport/r_viewport.h"
#include "swrenderer/r_memory.h"
#include "swrenderer/r_renderthread.h"

namespace swrenderer
{
	RenderFlatPlane::RenderFlatPlane(RenderThread *thread)
	{
		Thread = thread;
	}

	void RenderFlatPlane::Render(VisiblePlane *pl, double _xscale, double _yscale, fixed_t alpha, bool additive, bool masked, FDynamicColormap *colormap, FTexture *texture)
	{
		if (alpha <= 0)
		{
			return;
		}

		drawerargs.SetSolidColor(3);
		drawerargs.SetTexture(Thread, texture);

		float planeang = (pl->xform.Angle + pl->xform.baseAngle).Radians();
		float xstep, ystep, leftxfrac, leftyfrac, rightxfrac, rightyfrac;
		float x;

		if (planeang != 0)
		{
			float cosine = cos(planeang), sine = sin(planeang);
			pviewx = pl->xform.xOffs + Thread->Viewport->viewpoint.Pos.X * cosine - Thread->Viewport->viewpoint.Pos.Y * sine;
			pviewy = pl->xform.yOffs + pl->xform.baseyOffs - Thread->Viewport->viewpoint.Pos.X * sine - Thread->Viewport->viewpoint.Pos.Y * cosine;
		}
		else
		{
			pviewx = pl->xform.xOffs + Thread->Viewport->viewpoint.Pos.X;
			pviewy = pl->xform.yOffs - Thread->Viewport->viewpoint.Pos.Y;
		}

		pviewx = _xscale * pviewx;
		pviewy = _yscale * pviewy;

		// left to right mapping
		planeang += (Thread->Viewport->viewpoint.Angles.Yaw - 90).Radians();

		auto viewport = Thread->Viewport.get();

		// Scale will be unit scale at FocalLengthX (normally SCREENWIDTH/2) distance
		xstep = cos(planeang) / viewport->FocalLengthX;
		ystep = -sin(planeang) / viewport->FocalLengthX;

		// [RH] flip for mirrors
		RenderPortal *renderportal = Thread->Portal.get();
		if (renderportal->MirrorFlags & RF_XFLIP)
		{
			xstep = -xstep;
			ystep = -ystep;
		}

		planeang += M_PI * 0.5f;
		float cosine = cos(planeang), sine = -sin(planeang);
		x = pl->right - viewport->CenterX + 0.5;
		rightxfrac = _xscale * (cosine + x * xstep);
		rightyfrac = _yscale * (sine + x * ystep);
		x = pl->left - viewport->CenterX + 0.5;
		leftxfrac = _xscale * (cosine + x * xstep);
		leftyfrac = _yscale * (sine + x * ystep);

		basexfrac = leftxfrac;
		baseyfrac = leftyfrac;
		if (pl->left != pl->right)
		{
			xstepscale = (rightxfrac - leftxfrac) / (pl->right - pl->left);
			ystepscale = (rightyfrac - leftyfrac) / (pl->right - pl->left);
		}
		else
		{
			xstepscale = 0;
			ystepscale = 0;
		}

		minx = pl->left;

		planeheight = fabs(pl->height.Zat0() - Thread->Viewport->viewpoint.Pos.Z);

		basecolormap = colormap;

		// [RH] set foggy flag
		bool foggy = (level.fadeto || basecolormap->Fade || (level.flags & LEVEL_HASFADETABLE));

		GlobVis = Thread->Light->FlatPlaneGlobVis(foggy) / planeheight;

		CameraLight *cameraLight = CameraLight::Instance();
		if (cameraLight->FixedLightLevel() >= 0)
		{
			drawerargs.SetLight(basecolormap, 0, cameraLight->FixedLightLevelShade());
			plane_shade = false;
		}
		else if (cameraLight->FixedColormap())
		{
			drawerargs.SetLight(cameraLight->FixedColormap(), 0, 0);
			plane_shade = false;
		}
		else
		{
			plane_shade = true;
			planeshade = LightVisibility::LightLevelToShade(pl->lightlevel, foggy);
		}

		drawerargs.SetStyle(masked, additive, alpha);

		light_list = pl->lights;

		RenderLines(pl);
	}

	void RenderFlatPlane::RenderLine(int y, int x1, int x2)
	{
#ifdef RANGECHECK
		if (x2 < x1 || x1 < 0 || x2 >= viewwidth || (unsigned)y >= (unsigned)viewheight)
		{
			I_Error("R_MapPlane: %i, %i at %i", x1, x2, y);
		}
#endif

		auto viewport = Thread->Viewport.get();

		float curxfrac = basexfrac + xstepscale * (x1 - minx);
		float curyfrac = baseyfrac + ystepscale * (x1 - minx);

		float distance = viewport->PlaneDepth(y, planeheight);

		float zbufferdepth = (float)(1.0 / fabs(planeheight / Thread->Viewport->ScreenToViewY(y, 1.0f)));

		drawerargs.SetTextureUStep(distance * xstepscale / drawerargs.TextureWidth());
		drawerargs.SetTextureUPos((distance * curxfrac + pviewx) / drawerargs.TextureWidth());

		drawerargs.SetTextureVStep(distance * ystepscale / drawerargs.TextureHeight());
		drawerargs.SetTextureVPos((distance * curyfrac + pviewy) / drawerargs.TextureHeight());

		if (viewport->RenderTarget->IsBgra())
		{
			float distance2 = viewport->PlaneDepth(y + 1, planeheight);
			float xmagnitude = fabs(ystepscale * (distance2 - distance) * viewport->FocalLengthX);
			float ymagnitude = fabs(xstepscale * (distance2 - distance) * viewport->FocalLengthX);
			float magnitude = MAX(ymagnitude, xmagnitude);
			float min_lod = -1000.0f;
			drawerargs.SetTextureLOD(MAX(log2(magnitude) + r_lod_bias, min_lod));
		}

		if (plane_shade)
		{
			// Determine lighting based on the span's distance from the viewer.
			drawerargs.SetLight(basecolormap, (float)(GlobVis * fabs(viewport->CenterY - y)), planeshade);
		}

		if (r_dynlights && light_list)
		{
			const auto &vp = viewport->viewpoint;

			int tx = x1;
			bool mirror = !!(Thread->Portal->MirrorFlags & RF_XFLIP);
			if (mirror)
				tx = viewwidth - tx - 1;

			// Find row position in view space
			float zspan = (float)(planeheight / (fabs(y + 0.5 - viewport->CenterY) / viewport->InvZtoScale));
			drawerargs.dc_viewpos.X = (float)((tx + 0.5 - viewport->CenterX) / viewport->CenterX * zspan);
			drawerargs.dc_viewpos.Y = zspan;
			drawerargs.dc_viewpos.Z = (float)((viewport->CenterY - y - 0.5) / viewport->InvZtoScale * zspan);
			drawerargs.dc_viewpos_step.X = (float)(zspan / viewport->CenterX);

			if (mirror) drawerargs.dc_viewpos_step.X = -drawerargs.dc_viewpos_step.X;

			// Plane normal
			drawerargs.dc_normal.X = 0.0f;
			drawerargs.dc_normal.Y = 0.0f;
			drawerargs.dc_normal.Z = (y >= viewport->CenterY) ? 1.0f : -1.0f;

			// Hoist invariant camera & row constants outside the light loop
			const double camX = vp.Pos.X;
			const double camY = vp.Pos.Y;
			const double camZ = vp.Pos.Z;
			const double viewSin = vp.Sin;
			const double viewCos = vp.Cos;
			const double viewTanSin = vp.TanSin;
			const double viewTanCos = vp.TanCos;
			const float viewPosY = drawerargs.dc_viewpos.Y;
			const float viewPosZ = drawerargs.dc_viewpos.Z;
			const float normZ = drawerargs.dc_normal.Z;
			const bool doOcclude = (r_dynlights_occlude != 0);

			// Single-pass stack collection: avoids 3x list traversal, thread-unsafe Deactivate(),
			// and zero-light FrameMemory allocations on every horizontal scanline.
			enum { MAX_ROW_LIGHTS = 64 };
			DrawerLight temp_lights[MAX_ROW_LIGHTS];
			int num_lights = 0;

			const int camPortalGroup = (vp.camera && vp.camera->Sector) ? vp.camera->Sector->PortalGroup : 0;

			for (VisiblePlaneLight *cur_node = light_list; cur_node != nullptr && num_lights < MAX_ROW_LIGHTS; cur_node = cur_node->next)
			{
				FDynamicLight *light = cur_node->lightsource;
				if (light == nullptr || !light->IsActive() || light->target == nullptr) continue;

				float radius = light->GetRadius();
				if (radius <= 0.0f) continue;

				const bool isCamglowDynlightHere = light->IsDynlightCamglow();

				//Camglow radius is 2x to reduce BSP traversal early exit surface skip artifacts in GL1x/GL2x
				if (isCamglowDynlightHere) radius *= 0.5f;

				// 1. Portal-safe distance cull using local viewport position (vp.Pos)
				const float lightX = light->X() - camX;
				const float lightY = light->Y() - camY;
				const float lightZ = light->Z() - camZ;

				if (!isCamglowDynlightHere && r_dynlight_distance_cull > 0.0f)
				{
					const float maxDist = (float)(r_dynlight_distance_cull + radius);
					const float distSq3D = lightX * lightX + lightY * lightY + lightZ * lightZ;
					if (distSq3D > maxDist * maxDist) continue;
				}

				// 2. Scanline 2D touch test (runs BEFORE expensive occlusion checks!)
				const float lx = (float)(lightX * viewSin - lightY * viewCos);
				const float ly = (float)(lightX * viewTanCos + lightY * viewTanSin) - viewPosY;
				const float lz = (float)lightZ - viewPosZ;

				const bool is_point_light = light->IsAttenuated();
				const float lconstant = ly * ly + lz * lz;
				const float nlconstant = is_point_light ? (lz * normZ) : 0.0f;

				// Skip light immediately if it doesn't reach this scanline
				if (radius * radius < lconstant || nlconstant < 0.0f) continue;

				// 3. Software renderer visual occlusion shield (skip across different portal groups)
				sector_t *checkSector = light->Sector ? light->Sector : light->target->Sector;
				const int currentGroup = checkSector ? checkSector->PortalGroup : 0;

				if (doOcclude && !isCamglowDynlightHere && radius <= 468.0f && currentGroup == camPortalGroup)
				{
					DVector3 lightPosAbs = light->PosAbsolute(currentGroup);
					const float adjustedRadius = radius * 0.85f;

					bool isDynlightVisibleBehind2Sobstr = IsDynlightVisibleBehind2sidedLinedefSectObstrWrapperCached(vp.camera, light->target, adjustedRadius, true);
					bool isDynlightObstructedBy3DFloorFlats = IsDynlightBehind3DFloorPlaneCachedWrapper(Thread->Viewport->viewpoint.Pos, lightPosAbs, checkSector, light->target, adjustedRadius, true);
					bool isDynlightVisibleBehind3DFloorSides = IsDynlightVisibleBehind3DFloorSidesCachedWrapper(vp.camera, light->target, adjustedRadius, true);

					if (!isDynlightVisibleBehind2Sobstr || !isDynlightVisibleBehind3DFloorSides || isDynlightObstructedBy3DFloorFlats) continue;
				}

				const uint32_t red = light->GetRed();
				const uint32_t green = light->GetGreen();
				const uint32_t blue = light->GetBlue();

				DrawerLight &dl = temp_lights[num_lights++];
				dl.x = lx;
				dl.y = lconstant;
				dl.z = nlconstant;
				dl.radius = 256.0f / radius;
				dl.color = (red << 16) | (green << 8) | blue;
			}

			drawerargs.dc_num_lights = num_lights;
			if (num_lights > 0)
			{
				drawerargs.dc_lights = Thread->FrameMemory->AllocMemory<DrawerLight>(num_lights);
				memcpy(drawerargs.dc_lights, temp_lights, num_lights * sizeof(DrawerLight));
			}
		}
		else
		{
			drawerargs.dc_num_lights = 0;
		}

		drawerargs.SetDestY(viewport, y);
		drawerargs.SetDestX1(x1);
		drawerargs.SetDestX2(x2);

		drawerargs.DrawSpan(Thread);
		if (r_modelscene && r_models_carmack)
			drawerargs.DrawDepthSpan(Thread, zbufferdepth, zbufferdepth);
	}

	/////////////////////////////////////////////////////////////////////////
	
	RenderColoredPlane::RenderColoredPlane(RenderThread *thread)
	{
		Thread = thread;
	}

	void RenderColoredPlane::Render(VisiblePlane *pl)
	{
		RenderLines(pl);
	}

	void RenderColoredPlane::RenderLine(int y, int x1, int x2)
	{
		drawerargs.DrawColoredSpan(Thread, y, x1, x2);
	}
}
