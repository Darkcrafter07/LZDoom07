// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2026 Darkcrafter07
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

// gl_dynlightcache.h

#pragma once

//                           -----------------------
//     -------------=========================================------------------------------
// ================   DYNAMIC LIGHT 3D-MODEL OCCLUSION START   =================================
//     -------------=========================================------------------------------
//                           -----------------------

struct FLegacyDynlight3DmdlCache
{
	float absX, absY, absZ; // Absolute world dynlight coordinates
	float relX, relY, relZ; // Relative position of the dynamic light source
	float radius;       // Interaction radius of the light
	float r, g, b;      // Evaluated intensity color channels of the flare
};

struct F3DmdlDynlightUnifiedCacheEntry
{
	AActor* actorPtr;
	int lastCachedTime;
	int cachedBufferIndexModernGL;
	float cachedX, cachedY, cachedZ;
	float maxRadiusFound; // Stores evaluated clean physical radius of the largest light

	// Legacy path cached outputs
	TArray<FLegacyDynlight3DmdlCache> legacyLights;
	bool legacyActive;
	float outR, outG, outB;

	TArray<float> arrays[3]; // Modern GL3+ path deep-copy storage

	F3DmdlDynlightUnifiedCacheEntry() : actorPtr(nullptr), lastCachedTime(-1),
		                        cachedX(0), cachedY(0), cachedZ(0), 
		                       maxRadiusFound(0.0f), legacyActive(false), 
		                                     outR(0), outG(0), outB(0) {}
	~F3DmdlDynlightUnifiedCacheEntry()
	{
		legacyLights.Clear();
		arrays[0].Clear(); arrays[1].Clear(); arrays[2].Clear();
	}
};

#define UNIFIED_3DMDL_DYNLIGHT_CACHE_SIZE 4096
extern F3DmdlDynlightUnifiedCacheEntry g_3DmdlDynlightActorUnifiedCache[UNIFIED_3DMDL_DYNLIGHT_CACHE_SIZE];

//                           -----------------------
//     -------------=========================================------------------------------
// ================   DYNAMIC LIGHT 3D-MODEL OCCLUSION FINISH   ================================
//     -------------=========================================------------------------------
//                           -----------------------







//                           -----------------------
//     -------------=========================================------------------------------
// ================   DYNAMIC LIGHT MAP GEOMETRY OCCLUSION START   ===========================
//     -------------=========================================------------------------------
//                           -----------------------

//class GLWall;
//class GLFlat;
//
//// Struct to store baked wall segment parameters
//struct FBakedWall
//{
//	GLWall* wallObj;
//	int     masked;
//	int     foggy;
//};
//
//// Struct to store baked floor/ceiling flat parameters
//struct FBakedFlat
//{
//	GLFlat* flatObj;
//	int     masked;
//	int     foggy;
//};
//
//// Global high-speed linear cache descriptor
//struct FGLBSPCache
//{
//	int lastCachedTime;
//	bool isBakingActive;
//
//	TArray<FBakedWall> visibleWalls;
//	TArray<FBakedFlat> visibleFlats;
//
//	void Clear()
//	{
//		visibleWalls.Clear();
//		visibleFlats.Clear();
//	}
//};
//
//// Global cache instance variable shared across gl_bsp.cpp and gl_scene.cpp
//extern FGLBSPCache g_BSPRenderCache;

//                           -----------------------
//     -------------=========================================------------------------------
// ================   DYNAMIC LIGHT MAP GEOMETRY OCCLUSION FINISH   ==========================
//     -------------=========================================------------------------------
//                           -----------------------
