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

extern void* g_CurrentRenderingActorPtr;         // no need to add "AActor *actor" in RenderFrame


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
extern TArray<FLegacyModelLightCache> g_legacyModelLights;
bool isUsingVolumetric3DModelLegacyDynlight = false;  // for gl_20.cpp ApplyFixedFunction
bool g_currentModelIsWeldedSolid = false;             // store current model computed topology weight

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
			if (surf->numTriangles == 0 || surf->Vertices.Size() == 0 || surf->numVertices < 4) continue;

			// Spatial CCW Sorter Pass (Ring Index Builder)
			struct FTriangleSortMeta { MD3Triangle originalTriangle; float avgZ; float ccwAngle; };
			TArray<FTriangleSortMeta> sortingPool;
			sortingPool.Resize(surf->numTriangles);

			for (unsigned int k = 0; k < surf->numTriangles; k++)
			{
				sortingPool[k].originalTriangle = surf->Tris[k];
				float sumZ = 0.0f; float sumX = 0.0f; float sumY = 0.0f;
				for (int l = 0; l < 3; l++)
				{
					unsigned int idx = surf->Tris[k].VertIndex[l];
					if (idx < surf->numVertices)
					{
						sumX += surf->Vertices[idx].x; sumY += surf->Vertices[idx].y; sumZ += surf->Vertices[idx].z;
					}
				}
				sortingPool[k].avgZ = sumZ / 3.0f;
				sortingPool[k].ccwAngle = atan2f(sumY / 3.0f, sumX / 3.0f);
			}

			std::sort(sortingPool.Data(), sortingPool.Data() + surf->numTriangles,
				[](const FTriangleSortMeta& a, const FTriangleSortMeta& b) -> bool
			{
				if (fabsf(a.avgZ - b.avgZ) > 2.0f) return a.avgZ < b.avgZ;
				return a.ccwAngle < b.ccwAngle;
			});

			for (unsigned int k = 0; k < surf->numTriangles; k++) surf->Tris[k] = sortingPool[k].originalTriangle;

			// --- THE ADJACENT LINE PERPENDICULAR TRACKER PASS ---
			float prevPerpLineX = 0.0f; float prevPerpLineY = 0.0f; float prevPerpLineZ = 1.0f;
			bool firstLineCaptured = false;
			int off1 = 0; // Evaluate based on frame 0 baseline setup

			for (unsigned int v = 0; v < surf->numVertices - 1; v++)
			{
				float x1 = surf->Vertices[off1 + v].x; float y1 = surf->Vertices[off1 + v].y; float z1 = surf->Vertices[off1 + v].z;
				float x2 = surf->Vertices[off1 + v + 1].x; float y2 = surf->Vertices[off1 + v + 1].y; float z2 = surf->Vertices[off1 + v + 1].z;

				float edgeX = x2 - x1; float edgeY = y2 - y1; float edgeZ = z2 - z1;
				float edgeLen = sqrtf(edgeX*edgeX + edgeY * edgeY + edgeZ * edgeZ);
				if (edgeLen <= 0.001f) continue;

				// Extrude an explicit perpendicular cross-product line vector matching your layout!
				float perpLineX = edgeY;
				float perpLineY = -edgeX;
				float perpLineZ = 0.0f;

				if (fabsf(edgeX) < 0.01f && fabsf(edgeY) < 0.01f)
				{
					perpLineX = 1.0f; perpLineY = 0.0f; perpLineZ = 0.0f;
				}

				float perpLen = sqrtf(perpLineX*perpLineX + perpLineY * perpLineY + perpLineZ * perpLineZ);
				if (perpLen > 0.001f) { perpLineX /= perpLen; perpLineY /= perpLen; perpLineZ /= perpLen; }

				if (!firstLineCaptured)
				{
					prevPerpLineX = perpLineX; prevPerpLineY = perpLineY; prevPerpLineZ = perpLineZ;
					firstLineCaptured = true;
				}
				else
				{
					float lineDotProduct = (perpLineX * prevPerpLineX) + (perpLineY * prevPerpLineY) + (perpLineZ * prevPerpLineZ);

					// CRITICAL WRAPPING THRESHOLD GATE: cos(8 degrees) = ~0.9902f
					if (fabsf(lineDotProduct) < 0.990f)
					{
						modelContainsSharp3DCurvatureBends = true;
						break;
					}

					prevPerpLineX = perpLineX; prevPerpLineY = perpLineY; prevPerpLineZ = perpLineZ;
				}
			}
			if (modelContainsSharp3DCurvatureBends) break;
		}

		// Save the final unified single-verdict token natively
		g_currentModelIsWeldedSolid = modelContainsSharp3DCurvatureBends;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];

			surf->vindex = vindex;
			surf->iindex = iindex;

			// [Darkcrafter07]: PURE 3D SMOOTH NORMAL GENERATOR CONVEYOR
			// Create a temp CPU-buffer to accumulate and smooth faithful 3D normals
			TArray<float> accumNx; accumNx.Resize(surf->numVertices);
			TArray<float> accumNy; accumNy.Resize(surf->numVertices);
			TArray<float> accumNz; accumNz.Resize(surf->numVertices);
			for (unsigned int v = 0; v < surf->numVertices; v++) { accumNx[v] = 0.0f; accumNy[v] = 0.0f; accumNz[v] = 0.0f; }

			// Pass 1: Calc 3D cross-product for each triangle of current frame
			for (unsigned int k = 0; k < surf->numTriangles; k++)
			{
				unsigned int i0 = surf->Tris[k].VertIndex[0];
				unsigned int i1 = surf->Tris[k].VertIndex[1];
				unsigned int i2 = surf->Tris[k].VertIndex[2];

				if (i0 < surf->numVertices && i1 < surf->numVertices && i2 < surf->numVertices)
				{
					// Take baseline vertices of frame 0 to generate normals topology
					float v0x = surf->Vertices[i0].x; float v0y = surf->Vertices[i0].y; float v0z = surf->Vertices[i0].z;
					float v1x = surf->Vertices[i1].x; float v1y = surf->Vertices[i1].y; float v1z = surf->Vertices[i1].z;
					float v2x = surf->Vertices[i2].x; float v2y = surf->Vertices[i2].y; float v2z = surf->Vertices[i2].z;

					float edge1X = v1x - v0x; float edge1Y = v1y - v0y; float edge1Z = v1z - v0z;
					float edge2X = v2x - v0x; float edge2Y = v2y - v0y; float edge2Z = v2z - v0z;

					// Faithful volumetric cross-product for plane
					float fnx = (edge1Y * edge2Z) - (edge1Z * edge2Y);
					float fny = (edge1Z * edge2X) - (edge1X * edge2Z);
					float fnz = (edge1X * edge2Y) - (edge1Y * edge2X);

					// Accumulate (smooth) normals on the angles vertices
					accumNx[i0] += fnx; accumNy[i0] += fny; accumNz[i0] += fnz;
					accumNx[i1] += fnx; accumNy[i1] += fny; accumNz[i1] += fnz;
					accumNx[i2] += fnx; accumNy[i2] += fny; accumNz[i2] += fnz;
				}
			}

			// Pass 2: Normalize and pack faithful 3D-normals into the vertex VBO
			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
			{
				MD3Vertex* vert = &surf->Vertices[j];
				FModelVertex *bvert = &vertptr[vindex++];
				int tc = j % surf->numVertices;
				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);

				// Get accumulated smoothed-put 3D-normal for the current vertex
				float finalNx = accumNx[tc]; float finalNy = accumNy[tc]; float finalNz = accumNz[tc];
				float normLen = sqrtf((finalNx * finalNx) + (finalNy * finalNy) + (finalNz * finalNz));

				if (normLen > 0.001f) { finalNx /= normLen; finalNy /= normLen; finalNz /= normLen; }
				else { finalNx = 1.0f; finalNy = 0.0f; finalNz = 0.0f; }

				// Bake the volumetric normal into VBO accounting Quake/Doom axes flips
				bvert->SetNormal(finalNx, finalNz, finalNy);

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
			this->rawPositionsPool[v].X = vertptr[v].x; this->rawPositionsPool[v].Y = vertptr[v].y; this->rawPositionsPool[v].Z = vertptr[v].z;
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







static TArray<unsigned int> sliceIndicesPool[6]; // 6 completely independent and isolated 3D models in frame RAM
void FMD3Model::RenderFrame(FModelRenderer *renderer, FTexture * skin, int frameno, int frameno2, double inter, int translation)
{
	// ==============================================================================
	// GL1x/GL2x legacy mode
	// ==============================================================================
	if (gl.legacyMode)
	{
		if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;
		renderer->SetInterpolation(inter);

		const float inv255 = 1.0f / 255.0f;
		float baseColor = (float)g_legacyModelSectorLight * inv255;
		AActor* actor = (AActor*)g_CurrentRenderingActorPtr; // no need to add "AActor *actor" in RenderFrame

		if (!gl_lights) // Dynligths are off, clear array cache and shutdown surfaces
		// needs to be done, otherwise actors won't kill volumen dynlight when turned off in menu
		{ g_legacyLightActive = false; g_legacyModelLights.Clear(); }

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
			{ currentVBuf->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices); }

			// Repeat local check on empty memory of surface dynlights
			if (g_legacyModelLights.Size() == 0) g_legacyLightActive = false;

			bool isModeldefUseVolLegacyDynlightFlagSet = ((curSpriteMDLFrame->flags & MDL_USEGL1VOLUMEDYNLIGHT) != 0);

			// ==============================================================================
			// PATHWAY A: VOLUMETRIC DYNLIGHT 6-STAGE VERTICAL SLICER FOR SOLID MESHES
			// ==============================================================================
			// Evaluate condition: Only proceed with volumetric slicing if lights are active, 
			// triangles exist, and the model topology is confirmed as welded/solid.
			bool useSlicingLight = (gl_lights && g_legacyLightActive && g_currentModelIsWeldedSolid && 
				                    surf->numTriangles > 0 && isModeldefUseVolLegacyDynlightFlagSet && 
                                                                      g_legacyModelLights.Size() > 0);
			if (useSlicingLight)
			{
				isUsingVolumetric3DModelLegacyDynlight = true;
				const auto *mat = gl_RenderState.mModelMatrix.get();

				// Extract true model scales from the active render state
				float finalScaleX = (float)curSpriteMDLFrame->xscale;
				float finalScaleZ = (float)curSpriteMDLFrame->zscale;
				if (finalScaleX <= 0.0f) finalScaleX = 1.0f;
				if (finalScaleZ <= 0.0f) finalScaleZ = 1.0f;

				float referenceRockScale = 368.0f;
				float scaleNormalizeFactor = referenceRockScale / finalScaleZ;
				float trueVisualHeight = this->GetTrueMDLVisualHeight(frameno, finalScaleZ);
				float trueVisualRadius = this->GetTrueMDLVisualRadius(frameno, finalScaleX);
				if (trueVisualRadius <= 0.01f) trueVisualRadius = 32.0f;
				if (trueVisualHeight <= 0.01f) trueVisualHeight = 64.0f;

				// True actor coordinates on the map without camera distortion adjustments
				float worldActorX = actor->X(); float worldActorY = actor->Y(); float worldActorZ = actor->Z();
				float worldActorCenterZ = worldActorZ + ((float)trueVisualHeight * 0.5);

				// --- 1. ACCUMULATE TOTAL DIRECTION VECTOR TO DYNAMIC LIGHTS ---
				float sumDx = 0.0; float sumDy = 0.0; float sumDz = 0.0;
				int activeLightsCount = 0;

				for (unsigned int l = 0; l < g_legacyModelLights.Size(); l++)
				{
					FLegacyModelLightCache *light = &g_legacyModelLights[l];
					float dx = light->relX - (worldActorX * scaleNormalizeFactor);
					float dy = light->relY - (worldActorY * scaleNormalizeFactor);
					float dz = light->relZ - (worldActorCenterZ * scaleNormalizeFactor);

					float distSquared = dx * dx + dy * dy + dz * dz;
					float interactionRadius = (light->radius + trueVisualRadius) * scaleNormalizeFactor;

					if (distSquared < (float)(interactionRadius * interactionRadius))
					{
						sumDx += dx; sumDy += dy; sumDz += dz;
						activeLightsCount++;
					}
				}

				float wLdX = 0.0f; float wLdY = 0.0f; float wLdZ = 1.0f;
				if (activeLightsCount > 0)
				{
					float totalVectorLen = sqrt(sumDx*sumDx + sumDy * sumDy + sumDz * sumDz);
					if (totalVectorLen > 0.001)
					{ wLdX = sumDx / totalVectorLen; wLdY = sumDy / totalVectorLen; wLdZ = sumDz / totalVectorLen; }
				}

				// ==============================================================================
				// [Darkcrafter07]: THE ULTIMATE LEGACY OPENGL 1.1 COORD-TO-PHASE TRACKER
				// ==============================================================================
				// [HISTORY & ARCHITECTURE MANIFESTO]:
				// After a brutal 3-day war against Graf Zahl's fixed-function state locks, we 
				// discovered that monolith VBO buffers completely blind real-time index sorting
				// or CPU normal generation. Video drivers lock baked vertex arrays in place,
				// causing dynlight spots to freeze on a single static side of the model mesh.
				//
				// To shatter this lock, a "3D Spatial Compass Gate" was made. 
				// We bypassed Graf's broken relative coordinates (PortalGroup PosRelative data)
				// by hot-wiring raw, absolute world space vectors (PosAbsolute) from the engine's
				// FDynamicLight core directly into a custom expanded legacy light array cache.
				//
				// This module translates linear, absolute coordinate deltas into a flawless
				// full-amplitude polar wave phase [-1.0f .. 1.0f], completely immune to the 
				// bounding scale limits that previously blinded small-caliber cylinder meshes.
				//
				// [SMOOTHING]: 
				// To eliminate linear micro-stuttering, we adapted a classic 90s optimization 
				// blueprint inspired by Ken Silverman's Build Engine (Duke Nukem 3D 2048-cell sin_table).
				// By running the full-amplitude phase float through a continuous sinf() shaper, 
				// the light spot visually shifts from jagged linear tracking to an incredibly 
				// smooth, organic circular interpolation around the skin of any solid 3D asset.
				//
				// [THE GOLDEN LAWS - WHAT YOU CAN AND CANNOT DO]:
				// 1. CANNOT: Never divide deltas by trueVisualRadius again. It destroys small-mesh ranges.
				// 2. CANNOT: Never inject offsets inside intermediate Euler rotation stages (tx, ty, tz). 
				//    Trigonometric sinf/cosf loops will shred the phase multiplier into total noise.
				// 3. MUST DO: The final combined offset injection MUST be appended directly to the 
				//    very end of the terminal vector output channels (locLightDirX/Y/Z) after all matrix 
				//    transformations, perfectly "gl_gl1dynlightoffset" cvar shunt test
				// ==============================================================================
				float locLightDirX = wLdX; float locLightDirY = wLdY; float locLightDirZ = wLdZ;

				if (actor != nullptr && g_legacyModelLights.Size() > 0)
				{
					// Constants for inverse multiplication optimization
					const float inv180 = 1.0f / 180.0f; const float pi = 3.14159265f;
					const float invPi = 1.0f / pi; const float coef4096 = 1.0f / 4096.0f;

					// Virtual anchor forced projection: Forces 41-degree Yaw baseline to snap Quake MD3 vs Doom world axes
					float radYaw = 41.0f * (pi * inv180); float radPitch = 0.0f; float radRoll = 0.0f;

					float sy = sinf(radYaw);   float cy = cosf(radYaw);
					float sp = sinf(radPitch); float cp = cosf(radPitch);
					float sr = sinf(radRoll);  float cr = cosf(radRoll);

					// Canonical inverse matrix conversion baseline for stable vertex array stream tracking [INDEX: 0.1.4]
					float tx = wLdX * cy - wLdY * sy;
					float ty_fixed = wLdX * sy + wLdY * cy;
					float tz = wLdZ;

					float px = tx;
					float py = ty_fixed * cp + tz * sp;
					float pz = -ty_fixed * sp + tz * cp;

					// Cache actor absolute position coordinates to avoid redundant virtual calls in loop [INDEX: 0.1.4]
					float actorX = static_cast<float>(actor->X());
					float actorY = static_cast<float>(actor->Y());
					float actorZ = static_cast<float>(actor->Z());
					float midHeightOffset = actorZ + ((float)trueVisualHeight * 0.5f);

					// ==============================================================================
					// MASTER COMPASS EXPERIMENTAL SWITCH
					// SET TO 'true' TO RUN 3D VOLUMETRIC COMPASS PATHWAY
					// SET TO 'false' TO RUN 2D XY PLANAR REVENUE BLOCK FROM
					// ==============================================================================
					const bool useXYZ3Dcompass = true;

					if (useXYZ3Dcompass)
					{
						// --- PATHWAY A: RECTIFIED 3D COMPASS TRACKING (DIRECT MOTION) ---
						float sumHorizCompass3D = 0.0f;
						float sumVertCompass3D = 0.0f;
						float totalWeight3D = 0.0f;

						for (unsigned int l = 0; l < g_legacyModelLights.Size(); l++)
						{
							FLegacyModelLightCache *light = &g_legacyModelLights[l];
							float liveDx = light->absX - actorX;
							float liveDy = light->absY - actorY;
							float liveDz = light->absZ - midHeightOffset;
							float distSquared = liveDx * liveDx + liveDy * liveDy + liveDz * liveDz;

							// Extended visual radar buffer (+256 units) to expand the tracking horizon safely
							float globalRadarRadius = (512.0f + 256.0f) * scaleNormalizeFactor;

							if (distSquared < (globalRadarRadius * globalRadarRadius))
							{
								float groundDist = sqrtf(liveDx * liveDx + liveDy * liveDy);

								if (groundDist > 0.1f || fabsf(liveDz) > 0.1f)
								{
									float liveHorizAngle = atan2f(liveDy, liveDx);
									// FIXED: Removed input phase minus inversion for direct tracking alignment
									float singleHorizCompass = (liveHorizAngle * invPi);

									float liveVertAngle = atan2f(liveDz, groundDist);
									float singleVertCompass = (liveVertAngle * invPi);

									float lightWeight = 1.0f / ((light->radius + 1.0f) * coef4096);
									if (lightWeight < 0.001f) lightWeight = 0.001f;

									sumHorizCompass3D -= singleHorizCompass * lightWeight;
									sumVertCompass3D += singleVertCompass * lightWeight;
									totalWeight3D += lightWeight;
								}
							}
						}

						float finalHorizCompass3D = 0.0f; float finalVertCompass3D = 0.0f;
						if (totalWeight3D > 0.001f)
						{
							float invTotalWeight = 1.0f / totalWeight3D;
							finalHorizCompass3D = sumHorizCompass3D * invTotalWeight;
							finalVertCompass3D = sumVertCompass3D * invTotalWeight;
						}

						float smoothedYawOffset = sinf(finalHorizCompass3D * pi);
						float smoothedPitchOffset = sinf(finalVertCompass3D * pi);

						// ==============================================================================
						//              TERMINAL COORDINATE LOCK INJECTION SHUNT
						// Offsets strictly moved to final vector tail
						// Horizontal (X/Y) calculates offset using your proven stable 2D code (+),
						// Vertical (Z) locks using our verified straightened Pitch with negative sign (-).
						// ==============================================================================
						locLightDirX = px * cr - pz * sr - smoothedYawOffset + 0.5f;
						locLightDirY = px * sr + pz * cr + smoothedYawOffset;
						locLightDirZ = py                - smoothedPitchOffset;
					}
				}

				// Complete terminal high-utility normalization pass on local space light rays [INDEX: 0.1.6]
				float lLen = sqrtf(locLightDirX * locLightDirX + locLightDirY * locLightDirY + locLightDirZ * locLightDirZ);
				if (lLen > 0.001f)
				{
					// OPTIMIZATION: Replaced three channel divisions with single vector length inversion multiplied across axes
					float invLen = 1.0f / lLen; locLightDirX *= invLen; locLightDirY *= invLen; locLightDirZ *= invLen;
				}

				int off1 = frameno * surf->numVertices; int off2 = frameno2 * surf->numVertices;
				float interpFactor = (float)gl_RenderState.GetInterpolationFactor();

				// Fully purge all 6 isolated RAM containers before boolean index assembly
				const int totalSlices = 6;
				for (int s = 0; s < totalSlices; s++) sliceIndicesPool[s].Resize(0);

				// --- 2. BOOLEAN DISTRIBUTION OF TRIANGLES ACROSS 6 DISTINCT RAM SLICES ---
				for (unsigned int k = 0; k < surf->numTriangles; k++)
				{
					unsigned int i0 = surf->Tris[k].VertIndex[0];
					unsigned int i1 = surf->Tris[k].VertIndex[1];
					unsigned int i2 = surf->Tris[k].VertIndex[2];

					if (i0 >= surf->numVertices || i1 >= surf->numVertices || i2 >= surf->numVertices) continue;

					float v0x = surf->Vertices[off1 + i0].x * (1.0f - interpFactor) + surf->Vertices[off2 + i0].x * interpFactor;
					float v0y = surf->Vertices[off1 + i0].y * (1.0f - interpFactor) + surf->Vertices[off2 + i0].y * interpFactor;
					float v0z = surf->Vertices[off1 + i0].z * (1.0f - interpFactor) + surf->Vertices[off2 + i0].z * interpFactor;

					float v1x = surf->Vertices[off1 + i1].x * (1.0f - interpFactor) + surf->Vertices[off2 + i1].x * interpFactor;
					float v1y = surf->Vertices[off1 + i1].y * (1.0f - interpFactor) + surf->Vertices[off2 + i1].y * interpFactor;
					float v1z = surf->Vertices[off1 + i1].z * (1.0f - interpFactor) + surf->Vertices[off2 + i1].z * interpFactor;

					float v2x = surf->Vertices[off1 + i2].x * (1.0f - interpFactor) + surf->Vertices[off2 + i2].x * interpFactor;
					float v2y = surf->Vertices[off1 + i2].y * (1.0f - interpFactor) + surf->Vertices[off2 + i2].y * interpFactor;
					float v2z = surf->Vertices[off1 + i2].z * (1.0f - interpFactor) + surf->Vertices[off2 + i2].z * interpFactor;

					float edge1X = v1x - v0x; float edge1Y = v1y - v0y; float edge1Z = v1z - v0z;
					float edge2X = v2x - v0x; float edge2Y = v2y - v0y; float edge2Z = v2z - v0z;

					// True 3D Cross Product for the face plane normal with Doom-swapped Shadow axes
					float rawNx = (edge1Y * edge2Z) - (edge1Z * edge2Y);
					float rawNy = (edge1Z * edge2X) - (edge1X * edge2Z);
					float rawNz = (edge1X * edge2Y) - (edge1Y * edge2X);

					float faceNx = rawNx;
					float faceNy = rawNz; // Axis swap: Y <- Z 
					float faceNz = rawNy; // Axis swap: Z <- Y 

					float nLen = sqrtf(faceNx*faceNx + faceNy * faceNy + faceNz * faceNz);
					if (nLen > 0.001f) { faceNx /= nLen; faceNy /= nLen; faceNz /= nLen; }

					// Evaluate alignment between the face normal and the active light vector (Dot Product)
					float rawDot = (faceNx * locLightDirX) + (faceNy * locLightDirY) + (faceNz * locLightDirZ);
					float normalizedRating = (rawDot + 1.0f) * 0.5f;

					int targetSliceMeshID = (int)(normalizedRating * (float)totalSlices);
					if (targetSliceMeshID < 0) targetSliceMeshID = 0;
					if (targetSliceMeshID >= totalSlices) targetSliceMeshID = totalSlices - 1;

					sliceIndicesPool[targetSliceMeshID].Push(i0);
					sliceIndicesPool[targetSliceMeshID].Push(i1);
					sliceIndicesPool[targetSliceMeshID].Push(i2);
				}

				// Unbind Graf Zahl's baked EBO to open the direct RAM array transfer path
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

				// --- 3. RENDER 6 COMPLETELY INDEPENDENT SUB-MODELS VIA LIGHTPARMS ---
				for (int slice = 0; slice < totalSlices; slice++)
				{
					if (sliceIndicesPool[slice].Size() == 0) continue;

					float sliceProgress = (float)slice / (float)totalSlices;
					float diffuse = sliceProgress;

					// 3D displacement of the frame pivot relative to Height (wLdZ)
					float slicePivotOffsetFactor = trueVisualRadius * (1.0f - (sliceProgress * 2.0f));
					float sliceWorldX = worldActorX + (float)(wLdX * slicePivotOffsetFactor);
					float sliceWorldY = worldActorY + (float)(wLdY * slicePivotOffsetFactor);
					float sliceWorldZ = worldActorCenterZ + (float)(wLdZ * slicePivotOffsetFactor);
					float sliceInjectR = 0.0f; float sliceInjectG = 0.0f; float sliceInjectB = 0.0f;
					for (unsigned int l = 0; l < g_legacyModelLights.Size(); l++)
					{
						FLegacyModelLightCache *light = &g_legacyModelLights(l);
						float dx = ((float)light->relX - sliceWorldX) * scaleNormalizeFactor;
						float dy = ((float)light->relY - sliceWorldY) * scaleNormalizeFactor;
						float dz = ((float)light->relZ - sliceWorldZ) * scaleNormalizeFactor;
						float distSquared = dx * dx + dy * dy + dz * dz;
						float interactionRadius = (light->radius + trueVisualRadius) * scaleNormalizeFactor;
						if (distSquared < (float)(interactionRadius * interactionRadius))
						{
							float dist = sqrtf((float)distSquared);
							float linearFrac = 1.0f - (dist / interactionRadius);
							if (linearFrac > 0.0f)
							{
								float smoothFrac = linearFrac * linearFrac;
								sliceInjectR += light->r * smoothFrac * 1.25f;
								sliceInjectG += light->g * smoothFrac * 1.25f;
								sliceInjectB += light->b * smoothFrac * 1.25f;
							}
						}
					}
					// Final vivid slice shading using standard diffuse shading rules
					float finalR = baseColor + (sliceInjectR * diffuse);
					float finalG = baseColor + (sliceInjectG * diffuse);
					float finalB = baseColor + (sliceInjectB * diffuse);
					if (finalR > 1.0f) finalR = 1.0f;
					if (finalG > 1.0f) finalG = 1.0f;
					if (finalB > 1.0f) finalB = 1.0f;
					glColor4f(finalR, finalG, finalB, 1.0f);
					glDrawElements(GL_TRIANGLES, sliceIndicesPool[slice].Size(), GL_UNSIGNED_INT, sliceIndicesPool[slice].Data());
				}

				// CRASH FIX FOR RESET: Restore the original VBO/EBO state immediately 
				// after custom slice rendering to prevent LZDoom07 engine breakdown further down the line
				if (currentVBuf != nullptr) currentVBuf->BindVBO();
			}
			else
			{
				// ==============================================================================
				// PATHWAY B: FLAT LIGHT NO-SLICE CONVEYOR ROUTE FOR FLAT/DECORATIVE MODELS
				// ==============================================================================
				isUsingVolumetric3DModelLegacyDynlight = false; // They look bad with Pathway A
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
