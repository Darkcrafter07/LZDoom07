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
//



// a_spriteocclusion3d.cpp
// Sprite/Particle 3D occlusion system by Darkcrafter07



#include "g_shared\a_spriteocclusion3d.h"



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

// Frustum culling
static bool CheckFrustumCulling(AActor* thing)
{
	return false; // Not culled by frustum
}

// Frustum culling - unused
// If you turn around too fast you see sprites behind
//   take too long to get their collision checked and unculled and it's slower
static bool CheckFrustumCullingUNUSED(AActor* thing)
{
	const DVector3 viewerPos = r_viewpoint.Pos;
	const DVector3 thingPos = thing->Pos();

	// Extract the ACTUAL current FOV from the viewpoint
	// r_viewpoint.FOV is already in degrees, so we don't need to convert
	float currentFOV = r_viewpoint.FieldOfView.Degrees;
	float currentFOVenlarged = currentFOV * 2.0f;

	// Calculate frustum based on tilt
	float tilt = fabs(static_cast<float>(r_viewpoint.Angles.Pitch.Degrees));
	if (tilt > 46.0f) return false; // Don't cull at extreme angles

	// Use the actual FOV instead of hardcoded 90
	float floatangle = 2.0 + (45.0 + (tilt / 1.9)) * currentFOVenlarged * 48.0 / AspectMultiplier(r_viewwindow.WidescreenRatio) / currentFOV;
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
	float segmentX; float segmentY;
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
static TMap<line_t*, TMap<SpriteIntersectsLinedefCacheKey, spriteIntersectsLineCacheEntry>> SpriteIntersectsLineCache;

bool spriteIntersectsLineCachedWrapper(line_t* line, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float& ix, float& iy)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		// Generate a unique key for this segment (using start and end points)
		SpriteIntersectsLinedefCacheKey key(x1 * 1000.0f + x2, y1 * 1000.0f + y2);
		spriteIntersectsLineCacheEntry& entry = SpriteIntersectsLineCache[line][key];
		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7 && entry.cachedSpriteIntersectsLineValid)
		{
			ix = entry.cached_ix; iy = entry.cached_iy;
			return entry.cachedSpriteIntersectsLine;
		}
		// Compute fresh result and cache it
		entry.cachedSpriteIntersectsLine = SpriteIntersectsLinedef(x1, y1, x2, y2, x3, y3, x4, y4, entry.cached_ix, entry.cached_iy);
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
// === CACHED version of SpriteIntersectsLinedef - finish === UNUSED ===

// ******* Anamorphic "Forced-Pespective" common early exit checks - finish *******
//         ---===      ***************************************        ===---







//         ---===      ***************************************        ===---
// ******* 1-sided-linedef culling block start *******

// Regular 1side line crossing check, may not cull void sprites
// Culls coplanar leaks the best but does it pretty roughly
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

	const float spriteSize = (thing->radius + thing->Height) * 0.5f;

	// Sprite size thresholds
	const bool isMicroSprite = (spriteSize <= 8.0f);
	const bool isTinySprite = (spriteSize <= 12.0f);
	const bool isSmallSprite = (spriteSize <= 18.0f);
	const bool isMediumSprite = (spriteSize > 18.0f && spriteSize <= 38.0f);
	const bool isLargeSprite = (spriteSize > 38.0f && spriteSize < 39.0f);
	const bool isHugeSprite = (spriteSize >= 39.0f);

	// Scale adjustments
	float                     spriteScale = 0.44f;  // Default: micro/tiny/small sprites
	if (isMediumSprite)     { spriteScale = 0.5f;  }
	else if (isLargeSprite) { spriteScale = 0.15f; }
	else if (isHugeSprite)  { spriteScale = 0.1f;  }

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
	int lastMapTimeUpdateTick = -1;
	bool cached1sidedCrossResult = false;  // Default: assume NO 1-sided crossing
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



// If only one side of a sprite bbox closest to and facing the viewer crossed a 1sided line
bool SpriteBboxFacingCameraCrossed1sLine(AActor* thing, AActor* viewer)
{
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// 1. SPATIAL POOLING
	static TMap<uint64_t, bool> spatial1sPool;
	static int last1sPoolTick = -1;
	if (last1sPoolTick != level.maptime) { spatial1sPool.Clear(); last1sPoolTick = level.maptime; }

	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();
	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();

	const float fovInv = 1.0f / 90.0f;             // for 90 degrees FOV
	const float blockmapGridSizInv = 1.0f / 16.0f; // for 16x16 grid size

	// KEY: Using 16-unit grid. 
	// Added coarse angle (90 deg steps) to key so results change when you orbit the sprite
	int gridX = (int)floorf(thingX * blockmapGridSizInv);
	int gridY = (int)floorf(thingY * blockmapGridSizInv);
	int angleKey = (int)(viewer->Angles.Yaw.Degrees * fovInv);
	uint64_t spatialKey = ((uint64_t)gridX << 32) | ((uint32_t)gridY << 8) | (uint8_t)angleKey;

	if (spatial1sPool.CheckKey(spatialKey)) return spatial1sPool[spatialKey];

	// 2. FACING DIRECTION CALCULATION
	// Vector from sprite to viewer
	float dx = viewerX - thingX;
	float dy = viewerY - thingY;
	float vDist = sqrt(dx * dx + dy * dy);
	if (vDist > 0.0f) { dx /= vDist; dy /= vDist; }

	// 3. SIZE ADAPTATION
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool isTinySprite = (spriteSize < 18.0f);
	const bool isLargeSprite = (spriteSize >= 45.0f);

	float                   spriteScale = 7.5f;
	if      (isTinySprite)  spriteScale = 3.5f;
	else if (isLargeSprite) spriteScale = 2.5f;
	else                    spriteScale = 3.5f;

	float adjustedRadius = thing->radius * spriteScale;

	float                   strictZoneScale = 7.5f;
	if (isTinySprite)       strictZoneScale = 3.5f;
	else if (isLargeSprite) strictZoneScale = 5.5f;
	else                    strictZoneScale = 4.0f;

	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);

	// 4. SELECTIVE TEST POINTS (Facing Only)
	// We only test the center and the side most facing the camera
	float testPts[2][2];
	testPts[0][0] = thingX; testPts[0][1] = thingY; // Always test center

	// Pick the point on the bbox radius that is closest to the viewer
	testPts[1][0] = thingX + (dx * adjustedRadius);
	testPts[1][1] = thingY + (dy * adjustedRadius);

	// 5. LOCAL SCANNING
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);

	bool result = false;
	int linesCount = 0;

	for (int bx = minBX; bx <= maxBX && !result; bx++)
	{
		for (int by = minBY; by <= maxBY && !result; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;
			int* list = level.blockmap.GetLines(bx, by);
			for (int i = 0; list[i] != -1 && linesCount < 64; i++)
			{
				line_t* line = &level.lines[list[i]];
				linesCount++;
				if (line->backsector != nullptr) continue;

				float l1x = (float)line->v1->fX();
				float l1y = (float)line->v1->fY();
				float l2x = (float)line->v2->fX();
				float l2y = (float)line->v2->fY();

				for (int j = 0; j < 2; j++) // Only 2 points: center and facing edge
				{
					float ix, iy;
					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1], l1x, l1y, l2x, l2y, ix, iy))
					{
						float dx_int = ix - thingX;
						float dy_int = iy - thingY;
						if ((dx_int * dx_int + dy_int * dy_int) < strictZoneSq)
						{
							result = true;
							break;
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
	int lastMapTimeUpdateTick = -1;
	bool cached1sCrossBboxFacingResult = false;  // Default: assume NO 1-sided crossing
};

static TMap<AActor*, SpriteBboxFacingCameraCrossed1sLineCacheEntry> SpriteBboxFacingCrossed1sCache;

bool SpriteBboxFacingCameraCrossed1sLineCachedWrapper(AActor* thing, AActor* viewer)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		SpriteBboxFacingCameraCrossed1sLineCacheEntry& entry = SpriteBboxFacingCrossed1sCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
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



// this one culls sprites that are located in the void
bool SpriteCrossed1sidedVoidLinedef(AActor* thing, AActor* viewer)
{
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	// 1. SPATIAL POOLING SETUP
	// Cache for 1-sided line intersections within a 64x64 unit grid
	static TMap<uint64_t, bool> spatial1sPool;
	static int last1sPoolTick = -1;

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
	int gridX = (int)floorf(thingX * blockmapGridSizInv);
	int gridY = (int)floorf(thingY * blockmapGridSizInv);
	uint64_t spatialKey = ((uint64_t)gridX << 32) | (uint32_t)gridY;

	// 2. SPATIAL CACHE CHECK
	if (spatial1sPool.CheckKey(spatialKey))
	{
		return spatial1sPool[spatialKey];
	}

	// 3. SIZE ADAPTATION AND PARTICLE PROTECTION
	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;

	const bool isMicroSprite = (spriteSize <= 12.0f); // Combine micro/tiny
	const bool isSmallSprite = (spriteSize > 12.0f && spriteSize <= 38.0f);

	float                     spriteScale = 0.7f;
	if (isMicroSprite)      { spriteScale = 1.5f; }
	else if (isSmallSprite) { spriteScale = 0.5f; }

	float effectiveRadius = thing->radius;
	if (effectiveRadius < 1.0f) effectiveRadius = 4.0f;
	float adjustedRadius = effectiveRadius * spriteScale;

	float testPts[5][2] =
	{
		{thingX, thingY},
		{thingX + adjustedRadius, thingY},
		{thingX - adjustedRadius, thingY},
		{thingX, thingY + adjustedRadius},
		{thingX, thingY - adjustedRadius}
	};

	// 4. LOCAL BLOCKMAP SCANNING
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0f);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0f);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0f);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0f);

	bool result = false;

	for (int bx = minBX; bx <= maxBX && !result; bx++)
	{
		for (int by = minBY; by <= maxBY && !result; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;

			int* list = level.blockmap.GetLines(bx, by);
			for (int i = 0; list[i] != -1; i++)
			{
				line_t* line = &level.lines[list[i]];

				// Only 1-sided walls
				if (line->backsector != nullptr) continue;

				float l1x = (float)line->v1->fX();
				float l1y = (float)line->v1->fY();
				float l2x = (float)line->v2->fX();
				float l2y = (float)line->v2->fY();

				for (int j = 0; j < 5; j++)
				{
					float ix, iy;
					if (SpriteIntersectsLinedef(viewerX, viewerY, testPts[j][0], testPts[j][1], l1x, l1y, l2x, l2y, ix, iy))
					{
						result = true;
						break;
					}
				}
				if (result) break;
			}
		}
	}

	// 5. STORE AND RETURN
	spatial1sPool[spatialKey] = result;
	return result;
}

// Cache structure for Void 1-sided line crossing checks
struct SpriteCrossed1sidedVoidCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached1sVoidCrossResult = false;
};
static TMap<AActor*, SpriteCrossed1sidedVoidCacheEntry> SpriteCrossed1sidedVoidCache;

bool SpriteCrossed1sidedVoidLinedefCachedWrapper(AActor* thing, AActor* viewer)
{
	if (!thing || !viewer) return false;

	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		SpriteCrossed1sidedVoidCacheEntry& entry = SpriteCrossed1sidedVoidCache[thing];

		// Return cached result if valid each 7 ticks
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
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


// Here comes the asset for regular 1sided wall obstructions check
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

bool IsSpriteVisibleBehind1sidedLinesCachedWrapper(AActor* thing, AActor* viewer, const DVector3& thingpos)
{
	// Fast escape checks
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

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
}

// ******* 1-sided-linedef culling block finish *******
//         ---===      ***************************************        ===---







//         ---===      ***************************************        ===---
// ******* 2-sided-linedef tall enough sector obstructions culling block start *******
constexpr float MINCOORD2SIDED = -32768.0;
constexpr float MAXCOORD2SIDED = 32767.9999847;
constexpr float EPSILON2SIDED = 0.001;
float Ztolerance2sided = 2.0f;
float Ztolerance2sidedBot = 4.0f; // lower vals - legs on elevations cull more if seen from below

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

// Unused because doesn't look too good, use with BBOX camera facing one instead
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

// This one is nice for taming "increaseAnam" leaks
// This one culls sprites if they crossed 2sided lines
// but if a side of the sprite bounding box facing the viewer - uncull
// I know the facing check is disabled but it's still better than nonfacing 1sideXser
bool SpriteBboxFacingCameraCrossed2sLine(AActor* thing, AActor* viewer)
{
	if (!thing || !viewer) return false;

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	bool checkBboxCameraFace = false; // disabled by default

	sector_t* thingSector = thing->Sector;
	if (!thingSector || thingSector->Lines.Size() == 0) return false;

	float viewerX = (float)viewer->X();
	float viewerY = (float)viewer->Y();
	float thingX = (float)thing->X();
	float thingY = (float)thing->Y();

	// === 1. UNIVERSAL SIZE ADAPTATION ===
	const float spriteSize = (thing->radius + thing->Height) * 0.5f;
	const bool isMicroSprite =  (spriteSize < 12.0f);
	const bool isTinySprite =   (spriteSize >= 12.0f && spriteSize < 18.0f);
	const bool isSmallSprite =  (spriteSize >= 18.0f && spriteSize < 38.0f);
	const bool isMediumSprite = (spriteSize >= 38.0f && spriteSize < 45.0f);
	const bool isLargeSprite =  (spriteSize >= 45.0f && spriteSize < 60.0f);
	const bool isHugeSprite =   (spriteSize >= 60.0f);

	// Scale for test point offsets (how far we look around the center)
	float                          spriteScale = 10.5f;     // isMircosprite and other unmentioned
	if      (isTinySprite)       { spriteScale = 5.5f;  }
	else if (isSmallSprite)      { spriteScale = 2.5f;  }
	else if (isMediumSprite)     { spriteScale = 1.2f;  }
	else if (isLargeSprite)      { spriteScale = 0.3f;  }
	else if (isHugeSprite)       { spriteScale = 0.15f; }

	float adjustedRadius = thing->radius * spriteScale;

	// Scale for the "Kill Zone" (how close the portal must be to block anamorphosis)
	float                          strictZoneScale = 10.5f; // isMircosprite and other unmentioned
	if      (isTinySprite)       { strictZoneScale = 8.5f;  }
	else if (isSmallSprite)      { strictZoneScale = 5.0f;  }
	else if (isMediumSprite)     { strictZoneScale = 3.2f;  }
	else if (isLargeSprite)      { strictZoneScale = 2.5f;  }
	else if (isHugeSprite)       { strictZoneScale = 1.5f;  }
	float strictZoneSq = (thing->radius * strictZoneScale) * (thing->radius * strictZoneScale);

	// === 2. SETUP TEST POINTS (Both modes use the same adjustedRadius) ===
	float testPts[5][2];
	int numPoints = 0;

	if (checkBboxCameraFace)
	{
		// MODE A: FACING SIDE + CENTER
		float dx = viewerX - thingX;
		float dy = viewerY - thingY;
		float dist = sqrt(dx * dx + dy * dy);
		if (dist > 0.0f) { dx /= dist; dy /= dist; }

		// Center
		testPts[0][0] = thingX; testPts[0][1] = thingY;

		// Determine the most facing point using adjustedRadius
		if (fabs(dx) > fabs(dy))
		{
			testPts[1][0] = (dx > 0) ? thingX + adjustedRadius : thingX - adjustedRadius;
			testPts[1][1] = thingY;
		}
		else
		{
			testPts[1][0] = thingX;
			testPts[1][1] = (dy > 0) ? thingY + adjustedRadius : thingY - adjustedRadius;
		}
		numPoints = 2;
	}
	else
	{
		// MODE B: FULL STAR (CENTER + 4 SIDES)
		testPts[0][0] = thingX;                  testPts[0][1] = thingY;
		testPts[1][0] = thingX + adjustedRadius; testPts[1][1] = thingY;
		testPts[2][0] = thingX - adjustedRadius; testPts[2][1] = thingY;
		testPts[3][0] = thingX;                  testPts[3][1] = thingY + adjustedRadius;
		testPts[4][0] = thingX;                  testPts[4][1] = thingY - adjustedRadius;
		numPoints = 5;
	}

	// === 3. PORTAL SCANNING (FBlockmap + Level version) ===
	int minBX = level.blockmap.GetBlockX(thingX - adjustedRadius - 16.0);
	int maxBX = level.blockmap.GetBlockX(thingX + adjustedRadius + 16.0);
	int minBY = level.blockmap.GetBlockY(thingY - adjustedRadius - 16.0);
	int maxBY = level.blockmap.GetBlockY(thingY + adjustedRadius + 16.0);

	for (int bx = minBX; bx <= maxBX; bx++)
	{
		for (int by = minBY; by <= maxBY; by++)
		{
			if (!level.blockmap.isValidBlock(bx, by)) continue;

			// Get lines indexes in this block
			int* list = level.blockmap.GetLines(bx, by);

			// Iterate list till "-1" is met
			for (int i = 0; list[i] != -1; i++)
			{
				line_t* testLine = &level.lines[list[i]];

				if (!(testLine->flags & ML_TWOSIDED)) continue;

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

						// If portal is intersected inside a sprite "Kill Zone"
						if ((dx_int * dx_int + dy_int * dy_int) < strictZoneSq)
						{
							return true; // Intersection found, return TRUE to reset increaseAnam
						}
					}
				}
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
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;
		SpriteCrossed2SidedLineCacheEntry& entry = SpriteCrossed2sidedLineCache[thing];

		// Return cached result if valid (updated within last 3 ticks)
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached2sidedSpriteCrossedResult;
		}

		// Compute and cache fresh result from non-cached function
		bool resultSprX2sLine = SpriteBboxFacingCameraCrossed2sLine(thing, viewer);
		entry.cached2sidedSpriteCrossedResult = resultSprX2sLine;
		entry.lastMapTimeUpdateTick = currentMapTimeTick;
		return resultSprX2sLine;
	}
	else
	{
		// Original uncached behavior
		return SpriteBboxFacingCameraCrossed2sLine(thing, viewer);
	}
}



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
	bool isProjectileBehindObstacle;   // persistent flag for projectile door occlusion

	// Constructor to initialize all persistent flags
	ObstructionData2Sided()
	{
		minFloor = MAXCOORD2SIDED;    // Start high, find lowest
		maxFloor = MINCOORD2SIDED;    // Start low, find highest
		minCeiling = MAXCOORD2SIDED;  // Start high, find lowest
		maxCeiling = MINCOORD2SIDED;  // Start low, find highest
		valid = false;
		isTightSector = false;
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
		// ==========================================================================
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
			// 1. Update MAXFLOOR/MINCEIL early so debug and logic see them
			this->maxFloor = MAX(this->maxFloor, floorHeightInitial);
			this->minCeiling = MIN(this->minCeiling, ceilingHeightInitial);

			// 2. Z-Horizon Snapping
			// We check if the projectile is within the "Ledge Danger Zone" (32 units).
			// If we are close to the line, even a 21-unit gap (like in your debug) 
			// will cause a massive anamorphic leak. We force occlusion.
			float diffToFloor = (float)fabs(sprBottomAdj - floorHeightInitial);
			float diffToCeil = (float)fabs(sprTopAdj - ceilingHeightInitial);

			FVector2 tPos = { (float)thing->X(), (float)thing->Y() };
			float distToLine = (tPos - clamped).Length();

			// If distance to line is less than 8 AND Z-gap is less than 32 units
			if (distToLine < 8.0f)
			{
				if (diffToFloor <= 32.0f || diffToCeil <= 32.0f)
				{
					isProjectileBehindObstacle = true;
				}
			}
		}

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

		minCeiling = MIN(minCeiling, ceilingHeightAdj1);
		maxCeiling = MAX(maxCeiling, ceilingHeightAdj1);
		minFloor = MIN(minFloor, floorHeightAdj1);
		maxFloor = MAX(maxFloor, floorHeightAdj1);

		valid = true;
	}

	bool IsSpriteVisible2sided(AActor* viewer, AActor* thing, const FVector3& pos, float height) const
	{
		if (!valid) return true;
		if (isTightSector) return false;
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
		float LEDGE_THRESHOLD = 0.0f;               // Initialize the variable
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
	float RadiusExpansionFactor = isProjectileLargeSprite ? (isSmallSprite ? 8.5f : 1.2f) : 6.0f;
	float HeightExpansionFactor = isProjectileLargeSprite ? (isSmallSprite ? 12.0f : 1.0f) : 1.64f;

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

	// Calculate a spatial key based on a 64x64 unit world grid.
	// This allows sprites within ~64 units of each other to reuse tunnel data.
	int gridX = (int)(thingCenterPos.X / 64.0f);
	int gridY = (int)(thingCenterPos.Y / 64.0f);

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

			if (rayDist > 0.1f)
			{
				FVector2 normToSprite = toSprite / rayDist;
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
				// --- 3. TUNNEL BLOCKMAP SCAN ---
				// Step along the sight line to find all transit sectors (trenches, steps, etc.)
				FVector2 dir = toSprite / rayDist;
				const float stepSize = 128.0f; // Step by block-map units
				FVector2 currentPos = viewerPos;
				int lastBX = -1, lastBY = -1;

				for (float d = 0; d <= rayDist + stepSize; d += stepSize)
				{
					int bx = level.blockmap.GetBlockX(currentPos.X);
					int by = level.blockmap.GetBlockY(currentPos.Y);
					currentPos += dir * stepSize;

					if (!level.blockmap.isValidBlock(bx, by)) continue;
					if (bx == lastBX && by == lastBY) continue; // Skip already processed blocks
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

						// Gather vertical obstruction data from the intersected lines
						if (line->flags & ML_TWOSIDED)
						{
							if (line->frontsector) obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
							if (line->backsector)  obsData.Update2sidedTallObstructions(thing, viewer, line->backsector, clamped);
						}
						else if (line->frontsector)
						{
							obsData.Update2sidedTallObstructions(thing, viewer, line->frontsector, clamped);
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
	if (!viewer || !thing || viewer == thing) return false;

	// We call the function with "!" - that's why return "true" when culled
	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

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
	float viewerBottomAdj = viewerBottom + Ztolerance2sided;
	float viewerTopAdj = viewerTop + Ztolerance2sided;
	float spriteBottom = thing->Z();
	float spriteTop = (thing->Z()) + (thing->Height);
	float sprBottomAdj = spriteBottom + Ztolerance2sided;
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

		// 12. DISTANCE TO LINE SEGMENT CALCULATION
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

		// 13. ADAPTIVE TEXTURE TYPE HANDLING
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

		// 14. PROXIMITY FACTOR CALCULATION
		float distFromEdge = MAX(0.0f, dist - EDGE_BUFFER);
		float factor = lerp(0.25f, 1.0f, distFromEdge / maxDist);
		factor = clamp(factor, 0.25f, 1.0f);
		proximity_factor = MIN(proximity_factor, factor);

		//Printf("Proximity factor: %.2f (current min: %.2f)\n", factor, proximity_factor);
	}

	//Printf("===== FINAL PROXIMITY FACTOR: %.2f =====\n", proximity_factor);

	return proximity_factor;
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
			return entry.cachedProximityFactor >= 0.75f;
		}

		// Compute fresh proximity factor and cache it
		entry.cachedProximityFactor = CheckFacingMidTextureProximity(thing, viewer, thingpos);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;

		// Convert cached proximity to boolean
		return entry.cachedProximityFactor > 0.75f;
	}
	else
	{
		// Original uncached behavior
		float proximity = CheckFacingMidTextureProximity(thing, viewer, thingpos);
		return proximity >= 0.75f;
	}
}

//         ---===      ***************************************        ===---
// ******* 2-sided-linedef-based mid-texture in front of a sprite facing camera - finish *******







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
float GetActualSpriteFloorZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing)
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
float GetActualSpriteCeilingZ3DfloorsAndOther(sector_t* s, DVector3& pos, AActor* thing)
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
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

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

	if (CheckFrustumCulling(thing)) return false;
	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

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
static TMap<AActor*, a3DFloorPlaneCacheEntry> a3DFloorPlaneCache; // Global cache storage

bool IsSpriteBehind3DFloorPlaneCachedWrapper(DVector3& cameraPos, DVector3& spritePos, sector_t* sector, AActor* thing)
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
bool IsSpriteVisibleBehind3DFloorSides(AActor* viewer, AActor* thing)
{
	if (!viewer || !thing) return true;

	if (IsAnamorphicDistanceCulled(thing, 2048.0f)) return false;

	const FVector2 vd = { (float)viewer->Pos().X, (float)viewer->Pos().Y };
	const FVector2 sd = { (float)thing->Pos().X, (float)thing->Pos().Y };

	// --- 1. SETUP BOUNDS (Including MF_SPAWNCEILING inversion) ---
	float sprBot, sprTop;
	if (thing->flags & MF_SPAWNCEILING)
	{
		// Ceiling-mounted: Anchor is at the top, height grows downwards
		sprTop = (float)thing->Z();
		sprBot = sprTop - (float)thing->Height;
	}
	else
	{
		// Ground-mounted: Anchor is at the bottom, height grows upwards
		sprBot = (float)thing->Z();
		sprTop = sprBot + (float)thing->Height;
	}

	const float viewZ = (float)viewer->Pos().Z + 41.0f; // Eye height matching your 2S logic
	const float dist2D = (sd - vd).Length();
	if (dist2D < 0.1f) return true;

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
						if (!(floor->flags & FF_SOLID) || !(floor->flags & FF_EXISTS)) continue;

						// 1. Get 3D floor bounds at intersection point
						float fTop = (float)floor->top.plane->ZatPoint(intersectionPoint.X, intersectionPoint.Y);
						float fBot = (float)floor->bottom.plane->ZatPoint(intersectionPoint.X, intersectionPoint.Y);

						// 2. Project sprite bounds (using player's EYE height)
						float rayZBot = viewZ + t * (sprBot - viewZ);
						float rayZTop = viewZ + t * (sprTop - viewZ);

						// 3. Define the actual span of the sprite as seen by the player
						float rangeMin = MIN(rayZBot, rayZTop);
						float rangeMax = MAX(rayZBot, rayZTop);

						// 4. LEDGE_THRESHOLD - same as your 2S logic
						const float spriteSize = (thing->radius + thing->Height) * 0.5f;
						float LEDGE_THRESHOLD = (spriteSize <= 22.0f) ? 12.0f : 4.0f;

						// 5. THE CORE FIX: Intersection of two vertical segments
						// We check if the 3D floor's vertical "wall" overlaps with the sprite's "view"
						float intersectMin = MAX(rangeMin, fBot);
						float intersectMax = MIN(rangeMax, fTop);

						float overlapHeight = intersectMax - intersectMin;

						// 6. OCCLUSION CONDITION
						// If the overlap is significant, the 3D floor is blocking the view
						if (overlapHeight > LEDGE_THRESHOLD)
						{
							// Important: To prevent self-occlusion when standing on the same 3D floor
							// we only occlude if the floor is NOT at our exact feet level (with tolerance)
							float viewerFeet = (float)viewer->Z();

							// If the 3D floor side is significantly above your feet OR significantly below your head
							// (This mimics your 2S logic for maxFloor > viewerBottom + Ztolerance)
							if (fTop > (viewerFeet + 2.0f) || fBot < (viewZ - 2.0f))
							{
								return false; // OCCLUDED
							}
						}
					}
				}
			}
		}
	}

	return true;
}

// Cache structure for 3D floor side (ribs) checks
struct a3DFloorSideCacheEntry
{
	int lastMapTimeUpdateTick = -1;
	bool cached3DFloorSideResult = false;
};
static TMap<AActor*, a3DFloorSideCacheEntry> a3DFloorSideCache; // Global cache storage for 3D-floor sides

bool IsSpriteVisibleBehind3DFloorSidesCachedWrapper(AActor* viewer, AActor* thing)
{
	// Use caching if enabled
	if (enableAnamorphCache)
	{
		const int currentMapTimeTick = level.maptime;

		// Access or create entry for this specific actor
		a3DFloorSideCacheEntry& entry = a3DFloorSideCache[thing];

		// Return cached result if valid (updated within last 7 ticks)
		// We use the same 7-tick window as your plane check for consistency
		if (entry.lastMapTimeUpdateTick != -1 && (currentMapTimeTick - entry.lastMapTimeUpdateTick) < 7)
		{
			return entry.cached3DFloorSideResult;
		}

		// Compute fresh result from the blockmap scanner
		entry.cached3DFloorSideResult = IsSpriteVisibleBehind3DFloorSides(viewer, thing);
		entry.lastMapTimeUpdateTick = currentMapTimeTick;

		return entry.cached3DFloorSideResult;
	}
	else
	{
		// Direct call if caching is disabled
		return IsSpriteVisibleBehind3DFloorSides(viewer, thing);
	}
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
		SpriteIntersectsLineCache.Clear(0);
		SpriteCrossed1sidedLineCache.Clear(0);
		SpriteCrossed1sidedVoidCache.Clear(0);
		SpriteBboxFacingCrossed1sCache.Clear(0);
		break;
	case 1:
		SpriteCrossed2sidedLineCache.Clear(0);
		Visibility1sidedCache.Clear(0);
		a3DFloorSideCache.Clear(0);
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
// Sprite 3D Occlusion Culling - FINISH
//
//==========================================================================
