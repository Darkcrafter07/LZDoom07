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

#pragma once

// All structs are declared in gl_spritelight.cpp
struct FLegacyModelLightCache
{
	float absX, absY, absZ; // Absolute world dynlight coordinates
	float relX, relY, relZ; // Relative position of the dynamic light source
	float radius;       // Interaction radius of the light
	float r, g, b;      // Evaluated intensity color channels of the flare
};

struct FActorUnifiedCacheEntry
{
	AActor* actorPtr;
	int lastCachedTime;
	int cachedBufferIndexModernGL;
	double cachedX, cachedY, cachedZ;
	float maxRadiusFound; // Stores evaluated clean physical radius of the largest light

	// Legacy path cached outputs
	TArray<FLegacyModelLightCache> legacyLights;
	bool legacyActive;
	float outR, outG, outB;

	TArray<float> arrays[3]; // Modern GL3+ path deep-copy storage

	FActorUnifiedCacheEntry() : actorPtr(nullptr), lastCachedTime(-1),
		                        cachedX(0), cachedY(0), cachedZ(0), 
		                       maxRadiusFound(0.0f), legacyActive(false), 
		                                     outR(0), outG(0), outB(0) {}
	~FActorUnifiedCacheEntry()
	{
		legacyLights.Clear();
		arrays[0].Clear(); arrays[1].Clear(); arrays[2].Clear();
	}
};

#define UNIFIED_LIGHT_CACHE_SIZE 4096
extern FActorUnifiedCacheEntry g_ActorUnifiedCache[UNIFIED_LIGHT_CACHE_SIZE];
