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

struct FMD3BakedNormalAccum
{
	float nx = 0.0f;
	float ny = 0.0f;
	float nz = 0.0f;
};
static TArray<FMD3BakedNormalAccum> g_md3TempNormalAccumPool;

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
			minBoundsPerFrame[f] = FLT_MAX;
			maxBoundsPerFrame[f] = -FLT_MAX;
			this->trueVisualHeights[f] = 0.0f;
		}

		// [Darkcrafter07]: DIFFERENTIAL LINE FLEXURE CURVATURE SHIELD!
		// ISOLATED PASS: We scan all surfaces up to infinity BEFORE Graf's factory packaging loops.
		bool modelContainsSharp3DCurvatureBends = false;

		for (unsigned i = 0; i < Surfaces.Size(); i++)
		{
			MD3Surface * surf = &Surfaces[i];
			surf->vindex = vindex;
			surf->iindex = iindex;

			// ==============================================================================
			// [Darkcrafter07]: DIFFERENTIAL LINE FLEXURE CURVATURE SHIELD (O(N) FAST PASS)
			// Tracks running normal flow across triangles; detects impossible ~180-degree
			// inward flips (like inverted caps) and snaps them back outward.
			// ==============================================================================
			unsigned int totalNormalEntries = Frames.Size() * surf->numVertices;
			g_md3TempNormalAccumPool.Resize(totalNormalEntries);

			for (unsigned int v = 0; v < totalNormalEntries; v++)
			{
				g_md3TempNormalAccumPool[v].nx = 0.0f;
				g_md3TempNormalAccumPool[v].ny = 0.0f;
				g_md3TempNormalAccumPool[v].nz = 0.0f;
			}

			// Pass 1: Compute true 3D Cross Product face normals with Curvature Flex flip guard
			for (unsigned int f = 0; f < Frames.Size(); f++)
			{
				int frameVertexOffset = f * surf->numVertices;

				// Compute frame geometric center to anchor initial reference normal outward
				float centX = 0.0f, centY = 0.0f, centZ = 0.0f;
				if (surf->numVertices > 0)
				{
					for (unsigned int v = 0; v < surf->numVertices; ++v)
					{
						const MD3Vertex* vp = &surf->Vertices[frameVertexOffset + v];
						centX += vp->x;
						centY += vp->z;
						centZ += vp->y;
					}
					float invV = 1.0f / (float)surf->numVertices;
					centX *= invV; centY *= invV; centZ *= invV;
				}

				float refNx = 0.0f, refNy = 0.0f, refNz = 1.0f;
				bool hasRefNormal = false;

				for (unsigned int k = 0; k < surf->numTriangles; k++)
				{
					unsigned int i0 = surf->Tris[k].VertIndex[0];
					unsigned int i1 = surf->Tris[k].VertIndex[1];
					unsigned int i2 = surf->Tris[k].VertIndex[2];

					if (i0 < surf->numVertices && i1 < surf->numVertices && i2 < surf->numVertices)
					{
						MD3Vertex* vert0 = &surf->Vertices[frameVertexOffset + i0];
						MD3Vertex* vert1 = &surf->Vertices[frameVertexOffset + i1];
						MD3Vertex* vert2 = &surf->Vertices[frameVertexOffset + i2];

						// Convert MD3 axes (x, y=depth, z=up) to Doom world axes (X, Y=depth, Z=up)
						float edge1X = vert1->x - vert0->x; float edge1Y = vert1->z - vert0->z; float edge1Z = vert1->y - vert0->y;
						float edge2X = vert2->x - vert0->x; float edge2Y = vert2->z - vert0->z; float edge2Z = vert2->y - vert0->y;
						float edge12X = vert2->x - vert1->x; float edge12Y = vert2->z - vert1->z; float edge12Z = vert2->y - vert1->y;

						// Faithful SimSun cross-product face normal formula (Edge1 x Edge2)
						float fnx = (edge1Y * edge2Z) - (edge1Z * edge2Y);
						float fny = (edge1Z * edge2X) - (edge1X * edge2Z);
						float fnz = (edge1X * edge2Y) - (edge1Y * edge2X);

						float faceLen = sqrtf(fnx * fnx + fny * fny + fnz * fnz);
						if (faceLen > 0.00001f)
						{
							fnx /= faceLen; fny /= faceLen; fnz /= faceLen;

							// Triangle center relative to surface centroid
							float triMidX = ((vert0->x + vert1->x + vert2->x) * 0.333333f) - centX;
							float triMidY = ((vert0->z + vert1->z + vert2->z) * 0.333333f) - centY;
							float triMidZ = ((vert0->y + vert1->y + vert2->y) * 0.333333f) - centZ;

							// 1. Anchor first valid triangle outward from centroid (for closed/convex parts)
							// For flat horizontal caps, prefer upward (+Z)
							if (!hasRefNormal)
							{
								float outDot = triMidX * fnx + triMidY * fny + triMidZ * fnz;
								if (fabsf(fnz) > 0.85f && fnz < 0.0f)
								{
									fnx = -fnx; fny = -fny; fnz = -fnz;
								}
								else if (outDot < -0.1f && fabsf(fnz) <= 0.85f)
								{
									fnx = -fnx; fny = -fny; fnz = -fnz;
								}
								refNx = fnx; refNy = fny; refNz = fnz;
								hasRefNormal = true;
							}
							else
							{
								// 2. Curvature Flex check: detect sudden ~180-degree inversion against running flow
								float flowDot = fnx * refNx + fny * refNy + fnz * refNz;

								// Also check if a near-horizontal top cap suddenly points downward
								bool isInvertedCap = (fabsf(fnz) > 0.80f && fnz < 0.0f && triMidZ > 0.0f);

								if (flowDot < -0.65f || isInvertedCap)
								{
									fnx = -fnx; fny = -fny; fnz = -fnz;
								}

								// Smoothly update running reference flow (EMA filter)
								refNx = refNx * 0.85f + fnx * 0.15f;
								refNy = refNy * 0.85f + fny * 0.15f;
								refNz = refNz * 0.85f + fnz * 0.15f;
								float rLen = sqrtf(refNx * refNx + refNy * refNy + refNz * refNz);
								if (rLen > 0.001f) { refNx /= rLen; refNy /= rLen; refNz /= rLen; }
							}

							// Compute edge lengths for vertex corner angle weighting (Nelson Max, 1999)
							float l01 = sqrtf(edge1X * edge1X + edge1Y * edge1Y + edge1Z * edge1Z);
							float l02 = sqrtf(edge2X * edge2X + edge2Y * edge2Y + edge2Z * edge2Z);
							float l12 = sqrtf(edge12X * edge12X + edge12Y * edge12Y + edge12Z * edge12Z);

							float w0 = 1.0f, w1 = 1.0f, w2 = 1.0f;
							if (l01 > 0.0001f && l02 > 0.0001f && l12 > 0.0001f)
							{
								float cos0 = clamp((edge1X * edge2X + edge1Y * edge2Y + edge1Z * edge2Z) / (l01 * l02), -1.0f, 1.0f);
								float cos1 = clamp((-edge1X * edge12X - edge1Y * edge12Y - edge1Z * edge12Z) / (l01 * l12), -1.0f, 1.0f);
								float cos2 = clamp((-edge2X * -edge12X - edge2Y * -edge12Y - edge2Z * -edge12Z) / (l02 * l12), -1.0f, 1.0f);

								w0 = acosf(cos0); w1 = acosf(cos1); w2 = acosf(cos2);
							}

							g_md3TempNormalAccumPool[frameVertexOffset + i0].nx += fnx * w0;
							g_md3TempNormalAccumPool[frameVertexOffset + i0].ny += fny * w0;
							g_md3TempNormalAccumPool[frameVertexOffset + i0].nz += fnz * w0;

							g_md3TempNormalAccumPool[frameVertexOffset + i1].nx += fnx * w1;
							g_md3TempNormalAccumPool[frameVertexOffset + i1].ny += fny * w1;
							g_md3TempNormalAccumPool[frameVertexOffset + i1].nz += fnz * w1;

							g_md3TempNormalAccumPool[frameVertexOffset + i2].nx += fnx * w2;
							g_md3TempNormalAccumPool[frameVertexOffset + i2].ny += fny * w2;
							g_md3TempNormalAccumPool[frameVertexOffset + i2].nz += fnz * w2;
						}
					}
				}
			}

			// Pass 2: Normalize and pre-bake faithful 3D SimSun Sunlit weights into the vertex VBO arrays
			// AND overwrite surf->Vertices[j] normals in Doom-space (X, Y=depth, Z=up) for instant RAM lookup in RenderFrame
			for (unsigned j = 0; j < Frames.Size() * surf->numVertices; j++)
			{
				MD3Vertex* vert = &surf->Vertices[j];
				FModelVertex *bvert = &vertptr[vindex++];
				int tc = j % surf->numVertices;

				bvert->Set(vert->x, vert->z, vert->y, surf->Texcoords[tc].s, surf->Texcoords[tc].t);

				// Get accumulated smoothed-out true 3D-normal for the current vertex frame entry
				float finalNx = g_md3TempNormalAccumPool[j].nx;
				float finalNy = g_md3TempNormalAccumPool[j].ny;
				float finalNz = g_md3TempNormalAccumPool[j].nz;

				float normLen = sqrtf((finalNx * finalNx) + (finalNy * finalNy) + (finalNz * finalNz));
				if (normLen > 0.001f) { finalNx /= normLen; finalNy /= normLen; finalNz /= normLen; }
				else { finalNx = 0.0f; finalNy = 0.0f; finalNz = 1.0f; }

				// [Darkcrafter07]: Store normalized Doom-space normals directly back into RAM vertex array
				// so RenderFrame can read them directly with zero per-frame triangle math
				vert->nx = finalNx; vert->ny = finalNy; vert->nz = finalNz;

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

				// Bake compiled SimSun sunlit scale factor inside VBO normal coordinates vector, consider Quake/Doom axes flip
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

		rawPositionsPool.Resize(vbufsize);
		for (unsigned int v = 0; v < vindex; v++)
		{
			rawPositionsPool[v].X = vertptr[v].x;
			rawPositionsPool[v].Y = vertptr[v].y;
			rawPositionsPool[v].Z = vertptr[v].z;
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
// --- [Darkcrafter07]: GOURAUD SHADING, 3D-FLOOR & SUNLIGHT LIGHTING MANIFESTO ---
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
// 3. 3D-FLOOR COLOR RESOLVER: When 'RFL_NO_CLIP_PLANES' is active in GL1x/GL2x, the engine
//    sets lightlist to NULL for 3D models. We manually scan 'XFloor.lightlist' and cache
//    the resolved LightColor & lightlevel per actor per tic.
//
// Know how:
// - PATHWAY A: Overrides pipeline locks and binds RAM-realigned normals via 'GL_NORMAL_ARRAY'
//   with Quake 2 style Diffuse-to-Ambient mirroring for 360-degree volumetric dynlights.
// - PATHWAY B: Flat sector & 3D-floor colored base pass for non-dynlit models.
// - PATHWAY C (2nd-Pass Sunlight Overlay): Runs over both Pathway A and B when 'USEGL1SUNLIGHT'
//   is set in MODELDEF. Uses angle-weighted geometric normals (Darkcrafter07) baked into
//   RAM during BuildVertexBuffer() and projects world-locked directional sunlight via
//   2x multiplicative blending (GL_DST_COLOR, GL_SRC_COLOR) with GL_COMBINE RGB_REPLACE
//   (ignoring base texture RGB while preserving texture Alpha for masked cutouts).
//
// MODELDEF DIRECTIVE SYNTAX & PARAMETER ORDER:
//   USEGL1SUNLIGHT <sunShadowIntensity> <sunLightIntensity> <sunHighlightMax> <wSunX> <wSunY> <wSunZ> <useAlphaTest>
//
//   1. sunShadowIntensity (float) : Overall shadow contrast multiplier (e.g. 1.0 = normal, 5.5 = deep)
//   2. sunLightIntensity  (float) : Minimum brightness floor in deep shadow (e.g. 0.25)
//   3. sunHighlightMax    (float) : Maximum brightness peak on sunlit faces (>1.0 overexposures, e.g. 1.45)
//   4. wSunX              (float) : World Sun X direction (+East / -West, e.g. 0.55)
//   5. wSunY              (float) : World Sun Y direction (+North / -South, e.g. -0.45)
//   6. wSunZ              (float) : World Sun Z elevation (+Up / -Down, e.g. 0.70)
//   7. useAlphaTest       (int)   : 0 = solid/translucent pass (no alpha test), 1 = binary alpha-test cutout (fences/foliage)
//
// MODELDEF EXAMPLES:
//
//   // Example 1: Solid opaque prop (Cistern / Rock / Architecture) without alpha-test
//   Model Cistern_big_tank
//   {
//      Path "Models\Props"
//      SKIN 0 "Cistern_SHAWN2.png"
//      MODEL 0 "Cistern_big_tank_SHAWN2.md3"
//      USETRUEVISLIGHTBOUNDS
//      USEGL1VOLUMEDYNLIGHT
//      USEGL1SUNLIGHT 1.0 0.25 1.45 0.55 -0.45 0.70 0
//      Scale 1.0 1.0 1.0
//      FrameIndex CIST A 0 0
//   }
//
//   // Example 2: Masked foliage / fence with alpha-test enabled (last param = 1)
//   Model Tree_Pipal_Big1
//   {
//      Path "Models\Trees\Tree_oak"
//      SKIN 0 "0_Pipal_branches_leaves_fine.png"
//      MODEL 0 "Tree_oak_branches_leaves_fine.md3"
//      USEACTORROLL
//      USEACTORPITCH
//      USETRUEVISLIGHTBOUNDS
//      USEGL1VOLUMEDYNLIGHT
//      USEGL1SUNLIGHT 1.2 0.35 1.35 0.55 -0.45 0.70 1
//      Scale 8.0 8.0 8.0
//      FrameIndex TR01 A 0 0
//   }
// ====================================================================================

extern int g_legacyModelSectorLight;
extern bool g_legacyLightActive;
extern TArray<FLegacyDynlight3DmdlCache> g_legacyModelLights;
bool isUsingVolumetric3DModelLegacyDynlight = false;  // for gl_20.cpp ApplyFixedFunction
bool g_isCurrent3dMdlWeldedSolid = false;             // store current model computed topology weight

void FMD3Model::RenderFrame(AActor *actor, FModelRenderer *renderer, FTexture * skin, int frameno, int frameno2, double inter, int translation, const PClass *ti)
{
	const FSpriteModelFrame* safeFramePtr = nullptr;

	float xscaleMdldef = 1.0f, yscaleMdldef = 1.0f;
	float xyscaleMdldef = 1.0f, zscaleMdldef = 1.0f;
	float xyscaleMap = 1.0f, zscaleMap = 1.0f;
	float finalScaleX = 1.0f, finalScaleZ = 1.0f;
	float map2mdldefRatioXY = 1.0f;
	float mdldef2mapRatioXY = 1.0f;
	float map2mdldefRatioXYinv = 1.0f;
	float mdldef2mapRatioXYinv = 1.0f;
	float scaleZcombo = 1.0f, scaleZcomboInvCompens = 1.0f;

	const float invMul255 = 1.0f / 255.0f;

	bool isClassTypeValid = ti != nullptr;

	if (isClassTypeValid && actor != nullptr)
	{
		safeFramePtr = FindModelFrame(ti, actor->sprite, actor->frame, false);
	}

	if (safeFramePtr != nullptr)
	{
		xscaleMdldef = (float)safeFramePtr->xscale;
		yscaleMdldef = (float)safeFramePtr->yscale;
		zscaleMdldef = (float)safeFramePtr->zscale;

		// Secure scale assignments: use fallback safe unit matrix if actor memory is dead
		if (actor != nullptr)
		{
			xyscaleMap = (float)actor->Scale.X; // actually both X and Y axes wtf!
			zscaleMap = (float)actor->Scale.Y;  // actually Z (vertical) axis only!
		}
	}

	xyscaleMdldef = (xscaleMdldef + yscaleMdldef) * 0.5f;
	map2mdldefRatioXY = xyscaleMap / (xyscaleMdldef + 0.0001f);
	mdldef2mapRatioXY = xyscaleMdldef / (xyscaleMap + 0.0001f);

	// FIX: Corrected inverse equations to divide by calculated ratios instead of themselves
	map2mdldefRatioXYinv = 1.0f / (map2mdldefRatioXY + 0.0001f);
	mdldef2mapRatioXYinv = 1.0f / (mdldef2mapRatioXY + 0.0001f);

	float yawDeg = actor->Angles.Yaw.Degrees;
	float pitchDeg = 0.0f; float rollDeg = 0.0f;
	if (safeFramePtr != nullptr)
	{
		yawDeg += safeFramePtr->angleoffset;

		if (safeFramePtr->flags & MDL_USEACTORPITCH)
		{
			pitchDeg = actor->Angles.Pitch.Degrees + safeFramePtr->pitchoffset;
		}
		if (safeFramePtr->flags & MDL_USEACTORROLL)
		{
			rollDeg = actor->Angles.Roll.Degrees + safeFramePtr->rolloffset;
		}
	}
	const float deg2rad = 3.141592653589 / 180.0f;
	float radYaw = -yawDeg * deg2rad;
	float radPitch = -pitchDeg * deg2rad;
	float radRoll = -rollDeg * deg2rad;
	float radYawInv = 1.0f / radYaw;
	float radPitchInv = 1.0f / radPitch;
	float radRollInv = 1.0f / radRoll;

	// ==============================================================================
	// GL1x/GL2x legacy mode
	// ==============================================================================
	if (gl.legacyMode)
	{
		// In gl_sprite.cpp (GLSprite::Process), when running on GL1x/GL2x hardware
		// where "gl.flags & RFL_NO_CLIP_PLANES" is active, lightlist is set to NULL,
		// so 3D models won't receive special light color coming from 3D floors.
		// Sprites draw nicely because SplitSprite iterates through XFloor.lightlist,
		// slices the sprite rectangle horizontally into segments, and assigns each
		// segment its lightlevel and Colormap via CopyLight(lightlist[i].extra_colormap).
		// Manually resolve XFloor.lightlist once per actor before the surface loop.
		float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
		float secR = 1.0f, secG = 1.0f, secB = 1.0f, secA = 1.0f;
		int sectorLightLevel = g_legacyModelSectorLight;

		if (actor != nullptr && actor->Sector != nullptr)
		{
			FColormap cm = actor->Sector->Colormap;
			bool found3DFloor = false;

			// Check 3D-floors (mirroring GLSprite::Process)
			if (actor->Sector->e != nullptr)
			{
				TArray<lightlist_t> &lightlist = actor->Sector->e->XFloor.lightlist;
				const float checkZ = actor->Z() + (actor->Height * 0.5);

				for (unsigned int i = 0; i < lightlist.Size(); i++)
				{
					float lightbottom = (i < lightlist.Size() - 1)
						? (float)lightlist[i + 1].plane.ZatPoint(actor)
						: (float)actor->Sector->floorplane.ZatPoint(actor);

					if (lightbottom < checkZ)
					{
						cm.CopyLight(lightlist[i].extra_colormap);
						if (lightlist[i].p_lightlevel != nullptr)
						{
							sectorLightLevel = gl_ClampLight(*lightlist[i].p_lightlevel);
						}
						found3DFloor = true;
						break;
					}
				}
			}

			PalEntry lightCol = cm.LightColor;
			PalEntry spriteCol = actor->Sector->SpecialColors[sector_t::sprites];

			const float invMul65025 = 1.0f / (255.0f * 255.0f);

			// 1. Pure light tint of sector and 3D floor (for PATHWAY A ambient modulation)
			tintR = (lightCol.r * spriteCol.r) * invMul65025;
			tintG = (lightCol.g * spriteCol.g) * invMul65025;
			tintB = (lightCol.b * spriteCol.b) * invMul65025;

			// 2. Final color with engine light curve & flat dynlights (for PATHWAY B & PATHWAY A base):
			// If actor is under a 3D-floor, use its resolved lightlevel; otherwise keep native mColor
			float baseLumaR = found3DFloor ? (sectorLightLevel * invMul255) : gl_RenderState.mColor.vec[0];
			float baseLumaG = found3DFloor ? (sectorLightLevel * invMul255) : gl_RenderState.mColor.vec[1];
			float baseLumaB = found3DFloor ? (sectorLightLevel * invMul255) : gl_RenderState.mColor.vec[2];

			secR = clamp(baseLumaR + gl_RenderState.mDynColor.vec[0], 0.0f, 1.0f) *
				(lightCol.r * invMul255) * (gl_RenderState.mObjectColor.r * invMul255);
			secG = clamp(baseLumaG + gl_RenderState.mDynColor.vec[1], 0.0f, 1.0f) *
				(lightCol.g * invMul255) * (gl_RenderState.mObjectColor.g * invMul255);
			secB = clamp(baseLumaB + gl_RenderState.mDynColor.vec[2], 0.0f, 1.0f) *
				(lightCol.b * invMul255) * (gl_RenderState.mObjectColor.b * invMul255);
			secA = clamp(gl_RenderState.mColor.vec[3], 0.0f, 1.0f) * (gl_RenderState.mObjectColor.a * invMul255);
		}

		// ==============================================================================
		//            OPTIONAL GOURAD SUNLIGHT PRE-PROCESSING - START
		// ==============================================================================
		bool useGouraudSunPass2 = (curSpriteMDLFrame != nullptr && (curSpriteMDLFrame->flags & MDL_USEGL1SUNLIGHT) != 0);

		const float sunShadowIntensity = useGouraudSunPass2 ? curSpriteMDLFrame->sunShadowIntensity : 1.0f;
		const float sunLightIntensity = useGouraudSunPass2 ? curSpriteMDLFrame->sunLightIntensity : 0.25f;
		const float sunHighlightMax = useGouraudSunPass2 ? curSpriteMDLFrame->sunHighlightMax : 1.45f;

		TArray<unsigned char> sunPassColors;
		TArray<unsigned int> sunPassOffsets;
		sunPassOffsets.Resize(Surfaces.Size());

		if (useGouraudSunPass2 && actor != nullptr && Surfaces.Size() > 0)
		{
			// 1. World sun vector from MODELDEF (X = East, Y = North, Z = Top)
			float wSunX = curSpriteMDLFrame->wSunX;
			float wSunY = curSpriteMDLFrame->wSunY;
			float wSunZ = curSpriteMDLFrame->wSunZ;
			float wLen = sqrtf(wSunX * wSunX + wSunY * wSunY + wSunZ * wSunZ);
			if (wLen > 0.0001f) { wSunX /= wLen; wSunY /= wLen; wSunZ /= wLen; }

			// 2. Compensate actor world rotation (Yaw + angleoffset)
			float yawRad = -actor->Angles.Yaw.Radians();
			if (safeFramePtr != nullptr) yawRad -= (safeFramePtr->angleoffset * (3.14159265358979 / 180.0f));

			float cosY = (float)cos(yawRad);
			float sinY = (float)sin(yawRad);

			float locSunX = wSunX * cosY - wSunY * sinY;
			float locSunY = wSunX * sinY + wSunY * cosY;
			float locSunZ = wSunZ;

			unsigned int totalVerts = 0;
			for (unsigned int mIdx = 0; mIdx < Surfaces.Size(); ++mIdx)
			{
				totalVerts += Surfaces[mIdx].numVertices;
			}
			sunPassColors.Resize(totalVerts * 4);

			unsigned int writeIdx = 0;

			for (unsigned int mIdx = 0; mIdx < Surfaces.Size(); ++mIdx)
			{
				sunPassOffsets[mIdx] = writeIdx;
				MD3Surface* surf = &Surfaces[mIdx];

				if (surf->numVertices > 0 && surf->Vertices.Size() > 0)
				{
					const MD3Vertex* verts = &surf->Vertices[frameno * surf->numVertices];

					for (int v = 0; v < surf->numVertices; ++v)
					{
						// Read normals baked in BuildVertexBuffer
						float ndotl = verts[v].nx * locSunX + verts[v].ny * locSunY + verts[v].nz * locSunZ;
						float factor01 = clamp((ndotl + 1.0f) * 0.5f, 0.0f, 1.0f);

						float rawSun = sunLightIntensity + factor01 * (sunHighlightMax - sunLightIntensity);
						float sunFactor = 1.0f + (rawSun - 1.0f) * sunShadowIntensity;

						unsigned char cVal = (unsigned char)clamp(128.0f * sunFactor, 32.0f, 245.0f);

						sunPassColors[writeIdx + 0] = cVal;
						sunPassColors[writeIdx + 1] = cVal;
						sunPassColors[writeIdx + 2] = cVal;
						sunPassColors[writeIdx + 3] = 255;
						writeIdx += 4;
					}
				}
			}
		}
		// ==============================================================================
		//            OPTIONAL GOURAD SUNLIGHT PRE-PROCESSING - FINISH
		// ==============================================================================

		if ((unsigned)frameno >= Frames.Size() || (unsigned)frameno2 >= Frames.Size()) return;
		renderer->SetInterpolation(inter);
		float baseColor = (float)g_legacyModelSectorLight * invMul255;

		if (!gl_lights) // Dynligths are off, clear array cache and shutdown surfaces
		// needs to be done, otherwise actors won't kill volumetric dynlight when turned off in menu
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
			{
				currentVBuf->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);
			}

			// Repeat local check on empty memory of surface dynlights
			if (g_legacyModelLights.Size() == 0) g_legacyLightActive = false;

			bool isModeldefUseVolLegacyDynlightFlagSet = ((curSpriteMDLFrame->flags & MDL_USEGL1VOLUMEDYNLIGHT) != 0);

			// When "g_legacyLightActive" is present it turns OFF Gourad for models that are NOT lit by dynlights at all.
			// So that it jumps to PATHWAY B if no dynlights are nearby. May save some performance but not as consistent.
			bool useLegacyVolumetricLighting = (gl_lights && g_legacyLightActive &&
				surf->numTriangles > 0 && isModeldefUseVolLegacyDynlightFlagSet);

			// ==============================================================================
			// PATHWAY A: VOLUMETRIC GOURAD 3D MODEL DYNLIGHT
			// ==============================================================================
			if (useLegacyVolumetricLighting)
			{
				isUsingVolumetric3DModelLegacyDynlight = true;
				// Fetch the active model matrix from Graf's render state cache
				auto *modelMatrixPtr = &gl_RenderState.mModelMatrix;

				// Dimensions setup
				finalScaleX = xscaleMdldef * xyscaleMap;
				//finalScaleZ = zscaleMdldef * yscaleMapedit; // insufficient for tall objects
				scaleZcombo = zscaleMdldef * zscaleMap;
				// the higher the object, lesser the val is, so that it protects from overexpansion on smaller objects
				scaleZcomboInvCompens = (1.0f - (1.0f / scaleZcombo + 0.001f) * 35.5f); // 50 was too much for small objects
				finalScaleZ = scaleZcombo * expf(scaleZcombo * 0.5f) - (scaleZcomboInvCompens * scaleZcomboInvCompens);
				if (finalScaleX <= 0.0f) finalScaleX = 1.0f;
				if (finalScaleZ <= 0.0f) finalScaleZ = 1.0f;

				// --- SCALE NORMALIZER ---
				// Measure average scaling factor to elegantly adapt attenuation properties
				float avgModelScale = (finalScaleX + finalScaleZ) * 0.5f;
				float avgModelScaleInv = 1.0f / avgModelScale;
				if (avgModelScale <= 0.001f) avgModelScale = 1.0f;

				float referenceRockScale = 368.0f;
				float scaleNormalizeFactor = referenceRockScale / finalScaleZ;
				float trueVisualHeight = this->GetTrueMDLVisualHeight(frameno, finalScaleZ);
				float trueVisualRadius = this->GetTrueMDLVisualRadius(frameno, finalScaleX);
				if (trueVisualRadius <= 0.01f) trueVisualRadius = 32.0f;
				if (trueVisualHeight <= 0.01f) trueVisualHeight = 64.0f;

				const float legacyVolumDynlightIntensity = 3.0f;

				// Safe global registers light slots scope tracking variable
				unsigned int maxLightsToBind = g_legacyModelLights.Size();
				if (maxLightsToBind > 8) maxLightsToBind = 8; // OpenGL fixed function strict layout limits

				if (currentVBuf != nullptr && surf->numVertices > 0 && surf->Vertices.Size() > 0)
				{
					// --- STEP 1: INITIALIZE NATIVE VERTEX STREAM INTERPOLATION ---
					auto* vertexBuffer = GetVertexBuffer(renderer);
					vertexBuffer->SetupFrame(renderer, surf->vindex + frameno * surf->numVertices, surf->vindex + frameno2 * surf->numVertices, surf->numVertices);

					glShadeModel(GL_SMOOTH);
					glEnable(GL_LIGHTING); // Wake up hardware lighting engines

					// --- FORCE UNIT LENGTH NORMALS ---
					glEnable(GL_NORMALIZE);
					glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE); // Force two-sided math to secure all faces

					// Compute inverse model matrix to cancel out OpenGL Eye Space double-multiplication bugs
					VSMatrix inverseModelMatrix;
					inverseModelMatrix = *modelMatrixPtr;
					if (!modelMatrixPtr->inverseMatrix(inverseModelMatrix))
					{
						inverseModelMatrix.loadIdentity(); // Fallback to identity if inversion fails
					}

					// --- RAM INVERSE NORMAL REALIGNER ---
					// Load raw unpacked float 3D normals straight from 'surf->Vertices'.
					// Transform them using inverse matrix cells to align with world transformations.
					// Pipe data straight from system RAM safely by unbinding the buffer array.
					TArray<float> generatedNormals;
					generatedNormals.Resize(surf->numVertices * 3);

					int frameVertexOffset = frameno * surf->numVertices;
					const MD3Vertex* verticesBase = &surf->Vertices[frameVertexOffset];

					// Extract the internal 3x3 block arrays from the calculated inverseModelMatrix
					const FLOATTYPE* invMat = inverseModelMatrix.get();

					for (int v = 0; v < surf->numVertices; ++v)
					{
						float origNx = verticesBase[v].nx;
						float origNy = verticesBase[v].ny;
						float origNz = verticesBase[v].nz;

						// Multiply by inverse matrix to align normal planes exactly with world transformations
						float alignedNx = origNx * (float)invMat[0] + origNy * (float)invMat[1] + origNz * (float)invMat[2];
						float alignedNy = origNx * (float)invMat[4] + origNy * (float)invMat[5] + origNz * (float)invMat[6];
						float alignedNz = origNx * (float)invMat[8] + origNy * (float)invMat[9] + origNz * (float)invMat[10];

						float nLen = sqrtf(alignedNx * alignedNx + alignedNy * alignedNy + alignedNz * alignedNz);
						if (nLen > 0.001f)
						{ alignedNx /= nLen; alignedNy /= nLen; alignedNz /= nLen; }

						generatedNormals[v * 3 + 0] = alignedNx;
						generatedNormals[v * 3 + 1] = alignedNy;
						generatedNormals[v * 3 + 2] = alignedNz;
					}

					for (unsigned int l = 0; l < g_legacyModelLights.Size(); ++l)
					{
						FLegacyDynlight3DmdlCache &light = g_legacyModelLights[l];
						secR += light.r * legacyVolumDynlightIntensity * 0.01f;
						secG += light.g * legacyVolumDynlightIntensity * 0.01f;
						secB += light.b * legacyVolumDynlightIntensity * 0.01f;
					}

					float ambColor[4] = { secR, secG, secB, 1.0f };
					glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambColor);

					DVector3 modelPos = actor->Pos();
					float midZ = (float)modelPos.Z;

					// --- IDENTITY LIGHT STATE BINDING (EYE SPACE DOUBLE-MULT FIX) ---
					// Temporarily drop modelMatrix register to Identity status before binding lights.
					// This forces glLightfv to capture pure, un-inverted world light parameters,
					// entirely curing the 180-degree sign inversion lag.
					gl_RenderState.EnableModelMatrix(false);
					gl_RenderState.Apply(actor);

					// --- STEP 3: BIND CACHED FLARES TO STANDARD HARDWARE LIGHT SLOTS ---
					for (unsigned int l = 0; l < maxLightsToBind; ++l)
					{
						FLegacyDynlight3DmdlCache &cachedLight = g_legacyModelLights[l];
						GLenum lightSlot = GL_LIGHT0 + l;
						glEnable(lightSlot);

						// Pure WORLD space parameters coordinates [4], [0], [1], [2], [3]
						float lightPos[4];
						lightPos[0] = (float)cachedLight.absX;
						lightPos[1] = (float)cachedLight.absZ;
						lightPos[2] = (float)cachedLight.absY;
						lightPos[3] = 1.0f;

						// --- HIGH-SPEED STRIDE VERTEX SAMPLER SHUNT ---
						float closestVertexWorld[3]; //  [3], [0], [1], [2]
						closestVertexWorld[0] = (float)modelPos.X;
						closestVertexWorld[1] = (float)modelPos.Y;
						closestVertexWorld[2] = (float)midZ;

						float minVertexDistSquared = FLT_MAX;
						int totalVerts = surf->numVertices;

						if (totalVerts > 0)
						{
							int strideStep = totalVerts / 8;
							if (strideStep < 1) strideStep = 1;

							for (int vIdx = 0; vIdx < totalVerts; vIdx += strideStep)
							{
								const MD3Vertex* testVert = &verticesBase[vIdx];

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
						float dx = ((float)cachedLight.absX - closestVertexWorld[0]) * avgModelScaleInv;
						float dy = ((float)cachedLight.absY - closestVertexWorld[1]) * avgModelScaleInv;
						float dz = ((float)cachedLight.absZ - closestVertexWorld[2]) * avgModelScaleInv;
						float currentDist = sqrtf(dx * dx + dy * dy + dz * dz);

						float scaledRadius = cachedLight.radius * avgModelScale;
						if (scaledRadius <= 0.01f) scaledRadius = 1.0f;

						float distFactor = 1.0f - (currentDist / (scaledRadius * 0.75f));
						if (distFactor < 0.0f) distFactor = 0.0f;

						// --- ARRAY INDEXATION SQUARE BRACKETS [4], [0], [1], [2], [3] ---
						float diffuseColor[4];
						diffuseColor[0] = cachedLight.r * legacyVolumDynlightIntensity * distFactor * distFactor;
						diffuseColor[1] = cachedLight.g * legacyVolumDynlightIntensity * distFactor * distFactor;
						diffuseColor[2] = cachedLight.b * legacyVolumDynlightIntensity * distFactor * distFactor;
						diffuseColor[3] = 1.0f;

						// --- THE QUAKE 2 STYLE DIFFUSE MIRRORING INJECTOR ---
						// We mirror diffuseColor parameters straight into GL_AMBIENT
						// When the projectile flies to the back-face, hardware diffuse equations drop to 0,
						// but the ambient channel picks up the exact same value seamlessly.
						// This retains tight linear attenuation properties from all 360 degrees perfectly.
						glLightfv(lightSlot, GL_POSITION, lightPos);
						glLightfv(lightSlot, GL_DIFFUSE, diffuseColor);
						glLightfv(lightSlot, GL_SPECULAR, diffuseColor);
						glLightfv(lightSlot, GL_AMBIENT, diffuseColor); // Diffuse mirroring anchor channel

						// --- CLEAN LINEAR ATTENUATION INJECTOR ---
						float radiusInv = 1.0f / cachedLight.radius;
						float linAttA = radiusInv * currentDist * 48.0f;
						float linAttB = radiusInv * avgModelScale * 0.4f;
						float decrLightBlbSizOnBigMdl = map2mdldefRatioXY * 0.005f;
						float avgAttAB = (linAttA + linAttB) * 0.5f;
						glLightf(lightSlot, GL_CONSTANT_ATTENUATION, 0.0f);
						glLightf(lightSlot, GL_LINEAR_ATTENUATION, avgAttAB + decrLightBlbSizOnBigMdl);
						glLightf(lightSlot, GL_QUADRATIC_ATTENUATION, 0.0f);
					}
					// --- RESTORE ACTIVE MATRIX CONTEXT PATHWAY ---
					gl_RenderState.mModelMatrix = *modelMatrixPtr;
					gl_RenderState.EnableModelMatrix(true);
					gl_RenderState.Apply(actor);
					glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
					glEnable(GL_COLOR_MATERIAL);
					// --- REGISTER COUPLING GATE ---
					glBindBuffer(GL_ARRAY_BUFFER, 0);
					glEnableClientState(GL_NORMAL_ARRAY);
					glNormalPointer(GL_FLOAT, 0, generatedNormals.Size() ? generatedNormals.Data() : nullptr);
					if (currentVBuf != nullptr) currentVBuf->BindVBO();
				}

				// Dispatch the geometry using 1 single factory VBO Draw Call
				glColor4f(1.0f, 1.0f, 1.0f, secA); // prevents GL_COLOR_MATERIAL to shading twice and darkening it
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
				// PATHWAY B: FLAT 3D MODEL DYNLIGHT
				// ==============================================================================
				isUsingVolumetric3DModelLegacyDynlight = false;
				glColor4f(secR, secG, secB, secA); // to fix dark models and receieve 3D-floor special light color
				// CRASH FIX: If a model was previously rendered using Pathway A, 
				// GL_ELEMENT_ARRAY_BUFFER remains 0. Force-restore it now.
				if (currentVBuf != nullptr) currentVBuf->BindVBO();
				renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));
			}

			// =================================================================================
			// PATHWAY C: GOURAUD SUNLIGHT OVERLAY (100% PURE VERTEX COLOR + TEXTURE ALPHA)
			// =================================================================================
			if (useGouraudSunPass2 && sunPassColors.Size() > 0 && surf->Vertices.Size() > 0)
			{
				bool wasFogEnabled = glIsEnabled(GL_FOG) != 0;
				if (wasFogEnabled) glDisable(GL_FOG);

				glEnable(GL_TEXTURE_2D);

				// Use GL_COMBINE for OpenGL 1.3+
				// RGB is taken STRICTLY from the vertex sun (GL_PRIMARY_COLOR), texture color ignored
				// Alpha is taken STRICTLY from the texture (GL_TEXTURE) to cut holes in fences and leaves
				if (!(gl.gl1path && gl.gl1_v1dot1))
				{
					glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
					glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
					glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_PRIMARY_COLOR);
					glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);

					glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
					glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA, GL_TEXTURE);
					glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
				}
				else
				{
					// OpenGL 1.1 fallback: turn off the texture for the opaque solid models,
					// and if texture is with alpha (bMasked) - keep slightly compensated GL_MODULATE
					if (surfaceSkin != nullptr && !surfaceSkin->bMasked)
					{
						glDisable(GL_TEXTURE_2D);
					}
					else
					{
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					}
				}

				bool applyAlphaTest = (curSpriteMDLFrame != nullptr && curSpriteMDLFrame->useSunAlphaTest);
				if (applyAlphaTest)
				{
					glEnable(GL_ALPHA_TEST);
					glAlphaFunc(GL_GREATER, gl_mask_threshold);
				}

				glShadeModel(GL_SMOOTH);
				glEnable(GL_POLYGON_OFFSET_FILL);
				glPolygonOffset(-0.5f, -0.5f);

				// 2x multiplicative blend: 128 = no changes, >128 = sunshine, <128 = soft shadow
				glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);

				if (currentVBuf != nullptr) currentVBuf->BindVBO();
				glBindBuffer(GL_ARRAY_BUFFER, 0);

				glEnableClientState(GL_COLOR_ARRAY);
				glColorPointer(4, GL_UNSIGNED_BYTE, 0, &sunPassColors[sunPassOffsets[m]]);

				renderer->DrawElements(surf->numTriangles * 3, surf->iindex * sizeof(unsigned int));

				// State restoration
				glDisableClientState(GL_COLOR_ARRAY);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				if (applyAlphaTest)
				{
					glDisable(GL_ALPHA_TEST);
				}
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				glDisable(GL_POLYGON_OFFSET_FILL);
				glEnable(GL_TEXTURE_2D);

				if (wasFogEnabled) glEnable(GL_FOG);
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


