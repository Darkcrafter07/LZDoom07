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

// gl_20.h - added later in LZDoom07 to include in other rendering files.
// *** Fallback OpenGL rendering code for ancient hardware.
// OpenGL v1.1 is required (1997 cards?), the same file for GL2x path.
// The difference GL2 makes is no blurry textures thanks to NPOT support.

#ifndef __GL_20_H__
#define __GL_20_H__

#include "gl/system/gl_system.h"
#include "menu/menu.h"
#include "tarray.h"
#include "doomtype.h"
#include "m_argv.h"
#include "zstring.h"
#include "i_system.h"
#include "v_text.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "g_levellocals.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/utility/gl_geometric.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_scenedrawer.h"
#include "gl/data/gl_vertexbuffer.h"

#include "gl/shaders/gl_shader.h"



// Menu patching for legacy mode capabilities
void gl_PatchMenu();

// Texture management functions for fixed function pipeline
void gl_SetTextureMode(int type);

// Setup parameters to project one dynamic light onto a wall plane
bool gl_SetupLightWall(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive);

// Setup parameters to project one dynamic light onto a flat plane (floor/ceiling)
bool gl_SetupLightFlat(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive);

//	// Bakes big radius static dynlights into gl_walls_draw.cpp, case "GLPASS_BRIGHTMAP_LEGACY" to save on CPU
//bool gl_GetWallStaticLightmaps(seg_t *seg, float ztop, float zbottom, float *topLightmapColor, float *bottomLightmapColor);
//	// Bakes big radius static dynlights into gl_flats.cpp, case "GLPASS_BRIGHTMAP_LEGACY" to save on CPU
//bool gl_GetFlatStaticLightmaps(subsector_t *sub, const Plane &plane, float *lightmapColor);

// Binds the native dynamic light spotlight filter mask texture (glLight) to the render state
bool gl_SetupLightTexture();

// Main rendering loops for legacy multipass fallback path
void gl_FillScreen();

#endif // __GL_20_H__



// *** Initially, it's a GL1.3 implement. made by Graf Zahl (or somebody else),
// that was wrapped around a GL2 (thus it required GL2 surprisingly).
// That's why LZDoom 3.88b (GZDoom 3.3 fork) required at least GL2 card.
// GL1 support was added in LZDoom07 mainly by editing gl_postprocessstate.cpp,
// by writing a special conversion struct:
// 
// 	// Maps blend modes to a single (Src, Dest) pair required by 
//                                             glBlendFunc (GL1.x)
// struct GL1BlendFuncEntry
// {
// 	int blendSrcRgb;
// 	int blendDestRgb;
// 	int mapSrcRgb;    // The blendSrcRgb  we are looking up
// 	int mapDestRgb;   // The blendDestRgb we are looking up
// };
// 
// ...then npot support was added by resizing all npot textures
// to the square ones via bilinear interpolation
// but there must be some other better, yet undiscovered ways
//
//
//
// *** LZDoom07 is a fork that took it way beyond original capabilites in 2026:
// - dynlights are now rendered to all surfaces (remove faulty normals check);
// - dynlights are now rendered properly even to the foggy surfaces;
// - dynlights rendered even on midtextures with binary transparency;
// - TODO. Dynlights aren't rendered yet on transluscent surfaces
//  (idea to render them just like sprites, that should work);
//
// The software renderer alike camera glow is rendered by spawning
// a special dynlight handled by these files:
// - cvarinfo.zscamglow, zscript.zscamglow, zmapinfo.zscamglow,
//   camglow_handler.zs, camglow_holder.zs, camglow_light.zs;
// - 2 modes of light texture blob projection: standard (straight) and radial
//   they're activated in display options -> fog mode (gl_fogmode);
// - brightmaps that are first converted to colored brightmaps by
//   multiplying bw brightmaps by parent textures in gl_material.cpp
//   and also their alpha cut accordingly. The implementation is spread
//   across gl_sprites.cpp, gl_scene.cpp, gl_models.cpp. TODO:
//   A better idea should be just cut their alpha according to parent texture
//   and according to the brightmap brightness, so the brighter pixel is
//   less transparent it gets. Then draw it on top of parent sprites/surfaces.
//   That should look better but a dynlight must also be taken into account.
//
//
//
// Then GL1.1 mode was added and a subject for improvements. HENCE:
//         ---======= GL1.1 IMPLEMENTATION INFO =======---
// *** OpenGL 1.3+ Native Path (Original Architecture):
//  Utilizes the 'GL_COMBINE' texture environment modes (ARB_texture_env_combine). 
//  This allows the texture hardware unit to act as a discrete pixel pipeline, 
//  splitting processing behavior per-channel. For masked geometry (grates,windows) 
//  it executes a 'GL_REPLACE' operation on RGB channels (retaining the native 
//  unmultiplied sector light value) while executing a 'GL_MODULATE' operation on 
//  the Alpha channel to cleanly carve out transparency transparency thresholds via
//  the hardware Depth Buffer.
//
// *** OpenGL 1.1 Fallback Path:
//  Lacks 'GL_COMBINE' features entirely. The texture environment unit is restricted 
//  to basic linear 'GL_MODULATE' (multiplication) or 'GL_REPLACE' operations across 
//  the entire pixel fragment simultaneously. Splitting individual color and alpha 
//  channel behavior inside a single texture pass,
//  which is physically unsupported by the hardware.
//
//
// *** RESOLVED ARCHITECTURAL ISSUES & FALLBACK IMPLEMENTATION
// * Masked Midtextures (_MASKED Walls & Flats):
//  - Problem: In raw OpenGL 1.1, the lack of combine states forced 'TM_MASK' 
//    to drop down to standard 'GL_MODULATE'. During multi-pass rendering, 
//    this caused the grate textures to be multiplied by the framebuffer multiple 
//    times, squashing the contrast and causing severe darkening at a distance. 
//    Furthermore, when dynamic lightmaps were added, the pixels would flood with 
//    solid light washouts due to unmasked additive blending.
//  - BAD solution (unused): Route '_MASKED' render loops to mimic 1pass sprites.
//    Grates are drawn fully textured directly in Part 2 via standard 'GL_SRC_ALPHA' 
//    blending, completely skipping the third pass multiplication loop. 
//    Dynamic lights are mapped over the pre-existing texture lines using 
//    an explicit sprite-aligned 'GL_DST_COLOR, GL_ONE' blend function, 
//    securing linear light curves, preventing blowout flashes, 
//    and ensuring 100% perfect brightness from any distance.
//    Because even that way these are going to look "fine" in well lit areas
//    but in low lit conditions, these are going to receive LOW amounts of dynlights.
//    These are here for the historic record. The only way to improve the situation
//    was to modify PutWallCompat function to check the lightlevel of a sector the
//    wall was situated in and if it was "lit well enough" - clamp wall light to 128
//    //if (type == RENDERWALL_M2S || type == RENDERWALL_M2SNF)
//    //{
//    //	hasLights = true;
//    //	// We shouldn't brighten masked mid transluscent textures
//    //	// if they're in a dark sector
//    //	bool isSectorPitchBlack = 
//    //	(seg && seg->frontsector && seg->frontsector->lightlevel < 96);
//    //
//    //	if (!isSectorPitchBlack)
//    //	{
//    //		// Dark translucent midtextures receive less dynlights
//    //		// So everything darker gets clipped at lightlevel 128
//    //		if (lightlevel < 128) lightlevel = 128;
//    //	}
//    //}
//  - ***GOOD solution***: mark all transluscent surfaces like "foggy"
//    even if there's no fog.
//    That's going to make them look exactly like GL1.3 ones:
//    Like it's done in PutWallCompat or PutFlatCompat (100% identical blocks):
//    if (gl.gl1path && gl.gl1_v1dot1)
//    {
//    	if (masked) foggy = true;
//    }
//
// * Translucency & Soft Alpha Blending (Marker 99):
//  - Problem: Smooth alpha blending gradients on projectiles, particles, 
//    and explosions regularly vanished from the screen or collapsed into 
//    jagged binary edges because the engine's global Alpha Testing thresholds 
//    dynamically culled pixel fragments whenever alpha values fell below the sector
//    level bounds.
//  - Solution: Implemented a custom texture mode intercept marker (99),
//    inside 'gl_GetRenderStyle'. 
//    When evaluated under the OpenGL 1.1 path, these specific translucent blend styles 
//    programmatically clamp their color vector's alpha channel to a safe '0.51f' floor 
//    on the CPU level ('ApplyFixedFunction') while temporarily neutralizing hardware 
//    Alpha Testing checks. This forces soft particle gradients to reliably pass the 
//    GPU pipeline gates, restoring smooth, rich alpha transparency across all 
//    non-opaque actors.
//
// Put this block in both, otherwise transparent 3D floors won't render in GL1.1
// gl_walls.cpp, in PutWall, before "if (mDrawer->FixedColormap)":
// and in gl_flats.cpp in PutFlat, before "if (renderstyle!=STYLE_Translucent || 
//                        alpha < 1.f - FLT_EPSILON || fog || gltexture == NULL)
//
// 	if (gl.gl1path && gl.gl1_v1dot1)
//{
//	// GL1.1 limitations, GL1.3 does NOT need this
//	// GL1.1 limitation, can't be more transparent than this
//if (alpha <= 0.51f) alpha = 0.51f;
//	// I reckon, if it's super transparent, make it disappear at all
//else if (alpha <= 0.1f)  alpha = 0.01f;
//}
//
//
//-------------------------------------------------------------------------
//               THE TRANSLUSCENT DYNLIGHT 3DFLOOR-SURFACES
//-------------------------------------------------------------------------
// CORE ARCHITECTURE & FIXED-FUNCTION BLINDSPOTS:
// 1. Translucent planes (3D-floors and glass windows) are never baked 
//    into geometric VBO arrays ("dldrawlists") during BSP traversal.
//    Instead, they are compiled dynamically inside the back-to-front 
//    SortNode runtime tree loop at the very end of the scene.
// 2. Forcing these transparent primitives through static multipass 
//    arrays (_MASKED/_FOGMASKED) causes the culling "ss_renderflags" 
//    buffers to return a blind zero. The engine drops texture unit 
//    states completely, clips lights upon camera orbits, or throws 
//    null pointer read crashes (Access Violation inside SetMaterial).
// 3. Solution: Wrap the execution in GLDrawList::DoDraw (gl_drawinfo.cpp).
//    This traps the primitive in the exact microsecond of its true 
//    existence—immediately after the CPU pushes the translucent base.
//    It preserves pristine OpenGL 1.1 blend states, enforces the rigid 
//    GL_LEQUAL depth function, and executes a clean multi-pass cascade 
//    directly over the hot geometry, completely isolating light flags.
// 4. Multi-Layer Overdraw Attenuation Matrix (Anti-Hole Blowouts):
//    To prevent cumulative light stacking spikes (where multi-level 3D 
//    floors overlap inside deep floor holes, multiplying blending 
//    values into radioactive sunset glares), we deploy a CPU-driven 
//    Top-Layer Detector. Since tree traversal renders elements 
//    Back-to-Front (from deep abyss to top lakes), the engine scans 
//    height bounds in real-time. If it finds a higher flat inside the 
//    same sector chunk, it clamps the hardware color channels via 
//    gl_RenderState.SetColorMask(false). The GPU evaluates depth 
//    values cleanly but halts writing color pixels, burying deep 
//    shafts into natural matte darkness while keeping rich RGB color 
//    channels fully open on the upper visible lake surfaces!
//
// THREE-PASS CASCADE HARDWARE CONVEYOR:
// * Pass 1 (Modulated Lights): Enforces GL_DST_COLOR, GL_ONE blending 
//   to map standard dynamic projectiles smoothly over visible surfaces.
// * Pass 2 (Additive Lights): Enforces GL_SRC_ALPHA, GL_ONE blending 
//   with soft vertex tinting to project muzzle flares and BFG bursts.
// * Pass 3 (Subtractive Lights): Enforces GL_FUNC_REVERSE_SUBTRACT 
//   equation directly into GPU registers to render deep blackholes.
// * Hardware Registry Recovery: At the very end of traversal, the loop 
//   triggers an explicit glBlendEquation(GL_FUNC_ADD) hardware reset. 
//   This permanently shields plain walls, HUD elements, and monster 
//   sprites from experiencing color corruption or brightness leakage.
//
// FIXED-FUNCTION PIPELINE RULES:
// * WHAT WE MUST NEVER DO (CRITICAL STATE CORRUPTION TRAPS):
//   - NEVER duplicate translucent pointers inside "PutWall/PutFlat" 
//     list collectors: this fatally breaks VBO index memory bounds.
//   - NEVER cascade separate "DoDraw" passes inside "DoDrawSorted" loop: 
//     shuttling fixed states per-each separate node forces the state 
//     cacher to drift, wiping out monster sprites sorting arrays.
//   - NEVER invoke modelview resets like "glLoadIdentity()" or push 
//     attrib matrix loops: this breaks the viewport culling matrix.
//   - NEVER alter global vertex shading registers using cached 
//     "SetColor" during flat passes: the cacher explicitly strips 
//     vertex alpha layers, instantly culling the background skybox.
//
// FILE: gl_drawinfo.cpp
// METHOD: void GLDrawList::DoDraw(int pass, int i, bool trans)
//
//void GLDrawList::DoDraw(int pass, int i, bool trans)
//{
//	// STEP 1: Render base translucent surface
//	switch(drawitems[i].rendertype)
//	{
//	case GLDIT_FLAT:
//		RenderFlat.Clock();
//		flats[drawitems[i].index].Draw(pass, trans);
//		RenderFlat.Unclock();
//		break;
//	case GLDIT_WALL:
//		RenderWall.Clock();
//		walls[drawitems[i].index].Draw(pass);
//		RenderWall.Unclock();
//		break;
//	case GLDIT_SPRITE:
//		RenderSprite.Clock();
//		sprites[drawitems[i].index].Draw(pass);
//		RenderSprite.Unclock();
//		break;
//	}
//
//	// STEP 2: Hardware color mask filtration
//	if(pass == GLPASS_TRANSLUCENT && gl.legacyMode && GLRenderer->mLightCount)
//	{
//		int type = drawitems[i].rendertype;
//		int idx = drawitems[i].index;
//
//		if(type == GLDIT_FLAT || type == GLDIT_WALL)
//		{
//			// Fog boundary protection
//			if(type == GLDIT_WALL && walls[idx].type == RENDERWALL_FOGBOUNDARY)
//			{
//				float d1 = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y,
//							   walls[idx].glseg.x1, walls[idx].glseg.y1);
//				float d2 = Dist2(r_viewpoint.Pos.X, r_viewpoint.Pos.Y,
//							   walls[idx].glseg.x2, walls[idx].glseg.y2);
//				if(d1 < 576.0f || d2 < 576.0f) return;
//			}
//
//			// Setup light texture
//			if(gl_SetupLightTexture())
//			{
//				// Disable fog and set blend mode
//				gl_RenderState.EnableFog(false);
//				gl_RenderState.BlendFunc(GL_DST_COLOR, GL_ONE);
//				gl_RenderState.Apply();
//
//				glDisable(GL_FOG);
//				glDepthFunc(GL_LEQUAL);
//				glDepthMask(false);
//				glBlendFunc(GL_DST_COLOR, GL_ONE);
//				glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
//
//				// Anti-overdraw engine
//				bool maskOut = false;
//				if(type == GLDIT_FLAT)
//				{
//					GLFlat* f = &flats[idx];
//					if(f && f->sector && (r_viewpoint.Pos.Z - f->z) > 0.0f)
//					{
//						float cz = f->z;
//						for(unsigned j = 0; j < flats.Size(); j++)
//						{
//							if(flats[j].sector == f->sector && flats[j].z > cz)
//							{
//								maskOut = true;
//								break;
//							}
//						}
//					}
//				}
//
//				// Apply color mask if needed
//				if(maskOut)
//				{
//					gl_RenderState.SetColorMask(false, false, false, false);
//					gl_RenderState.ApplyColorMask();
//				}
//
//				// --- PIPELINE CASCADE PASS 1: MODULATED LIGHTS ---
//				if(type == GLDIT_WALL)
//					walls[idx].Draw(GLPASS_LIGHTTEX);
//				else if(type == GLDIT_FLAT)
//					flats[idx].Draw(GLPASS_LIGHTTEX, trans);
//
//				// --- PIPELINE CASCADE PASS 2: ADDITIVE LIGHTS ---
//				glBlendEquation(GL_FUNC_ADD);
//				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
//				glColor4f(0.12f, 0.12f, 0.12f, maskOut ? 0.25f : 1.0f);
//				if(type == GLDIT_WALL)
//					walls[idx].Draw(GLPASS_LIGHTTEX_ADDITIVE);
//				else if(type == GLDIT_FLAT)
//					flats[idx].Draw(GLPASS_LIGHTTEX_ADDITIVE, trans);
//
//				// --- PIPELINE CASCADE PASS 3: SUBTRACTIVE LIGHTS ---
//				glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
//				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
//				glColor4f(0.40f, 0.40f, 0.40f, maskOut ? 0.25f : 1.0f);
//				if(type == GLDIT_WALL)
//					walls[idx].Draw(GLPASS_TRANSLUCENT_LIGHTTEX);
//				else if(type == GLDIT_FLAT)
//					flats[idx].Draw(GLPASS_TRANSLUCENT_LIGHTTEX, trans);
//
//				// Restore state
//				if(maskOut)
//				{
//					gl_RenderState.ResetColorMask();
//					gl_RenderState.ApplyColorMask();
//				}
//
//				glBlendEquation(GL_FUNC_ADD); // Critical hardware fallback reset
//
//				glEnable(GL_FOG);
//				glDepthMask(true);
//				glDepthFunc(GL_LESS);
//				gl_RenderState.EnableFog(true);
//				gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
//				gl_RenderState.Apply();
//			}
//		}
//	}
//}
//
//
// RELATED FILES:
// gl_20.cpp, gl_walls_draw.cpp, gl_flats_draw.cpp, gl_walls.cpp, gl_scene.cpp
//
// gl_sprites.cpp, gl_scene.cpp, gl_models.cpp in brightmaps:
// glEnable(GL_POLYGON_OFFSET_FILL);
// glPolygonOffset(-0.5f, -0.5f);
//
//-------------------------------------------------------------------------


