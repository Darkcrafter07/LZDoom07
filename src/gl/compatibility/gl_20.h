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
// - dynlights are now rendered on transluscent map geometry surfaces like
//   3D floors (walls and flats) and also reflective flats;
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
// 2. Multi-Pass Cascade State Protection: Forcing transparent primitives 
//    through static arrays causes the state cacher to drift, throwing 
//    null pointer read crashes or completely culling the background skybox.
//    Solution: Wrap the execution in GLDrawList::DoDraw (gl_drawinfo.cpp).
//    This traps the primitive in the exact microsecond of its true 
//    existence—immediately after the CPU pushes the translucent base.
// 3. The 3D-Floor Overdraw Attenuation Matrix (Anti-Hole Blowouts):
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
// THE UDMF MIRROR SURFACE EXTRACTION SHIELD:
// 1. Fixed-function hardware registers completely collapse when regular 
//    or additive dynamic lights are forced to blend directly over active 
//    planar mirror reflections or specular river overlays, generating a 
//    permanent white-out blowout, and ugly yellow slime streaks.
// 2. The gl_fakeflat.cpp Memory Leak Trap: During runtime sector cloning, 
//    the factory "gl_FakeFlat" routine completely drops and breaks the 
//    "reflect" array pointer inside temporary "dest" structs. Calling 
//    a raw pointer check natively fails, bypassing reflectivity filters.
// 3. Solution: Deploy standalone Thread-Local Context State Links 
//    (g_isCurrentlyGL1xDynlightFlatDrawing / g_isCurrentlyGL1xDynlightWallDrawing) inside 
//    gl_20.cpp, securely bridged with a logic gate invert negation operator 
//    (!isTrueTranslucentFlat) right before calling Setup light systems!
// 4. Symmetrical Sanitizer & Taming: The setup engine calls your custom 
//    standalone "gl_dynlightTameBigLightsOnMirroredSurfacesLegacy" filter 
//    (using correct 0.22f intensity thresholds for monochrome lamps).
//    The flat loop reads uncorrupted master array color signatures directly 
//    inside "level.sectors[sector->sectornum].reflect" (0 == floor). 
//    If any UDMF reflection value or a secondary (renderstyle == STYLE_Add) 
//    mirror pass is detected on the hot sector, the CPU-side dynamic 
//    light brightness injector is forcefully shut down to 0.0f power!
// 5. This shields the mirror buffer registers from experiencing color 
//    overflows, smoothly separating 2x tamed dynamic light level boosts 
//    on true 3D-floors (brightnessBoost = 0.55f / darkSurfLightlevelThresh = 150) 
//    from pristine, deep analog mirror specular reflections on the river!
//
// THREE-PASS CASCADE HARDWARE CONVEYOR:
// * Pass 1 (Modulated Lights): Enforces GL_DST_COLOR, GL_ONE blending 
//   to map standard dynamic projectiles smoothly over visible surfaces.
// * Pass 2 (Additive Lights): Enforces GL_SRC_ALPHA, GL_ONE blending 
//   with soft vertex tinting to project muzzle flares and BFG bursts.
// * Pass 3 (Subtractive Lights): Enforces GL_FUNC_REVERSE_SUBTRACT 
//   equation dynamically coupled with low-level 0.75f CPU-side 
//   attenuation filters inside gl_dynlightHandleSpecialLightsLegacy().
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
// RELATED FILES:
// gl_20.cpp, gl_walls_draw.cpp, gl_flats_draw.cpp, gl_walls.cpp, gl_scene.cpp
//
// gl_sprites.cpp, gl_scene.cpp, gl_models.cpp in brightmaps:
// glEnable(GL_POLYGON_OFFSET_FILL);
// glPolygonOffset(-0.5f, -0.5f);
//
//-------------------------------------------------------------------------


