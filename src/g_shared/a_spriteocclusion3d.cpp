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



// a_spriteocclusion3d.cpp
// Sprite/Particle 3D occlusion system by Darkcrafter07



#include "a_spriteocclusion3d.h"



//==========================================================================
//
// Sprite 3D Occlusion Culling - START
//
//==========================================================================

bool enableAnamorphCache = true; // gives it a really considerable speed-up

// In case you don't have a "floorf" function
//_Check_return_ __inline float __CRTDECL floorf(_In_ float _X)
//{
//	return (float)floor(_X);
//}
//
//_Check_return_ inline float floor(_In_ float _Xx) _NOEXCEPT
//{
//	return (_CSTD floorf(_Xx));
//}



//                                ----------------
//         ---===      ***************************************        ===---
// ******* Anamorphic "Forced-Pespective" common early exit checks - start *******

// Distance cull checks on big maps with lots of 3D-floors to speed up
bool IsAnamorphicDistanceCulled(AActor *thing, float gl_anamorphic_spriteclip_distance_cull)
{
	if (!thing) return true; // Handle null pointers
	if (gl_anamorphic_spriteclip_distance_cull <= 0.0f) return false;

	const float cullDist = gl_anamorphic_spriteclip_distance_cull;
	const float cullDistSq = cullDist * cullDist;

	return (thing->Pos() - r_viewpoint.Pos).LengthSquared() > cullDistSq;
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// Frustum culling stub - keep for 1sided and midtxt checks
// Because with the FOV check they start leaking HEAVILY
static bool CheckFrustumCullingUNUSED(AActor *thing)
{
	return false; // Not culled by frustum
}

// A totally different story for 2 sided walls / 3D-floors
// USE the FOV check otherwise due to caching system imperfection
// You will see leaks if turn the head around fast!
static bool CheckFrustumCulling(AActor *thing)
{
	const DVector3 viewerPos = r_viewpoint.Pos;
	const DVector3 thingPos = thing->Pos();

	// Extract the ACTUAL current FOV from the viewpoint
	// r_viewpoint.FOV is already in degrees, so we don't need to convert
	float currentFOV = r_viewpoint.FieldOfView.Degrees;     // LZDoom07 way
	//float currentFOV = r_viewpoint.FieldOfView.Degrees();     // UZDoom way
	float currentFOVenlarged = currentFOV * 1.5f;

	// Calculate frustum based on tilt
	float tilt = fabs(static_cast<float>(r_viewpoint.Angles.Pitch.Degrees)); // LZDoom07 way
	//float tilt = fabs(static_cast<float>(r_viewpoint.Angles.Pitch.Degrees())); // UZDoom way
	if (tilt > 46.0f) return false; // Don't cull at extreme angles

	// Use the actual FOV instead of hardcoded 90
	float floatangle = 2.0f + (45.0f + (tilt / 1.9f)) * currentFOVenlarged * 48.0f /
		                AspectMultiplier(r_viewwindow.WidescreenRatio) / currentFOV;
	angle_t frustumAngle = DAngle(floatangle).BAMs();           // LZDoom07 way
	//angle_t frustumAngle = DAngle::fromDeg(floatangle).BAMs();    // UZDoom way

	if (frustumAngle < ANGLE_180)
	{
		DVector2 spriteVec(thingPos.X - viewerPos.X, thingPos.Y - viewerPos.Y);
		angle_t  spriteAng = spriteVec.Angle().BAMs();
		angle_t  viewerAng = r_viewpoint.Angles.Yaw.BAMs();

		angle_t diff1 = spriteAng > viewerAng ? spriteAng - viewerAng : viewerAng - spriteAng;
		angle_t angleDiff = (diff1 > ANGLE_MAX / 2) ? ANGLE_MAX - diff1 : diff1;

		if (angleDiff > frustumAngle / 2)
		{
			return true; // Culled by frustum
		}
	}

	return false; // Not culled by frustum
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
struct LineSegmentCommon
{
	float x1, y1, x2, y2;

	LineSegmentCommon(float x1, float y1, float x2, float y2) : x1(x1), y1(y1), x2(x2), y2(y2)
	{
	}

	bool IntersectsCommon(const LineSegmentCommon &other) const
	{
		auto cross = [](float ax, float ay, float bx, float by) { return ax * by - ay * bx; };

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
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// Function to check if a wall is thin (less than 12 units thick)
// how to call: bool thisisathinwall = IsThinWallCommon(thing, r_viewpoint.camera, thingpos);
static bool IsThinWallCommon(AActor *viewer, AActor *thing, DVector3 &thingpos)
{
	// Early exit for invalid inputs
	if (!viewer || !thing) return false;

	sector_t *viewSector = viewer->Sector;
	sector_t* thingSector = P_PointInSector(thingpos.X, thingpos.Y); // LZDoom07 way
	//sector_t *thingSector = level.PointInSector(thingpos); // UZDoom way

	// Only check if in different sectors
	if (viewSector == thingSector) return false;

	DVector3          viewerPos = viewer->Pos();
	LineSegmentCommon sight(viewerPos.X, viewerPos.Y, thingpos.X, thingpos.Y);

	// Precompute perpendicular direction for thickness checks
	const DVector2 perpDirs[2] =
	{
		{0,  1}, // Positive perpendicular
		{0, -1}  // Negative perpendicular
	};

	// Check both sectors
	for (auto *sector : { viewSector, thingSector })
	{
		for (auto &line : sector->Lines)
		{
			LineSegmentCommon wall(line->v1->fX(), line->v1->fY(), line->v2->fX(), line->v2->fY());

			if (wall.IntersectsCommon(sight))
			{
				// Get the other sector
				sector_t *otherSector = (sector == viewSector) ? thingSector : viewSector;

				// Wall direction vector
				DVector2 wallDir(line->v2->fX() - line->v1->fX(), line->v2->fY() - line->v1->fY());
				float    wallLength = wallDir.Length();

				if (wallLength <= 0) continue;

				// Normalize wall direction
				wallDir /= wallLength;

				// Sample points along the wall (3 points)
				for (float t : {0.2f, 0.5f, 0.8f})
				{
					DVector2 wallPoint(line->v1->fX() + wallDir.X * wallLength * t,
						              line->v1->fY() + wallDir.Y * wallLength * t);

					// Check thickness in both perpendicular directions
					for (const auto &perp : perpDirs)
					{
						DVector2 testPerp(perp.Y * wallDir.X - perp.X * wallDir.Y,
							             perp.X * wallDir.Y - perp.Y * wallDir.X);

						// Ray cast for thickness
						for (float rayDist = 1.0f; rayDist <= 15.0f; rayDist += 1.0f)
						{
							DVector2 testPos = wallPoint + testPerp * rayDist;

							if (P_PointInSector(testPos.X, testPos.Y) == otherSector) // LZDoom07 way
							//if (level.PointInSector(testPos) == otherSector) // UZDoom way
							{
								if (rayDist < 12.0f) return true; // Thin wall found
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
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// Wrapper function - to call struct LineSegment1sided from any other place
// bool thingCrossed1sidedLine = SpriteIntersectsLinedef(viewerPos.X, viewerPos.Y, edgeX, edgeY, line->v1->fX(),
//                                                      line->v1->fY(), line->v2->fX(), line->v2->fY(), ix, iy);
static bool SpriteIntersectsLinedef(float x1, float y1, float x2, float y2, float ox1, float oy1, 
	                                                  float ox2, float oy2, float &ix, float &iy)
{
	LineSegmentCommon seg1(x1, y1, x2, y2);
	LineSegmentCommon seg2(ox1, oy1, ox2, oy2);

	if (!seg1.IntersectsCommon(seg2)) return false;

	// Calculate intersection point
	float denom = (x1 - x2) * (oy1 - oy2) - (y1 - y2) * (ox1 - ox2);
	float t = ((x1 - ox1) * (oy1 - oy2) - (y1 - oy1) * (ox1 - ox2)) / denom;
	ix = x1 + t * (x2 - x1);
	iy = y1 + t * (y2 - y1);

	return true;
}

// === CACHED version of SpriteIntersectsLinedef - START === UNUSED
struct SpriteIntersectsLinedefCacheKey
{
	float segmentX;
	float segmentY;
	SpriteIntersectsLinedefCacheKey(float sx, float sy) : segmentX(sx), segmentY(sy)
	{
	}
	bool operator==(const SpriteIntersectsLinedefCacheKey &other) const
	{
		return segmentX == other.segmentX && segmentY == other.segmentY;
	}
	bool operator!=(const SpriteIntersectsLinedefCacheKey &other) const
	{
		return !(*this == other);
	}
	bool operator<(const SpriteIntersectsLinedefCacheKey &other) const
	{
		if (segmentX != other.segmentX)
			return segmentX < other.segmentX;
		return segmentY < other.segmentY;
	}
};

template <> struct THashTraits<SpriteIntersectsLinedefCacheKey>
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
	int   lastMapTimeUpdateTick = -1;
	bool  cachedSpriteIntersectsLine = false;
	bool  cachedSpriteIntersectsLineValid = false;
	float cached_ix = 0.0f, cached_iy = 0.0f;
};
static TMap<line_t *, TMap<SpriteIntersectsLinedefCacheKey, spriteIntersectsLineCacheEntry>> SpriteIntersectsLineCache;

bool spriteIntersectsLineCachedWrapper(line_t *line, float x1, float y1, float x2, float y2, 
	                           float x3, float y3, float x4, float y4, float &ix, float &iy)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		// Generate a unique key for this segment (using start and end points)
		SpriteIntersectsLinedefCacheKey key(x1 * 1000.0f + x2, y1 * 1000.0f + y2);
		spriteIntersectsLineCacheEntry &entry = SpriteIntersectsLineCache[line][key];
		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8 &&
			                                                         entry.cachedSpriteIntersectsLineValid)
		{
			ix = entry.cached_ix;
			iy = entry.cached_iy;
			return entry.cachedSpriteIntersectsLine;
		}
		// Compute fresh result and cache it
		entry.cachedSpriteIntersectsLine = SpriteIntersectsLinedef(x1, y1, x2, y2, x3, y3, 
			                                    x4, y4, entry.cached_ix, entry.cached_iy);
		entry.cachedSpriteIntersectsLineValid = true;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		// Set output values
		ix = entry.cached_ix; iy = entry.cached_iy;

		return entry.cachedSpriteIntersectsLine;
	}
	else
	{
		// Original uncached behavior
		return SpriteIntersectsLinedef(x1, y1, x2, y2, x3, y3, x4, y4, ix, iy);
	}
}
// === CACHED version of SpriteIntersectsLinedef - FINISH === UNUSED
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// ======= Detect big undersized sprites and expand their dimensions =======
bool isExpSprWorthMoreCull = false;
bool isMicroSprDimExp, isTinySprDimExp, isSmallSprDimExp, isMedSprDimExp, isOtherSprDimExp;
float spriteSizeExp, spriteRadiusExp; // declare them here and extern in gl_sprite.cpp/hw_sprites.cpp
bool hasSignificantNegativeOffset; int spriteRasterXdimen, spriteRasterYdimen;

void ExpandUndersizedSpriteDimensions(GLSprite* spr, AActor* thing) // LZDoom07 signature
//void ExpandUndersizedSpriteDimensions(AActor *thing) // UZDoom signature
{
	if (!thing) return;

	spriteRadiusExp = (float)thing->radius;
	spriteSizeExp = (thing->radius + thing->Height) * 0.5f;

	// === LZDoom07 way - START ===================================================================
	int spriteFileOffset = 0;      // Blank rows at top of texture (from file)
	spriteRasterXdimen = 0;        // Total texture width (including blank columns)
	spriteRasterYdimen = 0;        // Total texture height (including blank rows)
	hasSignificantNegativeOffset = false; // Reset the output reference directly
	//
	// ----------- OpenGL legacy way (faster) - START --------------
	if (spr->gltexture && spr->gltexture->tex)
	{
		FTexture* tex = spr->gltexture->tex;
		if (tex)
		{
			spriteRasterXdimen = tex->GetWidth();
			spriteRasterYdimen = tex->GetHeight();
			spriteFileOffset = tex->TopOffset;
			// Calculate visible sprite height (actual drawn pixels)
			const int visibleSpriteHeight = spriteRasterYdimen - spriteFileOffset;
			hasSignificantNegativeOffset = (visibleSpriteHeight >= 1);
		}
	}
	// ----------- OpenGL legacy way (faster) - FINISH --------------
	//
	// ----------- Renderer independed way (slower) - START --------------
	//if (TexMan.NumTextures() > 0)
	//{
	//	FTexture* tex = TexMan.ByIndex(thing->sprite); // Safe resource resolution pass
	//	if (tex)
	//	{
	//		spriteRasterXdimen = tex->GetWidth();
	//		spriteRasterYdimen = tex->GetHeight();
	//		spriteFileOffset = tex->TopOffset;
	//		// Calculate visible sprite height (actual drawn pixels)
	//		const int visibleSpriteHeight = spriteRasterYdimen - spriteFileOffset;
	//		hasSignificantNegativeOffset = (visibleSpriteHeight >= 1);
	//	}
	//}
	// ----------- Renderer independed way (slower) - FINISH --------------
	// === LZDoom07 way - FINISH===================================================================

	// === UZDoom way - START =====================================================================
	//int           spriteFileOffset = 0; // Blank rows at top of texture (from file)
	//int           spriteRasterXdimen = 0; // Total texture width (including blank columns)
	//int           spriteRasterYdimen = 0; // Total texture height (including blank rows)
	//bool          hasSignificantNegativeOffset = false;
	//FGameTexture *gtex = nullptr;
	//
	//// First, check for a direct texture override (picnum)
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
	//		// GetWidth/Height automatically account for Scale.X/Scale.Y
	//		spriteRasterXdimen = tex->GetWidth();
	//		spriteRasterYdimen = tex->GetHeight();
	//		spriteFileOffset = tex->TopOffset;
	//		// Calculate visible sprite height (actual drawn pixels)
	//		int visibleSpriteHeight = spriteRasterYdimen - spriteFileOffset;
	//		hasSignificantNegativeOffset = (visibleSpriteHeight >= 1);
	//		// Debug output to verify dimensions
	//		//Printf("Sprite Resolve: %s | RasterH: %d | TopOff: %d | Visible: %d\n", gtex->GetName().GetChars(),
	//		//       spriteRasterYdimen, spriteFileOffset, visibleSpriteHeight);
	//	}
	//}
	// === UZDoom way - FINISH ====================================================================

	const bool isMicroSprite = (spriteSizeExp <= 12.0f);
	const bool isTinySprite = (spriteSizeExp > 12.0f  && spriteSizeExp <= 18.0f);
	const bool isSmallSprite = (spriteSizeExp > 18.0f  && spriteSizeExp <= 38.0f);
	const bool isMediumSprite = (spriteSizeExp > 38.0f  && spriteSizeExp <= 45.0f);

	const bool islegacyversionprojectile =
		(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
		(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
		(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);
	const bool islegacyversionmonster =
		(thing->flags & MF_SHOOTABLE) && (thing->flags & MF_COUNTKILL) &&
		(thing->flags & MF_SOLID) && (thing->flags2 & MF2_PUSHWALL) &&
		(thing->flags4 & MF4_CANUSEWALLS) && (thing->flags2 & MF2_MCROSS) &&
		(thing->flags2 & MF2_PASSMOBJ) && (thing->flags3 & MF3_ISMONSTER);
	const bool isfloatingmonster =
		(thing->flags & MF_SHOOTABLE) && (thing->flags & MF_COUNTKILL) &&
		(thing->flags & MF_SOLID) && (thing->flags2 & MF2_PUSHWALL) &&
		(thing->flags4 & MF4_CANUSEWALLS) && (thing->flags2 & MF2_MCROSS) &&
		(thing->flags2 & MF2_PASSMOBJ) && (thing->flags3 & MF3_ISMONSTER) &&
		(thing->flags & MF_FLOAT || thing->flags & MF_INFLOAT);
	const bool isfloatingsprite = (thing->flags & MF_FLOAT || thing->flags & MF_INFLOAT);
	const bool isactoracorpse = (thing->flags & MF_CORPSE) || (thing->flags & MF_ICECORPSE);
	const bool isactorsmallbutnotcorpse = (spriteSizeExp >= 8.0f && spriteSizeExp <= 18.0f) && 
		                                                                      !isactoracorpse;
	const bool isaregularsizedmonster = (islegacyversionmonster && (spriteSizeExp <= 38.0f));
	const bool isabonusitem = ((spriteSizeExp >= 16.0f && spriteSizeExp <= 25.0f) &&
		(!islegacyversionmonster || !isfloatingsprite || !isactoracorpse));

	const bool smalDimSprExpansionSafeMode = false; // Enable turbo expansion mode
	const bool sprDimTooSmall = (spriteRasterXdimen >= thing->radius) || (spriteRasterYdimen >= thing->Height);
	const bool sprForExpansion = (islegacyversionmonster || islegacyversionprojectile || isactoracorpse ||
                                      isactorsmallbutnotcorpse || isaregularsizedmonster || isabonusitem);

	isExpSprWorthMoreCull = (!islegacyversionprojectile || !islegacyversionmonster ||
		                                   !isaregularsizedmonster || !isabonusitem);

	if (sprForExpansion && sprDimTooSmall) // Start expanding
	{
		if (smalDimSprExpansionSafeMode)
		{
			// Light expansion mode
			if (isMicroSprite)
			{
				if (spriteRasterXdimen >= 64.0f || spriteRasterYdimen >= 64.0f)
				{
					spriteSizeExp = 4.8f; spriteRadiusExp = 4.8f;
				}
				else
				{
					spriteSizeExp = 2.12f; spriteRadiusExp = 2.12f;
				}
				isMicroSprDimExp = true;
			}
			else if (isTinySprite)   { spriteSizeExp += 1.0f;  spriteRadiusExp += 1.0f; isTinySprDimExp  = true; }
			else if (isSmallSprite)  { spriteSizeExp += 1.2f;  spriteRadiusExp += 1.2f; isSmallSprDimExp = true; }
			else if (isMediumSprite) { spriteSizeExp += 2.4f;  spriteRadiusExp += 2.4f; isMedSprDimExp   = true; }
			else                     { spriteSizeExp *= 1.1f;  spriteRadiusExp *= 1.1f; isOtherSprDimExp = true; }
		}
		else
		{
			// Turbo expansion mode
			if (isMicroSprite)
			{
				if (spriteRasterXdimen >= 64.0f || spriteRasterYdimen >= 64.0f)
				{
					spriteSizeExp = 16.4f; spriteRadiusExp = 16.4f;
				}
				else
				{
					spriteSizeExp = 12.24f; spriteRadiusExp = 12.24f;
				}
				isMicroSprDimExp = true;
			}
			else if (isTinySprite)   { spriteSizeExp += 2.0f;  spriteRadiusExp += 2.0f; isTinySprDimExp  = true; }
			else if (isSmallSprite)  { spriteSizeExp += 4.4f;  spriteRadiusExp += 4.4f; isSmallSprDimExp = true; }
			else if (isMediumSprite) { spriteSizeExp += 7.8f;  spriteRadiusExp += 7.8f; isMedSprDimExp   = true; }
			else                     { spriteSizeExp *= 1.5f;  spriteRadiusExp *= 1.5f; isOtherSprDimExp = true; }
		}
	}
}

// ******* Anamorphic "Forced-Pespective" common early exit checks - finish *******
//         ---===      ***************************************        ===---
//                                ----------------







//                                ----------------
//         ---===      ***************************************        ===---
// ******* 1-sided-linedef culling block start *******

// This one checks if the line of sight vectors between the viewer and the sprite's bounding box
// physically cross any 1-sided void/border linedefs belonging strictly to the sprite's native sector.
bool ViewerCrossed1sidedLinedef(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	// The FOV check is VERY BAD for 1 sided stuff!
	//if (CheckFrustumCullingUNUSED(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// ONLY check sprite's sector, never viewer's sector
	sector_t *thingSector = thing->Sector;
	if (!thingSector || thingSector->Lines.Size() == 0) return false;

	// Calculate viewer->thing line geometry
	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	const float spriteSize = (thing->radius + thing->Height) * 0.5f;

	// Sprite size thresholds
	const bool isMicroSprite = (spriteSize <= 8.0f);
	const bool isTinySprite = (spriteSize <= 12.0f);
	const bool isSmallSprite = (spriteSize <= 18.0f);
	const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 38.0f);
	const bool isLargeSprite = (spriteSize > 38.0f && spriteSize < 39.0f);
	const bool isHugeSprite = (spriteSize >= 39.0f);

	// Scale adjustments matching your precise defensive tight metrics
	// Default: micro/tiny/small sprites
	float                        spriteScale = 0.44f;
	if      (isMediumSprite)     spriteScale = 0.5f;
	else if (isLargeSprite)      spriteScale = 0.15f;
	else if (isHugeSprite)       spriteScale = 0.1f;

	// Calculate scaled test points based on radius + size
	float adjustedRadius = thing->radius * spriteScale;

	// Generate 4 orthogonal test points around the sprite center (The Cross Matrix)
	float testPoints[4][2] =
	{
		{ thingX + adjustedRadius,                 thingY }, // Right
		{ thingX - adjustedRadius,                 thingY }, // Left
		{ 				  thingX, thingY + adjustedRadius }, // Up
		{ 				  thingX, thingY - adjustedRadius }  // Down
	};

	// Iterate strictly through the lines enclosing the target sprite's active sector boundaries
	for (auto testLine : thingSector->Lines)
	{
		if (!testLine) continue;
		
		// Skip non-1-sided lines (Void boundaries check)
		if (!testLine->sidedef[0] || testLine->sidedef[1])
			continue; 

		float lineX1 = (float)testLine->v1->fX();
		float lineY1 = (float)testLine->v1->fY();
		float lineX2 = (float)testLine->v2->fX();
		float lineY2 = (float)testLine->v2->fY();

		// Check each of the 4 orthogonal perimeter test points against the viewer line matrix
		for (int i = 0; i < 4; i++)
		{
			float ix, iy;
			if (SpriteIntersectsLinedef(viewerX, viewerY, testPoints[i][0], testPoints[i][1],
				lineX1, lineY1, lineX2, lineY2, ix, iy))
			{
				return true; // Sprite perimeter crosses a 1-sided line visible to viewer
			}
		}

		// Also check the core viewer->sprite center line itself as the primary fallback anchor
		float ix, iy;
		if (SpriteIntersectsLinedef(viewerX, viewerY, thingX, thingY,
			lineX1, lineY1, lineX2, lineY2, ix, iy))
		{
			return true; // Core line of sight intersected by the 1S boundary shirt
		}
	}
	
	return false; // Clear line-of-sight window path mapping for the viewer
}

struct ViewerCrossed1SidedLineCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cachedViewerCrossResult = false; // Default: assume NO 1-sided crossing
};
static TMap<AActor *, ViewerCrossed1SidedLineCacheEntry> ViewerCrossed1sidedLineCache;

bool ViewerCrossed1sidedLinedefCachedWrapper(AActor *thing, AActor *viewer)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// Use caching if enabled matching your standard engine layout
	if (enableAnamorphCache)
	{
		const int                          currentMapTimeTick = level.maptime;
		ViewerCrossed1SidedLineCacheEntry &entry = ViewerCrossed1sidedLineCache[thing];

		// Return cached result if valid (updated within last 8 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cachedViewerCrossResult;
		}

		// Cache miss: compute and cache fresh result from non-cached function
		bool result = ViewerCrossed1sidedLinedef(thing, viewer);
		entry.cachedViewerCrossResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Original uncached behavior fallback pass when cache is toggled off
		return ViewerCrossed1sidedLinedef(thing, viewer);
	}
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// Regular 1side line crossing check, may not cull void sprites
// Culls coplanar leaks the best but does it pretty roughly
bool SpriteCrossed1sidedLinedef(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	// The FOV check is VERY BAD for 1 sided stuff!
	//if (CheckFrustumCullingUNUSED(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// ONLY check sprite's sector, never viewer's sector
	sector_t *thingSector = thing->Sector;
	if (!thingSector || thingSector->Lines.Size() == 0) return false;

	// Calculate viewer->thing line geometry
	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	const float spriteSize = (thing->radius + thing->Height) * 0.5f;

	// Sprite size thresholds
	const bool isMicroSprite = (spriteSize <= 8.0f);
	const bool isTinySprite = (spriteSize <= 12.0f);
	const bool isSmallSprite = (spriteSize <= 18.0f);
	const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 38.0f);
	const bool isLargeSprite = (spriteSize > 38.0f && spriteSize < 39.0f);
	const bool isHugeSprite = (spriteSize >= 39.0f);

	// Scale adjustments
	// Default: micro/tiny/small sprites
	float                        spriteScale = 0.44f;
	if      (isMediumSprite)     spriteScale = 0.5f;
	else if (isLargeSprite)      spriteScale = 0.15f;
	else if (isHugeSprite)       spriteScale = 0.1f;

	// Calculate scaled test points based on radius + size
	float adjustedRadius = thing->radius * spriteScale;

	// Generate 4 orthogonal test points around the sprite center
	float testPoints[4][2] =
	{
		{ thingX + adjustedRadius,                 thingY }, // Right
		{ thingX - adjustedRadius,                 thingY }, // Left
		{ 				  thingX, thingY + adjustedRadius }, // Up
		{ 				  thingX, thingY - adjustedRadius }  // Down
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
			if (SpriteIntersectsLinedef(viewerX, viewerY, testPoints[i][0], testPoints[i][1],
				lineX1, lineY1, lineX2, lineY2, ix, iy))
			{
				return true; // Sprite crosses a 1-sided line visible to viewer
			}
		}

		// Also check the core viewer->sprite line itself
		float ix, iy;
		if (SpriteIntersectsLinedef(viewerX, viewerY, thingX, thingY,
			lineX1, lineY1, lineX2, lineY2, ix, iy))
		{
			return true;
		}
	}
	return false;
}

struct SpriteCrossed1SidedLineCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached1sidedCrossResult = false; // Default: assume NO 1-sided crossing
};
static TMap<AActor *, SpriteCrossed1SidedLineCacheEntry> SpriteCrossed1sidedLineCache;

bool SpriteCrossed1sidedLinedefCachedWrapper(AActor *thing, AActor *viewer)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int             currentMapTimeTick = level.maptime;
		SpriteCrossed1SidedLineCacheEntry &entry = SpriteCrossed1sidedLineCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached1sidedCrossResult;
		}

		// Compute and cache fresh result from non-cached function
		bool result = SpriteCrossed1sidedLinedef(thing, viewer);
		entry.cached1sidedCrossResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Original uncached behavior
		return SpriteCrossed1sidedLinedef(thing, viewer);
	}
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// The old, culling harder and gives more false positives culls, kept for history
// If only one side of a sprite bbox closest to and facing the viewer crossed a 1sided line
//bool SpriteBboxFacingCameraCrossed1sLineOLD(AActor *thing, AActor *viewer)
//{
//	if (!thing || !viewer) return false;
//
//	// The FOV check is VERY BAD for 1 sided stuff!
//	//if (CheckFrustumCullingUNUSED(thing)) return false;
//	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;
//
//	// 1. SPATIAL POOLING
//	static TMap<uint64_t, bool> spatial1sPool;
//	static int                  last1sPoolTick = -1;
//	if (last1sPoolTick != level.maptime)
//	{
//		spatial1sPool.Clear();
//		last1sPoolTick = level.maptime;
//	}
//
//	float thingX = (float)thing->X();
//	float thingY = (float)thing->Y();
//	float viewerX = (float)viewer->X();
//	float viewerY = (float)viewer->Y();
//
//	const float fovInv = 1.0f / 90.0f; // for 90 degrees FOV
//	const float blockmapGridSizInv = 1.0f / 16.0f; // for 16x16 grid size
//
//	// KEY: Using 16-unit grid.
//	// Added coarse angle (90 deg steps) to key so results change when you orbit the sprite
//	int      gridX = (int)floorf(thingX * blockmapGridSizInv);
//	int      gridY = (int)floorf(thingY * blockmapGridSizInv);
//	int      angleKey = (int)(viewer->Angles.Yaw.Degrees * fovInv);     // LZDoom07 way
//	//int      angleKey = (int)(viewer->Angles.Yaw.Degrees() * fovInv); // UZDoom way
//	uint64_t spatialKey = ((uint64_t)gridX << 32) | ((uint32_t)gridY << 8) | (uint8_t)angleKey;
//
//	if (spatial1sPool.CheckKey(spatialKey)) return spatial1sPool[spatialKey];
//
//	// 2. FACING DIRECTION CALCULATION
//	// Vector from sprite to viewer
//	float dx = viewerX - thingX;
//	float dy = viewerY - thingY;
//	float vDist = sqrt(dx * dx + dy * dy);
//	if (vDist > 0.0f)
//	{
//		dx /= vDist; dy /= vDist;
//	}
//
//	// 3. SIZE ADAPTATION
//	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
//	const bool  isTinySprite  = (spriteSize < 18.0f);
//	const bool  isLargeSprite = (spriteSize >= 45.0f);
//
//	float                                                spriteScale = 7.5f;
//	if      (isMicroSprDimExp && isExpSprWorthMoreCull)  spriteScale = 16.0f;
//	else if (isTinySprDimExp  && isExpSprWorthMoreCull)  spriteScale = 13.5f;
//	else if (isSmallSprDimExp && isExpSprWorthMoreCull)  spriteScale = 11.0f;
//	else if (isMedSprDimExp   && isExpSprWorthMoreCull)  spriteScale = 6.5f;
//	else if (isOtherSprDimExp && isExpSprWorthMoreCull)  spriteScale = 5.5f;
//	else if (isTinySprite)                               spriteScale = 3.5f;
//	else if (isLargeSprite)                              spriteScale = 2.5f;
//	else                                                 spriteScale = 3.5f;
//
//	float adjustedRadius = thing->radius * spriteScale;
//
//	float                                                strictZoneScale = 7.5f;
//	if      (isMicroSprDimExp && isExpSprWorthMoreCull)  strictZoneScale = 16.0f;
//	else if (isTinySprDimExp  && isExpSprWorthMoreCull)  strictZoneScale = 13.5f;
//	else if (isSmallSprDimExp && isExpSprWorthMoreCull)  strictZoneScale = 11.0f;
//	else if (isMedSprDimExp   && isExpSprWorthMoreCull)  strictZoneScale = 6.5f;
//	else if (isOtherSprDimExp && isExpSprWorthMoreCull)  strictZoneScale = 5.5f;
//	else if (isTinySprite)                               strictZoneScale = 3.5f;
//	else if (isLargeSprite)                              strictZoneScale = 5.5f;
//	else                                                 strictZoneScale = 4.0f;
//
//	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);
//
//	// 4. SELECTIVE TEST POINTS (Facing Only)
//	// We only test the center and the side most facing the camera
//	float testPts[2][2];
//	testPts[0][0] = thingX; testPts[0][1] = thingY; // Always test center
//
//	// Pick the point on the bbox radius that is closest to the viewer
//	testPts[1][0] = thingX + (dx * adjustedRadius);
//	testPts[1][1] = thingY + (dy * adjustedRadius);
//
//	// 5. LOCAL SCANNING
//	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
//	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
//	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
//	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);
//
//	bool result = false;
//	int  linesCount = 0;
//
//	for (int bx = minBX; bx <= maxBX && !result; bx++)
//	{
//		for (int by = minBY; by <= maxBY && !result; by++)
//		{
//			if (!level.blockmap.isValidBlock(bx, by)) continue;
//			int *list = level.blockmap.GetLines(bx, by);
//			for (int i = 0; list[i] != -1 && linesCount < 64; i++)
//			{
//				line_t *line = &level.lines[list[i]];
//				linesCount++;
//				if (line->backsector != nullptr) continue;
//
//				float l1x = (float)line->v1->fX();
//				float l1y = (float)line->v1->fY();
//				float l2x = (float)line->v2->fX();
//				float l2y = (float)line->v2->fY();
//
//				for (int j = 0; j < 2; j++) // Only 2 points: center and facing edge
//				{
//					float ix, iy;
//					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1], 
//						                                           l1x, l1y, l2x, l2y, ix, iy))
//					{
//						float dx_int = ix - thingX;
//						float dy_int = iy - thingY;
//						if ((dx_int * dx_int + dy_int * dy_int) < strictZoneSq)
//						{
//							result = true;
//							break;
//						}
//					}
//				}
//			}
//		}
//	}
//
//	spatial1sPool[spatialKey] = result;
//	return result;
//}

// THE PROBLEM WITH THE ORIGINAL IMPLEMENTATION :
// The legacy code simply verified line intersections blindly.On raised sectors(like Doom 2 Map01
//	imp podiums), rays tracing from the camera to the sprite center would hit 1 - sided void walls
//	located BEHIND or UNDERNEATH the actor.This caused severe false - positive culling, making
//	monsters completely vanish while standing in wide - open views.
//
//	HOW THIS ENHANCED VECTOR FILTER FIXES IT :
// We calculate a displacement vector from the sprite's absolute center to the exact wall
// intersection point(ix, iy), and then compute its DOT PRODUCT against the view direction vector.
//
// CRITICAL PROGRAMMING LAWS FOR THIS FUNCTION :
//
// 1. You MUST NOT allow culling if the dot product is POSITIVE.A positive dot product means
// the wall intersection occurs in the background, strictly BEHIND the sprite's physical center.
// Background walls MUST NEVER obstruct foreground objects!
//
// 2. You MUST NOT compute raw camera - to - wall vs camera - to - sprite linear scalar distances.
// Due to bounding - box radial inflation(adjustedRadius), the tested edge nodes regularly
// cross behind thin walls when orbiting, which breaks simple distance sorting and causes
// unstable frame - to - frame sprite flickering / blinking.
//
// 3. You MUST check the dot product to isolate geometry orientation.If 'dotProduct <= 0.0f',
// the wall is verified to sit strictly BETWEEN the viewer and the sprite.Only then is it
// mathematically SAFE to flag the node as occluded and proceed with culling.
//
// 4. You MUST implement radius - clamping padding for the outer edge node(j == 1).If a void wall
// merely skims the artificial outer boundaries of the expanded sprite scale but DOES NOT
// penetrate the core physical radius, you MUST bypass culling to maintain flawless rendering
// stability around sharp wall corners.
//  Verifies if a sprite's bbox edges cross a 1-sided void line relative to the viewer
bool SpriteBboxFacingCameraCrossed1sLine(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// --- 1. SPATIAL POOLING AND CACHING ---
	static TMap<uint64_t, bool> spatial1sPool;
	static int                  last1sPoolTick = -1;
	if (last1sPoolTick != level.maptime)
	{
		spatial1sPool.Clear();
		last1sPoolTick = level.maptime;
	}

	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();
	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();

	const float fovInv = 1.0f / 90.0f;             
	const float blockmapGridSizInv = 1.0f / 16.0f; 

	int      gridX = (int)floorf(thingX * blockmapGridSizInv);
	int      gridY = (int)floorf(thingY * blockmapGridSizInv);
	int      angleKey = (int)(viewer->Angles.Yaw.Degrees * fovInv); // LZDoom07 way
	//int      angleKey = (int)(viewer->Angles.Yaw.Degrees() * fovInv); // UZDoom way
	uint64_t spatialKey = ((uint64_t)gridX << 32) | ((uint32_t)gridY << 8) | (uint8_t)angleKey;

	if (spatial1sPool.CheckKey(spatialKey)) return spatial1sPool[spatialKey];

	// --- 2. FACING DIRECTION (VIEW VECTOR) ---
	float dx = viewerX - thingX;
	float dy = viewerY - thingY;
	float vDist = sqrtf((dx * dx) + (dy * dy));
	if (vDist > 0.0f)
	{
		dx /= vDist; dy /= vDist;
	}

	// --- 3. ADAPTIVE BOUNDING BOX SCALING ---
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool  isTinySprite  = (spriteSize < 18.0f);
	const bool  isLargeSprite = (spriteSize >= 45.0f);

	float spriteScale = 7.5f;
	if      (isMicroSprDimExp && isExpSprWorthMoreCull)  spriteScale = 16.0f;
	else if (isTinySprDimExp  && isExpSprWorthMoreCull)  spriteScale = 13.5f;
	else if (isSmallSprDimExp && isExpSprWorthMoreCull)  spriteScale = 11.0f;
	else if (isMedSprDimExp   && isExpSprWorthMoreCull)  spriteScale = 6.5f;
	else if (isOtherSprDimExp && isExpSprWorthMoreCull)  spriteScale = 5.5f;
	else if (isTinySprite)                               spriteScale = 3.5f;
	else if (isLargeSprite)                              spriteScale = 2.5f;
	else                                                 spriteScale = 3.5f;

	float adjustedRadius = thing->radius * spriteScale;

	float strictZoneScale = 7.5f;
	if      (isMicroSprDimExp && isExpSprWorthMoreCull)  strictZoneScale = 16.0f;
	else if (isTinySprDimExp  && isExpSprWorthMoreCull)  strictZoneScale = 13.5f;
	else if (isSmallSprDimExp && isExpSprWorthMoreCull)  strictZoneScale = 11.0f;
	else if (isMedSprDimExp   && isExpSprWorthMoreCull)  strictZoneScale = 6.5f;
	else if (isOtherSprDimExp && isExpSprWorthMoreCull)  strictZoneScale = 5.5f;
	else if (isTinySprite)                               strictZoneScale = 3.5f;
	else if (isLargeSprite)                              strictZoneScale = 5.5f;
	else                                                 strictZoneScale = 4.0f;

	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);

	// --- 4. EXPLICIT TEST NODES ---
	// testPts[0] is the center, testPts[1] is the edge pushed towards the camera
	float testPts[2][2];
	testPts[0][0] = thingX; testPts[0][1] = thingY; 
	testPts[1][0] = thingX + (dx * adjustedRadius); 
	testPts[1][1] = thingY + (dy * adjustedRadius);

	// --- 5. BLOCKMAP INTERSECTION SCANNING ---
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);

	bool result = false;
	int  linesCount = 0;

	for (int bx = minBX; bx <= maxBX && !result; bx++)
	{
		for (int by = minBY; by <= maxBY && !result; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;
			int *list = level.blockmap.GetLines(bx, by);
			
			for (int i = 0; list[i] != -1 && linesCount < 64; i++)
			{
				line_t *line = &level.lines[list[i]];
				linesCount++;
				if (line->backsector != nullptr) continue;

				float l1x = (float)line->v1->fX();
				float l1y = (float)line->v1->fY();
				float l2x = (float)line->v2->fX();
				float l2y = (float)line->v2->fY();

				for (int j = 0; j < 2; j++) 
				{
					float ix, iy;
					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1], 
						                                           l1x, l1y, l2x, l2y, ix, iy))
					{
						// Vector from sprite center to the intersection point
						float s_to_int_dx = ix - thingX;
						float s_to_int_dy = iy - thingY;

						// DOT PRODUCT FILTER: Project the intersection vector onto the view vector (dx, dy).
						// If the dot product is positive, the wall intersection happens BEHIND the sprite 
						// relative to the camera direction. We must NEVER cull based on background walls.
						float dotProduct = (s_to_int_dx * dx) + (s_to_int_dy * dy);

						if (dotProduct <= 0.0f) // The wall is strictly between the camera and the sprite
						{
							float distToIntersectionSq = (s_to_int_dx * s_to_int_dx) + (s_to_int_dy * s_to_int_dy);
							
							if (distToIntersectionSq < strictZoneSq)
							{
								// Buffer padding: If we are testing the facing edge node (j == 1), 
								// ensure the wall didn't just clip the outer edge of the expanded radius.
								if (j == 1 && distToIntersectionSq < (thing->radius * thing->radius))
								{
									continue; 
								}

								result = true;
								break;
							}
						}
					}
				}
			}
		}
	}

	spatial1sPool[spatialKey] = result;
	return result;
}

struct SpriteBboxFacingCameraCrossed1sLineCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached1sCrossBboxFacingResult = false; // Default: assume NO 1-sided crossing
};
static TMap<AActor *, SpriteBboxFacingCameraCrossed1sLineCacheEntry> SpriteBboxFacingCrossed1sCache;

bool SpriteBboxFacingCameraCrossed1sLineCachedWrapper(AActor *thing, AActor *viewer)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int                         currentMapTimeTick = level.maptime;
		SpriteBboxFacingCameraCrossed1sLineCacheEntry &entry = SpriteBboxFacingCrossed1sCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached1sCrossBboxFacingResult;
		}

		// Compute and cache fresh result from non-cached function
		bool result = SpriteBboxFacingCameraCrossed1sLine(thing, viewer);
		entry.cached1sCrossBboxFacingResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Original uncached behavior
		return SpriteBboxFacingCameraCrossed1sLine(thing, viewer);
	}
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// this one culls sprites that are located in the void
bool SpriteCrossed1sidedVoidLinedef(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	// The FOV check is VERY BAD for 1 sided stuff!
	//if (CheckFrustumCullingUNUSED(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const bool enablebboxface = false;

	// 1. SPATIAL POOLING SETUP
	// Cache for 1-sided line intersections within a 64x64 unit grid
	static TMap<uint64_t, bool> spatial1sPool;
	static int                  last1sPoolTick = -1;

	// Reset pool once per frame
	if (last1sPoolTick != level.maptime)
	{
		spatial1sPool.Clear();
		last1sPoolTick = level.maptime;
	}

	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	const float fovInv = 1.0f / 90.0f;             // for 90 degrees FOV
	const float blockmapGridSizInv = 1.0f / 64.0f; // for 64x64 grid size

	// Calculate grid coordinates for the 64-unit spatial key
	int      gridX = (int)floorf(thingX * blockmapGridSizInv);
	int      gridY = (int)floorf(thingY * blockmapGridSizInv);
	uint64_t spatialKey = ((uint64_t)gridX << 32) | (uint32_t)gridY;

	// 2. SPATIAL CACHE CHECK
	if (spatial1sPool.CheckKey(spatialKey))
	{
		return spatial1sPool[spatialKey];
	}

	// 3. SIZE ADAPTATION AND PARTICLE PROTECTION
	float       viewerX = (float)viewer->X();
	float       viewerY = (float)viewer->Y();
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;

	const bool isMicroSprite = (spriteSize <= 12.0f); // Combine micro/tiny
	const bool isSmallSprite = (spriteSize > 12.0f && spriteSize <= 38.0f);

	float                                                spriteScale = 4.7f;
	if      (isMicroSprDimExp && isExpSprWorthMoreCull)  spriteScale = 16.0f;
	else if (isTinySprDimExp  && isExpSprWorthMoreCull)  spriteScale = 13.5f;
	else if (isSmallSprDimExp && isExpSprWorthMoreCull)  spriteScale = 11.0f;
	else if (isMedSprDimExp   && isExpSprWorthMoreCull)  spriteScale = 6.5f;
	else if (isOtherSprDimExp && isExpSprWorthMoreCull)  spriteScale = 5.5f;
	else if (isMicroSprite)                              spriteScale = 3.5f;
	else if (isSmallSprite)                              spriteScale = 3.2f;

	float effectiveRadius = thing->radius;
	if (effectiveRadius < 1.0f) effectiveRadius = 4.0f;
	float adjustedRadius = effectiveRadius * spriteScale;


	// --- 3.5 FACING AND BBOX CONFIGURATION ---
	float testPts[5][2];
	int   numPoints = 0;

	if (enablebboxface)
	{
		// MODE A: FACING DIRECTION CALCULATION
		float dx = viewerX - thingX;
		float dy = viewerY - thingY;
		float vDist = sqrt(dx * dx + dy * dy);
		if (vDist > 0.0f)
		{
			dx /= vDist; dy /= vDist;
		}

		// Center always tested
		testPts[0][0] = thingX;  testPts[0][1] = thingY;

		// Pick the point on the bbox radius that is closest to the viewer
		testPts[1][0] = thingX + (dx * adjustedRadius);
		testPts[1][1] = thingY + (dy * adjustedRadius);
		numPoints = 2;
	}
	else
	{
		// MODE B: ORIGINAL FULL STAR MATRIX (CENTER + 4 SIDES)
		testPts[0][0] = thingX;                  testPts[0][1] = thingY;
		testPts[1][0] = thingX + adjustedRadius; testPts[1][1] = thingY;
		testPts[2][0] = thingX - adjustedRadius; testPts[2][1] = thingY;
		testPts[3][0] = thingX;                  testPts[3][1] = thingY + adjustedRadius;
		testPts[4][0] = thingX;                  testPts[4][1] = thingY - adjustedRadius;
		numPoints = 5;
	}

	// Track occlusion hits for each specific point independently
	bool pointIsObstructed[5] = { false, false, false, false, false };

	// 4. LOCAL BLOCKMAP SCANNING
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);

	for (int bx = minBX; bx <= maxBX; bx++)
	{
		for (int by = minBY; by <= maxBY; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;

			int *list = level.blockmap.GetLines(bx, by);
			for (int i = 0; list[i] != -1; i++)
			{
				line_t *line = &level.lines[list[i]];

				// Only 1-sided walls
				if (line->backsector != nullptr) continue;

				float l1x = (float)line->v1->fX();
				float l1y = (float)line->v1->fY();
				float l2x = (float)line->v2->fX();
				float l2y = (float)line->v2->fY();

				for (int j = 0; j < numPoints; j++)
				{
					// Skip calculation if this specific node point is already proven blocked
					if (pointIsObstructed[j]) continue;

					float ix, iy;
					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1],
						                                           l1x, l1y, l2x, l2y, ix, iy))
					{
						pointIsObstructed[j] = true;
					}
				}
			}
		}
	}

	// --- 5. MITIGATION EVALUATION (LAX VISIBILITY CORRECTION) ---
	// If at least ONE single point managed to find a completely clear line of sight
	// to the viewer (i.e., not obstructed or intersected by any 1-sided void wall),
	// make the entire sprite VISIBLE by completely overriding the cull result back to false!
	bool result = true; // Assume fully culled by default

	for (int j = 0; j < numPoints; j++)
	{
		if (!pointIsObstructed[j])
		{
			// "Rescue ray" found! At least one side is wide open, keep sprite visible!
			result = false;
			break;
		}
	}

	// 6. STORE AND RETURN
	spatial1sPool[spatialKey] = result;
	return result;
}

// Cache structure for Void 1-sided line crossing checks
struct SpriteCrossed1sidedVoidCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached1sVoidCrossResult = false;
};
static TMap<AActor *, SpriteCrossed1sidedVoidCacheEntry> SpriteCrossed1sidedVoidCache;

bool SpriteCrossed1sidedVoidLinedefCachedWrapper(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (enableAnamorphCache)
	{
		const int             currentMapTimeTick = level.maptime;
		SpriteCrossed1sidedVoidCacheEntry &entry = SpriteCrossed1sidedVoidCache[thing];

		// Return cached result if valid each 7 ticks
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached1sVoidCrossResult;
		}

		// Perform the heavy blockmap/intersection scan
		bool result = SpriteCrossed1sidedVoidLinedef(thing, viewer);

		// Update cache entry
		entry.cached1sVoidCrossResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Cache disabled: direct calculation
		return SpriteCrossed1sidedVoidLinedef(thing, viewer);
	}
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// this one culls sprites that are located in the void
bool SpriteCrossed1sBboxVoidLinedef(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	// The FOV check is VERY BAD for 1 sided stuff!
	//if (CheckFrustumCullingUNUSED(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const bool enablebboxface = true; // check bbox facing here

	// 1. SPATIAL POOLING SETUP
	// Cache for 1-sided line intersections within a 64x64 unit grid
	static TMap<uint64_t, bool> spatial1sVoidBboxPool;
	static int                  last1sPoolTick = -1;

	// Reset pool once per frame
	if (last1sPoolTick != level.maptime)
	{
		spatial1sVoidBboxPool.Clear();
		last1sPoolTick = level.maptime;
	}

	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	const float fovInv = 1.0f / 90.0f;             // for 90 degrees FOV
	const float blockmapGridSizInv = 1.0f / 64.0f; // for 64x64 grid size

	// Calculate grid coordinates for the 64-unit spatial key
	int      gridX = (int)floorf(thingX * blockmapGridSizInv);
	int      gridY = (int)floorf(thingY * blockmapGridSizInv);
	uint64_t spatialKey = ((uint64_t)gridX << 32) | (uint32_t)gridY;

	// 2. SPATIAL CACHE CHECK
	if (spatial1sVoidBboxPool.CheckKey(spatialKey))
	{
		return spatial1sVoidBboxPool[spatialKey];
	}

	// 3. SIZE ADAPTATION AND PARTICLE PROTECTION
	float spriteSize = (thing->radius + thing->Height) * 0.5f;
	if (spriteSize < 1.0f) spriteSize = 8.0f;

	float       viewerX = (float)viewer->X();
	float       viewerY = (float)viewer->Y();
	
	const bool isMicroSprite = (spriteSize <= 12.0f); // Combine micro/tiny
	const bool isSmallSprite = (spriteSize > 12.0f && spriteSize <= 38.0f);

	// Here's a thing about THIS PARTICULAR FUNCTION:
	// The smaller the "spriteScale" the MORE it CULLS
	float                                                spriteScale = 3.7f;
	if      (isMicroSprDimExp && isExpSprWorthMoreCull)  spriteScale = 0.5f;
	else if (isTinySprDimExp  && isExpSprWorthMoreCull)  spriteScale = 0.75f;
	else if (isSmallSprDimExp && isExpSprWorthMoreCull)  spriteScale = 1.0f;
	else if (isMedSprDimExp   && isExpSprWorthMoreCull)  spriteScale = 1.2f;
	else if (isOtherSprDimExp && isExpSprWorthMoreCull)  spriteScale = 0.7f;
	else if (isMicroSprite)                              spriteScale = 3.5f;
	else if (isSmallSprite)                              spriteScale = 2.2f;

	float effectiveRadius = thing->radius;
	if (effectiveRadius < 1.0f) effectiveRadius = 4.0f;
	float adjustedRadius = effectiveRadius * spriteScale;

	// --- 3.5 FACING AND BBOX CONFIGURATION ---
	float testPts[5][2];
	int   numPoints = 0;

	if (enablebboxface)
	{
		// MODE A: FACING DIRECTION CALCULATION
		float dx = viewerX - thingX;
		float dy = viewerY - thingY;
		float vDist = sqrt(dx * dx + dy * dy);
		if (vDist > 0.0f)
		{
			dx /= vDist; dy /= vDist;
		}

		// Center always tested
		testPts[0][0] = thingX;  testPts[0][1] = thingY;

		// Pick the point on the bbox radius that is closest to the viewer
		testPts[1][0] = thingX + (dx * adjustedRadius);
		testPts[1][1] = thingY + (dy * adjustedRadius);
		numPoints = 2;
	}
	else
	{
		// MODE B: ORIGINAL FULL STAR MATRIX (CENTER + 4 SIDES)
		testPts[0][0] = thingX;                  testPts[0][1] = thingY;
		testPts[1][0] = thingX + adjustedRadius; testPts[1][1] = thingY;
		testPts[2][0] = thingX - adjustedRadius; testPts[2][1] = thingY;
		testPts[3][0] = thingX;                  testPts[3][1] = thingY + adjustedRadius;
		testPts[4][0] = thingX;                  testPts[4][1] = thingY - adjustedRadius;
		numPoints = 5;
	}

	// Track occlusion hits for each specific point independently
	bool pointIsObstructed[5] = { false, false, false, false, false };

	// 4. LOCAL BLOCKMAP SCANNING
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);

	for (int bx = minBX; bx <= maxBX; bx++)
	{
		for (int by = minBY; by <= maxBY; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;

			int *list = level.blockmap.GetLines(bx, by);
			for (int i = 0; list[i] != -1; i++)
			{
				line_t *line = &level.lines[list[i]];

				// Only 1-sided walls
				if (line->backsector != nullptr) continue;

				float l1x = (float)line->v1->fX();
				float l1y = (float)line->v1->fY();
				float l2x = (float)line->v2->fX();
				float l2y = (float)line->v2->fY();

				for (int j = 0; j < numPoints; j++)
				{
					// Skip calculation if this specific node point is already proven blocked
					if (pointIsObstructed[j]) continue;

					float ix, iy;
					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1],
						l1x, l1y, l2x, l2y, ix, iy))
					{
						pointIsObstructed[j] = true;
					}
				}
			}
		}
	}

	// --- 5. MITIGATION EVALUATION (LAX VISIBILITY CORRECTION) ---
	// If at least ONE single point managed to find a completely clear line of sight
	// to the viewer (i.e., not obstructed or intersected by any 1-sided void wall),
	// make the entire sprite VISIBLE by completely overriding the cull result back to false!
	bool result = true; // Assume fully culled by default

	for (int j = 0; j < numPoints; j++)
	{
		if (!pointIsObstructed[j])
		{
			// "Rescue ray" found! At least one side is wide open, keep sprite visible!
			result = false;
			break;
		}
	}

	// 6. STORE AND RETURN
	spatial1sVoidBboxPool[spatialKey] = result;
	return result;
}

// Cache structure for Void 1-sided line crossing checks with bbox facing
struct SpriteCrossed1sVoidBboxCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached1sVoidCrossBboxFaceResult = false;
};
static TMap<AActor *, SpriteCrossed1sVoidBboxCacheEntry> SpriteCrossed1sVoidBboxCache;

bool SpriteCrossed1sidedVoidBboxFaceCachedWrapper(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (enableAnamorphCache)
	{
		const int             currentMapTimeTick = level.maptime;
		SpriteCrossed1sVoidBboxCacheEntry &entry = SpriteCrossed1sVoidBboxCache[thing];

		// Return cached result if valid each 7 ticks
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached1sVoidCrossBboxFaceResult;
		}

		// Perform the heavy blockmap/intersection scan
		bool result = SpriteCrossed1sBboxVoidLinedef(thing, viewer);

		// Update cache entry
		entry.cached1sVoidCrossBboxFaceResult = result;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return result;
	}
	else
	{
		// Cache disabled: direct calculation
		return SpriteCrossed1sBboxVoidLinedef(thing, viewer);
	}
}

// Here comes the asset for regular 1sided wall obstructions check
static bool CheckLineOfSight1sided(AActor *viewer, const DVector3 &thingpos, float spriteRadius)
{
	DVector3  viewerPos = viewer->Pos();
	sector_t *viewSector = viewer->Sector;

	sector_t* thingSector = P_PointInSector(thingpos.X, thingpos.Y); // LZDoom07 way
	//sector_t *thingSector = level.PointInSector(thingpos);         // UZDoom way

	// Early exit for same position
	if (viewerPos.XY() == thingpos.XY()) return true;

	// Direction vector (non-normalized)
	float dx = thingpos.X - viewerPos.X;
	float dy = thingpos.Y - viewerPos.Y;

	// Perpendicular vector
	float px = -dy; float py = dx;

	// Precompute test points (4 orthogonal directions)
	DVector2 testPoints[4];
	float    scale = spriteRadius / sqrt(dx * dx + dy * dy + 1e-8f);

	testPoints[0] = { thingpos.X + px * scale, thingpos.Y + py * scale };
	testPoints[1] = { thingpos.X - px * scale, thingpos.Y - py * scale };
	testPoints[2] = { thingpos.X + dx * scale, thingpos.Y + dy * scale };
	testPoints[3] = { thingpos.X - dx * scale, thingpos.Y - dy * scale };

	// Check both sectors
	for (auto *sector : { viewSector, thingSector })
	{
		for (auto &line : sector->Lines)
		{
			// Only check 1-sided lines
			if (!line->sidedef[0] || line->sidedef[1]) continue;

			LineSegmentCommon wall(line->v1->fX(), line->v1->fY(), line->v2->fX(), line->v2->fY());

			for (const auto &pt : testPoints)
			{
				LineSegmentCommon sight(viewerPos.X, viewerPos.Y, pt.X, pt.Y);

				if (wall.IntersectsCommon(sight)) return false; // Blocked by 1-sided line
			}
		}
	}

	return true; // No blocking 1-sided lines found
}

// Cache structure for 1-sided checks
struct Visibility1sidedCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached1sidedResult = true;
};
// Global cache storage for 1-sided checks
static TMap<AActor *, Visibility1sidedCacheEntry> Visibility1sidedCache;

bool IsSpriteVisibleBehind1sidedLinesCachedWrapper(AActor *thing, AActor *viewer, const DVector3 &thingpos)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// The FOV check is VERY BAD for 1 sided stuff!
	//if (CheckFrustumCullingUNUSED(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	float spriteScale = 0.15f;

	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int      currentMapTimeTick = level.maptime;
		Visibility1sidedCacheEntry &entry = Visibility1sidedCache[thing];

		// Return cached result if valid
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
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
}

// ******* 1-sided-linedef culling block finish *******
//         ---===      ***************************************        ===---
//                                ----------------







//                                ----------------
//         ---===      ***************************************        ===---
// ******* 2-sided-linedef tall enough sector obstructions culling block start *******

constexpr float MINCOORD2SIDED = -32768.0;
constexpr float MAXCOORD2SIDED = 32767.9999847;
constexpr float EPSILON2SIDED = 0.001;
float           Ztolerance2sided = 2.0f;
float           Ztolerance2sidedBot = 4.0f; // lower vals - legs on elevations cull more if seen from below

// Optimized position getters that avoid implicit conversions - again
static FVector2 GetVertexPosition(const vertex_t *vert)
{
	return FVector2(static_cast<float>(vert->fX()), static_cast<float>(vert->fY()));
}
static FVector2 GetActorPosition(const AActor *actor)
{
	return FVector2(static_cast<float>(actor->X()), static_cast<float>(actor->Y()));
}
static FVector3 GetActorPosition3D(const AActor *actor)
{
	return FVector3(static_cast<float>(actor->X()), static_cast<float>(actor->Y()), static_cast<float>(actor->Z()));
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// This one is nice for taming "increaseAnam" leaks
// This one culls sprites if they crossed 2sided lines and nothing more
bool SpriteCrossed2sLineSimple(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// Only check thing's sector - no other changes
	sector_t *currentSector = thing->Sector;
	if (!currentSector || currentSector->Lines.Size() == 0) return false;

	float vpx = (float)viewer->X();
	float vpy = (float)viewer->Y();
	float tpx = (float)thing->X();
	float tpy = (float)thing->Y();

	for (unsigned i = 0; i < currentSector->Lines.Size(); i++)
	{
		line_t *testLine = currentSector->Lines[i];

		// only check 2-sided linedefs (has both front and back sides)
		if (testLine->sidedef[0] && testLine->sidedef[1])
		{
			float ix, iy;

			// it's slower to call cached version from here, thus unused. If possible, call
			// "SpriteCrossed2sidedLinedefCachedWrapper" as it's going to be faster
			// if (spriteIntersectsLineCachedWrapper(testLine, vpx, vpy, tpx, tpy, testLine->v1->fX(),
			// testLine->v1->fY(), testLine->v2->fX(), testLine->v2->fY(), ix, iy))
			if (SpriteIntersectsLinedef(vpx, vpy, tpx, tpy, testLine->v1->fX(), testLine->v1->fY(), testLine->v2->fX(),
				testLine->v2->fY(), ix, iy))
			{
				return true;
			}
		}
	}
	return false;
}

struct SpriteCrossed2SLineSimpleCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached2sidedSpriteXSimpleResult = false;
};
static TMap<AActor *, SpriteCrossed2SLineSimpleCacheEntry> SpriteCrossed2sLineSimpleCache;

bool SpriteCrossed2sidedLineSimpleCachedWrapper(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (enableAnamorphCache) 	// Use caching if enabled
	{
		const int             currentMapTimeTick = level.maptime;
		SpriteCrossed2SLineSimpleCacheEntry &entry = SpriteCrossed2sLineSimpleCache[thing];

		// Return cached result if valid
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached2sidedSpriteXSimpleResult;
		}

		// Compute and cache fresh result from non-cached function
		bool resultSprX2sLine = SpriteCrossed2sLineSimple(thing, viewer);
		entry.cached2sidedSpriteXSimpleResult = resultSprX2sLine;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return resultSprX2sLine;
	}
	else
	{
		// Original uncached behavior
		return SpriteCrossed2sLineSimple(thing, viewer);
	}
}

// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// This one is nice for taming "increaseAnam" leaks
// This one culls sprites if they crossed 2sided lines
// but if a side of the sprite bounding box facing the viewer - uncull

// More strict original version that chops everything around:
//bool SpriteBboxFacingCameraCrossed2sLineOLD(AActor *thing, AActor *viewer)
//{
//	if (!thing || !viewer) return false;
//
//	//if (CheckFrustumCullingUNUSED(thing)) return false;
//	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;
//
//	const bool checkBboxCameraFace = true;
//
//	sector_t *thingSector = thing->Sector;
//	if (!thingSector || thingSector->Lines.Size() == 0) return false;
//
//	float viewerX = (float)viewer->X();
//	float viewerY = (float)viewer->Y();
//	float thingX = (float)thing->X();
//	float thingY = (float)thing->Y();
//
//	// 1. UNIVERSAL SIZE ADAPTATION
//	float spriteSize = (thing->radius + thing->Height) * 0.5f;
//	if (spriteSize <= 12.0f) spriteSize = 12.0f;
//	const bool  isMicroSprite = (spriteSize < 12.0f);
//	const bool  isTinySprite = (spriteSize >= 12.0f && spriteSize < 18.0f);
//	const bool  isSmallSprite = (spriteSize >= 18.0f && spriteSize < 38.0f);
//	const bool  isMediumSprite = (spriteSize >= 38.0f && spriteSize < 45.0f);
//	const bool  isLargeSprite = (spriteSize >= 45.0f && spriteSize < 60.0f);
//	const bool  isHugeSprite = (spriteSize >= 60.0f);
//
//	// Scale for test point offsets (how far we look around the center)
//	float                      spriteScale = 10.5f;     // isMircosprite and other unmentioned
//	if      (isTinySprite)   { spriteScale = 5.5f; }
//	else if (isSmallSprite)  { spriteScale = 2.5f; }
//	else if (isMediumSprite) { spriteScale = 1.2f; }
//	else if (isLargeSprite)  { spriteScale = 0.3f; }
//	else if (isHugeSprite)   { spriteScale = 0.15f; }
//
//	float adjustedRadius = thing->radius * spriteScale;
//
//	// Scale for the "Kill Zone" (how close the portal must be to block anamorphosis)
//	float                      strictZoneScale = 10.5f; // isMircosprite and other unmentioned
//	if      (isTinySprite)   { strictZoneScale = 8.5f; }
//	else if (isSmallSprite)  { strictZoneScale = 5.0f; }
//	else if (isMediumSprite) { strictZoneScale = 3.2f; }
//	else if (isLargeSprite)  { strictZoneScale = 2.5f; }
//	else if (isHugeSprite)   { strictZoneScale = 1.5f; }
//	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);
//
//	// 2. SETUP TEST POINTS (Both modes use the same adjustedRadius)
//	float testPts[5][2];
//	int   numPoints = 0;
//
//	if (checkBboxCameraFace)
//	{
//		// MODE A: FACING SIDE + CENTER
//		float dx = viewerX - thingX;
//		float dy = viewerY - thingY;
//		float dist = sqrt(dx * dx + dy * dy);
//		if (dist > 0.0f) { dx /= dist; dy /= dist; }
//
//		// Center
//		testPts[0][0] = thingX; testPts[0][1] = thingY;
//
//		// Determine the most facing point using adjustedRadius
//		if (fabs(dx) > fabs(dy))
//		{
//			testPts[1][0] = (dx > 0) ? thingX + adjustedRadius : thingX - adjustedRadius;
//			testPts[1][1] = thingY;
//		}
//		else
//		{
//			testPts[1][0] = thingX;
//			testPts[1][1] = (dy > 0) ? thingY + adjustedRadius : thingY - adjustedRadius;
//		}
//		numPoints = 2;
//	}
//	else
//	{
//		// MODE B: FULL STAR (CENTER + 4 SIDES)
//		testPts[0][0] = thingX;                  testPts[0][1] = thingY;
//		testPts[1][0] = thingX + adjustedRadius; testPts[1][1] = thingY;
//		testPts[2][0] = thingX - adjustedRadius; testPts[2][1] = thingY;
//		testPts[3][0] = thingX;                  testPts[3][1] = thingY + adjustedRadius;
//		testPts[4][0] = thingX;                  testPts[4][1] = thingY - adjustedRadius;
//		numPoints = 5;
//	}
//
//	// 3. PORTAL SCANNING (FBlockmap + Level version)
//	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0);
//	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0);
//	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0);
//	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0);
//
//	for (int bx = minBX; bx <= maxBX; bx++)
//	{
//		for (int by = minBY; by <= maxBY; by++)
//		{
//			if (!level.blockmap.isValidBlock(bx, by)) continue;
//
//			// Get lines indexes in this block
//			int *list = level.blockmap.GetLines(bx, by);
//
//			// Iterate list till "-1" is met
//			for (int i = 0; list[i] != -1; i++)
//			{
//				line_t *testLine = &level.lines[list[i]];
//
//				if (!(testLine->flags & ML_TWOSIDED)) continue;
//
//				float l1x = (float)testLine->v1->fX();
//				float l1y = (float)testLine->v1->fY();
//				float l2x = (float)testLine->v2->fX();
//				float l2y = (float)testLine->v2->fY();
//
//				for (int j = 0; j < numPoints; j++)
//				{
//					float ix, iy;
//					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1], l1x, l1y, l2x, l2y, ix, iy))
//					{
//						float dx_int = ix - thingX;
//						float dy_int = iy - thingY;
//
//						// If portal is intersected inside a sprite "Kill Zone"
//						if ((dx_int * dx_int + dy_int * dy_int) < strictZoneSq)
//						{
//							return true; // Intersection found, return TRUE to reset increaseAnam
//						}
//					}
//				}
//			}
//		}
//	}
//	return false;
//}

//	--------------------------------------------------------------------------------------------
//	METHOD 1 : SpriteBboxFacingCameraCrossed2sLine(The Directional Sight - Line Intersect Filter)
//	--------------------------------------------------------------------------------------------
//* APPROACH : Narrow - phase view - aligned ray casting(Razor - Sharp Sniper).
//	* LOGIC :
//	Completely omits the actor's center mass. It casts a single, precise vector ray targeting 
//	EXCLUSIVELY the closest bounding box edge node directly facing the player's screen-space matrix.
//* PRIMARY RESPONSIBILITY :
//    Safeguards sub - pixel corner junctions, door trim edges, and close - quarters window borders
//    (Map02 tight wooden corners) where expanded sprite limbs try to crawl through solid wood / metal.
//* THE THREE - WAY TOPOGRAPHY RESOLVER :
//	To prevent the tight ray from false - clamping on long - range open terrain, it runs an instant
//	low - level sector height evaluation(ZatPoint) right at the sub - pixel intersection spot :
//
//FLAT SEAM FILTER :
//  If floorDelta <= 4.0f && ceilingDelta <= 4.0f, the line is a flat layout seam on a high
//  ledge(Map26).It skips processing, keeping anamorphosis fully active.
//
//CLIFF DROP - OFF PASS :
//  If adjacentSectorFloor < currentSectorFloor, the height decreases towards the player.
//	The engine recognizes this is a wide - open cliff drop or window trench(Map02).
//	It completely disables culling, returning FALSE to keep the item visible.
//
//HARD LEDGE LOCK :
//  If the floor steps UP(adjacent > current) and changes massively, it is a real vertical
//  barrier(Map06 8 - unit border).It immediately returns TRUE to clamp the projection leak.
//
// Method 1 acts as the smart
// perspective filter that understands the difference between a solid door and an open window. 
//====================================================================================================

// This function checks the strictly closest facing bounding box node of the sprite.
// It applies a high-precision vertical sector delta filter to isolate real blocking walls 
// from flat room ledges (Map26) and deep drop-off trenches (Map02).
bool SpriteBboxFacingCameraCrossed2sLine(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const bool checkBboxCameraFace = true;

	sector_t *thingSector = thing->Sector;
	if (!thingSector || thingSector->Lines.Size() == 0) return false;

	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	// 1. UNIVERSAL SIZE ADAPTATION
	float spriteSize = (thing->radius + thing->Height) * 0.5f;
	if (spriteSize <= 12.0f) spriteSize = 12.0f;
	const bool  isMicroSprite = (spriteSize < 12.0f);
	const bool  isTinySprite = (spriteSize >= 12.0f && spriteSize < 18.0f);
	const bool  isSmallSprite = (spriteSize >= 18.0f && spriteSize < 38.0f);
	const bool  isMediumSprite = (spriteSize >= 38.0f && spriteSize < 45.0f);
	const bool  isLargeSprite = (spriteSize >= 45.0f && spriteSize < 60.0f);
	const bool  isHugeSprite = (spriteSize >= 60.0f);

	// Scale for test point offsets (how far we look around the center)
	// The RULE: the lesser the spriteScale - the MORE it culls
	float                      spriteScale = 7.1f; // isMircosprite and other unmentioned
	if      (isTinySprite)     spriteScale = 4.1f;
	else if (isSmallSprite)    spriteScale = 1.2f;
	else if (isMediumSprite)   spriteScale = 0.5f;
	else if (isLargeSprite)    spriteScale = 0.3f;
	else if (isHugeSprite)     spriteScale = 0.07f;

	float adjustedRadius = thing->radius * spriteScale;

	// Scale for the "Kill Zone" (how close the portal must be to block anamorphosis)
	// The Rule: the bigger the strictZoneScale - the MORE it culls
	float                      strictZoneScale = 12.5f; // isMircosprite and other unmentioned
	if      (isTinySprite)     strictZoneScale = 10.5f;
	else if (isSmallSprite)    strictZoneScale = 8.0f;
	else if (isMediumSprite)   strictZoneScale = 7.2f;
	else if (isLargeSprite)    strictZoneScale = 6.5f;
	else if (isHugeSprite)     strictZoneScale = 4.5f;
	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);

	// 2. SETUP TEST POINTS (Strict Single-Node Facing Logic)
	float testPts[1][2]; // Single, high-precision facing edge node
	int   numPoints = 1;

	if (checkBboxCameraFace)
	{
		float dx = viewerX - thingX;
		float dy = viewerY - thingY;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist > 0.0f) { dx /= dist; dy /= dist; }

		// Compute strictly the closest face edge that directly targets the camera view vector
		if (fabs(dx) > fabs(dy))
		{
			testPts[0][0] = (dx > 0) ? thingX + adjustedRadius : thingX - adjustedRadius;
			testPts[0][1] = thingY;
		}
		else
		{
			testPts[0][0] = thingX;
			testPts[0][1] = (dy > 0) ? thingY + adjustedRadius : thingY - adjustedRadius;
		}
	}
	else
	{
		testPts[0][0] = thingX; testPts[0][1] = thingY;
	}

	// 3. PORTAL SCANNING (FBlockmap + Level version)
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);

	for (int bx = minBX; bx <= maxBX; bx++)
	{
		for (int by = minBY; by <= maxBY; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;

			int *list = level.blockmap.GetLines(bx, by);
			for (int i = 0; list[i] != -1; i++)
			{
				line_t *testLine = &level.lines[list[i]];

				if (!(testLine->flags & ML_TWOSIDED)) continue;
				if (!testLine->frontsector || !testLine->backsector) continue;

				float l1x = (float)testLine->v1->fX();
				float l1y = (float)testLine->v1->fY();
				float l2x = (float)testLine->v2->fX();
				float l2y = (float)testLine->v2->fY();

				for (int j = 0; j < numPoints; j++)
				{
					float ix, iy;
					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1], l1x, l1y, l2x, l2y, ix, iy))
					{
						float dx_int = ix - thingX;
						float dy_int = iy - thingY;

						if ((dx_int * dx_int + dy_int * dy_int) < strictZoneSq)
						{
							// ============================================================================================
							// HARDCORE GEOMETRY SPLITTER] - Absolute Map01 vs Map06 Resolver
							// Completely bypasses floating-point sub-pixel erosion on sector seams.
							// ============================================================================================
							float fFloor = (float)testLine->frontsector->floorplane.ZatPoint(ix, iy);
							float bFloor = (float)testLine->backsector->floorplane.ZatPoint(ix, iy);
							float fCeil = (float)testLine->frontsector->ceilingplane.ZatPoint(ix, iy);
							float bCeil = (float)testLine->backsector->ceilingplane.ZatPoint(ix, iy);

							float floorDelta = fabsf(fFloor - bFloor);
							float ceilingDelta = fabsf(fCeil - bCeil);

							float highestFloor = MAX(fFloor, bFloor);
							float lowestCeil = MIN(fCeil, bCeil);

							// Fetch base structural ground levels
							float playerFloorZ = (float)viewer->Z();
							float actorFloorZ = (float)thingSector->floorplane.ZatPoint(ix, iy);

							// --- SPLITTER PATH 1: PURE FLAT CORRIDOR (Map01 Hallway Protection) ---
							// If the line has ABSOLUTELY ZERO floor change (floorDelta == 0), it is a flat floor seam.
							// However, on Map06, the 8-unit ledge edge line might return floorDelta == 0 under steep 
							// grazing angles. To isolate Map01, we verify: if it's dead flat AND sitting at the exact 
							// same floor level as the player's current boots (highestFloor == playerFloorZ), 
							// it is 100% a flat hallway light-grid strip. WE BYPASS CULLING INSTANTLY!
							const float zeroHeightDiff = 2.1f;
							if (floorDelta < zeroHeightDiff && ceilingDelta < zeroHeightDiff)
							{
								if (fabsf(highestFloor - playerFloorZ) < 1.0f)
								{
									continue; // Pure flat corridor floor seam detected (Map01), pass safely!
								}
							}

							// --- SPLITTER PATH 2: CLIFF DROP-OFF / TRENCH (Map02 Deep Ditch Protection) ---
							// Evaluate direction: if the floor drops DOWN as we step from target towards player 
							// (the adjacent floor is physically lower than the sprite's standing sector ground).
							float adjacentSectorFloor = (testLine->frontsector == thingSector) ? bFloor : fFloor;
							if (adjacentSectorFloor < (actorFloorZ - zeroHeightDiff))
							{
								continue; // Clear open cliff drop-off / trench detected (Map02), pass safely!
							}

							// --- SPLITTER PATH 3: HARD WALL & LEDGE INTERCEPT (Map06 Ledge Protection) ---
							// If we passed the corridor bypass and cliff bypass, any remaining elevated line 
							// or step rising above the player's floor level is a hard boundary structure!
							if (highestFloor >= (playerFloorZ + 4.0f) || lowestCeil <= (viewer->Z() + 41.0f))
							{
								return true; // True structural barrier or 8-unit ledge intercepted (Map06), CULL NOW!
							}
						}
					}
				}
			}
		}
	}

	return false;
}

struct SpriteCrossed2SBboxFaceCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached2sCrossedBBoxFaceResult = false;
};
static TMap<AActor *, SpriteCrossed2SBboxFaceCacheEntry> SpriteCrossed2sBboxFaceLineCache;

bool SpriteCrossed2sBBoxFaceLineCachedWrapper(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (enableAnamorphCache) 	// Use caching if enabled
	{
		const int             currentMapTimeTick = level.maptime;
		SpriteCrossed2SBboxFaceCacheEntry &entry = SpriteCrossed2sBboxFaceLineCache[thing];

		// Return cached result if valid
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached2sCrossedBBoxFaceResult;
		}

		// Compute and cache fresh result from non-cached function
		bool resultSprX2sLine = SpriteBboxFacingCameraCrossed2sLine(thing, viewer);
		entry.cached2sCrossedBBoxFaceResult = resultSprX2sLine;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return resultSprX2sLine;
	}
	else
	{
		// Original uncached behavior
		return SpriteBboxFacingCameraCrossed2sLine(thing, viewer);
	}
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
//--------------------------------------------------------------------------------------------
//METHOD 2: SpriteCrossed2sBboxFaceWallLinedef(The Area Density Sweeper)
//--------------------------------------------------------------------------------------------
//* APPROACH : Broad - phase AABB bounding box volume scanning(Carpet Bombing).
//* LOGIC :
//	Scans the ENTIRE expanded square layout around the actor via a blockmap geometry sweep.
//	It calculates a factual "wallRatio" based on the volumetric density of all lines
//	encountered inside the bounding area.
//* PRIMARY RESPONSIBILITY :
//  Target - locks massive structural blocks, huge 3D columns, moving vertical crushers(Map06),
//  and giant solid walls(Map12).It ensures that full - body sprites are hidden monolithically
//  behind massive geometry without dropouts.
//* LIMITATION :
//	Blind to precise corridor corners and narrow diagonal seams because tiny decorative floor
//	layout slits dilute its percentage ratio, causing it to occasionally miss critical corner edges.
//
// Method 2 handles the heavy physical architectural mass.
//--------------------------------------------------------------------------------------------

// This function tames "increaseAnam" projection leaks by evaluating surrounding 2-sided lines.
// It culls sprites if they are clipped by a high floor step or blocked by a valid middle texture.
// Relies on a stable static expanded radius and strict cross-product line-of-sight filtering.
bool anyWallBefore2Sline = false;
bool SpriteCrossed2sBboxFaceWallLinedef(AActor *thing, AActor *viewer, bool &outWallFound)
{
	if (!thing || !viewer) return false;

	//if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	sector_t *thingSector = thing->Sector;
	if (!thingSector || thingSector->Lines.Size() == 0) return false;

	const bool checkBboxCameraFace = true;

	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	float EyeHeight = 41.0f;
	if (viewer->player && viewer->player->mo)
	{
		EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
	}

	float viewerBottom = viewer->Z();
	float viewerTop = viewerBottom + EyeHeight;
	float viewerBottomAdj = viewerBottom - Ztolerance2sidedBot;
	float viewerTopAdj = viewerTop + Ztolerance2sided;

	float spriteBottom, spriteTop;
	if (thing->flags & MF_SPAWNCEILING)
	{
		spriteTop = (float)thing->Z();
		spriteBottom = spriteTop - (float)thing->Height;
	}
	else
	{
		spriteBottom = (float)thing->Z();
		spriteTop = spriteBottom + (float)thing->Height;
	}

	// 1. UNIVERSAL SIZE ADAPTATION (STATIC BACKUP)
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool  isMicroSprite = (spriteSize < 12.0f);
	const bool  isTinySprite = (spriteSize >= 12.0f && spriteSize < 18.0f);
	const bool  isSmallSprite = (spriteSize >= 18.0f && spriteSize < 38.0f);
	const bool  isMediumSprite = (spriteSize >= 38.0f && spriteSize < 45.0f);
	const bool  isLargeSprite = (spriteSize >= 45.0f && spriteSize < 60.0f);
	const bool  isHugeSprite = (spriteSize >= 60.0f);

	// Base Scales for test point offsets - kept static for reliable deep niche capture
	float                      spriteScale = 12.5f;
	if      (isTinySprite)     spriteScale = 9.5f;
	else if (isSmallSprite)    spriteScale = 8.5f;
	else if (isMediumSprite)   spriteScale = 7.7f;
	else if (isLargeSprite)    spriteScale = 5.4f;
	else if (isHugeSprite)     spriteScale = 2.15f;

	float adjustedRadius = thing->radius * spriteScale;

	// Scale for the "Kill Zone"
	float                      strictZoneScale = 12.5f;
	if      (isTinySprite)     strictZoneScale = 9.5f;
	else if (isSmallSprite)    strictZoneScale = 7.5f;
	else if (isMediumSprite)   strictZoneScale = 5.7f;
	else if (isLargeSprite)    strictZoneScale = 4.4f;
	else if (isHugeSprite)     strictZoneScale = 2.15f;

	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);

	// Reset the reference parameter bound to this specific execution call.
	// Never clear the global 'anyWallBefore2Sline' here as it wipes cached entries on trailing steps.
	outWallFound = false;

	// 2. SETUP DYNAMIC TOTAL AREA BOUNDS (NO MORE RAY-GAP LEAKS)
	float areaMinX = thingX - adjustedRadius; float areaMaxX = thingX + adjustedRadius;
	float areaMinY = thingY - adjustedRadius; float areaMaxY = thingY + adjustedRadius;

	if (checkBboxCameraFace)
	{
		float dx = viewerX - thingX; float dy = viewerY - thingY;
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist > 0.0f) { dx /= dist; dy /= dist; }

		float px = -dy; float py = dx;
		const float sidebboxfacetol = 0.7071f;

		float frontX = thingX + (dx * adjustedRadius);
		float frontY = thingY + (dy * adjustedRadius);
		float leftX = thingX + (dx * adjustedRadius * sidebboxfacetol) + (px * adjustedRadius * sidebboxfacetol);
		float leftY = thingY + (dy * adjustedRadius * sidebboxfacetol) + (py * adjustedRadius * sidebboxfacetol);
		float rightX = thingX + (dx * adjustedRadius * sidebboxfacetol) - (px * adjustedRadius * sidebboxfacetol);
		float rightY = thingY + (dy * adjustedRadius * sidebboxfacetol) - (py * adjustedRadius * sidebboxfacetol);

		areaMinX = MIN(thingX, MIN(frontX, MIN(leftX, rightX)));
		areaMaxX = MAX(thingX, MAX(frontX, MAX(leftX, rightX)));
		areaMinY = MIN(thingY, MIN(frontY, MIN(leftY, rightY)));
		areaMaxY = MAX(thingY, MAX(frontY, MAX(leftY, rightY)));
	}

	// 3. TOTAL AREA GEOMETRY SWEEP
	int minBX = level.blockmap.GetBlockX(areaMinX - 16.0f);
	int maxBX = level.blockmap.GetBlockX(areaMaxX + 16.0f);
	int minBY = level.blockmap.GetBlockY(areaMinY - 16.0f);
	int maxBY = level.blockmap.GetBlockY(areaMaxY + 16.0f);

	// Poll metrics for visibility window tolerance calculation
	int totalLinesEvaluated = 0;
	int solidWallsEncountered = 0;

	for (int bx = minBX; bx <= maxBX; bx++)
	{
		for (int by = minBY; by <= maxBY; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;

			int *list = level.blockmap.GetLines(bx, by);

			for (int i = 0; list[i] != -1; i++)
			{
				line_t *testLine = &level.lines[list[i]];

				// Only 2-sided internal walls
				if (!(testLine->flags & ML_TWOSIDED)) continue;

				float l1x = (float)testLine->v1->fX(); float l1y = (float)testLine->v1->fY();
				float l2x = (float)testLine->v2->fX(); float l2y = (float)testLine->v2->fY();

				// Check bounding box overlaps
				float lineMinX = MIN(l1x, l2x); float lineMaxX = MAX(l1x, l2x);
				float lineMinY = MIN(l1y, l2y); float lineMaxY = MAX(l1y, l2y);

				if (lineMaxX < areaMinX || lineMinX > areaMaxX || lineMaxY < areaMinY || lineMinY > areaMaxY)
				{
					continue;
				}

				float evalX = (l1x + l2x) * 0.5f;
				float evalY = (l1y + l2y) * 0.5f;

				// --- HARD PROXIMITY GATE (STRICTLY AT THING ZONE) ---
				float dxToThing = evalX - thingX;
				float dyToThing = evalY - thingY;
				float distToThingSq = (dxToThing * dxToThing) + (dyToThing * dyToThing);

				if (distToThingSq > strictZoneSq)
				{
					continue;
				}

				// Only count those 2S lines that got in the responsibility zone
				totalLinesEvaluated++;

				// 4. ADVANCED MID-TEXTURE PROXIMITY CULL FILTER
				bool lineContainsValidMidTex = false;
				for (int sideno = 0; sideno < 2; sideno++)
				{
					if (sideno == 1 && testLine->backsector == nullptr) continue;
					if (testLine->sidedef[sideno] == nullptr) continue;
					FTextureID midtex = testLine->sidedef[sideno]->GetTexture(side_t::mid);
					if (midtex.isValid() && midtex.GetIndex() > 0)
					{
						// === LZDoom07 way START ==============================================
						if (TexMan[midtex])
						{
							lineContainsValidMidTex = true;
							break;
						}
						// === LZDoom07 way FINISH =============================================
						// === UZDoom way START ================================================
						//FGameTexture *gtex = TexMan.GameTexture(midtex);
						//if (gtex && gtex->isValid() && gtex->GetTexture() != nullptr && !gtex->GetName().IsEmpty())
						//{
						//	lineContainsValidMidTex = true;
						//	break;
						//}
						// === UZDoom way FINISH ===============================================
					}
				}

				if (lineContainsValidMidTex)
				{
					solidWallsEncountered++;
					continue; // Go next, gather voices instead of hard return!
				}

				// UNCONDITIONAL VOLUME OCCLUSION VERDICT (HEIGHT STEPS)
				sector_t* frontSec = testLine->frontsector;
				sector_t* backSec = testLine->backsector;

				if (frontSec && backSec)
				{
					bool touchesSpriteSector = (frontSec == thingSector || backSec == thingSector);

					float fFloor = (float)frontSec->floorplane.ZatPoint(evalX, evalY);
					float bFloor = (float)backSec->floorplane.ZatPoint(evalX, evalY);

					float nextFloorZ = MAX(fFloor, bFloor);

					float cullingTolerance = touchesSpriteSector ? 32.0f : 1.0f;

					if (nextFloorZ > (spriteTop + cullingTolerance))
					{
						solidWallsEncountered++; // This is a hard wall
					}
				}
				else
				{
					solidWallsEncountered++;
				}
			}
		}
	}

	// --- POLL THE REALLY VISIBLE PART ---
	if (totalLinesEvaluated > 0)
	{
		float wallRatio = (float)solidWallsEncountered / (float)totalLinesEvaluated;

		if (wallRatio >= 0.4f)
		{
			outWallFound = true;
			return true; // Cull it hard: solid walls all around (Doom2 Remake - Map12)
		}
	}

	return false; // Visibility window tolerance: free area ahead, do NOT cull!
}

struct SpriteCrossed2sBboxWallCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached2sidedSpriteCrossedResult = false;
	bool cachedAnyWallBefore2Sline = false;
};
static TMap<AActor *, SpriteCrossed2sBboxWallCacheEntry> SpriteCrossed2sBboxWallCache;

bool SpriteCrossed2sBboxFaceWallCachedWrapper(AActor *thing, AActor *viewer)
{
	if (!thing || !viewer) return false;

	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		SpriteCrossed2sBboxWallCacheEntry &entry = SpriteCrossed2sBboxWallCache[thing];

		// 1. Cache hit: restore flag particulary for THIS sprite
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			anyWallBefore2Sline = entry.cachedAnyWallBefore2Sline;
			return entry.cached2sidedSpriteCrossedResult;
		}

		// 2. Calculation: pass local flag with the 3rd arg
		bool localWallFound = false;
		bool resultSprX2sBboxFaceLine = SpriteCrossed2sBboxFaceWallLinedef(thing, viewer, localWallFound);

		// 3. Record in cache: isolate data from rewrite by other sprites
		entry.cached2sidedSpriteCrossedResult = resultSprX2sBboxFaceLine;
		entry.cachedAnyWallBefore2Sline = localWallFound;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;

		// Export outside in the global bool
		anyWallBefore2Sline = localWallFound;
		return resultSprX2sBboxFaceLine;
	}
	else
	{
		// Uncached way
		bool localWallFound = false;
		bool result = SpriteCrossed2sBboxFaceWallLinedef(thing, viewer, localWallFound);
		anyWallBefore2Sline = localWallFound;
		return result;
	}
}
// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---



// --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- --- ---
// === Here comes regular 2sided obstructions check =======================================
// Here comes regular 2sided obstructions check
struct ObstructionData2Sided
{
	// Floor - track both lowest and highest
	float minFloor;
	float maxFloor;
	// Ceiling - track both lowest and highest
	float minCeiling;
	float maxCeiling;

	bool valid;
	bool isTightSector;                // flat/closed sectors like doors (gap <= 8 units)
	bool isPlatformTooHigh;
	bool isProjectileBehindObstacle;   // persistent flag for projectile door occlusion

	// Constructor to initialize all persistent flags
	ObstructionData2Sided()
	{
		minFloor = MAXCOORD2SIDED;     // Start high, find lowest
		maxFloor = MINCOORD2SIDED;     // Start low, find highest
		minCeiling = MAXCOORD2SIDED;   // Start high, find lowest
		maxCeiling = MINCOORD2SIDED;   // Start low, find highest
		valid = false;
		isTightSector = false;
		isPlatformTooHigh = false;
		isProjectileBehindObstacle = false;
	}

	void Update2sidedTallObstructions(AActor* thing, AActor* viewer, const sector_t* sector, const FVector2& point)
	{
		// Determine sprite classification
		// Must be done via "AND" but "OR" works better
		const bool isLegacyProjectile =
			(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
			(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
			(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);

		float spriteSize = (thing->radius + thing->Height) * 0.5f;
		const bool isSmallSprite = (spriteSize <= 18.0f);
		const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 40.0f);
		const bool isLargeSprite = (spriteSize > 40.0f);
		const bool isRegularMonster = !isSmallSprite && !isLegacyProjectile;

		float EyeHeight = 41.0f;
		if (viewer->player && viewer->player->mo)
		{
			EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
		}

		float viewerBottom = viewer->Z();
		float viewerTop = viewerBottom + EyeHeight;
		float viewerBottomAdj = viewerBottom - Ztolerance2sidedBot;
		float viewerTopAdj = viewerTop + Ztolerance2sided;

		float spriteBottom, spriteTop;
		if (thing->flags & MF_SPAWNCEILING)
		{
			spriteTop = (float)thing->Z();
			spriteBottom = spriteTop - (float)thing->Height;
		}
		else
		{
			spriteBottom = (float)thing->Z();
			spriteTop = spriteBottom + (float)thing->Height;
		}
		float sprBottomAdj = spriteBottom + Ztolerance2sidedBot;
		float sprTopAdj = spriteTop + Ztolerance2sided;
		float spriteMid = spriteBottom + ((spriteTop - spriteBottom) * 0.5f);

		//Printf("Viewer Info -> Bottom: %.2f | Top: %.2f | EyeHeight: %.2f | Class: %s\n", viewerBottom, viewerTop, EyeHeight, viewer->GetClass()->TypeName.GetChars());

		const FVector2 clamped =
		{
			clamp<float>(point.X, MINCOORD2SIDED, MAXCOORD2SIDED),
			clamp<float>(point.Y, MINCOORD2SIDED, MAXCOORD2SIDED)
		};

		float ceilingHeightInitial = sector->ceilingplane.ZatPoint(clamped.X, clamped.Y);
		float floorHeightInitial = sector->floorplane.ZatPoint(clamped.X, clamped.Y);

		// If the sector clearance is tightly shut or restricted within 8 units,
		// we flag it to prevent extreme anamorphic scaling from breaching closed doors/lifts.
		if ((ceilingHeightInitial - floorHeightInitial) <= 8.0f)
		{
			isTightSector = true;
		}

		// ==========================================================================
		// 3D-FLOOR LEDGE & CEILING BEAM OVERRIDE
		// Fixes both vertical extremes:
		// 1. When a 3D floor sits directly on the sector floor (solid steps).
		// 2. When a 3D floor is flush with the sector ceiling (hanging architectural beams).
		if (sector->e && sector->e->XFloor.ffloors.Size() > 0)
		{
			for (auto& floor : sector->e->XFloor.ffloors)
			{
				if (!(floor->flags & FF_SOLID)) continue;
				if (!floor->bottom.plane || !floor->top.plane) continue;

				float f3d_bottom = floor->bottom.plane->ZatPoint(clamped.X, clamped.Y);
				float f3d_top = floor->top.plane->ZatPoint(clamped.X, clamped.Y);

				// Case A: 3D floor is flush with the sector FLOOR (Solid step/ledge)
				if (fabs(f3d_bottom - floorHeightInitial) <= 1.0f)
				{
					if (f3d_top > viewerTopAdj)
					{
						// Forcefully lift the mathematical floor obstruction to the top of the 3D slab
						floorHeightInitial = MAX(floorHeightInitial, f3d_top);
					}
				}

				// Case B: 3D floor is flush with the sector CEILING (Hanging solid beam/roof)
				if (fabs(f3d_top - ceilingHeightInitial) <= 1.0f)
				{
					if (f3d_bottom < viewerBottomAdj)
					{
						// Forcefully push the mathematical ceiling obstruction down to the bottom of the 3D slab
						ceilingHeightInitial = MIN(ceilingHeightInitial, f3d_bottom);
					}
				}
			}
		}
		// ==========================================================================

		if (isLegacyProjectile)
		{
			// What this block also does is that in case projectile
			// is exploded in FRONT OF YOU and NOT an obstacle - uncull it too
			FVector2 vP = { (float)viewer->X(), (float)viewer->Y() };
			FVector2 tP = { (float)thing->X(), (float)thing->Y() };

			float d2LineSq = (vP - clamped).LengthSquared();
			float d2ThingSq = (vP - tP).LengthSquared();

			// 1. SKIP if the line is BEHIND the sprite
			if (d2LineSq > (d2ThingSq + 16.0f)) return;

			// 2. NEW: SKIP if the line is NOT an actual vertical obstruction
			bool blocksFloor = (floorHeightInitial > sprBottomAdj + 2.0f);
			bool blocksCeil = (ceilingHeightInitial < sprTopAdj - 2.0f);

			if (!blocksFloor && !blocksCeil) return; // Path is clear, skip this line

			// --- PRINTF: TRIGGER DATA ---
			// We only get here if the line is BETWEEN you and the sprite AND it has a height gap
			//Printf("PROJ_SCAN: %s | Floor: %.2f | SprAdjBtm: %.2f | blocksF: %d\n", 
			//       thing->GetClass()->TypeName.GetChars(), floorHeightInitial, sprBottomAdj, blocksFloor);

			// 3. REST OF THE LOGIC
			this->maxFloor = MAX(this->maxFloor, floorHeightInitial);
			this->minCeiling = MIN(this->minCeiling, ceilingHeightInitial);

			float diffToFloor = (float)fabs(sprBottomAdj - floorHeightInitial);
			float diffToCeil = (float)fabs(sprTopAdj - ceilingHeightInitial);

			float d2LineCenterSq = (tP - clamped).LengthSquared();

			if (d2LineCenterSq < 64.0f)
			{
				// Now it will only trigger if there's a real ledge (> 32 units)
				if (diffToFloor > 32.0f || diffToCeil > 32.0f)
				{
					isProjectileBehindObstacle = true;

					// --- PRINTF: CULLING EVENT ---
					// This is what actually hides your explosion
					// Printf("!!! CULLED !!! %s at [%.2f, %.2f] | DiffFloor: %.2f\n", 
					//	thing->GetClass()->TypeName.GetChars(), (float)thing->X(), (float)thing->Y(), diffToFloor);
				}
			}
		}

		float highestGameStep = 24.0f;
		float diffOfHigestStepAndHorizon = EyeHeight - highestGameStep;
		bool isPlatformTooHigh = (sprTopAdj - diffOfHigestStepAndHorizon + Ztolerance2sided) <= floorHeightInitial ||
			(sprBottomAdj + diffOfHigestStepAndHorizon + Ztolerance2sided) >= ceilingHeightInitial;

		
		// This block below fixes leaks on Doom2 Remake Map12
		// Calculate exact quadratic distances from the viewer to the intersection line and to the sprite
		FVector2 vP = { (float)viewer->X(), (float)viewer->Y() };
		FVector2 tP = { (float)thing->X(), (float)thing->Y() };
		float d2LineSq = (vP - clamped).LengthSquared();
		float d2ThingSq = (vP - tP).LengthSquared();

		// RULE 1: YOU MUST NOT CULL IF THE TWO-SIDED LINE IS LOCATED BEHIND THE SPRITE
		if (d2LineSq > (d2ThingSq + 16.0f)) return;

		// Calculate linear percentage (t) of where the intersection line sits along the sight ray (0.0 to 1.0)
		float t_intersect = 0.0f;
		if (d2ThingSq > 0.1f)
		{
			t_intersect = sqrtf(d2LineSq / d2ThingSq);
			t_intersect = clamp<float>(t_intersect, 0.0f, 1.0f);
		}

		// Linearly interpolate the exact mathematical height of the view ray at the intersection point.
		float rayAbsoluteZAtLine = viewerTop + (spriteMid - viewerTop) * t_intersect;
		float highestFloor = floorHeightInitial;

		bool rayIsBlockedByFloorLedge = (highestFloor >= rayAbsoluteZAtLine - 8.0f);
		bool rayIsBlockedByCeilingBeam = (ceilingHeightInitial <= rayAbsoluteZAtLine + 8.0f);

		// ============================================================================================
		// Coplanar Ledge Gate] - Bypasses Blind Spots on Map19 Ceiling Steps (C320 vs C288)
		// When the ceiling abruptly jumps up (e.g. from 288 to 320), the loose rayIsBlocked checks 
		// evaluate the line as completely transparent, leaving 'valid' as false and skipping occlusion.
		// We enforce a hard structural law: if the line's floor layout (highestFloor) stands physically 
		// HIGHER than the viewer's absolute feet (viewerBottom) AND we are inside the target's proximity,
		// it represents a solid lower barrier that blocks expanded bounding box edges. We FORCE register it.
		// ============================================================================================
		bool isCoplanarLedgeObstructingBottom = (highestFloor > (viewerBottom + Ztolerance2sidedBot)) &&
			(highestFloor >= (spriteBottom - 16.0f));

		if (rayIsBlockedByFloorLedge || rayIsBlockedByCeilingBeam || isCoplanarLedgeObstructingBottom)
		{
			// Calculate distance from the line intersect to the target sprite center
			float lineToThingDistSq = (clamped - tP).LengthSquared();

			// Only allow the line to lock a hard permanent obstruction if it sits near the target's portal cluster.
			// 16384.0f equals a stable 128-unit radius envelope around the actor.
			if (lineToThingDistSq <= 16384.0f || (thing->Sector == sector))
			{
				this->maxFloor = MAX(this->maxFloor, floorHeightInitial);
				this->minCeiling = MIN(this->minCeiling, ceilingHeightInitial);
				this->valid = true;
			}
		}
	}

	bool IsSpriteVisible2sided(AActor* viewer, AActor* thing, const FVector3& pos, float height) const
	{
		if (!valid) return true;
		if (isTightSector) return false;
		if (isPlatformTooHigh) return false;
		if (isProjectileBehindObstacle) return false;

		// In Doom, ceiling sprites (MF_SPAWNCEILING) have their Z at the top (anchor).
		// Their "height" grows DOWNWARDS. We must flip the bounds to avoid leaks.
		float spriteBottom, spriteTop;
		if (thing->flags & MF_SPAWNCEILING)
		{
			spriteTop = (float)thing->Z();                   // Anchor is at the top
			spriteBottom = spriteTop - (float)thing->Height; // Body grows downwards
		}
		else
		{
			spriteBottom = (float)thing->Z();                // Anchor is at the bottom
			spriteTop = spriteBottom + (float)thing->Height; // Body grows upwards
		}

		float viewerBottom = (float)viewer->Z();
		float viewerTop = viewerBottom + 41.0f; // Default eye height fallback

		const bool isLegacyProjectile =
			(thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
			(thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
			(thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);
		const float spriteSize = (thing->radius + thing->Height) * 0.5f;
		const bool isSmallSprite = (spriteSize <= 22.0f);

		//float distToCeil = fabs(spriteTop - minCeiling);   // For doors extruded from ceilings to floor
		//float distToFloor = fabs(spriteBottom - maxFloor); // For doors extruded from floor to ceilings

		// Min obstruction height to fully occlude a sprite behind it
		float              LEDGE_THRESHOLD = 0.0f;  // Initialize the variable
		if (isSmallSprite) LEDGE_THRESHOLD = 12.0f; // Needs taller obstructions to cull small sprites
		else               LEDGE_THRESHOLD = 4.0f;  // Shorter obstructions for bigger sprites not to leak through ledges

		// 1. Floor/Ledge occlusion
		// Trigger ONLY if the ledge is higher than the sprite's feet AND higher than your feet.
		if (maxFloor >= (spriteBottom + LEDGE_THRESHOLD))
		{
			if (maxFloor > (viewerBottom + Ztolerance2sided))
			{
				// This is a real obstacle in front of you, not the floor you're standing on.
				return false;
			}
		}

		// 2. Ceiling/Lintel occlusion
		// Trigger ONLY if the hanging wall is lower than the sprite's head AND lower than your head.
		if (minCeiling <= (spriteTop - LEDGE_THRESHOLD))
		{
			if (minCeiling < (viewerTop - Ztolerance2sided))
			{
				// This is a real hanging beam, not the high ceiling above your head.
				return false;
			}
		}

		return true;
	}

};

// Line-intersection test
static bool LineIntersectsSegment2sided(const FVector2& seg1Start, const FVector2& seg1End, const FVector2& seg2Start, const FVector2& seg2End, FVector2& intersectionPoint)
{
	const auto cross = [](const FVector2& a, const FVector2& b)
	{
		return a.X*b.Y - a.Y*b.X;
	};

	const FVector2 d1 = seg1End - seg1Start;
	const FVector2 d2 = seg2End - seg2Start;

	const FVector2 relA = seg2Start - seg1Start;
	const FVector2 relB = seg2End - seg1Start;

	const float cross1 = cross(d1, relA);
	const float cross2 = cross(d1, relB);

	// THE SEAM FIX - actually no difference but let it be here.
	// We use a tiny negative epsilon to ensure that if a ray hits a vertex (cross ~ 0), 
	// it's counted as a hit. 2.0f was creating a gap at every joint.
	if (cross1 * cross2 > 0.0001f) return false;

	const FVector2 relC = seg1Start - seg2Start;
	const FVector2 relD = seg1End - seg2Start;

	const float cross3 = cross(d2, relC);
	const float cross4 = cross(d2, relD);

	if (cross3 * cross4 > 0.0001f) return false;

	const float det = cross(d1, d2);

	// Keep the epsilon for parallel lines check only
	if (fabs(det) < 0.0001f) return false;

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
	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

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

	float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool isSmallSprite = (spriteSize <= 18.0f);
	const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 38.0f);
	const bool isLargeSprite = (spriteSize > 38.0f);
	const bool isRegularMonster = !isSmallSprite && !isLegacyProjectile;

	const bool isProjectileLargeSprite = (isLegacyProjectile || isLargeSprite);

	// Viewer positions
	float viewerBottom = viewer->Z();
	float viewerTop = viewerBottom + EyeHeight2;
	float viewerBottomAdj = viewerBottom - Ztolerance2sidedBot;
	float viewerTopAdj = viewerTop + Ztolerance2sided;

	// ==================================================================================================
	// THIS IS THE PLACE WHERE YOU CONFIGURE SPRITES CULLING AMOUNT PER TYPE FOR 2SIDED TALL OBSTRUCTIONS
	// 1st val is large spr, 2nd is small sprites, third is all the rest sprites, higher vals cull more
	float RadiusExpansionFactor = isProjectileLargeSprite ? (isSmallSprite ? 0.5f : 1.2f) : 2.5f;
	float HeightExpansionFactor = isProjectileLargeSprite ? (isSmallSprite ? 0.5f : 1.0f) : 2.5f;

	// Core sprite positions (using expansion factors)
	FVector2 viewerPos = GetActorPosition(viewer);
	FVector2 thingCenterPos = GetActorPosition(thing);
	float spriteBottom = thing->Z();
	float spriteTop = thing->Z() + thing->Height * HeightExpansionFactor;
	float sprBottomAdj = spriteBottom + Ztolerance2sidedBot;
	float sprTopAdj = spriteTop + Ztolerance2sided;
	float spriteMidHeight = spriteBottom + ((spriteTop - spriteBottom) * 0.5f);

	// Direction checks setup
	unsigned int directionMask = isProjectileLargeSprite ? 0xF : 0x0;
	int numDirectionChecks = CountBits2sidedObstr(directionMask);

	// --- SPATIAL POOLING SETUP ---
	// Grid-based cache for nearby actors to share calculations during the same frame
	static TMap<uint64_t, ObstructionData2Sided> spatialPool;
	static int lastPoolTick = -1;

	// Reset spatial pool only once per frame (1 tick)
	if (lastPoolTick != level.maptime)
	{
		spatialPool.Clear();
		lastPoolTick = level.maptime;
	}

	const float blockmGridSiz2S = 64.0f; // blockmap grid size (64x64)
	const float blockmGridSizInv2S = 1.0f / blockmGridSiz2S;

	// Calculate a spatial key based on a 64x64 unit world grid.
	// This allows sprites within ~64 units of each other to reuse tunnel data.
	int gridX = (int)(thingCenterPos.X * blockmGridSizInv2S);
	int gridY = (int)(thingCenterPos.Y * blockmGridSizInv2S);

	// Loop through which end of the viewer's eye line to test
	for (int EyeLevel = 0; EyeLevel < 2; EyeLevel++)
	{
		const float camHeight = (EyeLevel == 0) ? viewerBottomAdj : viewerTopAdj;
		FVector2 testPoint = thingCenterPos;

		for (int testIdx = 0; testIdx <= numDirectionChecks; ++testIdx)
		{
			ObstructionData2Sided obsData;

			// Generate a unique key per grid cell and per ray direction.
			// Generate a unique 64-bit key by packing GridX, GridY, and TestIdx.
			// We use large bit shifts (32 and 8) to act as "data containers" that never overlap.
			// This prevents "hash collisions" where sprites in different locations 
			// might accidentally share the same cache entry.
			// [64-bit Key Structure: 32 bits for X | 24 bits for Y | 8 bits for Ray Index]
			// MS Visual Studio 2022 may give you a warning that "(uint32_t)gridY << 8)" C6297,
			// meaning "Arithmetic Overflow" - do NOT change it to uint64_t as it gets broken!
			uint64_t spatialKey = ((uint64_t)gridX << 32) | ((uint32_t)gridY << 8) | (uint8_t)testIdx;

			if (testIdx > 0)
			{
				int shift = testIdx - 1;
				unsigned int currentDir = (1 << shift);
				if (!(directionMask & currentDir)) continue;
				testPoint = thingCenterPos + directionVectors[shift] * ((thing->radius) * RadiusExpansionFactor + 32.0f);
			}

			// --- 1. FACING CHECK (Dot Product) ---
			// Skip calculations for sprites behind the player's field of view
			FVector2 viewDir = { (float)cos(viewer->Angles.Yaw.Radians()), (float)sin(viewer->Angles.Yaw.Radians()) };
			FVector2 toSprite = (testPoint - viewerPos);
			float rayDist = toSprite.Length();
			const float rayDistInv = 1.0f / rayDist;

			if (rayDist > 0.1f)
			{
				FVector2 normToSprite = toSprite * rayDistInv;
				// If dot product < 0, sprite is behind the viewer
				if ((viewDir.X * normToSprite.X + viewDir.Y * normToSprite.Y) < 0.0f) continue;
			}

			// --- 2. SPATIAL CACHE CHECK ---
			// Reuse tunnel scan data if a nearby sprite already calculated this path
			if (spatialPool.CheckKey(spatialKey))
			{
				obsData = spatialPool[spatialKey];
			}
			else
			{
				// --- 3. TUNNEL BLOCKMAP SCAN (EXPLICIT FORWARD/BACKWARD FORK) ---
				// Considers window cases but may leak more if enabled
				const bool clearWindowObstrPath = true;

				// Calculate the structural sector floor delta via ZatPoint to bypass dynamic Z shifts (jumping).
				float viewerSectorFloor = (float)viewer->Sector->floorplane.ZatPoint(viewer->X(), viewer->Y());
				float thingSectorFloor = (float)thing->Sector->floorplane.ZatPoint(thing->X(), thing->Y());
				float staticFloorDelta = (float)fabs(viewerSectorFloor - thingSectorFloor);

				FVector2 dir;
				FVector2 currentPos;
				const float stepSize = 128.0f;
				int lastBX = -1, lastBY = -1;

				// Condition rule: Trigger backward tracing only if the global override is active,
				// there is a topographical split (delta > 16), AND both sectors are coplanar 
				// within a standard 72-unit vertical room scale tier (delta <= 72).
				if (clearWindowObstrPath && (staticFloorDelta > 16.0f) && (staticFloorDelta <= 72.0f))
				{
					// ============================================================================================
					// APPROACH A: NEW BACKWARD TRACING METHOD (Optimized for deep trenches and high windows)
					// Traces from target sprite backwards to viewer to clear front-facing window frames instantly.
					// ============================================================================================
					dir = -toSprite * rayDistInv;
					currentPos = testPoint;

					for (float d = 0; d <= rayDist + stepSize; d += stepSize)
					{
						int bx = level.blockmap.GetBlockX(currentPos.X);
						int by = level.blockmap.GetBlockY(currentPos.Y);
						currentPos += dir * stepSize;

						if (!level.blockmap.isValidBlock(bx, by)) continue;
						if (bx == lastBX && by == lastBY) continue;
						lastBX = bx; lastBY = by;

						int* list = level.blockmap.GetLines(bx, by);
						for (int i = 0; list[i] != -1; i++)
						{
							line_t* line = &level.lines[list[i]];
							const FVector2 lineStart = GetVertexPosition(line->v1);
							const FVector2 lineEnd = GetVertexPosition(line->v2);
							FVector2 intersectionPoint;

							if (!LineIntersectsSegment2sided(viewerPos, testPoint, lineStart, lineEnd, intersectionPoint))
								continue;

							const FVector2 clamped =
							{
								clamp<float>(intersectionPoint.X, MINCOORD2SIDED, MAXCOORD2SIDED),
								clamp<float>(intersectionPoint.Y, MINCOORD2SIDED, MAXCOORD2SIDED)
							};

							if (line->flags & ML_TWOSIDED)
							{
								if (line->frontsector) obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
								if (line->backsector) obsData.Update2sidedTallObstructions(thing, viewer, line->backsector, clamped);
							}
							else if (line->frontsector)
							{
								obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
							}
						}
					}
				}
				else
				{
					// ============================================================================================
					// APPROACH B: ORIGINAL FORWARD TRACING METHOD (Safe for aligned floors, dynamic actions, Map12)
					// Traces conventionally from player out to actor, ensuring close solid walls block first.
					// Runs as fallback if delta is tight, if sectors aren't coplanar (> 72), OR if switch is FALSE.
					// ============================================================================================
					dir = toSprite * rayDistInv;
					currentPos = viewerPos;

					for (float d = 0; d <= rayDist + stepSize; d += stepSize)
					{
						int bx = level.blockmap.GetBlockX(currentPos.X);
						int by = level.blockmap.GetBlockY(currentPos.Y);
						currentPos += dir * stepSize;

						if (!level.blockmap.isValidBlock(bx, by)) continue;
						if (bx == lastBX && by == lastBY) continue;
						lastBX = bx; lastBY = by;

						int* list = level.blockmap.GetLines(bx, by);
						for (int i = 0; list[i] != -1; i++)
						{
							line_t* line = &level.lines[list[i]];
							const FVector2 lineStart = GetVertexPosition(line->v1);
							const FVector2 lineEnd = GetVertexPosition(line->v2);
							FVector2 intersectionPoint;

							if (!LineIntersectsSegment2sided(viewerPos, testPoint, lineStart, lineEnd, intersectionPoint))
								continue;

							const FVector2 clamped =
							{
								clamp<float>(intersectionPoint.X, MINCOORD2SIDED, MAXCOORD2SIDED),
								clamp<float>(intersectionPoint.Y, MINCOORD2SIDED, MAXCOORD2SIDED)
							};

							if (line->flags & ML_TWOSIDED)
							{
								if (line->frontsector) obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
								if (line->backsector) obsData.Update2sidedTallObstructions(thing, viewer, line->backsector, clamped);
							}
							else if (line->frontsector)
							{
								obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
							}
						}
					}
				}
				// Store the result in the spatial pool for other sprites in this 64x64 grid cell
				spatialPool[spatialKey] = obsData;
			}

			// --- 4. FINAL VISIBILITY CHECK ---
			if (obsData.valid)
			{
				// Block visibility if path is tight or accumulated height covers the sprite
				if (obsData.isTightSector || !obsData.IsSpriteVisible2sided(viewer, thing, FVector3{ testPoint.X, testPoint.Y, spriteMidHeight }, spriteTop))
				{
					return false;
				}
			}
		}
	}

	return true; // Clear line of sight
}

struct Visibility2sidedObstrCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached2sidedObstrResult = true;
};
static TMap<AActor *, Visibility2sidedObstrCacheEntry> Visibility2sidedObstrCache;

// this function determines visibility of sprites behind tall enough 2-sided-linedef based obstructions, call it like
// that: bool visible2sideTallEnoughObstr =
// IsSpriteVisibleBehind2sidedLinedefbasedSectorObstructions(r_viewpoint.camera, thing);
bool IsSpriteVisibleBehind2sidedLinedefSectObstrWrapperCached(AActor *viewer, AActor *thing)
{
	//if (!viewer || !thing || viewer == thing) return false;

	// 1. Occlusion test - choose implementation based on toggle
	if (enableAnamorphCache)
	{
		// CACHED VERSION
		const int           currentMapTimeTick = level.maptime;
		Visibility2sidedObstrCacheEntry &entry = Visibility2sidedObstrCache[viewer, thing];

		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
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
//                                ----------------







//                                ----------------
//         ---===      ***************************************        ===---
// ******* 2-sided-linedef-based mid-texture in front of a sprite facing camera - start *******

// "CheckFacingMidTextureProximity" - finds out whether a mid texture facing camera
// lies on viewdirection between viewer and sprite behind that mid texture and returns
// a proximity value from 0 to 1, where 1 is fully visible. Large sprites must come at
// least 32 units to the texture to be fully occluded, all the rest sized ones - 64u.
// Also as soon as viewer comes very close to the sprite behind the mid texture, that
// immediately returns proximity of 1.0f which means no more occlusion, because sprites
// don't leak through mid textures on such low distances due to how GL depth works, so
// the viewer would have less chance seeing sprite chopped.

//constexpr float MINCOORD2SIDED      = -32768.0;
//constexpr float MAXCOORD2SIDED      = 32767.9999847;
//constexpr float EPSILON2SIDED       = 0.001;
//float           Ztolerance2sided    = 2.0f;
// 
//  lower vals - legs on elevations cull more if seen from below
//float           Ztolerance2sidedBot = 4.0f;

// The Accumulator structure based on 2sided obstruction architecture layout
struct MidTextureFencePathAccumulator
{
	float closestMidTexFenceDist;
	bool  isProjectileInFront; // Flag to abort occlusion if explosion occurs BEFORE the fence
	bool  hitValidFence;
	bool  isSolidFence;

	MidTextureFencePathAccumulator()
	{
		closestMidTexFenceDist = MAXCOORD2SIDED; // Start at maximum range
		isProjectileInFront = false;
		hitValidFence = false;
		isSolidFence = false;
	}

	void AccumulateMidTextureFenceData(line_t *line, float calculatedDist, AActor *thing, const AActor *viewer,
		                                                                     const FVector2 &intersectionPoint)
	{
		const bool isLegacyVersionProjectile = (thing->flags & (MF_MISSILE | MF_NOBLOCKMAP | MF_NOGRAVITY)) ||
			                                     (thing->flags2 & (MF2_IMPACT | MF2_NOTELEPORT | MF2_PCROSS));

		bool currentLineHasMid = false;
		bool currentLineIsSolid = false;
		FTextureID midtex;
		side_t* checkedSide = nullptr;

		for (int sideno = 0; sideno < 2; sideno++)
		{
			if (sideno == 1 && line->backsector == nullptr) continue;
			if (line->sidedef[sideno] == nullptr) continue;

			midtex = line->sidedef[sideno]->GetTexture(side_t::mid);
			if (midtex.isValid() && midtex.GetIndex() > 0)
			{
				// === LZDoom07 way START ==============================================================
				if (TexMan[midtex])
				{
					currentLineHasMid = true;
					checkedSide = line->sidedef[sideno];
					FTexture* tex = TexMan[midtex];
					if (tex && !tex->bMasked) currentLineIsSolid = true;
					break;
				}
				// === LZDoom07 way FINISH =============================================================

				// === UZDoom way START ================================================================
				//FGameTexture *gtex = TexMan.GameTexture(midtex);
				//// Verify that it is a real graphical texture map and not a generic empty node container
				//if (gtex && gtex->isValid() && gtex->GetTexture() != nullptr && !gtex->GetName().IsEmpty())
				//{
				//	currentLineHasMid = true;
				//	checkedSide = line->sidedef[sideno];
				//	if (!gtex->isMasked()) currentLineIsSolid = true;
				//	break;
				//}
				// === UZDoom way FINISH ===============================================================
			}
		}

		if (currentLineHasMid && checkedSide)
		{
			// === GEOMETRICAL RAY DIRECTION PROTECTION ===
			FVector2 vP = { (float)viewer->X(), (float)viewer->Y() };
			FVector2 tP = { (float)thing->X(), (float)thing->Y() };

			float d2LineSq = (vP - intersectionPoint).LengthSquared();
			float d2ThingSq = (vP - tP).LengthSquared();

			// THE CRITICAL DIVIDE (MAP26 vs MAP19 Unified Fix):
			// Genuine 2-sided fences/windows MUST stand strictly BETWEEN the viewer and the actor.
			// If a 2-sided mid-texture is behind the actor, we drop it immediately (MAP26 Chaingunguy Fix).
			// However, 1-sided walls (window frames/corners allowed by radial prescan) are allowed 
			// to be slightly further than the actor's center to clip grazing angles correctly (MAP19 Citadel Fix).
			const bool isLine2Sided = (line->backsector != nullptr);
			if (isLine2Sided && (d2LineSq > d2ThingSq))
			{
				return; // Uncull: Background 2S fences cannot block sight
			}

			// SPECIAL PROJECTILE / EXPLOSION PROTECTION (Brought from 2S system)
			if (isLegacyVersionProjectile)
			{
				if (d2LineSq > (d2ThingSq + 16.0f))
				{
					isProjectileInFront = true;
					return;
				}
			}

			hitValidFence = true;
			if (calculatedDist < closestMidTexFenceDist)
			{
				closestMidTexFenceDist = calculatedDist;
				isSolidFence = currentLineIsSolid;
			}
		}
	}
};

static float CheckFacingMidTextureProximity(AActor *thing, const AActor *viewer, TVector3<double> &thingpos)
{
	// Printf("===== TEXTURE PROXIMITY CHECK START =====\n");
	// Printf("Thing: %s at (%.1f, %.1f, %.1f)\n", thing->GetClass()->TypeName.GetChars(), thingpos.X, thingpos.Y,
	// thingpos.Z); Printf("Camera: (%.1f, %.1f, %.1f) facing %.1f degrees\n", camera->X(), camera->Y(), camera->Z(),
	// camera->Angles.Yaw.Degrees());

	// 1. Quick out: Frustum culling - DISABLED BECAUSE BAD FOR MID TEXTURES!!!
	//if (CheckFrustumCullingUNUSED(thing)) return 0.0f;

	// 2. Distance culling (far planes)
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return 0.0f;

	// 3. Actor classification flags and thresholds (CORPSE PROTECTION INTEGRATED)
	// If the actor is a corpse, its physical height drops to ~12, but for
	// occlusion perspective we MUST use its original living height (e.g. 56.0f)
	// to prevent it from narrowing down the ray fan adjustedRadius!
	float effectiveHeight = thing->Height;
	if ((thing->flags & MF_CORPSE) || effectiveHeight < 20.0f)
	{
		effectiveHeight = 56.0f; // Force standard monster height context
	}
	float spriteSize = (thing->radius + effectiveHeight) * 0.5f;
	const bool  isLargeSprite = (spriteSize > 40.0f);

	// SIZE ADAPTATION AND PARTICLE PROTECTION
	if (spriteSize <= 12.0f) spriteSize = 12.0f;
	const bool isMicroSprite = (spriteSize <= 12.0f);
	const bool isSmallSprite = (spriteSize > 12.0f && spriteSize <= 38.0f);

	float                   spriteScale = 5.7f;
	if      (isMicroSprite) spriteScale = 9.5f;
	else if (isSmallSprite) spriteScale = 9.0f;

	float effectiveRadius = thing->radius;
	// If a sprite has no radius or a really small one
	if (effectiveRadius < 1.0f) effectiveRadius = 12.0f;
	float adjustedRadius = effectiveRadius * spriteScale;

	const bool isLegacyVersionProjectile = thing->flags & (MF_MISSILE | MF_NOBLOCKMAP | MF_NOGRAVITY) ||
		                                     thing->flags2 & (MF2_IMPACT | MF2_NOTELEPORT | MF2_PCROSS);

	// Get vertical positioning info
	float EyeHeight = 41.0f;
	if (viewer->player && viewer->player->mo)
	{
		EyeHeight = (viewer->player->mo->FloatVar(NAME_ViewHeight) + viewer->player->crouchviewdelta);
		// Printf("EyeHeight1: %.2f (ViewHeight: %.2f + crouchdelta: %.2f)\n", EyeHeight1,
		// viewer->player->mo->FloatVar(NAME_ViewHeight), viewer->player->crouchviewdelta);
	}
	else
	{
		// Printf("EyeHeight1: %.2f (default value, no player object)\n", EyeHeight1);
	}
	const float Ztol2sMidTxt = 8.0f;
	float       viewerBottom = viewer->Z();
	float       viewerTop = viewerBottom + EyeHeight;
	float       viewerBottomAdj = viewerBottom + Ztol2sMidTxt;
	float       viewerTopAdj = viewerTop + Ztol2sMidTxt;
	float       spriteBottom, spriteTop;
	if (thing->flags & MF_SPAWNCEILING)
	{
		spriteTop = (float)thing->Z();
		spriteBottom = spriteTop - (float)thing->Height;
	}
	else
	{
		spriteBottom = (float)thing->Z();
		spriteTop = spriteBottom + (float)thing->Height;
	}
	float sprBottomAdj = spriteBottom + Ztolerance2sidedBot;
	float sprTopAdj = spriteTop + Ztolerance2sided;

	bool sprIsTooLow = (viewerBottomAdj + (EyeHeight * 0.3f)) > sprTopAdj;
	bool sprIsTooHigh = (viewerBottomAdj + (EyeHeight * 0.3f)) < sprTopAdj;

	// Printf("  Actor type: %s%s (size: %.1f)\n", isLegacyProjectile ? "Projectile " : "", isLargeSprite ? "Large" :
	// "Standard", spriteSize);

	// 4. Proximity thresholds where occlusion is disabled (to prevent popping)
	const float CLOSE_DIST_SMALL = 64.0f; // Standard avoidance distance
	const float CLOSE_DIST_LARGE = 96.0f; // Extended for large entities

	// 5. Measured distance from camera to thing
	float distToCamSq = (thingpos - TVector3<double>(viewer->X(), viewer->Y(), viewer->Z())).LengthSquared();

	bool  isViewerVerticallyAligned = (viewerBottomAdj <= spriteBottom) || (viewerBottom >= spriteTop);
	float minDist = (isLegacyVersionProjectile || isLargeSprite) ? CLOSE_DIST_LARGE : CLOSE_DIST_SMALL;

	// 6. If very close and vertically aligned - full visibility (disable occlusion)
	// because sprites don't override walls depth that hard when close and viewer is not under or above the sprite
	if (isViewerVerticallyAligned && distToCamSq <= minDist * minDist)
	{
		// Printf("Close proximity (%.1f units): Full visibility (factor=1.0)\n", sqrt(distToCamSq));
		return 1.0f;
	}
	// Printf("Distance to camera: %.1f units\n", sqrt(distToCamSq));

	// 7. Set detection radii based on actor type
	float       MAX_DIST_SOLID = 64.0f; // Standard solid wall max distance
	float       MAX_DIST_MASKED = 96.0f; // Masked textures have see-through parts
	const float EDGE_BUFFER = 32.0f; // Safety margin from texture edges

	// Adaptive adjustments for special actor types
	if (isLegacyVersionProjectile)
	{
		MAX_DIST_SOLID = 80.0f;
		MAX_DIST_MASKED = 112.0f;
		// Printf("  Using projectile detection ranges: SOLID=%.1f, MASKED=%.1f\n", MAX_DIST_SOLID, MAX_DIST_MASKED);
	}
	else if (isLargeSprite)
	{
		MAX_DIST_SOLID = 96.0f;
		MAX_DIST_MASKED = 128.0f;
		// Printf("Using large sprite detection ranges: SOLID=%.1f, MASKED=%.1f\n", MAX_DIST_SOLID, MAX_DIST_MASKED);
	}
	// else
	//{
	//     Printf("Using standard detection ranges: SOLID=%.1f, MASKED=%.1f\n", MAX_DIST_SOLID, MAX_DIST_MASKED);
	// }

	// Adaptive type calculation inside Paragraph 7 to keep distances perfectly synched
	bool isSolid = false;
	if (thing->Sector)
	{
		for (auto line : thing->Sector->Lines)
		{
			if (!line) continue;
			for (int sideno = 0; !isSolid && sideno < 2; sideno++)
			{
				if (sideno == 1 && line->backsector == nullptr) continue;
				if (line->sidedef[sideno] == nullptr) continue;

				FTextureID midtex = line->sidedef[sideno]->GetTexture(side_t::mid);
				if (midtex.isValid() && midtex.GetIndex() > 0)
				{
					// === LZDoom07 way START ==============================================================
					if (TexMan[midtex])
					{
						FTexture* tex = TexMan[midtex];
						if (tex && !tex->bMasked) isSolid = true;
						break;
					}
					// === LZDoom07 way FINISH =============================================================

					// === UZDoom way START ================================================================
					//FGameTexture *gtex = TexMan.GameTexture(midtex);
					//if (gtex && gtex->isValid() && gtex->GetTexture() != nullptr && !gtex->GetName().IsEmpty())
					//{
					//	if (!gtex->isMasked()) isSolid = true;
					//	break;
					//}
					// === UZDoom way FINISH ===============================================================
				}
			}
		}
	}

	// Explicit variables initialization inside the function scope
	float proximity_factor = 1.0f;
	bool  rayHitAnyValidMidTexture = false;

	// Setup vectors for line-intersection check matching CheckLineOfSight2sided
	FVector2 viewerPos = { (float)viewer->X(), (float)viewer->Y() };
	FVector2 thingPos2D = { (float)thingpos.X, (float)thingpos.Y };
	FVector2 toSprite = (thingPos2D - viewerPos);
	float    rayDist = toSprite.Length();
	if (rayDist <= 0.1f) return 1.0f;
	const float rayDistInv = 1.0f / rayDist;

	// Instantiate our path accumulator class
	MidTextureFencePathAccumulator pathData;

	// --- 8. TUNNEL BLOCKMAP SCAN (3-RAY FAN + LINEDEF PADDING + ACCUMULATOR) ---
	FVector2    dir = toSprite * rayDistInv;
	const float stepSize = 128.0f;
	FVector2    currentPos = viewerPos;
	int         lastBX = -1, lastBY = -1;

	for (float d = 0; d <= rayDist + stepSize; d += stepSize)
	{
		int bx = level.blockmap.GetBlockX(currentPos.X);
		int by = level.blockmap.GetBlockY(currentPos.Y);
		currentPos += dir * stepSize;

		if (!level.blockmap.isValidBlock(bx, by)) continue;
		if (bx == lastBX && by == lastBY) continue;
		lastBX = bx; lastBY = by;

		// === STEP 1: RADIAL 3x3 PRE-SCAN FOR MAP19 WINDOW CONTEXT ===
		// Look around the current block and its neighbors to see if any 2-sided mid-texture exists.
		// This protects window frames on MAP19 when split across blockmap boundaries.
		bool nearbyBlockContainsValid2SMidTex = false;

		for (int nx = -1; nx <= 1 && !nearbyBlockContainsValid2SMidTex; nx++)
		{
			for (int ny = -1; ny <= 1; ny++)
			{
				int targetBX = bx + nx;
				int targetBY = by + ny;

				if (!level.blockmap.isValidBlock(targetBX, targetBY)) continue;
				int *checkList = level.blockmap.GetLines(targetBX, targetBY);

				for (int j = 0; checkList[j] != -1; j++)
				{
					line_t *checkLine = &level.lines[checkList[j]];
					if (checkLine && (checkLine->flags & ML_TWOSIDED))
					{
						for (int sideno = 0; sideno < 2; sideno++)
						{
							if (sideno == 1 && checkLine->backsector == nullptr) continue;
							if (checkLine->sidedef[sideno] == nullptr) continue;

							FTextureID mtex = checkLine->sidedef[sideno]->GetTexture(side_t::mid);
							if (mtex.isValid() && mtex.GetIndex() > 0)
							{
								// === LZDoom07 way START ==============================================================
								if (TexMan[mtex])
								{
									nearbyBlockContainsValid2SMidTex = true;
									break;
								}
								// === LZDoom07 way FINISH =============================================================

								// === UZDoom way START ================================================================
								//FGameTexture *gtex = TexMan.GameTexture(mtex);
								//if (gtex && gtex->isValid() && gtex->GetTexture() != nullptr && !gtex->GetName().IsEmpty())
								//{
								//	nearbyBlockContainsValid2SMidTex = true;
								//	break;
								//}
								// === UZDoom way FINISH ===============================================================
							}
						}
					}
					if (nearbyBlockContainsValid2SMidTex) break;
				}
			}
		}

		// === STEP 2: MAIN SCROLLING LOOP (ANGULAR SPLITTER PROTECTION) ===
		int *list = level.blockmap.GetLines(bx, by);
		for (int i = 0; list[i] != -1; i++)
		{
			line_t *line = &level.lines[list[i]];
			if (!line || line->frontsector == nullptr) continue;

			// 9. MULTI-RAY PRECISION OCCLUSION DETECTION WITH VIRTUAL LINEDEF EXTENSION
			FVector2 lineStart = { (float)line->v1->fX(), (float)line->v1->fY() };
			FVector2 lineEnd = { (float)line->v2->fX(), (float)line->v2->fY() };
			FVector2 lineV = lineEnd - lineStart;

			float lineLen = lineV.Length();
			FVector2 lineDir = { 0.0f, 0.0f };
			if (lineLen > 0.1f)
			{
				lineDir = lineV / lineLen;
				const float paddingAmount = 16.0f;
				lineStart -= lineDir * paddingAmount;
				lineEnd += lineDir * paddingAmount;
				lineV = lineEnd - lineStart;
			}

			// ============================================================================================
			// [Dot-Product Angular Splitter] - Finely Calibrates Map19 vs Map26
			// 1S solid walls have no mid-textures but act as window trims. 
			// Under steep grazing angles (Map26), rays slide parallel to the wall, causing false culls.
			// We compute the Dot Product between the ray direction ('dir') and the wall direction ('lineDir').
			//
			// angleSens1S (0.0 to 1.0):
			// - Near 0.0: Allows almost ALL angles (Aggressive culling, leaks fixed, breaks Map26).
			// - Near 1.0: Blocks parallel grazing rays, allows only direct hits (Lax culling, fixes Map26).
			// We set a calibrated 0.65f threshold to completely bypass grazing column чирки on Map26, 
			// while cleanly capturing straight box window interventions on Map19.
			// ============================================================================================
			const bool isLine1Sided = (line->backsector == nullptr);
			if (isLine1Sided)
			{
				if (!nearbyBlockContainsValid2SMidTex) continue;

				if (d < 144.0f)
				{
					continue; // Safe baseline buffer for close proximity items
				}

				// Calculate angular alignment using absolute dot product
				// (1.0f means ray is perfectly parallel to the wall, 0.0f means perpendicular hit)
				float angularDotProduct = fabsf((dir.X * lineDir.X) + (dir.Y * lineDir.Y));

				const float angleSens1S = 0.65f; // Calibration sweet spot (from 0.0f to 1.0f)
				if (angularDotProduct > (1.0f - angleSens1S))
				{
					// The ray is sliding too parallel along this solid 1S pillar (Map26 corner glitch).
					// Skip it to prevent false-positive bounding box occlusion!
					continue;
				}
			}

			// === DYNAMIC RADIUS EXPANSION FOR FAR-PLACED SLITS ===
			float localAdjustedRadius = adjustedRadius;
			if (isLine1Sided && rayDist > 144.0f)
			{
				float distanceBonus = rayDist * 0.05f;
				localAdjustedRadius += clamp(distanceBonus, 0.0f, 14.0f);
			}

			// --- 3-RAY FAN GEOMETRY SETUP ---
			FVector2 sideOffset = { -dir.Y * localAdjustedRadius * 0.5f, dir.X * localAdjustedRadius * 0.5f };
			FVector2 raySources[3] = { viewerPos, viewerPos + sideOffset, viewerPos - sideOffset };
			FVector2 rayTargets[3] = { thingPos2D, thingPos2D + sideOffset, thingPos2D - sideOffset };
			bool     anyRayHitThisLine = false;
			FVector2 actualIntersectionPoint = { 0.0f, 0.0f };

			for (int r = 0; r < 3; r++)
			{
				FVector2 intersectionPoint;
				// Check every fan ray intersection with the virtually prolonged line
				// Passing correct array elements using index matching your vectors.h setup
				if (LineIntersectsSegment2sided(raySources[r], rayTargets[r], lineStart, lineEnd, intersectionPoint))
				{
					anyRayHitThisLine = true;
					actualIntersectionPoint = intersectionPoint; // Remember exactly where the ray clipped the wall
					break; 	// If at least one fan ray collided in the obstruction - OCCLUDE!
				}
			}

			if (!anyRayHitThisLine)
			{
				continue;
			}

			// Calculate the 2D planar distance to the intersected wall edge
			DVector2 lineVec2D(lineV.X, lineV.Y);
			double   lineLenSq = lineVec2D.LengthSquared();

			// Safe check to avoid any division-by-zero crashes
			if (lineLenSq < 1e-6) continue;

			DVector2 toSpriteCenter(thingpos.X - lineStart.X, thingpos.Y - lineStart.Y);
			float    dot = (float)(lineVec2D.X * toSpriteCenter.X + lineVec2D.Y * toSpriteCenter.Y);
			float    t_segment = clamp(float(dot / lineLenSq), 0.0f, 1.0f);

			DVector2 closest = { lineStart.X + t_segment * lineVec2D.X, lineStart.Y + t_segment * lineVec2D.Y };
			float    dist = float((thingpos - DVector3(closest, 0)).XY().Length());

			// Feed the intersection data to the accumulator class (including vector and actor pointers for missile checks)
			pathData.AccumulateMidTextureFenceData(line, dist, thing, viewer, actualIntersectionPoint);

			// Fast out if the projectile shortcut bypass triggered
			if (pathData.isProjectileInFront)
			{
				return 1.0f; // Return full visibility immediately!
			}
		}
	}

	// 9. FINAL VISIBILITY EVALUATION (SYNCHRONIZED WITH PHYSICAL RAY HIT)
	if (pathData.hitValidFence)
	{
		float maxDist = pathData.isSolidFence ? MAX_DIST_SOLID : MAX_DIST_MASKED;

		// ============================================================================================
		// [Anamorphic Scale Expansion Law] - Hard 320-Unit Fence Latch for Small Sprites
		// The 320-unit threshold expands the capture envelope strictly for pickup items/bonuses (MF_SPECIAL)
		// to resolve the Map19 side leaks. Regular monsters retain the accurate maxDist bounds.
		// ============================================================================================
		float threshold = maxDist + EDGE_BUFFER;
		if (isMicroSprite || isSmallSprite)
		{
			threshold = 320.0f; // Give explicit 320u geometric buffer ONLY to pickup bonuses (Map19)
		}

		if (pathData.closestMidTexFenceDist <= threshold)
		{
			// Calculate proximity factor from the strictly accumulated closest fence distance
			float distFromEdge = MAX(0.0f, pathData.closestMidTexFenceDist - EDGE_BUFFER);

			// Scale the lerp clamp safely to support the expanded envelope strictly for items
			float dynamicMaxDist = (thing->flags & MF_SPECIAL) ? 320.0f : maxDist;
			float factor = lerp(0.25f, 1.0f, distFromEdge / dynamicMaxDist);

			proximity_factor = clamp(factor, 0.25f, 1.0f);

			// ============================================================================================
			// [THE CRITICAL MISSING CONDITION] - Ray-Hit Verification Lock
			// Code set rayHitAnyValidMidTexture to true blindly if a fence was just *nearby* in the cache.
			// This caused all sprites within 320 units to drop out, regardless of whether the player stood inside 
			// or outside the cage. We lock this flag strictly to 'pathData.hitValidFence'. 
			// If the ray travels through clean air without piercing the grate, occlusion stays disabled!
			// ============================================================================================
			rayHitAnyValidMidTexture = true;
		}
	}
	else
	{
		// Force-kill the flag if the physical ray casting loop never intersected a fence line on this trace
		rayHitAnyValidMidTexture = false;
	}

	// 10. FINAL VERTICAL ANGLE CHECK WITH SINGULARITY HARD-CULL & LEDGE HEIGHT OFFSET MITIGATION
	if (fabs(viewerTop - spriteBottom) <= 1.5f && rayHitAnyValidMidTexture)
	{
		return 0.0f;
	}

	// Occlusion checks will now fire ONLY if the view ray is actively blocked by a fence polygon!
	if (rayHitAnyValidMidTexture && pathData.hitValidFence)
	{
		// --- ADAPTIVE LEDGE THRESHOLD & VIEW LEVEL MITIGATION ---
		float LEDGE_THRESHOLD = 0.0f;

		const float spriteCombinedSize = (thing->radius + thing->Height) * 0.5f;
		const bool  isSmallSpriteForLedge = (spriteCombinedSize <= 22.0f);

		if (isSmallSpriteForLedge) LEDGE_THRESHOLD = 7.0f;  // Needs taller obstructions to cull small sprites
		else                       LEDGE_THRESHOLD = 2.0f;  // For bigger sprites not to leak through short ledges

		if (sprIsTooLow || sprIsTooHigh)
		{
			// Fetch the true map sector planes where the fence stands to evaluate physical obstruction shifts
			float fenceFloor = (float)thing->Sector->floorplane.ZatPoint(thingPos2D);
			float fenceCeil = (float)thing->Sector->ceilingplane.ZatPoint(thingPos2D);

			// Floor/Ledge Occlusion with Height Shift Guard
			bool floorOccludes = false;
			if (fenceFloor >= (spriteBottom + LEDGE_THRESHOLD))
			{
				if (fenceFloor > (viewerBottom + Ztolerance2sided))
				{
					floorOccludes = true;
				}
			}

			// Ceiling/Lintel Occlusion with Height Shift Guard
			bool ceilingOccludes = false;
			if (fenceCeil <= (spriteTop - LEDGE_THRESHOLD))
			{
				if (fenceCeil < (viewerTop - Ztolerance2sided))
				{
					ceilingOccludes = true;
				}
			}

			// Only enforce the aggressive 0.25f clamping factor if the fence actually chokes your line of sight
			if (floorOccludes || ceilingOccludes)
			{
				proximity_factor = MIN(proximity_factor, 0.25f);
			}
		}
	}

	return proximity_factor;
}

struct MidTextureProximityCacheEntry
{
	int   lastMapTimeUpdateTick = -1;
	float cachedProximityFactor = 1.0f; // Changed to float
};
static TMap<AActor *, MidTextureProximityCacheEntry> MidTextureProximityCache;

bool CheckFacingMidTextureProximityWrapper(AActor *thing, AActor *viewer, TVector3<double> &thingpos)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int         currentMapTimeTick = level.maptime;
		MidTextureProximityCacheEntry &entry = MidTextureProximityCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			// DIRECTION: If proximity is high (>= 0.75f), it means NO occlusion, so return TRUE (Visible)
			// If proximity drops (e.g. 0.25f), return FALSE to let Pass 2 trigger active culling
			return entry.cachedProximityFactor >= 0.75f;
		}

		// Compute fresh proximity factor and cache it
		entry.cachedProximityFactor = CheckFacingMidTextureProximity(thing, viewer, thingpos);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;

		// DIRECTION: Strictly matching the verified visibility logic above
		return entry.cachedProximityFactor >= 0.75f;
	}
	else
	{
		// Original uncached behavior
		float proximity = CheckFacingMidTextureProximity(thing, viewer, thingpos);
		return proximity >= 0.75f;
	}
}

// ******* 2-sided-linedef-based mid-texture in front of a sprite facing camera - finish *******
//         ---===      ***************************************        ===---
//                                ----------------







//                                ----------------
//         ---===      ***************************************        ===---
// ******* 3DFloor-planar - floor and ceiling culling block start *******
static DVector3 GetSpriteOcclusionPoint3DFloors(AActor *thing, DVector3 &pos)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return DVector3(0, 0, 0);

	DVector3 occlusionPos = pos; // Start with original position

	// Only adjust RF_FACESPRITE types
	if ((thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
	{
		// Could be "and" but "or" works better
		const bool islegacyversionprojectile = (thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
                                               (thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
                                               (thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);

		const float base_radius = thing->radius;
		const float base_height = thing->Height;
		const float base_dimen = (base_radius + base_height) * 0.5f;

		const bool islittlesprite = (base_dimen <= 18.0f);
		const bool isactoracorpse = (thing->flags & MF_CORPSE) || (thing->flags & MF_ICECORPSE);
		const bool isactorsmallbutnotcorpse = islittlesprite && !isactoracorpse;

		if (isactorsmallbutnotcorpse)
		{
			// Expand virtual height dramatically for sprites with extended radius to block more agressively
			occlusionPos.Z += thing->Height * 2.0f;
			// Expand horizontally by 1600%
			occlusionPos += DVector3(thing->radius * 16.0f, thing->radius * 16.0f, 0);
		}
	}

	return occlusionPos;
}

// Shared filter to determine relevant 3D floors
static bool IsRelevantFFloor3DFloors(F3DFloor *rover, AActor *thing)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return 0;

	return (rover->flags & FF_EXISTS) && 
		   (rover->flags & FF_SOLID) && 
		   (rover->flags & FF_RENDERPLANES) &&
		   !(rover->flags & FF_TRANSLUCENT)
           //  no need to check those yet
           // && (rover->alpha >= 200)
         ; // don't miss the semicolon!
}

// Modified to use absolute sprite bottom
float GetActualSpriteFloorZ3DfloorsAndOther(sector_t *s, DVector3 &pos, AActor *thing)
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
float GetActualSpriteCeilingZ3DfloorsAndOther(sector_t *s, DVector3 &pos, AActor *thing)
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
static bool PlaneLineIntersection3DFloors(AActor *thing, secplane_t *plane, DVector3 &start, DVector3 &end, DVector3 &out)
{
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const DVector3 &n = plane->Normal();
	float      planeD = plane->fD();

	DVector3 dir = end - start;
	float    denominator = n | dir; // Dot product

	if (fabs(denominator) < 1e-8) return false; // Line is parallel to plane

	float t = -(planeD + (n | start)) / denominator;
	if (t < 0.0 || t > 1.0) return false; // Intersection point not between start and end

	out = start + t * dir;
	return true;
}

// Helper to get sector bounds
static void GetSectorBounds3DFloors(const sector_t *sec, DVector2 &minPoint, DVector2 &maxPoint)
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

bool IsSpriteBehind3DFloorPlane(DVector3 &cameraPos, DVector3 &spritePos, sector_t *sector, AActor *thing)
{
	if (!sector || !sector->e) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const float TOLERANCE = 16.0f;    // Vertex-aligned sector tolerance
	const float EPSILON = 8.0f;       // Vertical comparison safety
	const float HORIZ_SAFETY = 32.0f; // Horizontal expansion

	AActor     *viewer = players[consoleplayer].camera;
	const float viewerBottom = viewer->Z();                // Absolute bottom position
	const float viewerTop = viewerBottom + viewer->Height; // Absolute top position

	for (auto rover : sector->e->XFloor.ffloors)
	{
		if (!IsRelevantFFloor3DFloors(rover, thing)) continue;

		// 1. Get the 3D floor's target sector
		sector_t *target = rover->target;

		// 2. Calculate expanded bounds
		DVector2 minBound, maxBound;
		GetSectorBounds3DFloors(target, minBound, maxBound);

		// --- LZDoom07 way START ---
		 //minBound -= HORIZ_SAFETY;
		 //maxBound += HORIZ_SAFETY;
		// --- LZDoom07 way FINISH ---

		// --- UZDoom way START ---
		minBound.X -= HORIZ_SAFETY;
		minBound.Y -= HORIZ_SAFETY;
		maxBound.X += HORIZ_SAFETY;
		maxBound.Y += HORIZ_SAFETY;
		// --- UZDoom way FINISH ---

		// 3. Check if both camera and sprite are within bounds
		DVector2 camXY = cameraPos.XY();
		DVector2 sprXY = spritePos.XY();

		bool camInBounds = camXY.X >= minBound.X && camXY.X <= maxBound.X && 
			                 camXY.Y >= minBound.Y && camXY.Y <= maxBound.Y;
		bool sprInBounds = sprXY.X >= minBound.X && sprXY.X <= maxBound.X && 
			                 sprXY.Y >= minBound.Y && sprXY.Y <= maxBound.Y;

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
			if (viewerBottom > floorTop + EPSILON && spritePos.Z < floorTop - EPSILON)
				return true;

			// Camera below floor, sprite above
			if (viewerTop < floorBottom - EPSILON && spritePos.Z > floorBottom + EPSILON)
				return true;
		}
	}

	return false;
}

struct a3DFloorPlaneCacheEntry
{
	int  lastMapTimeUpdateTick = -1;
	bool cached3DFloorPlaneResult = false;
};
static TMap<AActor *, a3DFloorPlaneCacheEntry> a3DFloorPlaneCache; // Global cache storage

bool IsSpriteBehind3DFloorPlaneCachedWrapper(DVector3 &cameraPos, DVector3 &spritePos, sector_t *sector, AActor *thing)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int   currentMapTimeTick = level.maptime;
		a3DFloorPlaneCacheEntry &entry = a3DFloorPlaneCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
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
//                                ----------------







//                                ----------------
//         ---===      ***************************************        ===---
// ****************   3DFloor-sides culling block start   ****************

// 1. OBSTRUCTION DATA ACCUMULATOR FOR 3D FLOORS
struct ObstructionData3DFloor
{
	bool valid;
	bool isProjectileBehindObstacle; // Persistent flag for 3D-floor projectile door/ledge occlusion
	float maxFloorOverlap;   // Highest bottom plane Z that overlaps the ray
	float minCeilingOverlap; // Lowest top plane Z that overlaps the ray

	ObstructionData3DFloor()
	{
		valid = false;
		isProjectileBehindObstacle = false;
		maxFloorOverlap = -999999.0f; // Start far apart so the bounds can contract during accumulation
		minCeilingOverlap = 999999.0f;
	}

	void Accumulate3DFloorObstruction(AActor* thing, AActor* viewer, float fBot, float fTop,
		                      float rangeMin, float rangeMax, float viewerFeet, float viewZ,
		                      const FVector2& intersectionPoint, float sprBot, float sprTop)
	{
		// Determine sprite classification (matching the 2S logic setup)
		const bool isLegacyProjectile = (thing->flags & MF_MISSILE) || (thing->flags & MF_NOBLOCKMAP) ||
			                            (thing->flags & MF_NOGRAVITY) || (thing->flags2 & MF2_IMPACT) ||
			                            (thing->flags2 & MF2_NOTELEPORT) || (thing->flags2 & MF2_PCROSS);

		// THE CORE FIX: Intersection of two vertical segments
		float intersectMin = MAX(rangeMin, fBot);
		float intersectMax = MIN(rangeMax, fTop);
		float overlapHeight = intersectMax - intersectMin;

		// OCCLUSION CONDITION
		if (overlapHeight > 0.0f)
		{
			// Prevent self-occlusion when standing on the same 3D floor or high ceiling
			bool isPlayerStandingOnIt = (fTop >= (viewerFeet - 4.0f) && fTop <= (viewerFeet + 4.0f));
			bool isHighCeiling = (fBot > (viewZ + 8.0f));

			if (!isPlayerStandingOnIt && !isHighCeiling)
			{
				// SPECIAL PROJECTILE / EXPLOSION LOGIC (Brought from 2S system)
				if (isLegacyProjectile)
				{
					FVector2 vP = { (float)viewer->X(), (float)viewer->Y() };
					FVector2 tP = { (float)thing->X(), (float)thing->Y() };

					float d2LineSq = (vP - intersectionPoint).LengthSquared();
					float d2ThingSq = (vP - tP).LengthSquared();

					// 1. SKIP if the 3D-floor side line is BEHIND the sprite/explosion
					if (d2LineSq > (d2ThingSq + 16.0f)) return;

					// 2. SKIP if this specific 3D floor is NOT an actual vertical obstruction
					bool blocksFloor = (fTop > sprBot + 2.0f);
					bool blocksCeil = (fBot < sprTop - 2.0f);
					if (!blocksFloor && !blocksCeil) return; // Path is clear, skip this 3D floor

					// 3. Accumulate projectile specifics
					if (fTop > maxFloorOverlap) maxFloorOverlap = fTop;
					if (fBot < minCeilingOverlap) minCeilingOverlap = fBot;
					valid = true;

					float diffToFloor = (float)fabs(sprBot - fTop);
					float diffToCeil = (float)fabs(sprTop - fBot);
					float d2LineCenterSq = (tP - intersectionPoint).LengthSquared();

					// Only cull projectile if it's tightly close to a massive 3D ledge (> 32 units)
					if (d2LineCenterSq < 64.0f)
					{
						if (diffToFloor > 32.0f || diffToCeil > 32.0f)
						{
							isProjectileBehindObstacle = true;
						}
					}
				}
				else
				{
					// Regular monsters and items accumulation
					if (fTop > maxFloorOverlap) maxFloorOverlap = fTop;
					if (fBot < minCeilingOverlap) minCeilingOverlap = fBot;
					valid = true;
				}
			}
		}
	}
};

// Cache structure for 3D floor side (ribs) checks
struct a3DFloorSideCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached3DFloorSideResult = true;
};
static TMap<AActor*, a3DFloorSideCacheEntry> a3DFloorSideCache;

// Utility: get plane height at a 2D point
bool IsSpriteVisibleBehind3DFloorSides(AActor* viewer, AActor* thing)
{
	if (!viewer || !thing) return true;

	// 1. Frustum culling - DISABLED BECAUSE BAD
	// if (CheckFrustumCullingUNUSED(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const FVector2 vd = { (float)viewer->Pos().X, (float)viewer->Pos().Y };
	const FVector2 sd = { (float)thing->Pos().X, (float)thing->Pos().Y };

	// --- 1. SETUP BOUNDS (Including MF_SPAWNCEILING inversion) ---
	float sprBot, sprTop;
	if (thing->flags & MF_SPAWNCEILING)
	{
		sprTop = (float)thing->Z();
		sprBot = sprTop - (float)thing->Height;
	}
	else
	{
		sprBot = (float)thing->Z();
		sprTop = sprBot + (float)thing->Height;
	}

	const float viewZ = (float)viewer->Pos().Z + 41.0f;
	const float viewerFeet = (float)viewer->Z();
	const float dist2D = (sd - vd).Length();
	if (dist2D < 0.1f) return true;

	// Instance of the accumulator
	ObstructionData3DFloor obsData;

	// --- 2. BLOCKMAP SCANNING ---
	int minBX = level.blockmap.GetBlockX(MIN(vd.X, sd.X) - 16.0f);
	int maxBX = level.blockmap.GetBlockX(MAX(vd.X, sd.X) + 16.0f);
	int minBY = level.blockmap.GetBlockY(MIN(vd.Y, sd.Y) - 16.0f);
	int maxBY = level.blockmap.GetBlockY(MAX(vd.Y, sd.Y) + 16.0f);

	for (int bx = minBX; bx <= maxBX; bx++)
	{
		for (int by = minBY; by <= maxBY; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;
			int* list = level.blockmap.GetLines(bx, by);

			for (int i = 0; list[i] != -1; i++)
			{
				line_t* line = &level.lines[list[i]];
				FVector2 intersectionPoint;

				if (!LineIntersectsSegment2sided(vd, sd, GetVertexPosition(line->v1), GetVertexPosition(line->v2), intersectionPoint))
				{
					continue;
				}

				float t = (intersectionPoint - vd).Length() / dist2D;

				for (int s = 0; s < 2; s++)
				{
					sector_t* sec = (s == 0) ? line->frontsector : line->backsector;
					if (!sec || !sec->e || sec->e->XFloor.ffloors.Size() == 0) continue;

					for (auto& floor : sec->e->XFloor.ffloors)
					{
						if (!(floor->flags & FF_EXISTS) || !(floor->flags & FF_SOLID)) continue;
						if (!(floor->flags & (FF_RENDERSIDES | FF_RENDERPLANES))) continue;

						float fTop = (float)floor->top.plane->ZatPoint(intersectionPoint.X, intersectionPoint.Y);
						float fBot = (float)floor->bottom.plane->ZatPoint(intersectionPoint.X, intersectionPoint.Y);

						float rayZBot = viewZ + t * (sprBot - viewZ);
						float rayZTop = viewZ + t * (sprTop - viewZ);

						float rangeMin = MIN(rayZBot, rayZTop);
						float rangeMax = MAX(rayZBot, rayZTop);

						// Pass all data down to accumulator including projectile helper traits
						obsData.Accumulate3DFloorObstruction(thing, viewer, fBot, fTop, rangeMin, rangeMax,
							                         viewerFeet, viewZ, intersectionPoint, sprBot, sprTop);
					}
				}
			}
		}
	}

	// --- 3. FINAL VISIBILITY EVALUATION ---
	// If a projectile shortcut flag triggered a hard cull, hide it immediately
	if (obsData.isProjectileBehindObstacle) return false;

	if (obsData.valid)
	{
		const float spriteSize = (thing->radius + thing->Height) * 0.5f;
		float LEDGE_THRESHOLD = (spriteSize <= 22.0f) ? 12.0f : 4.0f;

		// Check A: Floor/Ledge occlusion
		if (obsData.maxFloorOverlap >= (sprBot + LEDGE_THRESHOLD))
		{
			if (obsData.maxFloorOverlap > (viewerFeet + 4.0f))
			{
				return false;
			}
		}

		// Check B: Ceiling/Lintel occlusion
		if (obsData.minCeilingOverlap <= (sprTop - LEDGE_THRESHOLD))
		{
			if (obsData.minCeilingOverlap < (viewZ - 4.0f))
			{
				return false;
			}
		}
	}

	return true;
}

// Cached wrapper function
bool IsSpriteVisibleBehind3DFloorSidesCachedWrapper(AActor* viewer, AActor* thing)
{
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		a3DFloorSideCacheEntry& entry = a3DFloorSideCache[thing];

		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 8)
		{
			return entry.cached3DFloorSideResult;
		}

		entry.cached3DFloorSideResult = IsSpriteVisibleBehind3DFloorSides(viewer, thing);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return entry.cached3DFloorSideResult;
	}
	else
	{
		return IsSpriteVisibleBehind3DFloorSides(viewer, thing);
	}
}

// ****************   3DFloor-sides culling block FINISH   ****************
//         ---===      ***************************************        ===---
//                                ----------------



static int resetCounter = -1;
void ResetAnamorphCache()
{
	// Only reset 1-5 caches per frame to spread out the cost
	switch (resetCounter % 8)
	{
	case 0:
		Visibility1sidedCache.Clear(0);
		Visibility2sidedObstrCache.Clear(0);
		break;
	case 1:
		SpriteCrossed2sLineSimpleCache.Clear(0);
		SpriteCrossed2sBboxFaceLineCache.Clear(0);
		SpriteCrossed2sBboxWallCache.Clear(0);
		break;
	case 2:
		SpriteIntersectsLineCache.Clear(0);
		SpriteBboxFacingCrossed1sCache.Clear(0);
		break;
	case 3:
		ViewerCrossed1sidedLineCache.Clear(0);
		SpriteCrossed1sidedVoidCache.Clear(0);
		SpriteCrossed1sVoidBboxCache.Clear(0);
		break;
	case 4:
		SpriteCrossed1sidedLineCache.Clear(0);
		MidTextureProximityCache.Clear(0);
		break;
	case 5:
		a3DFloorSideCache.Clear(0);
		a3DFloorPlaneCache.Clear(0);
		break;
	}
	resetCounter++;
}

//==========================================================================
//
// Sprite 3D Occlusion Culling - FINISH
//
//==========================================================================
