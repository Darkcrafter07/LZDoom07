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

// Static member definition
DVector2 StaticGeometryBaker::StaticBakerSortedCenter;

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

void StaticGeometryBaker::BakeSectorGeometry(sector_t* sec)
{
	if (!sec->e) return;

	// Process 3D floors first
	for (unsigned j = 0; j < sec->e->XFloor.ffloors.Size(); j++)
	{
		F3DFloor* ffloor = sec->e->XFloor.ffloors[j];
		if (!(ffloor->flags & FF_EXISTS) || !(ffloor->flags & FF_SOLID)) continue;

		// Bake 3D floor geometry
		Bake3DFloor(sec, ffloor->model, ffloor->master, ffloor->alpha, ffloor->flags);
	}

	// Process regular floor
	FTextureID floorTexID = sec->planes[sector_t::floor].Texture;
	if (!floorTexID.isNull() && !IsSkyFlat(floorTexID))
	{
		BakeFlatGeometry(sec, sector_t::floor);
	}

	// Process regular ceiling
	FTextureID ceilTexID = sec->planes[sector_t::ceiling].Texture;
	if (!ceilTexID.isNull() && !IsSkyFlat(ceilTexID))
	{
		BakeFlatGeometry(sec, sector_t::ceiling);
	}
}

void StaticGeometryBaker::BakeLineGeometry(line_t* line)
{
	for (int i = 0; i < 2; i++)
	{
		side_t* side = line->sidedef[i];
		if (!side) continue;

		// Skip if already processed
		bool found = false;
		for (unsigned k = 0; k < bakedLinedefs.Size(); k++)
		{
			if (bakedLinedefs[k] == line)
			{
				found = true;
				break;
			}
		}
		if (found) continue;

		// Check if line is static
		if (line->special != 0) continue;
		if (side->Flags & WALLF_POLYOBJ) continue;
		if (line->frontsector && line->frontsector->e->XFloor.ffloors.Size() > 0) continue;
		if (line->backsector && line->backsector->e->XFloor.ffloors.Size() > 0) continue;

		bakedLinedefs.Push(line);
		BakeWallGeometry(line, side);
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
	CalculateFlatUVs(sec, x1, y1, x2, y2, u1, v1, u2, v2);

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

	double z1_top = front->ceilingplane.ZatPoint(line->v1);
	double z1_bot = front->floorplane.ZatPoint(line->v1);
	double z2_top = front->ceilingplane.ZatPoint(line->v2);
	double z2_bot = front->floorplane.ZatPoint(line->v2);

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

	// Light
	int rawLight = front->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;

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

void StaticGeometryBaker::Bake3DFloor(sector_t* dummySec, sector_t* targetSec, line_t* masterLine, int alpha, int flags)
{
	// Get surface texture
	FTexture* surfaceTex = TexMan[dummySec->planes[sector_t::floor].Texture];
	if (!surfaceTex) return;

	FString surfaceTexName = surfaceTex->Name;
	int steppedLight = (targetSec->lightlevel / 16) * 16;

	// Calculate bounds
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

	// Calculate UVs
	double u1, v1, u2, v2;
	CalculateFlatUVs(dummySec, x1, y1, x2, y2, u1, v1, u2, v2);

	// Generate geometry
	FString layerKey = "F3D_" + surfaceTexName;
	StaticGeometryBuffer& buffer = staticGeometryData[layerKey][steppedLight];

	// Top face
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		x1, dummySec->ceilingplane.ZatPoint(dummySec->centerspot), y1,
		x2, dummySec->ceilingplane.ZatPoint(dummySec->centerspot), y1,
		x2, dummySec->ceilingplane.ZatPoint(dummySec->centerspot), y2,
		x1, dummySec->ceilingplane.ZatPoint(dummySec->centerspot), y2);

	// Bottom face
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		x1, dummySec->floorplane.ZatPoint(dummySec->centerspot), y1,
		x1, dummySec->floorplane.ZatPoint(dummySec->centerspot), y2,
		x2, dummySec->floorplane.ZatPoint(dummySec->centerspot), y2,
		x2, dummySec->floorplane.ZatPoint(dummySec->centerspot), y1);

	// UVs
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v1, u2, v1, u2, v2, u1, v2);
	buffer.UVs.AppendFormat("vt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\nvt %.6f %.6f\n",
		u1, v1, u1, v2, u2, v2, u2, v1);

	// Faces
	int v = buffer.VertCount;
	int u = buffer.UVCount;
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v, u, v + 1, u + 1, v + 2, u + 2, v, u, v + 2, u + 2, v + 3, u + 3);
	buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\nf %d/%d %d/%d %d/%d\n",
		v + 4, u + 4, v + 5, u + 5, v + 6, u + 6, v + 4, u + 4, v + 6, u + 6, v + 7, u + 7);

	buffer.VertCount += 8;
	buffer.UVCount += 8;

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", surfaceTexName.GetChars());
	}
}

//==========================================================================
//
// UV CALCULATION HELPERS
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

void StaticGeometryBaker::CalculateFlatUVs(sector_t* sec, double x1, double y1, double x2, double y2, double& u1, double& v1, double& u2, double& v2)
{
	FTextureID texID = sec->planes[sector_t::floor].Texture;
	if (texID.isNull()) texID = sec->planes[sector_t::ceiling].Texture;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	int texW = tex->GetWidth();
	int texH = tex->GetHeight();

	double flatXOffset = sec->planes[sector_t::floor].xform.xOffs;
	double flatYOffset = sec->planes[sector_t::floor].xform.yOffs;
	double flatXScale = sec->planes[sector_t::floor].xform.xScale;
	double flatYScale = sec->planes[sector_t::floor].xform.yScale;

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

//==========================================================================
//
// BAKE 3D FLOORS
//
//==========================================================================

void StaticGeometryBaker::Bake3DFloorWall(seg_t* seg, F3DFloor* ffloor)
{
	if (!seg || !seg->sidedef || !ffloor) return;

	// Get surface texture (different from floor texture)
	FTexture* sideTex = TexMan[seg->sidedef->GetTexture(side_t::mid)];
	if (!sideTex) return;

	FString texName = sideTex->Name;
	int rawLight = seg->frontsector->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;

	// Calculate 3D floor wall geometry
	double x1 = seg->v1->fX(), y1 = seg->v1->fY();
	double x2 = seg->v2->fX(), y2 = seg->v2->fY();

	// Get 3D floor height
	double z_bottom = ffloor->model->floorplane.ZatPoint(ffloor->model->centerspot);
	double z_top = ffloor->model->ceilingplane.ZatPoint(ffloor->model->centerspot);

	// Skip if height is invalid
	if (z_top <= z_bottom) return;

	// Calculate UVs for 3D floor wall
	double u1, v1, u2, v2;
	CalculateWallUVs(seg, u1, u2, v1, v2);

	// Generate geometry with special prefix for 3D floors
	FString textureKey = "3DFLOOR_" + texName;
	StaticGeometryBuffer& buffer = staticGeometryData[textureKey][steppedLight];

	// Vertices
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
		for (int j = 0; j < sub->numlines; j++)
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
				if (ffloor->master && ffloor->master == seg->linedef)
				{
					Bake3DFloorWall(seg, ffloor);
				}
			}
		}
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

	// Process all sectors
	for (unsigned i = 0; i < level.sectors.Size(); i++)
	{
		sector_t* sec = &level.sectors[i];
		BakeSectorGeometry(sec);
	}

	// Process all subsectors to get segs (regular walls)
	for (unsigned i = 0; i < level.subsectors.Size(); i++)
	{
		subsector_t* sub = &level.subsectors[i];
		for (int j = 0; j < sub->numlines; j++)
		{
			seg_t* seg = &sub->firstline[j];
			if (IsStatic(seg))
			{
				// Skip if already processed
				bool found = false;
				for (unsigned k = 0; k < bakedLinedefs.Size(); k++)
				{
					if (bakedLinedefs[k] == seg->linedef)
					{
						found = true;
						break;
					}
				}
				if (found) continue;

				bakedLinedefs.Push(seg->linedef);

				// Process wall geometry
				side_t* side = seg->sidedef;
				if (side)
				{
					FTextureID texID = side->GetTexture(side_t::mid);
					if (!texID.isNull())
					{
						FTexture* tex = TexMan[texID];
						if (tex)
						{
							FString texName = tex->Name;
							int rawLight = seg->frontsector->lightlevel;
							if (rawLight > 255) rawLight = 255;
							int steppedLight = (rawLight / 16) * 16;

							// Calculate UVs
							double u1, v1, u2, v2;
							CalculateWallUVs(seg, u1, u2, v1, v2);

							// Generate geometry
							StaticGeometryBuffer& buffer = staticGeometryData[texName][steppedLight];

							// Vertices
							double x1 = seg->v1->fX(), y1 = seg->v1->fY();
							double x2 = seg->v2->fX(), y2 = seg->v2->fY();
							double z1_top = seg->frontsector->ceilingplane.ZatPoint(seg->v1);
							double z1_bot = seg->frontsector->floorplane.ZatPoint(seg->v1);
							double z2_top = seg->frontsector->ceilingplane.ZatPoint(seg->v2);
							double z2_bot = seg->frontsector->floorplane.ZatPoint(seg->v2);

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
					}
				}
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