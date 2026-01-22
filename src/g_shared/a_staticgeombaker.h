#pragma once

#include "tarray.h"
#include "r_defs.h"
#include "textures.h"

// Structure to hold geometry data for OBJ generation
struct StaticGeometryBuffer
{
	FString Vertices;
	FString UVs;
	FString Faces;
	FString Materials;

	int VertCount = 1;
	int UVCount = 1;
};

class StaticGeometryBaker
{
public:
	bool IsStatic(const seg_t* seg);
	void Bake3DFloorWall(seg_t* seg, F3DFloor* ffloor);
	void Process3DFloorWalls();
	void Bake();

	// Clean texture name for actor/model naming
	static FString CleanTextureName(const FString& texName)
	{
		FString clean = texName;
		clean.ReplaceChars('.', 'd');
		clean.ReplaceChars('/', '_');
		clean.ReplaceChars('\\', '_');
		clean.ReplaceChars(' ', '_');
		return clean;
	}

	// Get all unique texture-light combinations for actor spawning
	void GetTextureLightKeys(TArray<FString>& output);

private:
	// File helpers
	bool FileExists(const FString& filename);
	void EnsureDirectoryExists(const FString& path);

	// DECORATE/MODELDEF generation
	void GenerateStaticFiles();
	void GenerateDecorateContent(FString& content);
	void GenerateModelDefContent(FString& content);

	// Geometry baking helpers
	void BakeSectorGeometry(sector_t* sec);
	void BakeLineGeometry(line_t* line);
	void Bake3DFloor(sector_t* dummySec, sector_t* targetSec, line_t* masterLine, int alpha, int flags);
	void BakeFlatGeometry(sector_t* sec, int planeType);
	void BakeWallGeometry(line_t* line, side_t* side);
	bool IsSkyFlat(FTextureID texID);

	// UV calculation helpers
	void CalculateWallUVs(const seg_t* seg, double& u1, double& u2, double& v1, double& v2);
	void CalculateFlatUVs(sector_t* sec, double x1, double y1, double x2, double y2, double& u1, double& v1, double& u2, double& v2);

	// Static members for sorting
	static int ComparePoints(const void* a, const void* b);
	static void SortSectorPoints(TArray<DVector2>& points, DVector2 center);
	static DVector2 StaticBakerSortedCenter;

	// Instance members
	bool baked = false;
	TArray<line_t*> bakedLinedefs;
	TMap<FString, TMap<int, StaticGeometryBuffer>> staticGeometryData;
};

// Global instance
extern StaticGeometryBaker GStaticBaker;