// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2026 Vadim Taranov
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
// The GZDoom renderer gets very slow on large maps with lots of geometry.
// The idea that works is to dump all static geometry in a set of 3D models
// of *.obj or *.md3 (faster) formats. Then generate necessary
// definitions in order to make them spawn and display as 3D-models.
// Then exlude dumped geometry from BSP-traversal. It was done earlier in
// HellRen Map15 but done entirely by hand and by gl_line_distance_cull
// to reduce BSP-traversal in the distance. What's going on here is an
// attempt to do the same but better and fully automatically.
// 1. in p_setup.cpp, void P_SetupLevel(const char *lumpname, int position,
//    bool newGame): after "	times[13].Unclock();" line place a code
//    to call "Bake" function presented here;
// 2. in d_main.cpp, in static void AddAutoloadFiles(const char *autoname):
//    autoload generated files on the next game launch;
// 3. in the same section of p_setup.cpp code after "Bake" was called,
//    place a code to spawn the actors;
//
//--------------------------------------------------------------------------
//

#include "a_staticgeombaker.h"
#include "g_levellocals.h"
#include "p_tags.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir(a,b) _mkdir (a)
#else
#include <sys/stat.h>
#endif

EXTERN_CVAR(Int, r_fakecontrast)

//==========================================================================
//
// FILE HELPERS
//
//==========================================================================

bool StaticGeometryBaker::FileExists(const FString& filename)
{
	FILE* f = fopen(filename, "r");
	if (f) { fclose(f); return true; }
	return false;
}

void StaticGeometryBaker::EnsureDirectoryExists(const FString& path)
{
	if (!DirEntryExists(path))
	{
#ifdef _WIN32
		_mkdir(path);
#else
		mkdir(path, 0755);
#endif
	}
}

//==========================================================================
//
// GET TEXTURE-LIGHT KEYS
//
//==========================================================================

void StaticGeometryBaker::GetTextureLightKeys(TArray<FString>& output)
{
	TMap<FString, TMap<int, StaticGeometryBuffer>>::Iterator texIt(staticGeometryData);
	TMap<FString, TMap<int, StaticGeometryBuffer>>::Pair* texPair;

	while (texIt.NextPair(texPair))
	{
		FString textureName = texPair->Key;
		TMap<int, StaticGeometryBuffer>& lightLevels = texPair->Value;

		TMap<int, StaticGeometryBuffer>::Iterator lightIt(lightLevels);
		TMap<int, StaticGeometryBuffer>::Pair* lightPair;

		while (lightIt.NextPair(lightPair))
		{
			int light = lightPair->Key;
			FString lightPart;
			lightPart.Format("_L%03d", light);
			output.Push("StaticGeomDump_" + CleanTextureName(textureName) + lightPart);
		}
	}
}

//==========================================================================
//
// SLOPES HANDLING
//
//==========================================================================

// Helper to calculate Z at any XY point on a slope
double StaticGeometryBaker::CalculateZAtPoint(const secplane_t* plane, double x, double y)
{
	if (plane == nullptr) return 0.0;

	// The plane is defined as: normal.X*x + normal.Y*y + normal.Z*z + D = 0
	// => z = -(normal.X*x + normal.Y*y + D) / normal.Z
	// Handle the case where normal.Z is 0 (vertical plane)

	double normalZ = plane->normal.Z;
	if (fabs(normalZ) < 1e-6)
	{
		// Vertical plane - use default height
		return 0.0;
	}

	return -(plane->normal.X * x + plane->normal.Y * y + plane->D) / normalZ;
}

// Check if a plane is sloped (using the plane's own method)
bool StaticGeometryBaker::IsPlaneSloped(const secplane_t* plane)
{
	if (plane == nullptr) return false;
	return plane->isSlope();
}

//==========================================================================
//
// GEO BAKING FUNCTIONS
//
//==========================================================================

bool StaticGeometryBaker::IsSkyFlat(FTextureID texID)
{
	if (!texID.isValid()) return false;
	FTexture* tex = TexMan[texID];
	if (!tex) return false;
	return stricmp(tex->Name, "F_SKY1") == 0 || stricmp(tex->Name, "F_SKY") == 0;
}

bool StaticGeometryBaker::IsStatic(const seg_t* seg)
{
	if (!seg->linedef) return false;
	if (seg->linedef->special != 0) return false;
	if (seg->sidedef->Flags & WALLF_POLYOBJ) return false;
	if (seg->frontsector->e->XFloor.ffloors.Size() > 0) return false;
	if (seg->backsector && seg->backsector->e->XFloor.ffloors.Size() > 0) return false;
	return true;
}

void StaticGeometryBaker::BakeTerrainFlat(sector_t* sec, int planeType)
{
	if (!sec || !sec->e) return;

	// Terrain sectors must be triangular
	if (sec->Lines.Size() != 3) return;

	FTextureID texID = sec->planes[planeType].Texture;
	if (texID.isNull()) return;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	FString texName = tex->Name;
	int rawLight = sec->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;
	if (steppedLight < 0) steppedLight = 0;
	if (steppedLight > 240) steppedLight = 240;

	// Get the three vertices of the triangle
	vertex_t* v1 = sec->Lines[0]->v1;
	vertex_t* v2 = sec->Lines[0]->v2;
	vertex_t* v3 = nullptr;

	// Find the third vertex (not shared with first line)
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		if (line->v1 != v1 && line->v1 != v2)
		{
			v3 = line->v1;
			break;
		}
		if (line->v2 != v1 && line->v2 != v2)
		{
			v3 = line->v2;
			break;
		}
	}

	if (!v3) return;

	// Get vertex heights (if available)
	// These come from vertexdatas (UDMF zfloor/zceiling)
	double z1, z2, z3;

	// Check if we have vertex height data
	// This is stored in vertexdatas during UDMF parsing
	// We need to access it via vertex indices
	unsigned v1_idx = 0, v2_idx = 0, v3_idx = 0;

	// Find vertex indices (this is inefficient but works)
	for (unsigned i = 0; i < level.vertexes.Size(); i++)
	{
		if (&level.vertexes[i] == v1) v1_idx = i;
		if (&level.vertexes[i] == v2) v2_idx = i;
		if (&level.vertexes[i] == v3) v3_idx = i;
	}

	// Check if vertex has height overrides
	bool hasV1Height = false, hasV2Height = false, hasV3Height = false;
	double v1_height = 0, v2_height = 0, v3_height = 0;

	// Access vertex height data from p_udmf.cpp
	// This is how UDMF vertex heights work
	if (v1_idx < vertexdatas.Size() && (vertexdatas[v1_idx].flags & VERTEXFLAG_ZFloorEnabled))
	{
		v1_height = vertexdatas[v1_idx].zFloor;
		hasV1Height = true;
	}
	if (v2_idx < vertexdatas.Size() && (vertexdatas[v2_idx].flags & VERTEXFLAG_ZFloorEnabled))
	{
		v2_height = vertexdatas[v2_idx].zFloor;
		hasV2Height = true;
	}
	if (v3_idx < vertexdatas.Size() && (vertexdatas[v3_idx].flags & VERTEXFLAG_ZFloorEnabled))
	{
		v3_height = vertexdatas[v3_idx].zFloor;
		hasV3Height = true;
	}

	// If we have vertex heights, use them; otherwise use sector plane
	if (planeType == sector_t::floor)
	{
		if (hasV1Height || hasV2Height || hasV3Height)
		{
			// Use vertex heights
			z1 = hasV1Height ? v1_height : sec->floorplane.ZatPoint(v1);
			z2 = hasV2Height ? v2_height : sec->floorplane.ZatPoint(v2);
			z3 = hasV3Height ? v3_height : sec->floorplane.ZatPoint(v3);
		}
		else
		{
			// Use sector plane
			z1 = sec->floorplane.ZatPoint(v1);
			z2 = sec->floorplane.ZatPoint(v2);
			z3 = sec->floorplane.ZatPoint(v3);
		}
	}
	else
	{
		// Check for ceiling heights
		bool hasV1Ceil = false, hasV2Ceil = false, hasV3Ceil = false;
		double v1_ceil = 0, v2_ceil = 0, v3_ceil = 0;

		if (v1_idx < vertexdatas.Size() && (vertexdatas[v1_idx].flags & VERTEXFLAG_ZCeilingEnabled))
		{
			v1_ceil = vertexdatas[v1_idx].zCeiling;
			hasV1Ceil = true;
		}
		if (v2_idx < vertexdatas.Size() && (vertexdatas[v2_idx].flags & VERTEXFLAG_ZCeilingEnabled))
		{
			v2_ceil = vertexdatas[v2_idx].zCeiling;
			hasV2Ceil = true;
		}
		if (v3_idx < vertexdatas.Size() && (vertexdatas[v3_idx].flags & VERTEXFLAG_ZCeilingEnabled))
		{
			v3_ceil = vertexdatas[v3_idx].zCeiling;
			hasV3Ceil = true;
		}

		if (hasV1Ceil || hasV2Ceil || hasV3Ceil)
		{
			z1 = hasV1Ceil ? v1_ceil : sec->ceilingplane.ZatPoint(v1);
			z2 = hasV2Ceil ? v2_ceil : sec->ceilingplane.ZatPoint(v2);
			z3 = hasV3Ceil ? v3_ceil : sec->ceilingplane.ZatPoint(v3);
		}
		else
		{
			z1 = sec->ceilingplane.ZatPoint(v1);
			z2 = sec->ceilingplane.ZatPoint(v2);
			z3 = sec->ceilingplane.ZatPoint(v3);
		}
	}

	// Calculate triangle bounds
	double x1 = v1->fX(), y1 = v1->fY();
	double x2 = v2->fX(), y2 = v2->fY();
	double x3 = v3->fX(), y3 = v3->fY();

	// Calculate UVs for the triangle
	double minX = MIN(x1, MIN(x2, x3));
	double maxX = MAX(x1, MAX(x2, x3));
	double minY = MIN(y1, MIN(y2, y3));
	double maxY = MAX(y1, MAX(y2, y3));

	// Calculate UVs based on triangle bounds
	double u1, v1_uv, u2, v2_uv, u3, v3_uv;
	CalculateFlatUVs(sec, planeType, minX, minY, maxX, maxY, u1, v1_uv, u2, v2_uv);

	// Map triangle vertices to UV coordinates
	// Simple mapping: project vertex positions to UV space
	double texW = tex->GetWidth();
	double texH = tex->GetHeight();
	double flatXOffset = sec->planes[planeType].xform.xOffs;
	double flatYOffset = sec->planes[planeType].xform.yOffs;
	double flatXScale = sec->planes[planeType].xform.xScale;
	double flatYScale = sec->planes[planeType].xform.yScale;

	u1 = (x1 - minX) / (texW * flatXScale) + (flatXOffset / (texW * flatXScale));
	v1_uv = (y1 - minY) / (texH * flatYScale) + (flatYOffset / (texH * flatYScale));
	u2 = (x2 - minX) / (texW * flatXScale) + (flatXOffset / (texW * flatXScale));
	v2_uv = (y2 - minY) / (texH * flatYScale) + (flatYOffset / (texH * flatYScale));
	u3 = (x3 - minX) / (texW * flatXScale) + (flatXOffset / (texW * flatXScale));
	v3_uv = (y3 - minY) / (texH * flatYScale) + (flatYOffset / (texH * flatYScale));

	// Generate geometry
	StaticGeometryBuffer& buffer = staticGeometryData[texName][steppedLight];

	// Vertices (triangle)
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		x1, z1, y1, x2, z2, y2, x3, z3, y3);

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v1_uv, u2, v2_uv, u3, v3_uv);

	// Faces (single triangle)
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2);

	buffer.VertCount += 3;
	buffer.UVCount += 3;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

void StaticGeometryBaker::BakeSlopedFlat(sector_t* sec, int planeType, const secplane_t* slopePlane, bool is3DFloor)
{
	if (!slopePlane) return;

	FTextureID texID = sec->planes[planeType].Texture;
	if (texID.isNull()) return;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	FString texName = tex->Name;
	int rawLight = sec->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;
	if (steppedLight < 0) steppedLight = 0;
	if (steppedLight > 240) steppedLight = 240;

	// Get sector bounds
	double centerX = sec->centerspot.X;
	double centerY = sec->centerspot.Y;

	double minX = 999999, maxX = -999999, minY = 999999, maxY = -999999;
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		double x1 = line->v1->fX();
		double y1 = line->v1->fY();
		double x2 = line->v2->fX();
		double y2 = line->v2->fY();
		minX = MIN(minX, x1); maxX = MAX(maxX, x1);
		minY = MIN(minY, y1); maxY = MAX(maxY, y1);
		minX = MIN(minX, x2); maxX = MAX(maxX, x2);
		minY = MIN(minY, y2); maxY = MAX(maxY, y2);
	}

	if (minX == 999999)
	{
		minX = centerX - 64.0;
		maxX = centerX + 64.0;
		minY = centerY - 64.0;
		maxY = centerY + 64.0;
	}

	double margin = 2.0;
	double x1 = minX - margin;
	double x2 = maxX + margin;
	double y1 = minY - margin;
	double y2 = maxY + margin;

	// Calculate Z values at all four corners using the slope plane
	double z11 = CalculateZAtPoint(slopePlane, x1, y1);
	double z12 = CalculateZAtPoint(slopePlane, x1, y2);
	double z21 = CalculateZAtPoint(slopePlane, x2, y1);
	double z22 = CalculateZAtPoint(slopePlane, x2, y2);

	// Calculate UVs
	double u1, v1, u2, v2;
	CalculateFlatUVs(sec, planeType, x1, y1, x2, y2, u1, v1, u2, v2);

	// Generate geometry
	// Use a special key for 3DFloor flats to separate them
	FString bufferKey;
	if (is3DFloor)
	{
		bufferKey = "3DFLOOR_" + texName;
	}
	else
	{
		bufferKey = texName;
	}

	StaticGeometryBuffer& buffer = staticGeometryData[bufferKey][steppedLight];

	// Vertices (using actual Z values from slope plane)
	if (planeType == sector_t::floor)
	{
		// Floor face - counter-clockwise order
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z11, y1, x1, z12, y2, x2, z22, y2, x2, z21, y1);
	}
	else
	{
		// Ceiling face - clockwise order
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z11, y1, x2, z21, y1, x2, z22, y2, x1, z12, y2);
	}

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v1, u1, v2, u2, v2, u2, v1);

	// Faces
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2, v, u, v + 2, u + 2, v + 3, u + 3);

	buffer.VertCount += 4;
	buffer.UVCount += 4;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

void StaticGeometryBaker::BakeFlatGeometry(sector_t* sec, int planeType)
{
	FTextureID texID = sec->planes[planeType].Texture;
	if (texID.isNull()) return;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	FString texName = tex->Name;
	int rawLight = sec->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;

	// Get sector bounds
	double centerX = sec->centerspot.X;
	double centerY = sec->centerspot.Y;

	double minX = 999999, maxX = -999999, minY = 999999, maxY = -999999;
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		double x1 = line->v1->fX();
		double y1 = line->v1->fY();
		double x2 = line->v2->fX();
		double y2 = line->v2->fY();
		minX = MIN(minX, x1); maxX = MAX(maxX, x1);
		minY = MIN(minY, y1); maxY = MAX(maxY, y1);
		minX = MIN(minX, x2); maxX = MAX(maxX, x2);
		minY = MIN(minY, y2); maxY = MAX(maxY, y2);
	}

	if (minX == 999999)
	{
		minX = centerX - 64.0;
		maxX = centerX + 64.0;
		minY = centerY - 64.0;
		maxY = centerY + 64.0;
	}

	double margin = 2.0;
	double x1 = minX - margin;
	double x2 = maxX + margin;
	double y1 = minY - margin;
	double y2 = maxY + margin;

	double z;
	if (planeType == sector_t::floor)
		z = sec->floorplane.ZatPoint(sec->centerspot);
	else
		z = sec->ceilingplane.ZatPoint(sec->centerspot);

	// Calculate UVs
	double u1, v1, u2, v2;
	CalculateFlatUVs(sec, planeType, x1, y1, x2, y2, u1, v1, u2, v2);

	// Generate geometry
	StaticGeometryBuffer& buffer = staticGeometryData[texName][steppedLight];

	// Vertices
	if (planeType == sector_t::floor)
	{
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z, y1, x1, z, y2, x2, z, y2, x2, z, y1);
	}
	else
	{
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z, y1, x2, z, y1, x2, z, y2, x1, z, y2);
	}

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v1, u1, v2, u2, v2, u2, v1);

	// Faces
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2, v, u, v + 2, u + 2, v + 3, u + 3);

	buffer.VertCount += 4;
	buffer.UVCount += 4;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

void StaticGeometryBaker::BakeWallGeometry(line_t* line, side_t* side)
{
	FTextureID texID = side->GetTexture(side_t::mid);
	if (texID.isNull()) return;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	FString texName = tex->Name;
	sector_t* front = line->frontsector;
	sector_t* back = line->backsector;
	if (!front) return;

	// Check sky conditions
	bool frontHasSkyCeil = IsSkyFlat(front->planes[sector_t::ceiling].Texture);
	bool frontHasSkyFloor = IsSkyFlat(front->planes[sector_t::floor].Texture);
	bool backHasSkyCeil = back && IsSkyFlat(back->planes[sector_t::ceiling].Texture);
	bool backHasSkyFloor = back && IsSkyFlat(back->planes[sector_t::floor].Texture);

	// Skip if both sides sky
	if (frontHasSkyCeil && backHasSkyCeil) return;
	if (frontHasSkyFloor && backHasSkyFloor) return;

	// Calculate heights
	double x1 = line->v1->fX(), y1 = line->v1->fY();
	double x2 = line->v2->fX(), y2 = line->v2->fY();

	double z1_top, z1_bot, z2_top, z2_bot;

	// Get base heights from the side's sector (could be front or back)
	sector_t* wallSec = side->sector;
	if (!wallSec) wallSec = front;

	// Check if we need to calculate slopes for this wall
	// This depends on whether the line is being used for Plane_Align/Plane_Copy
	// For now, we'll use the sector's current plane state
	// (slopes should already be set by GZDoom during map init)

	z1_top = wallSec->ceilingplane.ZatPoint(line->v1);
	z1_bot = wallSec->floorplane.ZatPoint(line->v1);
	z2_top = wallSec->ceilingplane.ZatPoint(line->v2);
	z2_bot = wallSec->floorplane.ZatPoint(line->v2);

	// Trim sky areas
	if (frontHasSkyCeil)
	{
		if (back && !backHasSkyCeil)
		{
			z1_top = back->ceilingplane.ZatPoint(line->v1);
			z2_top = back->ceilingplane.ZatPoint(line->v2);
		}
		else if (!back) return;
	}

	if (frontHasSkyFloor)
	{
		if (back && !backHasSkyFloor)
		{
			z1_bot = back->floorplane.ZatPoint(line->v1);
			z2_bot = back->floorplane.ZatPoint(line->v2);
		}
		else if (!back) return;
	}

	if (z1_top <= z1_bot || z2_top <= z2_bot) return;

	// Calculate base light
	int rawLight = wallSec->lightlevel;
	if (rawLight > 255) rawLight = 255;

	// Apply fake contrast BEFORE stepping
	ApplyFakeContrast(rawLight, line, side);

	int steppedLight = (rawLight / 16) * 16;
	if (steppedLight < 0) steppedLight = 0;
	if (steppedLight > 240) steppedLight = 240;

	// Create a temporary seg_t for UV calculation
	seg_t tempSeg;
	tempSeg.v1 = line->v1;
	tempSeg.v2 = line->v2;
	tempSeg.sidedef = side;
	tempSeg.linedef = line;
	tempSeg.frontsector = front;
	tempSeg.backsector = back;

	// UVs
	double u1, v1, u2, v2;
	CalculateWallUVs(&tempSeg, u1, u2, v1, v2);

	// Generate geometry
	StaticGeometryBuffer& buffer = staticGeometryData[texName][steppedLight];

	// Vertices
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		x1, z1_top, y1, x1, z1_bot, y1, x2, z2_bot, y2, x2, z2_top, y2);

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v2, u1, v1, u2, v1, u2, v2);

	// Faces
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2, v, u, v + 2, u + 2, v + 3, u + 3);

	buffer.VertCount += 4;
	buffer.UVCount += 4;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

//==========================================================================
//
// BAKE 3D FLOORS
//
//==========================================================================

void StaticGeometryBaker::Bake3DFloorWall(seg_t* seg, F3DFloor* ffloor)
{
	if (!seg || !seg->sidedef || !ffloor || !ffloor->model) return;

	// Get surface texture (different from floor texture)
	FTexture* sideTex = TexMan[seg->sidedef->GetTexture(side_t::mid)];
	if (!sideTex) return;

	FString texName = sideTex->Name;
	sector_t* wallSec = seg->sidedef->sector;
	if (!wallSec) wallSec = seg->frontsector;

	// Check if the wall is part of a 3DFloor that has slope from dummy sector
	// The 3DFloor's dummy sector might have slopes from line specials
	secplane_t* slopePlane = &ffloor->model->floorplane; // Assume floor slope for now

	// Get light from the 3DFloor's sector
	int rawLight = seg->frontsector->lightlevel;
	if (rawLight > 255) rawLight = 255;

	// Apply fake contrast
	ApplyFakeContrast(rawLight, seg->linedef, seg->sidedef);

	int steppedLight = (rawLight / 16) * 16;
	if (steppedLight < 0) steppedLight = 0;
	if (steppedLight > 240) steppedLight = 240;

	// Calculate 3D floor wall geometry
	double x1 = seg->v1->fX(), y1 = seg->v1->fY();
	double x2 = seg->v2->fX(), y2 = seg->v2->fY();

	// For 3DFloor walls, we need to get the actual heights
	// The wall goes from floor height to ceiling height of the 3DFloor
	double z_bottom = ffloor->model->floorplane.ZatPoint(seg->v1);
	double z_top = ffloor->model->ceilingplane.ZatPoint(seg->v1);

	// Apply slope to the wall if the 3DFloor has slope
	// Note: 3DFloor walls are vertical, so slope doesn't affect their top/bottom
	// But the position might be affected if we consider the sloped floor

	// Skip if height is invalid
	if (z_top <= z_bottom) return;

	// Calculate UVs for 3D floor wall
	double u1, v1, u2, v2;
	CalculateWallUVs(seg, u1, u2, v1, v2);

	// Generate geometry with special prefix for 3D floors
	FString textureKey = "3DFLOOR_" + texName;
	StaticGeometryBuffer& buffer = staticGeometryData[textureKey][steppedLight];

	// Vertices - 3DFloor walls are vertical, so use the Z values directly
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		x1, z_top, y1, x1, z_bottom, y1, x2, z_bottom, y2, x2, z_top, y2);

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v2, u1, v1, u2, v1, u2, v2);

	// Faces
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2, v, u, v + 2, u + 2, v + 3, u + 3);

	buffer.VertCount += 4;
	buffer.UVCount += 4;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

void StaticGeometryBaker::Process3DFloorWalls()
{
	for (unsigned i = 0; i < level.subsectors.Size(); i++)
	{
		subsector_t* sub = &level.subsectors[i];
		for (unsigned int j = 0; j < sub->numlines; j++)
		{
			seg_t* seg = &sub->firstline[j];
			if (!seg->linedef) continue;

			// Check for 3D floors
			if (!seg->frontsector || !seg->frontsector->e) continue;

			for (unsigned k = 0; k < seg->frontsector->e->XFloor.ffloors.Size(); k++)
			{
				F3DFloor* ffloor = seg->frontsector->e->XFloor.ffloors[k];
				if (!(ffloor->flags & FF_EXISTS) || !(ffloor->flags & FF_SOLID)) continue;

				// Check if this is a wall segment of the 3D floor
				// 3DFloor walls are vertical surfaces at the edges of the 3DFloor
				if (ffloor->master && ffloor->master == seg->linedef)
				{
					Bake3DFloorWall(seg, ffloor);
				}
			}
		}
	}
}

void StaticGeometryBaker::Bake3DFloorFlat(sector_t* dummySec, sector_t* targetSec, int planeType, F3DFloor* ffloor)
{
	if (!dummySec || !ffloor || !ffloor->model) return;

	// Get surface texture from dummy sector
	FTexture* surfaceTex = TexMan[dummySec->planes[planeType].Texture];
	if (!surfaceTex) return;

	FString surfaceTexName = surfaceTex->Name;
	int rawLight = targetSec->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;
	if (steppedLight < 0) steppedLight = 0;
	if (steppedLight > 240) steppedLight = 240;

	// Calculate bounds from target sector
	double minX = 999999, maxX = -999999, minY = 999999, maxY = -999999;
	for (auto& line : targetSec->Lines)
	{
		minX = MIN(minX, line->v1->fX()); maxX = MAX(maxX, line->v1->fX());
		minY = MIN(minY, line->v1->fY()); maxY = MAX(maxY, line->v1->fY());
		minX = MIN(minX, line->v2->fX()); maxX = MAX(maxX, line->v2->fX());
		minY = MIN(minY, line->v2->fY()); maxY = MAX(maxY, line->v2->fY());
	}

	if (minX == 999999) return;

	// Add margin
	double margin = 2.0;
	double x1 = minX - margin;
	double x2 = maxX + margin;
	double y1 = minY - margin;
	double y2 = maxY + margin;

	// CRITICAL: Get slope plane from the DUMMY SECTOR
	// The dummy sector inherits slopes from line specials (Plane_Align, Plane_Copy, etc.)
	const secplane_t* slopePlane = planeType == sector_t::floor ?
		&dummySec->floorplane : &dummySec->ceilingplane;

	// Check if the dummy sector has a slope
	bool hasSlope = IsPlaneSloped(slopePlane);

	// Calculate Z values at all four corners
	double z11, z12, z21, z22;

	if (hasSlope)
	{
		// Use the sloped plane from the dummy sector
		z11 = CalculateZAtPoint(slopePlane, x1, y1);
		z12 = CalculateZAtPoint(slopePlane, x1, y2);
		z21 = CalculateZAtPoint(slopePlane, x2, y1);
		z22 = CalculateZAtPoint(slopePlane, x2, y2);
	}
	else
	{
		// Flat 3DFloor - use the model's height
		double flatHeight = dummySec->GetPlaneTexZ(planeType);
		z11 = flatHeight;
		z12 = flatHeight;
		z21 = flatHeight;
		z22 = flatHeight;
	}

	// Calculate UVs
	double u1, v1, u2, v2;
	CalculateFlatUVs(dummySec, planeType, x1, y1, x2, y2, u1, v1, u2, v2);

	// Generate geometry
	FString layerKey = "3DFLOOR_" + surfaceTexName;
	StaticGeometryBuffer& buffer = staticGeometryData[layerKey][steppedLight];

	// Vertices
	if (planeType == sector_t::floor)
	{
		// Top face
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z11, y1, x2, z21, y1, x2, z22, y2, x1, z12, y2);
	}
	else
	{
		// Bottom face
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z11, y1, x1, z12, y2, x2, z22, y2, x2, z21, y1);
	}

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v1, u1, v2, u2, v2, u2, v1);

	// Faces
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2, v, u, v + 2, u + 2, v + 3, u + 3);

	buffer.VertCount += 4;
	buffer.UVCount += 4;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", surfaceTexName.GetChars());
	}
}

void StaticGeometryBaker::BakeSectorGeometry(sector_t* sec)
{
	if (!sec->e) return;

	// Process 3D floors FIRST
	// 3DFloors can have slopes from their dummy sectors
	for (unsigned j = 0; j < sec->e->XFloor.ffloors.Size(); j++)
	{
		F3DFloor* ffloor = sec->e->XFloor.ffloors[j];
		if (!(ffloor->flags & FF_EXISTS) || !(ffloor->flags & FF_SOLID)) continue;

		// Bake 3D floor flats with their own slopes (from dummy sector)
		Bake3DFloorFlat(ffloor->model, sec, sector_t::floor, ffloor);
		Bake3DFloorFlat(ffloor->model, sec, sector_t::ceiling, ffloor);
	}

	// Check for terrain (triangular sectors with vertex heights)
	// Terrain sectors are triangular and use vertex heights (zfloor/zceiling)
	bool isTerrain = (sec->Lines.Size() == 3);

	// Process regular floor
	FTextureID floorTexID = sec->planes[sector_t::floor].Texture;
	if (!floorTexID.isNull() && !IsSkyFlat(floorTexID))
	{
		if (isTerrain)
		{
			// Terrain sector - check if it has vertex heights
			// If it has vertex heights, use terrain baking
			// If it's just a sloped plane, use slope baking
			bool hasVertexHeights = false;
			for (unsigned i = 0; i < sec->Lines.Size(); i++)
			{
				vertex_t* v = sec->Lines[i]->v1;
				// Check if vertex has height data
				unsigned idx = 0;
				for (unsigned j = 0; j < level.vertexes.Size(); j++)
				{
					if (&level.vertexes[j] == v)
					{
						idx = j;
						break;
					}
				}
				if (idx < vertexdatas.Size())
				{
					if ((vertexdatas[idx].flags & VERTEXFLAG_ZFloorEnabled) ||
						(vertexdatas[idx].flags & VERTEXFLAG_ZCeilingEnabled))
					{
						hasVertexHeights = true;
						break;
					}
				}
			}

			if (hasVertexHeights)
			{
				// True terrain with vertex heights
				BakeTerrainFlat(sec, sector_t::floor);
			}
			else if (IsPlaneSloped(&sec->floorplane))
			{
				// Sloped floor (from plane equation)
				BakeSlopedFlat(sec, sector_t::floor, &sec->floorplane, false);
			}
			else
			{
				// Flat terrain
				BakeFlatGeometry(sec, sector_t::floor);
			}
		}
		else if (IsPlaneSloped(&sec->floorplane))
		{
			// Sloped floor (from plane equation or line specials)
			BakeSlopedFlat(sec, sector_t::floor, &sec->floorplane, false);
		}
		else
		{
			// Flat floor
			BakeFlatGeometry(sec, sector_t::floor);
		}
	}

	// Process regular ceiling
	FTextureID ceilTexID = sec->planes[sector_t::ceiling].Texture;
	if (!ceilTexID.isNull() && !IsSkyFlat(ceilTexID))
	{
		if (isTerrain)
		{
			bool hasVertexHeights = false;
			for (unsigned i = 0; i < sec->Lines.Size(); i++)
			{
				vertex_t* v = sec->Lines[i]->v1;
				unsigned idx = 0;
				for (unsigned j = 0; j < level.vertexes.Size(); j++)
				{
					if (&level.vertexes[j] == v)
					{
						idx = j;
						break;
					}
				}
				if (idx < vertexdatas.Size())
				{
					if ((vertexdatas[idx].flags & VERTEXFLAG_ZCeilingEnabled) ||
						(vertexdatas[idx].flags & VERTEXFLAG_ZFloorEnabled))
					{
						hasVertexHeights = true;
						break;
					}
				}
			}

			if (hasVertexHeights)
			{
				// True terrain with vertex heights
				BakeTerrainFlat(sec, sector_t::ceiling);
			}
			else if (IsPlaneSloped(&sec->ceilingplane))
			{
				// Sloped ceiling (from plane equation)
				BakeSlopedFlat(sec, sector_t::ceiling, &sec->ceilingplane, false);
			}
			else
			{
				// Flat terrain
				BakeFlatGeometry(sec, sector_t::ceiling);
			}
		}
		else if (IsPlaneSloped(&sec->ceilingplane))
		{
			// Sloped ceiling (from plane equation or line specials)
			BakeSlopedFlat(sec, sector_t::ceiling, &sec->ceilingplane, false);
		}
		else
		{
			// Flat ceiling
			BakeFlatGeometry(sec, sector_t::ceiling);
		}
	}
}

//==========================================================================
//
// FAKE CONTRAST
//
//==========================================================================

int StaticGeometryBaker::CalculateFakeContrast(line_t* line, side_t* side)
{
	if (!line || !side) return 0;

	// For now, we'll use level flags as proxy
	bool useSmooth = (level.flags2 & LEVEL2_SMOOTHLIGHTING) || (side->Flags & WALLF_SMOOTHLIGHTING) || r_fakecontrast == 2;

	if (!(side->Flags & WALLF_NOFAKECONTRAST) && r_fakecontrast != 0)
	{
		DVector2 delta = line->Delta();
		int rel = 0;

		if (useSmooth && delta.X != 0)
		{
			// Smooth lighting (option 2)
			double angle = atan2(delta.Y, delta.X);
			rel = xs_RoundToInt(level.WallHorizLight + fabs(angle / 1.57079) * (level.WallVertLight - level.WallHorizLight));
		}
		else
		{
			// Option 1 (default)
			if (delta.X == 0)
				rel = level.WallVertLight;
			else if (delta.Y == 0)
				rel = level.WallHorizLight;
			else
				rel = 0;
		}

		return rel;
	}

	return 0;
}

void StaticGeometryBaker::ApplyFakeContrast(int& lightLevel, line_t* line, side_t* side)
{
	if (!line || !side) return;

	// Check flags for disabled fake contrast
	if (side->Flags & WALLF_NOFAKECONTRAST) return;
	if (level.flags3 & LEVEL3_FORCEFAKECONTRAST) // Force it even if disabled
	{
		// Always apply
	}

	// Apply fake contrast
	int fakeContrast = CalculateFakeContrast(line, side);
	lightLevel += fakeContrast;

	// Clamp to valid range
	if (lightLevel < 0) lightLevel = 0;
	if (lightLevel > 255) lightLevel = 255;
}

//==========================================================================
//
// UV HELPERS
//
//==========================================================================

void StaticGeometryBaker::CalculateWallUVs(const seg_t* seg, double& u1, double& u2, double& v1, double& v2)
{
	FTexture* tex = TexMan[seg->sidedef->GetTexture(side_t::mid)];
	if (!tex) return;

	// Get texture properties
	int texWidth = tex->GetWidth();
	int texHeight = tex->GetHeight();
	bool worldPanning = tex->bWorldPanning || (level.flags3 & LEVEL3_FORCEWORLDPANNING);

	// Get side texture offset and scale
	double texXOffset = seg->sidedef->textures[side_t::mid].xOffset;
	double texYOffset = seg->sidedef->textures[side_t::mid].yOffset;
	double texXScale = seg->sidedef->textures[side_t::mid].xScale;
	double texYScale = seg->sidedef->textures[side_t::mid].yScale;

	// Calculate wall length
	double wallLen = seg->linedef->Delta().Length();
	double wallHeight = seg->frontsector->ceilingplane.ZatPoint(seg->v1) -
		seg->frontsector->floorplane.ZatPoint(seg->v1);

	// UV calculation
	double uOffset = worldPanning ? (texXOffset / texWidth) : (texXOffset / texWidth);
	double uScale = 1.0 / texXScale;
	u1 = uOffset + (0.0 * uScale);
	u2 = uOffset + (wallLen / texWidth * uScale);

	double vOffset = worldPanning ? (texYOffset / texHeight) : (texYOffset / texHeight);
	double vScale = 1.0 / texYScale;
	v1 = vOffset;
	v2 = vOffset + (wallHeight / texHeight * vScale);
}

void StaticGeometryBaker::CalculateFlatUVs(sector_t* sec, int planeType, double x1, double y1, double x2, double y2,
	double& u1, double& v1, double& u2, double& v2)
{
	FTextureID texID = sec->planes[planeType].Texture;
	if (texID.isNull()) return;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	int texW = tex->GetWidth();
	int texH = tex->GetHeight();

	double flatXOffset = sec->planes[planeType].xform.xOffs;
	double flatYOffset = sec->planes[planeType].xform.yOffs;
	double flatXScale = sec->planes[planeType].xform.xScale;
	double flatYScale = sec->planes[planeType].xform.yScale;

	u1 = x1 / (texW * flatXScale) + (flatXOffset / (texW * flatXScale));
	v1 = y1 / (texH * flatYScale) + (flatYOffset / (texH * flatYScale));
	u2 = x2 / (texW * flatXScale) + (flatXOffset / (texW * flatXScale));
	v2 = y2 / (texH * flatYScale) + (flatYOffset / (texH * flatYScale));
}

//==========================================================================
//
// DECORATE/MODELDEF GENERATION
//
//==========================================================================

void StaticGeometryBaker::GenerateDecorateContent(FString& content)
{
	content = "// Auto-generated static geometry actors\n\n";

	TMap<FString, TMap<int, StaticGeometryBuffer>>::Iterator texIt(staticGeometryData);
	TMap<FString, TMap<int, StaticGeometryBuffer>>::Pair* texPair;

	while (texIt.NextPair(texPair))
	{
		FString textureName = texPair->Key;
		FString cleanName = CleanTextureName(textureName);
		TMap<int, StaticGeometryBuffer>& lightLevels = texPair->Value;

		TMap<int, StaticGeometryBuffer>::Iterator lightIt(lightLevels);
		TMap<int, StaticGeometryBuffer>::Pair* lightPair;

		while (lightIt.NextPair(lightPair))
		{
			int lightLevel = lightPair->Key;
			content.AppendFormat("ACTOR StaticGeomDump_%s_L%03d\n", cleanName.GetChars(), lightLevel);
			content += "{\n";
			content += "  Height 16\n";
			content += "  Radius 16\n";
			content += "  RenderRadius 2048.0\n\n";
			content += "  +NOGRAVITY\n";
			content += "  +NOCLIP\n";
			content += "  +NOBLOCKMAP\n";
			content += "  States\n";
			content += "  {\n";
			content += "      Spawn:\n";
			content += "          SP1R A -1\n";
			content += "          Stop\n";
			content += "  }\n";
			content += "}\n\n";
		}
	}
}

void StaticGeometryBaker::GenerateModelDefContent(FString& content)
{
	content = "// Auto-generated static geometry models\n\n";

	TMap<FString, TMap<int, StaticGeometryBuffer>>::Iterator texIt(staticGeometryData);
	TMap<FString, TMap<int, StaticGeometryBuffer>>::Pair* texPair;

	while (texIt.NextPair(texPair))
	{
		FString textureName = texPair->Key;
		FString cleanName = CleanTextureName(textureName);
		TMap<int, StaticGeometryBuffer>& lightLevels = texPair->Value;

		TMap<int, StaticGeometryBuffer>::Iterator lightIt(lightLevels);
		TMap<int, StaticGeometryBuffer>::Pair* lightPair;

		while (lightIt.NextPair(lightPair))
		{
			int lightLevel = lightPair->Key;
			content.AppendFormat("Model StaticGeomDump_%s_L%03d\n{\n", cleanName.GetChars(), lightLevel);
			content += "    Path \"Models/GeomDump\"\n";
			content.AppendFormat("    Model 0 \"%s_L%03d.obj\"\n", textureName.GetChars(), lightLevel);
			content.AppendFormat("    Skin 0 \"%s\"\n", textureName.GetChars());
			content += "    Scale 1.0 1.0 1.0\n    ZOffset 0.0\n";
			content += "    FrameIndex SP1R A 0 0\n}\n\n";
		}
	}
}

void StaticGeometryBaker::GenerateStaticFiles()
{
	FString decorateContent;
	FString modeldefContent;

	GenerateDecorateContent(decorateContent);
	GenerateModelDefContent(modeldefContent);

	// Write files with correct extensions
	FileWriter* decoFile = FileWriter::Open("MdlDump/decorate.staticgeomdmp");
	if (decoFile)
	{
		decoFile->Write(decorateContent.GetChars(), decorateContent.Len());
		delete decoFile;
	}

	FileWriter* modelDefFile = FileWriter::Open("MdlDump/modeldef.staticgeomdmp");
	if (modelDefFile)
	{
		modelDefFile->Write(modeldefContent.GetChars(), modeldefContent.Len());
		delete modelDefFile;
	}
}

//==========================================================================
//
// MAIN BAKE FUNCTION
//
//==========================================================================

void StaticGeometryBaker::Bake()
{
	if (baked) return;
	baked = true;
	bakedLinedefs.Clear();
	staticGeometryData.Clear();

	// Process all sectors FIRST
	// This is when slopes are read from sector planes
	// (GZDoom already processed Plane_Align, Plane_Copy, etc.)
	for (unsigned i = 0; i < level.sectors.Size(); i++)
	{
		sector_t* sec = &level.sectors[i];
		BakeSectorGeometry(sec);
	}

	// Process all subsectors to get segs (regular walls)
	for (unsigned i = 0; i < level.subsectors.Size(); i++)
	{
		subsector_t* sub = &level.subsectors[i];
		for (unsigned int j = 0; j < sub->numlines; j++)
		{
			seg_t* seg = &sub->firstline[j];
			if (IsStatic(seg) && bakedLinedefs.Find(seg->linedef) == bakedLinedefs.Size())
			{
				bakedLinedefs.Push(seg->linedef);
				BakeWallGeometry(seg->linedef, seg->sidedef);
			}
		}
	}

	// Process 3DFloor walls separately
	Process3DFloorWalls();

	// Write OBJ files
	FString baseDir = "MdlDump/Models/GeomDump/";
	EnsureDirectoryExists("MdlDump");
	EnsureDirectoryExists("MdlDump/Models");
	EnsureDirectoryExists(baseDir);

	TMap<FString, TMap<int, StaticGeometryBuffer>>::Iterator texIt(staticGeometryData);
	TMap<FString, TMap<int, StaticGeometryBuffer>>::Pair* texPair;

	while (texIt.NextPair(texPair))
	{
		FString textureName = texPair->Key;
		TMap<int, StaticGeometryBuffer>& lightLevels = texPair->Value;

		TMap<int, StaticGeometryBuffer>::Iterator lightIt(lightLevels);
		TMap<int, StaticGeometryBuffer>::Pair* lightPair;

		while (lightIt.NextPair(lightPair))
		{
			int lightLevel = lightPair->Key;
			StaticGeometryBuffer& buffer = lightPair->Value;

			if (buffer.Vertices.IsEmpty()) continue;

			FString cleanName = CleanTextureName(textureName);
			FString filename;
			filename.Format("%s%s_L%03d.obj", baseDir.GetChars(), cleanName.GetChars(), lightLevel);

			FileWriter* file = FileWriter::Open(filename);
			if (file)
			{
				file->Printf("# Static Geometry: %s (Light %d)\n", cleanName.GetChars(), lightLevel);
				file->Printf("%s", buffer.Materials.GetChars());
				file->Write(buffer.Vertices.GetChars(), buffer.Vertices.Len());
				file->Write(buffer.UVs.GetChars(), buffer.UVs.Len());
				file->Write(buffer.Faces.GetChars(), buffer.Faces.Len());
				delete file;
			}
		}
	}

	// Generate DECORATE and MODELDEF
	GenerateStaticFiles();
}

// Global instance
StaticGeometryBaker GStaticBaker;