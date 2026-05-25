// a_spriteocclusion3d.h - Sprite/Particle 3D occlusion system by Darkcrafter07

#pragma once

#include "r_utility.h"
#include "d_player.h"
#include "g_levellocals.h"

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

extern float Ztolerance2sided, Ztolerance2sidedBot;

bool IsAnamorphicDistanceCulled(AActor* thing, float gl_anamorphic_spriteclip_distance_cull);

float GetActualSpriteFloorZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing);
float GetActualSpriteCeilingZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing);

bool SpriteBboxFacingCameraCrossed1sLineCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed1sidedLinedefCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed1sidedVoidLinedefCachedWrapper(AActor* thing, AActor* viewer);
bool SpriteCrossed2sidedLinedefCachedWrapper(AActor* thing, AActor* viewer);
bool IsSpriteVisibleBehind1sidedLinesCachedWrapper(AActor* thing, AActor* viewer, const DVector3& thingpos);
bool IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(AActor* viewer, AActor* thing);
bool CheckFacingMidTextureProximityWrapper(AActor* thing, AActor* viewer, TVector3<double>& thingpos);
bool IsSpriteVisibleBehind3DFloorSidesCachedWrapper(AActor* viewer, AActor* thing);
bool IsSpriteBehind3DFloorPlaneCachedWrapper(DVector3& cameraPos, DVector3& spritePos, sector_t* sector, AActor* thing);

void ResetAnamorphCache();
