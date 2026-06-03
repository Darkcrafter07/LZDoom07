//
//---------------------------------------------------------------------------
//
// Copyright(C) 2026 - Vadim Taranov (Darkcrafter07)
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
//    ---=== Sprite/Particle 3D occlusion system by Darkcrafter07 ===---
// ... made specifically for Hybrid Forced-Perspective sprite clipping.
// The code is made for LZDoom07 (fork of LZDoom v3.88b which is GZDoom v3)
// but contains commented out lines for easy porting to modern code base
// like GZDoom v4x / UZDoom v4x. Comment out LZDoom07 and uncomment UZDoom.
// For LZDoom07 use "MIN" and "MAX" and for UZDoom - "min" and "max".
// -------------------------------------------------------------------------



// a_spriteocclusion3d.h - Sprite/Particle 3D occlusion system by Darkcrafter07



#pragma once

// Common includes for both LZDoom07, UZDoom
#include "r_utility.h"
#include "d_player.h"
#include "g_levellocals.h"

// additional includes for LZDoom07 only:
#include "gl/scene/gl_wall.h"
#include "gl/textures/gl_material.h"

// additional includes for UZDoom only:
//#include "events.h"
//#include "texturemanager.h"

#ifdef _MSC_VER
// Everything seems to work just fine even with such warnings.
// Disable warning about conversion from 'double' to 'float', possible loss of data.
#pragma warning(disable:4244)
// Disable warning about 'initializing': truncation from 'double' to 'float'
#pragma warning(disable:4305)
#endif

//	#if defined(__GNUC__) || defined(__clang__)
//	// GCC/Clang: Disable warnings about implicit float conversions.
//	// -Wfloat-conversion handles double to float specifically.
//	// -Wconversion is a broader check for all type casts.
//	#pragma GCC diagnostic ignored "-Wfloat-conversion"
//	#pragma GCC diagnostic ignored "-Wconversion"
//	#endif



// How to call them examples (both LZDoom07 and UZDoom)
// ====================================================
//	//Make sure all 1sided checks and MidTxt checks do NOT have FOV(Frustum Culling) and 2sided - HAVE them instead
//ExpandUndersizedSpriteDimensionsCachedWrapper(this, thing); // LZDoom07 signature
//ExpandUndersizedSpriteDimensionsCachedWrapper(thing); // UZDoom signature
//if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;
//bool viewerCrossed1sLine = ViewerCrossed1sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
//bool thingFacingBboxCrossed1sided = SpriteBboxFacingCameraCrossed1sLineCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed1sidedLine = SpriteCrossed1sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed1sVoidLine = SpriteCrossed1sidedVoidLinedefCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed1sVoidBbox = SpriteCrossed1sidedVoidBboxFaceCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed2sLineSimple = SpriteCrossed2sidedLineSimpleCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed2sBboxFacing = SpriteCrossed2sBBoxFaceLineCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed2sBboxWall = SpriteCrossed2sBboxFaceWallCachedWrapper(thing, r_viewpoint.camera);
//bool visible1sidesInfTallObstr = IsSpriteVisibleBehind1sidedLinesCachedWrapper(thing, r_viewpoint.camera, thingpos);
//bool visible2sideTallEnoughObstr = IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(r_viewpoint.camera, thing);
//bool visible2sideMidTex = CheckFacingMidTextureProximityWrapper(thing, r_viewpoint.camera, thingpos);
//bool visible3dfloorSides = IsSpriteVisibleBehind3DFloorSidesCachedWrapper(r_viewpoint.camera, thing);
//bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, thingpos, thing->Sector, thing);



extern bool anyWallBefore2Sline;
extern float Ztolerance2sided, Ztolerance2sidedBot;

void ExpandUndersizedSpriteDimensions(GLSprite* spr, AActor* thing); // LZDoom07 signature
//void ExpandUndersizedSpriteDimensions(AActor *thing); // UZDoom signature

bool IsAnamorphicDistanceCulled(AActor* thing, float gl_anamorphic_spriteclip_distance_cull);

float GetActualSpriteFloorZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing);
float GetActualSpriteCeilingZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing);

bool ViewerCrossed1sidedLinedefCachedWrapper(AActor *thing, AActor *viewer);
bool SpriteBboxFacingCameraCrossed1sLineCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed1sidedLinedefCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed1sidedVoidLinedefCachedWrapper(AActor *thing, AActor *viewer);
bool SpriteCrossed1sidedVoidBboxFaceCachedWrapper(AActor *thing, AActor *viewer);
bool SpriteCrossed2sidedLineSimpleCachedWrapper(AActor *thing, AActor *viewer);
bool SpriteCrossed2sBBoxFaceLineCachedWrapper(AActor *thing, AActor *viewer);
bool SpriteCrossed2sBboxFaceWallCachedWrapper(AActor *thing, AActor *viewer);
bool IsSpriteVisibleBehind1sidedLinesCachedWrapper(AActor* thing, AActor* viewer, const DVector3& thingpos);
bool IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(AActor* viewer, AActor* thing);
bool CheckFacingMidTextureProximityWrapper(AActor* thing, AActor* viewer, TVector3<double>& thingpos);
bool IsSpriteVisibleBehind3DFloorSidesCachedWrapper(AActor* viewer, AActor* thing);
bool IsSpriteBehind3DFloorPlaneCachedWrapper(DVector3& cameraPos, DVector3& spritePos, sector_t* sector, AActor* thing);

void ResetAnamorphCache();
