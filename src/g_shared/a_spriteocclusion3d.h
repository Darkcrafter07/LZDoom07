// a_spriteocclusion3d.h - Sprite/Particle 3D occlusion system by Darkcrafter07

#pragma once

#include "r_utility.h"
#include "d_player.h"
#include "g_levellocals.h"

// additional includes to port from LZDoom07 to UZDoom/GZDoom v4.14.2
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
//bool thingFacingBboxCrossed1sided = SpriteBboxFacingCameraCrossed1sLineCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed1sidedLine       = SpriteCrossed1sidedLinedefCachedWrapper(thing, r_viewpoint.camera);
//bool thingCrossed1sVoidLine       = SpriteCrossed1sidedVoidLinedefCachedWrapper(thing, r_viewpoint.camera, false);
//bool thingCrossed1sVoidBbox       = SpriteCrossed1sidedVoidBboxFaceCachedWrapper(thing, r_viewpoint.camera, true);
//bool thingCrossed2sidedLine       = SpriteCrossed2sidedLinedefCachedWrapper(thing, r_viewpoint.camera, false);
//bool thingCrossed2sBboxLine       = SpriteCrossed2sBboxFaceCachedWrapper(thing, r_viewpoint.camera, true);
//bool visible1sidesInfTallObstr    = IsSpriteVisibleBehind1sidedLinesCachedWrapper(thing, r_viewpoint.camera, thingpos);
//bool visible2sideTallEnoughObstr  = IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(r_viewpoint.camera, thing);
//bool visible2sideMidTex           = CheckFacingMidTextureProximityWrapper(thing, r_viewpoint.camera, thingpos);
//bool visible3dfloorSides          = IsSpriteVisibleBehind3DFloorSidesCachedWrapper(r_viewpoint.camera, thing);
//bool a3DfloorPlaneObstructed = IsSpriteBehind3DFloorPlaneCachedWrapper(r_viewpoint.Pos, thingpos, thing->Sector, thing);
//	// void detection is still important for "CrossedAnyWall" besides it was already used in "Anamorphosis culling pass 1"
//bool CrossedAnyWall = thingCrossed1sVoidLine || thingCrossed1sidedLine || thingCrossed2sidedLine;
//	// notice "isSprVisibleInObstr" has no "! negation signs" as it means sprite is NOT obstructed and reported as visible
//bool isSprVisibleInObstr =
//	(visible1sidesInfTallObstr || visible2sideTallEnoughObstr || visible2sideMidTex || visible3dfloorSides);



extern float Ztolerance2sided, Ztolerance2sidedBot;

bool IsAnamorphicDistanceCulled(AActor* thing, float gl_anamorphic_spriteclip_distance_cull);

float GetActualSpriteFloorZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing);
float GetActualSpriteCeilingZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing);

bool SpriteBboxFacingCameraCrossed1sLineCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed1sidedLinedefCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed1sidedVoidLinedefCachedWrapper(AActor *thing, AActor *viewer, bool enablebboxface);
bool SpriteCrossed1sidedVoidBboxFaceCachedWrapper(AActor *thing, AActor *viewer, bool enablebboxface);
bool SpriteCrossed2sidedLinedefCachedWrapper(AActor *thing, AActor *viewer, bool checkBboxCameraFace);
bool SpriteCrossed2sBboxFaceWallCachedWrapper(AActor *thing, AActor *viewer, bool checkBboxCameraFace);
bool IsSpriteVisibleBehind1sidedLinesCachedWrapper(AActor* thing, AActor* viewer, const DVector3& thingpos);
bool IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(AActor* viewer, AActor* thing);
bool CheckFacingMidTextureProximityWrapper(AActor* thing, AActor* viewer, TVector3<double>& thingpos);
bool IsSpriteVisibleBehind3DFloorSidesCachedWrapper(AActor* viewer, AActor* thing);
bool IsSpriteBehind3DFloorPlaneCachedWrapper(DVector3& cameraPos, DVector3& spritePos, sector_t* sector, AActor* thing);

void ResetAnamorphCache();
