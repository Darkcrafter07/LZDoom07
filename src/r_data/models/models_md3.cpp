// models_md3.cpp
//---------------------------------------------------------------------------
//
// Copyright(C) 2006-2016 Christoph Oelckers
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

#include "w_wad.h"
#include "cmdlib.h"
#include "sc_man.h"
#include "m_crc32.h"
#include "r_data/models/models.h"

#include "gl/system/gl_system.h"
#include "gl/system/gl_interface.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/models/gl_models.h"
#include "gl/textures/gl_material.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/shaders/gl_shader.h"
#include "gl/compatibility/gl_20.h"
#include "gl/dynlights/gl_dynlight.h"

#include "gl/dynlights/gl_dynlightcache.h"

extern void* g_CurrentRendering3dmdlActorPtr;         // no need to add "AActor *actor" in RenderFrame


#define MAX_QPATH 64

#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

//CVAR(Float, gl_gl1dynlightoffset, 0.0f, CVAR_ARCHIVE)   // Intensity multiplier for walls

//===========================================================================
//
// decode the lat/lng normal to a 3 float normal
//
//===========================================================================

static void UnpackVector(unsigned short packed, float & nx, float & ny, float & nz)
{
	double lat = ( packed >> 8 ) & 0xff;
	double lng = ( packed & 0xff );
	lat *= M_PI/128;
	lng *= M_PI/128;

	nx = cos(lat) * sin(lng);
	ny = sin(lat) * sin(lng);
	nz = cos(lng);
}

//===========================================================================
//
// MD3 File structure
//
//===========================================================================

#pragma pack(4)
struct md3_header_t
{
	uint32_t Magic;
	uint32_t Version;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Tags;
	uint32_t Num_Surfaces;
	uint32_t Num_Skins;
	uint32_t Ofs_Frames;
	uint32_t Ofs_Tags;
	uint32_t Ofs_Surfaces;
	uint32_t Ofs_Eof;
};

struct md3_surface_t
{
	uint32_t Magic;
	char Name[MAX_QPATH];
	uint32_t Flags;
	uint32_t Num_Frames;
	uint32_t Num_Shaders;
	uint32_t Num_Verts;
	uint32_t Num_Triangles;
	uint32_t Ofs_Triangles;
	uint32_t Ofs_Shaders;
	uint32_t Ofs_Texcoord;
	uint32_t Ofs_XYZNormal;
	uint32_t Ofs_End;
};

struct md3_triangle_t
{
	uint32_t vt_index[3];
};

struct md3_shader_t
{
	char Name[MAX_QPATH];
	uint32_t index;
};

struct md3_texcoord_t
{
	float s, t;
};

struct md3_vertex_t
{
	short x, y, z, n;
};

struct md3_frame_t
{
	float min_Bounds[3];
	float max_Bounds[3];
	float localorigin[3];
	float radius;
	char Name[16];
};
#pragma pack()


//===========================================================================
//
//
//
//===========================================================================

bool FMD3Model::Load(const char * path, int lumpnum, const char * buffer, int length)
{
	md3_header_t * hdr = (md3_header_t *)buffer;

	auto numFrames = LittleLong(hdr->Num_Frames);
	auto numSurfaces = LittleLong(hdr->Num_Surfaces);
	
	numTags = LittleLong(hdr->Num_Tags);

	md3_frame_t * frm = (md3_frame_t*)(buffer + LittleLong(hdr->Ofs_Frames));

	Frames.Resize(numFrames);
	for (unsigned i = 0; i < numFrames; i++)
	{
		strncpy(Frames[i].Name, frm[i].Name, 16);
		for (int j = 0; j < 3; j++) Frames[i].origin[j] = frm[i].localorigin[j];
	}

	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	Surfaces.Resize(numSurfaces);

	for (unsigned i = 0; i < numSurfaces; i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		s->numSkins = LittleLong(ss->Num_Shaders);
		s->numTriangles = LittleLong(ss->Num_Triangles);
		s->numVertices = LittleLong(ss->Num_Verts);

		// copy shaders (skins)
		md3_shader_t * shader = (md3_shader_t*)(((char*)ss) + LittleLong(ss->Ofs_Shaders));
		s->Skins.Resize(s->numSkins);

		for (unsigned i = 0; i < s->numSkins; i++)
		{
			// [BB] According to the MD3 spec, Name is supposed to include the full path.
			// ... and since some tools seem to output backslashes, these need to be replaced with forward slashes to work.
			FixPathSeperator(shader[i].Name);
			s->Skins[i] = LoadSkin("", shader[i].Name);
			// [BB] Fall back and check if Name is relative.
			if (!s->Skins[i].isValid())
				s->Skins[i] = LoadSkin(path, shader[i].Name);
		}
	}
	mLumpNum = lumpnum;
	return true;
}

//===========================================================================
//
//
//
//===========================================================================

void FMD3Model::LoadGeometry()
{
	FMemLump lumpdata = Wads.ReadLump(mLumpNum);
	const char *buffer = (const char *)lumpdata.GetMem();
	md3_header_t * hdr = (md3_header_t *)buffer;
	md3_surface_t * surf = (md3_surface_t*)(buffer + LittleLong(hdr->Ofs_Surfaces));

	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		MD3Surface * s = &Surfaces[i];
		md3_surface_t * ss = surf;

		surf = (md3_surface_t *)(((char*)surf) + LittleLong(surf->Ofs_End));

		// copy triangle indices
		md3_triangle_t * tris = (md3_triangle_t*)(((char*)ss) + LittleLong(ss->Ofs_Triangles));
		s->Tris.Resize(s->numTriangles);

		for (unsigned i = 0; i < s->numTriangles; i++) for (int j = 0; j < 3; j++)
		{
			s->Tris[i].VertIndex[j] = LittleLong(tris[i].vt_index[j]);
		}

		// Load texture coordinates
		md3_texcoord_t * tc = (md3_texcoord_t*)(((char*)ss) + LittleLong(ss->Ofs_Texcoord));
		s->Texcoords.Resize(s->numVertices);

		for (unsigned i = 0; i < s->numVertices; i++)
		{
			s->Texcoords[i].s = tc[i].s;
			s->Texcoords[i].t = tc[i].t;
		}

		// Load vertices and texture coordinates
		md3_vertex_t * vt = (md3_vertex_t*)(((char*)ss) + LittleLong(ss->Ofs_XYZNormal));
		s->Vertices.Resize(s->numVertices * Frames.Size());

		for (unsigned i = 0; i < s->numVertices * Frames.Size(); i++)
		{
			s->Vertices[i].x = LittleShort(vt[i].x) / 64.f;
			s->Vertices[i].y = LittleShort(vt[i].y) / 64.f;
			s->Vertices[i].z = LittleShort(vt[i].z) / 64.f;
			UnpackVector(LittleShort(vt[i].n), s->Vertices[i].nx, s->Vertices[i].ny, s->Vertices[i].nz);
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

//void FMD3Model::BuildVertexBuffer(FModelRenderer *renderer)
//{
//	if (!GetVertexBuffer(renderer))
//	{
//		LoadGeometry();
//
//		unsigned int vbufsize = 0;
//		unsigned int ibufsize = 0;
//
//		this->trueVisualRadii.Resize(Frames.Size());
//		for (unsigned f = 0; f < Frames.Size(); f++) this->trueVisualRadii[f] = 0.0f;
//
//		for (unsigned i = 0; i < Surfaces.Size(); i++)
//		{
//			MD3Surface * surf = &Surfaces[i];
//			vbufsize += Frames.Size() * surf->numVertices;
//			ibufsize += 3 * surf->numTriangles;
//		}
//
//		auto vbuf = renderer->CreateVertexBuffer(true, Frames.Size() == 1);
//		SetVertexBuffer(renderer, vbuf);
//
//		FModelVertex *vertptr = vbuf->LockVertexBuffer(vbufsize);
//		unsigned int *indxptr = vbuf->LockIndexBuffer(ibufsize);
//
//		assert(vertptr != nullptr && indxptr != nullptr);
//
//		unsigned int vindex = 0, iindex = 0;
//
//		// [Darkcrafter07]: Prepare individual frame bounds tracking vectors
//		this->trueVisualHeights.Resize(Frames.Size());
//		TArray<float> minBoundsPerFrame;
//		TArray<float> maxBoundsPerFrame;
//		minBoundsPerFrame.Resize(Frames.Size());
//		maxBoundsPerFrame.Resize(Frames.Size());
//
//		for (unsigned f = 0; f < Frames.Size(); f++)
//		{
//			minBoundsPerFrame[f] = 1000000.0f;  // Positive infinity seed
//			maxBoundsPerFrame[f] = -1000000.0f; // Negative infinity seed
//			this->trueVisualHeights[f] = 0.0f;
//		}
//
//		for (unsigned i = 0; i < Surfaces.Size(); i++)
//		{
//			MD3Surface * surf = &Surfaces[i];
//
//			surf->vindex = vindex;
//			surf->iindex = iindex;
//			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
//			{
//				MD3Vertex* vert = &surf->Vertices[j];
//
//				FModelVertex *bvert = &vertptr[vindex++];
//
//				int tc = j % surf->numVertices;
//				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);
//				bvert->SetNormal(vert->nx, vert->nz, vert->ny);
//
//				// [Darkcrafter07]: VERTEX-PERFECT HEIGHT SCANNER FOR EACH ANIMATION FRAME!
//				// Determine which specific frame index this vertex belongs to based on step sequences
//				if (surf->numVertices > 0)
//				{
//					unsigned int targetFrameIndex = j / surf->numVertices;
//					if (targetFrameIndex < Frames.Size())
//					{
//						if (bvert->y < minBoundsPerFrame[targetFrameIndex]) minBoundsPerFrame[targetFrameIndex] = bvert->y;
//						if (bvert->y > maxBoundsPerFrame[targetFrameIndex]) maxBoundsPerFrame[targetFrameIndex] = bvert->y;
//
//						// TRUE RADIUS VERTEX SCANNER FOR MD3!
//						// Calculate pure horizontal 2D vertex distance distance from the mesh pivot center
//						float horizDist = sqrtf((bvert->x * bvert->x) + (bvert->z * bvert->z));
//						if (horizDist > this->trueVisualRadii[targetFrameIndex]) this->trueVisualRadii[targetFrameIndex] = horizDist;
//					}
//				}
//			}
//
//			for (unsigned k = 0; k < surf->numTriangles; k++)
//			{
//				for (int l = 0; l < 3; l++)
//				{
//					indxptr[iindex++] = surf->Tris[k].VertIndex[l];
//				}
//			}
//
//			// for the mesh collision to work you must comment out the line like that!
//			//surf->UnloadGeometry();
//			surf->UnloadGeometry();
//		}
//
//		// [Darkcrafter07]: Lock down the exact visual height delta payload per animation frame step!
//		for (unsigned f = 0; f < Frames.Size(); f++)
//		{
//			float heightDelta = maxBoundsPerFrame[f] - minBoundsPerFrame[f];
//			if (heightDelta < 0.0f) heightDelta = 0.0f;
//			this->trueVisualHeights[f] = heightDelta;
//		}
//
//		// [Darkcrafter07]: Old-school 90s Engine Trick: 
//		// Mirror and cache all raw unscaled local vertex positions into our CPU shadow pool!
//		// This completely eliminates the NULL VMO pointer read crashes during real-time loops.
//		this->rawPositionsPool.Resize(vbufsize);
//		for (unsigned int v = 0; v < vindex; v++)
//		{
//			this->rawPositionsPool[v].X = vertptr[v].x;
//			this->rawPositionsPool[v].Y = vertptr[v].y;
//			this->rawPositionsPool[v].Z = vertptr[v].z;
//		}
//
//		vbuf->UnlockVertexBuffer();
//		vbuf->UnlockIndexBuffer();
//	}
//}




extern int modellightindex;
extern int g_legacyModelSectorLight;
extern bool g_legacyLightActive;
extern TArray<FLegacyDynlight3DmdlCache> g_legacyModelLights;
bool isUsingVolumetric3DModelLegacyDynlight = false;  // for gl_20.cpp ApplyFixedFunction
bool g_isCurrent3dMdlWeldedSolid = false;             // store current model computed topology weight

struct FTriangleSortMeta
{
	FMD3Model::MD3Triangle originalTriangle;
	float avgZ;
	float ccwAngle;
};

void FMD3Model::BuildVertexBuffer(FModelRenderer *renderer)
{
	if (!GetVertexBuffer(renderer))
	{
		LoadGeometry();

		unsigned int vbufsize = 0;
		unsigned int ibufsize = 0;

		this->trueVisualRadii.Resize(Frames.Size());
		for (unsigned f = 0; f < Frames.Size(); f++) this->trueVisualRadii[f] = 0.0f;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];
			vbufsize += Frames.Size() * surf->numVertices;
			ibufsize += 3 * surf->numTriangles;
		}

		auto vbuf = renderer->CreateVertexBuffer(true, Frames.Size() == 1);
		SetVertexBuffer(renderer, vbuf);

		FModelVertex *vertptr = vbuf->LockVertexBuffer(vbufsize);
		unsigned int *indxptr = vbuf->LockIndexBuffer(ibufsize);

		assert(vertptr != nullptr && indxptr != nullptr);

		unsigned int vindex = 0, iindex = 0;

		this->trueVisualHeights.Resize(Frames.Size());
		TArray<float> minBoundsPerFrame;
		TArray<float> maxBoundsPerFrame;
		minBoundsPerFrame.Resize(Frames.Size());
		maxBoundsPerFrame.Resize(Frames.Size());

		for (unsigned f = 0; f < Frames.Size(); f++)
		{
			minBoundsPerFrame[f] = 1000000.0f;
			maxBoundsPerFrame[f] = -1000000.0f;
			this->trueVisualHeights[f] = 0.0f;
		}

		// ==============================================================================
		// [Darkcrafter07]: DIFFERENTIAL LINE FLEXURE CURVATURE SHIELD!
		// ISOLATED PASS: We scan all surfaces up to infinity BEFORE Graf's factory packaging loops.
		// Measures the deflection delta of normal line vectors between adjacent vertices in RAM.
		// Completely immunizes the pipeline from any "surf - is undefined" compiler errors!
		// ==============================================================================
		bool modelContainsSharp3DCurvatureBends = false;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];
			surf->vindex = vindex;
			surf->iindex = iindex;

			unsigned int totalNormalEntries = Frames.Size() * surf->numVertices;
			TArray<float> accumNx; accumNx.Resize(totalNormalEntries);
			TArray<float> accumNy; accumNy.Resize(totalNormalEntries);
			TArray<float> accumNz; accumNz.Resize(totalNormalEntries);

			for (unsigned int v = 0; v < totalNormalEntries; v++)
			{
				accumNx[v] = 0.0f; accumNy[v] = 0.0f; accumNz[v] = 0.0f;
			}

			// Pass 1: Compute true 3D Cross Product face normals for EVERY single frame individually
			for (unsigned int f = 0; f < Frames.Size(); f++)
			{
				// --- FIX: FIXED TYPO AND SCOPE DEFINITION FOR FRAME VERTEX OFFSET ---
				int frameVertexOffset = f * surf->numVertices;

				for (unsigned int k = 0; k < surf->numTriangles; k++)
				{
					// --- FIX: RESTORED EXACT ARRAY INDEXATION SQUARE BRACKETS ---
					unsigned int i0 = surf->Tris[k].VertIndex[0];
					unsigned int i1 = surf->Tris[k].VertIndex[1];
					unsigned int i2 = surf->Tris[k].VertIndex[2];

					if (i0 < surf->numVertices && i1 < surf->numVertices && i2 < surf->numVertices)
					{
						// Fetch real dynamic vertex arrays mapping active animation frames natively
						MD3Vertex* vert0 = &surf->Vertices[frameVertexOffset + i0];
						MD3Vertex* vert1 = &surf->Vertices[frameVertexOffset + i1];
						MD3Vertex* vert2 = &surf->Vertices[frameVertexOffset + i2];

						float edge1X = vert1->x - vert0->x; float edge1Y = vert1->z - vert0->z; float edge1Z = vert1->y - vert0->y;
						float edge2X = vert2->x - vert0->x; float edge2Y = vert2->z - vert0->z; float edge2Z = vert2->y - vert0->y;

						// Faithful SimSun cross-product face normal formula (Edge1 x Edge2)
						float fnx = (edge1Y * edge2Z) - (edge1Z * edge2Y);
						float fny = (edge1Z * edge2X) - (edge1X * edge2Z);
						float fnz = (edge1X * edge2Y) - (edge1Y * edge2X);

						// Accumulate smooth vector weights into vertex specific frame slots
						accumNx[frameVertexOffset + i0] += fnx; accumNy[frameVertexOffset + i0] += fny; accumNz[frameVertexOffset + i0] += fnz;
						accumNx[frameVertexOffset + i1] += fnx; accumNy[frameVertexOffset + i1] += fny; accumNz[frameVertexOffset + i1] += fnz;
						accumNx[frameVertexOffset + i2] += fnx; accumNy[frameVertexOffset + i2] += fny; accumNz[frameVertexOffset + i2] += fnz;
					}
				}
			}

			// Pass 2: Normalize and pre-bake faithful 3D SimSun Sunlit weights into the vertex VBO arrays
			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
			{
				MD3Vertex* vert = &surf->Vertices[j];
				FModelVertex *bvert = &vertptr[vindex++];
				int tc = j % surf->numVertices;

				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);

				// Get accumulated smoothed-put true 3D-normal for the current vertex frame entry
				float finalNx = accumNx[j];
				float finalNy = accumNy[j];
				float finalNz = accumNz[j];

				float normLen = sqrtf((finalNx * finalNx) + (finalNy * finalNy) + (finalNz * finalNz));
				if (normLen > 0.001f) { finalNx /= normLen; finalNy /= normLen; finalNz /= normLen; }
				else { finalNx = 0.0f; finalNy = 0.0f; finalNz = 1.0f; }

				float sunDirX = 1.0f; float sunDirY = -1.0f; float sunDirZ = -1.0f;
				float sunLen = sqrtf(sunDirX * sunDirX + sunDirY * sunDirY + sunDirZ * sunDirZ);
				float invSunLen = (sunLen > 0.001f) ? (1.0f / sunLen) : 1.0f;

				// Compute pure spatial scalar dot product alignment relative to sun directional arrays
				float dotProduct = (sunDirX * finalNx + sunDirY * finalNy + sunDirZ * finalNz) * invSunLen;
				if (dotProduct > 1.0f)  dotProduct = 1.0f;
				if (dotProduct < -1.0f) dotProduct = -1.0f;

				float angle = acosf(dotProduct);
				float lightLevel = angle * (1.0f / 3.14159265f); // pi inverse multiplication!

				// Faithful SimSun brightness adjustment shift (+0.32) to secure soft mid-tones
				lightLevel += 0.32f;
				if (lightLevel > 1.0f) lightLevel = 1.0f;
				if (lightLevel < 0.05f) lightLevel = 0.05f;

				// Bake the compiled SimSun sunlit scale factor right inside the VBO normal coordinates vector,
				// accounting for Quake/Doom axes flips
				bvert->SetNormal(finalNx * lightLevel, finalNz * lightLevel, finalNy * lightLevel);

				if (surf->numVertices > 0)
				{
					unsigned int targetFrameIndex = j / surf->numVertices;
					if (targetFrameIndex < Frames.Size())
					{
						if (bvert->y < minBoundsPerFrame[targetFrameIndex]) minBoundsPerFrame[targetFrameIndex] = bvert->y;
						if (bvert->y > maxBoundsPerFrame[targetFrameIndex]) maxBoundsPerFrame[targetFrameIndex] = bvert->y;
						float horizDist = sqrtf((bvert->x * bvert->x) + (bvert->z * bvert->z));
						if (horizDist > this->trueVisualRadii[targetFrameIndex]) this->trueVisualRadii[targetFrameIndex] = horizDist;
					}
				}
			}

			for (unsigned k = 0; k < surf->numTriangles; k++)
			{
				for (int l = 0; l < 3; l++) indxptr[iindex++] = surf->Tris[k].VertIndex[l];
			}
		}

		for (unsigned f = 0; f < Frames.Size(); f++)
		{
			float heightDelta = maxBoundsPerFrame[f] - minBoundsPerFrame[f];
			if (heightDelta < 0.0f) heightDelta = 0.0f;
			this->trueVisualHeights[f] = heightDelta;
		}

		this->rawPositionsPool.Resize(vbufsize);
		for (unsigned int v = 0; v < vindex; v++)
		{
			this->rawPositionsPool[v].X = vertptr[v].x;
			this->rawPositionsPool[v].Y = vertptr[v].y;
			this->rawPositionsPool[v].Z = vertptr[v].z;
		}

		vbuf->UnlockVertexBuffer();
		vbuf->UnlockIndexBuffer();
	}
}

//===========================================================================
//
// for skin precaching
//
//===========================================================================

void FMD3Model::AddSkins(uint8_t *hitlist)
{
	for (unsigned i = 0; i < Surfaces.Size(); i++)
	{
		if (curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][i].isValid())
		{
			hitlist[curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][i].GetIndex()] |= FTextureManager::HIT_Flat;
		}

		MD3Surface * surf = &Surfaces[i];
		for (unsigned j = 0; j < surf->numSkins; j++)
		{
			if (surf->Skins[j].isValid())
			{
				hitlist[surf->Skins[j].GetIndex()] |= FTextureManager::HIT_Flat;
			}
		}
	}
}

//===========================================================================
//
//
//
//===========================================================================

int FMD3Model::FindFrame(const char * name)
{
	for (unsigned i = 0; i < Frames.Size(); i++)
	{
		if (!stricmp(name, Frames[i].Name)) return i;
	}
	return -1;
}

//===========================================================================
//
//
//
//===========================================================================

//void FMD3Model::RenderFrame(FModelRenderer *renderer, FTexture * skin, int frameno, int frameno2, double inter, int translation)
//{
//	if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;
//
//	renderer->SetInterpolation(inter);
//	for (unsigned i = 0; i < Surfaces.Size(); i++)
//	{
//		MD3Surface * surf = &Surfaces[i];
//
//		// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
//		// Note: Each surface may have a different skin.
//		FTexture *surfaceSkin = skin;
//		if (!surfaceSkin)
//		{
//			if (curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][i].isValid())
//			{
//				surfaceSkin = TexMan(curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][i]);
//			}
//			else if (surf->numSkins > 0 && surf->Skins[0].isValid())
//			{
//				surfaceSkin = TexMan(surf->Skins[0]);
//			}
//
//			if (!surfaceSkin)
//			{
//				continue;
//			}
//		}
//
//		renderer->SetMaterial(surfaceSkin, false, translation);
//		GetVertexBuffer(renderer)->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);
//		renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
//	}
//	renderer->SetInterpolation(0.f);
//}






// ====================================================================================
// --- [Darkcrafter07]: GOURAUD SHADING & VERTEX LIGHTING MANIFESTO ---
//
// HISTORICAL FIX LOG: Why native hardware 3D volumetric light shaded absolutely FLAT?
// 1. THE ROOT OF THE PROBLEM: In LZDoom 07's legacy compatibility pipeline (GL1x/GL2x),
//    the core state-manager explicitly enforces 'glDisableClientState(GL_NORMAL_ARRAY)'
//    inside the FFlatVertexBuffer and FModelVertexBuffer binding passes. It assumed that 
//    stepped software subsector lightlevels required no normal tracking vectors.
// 2. THE HARDWARE BLINDSPOT: Because the driver client normal arrays were forced dead,
//    the GPU hardware fixed-function pipeline was completely blind to smoothly averaged
//    curvature normals baked inside 'bvert->SetNormal(finalNx, finalNz, finalNy)'
//    during the load-time BuildVertexBuffer() pass. The driver fell back to a default vector 
//    of (0,0,1) pointing straight up, wiping out all 3D depth and making the dynamic spots flat.
//
// Know how:
// By overriding the pipeline locks directly inside the mesh dispatch loop and force-activating 
// 'glEnableClientState(GL_NORMAL_ARRAY);', we ripped off the blindfold and forced the GPU
// to see the pre-calculated geometric vertex curvature deltas. Combined with smooth shading,
// the GPU instantly fired up its internal Gouraud Shading processors.
// ====================================================================================

void FMD3Model::RenderFrame(FModelRenderer *renderer, FTexture * skin, int frameno, int frameno2, double inter, int translation)
{
	// ==============================================================================
	// GL1x/GL2x legacy mode
	// ==============================================================================
	if (gl.legacyMode)
	{
		if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;
		renderer->SetInterpolation(inter);

		const float invMul127 = 1.0f / 127.0f;
		const float invMul255 = 1.0f / 255.0f;
		float baseColor = (float)g_legacyModelSectorLight * invMul255;
		AActor* actor = (AActor*)g_CurrentRendering3dmdlActorPtr; // no need to add "AActor *actor" in RenderFrame

		if (!gl_lights) // Dynligths are off, clear array cache and shutdown surfaces
		// needs to be done, otherwise actors won't kill volumen dynlight when turned off in menu
		{
			g_legacyLightActive = false; g_legacyModelLights.Clear();
		}

		if (!actor)
		{
			glColor4f(baseColor, baseColor, baseColor, 1.0f);
			for (unsigned m = 0; m < Surfaces.Size(); m++)
			{
				MD3Surface * surf = &Surfaces[m];
				renderer->SetMaterial(skin, false, translation);
				GetVertexBuffer(renderer)->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);
				renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
			}
			renderer->SetInterpolation(0.f);
			return;
		}

		glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

		for (unsigned m = 0; m < Surfaces.Size(); m++)
		{
			MD3Surface * surf = &Surfaces[m];
			FTexture *surfaceSkin = skin;
			if (!surfaceSkin)
			{
				if (curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][m].isValid())
					surfaceSkin = TexMan(curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][m]);
				else if (surf->numSkins > 0 && surf->Skins.Size() > 0)
					surfaceSkin = TexMan(surf->Skins[0]);

				if (!surfaceSkin) continue;
			}

			renderer->SetMaterial(surfaceSkin, false, translation);

			auto currentVBuf = (FModelVertexBuffer*)GetVertexBuffer(renderer);
			if (currentVBuf != nullptr)
			{
				currentVBuf->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);
			}

			// Repeat local check on empty memory of surface dynlights
			if (g_legacyModelLights.Size() == 0) g_legacyLightActive = false;

			bool isModeldefUseVolLegacyDynlightFlagSet = ((curSpriteMDLFrame->flags & MDL_USEGL1VOLUMEDYNLIGHT) != 0);

			// ==============================================================================
			// PATHWAY A: VOLUMETRIC DYNLIGHT WITH GOURAD SHADING
			// ==============================================================================
			// Do volumetric lighting if lights are active and triangles exist
			//bool useLegacyVolumetricLighting = (gl_lights && g_legacyLightActive && g_isCurrent3dMdlWeldedSolid && 
			//	                    surf->numTriangles > 0 && isModeldefUseVolLegacyDynlightFlagSet && 
			//															g_legacyModelLights.Size() > 0);
			bool useLegacyVolumetricLighting = (gl_lights && g_legacyLightActive &&
				surf->numTriangles > 0 && isModeldefUseVolLegacyDynlightFlagSet &&
				g_legacyModelLights.Size() > 0);
			if (useLegacyVolumetricLighting)
			{
				isUsingVolumetric3DModelLegacyDynlight = true;
				// Fetch the active model matrix from render state cache
				auto *modelMatrixPtr = &gl_RenderState.mModelMatrix;

				// --- COMPOSITE MATRIX SCALE EQUALIZER ---
				// Extract the map editor scale bound with MODELDEF multipliers layout
				float finalScaleX = (float)actor->Scale.X * (float)curSpriteMDLFrame->xscale;
				float finalScaleZ = (float)actor->Scale.Y * (float)curSpriteMDLFrame->zscale;
				if (finalScaleX <= 0.0f) finalScaleX = 1.0f;
				if (finalScaleZ <= 0.0f) finalScaleZ = 1.0f;

				// --- SCALE NORMALIZER ---
				// Measure average scaling factor to elegantly adapt attenuation properties.
				float avgModelScale = (finalScaleX + finalScaleZ) * 0.5f;
				if (avgModelScale <= 0.001f) avgModelScale = 1.0f;

				float referenceRockScale = 368.0f;
				float scaleNormalizeFactor = referenceRockScale / finalScaleZ;
				float trueVisualHeight = this->GetTrueMDLVisualHeight(frameno, finalScaleZ);
				float trueVisualRadius = this->GetTrueMDLVisualRadius(frameno, finalScaleX);
				if (trueVisualRadius <= 0.01f) trueVisualRadius = 32.0f;
				if (trueVisualHeight <= 0.01f) trueVisualHeight = 64.0f;

				const float legacyVolumDynlightIntensity = 0.5f;

				// Safe global registers light slots scope tracking variable
				unsigned int maxLightsToBind = g_legacyModelLights.Size();
				if (maxLightsToBind > 8) maxLightsToBind = 8; // OpenGL fixed function strict layout limits

				if (currentVBuf != nullptr)
				{
					// --- STEP 1: INITIALIZE NATIVE VERTEX STREAM INTERPOLATION ---
					auto* vertexBuffer = GetVertexBuffer(renderer);
					vertexBuffer->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);

					glShadeModel(GL_SMOOTH);
					glEnable(GL_LIGHTING); // Wake up hardware lighting engines

					// --- FORCE UNIT LENGTH NORMALS ---
					// Automatically recalculate model compressed or stretched normals on the fly
					glEnable(GL_NORMALIZE);

					glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE); // Force two-sided math to secure all faces
					glDisableClientState(GL_NORMAL_ARRAY); // Shut down the corrupted pointer stream

					// Inject a baseline exterior normal to reset the fixed function driver pipeline registers
					glNormal3f(0.0f, 0.0f, 1.0f);

					float baseAmbR = actor->Sector->lightlevel * invMul127;
					float baseAmbG = actor->Sector->lightlevel * invMul127;
					float baseAmbB = actor->Sector->lightlevel * invMul127;

					for (unsigned int l = 0; l < g_legacyModelLights.Size(); ++l)
					{
						FLegacyDynlight3DmdlCache &light = g_legacyModelLights[l];
						// Inject a smooth 1% global bounce light bleed from all active flares in space
						baseAmbR += light.r * legacyVolumDynlightIntensity * 0.01f;
						baseAmbG += light.g * legacyVolumDynlightIntensity * 0.01f;
						baseAmbB += light.b * legacyVolumDynlightIntensity * 0.01f;
					}
					if (baseAmbR > 1.0f) baseAmbR = 1.0f;
					if (baseAmbG > 1.0f) baseAmbG = 1.0f;
					if (baseAmbB > 1.0f) baseAmbB = 1.0f;

					// --- EXACT ARRAY INDEXATION SQUARE BRACKETS MUST BE HERE [4] ---
					float ambColor[4] = { baseAmbR, baseAmbG, baseAmbB, 1.0f };
					glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambColor);

					// Compute inverse model matrix to cancel out OpenGL Eye Space double-multiplication bugs
					VSMatrix inverseModelMatrix;
					inverseModelMatrix = *modelMatrixPtr;
					if (!modelMatrixPtr->inverseMatrix(inverseModelMatrix))
					{
						inverseModelMatrix.loadIdentity(); // Fallback to identity if inversion fails
					}

					DVector3 modelPos = actor->Pos();

					// --- VERTICAL HEIGHT SCALED PROPERLY BEFORE MATRIX ASSIGNMENT ---
					float midZ = (float)modelPos.Z + (trueVisualHeight * 0.5f);

					// --- STEP 3: BIND CACHED FLARES TO STANDARD HARDWARE LIGHT SLOTS ---
					for (unsigned int l = 0; l < maxLightsToBind; ++l)
					{
						FLegacyDynlight3DmdlCache &cachedLight = g_legacyModelLights[l];
						GLenum lightSlot = GL_LIGHT0 + l;
						glEnable(lightSlot);

						// Setup raw world position vectors of the light source
						FLOATTYPE worldLightPos[4] = {
							(FLOATTYPE)cachedLight.absX,
							(FLOATTYPE)cachedLight.absZ,
							(FLOATTYPE)cachedLight.absY,
							1.0f
						};
						FLOATTYPE localLightPos[4] = { 0 };

						// Multiply world parameters by inverse matrix using native multMatrixPoint pointer route
						inverseModelMatrix.multMatrixPoint(worldLightPos, localLightPos);

						// --- EXACT ARRAY INDEXATION SQUARE BRACKETS MUST BE HERE [4], [0], [1], [2] ---
						float lightPos[4] = { (float)localLightPos[0], (float)localLightPos[1], (float)localLightPos[2], 1.0f };

						// --- HIGH-SPEED STRIDE VERTEX SAMPLER SHUNT ---
						float closestVertexWorld[3] = { (float)modelPos.X, (float)modelPos.Y, (float)midZ };
						float minVertexDistSquared = 99999999.0f;
						int totalVerts = surf->numVertices;

						if (totalVerts > 0 && surf->Vertices.Size() > 0)
						{
							int frameVertexOffset = frameno * totalVerts;
							int strideStep = totalVerts / 8;
							if (strideStep < 1) strideStep = 1;

							// Cache alignment pointer stream shortcut
							const MD3Vertex* verticesBase = &surf->Vertices[frameVertexOffset];
							for (int vIdx = 0; vIdx < totalVerts; vIdx += strideStep)
							{
								const MD3Vertex* testVert = &verticesBase[vIdx];

								// --- NATIVE 3D COORDINATE CONVERSION SYNCED WITH SCALES ---
								float vWorldX = (float)modelPos.X + (testVert->x * finalScaleX);
								float vWorldY = (float)modelPos.Y + (testVert->z * finalScaleX);
								float vWorldZ = (float)modelPos.Z + (testVert->y * finalScaleZ);

								float vDx = (float)cachedLight.absX - vWorldX;
								float vDy = (float)cachedLight.absY - vWorldY;
								float vDz = (float)cachedLight.absZ - vWorldZ;
								float tDistSq = vDx * vDx + vDy * vDy + vDz * vDz;

								if (tDistSq < minVertexDistSquared)
								{
									minVertexDistSquared = tDistSq;
									closestVertexWorld[0] = vWorldX;
									closestVertexWorld[1] = vWorldY;
									closestVertexWorld[2] = vWorldZ;
								}
							}
						}

						// --- CLEAN SCALED DISTANCE CALCULATION ---
						float dx = ((float)cachedLight.absX - closestVertexWorld[0]) / avgModelScale;
						float dy = ((float)cachedLight.absY - closestVertexWorld[1]) / avgModelScale;
						float dz = ((float)cachedLight.absZ - closestVertexWorld[2]) / avgModelScale;
						float currentDist = sqrtf(dx * dx + dy * dy + dz * dz);

						float scaledRadius = cachedLight.radius * avgModelScale;
						if (scaledRadius <= 0.01f) scaledRadius = 1.0f;

						float distFactor = 1.0f - (currentDist / (scaledRadius * 0.75f));
						if (distFactor < 0.0f) distFactor = 0.0f;

						// --- EXACT ARRAY INDEXATION SQUARE BRACKETS MUST BE HERE [4] ---
						float diffuseColor[4] = {
							cachedLight.r * legacyVolumDynlightIntensity * distFactor * distFactor,
							cachedLight.g * legacyVolumDynlightIntensity * distFactor * distFactor,
							cachedLight.b * legacyVolumDynlightIntensity * distFactor * distFactor,
							1.0f
						};

						// --- EXACT ARRAY INDEXATION SQUARE BRACKETS MUST BE HERE [4] ---
						float zeroAmbient[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

						glLightfv(lightSlot, GL_POSITION, lightPos);
						glLightfv(lightSlot, GL_DIFFUSE, diffuseColor);
						glLightfv(lightSlot, GL_SPECULAR, diffuseColor);
						glLightfv(lightSlot, GL_AMBIENT, zeroAmbient);

						// --- CLEAN ATTENUATION INJECTOR ---
						glLightf(lightSlot, GL_CONSTANT_ATTENUATION, 0.0f);
						glLightf(lightSlot, GL_LINEAR_ATTENUATION, (1.0f / scaledRadius) * currentDist * 6.0f);
						glLightf(lightSlot, GL_QUADRATIC_ATTENUATION, 0.0f);
					}

					glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
					glEnable(GL_COLOR_MATERIAL); // Enable native color tracking
				}

				// Dispatch the geometry using 1 single factory VBO Draw Call
				renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));

				// --- STEP 4: RECOVERY CLEANUP RESTORE GATE ---
				if (currentVBuf != nullptr) currentVBuf->BindVBO();

				if (gl.legacyMode)
				{
					for (unsigned int l = 0; l < maxLightsToBind; ++l)
					{
						glDisable(GL_LIGHT0 + l);
					}

					glDisable(GL_LIGHTING);
					glDisable(GL_COLOR_SUM); // Safely shut down hardware color summary blending pass
					glDisableClientState(GL_NORMAL_ARRAY); // Safely isolate normal tracking back to default
					glDisable(GL_COLOR_MATERIAL);

					// Force immediate pipeline synchronization pass right now to flush cache registers cleanly
					// Commented lines are NOT needed here, otherwise some models get lit at all times - kept for history
					//gl_RenderState.EnableFog(true); // NOT needed perhaps
					//gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // NOT needed perhaps
					//gl_RenderState.SetTextureMode(TM_MODULATE); // NOT needed perhaps
					//gl_RenderState.ResetColor(); // NOT needed perhaps
					//gl_RenderState.Apply();
				}
				isUsingVolumetric3DModelLegacyDynlight = false;
			}
			else
			{
				// ==============================================================================
				// PATHWAY B: FLAT LIGHT NO-SLICE CONVEYOR ROUTE FOR FLAT/DECORATIVE MODELS
				// ==============================================================================
				isUsingVolumetric3DModelLegacyDynlight = false;
				// PATHWAY B CRASH FIX: If a model was previously rendered using Pathway A, 
				// GL_ELEMENT_ARRAY_BUFFER remains 0. Force-restore it now.
				if (currentVBuf != nullptr) currentVBuf->BindVBO();
				renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
			}
		}
	}
	else
	{
		// ==============================================================================
		// MODERN PATH: GL3+ ORIGINAL RENDERING STRATEGY
		// ==============================================================================
		if (gl_lights) isUsingVolumetric3DModelLegacyDynlight = false;
		if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;

		renderer->SetInterpolation(inter);
		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];

			// [BB] In case no skin is specified via MODELDEF, check if the MD3 has a skin for the current surface.
			// Note: Each surface may have a different skin.
			FTexture *surfaceSkin = skin;
			if (!surfaceSkin)
			{
				if (curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][i].isValid())
				{
					surfaceSkin = TexMan(curSpriteMDLFrame->surfaceskinIDs[curMDLIndex][i]);
				}
				else if (surf->numSkins > 0 && surf->Skins[0].isValid())
				{
					surfaceSkin = TexMan(surf->Skins[0]);
				}

				if (!surfaceSkin)
				{
					continue;
				}
			}

			renderer->SetMaterial(surfaceSkin, false, translation);
			GetVertexBuffer(renderer)->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);
			renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
		}
		renderer->SetInterpolation(0.f);
	}
}
