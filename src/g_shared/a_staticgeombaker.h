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


	// Ear clipping structures and helpers
	struct EarClipVertex
	{
		DVector2 point;
		int index;
		bool isReflex;
		TArray<int> neighbors;  // Connected vertices (clockwise)
		double angle;  // Internal angle

		EarClipVertex() : index(-1), isReflex(false), angle(0.0) {}
	};
	struct EarClipEdge
	{
		int start;
		int end;
		bool isBridge;  // Is this a bridge edge for holes?

		EarClipEdge(int s, int e, bool bridge = false) : start(s), end(e), isBridge(bridge) {}
	};

	void SortVerticesClockwise(TArray<DVector2>& vertices, const DVector2& center);
	void ExtractSectorVectorShape(sector_t* sec, TArray<DVector2>& vertices);
	void CleanPolygon(TArray<DVector2>& vertices);
	bool IsPolygonValid(const TArray<DVector2>& vertices);
	void GenerateMeshFromPolygon(TArray<DVector2>& vertices, TArray<unsigned>& indices);
	bool IsPolygonConvex(const TArray<DVector2>& vertices);

	void UpdateNeighborsAfterRemoval(TArray<EarClipVertex>& vertices, int prev, int next);
	void SimpleFanTriangulation(TArray<DVector2>& vertices, TArray<unsigned>& indices);
	void EarClipPolygon(TArray<DVector2>& vertices, TArray<unsigned>& indices);
	bool IsEar(const EarClipVertex& v, const TArray<EarClipVertex>& vertices, const TArray<EarClipEdge>& edges);
	void BuildVertexNeighbors(TArray<EarClipVertex>& vertices, TArray<EarClipEdge>& edges);
	bool IsPointInTriangle(const DVector2& p, const DVector2& a, const DVector2& b, const DVector2& c);
	double CalculateAngle(const EarClipVertex& v, const TArray<EarClipVertex>& vertices);

	// Sector boundary, hole detection and polygon processing
	struct Polygon
	{
		TArray<DVector2> vertices;
		TArray<int> holes;  // Indices of holes inside this polygon
		DVector2 center;

		Polygon() {}

		void CalculateCenter()
		{
			center = DVector2(0, 0);
			for (unsigned i = 0; i < vertices.Size(); i++)
			{
				center += vertices[i];
			}
			if (vertices.Size() > 0)
				center /= vertices.Size();
		}
	};
	void GetSectorBoundaryVertices(sector_t* sec, TArray<DVector2>& vertices);
	void ConnectHoleToOuter(Polygon& outer, const Polygon& hole, int outerIndex, int holeIndex);
	DVector2 ClosestPointOnSegment(const DVector2& v1, const DVector2& v2, const DVector2& p);
	int FindClosestEdge(const Polygon& outer, const Polygon& hole, int& outerIndex, int& holeIndex);
	bool IsPointInPolygon(const DVector2& point, const Polygon& poly);
	void CreateSinglePolygonWithHoles(const TArray<Polygon>& polygons, TArray<DVector2>& result);
	void CreateSinglePolygon(TArray<DVector2>& outer, TArray<TArray<DVector2>>& holes, TArray<DVector2>& result);
	void GroupNearbyPoints(const TArray<DVector2>& points, TArray<DVector2>& result);
	bool IsOnSegment(const DVector2& p1, const DVector2& p2, const DVector2& p3);
	double Direction(const DVector2& p1, const DVector2& p2, const DVector2& p3);
	bool LinesIntersect(const DVector2& p1, const DVector2& p2, const DVector2& p3, const DVector2& p4);
	bool IsPointInHole(const DVector2& point, sector_t* sec);
	bool IsPointInSector(const DVector2& point, sector_t* sec);
	void DetectHolesUsingGrid(sector_t* sec, TArray<Polygon>& polygons);
	void FindSectorHoles(sector_t* sec, TArray<Polygon>& polygons);
	void GetSectorHoles(sector_t* sec, TArray<TArray<DVector2>>& holes);


	void GenerateFlatGeometry(const FString& texName, int lightLevel, sector_t* sec, int planeType, TArray<DVector2>& vertices, TArray<unsigned>& indices);

	// Geometry baking helpers
	double CalculateZAtPoint(const secplane_t* plane, double x, double y);
	bool IsPlaneSloped(const secplane_t* plane);
	void GetTerrainVertexHeights(sector_t* sec, int planeType, double& z1, double& z2, double& z3);
	void GenerateTerrainGeometry(const FString& texName, int lightLevel, sector_t* sec, int planeType, TArray<DVector2>& vertices, TArray<double>& vertexZ, TArray<unsigned>& indices);
	void GenerateSlopedGeometry(const FString& texName, int lightLevel, sector_t* sec, int planeType, const secplane_t* slopePlane, TArray<DVector2>& vertices, TArray<unsigned>& indices);
	void BakeTerrainFlatFallback(sector_t* sec, int planeType, const FString& texName, int steppedLight);
	void BakeTerrainFlat(sector_t* sec, int planeType);
	void BakeSlopedFlatFallback(sector_t* sec, int planeType, const secplane_t* slopePlane, bool is3DFloor, const FString& texName, int steppedLight);
	void BakeSlopedFlat(sector_t* sec, int planeType, const secplane_t* slopePlane, bool is3DFloor);
	void BakeSectorGeometry(sector_t* sec);

	void BakeFlatGeometryFallback(sector_t* sec, int planeType, const FString& texName, int steppedLight);
	void BakeFlatGeometry(sector_t* sec, int planeType);
	void BakeWallGeometry(line_t* line, side_t* side);
	bool IsSkyFlat(FTextureID texID);

	// 3DFloor geometry baking helpers
	void Bake3DFloorWall(seg_t* seg, F3DFloor* ffloor);
	void Process3DFloorWalls();
	void Bake3DFloorFlatFallback(sector_t* dummySec, sector_t* targetSec, int planeType, F3DFloor* ffloor, const FString& texName, int steppedLight);
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