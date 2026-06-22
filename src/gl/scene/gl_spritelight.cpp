// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
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
/*
** gl_light.cpp
** Light level / fog management / dynamic lights
**
*/

#include "gl/system/gl_system.h"
#include "c_dispatch.h"
#include "p_local.h"
#include "p_effect.h"
#include "vectors.h"
#include "gl/gl_functions.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "r_data/models/models.h"

#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/dynlights/gl_lightbuffer.h"

#include "r_data/models/models_ue1.h"
#include "r_data/models/models_obj.h"

FDynLightData modellightdata;
int modellightindex = -1;

template<class T>
T smoothstep(const T edge0, const T edge1, const T x)
{
	auto t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

CVAR(Bool, gl_cachedynlightmdlocclusion, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//==========================================================================
//
// Sets a single light value from all dynamic lights affecting the specified location
//
//==========================================================================

void gl_SetDynSpriteLight(AActor *self, float x, float y, float z, subsector_t * subsec)
{
	FDynamicLight *light;
	float frac, lr, lg, lb;
	float radius;
	float out[3] = { 0.0f, 0.0f, 0.0f };
	
	// Go through both light lists
	FLightNode * node = subsec->lighthead;
	while (node)
	{
		light=node->lightsource;
		if (light->ShouldLightActor(self))
		{
			float dist;
			FVector3 L;

			// This is a performance critical section of code where we cannot afford to let the compiler decide whether to inline the function or not.
			// This will do the calculations explicitly rather than calling one of AActor's utility functions.
			if (Displacements.size > 0)
			{
				int fromgroup = light->Sector->PortalGroup;
				int togroup = subsec->sector->PortalGroup;
				if (fromgroup == togroup || fromgroup == 0 || togroup == 0) goto direct;

				DVector2 offset = Displacements.getOffset(fromgroup, togroup);
				L = FVector3(x - (float)(light->X() + offset.X), y - (float)(light->Y() + offset.Y), z - (float)light->Z());
			}
			else
			{
			direct:
				L = FVector3(x - (float)light->X(), y - (float)light->Y(), z - (float)light->Z());
			}

			dist = (float)L.LengthSquared();
			radius = light->GetRadius();

			if (dist < radius * radius)
			{
				dist = sqrtf(dist);	// only calculate the square root if we really need it.

				frac = 1.0f - (dist / radius);

				if (light->IsSpot())
				{
					L *= -1.0f / dist;
					DAngle negPitch = -*light->pPitch;
					DAngle Angle = light->target->Angles.Yaw;
					double xyLen = negPitch.Cos();
					double spotDirX = -Angle.Cos() * xyLen;
					double spotDirY = -Angle.Sin() * xyLen;
					double spotDirZ = -negPitch.Sin();
					double cosDir = L.X * spotDirX + L.Y * spotDirY + L.Z * spotDirZ;
					frac *= (float)smoothstep(light->pSpotOuterAngle->Cos(), light->pSpotInnerAngle->Cos(), cosDir);
				}

				if (frac > 0 && GLRenderer->mShadowMap.ShadowTest(light, { x, y, z }))
				{
					lr = light->GetRed() / 255.0f;
					lg = light->GetGreen() / 255.0f;
					lb = light->GetBlue() / 255.0f;
					if (light->IsSubtractive())
					{
						float bright = (float)FVector3(lr, lg, lb).Length();
						FVector3 lightColor(lr, lg, lb);
						lr = (bright - lr) * -1;
						lg = (bright - lg) * -1;
						lb = (bright - lb) * -1;
					}

					out[0] += lr * frac;
					out[1] += lg * frac;
					out[2] += lb * frac;
				}
			}
		}
		node = node->nextLight;
	}
	gl_RenderState.SetDynLight(out[0], out[1], out[2]);
	modellightindex = -1;
}

void gl_SetDynSpriteLight(AActor *thing, particle_t *particle)
{
	if (thing != NULL)
	{
		gl_SetDynSpriteLight(thing, (float)thing->X(), (float)thing->Y(), (float)thing->Center(), thing->subsector);
	}
	else if (particle != NULL)
	{
		gl_SetDynSpriteLight(NULL, (float)particle->Pos.X, (float)particle->Pos.Y, (float)particle->Pos.Z, particle->subsector);
	}
}


//                           -----------------------
//     -------------=========================================------------------------------
// ================   DYNAMIC LIGHT 3D-MODEL OCCLUSION START   =================================
//     -------------=========================================------------------------------
//                           -----------------------


// ------------------------------------------------------------------------------------
//              MULTI-SUBSECTOR INJECTION WITH MONOLITH PROTECTION
// Huge rocks remain whole. Implemented adaptive filtering based on RenderRadius().
// Grass and barrels use the fast path (175 FPS), while massive monolithic rocks
// undergo proper BSP tree traversal for artifact-free, perfect rendering!
// ------------------------------------------------------------------------------------

bool g_legacyLightActive;
struct FLegacyModelLightCache
{
	float x, y, z;      // Relative position of the dynamic light source
	float trueX, trueY, trueZ; // Absolute world dynlight coordinates
	float radius;       // Interaction radius of the light
	float r, g, b;      // Evaluated intensity color channels of the flare
};
// Global pool trackers visible across gl_models.cpp and gl_spritelight.cpp
int g_legacyModelSectorLight; // Caches the actor's real-time sector lightlevel
TArray<FLegacyModelLightCache> g_legacyModelLights; // Holds ALL active lights affecting current model

template<typename Callback>
void isActorVisibToDynlightRegularWrapper(float x, float y, float radius, const Callback &callback)
{
	// Extract the runtime memory address of the currently processed actor mesh
	AActor* self = (AActor*)g_CurrentRenderingActorPtr;

	// Fail-safe hardware fallback: if the actor context is lost, preserve the thread pipeline
	if (!self || !self->subsector)
	{
		if (level.subsectors.Size() > 0) callback(&level.subsectors[0]);
		return;
	}

	// ADAPTIVE FILTER:
	// If a massive rock or giant asset enters (radius >= 256.0f),
	// we COMPLETELY bypass the fast-path buffer and route it through proper original BSP traversal!
	// This completely eliminates slicing artifacts and holes while maintaining cosmic speeds for foliage!
	if ((float)self->RenderRadius() >= 256.0f)
	{
		// ×åñòíûé ãëóáîêèé ðàäàð Ãðàôà Çàõëà ñòðîãî äëÿ ãèãàíòñêèõ ãîð êàðòû
		BSPWalkCircle(x, y, radius, callback);
		return;
	}

	// 1: DIRECT INTRA-FRAME SUBSECTOR SHUNT (FOR GRASS AND SMALL ASSETS)
	if (self->subsector)
	{
		callback(self->subsector);
	}

	// 2: MULTI-ZONE ADJACENT SECTOR FILLER (FOR CISTERNS AND BARRELS)
	sector_t *currentSector = self->Sector;
	if (currentSector != nullptr)
	{
		for (int i = 0; i < currentSector->subsectorcount; ++i)
		{
			subsector_t *sub = currentSector->subsectors[i];
			if (sub != nullptr && sub != self->subsector)
			{
				float subX = (float)sub->sector->centerspot.X;
				float subY = (float)sub->sector->centerspot.Y;
				float dx = subX - x;
				float dy = subY - y;

				//if ((dx * dx + dy * dy) <= ((radius + 4096.0f) * (radius + 4096.0f)))
				if ((dx * dx + dy * dy) <= (radius * radius))
				{
					callback(sub);
				}
			}
		}
	}
}


// --------------------------------------------------------------------------------------------
// ---------------------   DYNAMIC LIGHT OCCLUSION CACHED - START   ---------------------------

struct FActorUnifiedCacheEntry
{
	AActor* actorPtr;
	int lastCachedTime;
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
static FActorUnifiedCacheEntry g_ActorUnifiedCache[UNIFIED_LIGHT_CACHE_SIZE];

struct GLModelLightContext
{
	static AActor* CurrentActor;
	static float* outR;
	static float* outG;
	static float* outB;
	static FDynLightData* modernDataPtr;
	static float trueVisualRadius; // Global placeholder for raw model width
};
AActor*        GLModelLightContext::CurrentActor = nullptr;
float*         GLModelLightContext::outR = nullptr;
float*         GLModelLightContext::outG = nullptr;
float*         GLModelLightContext::outB = nullptr;
FDynLightData* GLModelLightContext::modernDataPtr = nullptr;
float          GLModelLightContext::trueVisualRadius = 0.0f; // Initialize with zero

// ====================================================================================
//                   HYBRID SPATIAL INTRA-FRAME GAUGE WRAPPER
// Full unification in action! Small-to-medium objects (grass, tanks) are processed
// via direct C-style fast path (175 FPS). Massive monolithic structures (rocks, cliffs)
// fall back to the original BSP tree traversal but are safeguarded by the adaptive
// FActorUnifiedCacheEntry with dormant tick optimization.
// ====================================================================================
template<typename Callback>
void isActorVisibToDynlightCachedWrapper(float modelX, float modelY, float searchRadius, const Callback &callback)
{
	AActor* currentActor = (AActor*)g_CurrentRenderingActorPtr;
	if (!currentActor || !currentActor->subsector)
	{
		if (level.subsectors.Size() > 0) callback(&level.subsectors[0]);
		return;
	}

	// PATH A: ULTRA-LIGHT DIRECT INJECTION SHUNT (FOR GRASS AND SMALL ASSETS)
	// If the model's radius is less than 256 units, we completely bypass all caches, ticks,
	// and BSP tree traversal. Instantaneous straight-through subsector forwarding at 175 FPS!
	if ((float)currentActor->RenderRadius() < 256.0f)
	{
		if (currentActor->subsector)
		{
			callback(currentActor->subsector);
		}

		sector_t *currentSector = currentActor->Sector;
		if (currentSector != nullptr)
		{
			for (int i = 0; i < currentSector->subsectorcount; ++i)
			{
				subsector_t *sub = currentSector->subsectors[i];
				if (sub != nullptr && sub != currentActor->subsector)
				{
					float subX = (float)sub->sector->centerspot.X;
					float subY = (float)sub->sector->centerspot.Y;
					float dx = subX - modelX;
					float dy = subY - modelY;


					if ((dx * dx + dy * dy) <= (searchRadius * searchRadius))
					{
						callback(sub);
					}
				}
			}
		}
		return; // exit quickly
	}

	// PATH B: ADAPTIVE 35-TICK UNIFIED CACHE (FOR GIANT ASSETS & ROCK MONOLITHS ONLY)
	// Reintroducing the per-frame cache with dormant ticks, now exclusively for 
	// massive rock formations—eliminating software overhead
	static int lastSeenEngineTime = -1;
	if (level.time < lastSeenEngineTime || level.time == 0)
	{
		for (int i = 0; i < UNIFIED_LIGHT_CACHE_SIZE; ++i)
		{
			g_ActorUnifiedCache[i].actorPtr = nullptr;
			g_ActorUnifiedCache[i].lastCachedTime = -1;
			g_ActorUnifiedCache[i].legacyLights.Clear();
			g_ActorUnifiedCache[i].maxRadiusFound = 0.0f; 
			g_ActorUnifiedCache[i].arrays[0].Clear();
			g_ActorUnifiedCache[i].arrays[1].Clear();
			g_ActorUnifiedCache[i].arrays[2].Clear();
		}
	}
	lastSeenEngineTime = level.time;

	// Resolve cache slot
	uint32_t baseIdx = ((uint32_t)(size_t)currentActor >> 4) % UNIFIED_LIGHT_CACHE_SIZE;
	int targetSlot = -1;
	for (uint32_t step = 0; step < 4; ++step)
	{
		uint32_t checkIdx = (baseIdx + step) % UNIFIED_LIGHT_CACHE_SIZE;
		if (g_ActorUnifiedCache[checkIdx].actorPtr == currentActor) { targetSlot = checkIdx; break; }
		if (g_ActorUnifiedCache[checkIdx].actorPtr == nullptr || (level.time - g_ActorUnifiedCache[checkIdx].lastCachedTime) >= 5)
		{
			if (targetSlot == -1) targetSlot = checkIdx;
		}
	}
	if (targetSlot == -1) targetSlot = baseIdx;
	FActorUnifiedCacheEntry& cache = g_ActorUnifiedCache[targetSlot];

	bool hasMoved = (cache.actorPtr == currentActor) &&
		         (cache.cachedX != currentActor->X() ||
			      cache.cachedY != currentActor->Y() ||
			       cache.cachedZ != currentActor->Z());

	// C-style tick hibernation gate for massive rocks models
	int requiredSleepTics = 0;
	if (cache.maxRadiusFound < 2048.0f)       requiredSleepTics = 1;
	else if (cache.maxRadiusFound < 8192.0f)  requiredSleepTics = 2;
	else if (cache.maxRadiusFound < 16000.0f) requiredSleepTics = 4;
	else                                      requiredSleepTics = 8; // Mega-dynlights to sleep for 8 tics

	// LONG CACHE HIT FOR HUGE ROCKS: If the mountain hasn't moved, get dynlight snapshot from memory
	if (cache.actorPtr == currentActor && !hasMoved && (level.time - cache.lastCachedTime) < requiredSleepTics && cache.lastCachedTime != -1)
	{
		if (gl.lightmethod == LM_LEGACY)
		{
			g_legacyModelLights = cache.legacyLights;
			g_legacyLightActive = cache.legacyActive;
			if (GLModelLightContext::outR) *GLModelLightContext::outR = cache.outR;
			if (GLModelLightContext::outG) *GLModelLightContext::outG = cache.outG;
			if (GLModelLightContext::outB) *GLModelLightContext::outB = cache.outB;
		}
		else
		{
			if (GLModelLightContext::modernDataPtr)
			{
				GLModelLightContext::modernDataPtr->Clear();
				GLModelLightContext::modernDataPtr->arrays[0] = cache.arrays[0];
				GLModelLightContext::modernDataPtr->arrays[1] = cache.arrays[1];
				GLModelLightContext::modernDataPtr->arrays[2] = cache.arrays[2];
			}
		}
		return; // Huge rock drawn for 0 CPU cycles without BSP traversal
	}

	// Huge rock CACHE-MISS: traverse faithful BSP nodes once per few tics
	if (gl.lightmethod == LM_LEGACY)
	{
		g_legacyModelLights.Clear(); g_legacyLightActive = false;
	}
	else
	{
		if (GLModelLightContext::modernDataPtr) GLModelLightContext::modernDataPtr->Clear();
	}

	// Call original BSPWalkCircle strictly for that huge rock!
	BSPWalkCircle(modelX, modelY, searchRadius, callback);

	// Bake BSP traversal results in the original per-actor cache
	cache.actorPtr = currentActor;
	cache.lastCachedTime = level.time;
	cache.cachedX = currentActor->X(); cache.cachedY = currentActor->Y(); cache.cachedZ = currentActor->Z();

	float largestRadiusThisFrame = 0.0f;
	if (gl.lightmethod == LM_LEGACY)
	{
		cache.legacyLights = g_legacyModelLights;
		cache.legacyActive = g_legacyLightActive;
		if (GLModelLightContext::outR) cache.outR = *GLModelLightContext::outR;
		if (GLModelLightContext::outG) cache.outG = *GLModelLightContext::outG;
		if (GLModelLightContext::outB) cache.outB = *GLModelLightContext::outB;

		float baseObjectWidth = GLModelLightContext::trueVisualRadius;
		float calculatedNormalizer = 1.0f;
		if (baseObjectWidth > 0.01f)
		{
			calculatedNormalizer = searchRadius / (baseObjectWidth + 512.0f);
		}
		for (unsigned int j = 0; j < g_legacyModelLights.Size(); ++j)
		{
			float pureLightRadius = (g_legacyModelLights[j].radius / calculatedNormalizer) - baseObjectWidth;
			if (pureLightRadius < 0.0f) pureLightRadius = 0.0f;
			if (pureLightRadius > largestRadiusThisFrame) largestRadiusThisFrame = pureLightRadius;
		}
	}
	else
	{
		if (GLModelLightContext::modernDataPtr)
		{
			cache.arrays[0] = GLModelLightContext::modernDataPtr->arrays[0];
			cache.arrays[1] = GLModelLightContext::modernDataPtr->arrays[1];
			cache.arrays[2] = GLModelLightContext::modernDataPtr->arrays[2];
		}
		largestRadiusThisFrame = (searchRadius > 512.0f) ? (searchRadius - 512.0f) : searchRadius;
	}
	cache.maxRadiusFound = largestRadiusThisFrame;
}

// ---------------------   DYNAMIC LIGHT OCCLUSION CACHED - FINISH   --------------------------
// --------------------------------------------------------------------------------------------



template<typename Callback>
void isActorVisibToDynlightMainWrapper(float x, float y, float radius, const Callback &callback)
{
	// Cached dynlight occlusion gives up to 10% FPS boost on big actors piercing through huge maps ...
	if (gl_cachedynlightmdlocclusion) return isActorVisibToDynlightCachedWrapper(x, y, radius, callback);
	// ... but here is the simpler and perhaps more stable version, use this one if it starts flickering
	else                           return isActorVisibToDynlightRegularWrapper(x, y, radius, callback);
}



void* g_CurrentRenderingActorPtr = nullptr; // no need to add "AActor *actor" in RenderFrame
extern bool g_currentModelIsWeldedSolid;
extern bool isUsingVolumetric3DModelLegacyDynlight;
int gl_SetDynModelLightVolumetricHuge(AActor *self, int dynlightindex) // old name: "gl_SetDynModelLight"
{
	isUsingVolumetric3DModelLegacyDynlight = false;

	if (gl.lightmethod == LM_DEFERRED && dynlightindex != -1)
	{
		gl_RenderState.SetDynLight(0, 0, 0);
		modellightindex = dynlightindex;
		return dynlightindex;
	}

	g_CurrentRenderingActorPtr = (void*)self; // no need to add "AActor *actor" in RenderFrame

	// ====================================================================================
	// GL1x/GL2x LEGACY way with unified all-format virtual injection
	// ====================================================================================
	if (gl.lightmethod == LM_LEGACY)
	{
		static AActor* lastProcessedActor = nullptr;

		if (self != lastProcessedActor)
		{
			g_legacyModelLights.Clear();
			g_legacyLightActive = false;
			lastProcessedActor = self;
		}

		g_legacyModelSectorLight = 128;
		float outR = 0.0f; float outG = 0.0f; float outB = 0.0f;

		if (self && self->subsector)
		{
			if (self->Sector != nullptr)
			{
				g_legacyModelSectorLight = self->Sector->lightlevel;
			}

			float modelX = (float)self->X(); float modelY = (float)self->Y();

			FSpriteModelFrame *smf = FindModelFrame(self->GetClass(), self->sprite, self->frame, false);
			float finalScaleX = (float)self->Scale.X; float finalScaleZ = (float)self->Scale.Y;
			if (smf != nullptr) { finalScaleX *= smf->xscale; finalScaleZ *= smf->zscale; }
			if (finalScaleX <= 0.0f) finalScaleX = 1.0f; if (finalScaleZ <= 0.0f) finalScaleZ = 1.0f;
			float modelFloorZ = (float)self->Z() + (float)self->GetBobOffset() + (float)self->SpriteOffset.Y;

			float trueVisualHeight = 0.0f; float trueVisualRadius = 0.0f;
			if (smf != nullptr && smf->modelIDs[0] != -1)
			{
				FModel *baseMdl = Models[smf->modelIDs[0]];
				int currentFrameNo = smf->modelframes[0];
				if (baseMdl != nullptr && currentFrameNo >= 0)
				{
					trueVisualHeight = baseMdl->GetTrueMDLVisualHeight(currentFrameNo, finalScaleZ);
					trueVisualRadius = baseMdl->GetTrueMDLVisualRadius(currentFrameNo, finalScaleX);
				}
			}

			if (trueVisualRadius <= 0.01f) trueVisualRadius = (float)self->RenderRadius() * (float)self->Scale.X;

			if (trueVisualHeight <= 0.0f)
			{
				trueVisualHeight = (float)self->Height * finalScaleZ;
				if (trueVisualHeight <= 2.0f) trueVisualHeight = trueVisualRadius * 2.0f;
				trueVisualHeight /= 2.0f;
			}

			if (trueVisualHeight <= 0.1f) trueVisualHeight = 6.0f;

			float modelTopZ = modelFloorZ + trueVisualHeight;

			// 5X HYPER-SCALE GEOMETRIC MULTIPLIER UNLOCKER!
			// Since the small tank is 5x smaller visually, its surface area drops by 25x!
			// We raise the baseline reference normalization threshold to 512.0 units 
			// and enforce a massive 4096.0 minimum world search horizon for BSPWalkCircle!
			float maxObjectDimForSearch = (trueVisualRadius > trueVisualHeight) ? trueVisualRadius : trueVisualHeight;
			float searchScaleFactor = 1.0f;

			// Reference 512.0f threshold to catch all small composite asset nodes easily
			if (maxObjectDimForSearch > 0.1f && maxObjectDimForSearch < 512.0f)
			{
				searchScaleFactor = 512.0f / maxObjectDimForSearch; // Yields massive scale payload (e.g. 5x - 10x boost)
				if (searchScaleFactor > 16.0f) searchScaleFactor = 16.0f;
			}

			// Hard-lock a massive 4096.0 minimum search radius to guarantee BSP subsectors break open!
			float searchRadius = (trueVisualRadius + 512.0f) * searchScaleFactor;
			if (searchRadius < 4096.0f) searchRadius = 4096.0f;

			// Local vector instance guarantees clean subsector lookups isolation
			std::vector<FDynamicLight*> addedLights;
			addedLights.clear();

			// Launch the unified multi-tiered adaptive throttling cache pass wrapper
			isActorVisibToDynlightMainWrapper(modelX, modelY, searchRadius, [&](subsector_t *subsector)
			{
				FLightNode * node = subsector->lighthead;
				while (node)
				{
					FDynamicLight *light = node->lightsource;
					if (light->ShouldLightActor(self))
					{
						int group = subsector->sector->PortalGroup;
						DVector3 reldynlightpos = light->PosRelative(group);
						DVector3 absdynlightpos = light->PosAbsolute(group);
						float radius = light->GetRadius();

						if (radius > 0.0f)
						{
							float dx = reldynlightpos.X - (float)modelX;
							float dy = reldynlightpos.Y - (float)modelY;
							float closestZ = reldynlightpos.Z;

							if (closestZ < (float)modelFloorZ) closestZ = (float)modelFloorZ;
							if (closestZ > (float)modelTopZ)   closestZ = (float)modelTopZ;
							float dz = reldynlightpos.Z - closestZ;

							float distSquared = dx * dx + dy * dy + dz * dz;

							float boundsRadiusCushion = trueVisualRadius;
							if (trueVisualHeight <= 6.1f) boundsRadiusCushion = trueVisualRadius * 0.5f;

							float totalInteractionRadius = radius + boundsRadiusCushion;

							if (distSquared < (float)(totalInteractionRadius * totalInteractionRadius))
							{
								if (std::find(addedLights.begin(), addedLights.end(), light) == addedLights.end())
								{
									addedLights.push_back(light);

									float dist = sqrtf((float)distSquared);
									float frac = 1.0f - (dist / totalInteractionRadius);

									if (trueVisualHeight <= 6.1f && reldynlightpos.Z > (float)modelTopZ)
									{
										float verticalDelta = (float)(reldynlightpos.Z - (float)modelTopZ);
										if (verticalDelta > radius) frac = 0.0f;
										else                        frac *= (1.0f - (verticalDelta / radius));
									}

									if (frac > 0.0f)
									{
										if (light->IsAdditive()) frac *= 0.2f;

										float r, g, b;
										if (radius >= 384.0f && radius <= 800.0f)
										{
											r = light->GetRed() / 286.0f * frac;
											g = light->GetGreen() / 286.0f * frac;
											b = light->GetBlue() / 286.0f * frac;
										}
										else if (radius >= 800.0f && radius <= 1600.0f)
										{
											r = light->GetRed() / 322.0f * frac;
											g = light->GetGreen() / 324.0f * frac;
											b = light->GetBlue() / 324.0f * frac;
										}
										else if (radius >= 1600.0f)
										{
											r = light->GetRed() / 424.0f * frac;
											g = light->GetGreen() / 424.0f * frac;
											b = light->GetBlue() / 424.0f * frac;
										}
										else
										{
											r = light->GetRed() / 255.0f * frac;
											g = light->GetGreen() / 255.0f * frac;
											b = light->GetBlue() / 255.0f * frac;
										}

										if (radius >= 1000.0f && radius <= 4096.0f)
										{ r *= 0.25f; g *= 0.25f; b *= 0.25f; }

										// Terminate overbright blowout for huge map lights (19,000 unit suns)
										if (radius > 4096.0f)
										{
											float overSizeFactor = radius / 4096.0f;
											float dynamicSquelch = 0.25f / (overSizeFactor * overSizeFactor);
											if (dynamicSquelch < 0.01f) dynamicSquelch = 0.01f;
											r *= dynamicSquelch; g *= dynamicSquelch; b *= dynamicSquelch;
										}

										// --- ADAPTIVE MULTI-STAGE INVERSE LIGHT SCALE CONTROLLER ---
										float maxObjectDim = (trueVisualRadius > trueVisualHeight) ? trueVisualRadius : trueVisualHeight;
										float modelScaleFactor = 1.0f;

										// Re-mapped to 512.0f master target threshold window
										if (maxObjectDim > 0.1f && maxObjectDim < 512.0f)
										{
											modelScaleFactor = maxObjectDim / 512.0f;
											if (modelScaleFactor < 0.02f) modelScaleFactor = 0.02f;
										}

										float finalInteractionRadius = totalInteractionRadius;

										if (modelScaleFactor < 0.99f)
										{
											// Multiplied inverse expansion tracking: fully scales up tiny bounds maps
											finalInteractionRadius /= (modelScaleFactor * modelScaleFactor);
										}

										if (radius < 8192.0f && radius > 0.0f)
										{
											float radiusExpansionFactor = 8192.0f / radius;
											if (radiusExpansionFactor > 64.0f) radiusExpansionFactor = 64.0f;
											finalInteractionRadius *= radiusExpansionFactor;
										}

										// Pack parameters straight into your global Gouraud structure array pool
										FLegacyModelLightCache item;
										item.x = (float)reldynlightpos.X; item.z = (float)reldynlightpos.Z; item.y = (float)reldynlightpos.Y;
										item.trueX = (float)absdynlightpos.X; item.trueZ = (float)absdynlightpos.Z; item.trueY = (float)absdynlightpos.Y;
										item.radius = finalInteractionRadius; item.r = r; item.g = g; item.b = b;
										g_legacyModelLights.Push(item); g_legacyLightActive = true;
										outR += r; outG += g; outB += b;
									}
								}
							}
						}
					}
					node = node->nextLight;
				}
			});
		}

		// Flush flat lighting fallback outputs to baseline engine buffers
		gl_RenderState.SetDynLight(outR / 4.0f, outG / 4.0f, outB / 4.0f);
		gl_RenderState.ResetColor();

		if (g_legacyLightActive && g_legacyModelLights.Size() > 0)
		{
			// Tell gl_20.cpp ApplyFixedFunction it's legacy VOLUMETRIC dynlight
			modellightindex = -2;
			return -2;
		}

		modellightindex = -1;
		return -1;
	}
	else
	{
		// ====================================================================================
		// GL3+ MODERN shader based path with unified all-format virtual injection
		// ====================================================================================
		modellightdata.Clear();
		if (self)
		{
			float modelX = (float)self->X(); float modelY = (float)self->Y();

			FSpriteModelFrame *smf = FindModelFrame(self->GetClass(), self->sprite, self->frame, false);
			float finalScaleX = (float)self->Scale.X;
			float finalScaleZ = (float)self->Scale.Y;
			if (smf != nullptr)
			{ finalScaleX *= smf->xscale; finalScaleZ *= smf->zscale; }
			if (finalScaleX <= 0.0f) finalScaleX = 1.0f; if (finalScaleZ <= 0.0f) finalScaleZ = 1.0f;
			float modelFloorZ = (float)self->Z() + (float)self->GetBobOffset() + (float)self->SpriteOffset.Y;

			// Extract frame-perfect unscaled height computed straight from the 3D vertex mesh constraints
			float trueVisualHeight = 0.0f; float trueVisualRadius = 0.0f;
			if (smf != nullptr && smf->modelIDs[0] != -1)
			{
				FModel *baseMdl = Models[smf->modelIDs[0]];
				int currentFrameNo = smf->modelframes[0];

				if (baseMdl != nullptr && currentFrameNo >= 0)
				{
					trueVisualHeight = baseMdl->GetTrueMDLVisualHeight(currentFrameNo, finalScaleZ);
					trueVisualRadius = baseMdl->GetTrueMDLVisualRadius(currentFrameNo, finalScaleX);
				}
			}

			if (trueVisualRadius <= 0.01f) trueVisualRadius = (float)self->RenderRadius() * (float)self->Scale.X;
			if (trueVisualHeight <= 0.0f)
			{
				trueVisualHeight = (float)self->Height * finalScaleZ;
				if (trueVisualHeight <= 2.0f) trueVisualHeight = trueVisualRadius * 2.0f;
			}

			if (trueVisualHeight <= 0.1f) trueVisualHeight = 6.0f; // Sync flat mesh clip envelope to 6.0 units

			float modelTopZ = modelFloorZ + trueVisualHeight;
			float searchRadius = trueVisualRadius + 512.0f;

			static std::vector<FDynamicLight*> addedLights;
			addedLights.clear();

			isActorVisibToDynlightMainWrapper(modelX, modelY, searchRadius, [&](subsector_t *subsector)
			{
				FLightNode * node = subsector->lighthead;
				while (node)
				{
					FDynamicLight *light = node->lightsource;
					if (light->ShouldLightActor(self))
					{
						int group = subsector->sector->PortalGroup;
						DVector3 reldynlightpos = light->PosRelative(group);
						float radius = light->GetRadius();

						if (radius > 0.0f)
						{
							float dx = reldynlightpos.X - (float)modelX;
							float dy = reldynlightpos.Y - (float)modelY;

							float closestZ = reldynlightpos.Z;
							if (closestZ < (float)modelFloorZ) closestZ = (float)modelFloorZ;
							if (closestZ > (float)modelTopZ)   closestZ = (float)modelTopZ;

							float dz = reldynlightpos.Z - closestZ;
							float distSquared = dx * dx + dy * dy + dz * dz;

							// Compress horizontal padding for flat sheets in GL3+ path too
							float boundsRadiusCushion = trueVisualRadius;
							if (trueVisualHeight <= 6.1f) boundsRadiusCushion = trueVisualRadius * 0.5f;

							float totalInteractionRadius = radius + boundsRadiusCushion;

							if (distSquared < (float)(totalInteractionRadius * totalInteractionRadius))
							{
								if (std::find(addedLights.begin(), addedLights.end(), light) == addedLights.end())
								{
									gl_AddLightToList(group, light, modellightdata);
									addedLights.push_back(light);
								}
							}
						}
					}
					node = node->nextLight;
				}
			});
		}
	}

	dynlightindex = GLRenderer->mLights->UploadLights(modellightdata);

	if (gl.lightmethod != LM_DEFERRED)
	{
		gl_RenderState.SetDynLight(0, 0, 0);
		modellightindex = dynlightindex;
	}

	return dynlightindex;
}



// Original function we use on models if usegl1volumedynlight flag is not set for the actor modeldef
int gl_SetDynModelLightFlat(AActor *self, int dynlightindex)
{
	// For deferred light mode this function gets called twice. First time for list upload, and second for draw.
	if (gl.lightmethod == LM_DEFERRED && dynlightindex != -1)
	{
		gl_RenderState.SetDynLight(0, 0, 0);
		modellightindex = dynlightindex;
		return dynlightindex;
	}

	g_CurrentRenderingActorPtr = (void*)self; // no need to add "AActor *actor" in RenderFrame

	// Legacy render path gets the old flat model light
	if (gl.lightmethod == LM_LEGACY)
	{
		gl_SetDynSpriteLight(self, nullptr);
		return -1;
	}

	modellightdata.Clear();

	if (self)
	{
		static std::vector<FDynamicLight*> addedLights; // static so that we build up a reserve (memory allocations stop)

		addedLights.clear();

		float x = (float)self->X();
		float y = (float)self->Y();
		float z = (float)self->Center();
		float actorradius = (float)self->RenderRadius();
		float radiusSquared = actorradius * actorradius;

		// Iterate through all subsectors potentially touched by actor
		isActorVisibToDynlightMainWrapper(x, y, radiusSquared, [&](subsector_t *subsector)
		{
			FLightNode * node = subsector->lighthead;
			while (node) // check all lights touching a subsector
			{
				FDynamicLight *light = node->lightsource;
				if (light->ShouldLightActor(self))
				{
					int group = subsector->sector->PortalGroup;
					DVector3 pos = light->PosRelative(group);
					float radius = light->GetRadius() + actorradius;
					float dx = pos.X - x; float dy = pos.Y - y; float dz = pos.Z - z;
					float distSquared = dx * dx + dy * dy + dz * dz;
					if (distSquared < radius * radius) // Light and actor touches
					{
						// Check if we already added this light from a different subsector
						if (std::find(addedLights.begin(), addedLights.end(), light) == addedLights.end())
						{
							gl_AddLightToList(group, light, modellightdata);
							addedLights.push_back(light);
						}
					}
				}
				node = node->nextLight;
			}
		});
	}

	dynlightindex = GLRenderer->mLights->UploadLights(modellightdata);

	if (gl.lightmethod != LM_DEFERRED)
	{
		gl_RenderState.SetDynLight(0, 0, 0);
		modellightindex = dynlightindex;
	}
	return dynlightindex;
}



int gl_SetDynModelLight(AActor *self, int dynlightindex) // main wrapper function to call
{
	// High-speed legacy render path router
	if (gl.lightmethod == LM_LEGACY && self != nullptr)
	{
		// Fetch the active frame model property without layout overhead
		FSpriteModelFrame *smf = FindModelFrame(self->GetClass(), self->sprite, self->frame, false);
		bool usegl1volumedynlight = (smf != nullptr && (smf->flags & MDL_USEGL1VOLUMEDYNLIGHT) != 0);

		// If explicit flag is present or the core architecture marks mesh topology as solid
		if (usegl1volumedynlight)
		{
			// Volumetric dynamic light on 3D models
			return gl_SetDynModelLightVolumetricHuge(self, dynlightindex);
		}
		else
		{
			// Flat geometry for which "usegl1volumedynlight" modeldef flag isn't set
			return gl_SetDynModelLightFlat(self, dynlightindex);
		}
	}

	// Modern GL3+ core layout strategy fallback
	modellightdata.Clear();
	dynlightindex = GLRenderer->mLights->UploadLights(modellightdata);
	if (gl.lightmethod != LM_DEFERRED)
	{
		gl_RenderState.SetDynLight(0, 0, 0);
		modellightindex = dynlightindex;
		return dynlightindex;
	}
	return dynlightindex;
}


//                           -----------------------
//     -------------=========================================------------------------------
// ================   DYNAMIC LIGHT 3D-MODEL OCCLUSION START   =================================
//     -------------=========================================------------------------------
//                           -----------------------


