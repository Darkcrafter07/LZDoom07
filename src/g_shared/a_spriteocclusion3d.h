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
//ExpandUndersizedSpriteDimensionsCachedWrapper(thing, spriteSize, spriteRadius, hasSignificantNegativeOffset,
//                                                                    spriteRasterXdimen, spriteRasterYdimen);
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

void ExpandUndersizedSpriteDimensions(GLSprite* spr, AActor* thing);

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
