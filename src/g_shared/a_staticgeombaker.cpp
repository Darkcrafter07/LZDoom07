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
// SECTOR VECTOR SHAPE EXTRACTION
//
//==========================================================================

void StaticGeometryBaker::SortVerticesClockwise(TArray<DVector2>& vertices, const DVector2& center)
{
	if (vertices.Size() < 3) return;

	// Calculate angles from center
	TArray<double> angles;
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		angles.Push(atan2(vertices[i].Y - center.Y, vertices[i].X - center.X));
	}

	// Sort vertices based on angles
	for (unsigned i = 0; i < vertices.Size() - 1; i++)
	{
		for (unsigned j = i + 1; j < vertices.Size(); j++)
		{
			if (angles[i] > angles[j])
			{
				// Swap vertices
				DVector2 temp = vertices[i];
				vertices[i] = vertices[j];
				vertices[j] = temp;

				// Swap angles
				double tempAngle = angles[i];
				angles[i] = angles[j];
				angles[j] = tempAngle;
			}
		}
	}
}

void StaticGeometryBaker::ExtractSectorVectorShape(sector_t* sec, TArray<DVector2>& vertices)
{
	if (!sec) return;

	// Step 1: Collect all unique vertices from sector lines
	TArray<DVector2> allVertices;
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		allVertices.Push(DVector2(line->v1->fX(), line->v1->fY()));
		allVertices.Push(DVector2(line->v2->fX(), line->v2->fY()));
	}

	// Step 2: Remove duplicate vertices (with tolerance)
	vertices.Clear();
	for (unsigned i = 0; i < allVertices.Size(); i++)
	{
		bool isDuplicate = false;
		for (unsigned j = 0; j < vertices.Size(); j++)
		{
			if (allVertices[i].ApproximatelyEquals(vertices[j]))
			{
				isDuplicate = true;
				break;
			}
		}
		if (!isDuplicate)
		{
			vertices.Push(allVertices[i]);
		}
	}

	// Step 3: Sort vertices in clockwise order around sector center
	SortVerticesClockwise(vertices, sec->centerspot);
}

//==========================================================================
//
// POLYGON CLEANING AND VALIDATION
//
//==========================================================================

void StaticGeometryBaker::CleanPolygon(TArray<DVector2>& vertices)
{
	if (vertices.Size() < 3) return;

	// Remove consecutive duplicate vertices
	TArray<DVector2> cleaned;
	cleaned.Push(vertices[0]);

	for (unsigned i = 1; i < vertices.Size(); i++)
	{
		if (!vertices[i].ApproximatelyEquals(cleaned.Last()))
		{
			cleaned.Push(vertices[i]);
		}
	}

	// Ensure the polygon is closed (first and last vertices are the same)
	if (!cleaned[0].ApproximatelyEquals(cleaned.Last()))
	{
		cleaned.Push(cleaned[0]);
	}

	vertices = cleaned;
}

bool StaticGeometryBaker::IsPolygonValid(const TArray<DVector2>& vertices)
{
	if (vertices.Size() < 3) return false;

	// Check if polygon is closed
	if (!vertices[0].ApproximatelyEquals(vertices.Last()))
		return false;

	// Check for self-intersections
	for (unsigned i = 0; i < vertices.Size() - 1; i++)
	{
		for (unsigned j = i + 2; j < vertices.Size() - 1; j++)
		{
			if (LinesIntersect(vertices[i], vertices[i + 1],
				vertices[j], vertices[j + 1]))
			{
				return false;
			}
		}
	}

	return true;
}

//==========================================================================
//
// MESH GENERATION FROM POLYGON
//
//==========================================================================

void StaticGeometryBaker::GenerateMeshFromPolygon(TArray<DVector2>& vertices, TArray<unsigned>& indices)
{
	if (vertices.Size() < 3) return;

	// Simple triangulation for convex polygons
	if (IsPolygonConvex(vertices))
	{
		SimpleFanTriangulation(vertices, indices);
	}
	else
	{
		// Use Ear Clipping for complex polygons
		EarClipPolygon(vertices, indices);
	}
}

bool StaticGeometryBaker::IsPolygonConvex(const TArray<DVector2>& vertices)
{
	if (vertices.Size() < 3) return false;

	bool hasPositive = false;
	bool hasNegative = false;

	for (unsigned i = 0; i < vertices.Size() - 1; i++)
	{
		const DVector2& a = vertices[i];
		const DVector2& b = vertices[i + 1];
		const DVector2& c = vertices[(i + 2) % vertices.Size()];

		double cross = (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);

		if (cross > 0) hasPositive = true;
		if (cross < 0) hasNegative = true;

		if (hasPositive && hasNegative) return false;
	}

	return true;
}

//==========================================================================
//
// EAR CLIPPING ALGORITHM - Mesh Generation for flats
//
//==========================================================================

void StaticGeometryBaker::UpdateNeighborsAfterRemoval(TArray<EarClipVertex>& vertices, int prev, int next)
{
	// Update neighbor pointers after vertex removal
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		if (vertices[i].neighbors[0] == prev + 1)
			vertices[i].neighbors[0] = prev;
		if (vertices[i].neighbors[1] == prev + 1)
			vertices[i].neighbors[1] = prev;
		if (vertices[i].neighbors[0] == next + 1)
			vertices[i].neighbors[0] = next;
		if (vertices[i].neighbors[1] == next + 1)
			vertices[i].neighbors[1] = next;
	}
}

void StaticGeometryBaker::SimpleFanTriangulation(TArray<DVector2>& vertices, TArray<unsigned>& indices)
{
	// Fallback for degenerate polygons
	if (vertices.Size() < 3) return;

	unsigned centerIndex = 0;
	for (unsigned i = 1; i < vertices.Size() - 1; i++)
	{
		indices.Push(centerIndex);
		indices.Push(i);
		indices.Push(i + 1);
	}
}

void StaticGeometryBaker::EarClipPolygon(TArray<DVector2>& vertices, TArray<unsigned>& indices)
{
	if (vertices.Size() < 3) return;

	// Handle holes by creating a single polygon with bridges
	TArray<EarClipVertex> clippedVertices;
	TArray<EarClipEdge> edges;

	// Convert vertices to EarClipVertex format
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		clippedVertices.Push(EarClipVertex());
		clippedVertices[i].point = vertices[i];
		clippedVertices[i].index = i;
	}

	// Build connectivity information
	BuildVertexNeighbors(clippedVertices, edges);

	// Calculate angles for each vertex
	for (unsigned i = 0; i < clippedVertices.Size(); i++)
	{
		clippedVertices[i].angle = CalculateAngle(clippedVertices[i], clippedVertices);
	}

	// Main ear clipping loop
	unsigned remaining = clippedVertices.Size();
	while (remaining >= 3)
	{
		// Find the first ear
		int earIndex = -1;
		for (unsigned i = 0; i < clippedVertices.Size(); i++)
		{
			if (!clippedVertices[i].isReflex && IsEar(clippedVertices[i], clippedVertices, edges))
			{
				earIndex = i;
				break;
			}
		}

		if (earIndex == -1)
		{
			// No ear found - fallback to simple triangulation
			SimpleFanTriangulation(vertices, indices);
			return;
		}

		// Add the triangle (v_prev, ear, v_next)
		EarClipVertex& ear = clippedVertices[earIndex];
		int prevIndex = ear.neighbors[0];
		int nextIndex = ear.neighbors[1];

		indices.Push(prevIndex);
		indices.Push(earIndex);
		indices.Push(nextIndex);

		// Remove the ear vertex and update neighbors
		clippedVertices.Delete(earIndex);
		remaining--;

		// Update neighbor relationships
		UpdateNeighborsAfterRemoval(clippedVertices, prevIndex, nextIndex);

		// Recalculate angles for affected vertices
		if (prevIndex < clippedVertices.Size())
		{
			clippedVertices[prevIndex].angle = CalculateAngle(clippedVertices[prevIndex], clippedVertices);
		}
		if (nextIndex < clippedVertices.Size())
		{
			clippedVertices[nextIndex].angle = CalculateAngle(clippedVertices[nextIndex], clippedVertices);
		}
	}
}

void StaticGeometryBaker::BuildVertexNeighbors(TArray<EarClipVertex>& vertices, TArray<EarClipEdge>& edges)
{
	edges.Clear();

	// For a simple polygon (no holes), each vertex connects to its immediate neighbors
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		int prev = (i == 0) ? vertices.Size() - 1 : i - 1;
		int next = (i == vertices.Size() - 1) ? 0 : i + 1;

		vertices[i].neighbors.Push(prev);
		vertices[i].neighbors.Push(next);

		edges.Push(EarClipEdge(i, next));
	}

	// Mark reflex vertices (internal angle > 180 degrees)
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		DVector2& v = vertices[i].point;
		DVector2& prev = vertices[vertices[i].neighbors[0]].point;
		DVector2& next = vertices[vertices[i].neighbors[1]].point;

		// Calculate cross product to determine if reflex
		double cross = (next.X - v.X) * (prev.Y - v.Y) - (next.Y - v.Y) * (prev.X - v.X);
		vertices[i].isReflex = (cross > 0);  // Positive = convex, Negative = reflex
	}
}

bool StaticGeometryBaker::IsEar(const EarClipVertex& v, const TArray<EarClipVertex>& vertices, const TArray<EarClipEdge>& edges)
{
	if (v.isReflex) return false;  // Reflex vertices can't be ears

	// Use const references properly
	const DVector2& a = vertices[v.neighbors[0]].point;
	const DVector2& b = v.point;
	const DVector2& c = vertices[v.neighbors[1]].point;

	// Check if no other vertex is inside triangle abc
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		if (i == v.neighbors[0] || i == v.index || i == v.neighbors[1]) continue;

		if (IsPointInTriangle(vertices[i].point, a, b, c))
		{
			return false;  // Triangle contains another vertex
		}
	}

	return true;
}

bool StaticGeometryBaker::IsPointInTriangle(const DVector2& p, const DVector2& a, const DVector2& b, const DVector2& c)
{
	// Barycentric coordinate method
	double denom = (b.Y - c.Y) * (a.X - c.X) + (c.X - b.X) * (a.Y - c.Y);
	if (fabs(denom) < 1e-6) return false;

	double alpha = ((b.Y - c.Y) * (p.X - c.X) + (c.X - b.X) * (p.Y - c.Y)) / denom;
	double beta = ((c.Y - a.Y) * (p.X - c.X) + (a.X - c.X) * (p.Y - c.Y)) / denom;
	double gamma = 1.0 - alpha - beta;

	return (alpha >= 0 && beta >= 0 && gamma >= 0);
}

double StaticGeometryBaker::CalculateAngle(const EarClipVertex& v, const TArray<EarClipVertex>& vertices)
{
	// Use const references properly
	const DVector2& prev = vertices[v.neighbors[0]].point;
	const DVector2& curr = v.point;
	const DVector2& next = vertices[v.neighbors[1]].point;

	// Calculate angle at vertex
	DVector2 v1 = prev - curr;
	DVector2 v2 = next - curr;

	// Normalize vectors
	double len1 = sqrt(v1.X * v1.X + v1.Y * v1.Y);
	double len2 = sqrt(v2.X * v2.X + v2.Y * v2.Y);

	if (len1 < 1e-6 || len2 < 1e-6) return 0.0;

	v1.X /= len1; v1.Y /= len1;
	v2.X /= len2; v2.Y /= len2;

	// Calculate dot product (cos of angle)
	double dot = v1.X * v2.X + v1.Y * v2.Y;

	// Clamp to valid range
	if (dot > 1.0) dot = 1.0;
	if (dot < -1.0) dot = -1.0;

	return acos(dot);
}

//==========================================================================
//
// SECTOR PROCESSING HELPERS
//
//==========================================================================

void StaticGeometryBaker::GetSectorBoundaryVertices(sector_t* sec, TArray<DVector2>& vertices)
{
	if (!sec) return;

	// Collect all vertices from sector lines
	TArray<DVector2> tempVertices;
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		tempVertices.Push(DVector2(line->v1->fX(), line->v1->fY()));
		tempVertices.Push(DVector2(line->v2->fX(), line->v2->fY()));
	}

	// Remove duplicates
	for (unsigned i = 0; i < tempVertices.Size(); i++)
	{
		for (unsigned j = i + 1; j < tempVertices.Size(); )
		{
			if (tempVertices[i].ApproximatelyEquals(tempVertices[j]))
			{
				tempVertices.Delete(j);
			}
			else
			{
				j++;
			}
		}
	}

	// Sort vertices clockwise
	vertices = tempVertices;
	SortVerticesClockwise(vertices, sec->centerspot);
}

void StaticGeometryBaker::ConnectHoleToOuter(Polygon& outer, const Polygon& hole, int outerIndex, int holeIndex)
{
	// Validate indices
	if (outerIndex < 0 || outerIndex >= outer.vertices.Size()) return;
	if (holeIndex < 0 || holeIndex >= hole.vertices.Size()) return;

	// Get the connection points with bounds checking
	DVector2 outerPoint1 = outer.vertices[outerIndex];
	DVector2 outerPoint2 = outer.vertices[(outerIndex + 1) % outer.vertices.Size()];
	DVector2 holePoint1 = hole.vertices[holeIndex];
	DVector2 holePoint2 = hole.vertices[(holeIndex + 1) % hole.vertices.Size()];

	// Find the best connection points using the centers
	DVector2 outerCenter(0, 0), holeCenter(0, 0);

	for (unsigned i = 0; i < outer.vertices.Size(); i++)
		outerCenter += outer.vertices[i];
	outerCenter /= outer.vertices.Size();

	for (unsigned i = 0; i < hole.vertices.Size(); i++)
		holeCenter += hole.vertices[i];
	holeCenter /= hole.vertices.Size();

	// Find closest points between outer and hole
	DVector2 connectOuter = ClosestPointOnSegment(outerPoint1, outerPoint2, holeCenter);
	DVector2 connectHole = ClosestPointOnSegment(holePoint1, holePoint2, outerCenter);

	// Create bridge vertices
	DVector2 bridgeStart = connectOuter;
	DVector2 bridgeEnd = connectHole;

	// Modify the outer polygon to include the hole
	TArray<DVector2> newVertices;

	// Add vertices up to the connection point
	for (unsigned i = 0; i <= outerIndex; i++)
	{
		newVertices.Push(outer.vertices[i]);
	}

	// Add bridge start point
	newVertices.Push(bridgeStart);

	// Add bridge end point
	newVertices.Push(bridgeEnd);

	// Add hole vertices in reverse order (to maintain winding)
	for (int i = holeIndex; i >= 0; i--)
	{
		newVertices.Push(hole.vertices[i]);
	}
	for (int i = hole.vertices.Size() - 1; i > holeIndex; i--)
	{
		newVertices.Push(hole.vertices[i]);
	}

	// Add the remaining outer vertices
	for (unsigned i = outerIndex + 1; i < outer.vertices.Size(); i++)
	{
		newVertices.Push(outer.vertices[i]);
	}

	// Close the polygon by adding bridge start back to outer
	newVertices.Push(bridgeStart);

	// Update outer polygon
	outer.vertices = newVertices;
	outer.CalculateCenter();
}

DVector2 StaticGeometryBaker::ClosestPointOnSegment(const DVector2& v1, const DVector2& v2, const DVector2& p)
{
	// Vector from v1 to v2
	DVector2 segment = v2 - v1;
	double segmentLengthSquared = segment.X * segment.X + segment.Y * segment.Y;

	if (segmentLengthSquared < 1e-6)
		return v1; // v1 and v2 are the same point

	// Vector from v1 to p
	DVector2 toP = p - v1;

	// Project toP onto segment
	double t = (toP.X * segment.X + toP.Y * segment.Y) / segmentLengthSquared;

	// Clamp t to [0, 1]
	t = clamp(t, 0.0, 1.0);

	// Closest point on segment
	return v1 + segment * t;
}

int StaticGeometryBaker::FindClosestEdge(const Polygon& outer, const Polygon& hole, int& outerIndex, int& holeIndex)
{
	double minDistance = 999999;
	outerIndex = -1;
	holeIndex = -1;

	// Default to reasonable values if no better found
	outerIndex = 0;
	holeIndex = 0;

	// Find the closest edge on outer polygon to the hole center
	for (unsigned i = 0; i < outer.vertices.Size(); i++)
	{
		const DVector2& v1 = outer.vertices[i];
		const DVector2& v2 = outer.vertices[(i + 1) % outer.vertices.Size()];

		// Find closest point on this edge to hole center
		DVector2 closestPoint = ClosestPointOnSegment(v1, v2, hole.center);
		double distance = sqrt((closestPoint.X - hole.center.X) * (closestPoint.X - hole.center.X) +
			(closestPoint.Y - hole.center.Y) * (closestPoint.Y - hole.center.Y));

		if (distance < minDistance)
		{
			minDistance = distance;
			outerIndex = i;
		}
	}

	// Find the closest edge on hole to the outer center
	for (unsigned i = 0; i < hole.vertices.Size(); i++)
	{
		const DVector2& v1 = hole.vertices[i];
		const DVector2& v2 = hole.vertices[(i + 1) % hole.vertices.Size()];

		DVector2 closestPoint = ClosestPointOnSegment(v1, v2, outer.center);
		double distance = sqrt((closestPoint.X - outer.center.X) * (closestPoint.X - outer.center.X) +
			(closestPoint.Y - outer.center.Y) * (closestPoint.Y - outer.center.Y));

		if (distance < minDistance)
		{
			minDistance = distance;
			holeIndex = i;
			// Make sure outerIndex is also valid
			if (outerIndex == -1) outerIndex = 0;
		}
	}

	// Ensure we always have valid indices
	if (outerIndex == -1) outerIndex = 0;
	if (holeIndex == -1) holeIndex = 0;

	return (minDistance < 999999); // Return true if we found something reasonable
}

bool StaticGeometryBaker::IsPointInPolygon(const DVector2& point, const Polygon& poly)
{
	// Ray casting algorithm
	bool inside = false;
	for (unsigned i = 0, j = poly.vertices.Size() - 1; i < poly.vertices.Size(); j = i++)
	{
		const DVector2& v1 = poly.vertices[i];
		const DVector2& v2 = poly.vertices[j];

		if (((v1.Y > point.Y) != (v2.Y > point.Y)) &&
			(point.X < (v2.X - v1.X) * (point.Y - v1.Y) / (v2.Y - v1.Y) + v1.X))
		{
			inside = !inside;
		}
	}
	return inside;
}

void StaticGeometryBaker::CreateSinglePolygonWithHoles(const TArray<Polygon>& polygons, TArray<DVector2>& result)
{
	if (polygons.Size() == 0) return;

	// Start with the outer polygon
	Polygon currentPoly = polygons[0];

	// Connect each hole to the outer polygon
	for (unsigned i = 1; i < polygons.Size(); i++)
	{
		const Polygon& hole = polygons[i];

		// Find the closest edges
		int outerIndex, holeIndex;
		if (FindClosestEdge(currentPoly, hole, outerIndex, holeIndex))
		{
			// Validate indices before connecting
			if (outerIndex >= 0 && outerIndex < currentPoly.vertices.Size() &&
				holeIndex >= 0 && holeIndex < hole.vertices.Size())
			{
				// Connect the hole to the outer polygon
				ConnectHoleToOuter(currentPoly, hole, outerIndex, holeIndex);
			}
		}
	}

	result = currentPoly.vertices;
}

void StaticGeometryBaker::CreateSinglePolygon(TArray<DVector2>& outer, TArray<TArray<DVector2>>& holes, TArray<DVector2>& result)
{
	if (outer.Size() < 3) return;

	// Convert to Polygon structures
	TArray<Polygon> polygons;
	Polygon outerPoly;
	outerPoly.vertices = outer;
	outerPoly.CalculateCenter();
	polygons.Push(outerPoly);

	for (unsigned i = 0; i < holes.Size(); i++)
	{
		Polygon holePoly;
		holePoly.vertices = holes[i];
		holePoly.CalculateCenter();
		polygons.Push(holePoly);
	}

	// Create single polygon with holes
	CreateSinglePolygonWithHoles(polygons, result);
}

void StaticGeometryBaker::GroupNearbyPoints(const TArray<DVector2>& points, TArray<DVector2>& result)
{
	if (points.Size() == 0) return;

	// Simple clustering algorithm
	double clusterDistance = 32.0; // Adjust this value as needed
	TArray<bool> used;
	used.Resize(points.Size());
	for (unsigned i = 0; i < used.Size(); i++)
		used[i] = false;

	for (unsigned i = 0; i < points.Size(); i++)
	{
		if (used[i]) continue;

		// Start a new cluster
		TArray<DVector2> cluster;
		cluster.Push(points[i]);
		used[i] = true;

		// Find all nearby points
		for (unsigned j = 0; j < points.Size(); j++)
		{
			if (used[j]) continue;

			double dist = sqrt((points[i].X - points[j].X) * (points[i].X - points[j].X) +
				(points[i].Y - points[j].Y) * (points[i].Y - points[j].Y));

			if (dist < clusterDistance)
			{
				cluster.Push(points[j]);
				used[j] = true;
			}
		}

		// If cluster has enough points, use its center
		if (cluster.Size() >= 3)
		{
			DVector2 center(0, 0);
			for (unsigned k = 0; k < cluster.Size(); k++)
			{
				center += cluster[k];
			}
			center /= cluster.Size();
			result.Push(center);
		}
	}
}

bool StaticGeometryBaker::IsOnSegment(const DVector2& p1, const DVector2& p2, const DVector2& p3)
{
	// Check if p3 is on the line segment p1-p2
	double d1 = sqrt((p3.X - p1.X) * (p3.X - p1.X) + (p3.Y - p1.Y) * (p3.Y - p1.Y));
	double d2 = sqrt((p3.X - p2.X) * (p3.X - p2.X) + (p3.Y - p2.Y) * (p3.Y - p2.Y));
	double d3 = sqrt((p2.X - p1.X) * (p2.X - p1.X) + (p2.Y - p1.Y) * (p2.Y - p1.Y));

	return (fabs(d1 + d2 - d3) < 1e-6);
}

double StaticGeometryBaker::Direction(const DVector2& p1, const DVector2& p2, const DVector2& p3)
{
	return ((p2.X - p1.X) * (p3.Y - p1.Y) - (p2.Y - p1.Y) * (p3.X - p1.X));
}

bool StaticGeometryBaker::LinesIntersect(const DVector2& p1, const DVector2& p2, const DVector2& p3, const DVector2& p4)
{
	// Check if line segments p1-p2 and p3-p4 intersect
	double d1 = Direction(p3, p4, p1);
	double d2 = Direction(p3, p4, p2);
	double d3 = Direction(p1, p2, p3);
	double d4 = Direction(p1, p2, p4);

	if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
		((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
	{
		return true;
	}

	// Check if endpoints are on the line
	if (fabs(d1) < 1e-6 && IsOnSegment(p3, p4, p1)) return true;
	if (fabs(d2) < 1e-6 && IsOnSegment(p3, p4, p2)) return true;
	if (fabs(d3) < 1e-6 && IsOnSegment(p1, p2, p3)) return true;
	if (fabs(d4) < 1e-6 && IsOnSegment(p1, p2, p4)) return true;

	return false;
}

bool StaticGeometryBaker::IsPointInHole(const DVector2& point, sector_t* sec)
{
	if (!sec) return false;

	// A point is in a hole if it's inside the sector but not visible from the center
	DVector2 center = sec->centerspot;

	// Check line of sight from center to point
	int intersections = 0;
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		DVector2 v1(line->v1->fX(), line->v1->fY());
		DVector2 v2(line->v2->fX(), line->v2->fY());

		if (LinesIntersect(center, point, v1, v2))
		{
			intersections++;
		}
	}

	// If there are intersections, the point might be in a hole
	return (intersections > 0);
}

bool StaticGeometryBaker::IsPointInSector(const DVector2& point, sector_t* sec)
{
	if (!sec) return false;

	// Ray casting algorithm
	bool inside = false;
	for (unsigned i = 0, j = sec->Lines.Size() - 1; i < sec->Lines.Size(); j = i++)
	{
		line_t* line = sec->Lines[i];
		DVector2 v1(line->v1->fX(), line->v1->fY());
		DVector2 v2(line->v2->fX(), line->v2->fY());

		if (((v1.Y > point.Y) != (v2.Y > point.Y)) &&
			(point.X < (v2.X - v1.X) * (point.Y - v1.Y) / (v2.Y - v1.Y) + v1.X))
		{
			inside = !inside;
		}
	}
	return inside;
}

void StaticGeometryBaker::DetectHolesUsingGrid(sector_t* sec, TArray<Polygon>& polygons)
{
	if (!sec) return;

	// Calculate sector bounds
	double minX = 999999, maxX = -999999;
	double minY = 999999, maxY = -999999;

	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		double x1 = line->v1->fX();
		double y1 = line->v1->fY();
		double x2 = line->v2->fX();
		double y2 = line->v2->fY();

		if (x1 < minX) minX = x1;
		if (x1 > maxX) maxX = x1;
		if (y1 < minY) minY = y1;
		if (y1 > maxY) maxY = y1;
		if (x2 < minX) minX = x2;
		if (x2 > maxX) maxX = x2;
		if (y2 < minY) minY = y2;
		if (y2 > maxY) maxY = y2;
	}

	// Add margin
	double margin = 64.0;
	minX -= margin;
	maxX += margin;
	minY -= margin;
	maxY += margin;

	// Create a grid to test points
	const int GRID_SIZE = 32;
	double cellWidth = (maxX - minX) / GRID_SIZE;
	double cellHeight = (maxY - minY) / GRID_SIZE;

	// Test grid points to find holes
	TArray<DVector2> holeVertices;
	for (int i = 1; i < GRID_SIZE; i++)
	{
		for (int j = 1; j < GRID_SIZE; j++)
		{
			double x = minX + i * cellWidth;
			double y = minY + j * cellHeight;

			DVector2 testPoint(x, y);

			// Check if point is inside sector
			if (IsPointInSector(testPoint, sec))
			{
				// Check if it's a hole (surrounded by sector lines)
				bool isHole = IsPointInHole(testPoint, sec);

				if (isHole)
				{
					// This point is in a hole - add to our hole detection
					holeVertices.Push(testPoint);
				}
			}
		}
	}

	// If we found hole vertices, try to form a polygon
	if (holeVertices.Size() >= 3)
	{
		// Group nearby vertices to form hole polygon
		Polygon hole;
		GroupNearbyPoints(holeVertices, hole.vertices);

		if (hole.vertices.Size() >= 3)
		{
			hole.CalculateCenter();
			polygons.Push(hole);
		}
	}
}

void StaticGeometryBaker::FindSectorHoles(sector_t* sec, TArray<Polygon>& polygons)
{
	if (!sec) return;

	// First polygon is always the outer boundary
	Polygon outer;
	GetSectorBoundaryVertices(sec, outer.vertices);
	outer.CalculateCenter();
	polygons.Push(outer);

	// Now find inner polygons (holes)
	// This is the tricky part - we need to analyze the sector structure

	// Method 1: Check for vertices that are not on the boundary
	TArray<DVector2> allVertices;
	for (unsigned i = 0; i < sec->Lines.Size(); i++)
	{
		line_t* line = sec->Lines[i];
		allVertices.Push(DVector2(line->v1->fX(), line->v1->fY()));
		allVertices.Push(DVector2(line->v2->fX(), line->v2->fY()));
	}

	// Remove duplicates
	for (unsigned i = 0; i < allVertices.Size(); i++)
	{
		for (unsigned j = i + 1; j < allVertices.Size(); )
		{
			if (allVertices[i].ApproximatelyEquals(allVertices[j]))
			{
				allVertices.Delete(j);
			}
			else
			{
				j++;
			}
		}
	}

	// Method 2: Find regions inside the sector
	// We'll use a grid-based approach to detect holes
	DetectHolesUsingGrid(sec, polygons);
}

void StaticGeometryBaker::GetSectorHoles(sector_t* sec, TArray<TArray<DVector2>>& holes)
{
	if (!sec) return;

	// Convert sector to polygons array
	TArray<Polygon> polygons;
	FindSectorHoles(sec, polygons);

	// Extract holes (all polygons except the first, which is the outer boundary)
	holes.Clear();
	for (unsigned i = 1; i < polygons.Size(); i++)
	{
		holes.Push(polygons[i].vertices);
	}
}
void StaticGeometryBaker::GenerateFlatGeometry(const FString& texName, int lightLevel, sector_t* sec, int planeType, TArray<DVector2>& vertices, TArray<unsigned>& indices)
{
	StaticGeometryBuffer& buffer = staticGeometryData[texName][lightLevel];

	if (vertices.Size() < 3) return;

	// Calculate UV bounds for world-space mapping
	double minX = vertices[0].X, maxX = vertices[0].X;
	double minY = vertices[0].Y, maxY = vertices[0].Y;
	for (unsigned i = 1; i < vertices.Size(); i++)
	{
		if (vertices[i].X < minX) minX = vertices[i].X;
		if (vertices[i].X > maxX) maxX = vertices[i].X;
		if (vertices[i].Y < minY) minY = vertices[i].Y;
		if (vertices[i].Y > maxY) maxY = vertices[i].Y;
	}

	// Add vertices with Z coordinates
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		double z;
		if (planeType == sector_t::floor)
			z = sec->floorplane.ZatPoint(vertices[i].X, vertices[i].Y);
		else
			z = sec->ceilingplane.ZatPoint(vertices[i].X, vertices[i].Y);

		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\n", vertices[i].X, z, vertices[i].Y);

		// Calculate world-space UVs
		double u = (vertices[i].X - minX) / (maxX - minX);
		double v = (vertices[i].Y - minY) / (maxY - minY);
		buffer.UVs.AppendFormat("vt %.6f %.6f\n", u, v);
	}

	// Add faces
	for (unsigned i = 0; i < indices.Size(); i += 3)
	{
		int v1 = buffer.VertCount + indices[i];
		int v2 = buffer.VertCount + indices[i + 1];
		int v3 = buffer.VertCount + indices[i + 2];
		int u1 = buffer.UVCount + indices[i];
		int u2 = buffer.UVCount + indices[i + 1];
		int u3 = buffer.UVCount + indices[i + 2];

		buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\n", v1, u1, v2, u2, v3, u3);
	}

	buffer.VertCount += vertices.Size();
	buffer.UVCount += vertices.Size();

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
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

void StaticGeometryBaker::GetTerrainVertexHeights(sector_t* sec, int planeType, double& z1, double& z2, double& z3)
{
	if (!sec || sec->Lines.Size() != 3) return;

	// Get the three vertices
	vertex_t* v1 = sec->Lines[0]->v1;
	vertex_t* v2 = sec->Lines[0]->v2;
	vertex_t* v3 = nullptr;

	// Find the third vertex
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

	// Get vertex indices for vertex data lookup
	unsigned v1_idx = 0, v2_idx = 0, v3_idx = 0;
	for (unsigned i = 0; i < level.vertexes.Size(); i++)
	{
		if (&level.vertexes[i] == v1) v1_idx = i;
		if (&level.vertexes[i] == v2) v2_idx = i;
		if (&level.vertexes[i] == v3) v3_idx = i;
	}

	// Get heights based on vertex data or plane
	if (planeType == sector_t::floor)
	{
		z1 = (v1_idx < vertexdatas.Size() && (vertexdatas[v1_idx].flags & VERTEXFLAG_ZFloorEnabled)) ?
			vertexdatas[v1_idx].zFloor : sec->floorplane.ZatPoint(v1);
		z2 = (v2_idx < vertexdatas.Size() && (vertexdatas[v2_idx].flags & VERTEXFLAG_ZFloorEnabled)) ?
			vertexdatas[v2_idx].zFloor : sec->floorplane.ZatPoint(v2);
		z3 = (v3_idx < vertexdatas.Size() && (vertexdatas[v3_idx].flags & VERTEXFLAG_ZFloorEnabled)) ?
			vertexdatas[v3_idx].zFloor : sec->floorplane.ZatPoint(v3);
	}
	else
	{
		z1 = (v1_idx < vertexdatas.Size() && (vertexdatas[v1_idx].flags & VERTEXFLAG_ZCeilingEnabled)) ?
			vertexdatas[v1_idx].zCeiling : sec->ceilingplane.ZatPoint(v1);
		z2 = (v2_idx < vertexdatas.Size() && (vertexdatas[v2_idx].flags & VERTEXFLAG_ZCeilingEnabled)) ?
			vertexdatas[v2_idx].zCeiling : sec->ceilingplane.ZatPoint(v2);
		z3 = (v3_idx < vertexdatas.Size() && (vertexdatas[v3_idx].flags & VERTEXFLAG_ZCeilingEnabled)) ?
			vertexdatas[v3_idx].zCeiling : sec->ceilingplane.ZatPoint(v3);
	}
}

void StaticGeometryBaker::GenerateTerrainGeometry(const FString& texName, int lightLevel, sector_t* sec, int planeType, TArray<DVector2>& vertices, TArray<double>& vertexZ, TArray<unsigned>& indices)
{
	StaticGeometryBuffer& buffer = staticGeometryData[texName][lightLevel];

	if (vertices.Size() != vertexZ.Size() || vertices.Size() < 3) return;

	// Calculate UV bounds for world-space mapping
	double minX = vertices[0].X, maxX = vertices[0].X;
	double minY = vertices[0].Y, maxY = vertices[0].Y;
	for (unsigned i = 1; i < vertices.Size(); i++)
	{
		if (vertices[i].X < minX) minX = vertices[i].X;
		if (vertices[i].X > maxX) maxX = vertices[i].X;
		if (vertices[i].Y < minY) minY = vertices[i].Y;
		if (vertices[i].Y > maxY) maxY = vertices[i].Y;
	}

	// Add vertices with terrain Z coordinates
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\n", vertices[i].X, vertexZ[i], vertices[i].Y);

		// Calculate world-space UVs
		double u = (vertices[i].X - minX) / (maxX - minX);
		double v = (vertices[i].Y - minY) / (maxY - minY);
		buffer.UVs.AppendFormat("vt %.6f %.6f\n", u, v);
	}

	// Add faces
	for (unsigned i = 0; i < indices.Size(); i += 3)
	{
		int v1 = buffer.VertCount + indices[i];
		int v2 = buffer.VertCount + indices[i + 1];
		int v3 = buffer.VertCount + indices[i + 2];
		int u1 = buffer.UVCount + indices[i];
		int u2 = buffer.UVCount + indices[i + 1];
		int u3 = buffer.UVCount + indices[i + 2];

		buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\n", v1, u1, v2, u2, v3, u3);
	}

	buffer.VertCount += vertices.Size();
	buffer.UVCount += vertices.Size();

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

void StaticGeometryBaker::GenerateSlopedGeometry(const FString& texName, int lightLevel, sector_t* sec, int planeType, const secplane_t* slopePlane, TArray<DVector2>& vertices, TArray<unsigned>& indices)
{
	StaticGeometryBuffer& buffer = staticGeometryData[texName][lightLevel];

	if (vertices.Size() < 3) return;

	// Calculate UV bounds for world-space mapping
	double minX = vertices[0].X, maxX = vertices[0].X;
	double minY = vertices[0].Y, maxY = vertices[0].Y;
	for (unsigned i = 1; i < vertices.Size(); i++)
	{
		if (vertices[i].X < minX) minX = vertices[i].X;
		if (vertices[i].X > maxY) maxX = vertices[i].X;
		if (vertices[i].Y < minY) minY = vertices[i].Y;
		if (vertices[i].Y > maxY) maxY = vertices[i].Y;
	}

	// Add vertices with Z coordinates from slope plane
	for (unsigned i = 0; i < vertices.Size(); i++)
	{
		double z = CalculateZAtPoint(slopePlane, vertices[i].X, vertices[i].Y);
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\n", vertices[i].X, z, vertices[i].Y);

		// Calculate world-space UVs
		double u = (vertices[i].X - minX) / (maxX - minX);
		double v = (vertices[i].Y - minY) / (maxY - minY);
		buffer.UVs.AppendFormat("vt %.6f %.6f\n", u, v);
	}

	// Add faces
	for (unsigned i = 0; i < indices.Size(); i += 3)
	{
		int v1 = buffer.VertCount + indices[i];
		int v2 = buffer.VertCount + indices[i + 1];
		int v3 = buffer.VertCount + indices[i + 2];
		int u1 = buffer.UVCount + indices[i];
		int u2 = buffer.UVCount + indices[i + 1];
		int u3 = buffer.UVCount + indices[i + 2];

		buffer.Faces.AppendFormat("f %d/%d %d/%d %d/%d\n", v1, u1, v2, u2, v3, u3);
	}

	buffer.VertCount += vertices.Size();
	buffer.UVCount += vertices.Size();

	if (buffer.Materials.IsEmpty())
	{
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
	}
}

void StaticGeometryBaker::BakeTerrainFlatFallback(sector_t* sec, int planeType, const FString& texName, int steppedLight)
{
	// Fallback for invalid terrain sectors
	double centerX = sec->centerspot.X;
	double centerY = sec->centerspot.Y;

	double minX = centerX - 64.0;
	double maxX = centerX + 64.0;
	double minY = centerY - 64.0;
	double maxY = centerY + 64.0;

	double z;
	if (planeType == sector_t::floor)
		z = sec->floorplane.ZatPoint(sec->centerspot);
	else
		z = sec->ceilingplane.ZatPoint(sec->centerspot);

	// Calculate UVs
	double u1, v1, u2, v2;
	CalculateFlatUVs(sec, planeType, minX, minY, maxX, maxY, u1, v1, u2, v2);

	// Generate geometry
	StaticGeometryBuffer& buffer = staticGeometryData[texName][steppedLight];

	// Vertices
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		minX, z, minY, minX, z, maxY, maxX, z, maxY, maxX, z, minY);

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

void StaticGeometryBaker::BakeTerrainFlat(sector_t* sec, int planeType)
{
	if (!sec || !sec->e) return;
	if (sec->Lines.Size() != 3) return; // Only for triangular sectors

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
	TArray<DVector2> vertices;
	ExtractSectorVectorShape(sec, vertices);

	if (vertices.Size() != 3)
	{
		// Fallback if not exactly 3 vertices
		BakeTerrainFlatFallback(sec, planeType, texName, steppedLight);
		return;
	}

	// Get vertex heights
	double z1, z2, z3;
	GetTerrainVertexHeights(sec, planeType, z1, z2, z3);

	// Create vertex Z array for the terrain
	TArray<double> vertexZ;
	vertexZ.Push(z1);
	vertexZ.Push(z2);
	vertexZ.Push(z3);

	// Generate mesh (simple triangle)
	TArray<unsigned> indices;
	indices.Push(0);
	indices.Push(1);
	indices.Push(2);

	// Generate geometry with terrain heights
	GenerateTerrainGeometry(texName, steppedLight, sec, planeType, vertices, vertexZ, indices);
}

void StaticGeometryBaker::BakeSlopedFlatFallback(sector_t* sec, int planeType, const secplane_t* slopePlane, bool is3DFloor, const FString& texName, int steppedLight)
{
	// Original bounding box approach as fallback
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

	// Calculate Z values using slope plane
	double z11 = CalculateZAtPoint(slopePlane, x1, y1);
	double z12 = CalculateZAtPoint(slopePlane, x1, y2);
	double z21 = CalculateZAtPoint(slopePlane, x2, y1);
	double z22 = CalculateZAtPoint(slopePlane, x2, y2);

	// Calculate UVs
	double u1, v1, u2, v2;
	CalculateFlatUVs(sec, planeType, minX, minY, maxX, maxY, u1, v1, u2, v2);

	// Generate geometry
	FString bufferKey = is3DFloor ? "3DFLOOR_" + texName : texName;
	StaticGeometryBuffer& buffer = staticGeometryData[bufferKey][steppedLight];

	// Vertices
	if (planeType == sector_t::floor)
	{
		buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
			x1, z11, y1, x1, z12, y2, x2, z22, y2, x2, z21, y1);
	}
	else
	{
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

	// Step 1: Extract sector vector shape
	TArray<DVector2> vertices;
	ExtractSectorVectorShape(sec, vertices);

	if (vertices.Size() < 3)
	{
		// Fallback to bounding box for invalid sectors
		BakeSlopedFlatFallback(sec, planeType, slopePlane, is3DFloor, texName, steppedLight);
		return;
	}

	// Step 2: Clean and validate the polygon
	CleanPolygon(vertices);

	if (!IsPolygonValid(vertices))
	{
		// Fallback for invalid polygons
		BakeSlopedFlatFallback(sec, planeType, slopePlane, is3DFloor, texName, steppedLight);
		return;
	}

	// Step 3: Generate mesh from polygon
	TArray<unsigned> indices;
	GenerateMeshFromPolygon(vertices, indices);

	if (indices.Size() < 3)
	{
		// Fallback if triangulation fails
		BakeSlopedFlatFallback(sec, planeType, slopePlane, is3DFloor, texName, steppedLight);
		return;
	}

	// Step 4: Generate final geometry with slope
	FString bufferKey = is3DFloor ? "3DFLOOR_" + texName : texName;
	GenerateSlopedGeometry(bufferKey, steppedLight, sec, planeType, slopePlane, vertices, indices);
}

void StaticGeometryBaker::BakeFlatGeometryFallback(sector_t* sec, int planeType, const FString& texName, int steppedLight)
{
	// Original bounding box approach as fallback
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
	CalculateFlatUVs(sec, planeType, minX, minY, maxX, maxY, u1, v1, u2, v2);

	// Generate geometry
	StaticGeometryBuffer& buffer = staticGeometryData[texName][steppedLight];

	// Vertices
	buffer.Vertices.AppendFormat("v %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\nv %.6f %.6f %.6f\n",
		x1, z, y1, x1, z, y2, x2, z, y2, x2, z, y1);

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
	if (texID.isNull() || IsSkyFlat(texID)) return;

	FTexture* tex = TexMan[texID];
	if (!tex) return;

	FString texName = tex->Name;
	int rawLight = sec->lightlevel;
	if (rawLight > 255) rawLight = 255;
	int steppedLight = (rawLight / 16) * 16;
	if (steppedLight < 0) steppedLight = 0;
	if (steppedLight > 240) steppedLight = 240;

	// Step 1: Extract sector vector shape
	TArray<DVector2> vertices;
	ExtractSectorVectorShape(sec, vertices);

	if (vertices.Size() < 3)
	{
		// Fallback to bounding box for invalid sectors
		BakeFlatGeometryFallback(sec, planeType, texName, steppedLight);
		return;
	}

	// Step 2: Clean and validate the polygon
	CleanPolygon(vertices);

	if (!IsPolygonValid(vertices))
	{
		// Fallback for invalid polygons
		BakeFlatGeometryFallback(sec, planeType, texName, steppedLight);
		return;
	}

	// Step 3: Generate mesh from polygon
	TArray<unsigned> indices;
	GenerateMeshFromPolygon(vertices, indices);

	if (indices.Size() < 3)
	{
		// Fallback if triangulation fails
		BakeFlatGeometryFallback(sec, planeType, texName, steppedLight);
		return;
	}

	// Step 4: Generate final geometry
	GenerateFlatGeometry(texName, steppedLight, sec, planeType, vertices, indices);
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

void StaticGeometryBaker::Bake3DFloorFlatFallback(sector_t* dummySec, sector_t* targetSec, int planeType, F3DFloor* ffloor, const FString& texName, int steppedLight)
{
	if (!dummySec || !targetSec) return;

	double minX = 999999, maxX = -999999, minY = 999999, maxY = -999999;
	for (unsigned i = 0; i < targetSec->Lines.Size(); i++)
	{
		line_t* line = targetSec->Lines[i];
		double x1 = line->v1->fX();
		double y1 = line->v1->fY();
		double x2 = line->v2->fX();
		double y2 = line->v2->fY();
		minX = MIN(minX, x1); maxX = MAX(maxX, x1);
		minY = MIN(minY, y1); maxY = MAX(maxY, y1);
		minX = MIN(minX, x2); maxX = MAX(maxX, x2);
		minY = MIN(minY, y2); maxY = MAX(maxY, y2);
	}

	if (minX == 999999) return;

	// Add margin
	double margin = 2.0;
	double x1 = minX - margin;
	double x2 = maxX + margin;
	double y1 = minY - margin;
	double y2 = maxY + margin;

	// Get slope plane from dummy sector
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
	CalculateFlatUVs(dummySec, planeType, minX, minY, maxX, maxY, u1, v1, u2, v2);

	// Generate geometry
	FString layerKey = "3DFLOOR_" + texName;
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
		buffer.Materials.Format("usemtl %s\n", texName.GetChars());
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

	// Step 1: Extract target sector vector shape
	TArray<DVector2> vertices;
	ExtractSectorVectorShape(targetSec, vertices);

	if (vertices.Size() < 3)
	{
		// Fallback to bounding box
		Bake3DFloorFlatFallback(dummySec, targetSec, planeType, ffloor, surfaceTexName, steppedLight);
		return;
	}

	// Step 2: Clean and validate the polygon
	CleanPolygon(vertices);

	if (!IsPolygonValid(vertices))
	{
		// Fallback for invalid polygons
		Bake3DFloorFlatFallback(dummySec, targetSec, planeType, ffloor, surfaceTexName, steppedLight);
		return;
	}

	// Step 3: Generate mesh from polygon
	TArray<unsigned> indices;
	GenerateMeshFromPolygon(vertices, indices);

	if (indices.Size() < 3)
	{
		// Fallback if triangulation fails
		Bake3DFloorFlatFallback(dummySec, targetSec, planeType, ffloor, surfaceTexName, steppedLight);
		return;
	}

	// Step 4: Generate final geometry
	FString layerKey = "3DFLOOR_" + surfaceTexName;
	GenerateFlatGeometry(layerKey, steppedLight, dummySec, planeType, vertices, indices);
}

void StaticGeometryBaker::BakeSectorGeometry(sector_t* sec)
{
	if (!sec->e) return;

	// Process 3D floors FIRST
	for (unsigned j = 0; j < sec->e->XFloor.ffloors.Size(); j++)
	{
		F3DFloor* ffloor = sec->e->XFloor.ffloors[j];
		if (!(ffloor->flags & FF_EXISTS) || !(ffloor->flags & FF_SOLID)) continue;

		Bake3DFloorFlat(ffloor->model, sec, sector_t::floor, ffloor);
		Bake3DFloorFlat(ffloor->model, sec, sector_t::ceiling, ffloor);
	}

	// Check for terrain (triangular sectors with vertex heights)
	bool isTerrain = (sec->Lines.Size() == 3);

	// Process regular floor
	FTextureID floorTexID = sec->planes[sector_t::floor].Texture;
	if (!floorTexID.isNull() && !IsSkyFlat(floorTexID))
	{
		if (isTerrain)
		{
			BakeTerrainFlat(sec, sector_t::floor);
		}
		else if (IsPlaneSloped(&sec->floorplane))
		{
			BakeSlopedFlat(sec, sector_t::floor, &sec->floorplane, false);
		}
		else
		{
			BakeFlatGeometry(sec, sector_t::floor);
		}
	}

	// Process regular ceiling
	FTextureID ceilTexID = sec->planes[sector_t::ceiling].Texture;
	if (!ceilTexID.isNull() && !IsSkyFlat(ceilTexID))
	{
		if (isTerrain)
		{
			BakeTerrainFlat(sec, sector_t::ceiling);
		}
		else if (IsPlaneSloped(&sec->ceilingplane))
		{
			BakeSlopedFlat(sec, sector_t::ceiling, &sec->ceilingplane, false);
		}
		else
		{
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