#pragma once

#include "tarray.h"
#include "r_defs.h"
#include "textures.h"

// Structure to hold geometry data for OBJ generation
struct StaticGeometryBuffer
{
	FString UVs;
	FString Faces;
	FString Vertices;
	FString Materials;

	int VertCount = 1;
	int UVCount = 1;
};

class StaticGeometryBaker
{
public:
	bool IsStatic(const seg_t* seg);
	int CalculateFakeContrast(line_t* line, side_t* side);
	void ApplyFakeContrast(int& lightLevel, line_t* line, side_t* side);
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
	double CalculateZAtPoint(const secplane_t* plane, double x, double y);
	bool IsPlaneSloped(const secplane_t* plane);
	void BakeTerrainFlat(sector_t* sec, int planeType);
	void BakeSlopedFlat(sector_t* sec, int planeType, const secplane_t* slopePlane, bool is3DFloor);
	void BakeSectorGeometry(sector_t* sec);

	void BakeFlatGeometry(sector_t* sec, int planeType);
	void BakeWallGeometry(line_t* line, side_t* side);
	bool IsSkyFlat(FTextureID texID);

	// 3DFloor geometry baking helpers
	void Bake3DFloorWall(seg_t* seg, F3DFloor* ffloor);
	void Process3DFloorWalls();
	void Bake3DFloorFlat(sector_t* dummySec, sector_t* targetSec, int planeType, F3DFloor* ffloor);

	// UV calculation helpers
	void CalculateWallUVs(const seg_t* seg, double& u1, double& u2, double& v1, double& v2);
	void CalculateFlatUVs(sector_t* sec, int planeType, double x1, double y1, double x2, double y2, double& u1, double& v1, double& u2, double& v2);

	// Instance members
	bool baked = false;
	TArray<line_t*> bakedLinedefs;
	TMap<FString, TMap<int, StaticGeometryBuffer>> staticGeometryData;
};

// Global instance
extern StaticGeometryBaker GStaticBaker;