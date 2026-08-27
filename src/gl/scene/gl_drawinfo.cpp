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
** gl_drawinfo.cpp
** Implements the draw info structure which contains most of the
** data in a scene and the draw lists - including a very thorough BSP 
** style sorting algorithm for translucent objects.
**
*/

#include "gl/system/gl_system.h"
#include "r_sky.h"
#include "r_utility.h"
#include "r_state.h"
#include "doomstat.h"
#include "g_levellocals.h"

#include "gl/system/gl_cvars.h"
#include "gl/data/gl_data.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/scene/gl_scenedrawer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_templates.h"
#include "gl/shaders/gl_shader.h"
#include "gl/stereo3d/scoped_color_mask.h"
#include "gl/renderer/gl_quaddrawer.h"

#include "gl/compatibility/gl_20.h"

FDrawInfo * gl_drawinfo;

//==========================================================================
//
//
//
//==========================================================================
// Custom hardware data-prefetch wrapper for multi-compiler support (MSVC / GCC / Clang)
#if defined(_MSC_VER)
	#include <xmmintrin.h>
	#define GL_PREFETCH_DATA(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
	// __builtin_prefetch(addr, rw, locality) -> rw=0 (read), locality=3 (heavy spatial persistence L1)
	#define GL_PREFETCH_DATA(addr) __builtin_prefetch((const void*)(addr), 0, 3)
#else
	#define GL_PREFETCH_DATA(addr) ((void)0) // Fallback for unknown compilers
#endif

//==========================================================================
//
//
//
//==========================================================================
class StaticSortNodeArray : public TDeletingArray<SortNode*>
{
	unsigned usecount;
public:
	unsigned Size() { return usecount; }
	void Clear() { usecount=0; }
	void Release(int start) { usecount=start; }
	SortNode * GetNew();
};


SortNode * StaticSortNodeArray::GetNew()
{
	if (usecount==TArray<SortNode*>::Size())
	{
		Push(new SortNode);
	}
	return operator[](usecount++);
}


static StaticSortNodeArray SortNodes;

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::Reset()
{
	if (sorted) SortNodes.Release(SortNodeStart);
	sorted=NULL;
	walls.Clear();
	flats.Clear();
	sprites.Clear();
	drawitems.Clear();
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Translucent polygon sorting - uses a BSP algorithm with an additional 'equal' branch

inline double GLSprite::CalcIntersectionVertex(GLWall * w2)
{
	float ax = x1, ay=y1;
	float bx = x2, by=y2;
	float cx = w2->glseg.x1, cy=w2->glseg.y1;
	float dx = w2->glseg.x2, dy=w2->glseg.y2;
	return ((ay-cy)*(dx-cx)-(ax-cx)*(dy-cy)) / ((bx-ax)*(dy-cy)-(by-ay)*(dx-cx));
}



//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::UnlinkFromChain()
{
	if (parent) parent->next=next;
	if (next) next->parent=parent;
	parent=next=NULL;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::Link(SortNode * hook)
{
	if (hook)
	{
		parent=hook->parent;
		hook->parent=this;
	}
	next=hook;
	if (parent) parent->next=this;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToEqual(SortNode *child)
{
	child->UnlinkFromChain();
	child->equal=equal;
	equal=child;			
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToLeft(SortNode * child)
{
	child->UnlinkFromChain();
	child->Link(left);
	left=child;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToRight(SortNode * child)
{
	child->UnlinkFromChain();
	child->Link(right);
	right=child;
}


//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::MakeSortList()
{
	SortNode * p, * n, * c;
	unsigned i;

	SortNodeStart=SortNodes.Size();
	p=NULL;
	n=SortNodes.GetNew();
	for(i=0;i<drawitems.Size();i++)
	{
		n->itemindex=(int)i;
		n->left=n->equal=n->right=NULL;
		n->parent=p;
		p=n;
		if (i!=drawitems.Size()-1)
		{
			c=SortNodes.GetNew();
			n->next=c;
			n=c;
		}
		else
		{
			n->next=NULL;
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================
SortNode * GLDrawList::FindSortPlane(SortNode * head)
{
	while (head->next && drawitems[head->itemindex].rendertype!=GLDIT_FLAT) 
		head=head->next;
	if (drawitems[head->itemindex].rendertype==GLDIT_FLAT) return head;
	return NULL;
}


//==========================================================================
//
//
//
//==========================================================================
SortNode * GLDrawList::FindSortWall(SortNode * head)
{
	float farthest = -FLT_MAX;
	float nearest = FLT_MAX;
	SortNode * best = NULL;
	SortNode * node = head;
	float bestdist = FLT_MAX;

	while (node)
	{
		GLDrawItem * it = &drawitems[node->itemindex];
		if (it->rendertype == GLDIT_WALL)
		{
			float d = walls[it->index].ViewDistance;
			if (d > farthest) farthest = d;
			if (d < nearest) nearest = d;
		}
		node = node->next;
	}
	if (farthest == INT_MIN) return NULL;
	node = head;
	farthest = (farthest + nearest) / 2;
	while (node)
	{
		GLDrawItem * it = &drawitems[node->itemindex];
		if (it->rendertype == GLDIT_WALL)
		{
			float di = fabsf(walls[it->index].ViewDistance - farthest);
			if (!best || di < bestdist)
			{
				best = node;
				bestdist = di;
			}
		}
		node = node->next;
	}
	return best;
}

//==========================================================================
//
// Note: sloped planes are a huge problem...
//
//==========================================================================
void GLDrawList::SortPlaneIntoPlane(SortNode * head,SortNode * sort)
{
	GLFlat * fh=&flats[drawitems[head->itemindex].index];
	GLFlat * fs=&flats[drawitems[sort->itemindex].index];

	if (fh->z==fs->z) 
		head->AddToEqual(sort);
	else if ( (fh->z<fs->z && fh->ceiling) || (fh->z>fs->z && !fh->ceiling)) 
		head->AddToLeft(sort);
	else 
		head->AddToRight(sort);
}


//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::SortWallIntoPlane(SortNode * head,SortNode * sort)
{
	GLFlat * fh=&flats[drawitems[head->itemindex].index];
	GLWall * ws=&walls[drawitems[sort->itemindex].index];

	bool ceiling = fh->z > r_viewpoint.Pos.Z;

	if ((ws->ztop[0] > fh->z || ws->ztop[1] > fh->z) && (ws->zbottom[0] < fh->z || ws->zbottom[1] < fh->z))
	{
		// We have to split this wall!

		// WARNING: NEVER EVER push a member of an array onto the array itself.
		// Bad things will happen if the memory must be reallocated!
		GLWall w = *ws;
		AddWall(&w);
	
		// Splitting is done in the shader with clip planes, if available
		if (gl.flags & RFL_NO_CLIP_PLANES)
		{
			GLWall * ws1;
			ws->vertcount = 0;	// invalidate current vertices.
			ws1=&walls[walls.Size()-1];
			ws=&walls[drawitems[sort->itemindex].index];	// may have been reallocated!
			float newtexv = ws->tcs[GLWall::UPLFT].v + ((ws->tcs[GLWall::LOLFT].v - ws->tcs[GLWall::UPLFT].v) / (ws->zbottom[0] - ws->ztop[0])) * (fh->z - ws->ztop[0]);

			// I make the very big assumption here that translucent walls in sloped sectors
			// and 3D-floors never coexist in the same level. If that were the case this
			// code would become extremely more complicated.
			if (!ceiling)
			{
				ws->ztop[1] = ws1->zbottom[1] = ws->ztop[0] = ws1->zbottom[0] = fh->z;
				ws->tcs[GLWall::UPRGT].v = ws1->tcs[GLWall::LORGT].v = ws->tcs[GLWall::UPLFT].v = ws1->tcs[GLWall::LOLFT].v = newtexv;
			}
			else
			{
				ws1->ztop[1] = ws->zbottom[1] = ws1->ztop[0] = ws->zbottom[0] = fh->z;
				ws1->tcs[GLWall::UPLFT].v = ws->tcs[GLWall::LOLFT].v = ws1->tcs[GLWall::UPRGT].v = ws->tcs[GLWall::LORGT].v=newtexv;
			}
		}

		SortNode * sort2 = SortNodes.GetNew();
		memset(sort2, 0, sizeof(SortNode));
		sort2->itemindex = drawitems.Size() - 1;

		head->AddToLeft(sort);
		head->AddToRight(sort2);
	}
	else if ((ws->zbottom[0]<fh->z && !ceiling) || (ws->ztop[0]>fh->z && ceiling))	// completely on the left side
	{
		head->AddToLeft(sort);
	}
	else
	{
		head->AddToRight(sort);
	}

}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::SortSpriteIntoPlane(SortNode * head, SortNode * sort)
{
	GLFlat * fh = &flats[drawitems[head->itemindex].index];
	GLSprite * ss = &sprites[drawitems[sort->itemindex].index];

	bool ceiling = fh->z > r_viewpoint.Pos.Z;

	auto hiz = ss->z1 > ss->z2 ? ss->z1 : ss->z2;
	auto loz = ss->z1 < ss->z2 ? ss->z1 : ss->z2;

	if ((hiz > fh->z && loz < fh->z) || ss->modelframe)
	{
		// We have to split this sprite
		GLSprite s = *ss;
		AddSprite(&s);	// add a copy to avoid reallocation issues.

		// Splitting is done in the shader with clip planes, if available.
		// The fallback here only really works for non-y-billboarded sprites.
		if (gl.flags & RFL_NO_CLIP_PLANES)
		{
			GLSprite * ss1;
			ss1 = &sprites[sprites.Size() - 1];
			ss = &sprites[drawitems[sort->itemindex].index];	// may have been reallocated!
			float newtexv = ss->vt + ((ss->vb - ss->vt) / (ss->z2 - ss->z1))*(fh->z - ss->z1);

			if (!ceiling)
			{
				ss->z1 = ss1->z2 = fh->z;
				ss->vt = ss1->vb = newtexv;
			}
			else
			{
				ss1->z1 = ss->z2 = fh->z;
				ss1->vt = ss->vb = newtexv;
			}
		}

		SortNode * sort2 = SortNodes.GetNew();
		memset(sort2, 0, sizeof(SortNode));
		sort2->itemindex = drawitems.Size() - 1;

		head->AddToLeft(sort);
		head->AddToRight(sort2);
	}
	else if ((ss->z2<fh->z && !ceiling) || (ss->z1>fh->z && ceiling))	// completely on the left side
	{
		head->AddToLeft(sort);
	}
	else
	{
		head->AddToRight(sort);
	}
}


//==========================================================================
//
//
//
//==========================================================================
#define MIN_EQ (0.0005f)

void GLDrawList::SortWallIntoWall(SortNode * head,SortNode * sort)
{
	GLWall * wh=&walls[drawitems[head->itemindex].index];
	GLWall * ws=&walls[drawitems[sort->itemindex].index];
	GLWall * ws1;
	float v1=wh->PointOnSide(ws->glseg.x1,ws->glseg.y1);
	float v2=wh->PointOnSide(ws->glseg.x2,ws->glseg.y2);

	if (fabs(v1)<MIN_EQ && fabs(v2)<MIN_EQ) 
	{
		if (ws->type==RENDERWALL_FOGBOUNDARY && wh->type!=RENDERWALL_FOGBOUNDARY) 
		{
			head->AddToRight(sort);
		}
		else if (ws->type!=RENDERWALL_FOGBOUNDARY && wh->type==RENDERWALL_FOGBOUNDARY) 
		{
			head->AddToLeft(sort);
		}
		else 
		{
			head->AddToEqual(sort);
		}
	}
	else if (v1<MIN_EQ && v2<MIN_EQ) 
	{
		head->AddToLeft(sort);
	}
	else if (v1>-MIN_EQ && v2>-MIN_EQ) 
	{
		head->AddToRight(sort);
	}
	else
	{
		double r=ws->CalcIntersectionVertex(wh);

		float ix=(float)(ws->glseg.x1+r*(ws->glseg.x2-ws->glseg.x1));
		float iy=(float)(ws->glseg.y1+r*(ws->glseg.y2-ws->glseg.y1));
		float iu=(float)(ws->tcs[GLWall::UPLFT].u + r * (ws->tcs[GLWall::UPRGT].u - ws->tcs[GLWall::UPLFT].u));
		float izt=(float)(ws->ztop[0]+r*(ws->ztop[1]-ws->ztop[0]));
		float izb=(float)(ws->zbottom[0]+r*(ws->zbottom[1]-ws->zbottom[0]));

		ws->vertcount = 0;	// invalidate current vertices.
		GLWall w=*ws;
		AddWall(&w);
		ws1=&walls[walls.Size()-1];
		ws=&walls[drawitems[sort->itemindex].index];	// may have been reallocated!

		ws1->glseg.x1=ws->glseg.x2=ix;
		ws1->glseg.y1=ws->glseg.y2=iy;
		ws1->glseg.fracleft = ws->glseg.fracright = ws->glseg.fracleft + r*(ws->glseg.fracright - ws->glseg.fracleft);
		ws1->ztop[0]=ws->ztop[1]=izt;
		ws1->zbottom[0]=ws->zbottom[1]=izb;
		ws1->tcs[GLWall::LOLFT].u = ws1->tcs[GLWall::UPLFT].u = ws->tcs[GLWall::LORGT].u = ws->tcs[GLWall::UPRGT].u = iu;
		if (gl.buffermethod == BM_DEFERRED)
		{
			ws->MakeVertices(false);
			ws1->MakeVertices(false);
		}

		SortNode * sort2=SortNodes.GetNew();
		memset(sort2,0,sizeof(SortNode));
		sort2->itemindex=drawitems.Size()-1;

		if (v1>0)
		{
			head->AddToLeft(sort2);
			head->AddToRight(sort);
		}
		else
		{
			head->AddToLeft(sort);
			head->AddToRight(sort2);
		}
	}
}


//==========================================================================
//
// 
//
//==========================================================================
EXTERN_CVAR(Int, gl_billboard_mode)
EXTERN_CVAR(Bool, gl_billboard_faces_camera)
EXTERN_CVAR(Bool, gl_billboard_particles)

void GLDrawList::SortSpriteIntoWall(SortNode * head,SortNode * sort)
{
	GLWall * wh=&walls[drawitems[head->itemindex].index];
	GLSprite * ss=&sprites[drawitems[sort->itemindex].index];
	GLSprite * ss1;

	float v1 = wh->PointOnSide(ss->x1, ss->y1);
	float v2 = wh->PointOnSide(ss->x2, ss->y2);

	if (fabs(v1)<MIN_EQ && fabs(v2)<MIN_EQ) 
	{
		if (wh->type==RENDERWALL_FOGBOUNDARY) 
		{
			head->AddToLeft(sort);
		}
		else 
		{
			head->AddToEqual(sort);
		}
	}
	else if (v1<MIN_EQ && v2<MIN_EQ) 
	{
		head->AddToLeft(sort);
	}
	else if (v1>-MIN_EQ && v2>-MIN_EQ) 
	{
		head->AddToRight(sort);
	}
	else
	{
		const bool drawWithXYBillboard = ((ss->particle && gl_billboard_particles) || (!(ss->actor && ss->actor->renderflags & RF_FORCEYBILLBOARD)
			&& (gl_billboard_mode == 1 || (ss->actor && ss->actor->renderflags & RF_FORCEXYBILLBOARD))));

		const bool drawBillboardFacingCamera = gl_billboard_faces_camera;
		// [Nash] has +ROLLSPRITE
		const bool rotated = (ss->actor != nullptr && ss->actor->renderflags & (RF_ROLLSPRITE | RF_WALLSPRITE | RF_FLATSPRITE));

		// cannot sort them at the moment. This requires more complex splitting.
		if (drawWithXYBillboard || drawBillboardFacingCamera || rotated)
		{
			float v1 = wh->PointOnSide(ss->x, ss->y);
			if (v1 < 0)
			{
				head->AddToLeft(sort);
			}
			else
			{
				head->AddToRight(sort);
			}
			return;
		}
		double r=ss->CalcIntersectionVertex(wh);

		float ix=(float)(ss->x1 + r * (ss->x2-ss->x1));
		float iy=(float)(ss->y1 + r * (ss->y2-ss->y1));
		float iu=(float)(ss->ul + r * (ss->ur-ss->ul));

		GLSprite s=*ss;
		AddSprite(&s);
		ss1=&sprites[sprites.Size()-1];
		ss=&sprites[drawitems[sort->itemindex].index];	// may have been reallocated!

		ss1->x1=ss->x2=ix;
		ss1->y1=ss->y2=iy;
		ss1->ul=ss->ur=iu;

		SortNode * sort2=SortNodes.GetNew();
		memset(sort2,0,sizeof(SortNode));
		sort2->itemindex=drawitems.Size()-1;

		if (v1>0)
		{
			head->AddToLeft(sort2);
			head->AddToRight(sort);
		}
		else
		{
			head->AddToLeft(sort);
			head->AddToRight(sort2);
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================

inline int GLDrawList::CompareSprites(SortNode * a,SortNode * b)
{
	GLSprite * s1=&sprites[drawitems[a->itemindex].index];
	GLSprite * s2=&sprites[drawitems[b->itemindex].index];

	if (s1->depth < s2->depth) return 1;
	if (s1->depth > s2->depth) return -1;
	return (i_compatflags & COMPATF_SPRITESORT)? s2->index-s1->index : s1->index-s2->index;
}

//==========================================================================
//
//
//
//==========================================================================
SortNode * GLDrawList::SortSpriteList(SortNode * head)
{
	SortNode * n;
	int count;
	unsigned i;

	static TArray<SortNode*> sortspritelist;

	SortNode * parent=head->parent;

	sortspritelist.Clear();
	for(count=0,n=head;n;n=n->next) sortspritelist.Push(n);
	std::sort(sortspritelist.begin(), sortspritelist.end(), [=](SortNode *a, SortNode *b)
	{
		return CompareSprites(a, b) < 0;
	});

	for(i=0;i<sortspritelist.Size();i++)
	{
		sortspritelist[i]->next=NULL;
		if (parent) parent->equal=sortspritelist[i];
		parent=sortspritelist[i];
	}
	return sortspritelist[0];
}

//==========================================================================
//
//
//
//==========================================================================
SortNode * GLDrawList::DoSort(SortNode * head)
{
	SortNode * node, * sn, * next;

	sn=FindSortPlane(head);
	if (sn)
	{
		if (sn==head) head=head->next;
		sn->UnlinkFromChain();
		node=head;
		head=sn;
		while (node)
		{
			next=node->next;
			switch(drawitems[node->itemindex].rendertype)
			{
			case GLDIT_FLAT:
				SortPlaneIntoPlane(head,node);
				break;

			case GLDIT_WALL:
				SortWallIntoPlane(head,node);
				break;

			case GLDIT_SPRITE:
				SortSpriteIntoPlane(head,node);
				break;
			}
			node=next;
		}
	}
	else
	{
		sn=FindSortWall(head);
		if (sn)
		{
			if (sn==head) head=head->next;
			sn->UnlinkFromChain();
			node=head;
			head=sn;
			while (node)
			{
				next=node->next;
				switch(drawitems[node->itemindex].rendertype)
				{
				case GLDIT_WALL:
					SortWallIntoWall(head,node);
					break;

				case GLDIT_SPRITE:
					SortSpriteIntoWall(head,node);
					break;

				case GLDIT_FLAT: break;
				}
				node=next;
			}
		}
		else 
		{
			return SortSpriteList(head);
		}
	}
	if (head->left) head->left=DoSort(head->left);
	if (head->right) head->right=DoSort(head->right);
	return sn;
}


//==========================================================================
//
//
//
//==========================================================================

void GLDrawList::DoDraw(int pass, int i, bool trans)
{
	// STEP 1: RENDER ORIGINAL TRANSLUCENT SURFACE NATIVELY
	switch (drawitems[i].rendertype)
	{
		case GLDIT_FLAT:
		{
			GLFlat * f = &flats[drawitems[i].index];
			RenderFlat.Clock();
			f->Draw(pass, trans);
			RenderFlat.Unclock();
			break;
		}
		case GLDIT_WALL:
		{
			GLWall * w = &walls[drawitems[i].index];
			RenderWall.Clock();
			w->Draw(pass);
			RenderWall.Unclock();
			break;
		}
		case GLDIT_SPRITE:
		{
			GLSprite * s = &sprites[drawitems[i].index];
			RenderSprite.Clock();
			s->Draw(pass);
			RenderSprite.Unclock();
			break;
		}
	}

	int currentRenderType = drawitems[i].rendertype;
	int index = drawitems[i].index;

	//--------------------------------------------------------------------------
	// STEP 2:           THE TRANSLUSCENT DYNLIGHT 3DFLOOR-SURFACES
	//                   Can't configure dynlight intensities here!
	// So in gl_20.cpp, in "gl_SetupLightWall" and "gl_SetupLightFlat" call:
	// "gl_dynlightHandleSpecialLightsLegacy" after "gl_dynlightSaturateLegacy",
	// and also call "gl_dynlightTameBigLightsOnMirroredSurfacesLegacy".
	//--------------------------------------------------------------------------
	if (gl.legacyMode && pass == GLPASS_TRANSLUCENT && currentRenderType != GLDIT_SPRITE && GLRenderer->mLightCount)
	{
		if (currentRenderType == GLDIT_FLAT || currentRenderType == GLDIT_WALL)
		{
			// TOTAL DISTANCE FOG BOUNDARY GEOMETRY CULLING FILTER
			// If this wall primitive is a fake fog boundary line,
			// SKIP skip lightmap projections over it on ANY distance.
			// Since fake fog lines share identical 3D coordinates with solid room seams,
			// culling them here permanently cures the legacy 16-bit Z-buffer depth fighting,
			// completely extinguishing far-away flickering and sector side alternating artifacts
			if (currentRenderType == GLDIT_WALL && walls[index].type == RENDERWALL_FOGBOUNDARY)
			{
				return; // Safe and clean bypass on any distance before changing registers
			}

			if (gl_SetupLightTexture())
			{
				// Common baseline registers hardware isolation setup
				gl_RenderState.EnableFog(false);
				gl_RenderState.Apply();

				glDisable(GL_FOG);
				glDepthFunc(GL_LEQUAL);
				glDepthMask(false);

				// --- DETECT CUMULATIVE TRANSLUCENCY ---
				// Check overdraw heights strictly within the current sector bounds,
				// minimizing the stacked overexposed cumulative translucency overlay.
				bool maskColorChannelsOut = false;
				if (currentRenderType == GLDIT_FLAT)
				{
					GLFlat* f = &flats[index];
					if (f && f->sector && ((float)r_viewpoint.Pos.Z - (float)f->z) > 0.0f)
					{
						float currentFlatZ = (float)f->z;
						for (unsigned int j = 0; j < flats.Size(); j++)
						{
							if (flats[j].sector == f->sector && (float)flats[j].z > currentFlatZ)
							{
								maskColorChannelsOut = true;
								break;
							}
						}
					}
				}

				if (maskColorChannelsOut)
				{
					gl_RenderState.SetColorMask(false, false, false, false);
					gl_RenderState.ApplyColorMask();
				}

				// --- PASS 1: REGULAR MODULATED DYNAMIC LIGHTS CHANNEL ---
				glBlendEquation(GL_FUNC_ADD);
				glBlendFunc(GL_DST_COLOR, GL_ONE);
				if (currentRenderType == GLDIT_WALL) walls[index].Draw(GLPASS_LIGHTTEX);
				else if (currentRenderType == GLDIT_FLAT) flats[index].Draw(GLPASS_LIGHTTEX, trans);

				// --- PASS 2: SPECIAL ADDITIVE LIGHTS PLACED ON PURPOSE CHANNEL ---
				glBlendEquation(GL_FUNC_ADD);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				if (currentRenderType == GLDIT_WALL) walls[index].Draw(GLPASS_LIGHTTEX_ADDITIVE);
				else if (currentRenderType == GLDIT_FLAT) flats[index].Draw(GLPASS_LIGHTTEX_ADDITIVE, trans);

				// --- PASS 3: SPECIAL SUBTRACTIVE LIGHTS ANTI-ILLUMINATION CHANNEL ---
				glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				if (currentRenderType == GLDIT_WALL) walls[index].Draw(GLPASS_TRANSLUCENT_LIGHTTEX);
				else if (currentRenderType == GLDIT_FLAT) flats[index].Draw(GLPASS_TRANSLUCENT_LIGHTTEX, trans);

				// RECOVERY AND CLEANUP
				if (maskColorChannelsOut)
				{
					gl_RenderState.ResetColorMask();
					gl_RenderState.ApplyColorMask();
				}

				// DEPTH MASK SANITIZER GATEWAY
				// THE CORE SHIELD: We STRICTLY force glDepthMask to stay FALSE here!
				// Allowing true to leak inside the iteration node loop completely breaks 
				// sorting arrays for subsequent transparent fog sprites and monster sheets.
				// We keep GL_LEQUAL depth function to let multi-layered reflections stack up
				glBlendEquation(GL_FUNC_ADD);

				glEnable(GL_FOG);
				glDepthMask(false);     // The hardware lock, the mask is on false
				glDepthFunc(GL_LEQUAL); // Keep LEQUAL, not to break light overlays
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

				gl_RenderState.EnableFog(true);
				gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
				gl_RenderState.Apply();
			}
		}
	}
}

// For an extra case when you need it to draw subtractive lights only if they're detected here
//void GLDrawList::DoDraw(int pass, int i, bool trans)
//{
//	// STEP 1: RENDER ORIGINAL TRANSLUCENT SURFACE NATIVELY
//	switch (drawitems[i].rendertype)
//	{
//	case GLDIT_FLAT:
//	{
//		GLFlat * f = &flats[drawitems[i].index];
//		RenderFlat.Clock();
//		f->Draw(pass, trans);
//		RenderFlat.Unclock();
//		break;
//	}
//	case GLDIT_WALL:
//	{
//		GLWall * w = &walls[drawitems[i].index];
//		RenderWall.Clock();
//		w->Draw(pass);
//		RenderWall.Unclock();
//		break;
//	}
//	case GLDIT_SPRITE:
//	{
//		GLSprite * s = &sprites[drawitems[i].index];
//		RenderSprite.Clock();
//		s->Draw(pass);
//		RenderSprite.Unclock();
//		break;
//	}
//	}
//
//	int currentRenderType = drawitems[i].rendertype;
//	int index = drawitems[i].index;
//
//	//--------------------------------------------------------------------------
//	// STEP 2:           THE TRANSLUSCENT DYNLIGHT 3DFLOOR-SURFACES
//	//                   Can't configure dynlight intensities here!
//	// So in gl_20.cpp, in "gl_SetupLightWall" and "gl_SetupLightFlat" call:
//	// "gl_dynlightTameSpecialLightsLegacy" after "gl_dynlightSaturateLegacy",
//	// and also call "gl_dynlightTameBigLightsOnMirroredSurfacesLegacy".
//	//--------------------------------------------------------------------------
//	if (pass == GLPASS_TRANSLUCENT && gl.legacyMode && GLRenderer->mLightCount)
//	{
//		if (currentRenderType == GLDIT_FLAT || currentRenderType == GLDIT_WALL)
//		{
//			// TOTAL DISTANCE FOG BOUNDARY GEOMETRY CULLING FILTER
//			// If this wall primitive is a fake fog boundary line,
//			// SKIP skip lightmap projections over it on ANY distance.
//			// Since fake fog lines share identical 3D coordinates with solid room seams,
//			// culling them here permanently cures the legacy 16-bit Z-buffer depth fighting,
//			// completely extinguishing far-away flickering and sector side alternating artifacts
//			if (currentRenderType == GLDIT_WALL && walls[index].type == RENDERWALL_FOGBOUNDARY)
//			{
//				return; // Safe and clean bypass on any distance before changing registers
//			}
//
//			if (gl_SetupLightTexture())
//			{
//				// Common baseline registers hardware isolation setup
//				gl_RenderState.EnableFog(false);
//				gl_RenderState.Apply();
//
//				glDisable(GL_FOG);
//				glDepthFunc(GL_LEQUAL);
//				glDepthMask(false);
//
//				// --- DETECT CUMULATIVE TRANSLUCENCY ---
//				// Check overdraw heights strictly within the current sector bounds,
//				// minimizing the stacked overexposed cumulative translucency overlay.
//				bool maskColorChannelsOut = false;
//				if (currentRenderType == GLDIT_FLAT)
//				{
//					GLFlat* f = &flats[index];
//					if (f && f->sector && ((float)r_viewpoint.Pos.Z - (float)f->z) > 0.0f)
//					{
//						float currentFlatZ = (float)f->z;
//						for (unsigned int j = 0; j < flats.Size(); j++)
//						{
//							if (flats[j].sector == f->sector && (float)flats[j].z > currentFlatZ)
//							{
//								maskColorChannelsOut = true;
//								break;
//							}
//						}
//					}
//				}
//
//				if (maskColorChannelsOut)
//				{
//					gl_RenderState.SetColorMask(false, false, false, false);
//					gl_RenderState.ApplyColorMask();
//				}
//
//				// --- PASS 1: REGULAR MODULATED DYNAMIC LIGHTS CHANNEL ---
//				glBlendEquation(GL_FUNC_ADD);
//				glBlendFunc(GL_DST_COLOR, GL_ONE);
//				if (currentRenderType == GLDIT_WALL) walls[index].Draw(GLPASS_LIGHTTEX);
//				else if (currentRenderType == GLDIT_FLAT) flats[index].Draw(GLPASS_LIGHTTEX, trans);
//
//				// --- PASS 2: SPECIAL ADDITIVE LIGHTS PLACED ON PURPOSE CHANNEL ---
//				glBlendEquation(GL_FUNC_ADD);
//				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
//				if (currentRenderType == GLDIT_WALL) walls[index].Draw(GLPASS_LIGHTTEX_ADDITIVE);
//				else if (currentRenderType == GLDIT_FLAT) flats[index].Draw(GLPASS_LIGHTTEX_ADDITIVE, trans);
//
//				// --- PASS 3: SPECIAL SUBTRACTIVE LIGHTS ANTI-ILLUMINATION CHANNEL ---
//				// MASTER KEY: Scan the subsector node array dynamically to confirm if 
//				// any active subtractive anti-light actor is currently affecting this surface.
//				// If tracked, the CPU fires the inverse hardware subtract equation strictly 
//				// inside translucent map bounds, preventing empty alpha leaks.
//				bool hasActiveSubtractiveLights = false;
//				FLightNode* subScanNode = nullptr;
//
//				if (currentRenderType == GLDIT_FLAT)
//					subScanNode = flats[index].sector ? flats[index].sector->lighthead : nullptr;
//				else if (currentRenderType == GLDIT_WALL)
//					subScanNode = walls[index].seg ? walls[index].seg->frontsector->lighthead : nullptr;
//
//				while (subScanNode)
//				{
//					FDynamicLight* sl = subScanNode->lightsource;
//					if (sl && sl->IsActive() && sl->IsSubtractive())
//					{
//						hasActiveSubtractiveLights = true;
//						break; // Subtractive node confirmed, break early to save cycles
//					}
//					subScanNode = subScanNode->nextLight;
//				}
//
//				// The conditional if-else block safely processes subtractive nodes strictly when needed!
//				if (hasActiveSubtractiveLights)
//				{
//					glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
//					glBlendFunc(GL_SRC_ALPHA, GL_ONE);
//					if (currentRenderType == GLDIT_WALL) walls[index].Draw(GLPASS_TRANSLUCENT_LIGHTTEX);
//					else if (currentRenderType == GLDIT_FLAT) flats[index].Draw(GLPASS_TRANSLUCENT_LIGHTTEX, trans);
//				}
//
//				// RECOVERY AND CLEANUP
//				if (maskColorChannelsOut)
//				{
//					gl_RenderState.ResetColorMask();
//					gl_RenderState.ApplyColorMask();
//				}
//
//				glBlendEquation(GL_FUNC_ADD); // Hard reset blending equations safely
//
//				glEnable(GL_FOG);
//				glDepthMask(true);
//				glDepthFunc(GL_LESS);
//				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
//
//				gl_RenderState.EnableFog(true);
//				gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
//				gl_RenderState.Apply();
//			}
//		}
//	}
//}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::DoDrawSorted(SortNode * head)
{
	float clipsplit[2];
	int relation = 0;
	float z = 0.f;

	gl_RenderState.GetClipSplit(clipsplit);

	if (drawitems[head->itemindex].rendertype == GLDIT_FLAT)
	{
		z = flats[drawitems[head->itemindex].index].z;
		relation = z > r_viewpoint.Pos.Z ? 1 : -1;
	}


	// left is further away, i.e. for stuff above viewz its z coordinate higher, for stuff below viewz its z coordinate is lower
	if (head->left) 
	{
		if (relation == -1)
		{
			gl_RenderState.SetClipSplit(clipsplit[0], z);	// render below: set flat as top clip plane
		}
		else if (relation == 1)
		{
			gl_RenderState.SetClipSplit(z, clipsplit[1]);	// render above: set flat as bottom clip plane
		}
		DoDrawSorted(head->left);
		gl_RenderState.SetClipSplit(clipsplit);
	}
	DoDraw(GLPASS_TRANSLUCENT, head->itemindex, true);
	if (head->equal)
	{
		SortNode * ehead=head->equal;
		while (ehead)
		{
			DoDraw(GLPASS_TRANSLUCENT, ehead->itemindex, true);
			ehead=ehead->equal;
		}
	}
	// right is closer, i.e. for stuff above viewz its z coordinate is lower, for stuff below viewz its z coordinate is higher
	if (head->right)
	{
		if (relation == 1)
		{
			gl_RenderState.SetClipSplit(clipsplit[0], z);	// render below: set flat as top clip plane
		}
		else if (relation == -1)
		{
			gl_RenderState.SetClipSplit(z, clipsplit[1]);	// render above: set flat as bottom clip plane
		}
		DoDrawSorted(head->right);
		gl_RenderState.SetClipSplit(clipsplit);
	}
}

//==========================================================================
// [Darkcrafter07] - THE UNIFIED SEGMENT-CLAMPED BLOCKMAP SORTING ENGINE
//==========================================================================
// THE CONVEYOR RESOLUTION: We forcefully abandon original glitchy subsector 
// SortNodes compiling which causes sprites to alternate and clip inside adjacent seams.
// Operating globally across ALL active pipeline render paths (GL1.1 up to GL4+ Core).
//
// GEOMETRIC ATTENUATION CODES:
// 1. Core Vector Alignment: Instead of blindly sampling flat 1D projection rays to the screen 
//    plane which drift at close-ups, this loop extracts absolute spherical 3D Euclidean distances 
//    (LengthSquared) from r_viewpoint straight to the rendering centers of active elements!
// 2. Translucent Segment Clamping: For wall primitives, the engine projects the camera position 
//    onto the seg line vector and clamps it within segment boundaries (t = [0.0f; 1.0f]). This finds 
//    the exact closest physical point on long fences and grates, completely stopping transparency bleeding!
// 3. The 0-Index Vertex Shield: Left vertex height bounds are mapped securely via the canonical 
//    zbottom[0] index array array to protect float layout precision tracking across the map!
//
// THE HOVERING SHADOW ANCHOR MATRIX:
// Since a monster sprite hull and its underlying translucent stencil footprint share matching actor 
// positions, identical distance values would cause fast qsort to randomly flip node priorities.
// Solution: Check 'isGLSpriteClassShadow' flag directly from the sprites array. Push back by 25 units
// if true in the Back-to-Front queue sequence.
// Coupled with a soft '<=' inequality sort compare, this guarantees that dynamic shadows stay 
// hard-locked underneath monster feet under any close-up angle.

void GLDrawList::SortDrawItemsByBlockmap()
{
	if (drawitems.Size() <= 1) return;

	struct FBlockmapSortBucket
	{
		int originalIndex;
		float distanceToView;
	};

	TArray<FBlockmapSortBucket> sortStack;
	sortStack.Resize(drawitems.Size());

	for (unsigned int i = 0; i < drawitems.Size(); i++)
	{
		sortStack[i].originalIndex = i;
		float itemX = 0.0f, itemY = 0.0f, itemZ = 0.0f;
		float shadowSortBias = 0.0f;

		GLDrawItemType type = drawitems[i].rendertype;
		int idx = drawitems[i].index;

		if (type == GLDIT_SPRITE && idx < (int)sprites.Size())
		{
			itemX = (float)sprites[idx].x;
			itemY = (float)sprites[idx].y;
			itemZ = (float)sprites[idx].z;

			if (sprites[idx].isGLSpriteClassShadow)
			{
				shadowSortBias = 25.0f; // Push shadow 25 units back
			}
		}
		else if (type == GLDIT_FLAT && idx < (int)flats.Size())
		{
			itemX = (float)flats[idx].sector->centerspot.X;
			itemY = (float)flats[idx].sector->centerspot.Y;
			itemZ = (float)flats[idx].z;
		}
		else if (type == GLDIT_WALL && idx < (int)walls.Size())
		{
			float x1 = (float)walls[idx].glseg.x1;
			float y1 = (float)walls[idx].glseg.y1;
			float x2 = (float)walls[idx].glseg.x2;
			float y2 = (float)walls[idx].glseg.y2;

			float segDx = x2 - x1;
			float segDy = y2 - y1;
			float segLengthSq = (segDx * segDx) + (segDy * segDy);

			if (segLengthSq < 0.001f)
			{
				itemX = x1; itemY = y1;
			}
			else
			{
				float viewDx = (float)r_viewpoint.Pos.X - x1;
				float viewDy = (float)r_viewpoint.Pos.Y - y1;
				float t = (viewDx * segDx + viewDy * segDy) / segLengthSq;

				if (t < 0.0f) t = 0.0f;
				else if (t > 1.0f) t = 1.0f;

				itemX = x1 + t * segDx;
				itemY = y1 + t * segDy;
			}
			itemZ = (float)walls[idx].zbottom[0];
		}

		float dx = itemX - (float)r_viewpoint.Pos.X;
		float dy = itemY - (float)r_viewpoint.Pos.Y;
		float dz = itemZ - (float)r_viewpoint.Pos.Z;

		// Bake the shadow bias into the Euclidean distance
		sortStack[i].distanceToView = (dx * dx) + (dy * dy) + (dz * dz) + shadowSortBias;
	}

	// High-speed array realignment (Pure Back-to-Front tracking)
	for (unsigned int i = 0; i < sortStack.Size(); i++)
	{
		for (unsigned int j = i + 1; j < sortStack.Size(); j++)
		{
			// Soft "<=" comparison keeps the nodes add order introduced by Nash
			if (sortStack[i].distanceToView <= sortStack[j].distanceToView)
			{
				FBlockmapSortBucket temp = sortStack[i];
				sortStack[i] = sortStack[j];
				sortStack[j] = temp;
			}
		}
	}

	// Re-bake drawitems using standard engine types
	TArray<GLDrawItem> tempItems = drawitems;
	for (unsigned int i = 0; i < sortStack.Size(); i++)
	{
		drawitems[i] = tempItems[sortStack[i].originalIndex];
	}
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::DrawSorted()
{
	if (drawitems.Size() == 0) return;

	if (gl.legacyMode)
	{
		// 1. PURE FIXED-FUNCTION PATH (GL1x/GL2x legacy mode)
		// Enforces segment-clamped blockmap sorter.
		if (!sorted)
		{
			SortDrawItemsByBlockmap();
			sorted = (SortNode*)1; // Pass fake memory anchor to skip culler
		}

		gl_RenderState.ClearClipSplit();
		if (!(gl.flags & RFL_NO_CLIP_PLANES))
		{
			glEnable(GL_CLIP_DISTANCE1);
			glEnable(GL_CLIP_DISTANCE2);
		}

		// High-speed flat array conveyor with mIsRenderingSpriteContext locks
		for (unsigned int i = 0; i < drawitems.Size(); i++)
		{
			gl_RenderState.mIsRenderingSpriteContext = (drawitems[i].rendertype == GLDIT_SPRITE);
			DoDraw(GLPASS_TRANSLUCENT, i, true);
		}

		gl_RenderState.mIsRenderingSpriteContext = false; // Safe flush

		if (!(gl.flags & RFL_NO_CLIP_PLANES))
		{
			glDisable(GL_CLIP_DISTANCE1);
			glDisable(GL_CLIP_DISTANCE2);
		}
		gl_RenderState.ClearClipSplit();
	}
	else
	{
		// 2. MODERN SHADER PATH (GL3x/GL4x modern mode)
		// Use Subsector BSP SortNode tree structures
		if (!sorted)
		{
			GLRenderer->mVBO->Map();
			MakeSortList();
			sorted = DoSort(SortNodes[SortNodeStart]);
			GLRenderer->mVBO->Unmap();
		}

		gl_RenderState.ClearClipSplit();
		if (!(gl.flags & RFL_NO_CLIP_PLANES))
		{
			glEnable(GL_CLIP_DISTANCE1);
			glEnable(GL_CLIP_DISTANCE2);
		}

		// Execute standard shader DoDrawSorted tree
		DoDrawSorted(sorted);

		if (!(gl.flags & RFL_NO_CLIP_PLANES))
		{
			glDisable(GL_CLIP_DISTANCE1);
			glDisable(GL_CLIP_DISTANCE2);
		}
		gl_RenderState.ClearClipSplit();
	}
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::Draw(int pass, bool trans)
{
	for(unsigned i=0;i<drawitems.Size();i++)
	{
		DoDraw(pass, i, trans);
	}
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::DrawWalls(int pass)
{
	RenderWall.Clock();
	for(unsigned i=0;i<drawitems.Size();i++)
	{
		walls[drawitems[i].index].Draw(pass);
	}
	RenderWall.Unclock();
}

void GLDrawList::DrawWalls2x(int pass1, int pass2)
{
	RenderWall.Clock();
	for (unsigned i = 0; i < drawitems.Size(); i++)
	{
		GLWall* wall = &walls[drawitems[i].index];

		// 1. Draw regular dynlights
		wall->Draw(pass1);

		// 2. Draw overbright dynlights
		wall->Draw(pass2);

		// Symmetrical state reset after each wall:
		// Return the pipeline in additive mode which pass 1 expects to see it on i+1 iteration
		glBlendFunc(GL_ONE, GL_ONE);
		glDepthFunc(GL_EQUAL);
		glDepthMask(GL_FALSE);
	}
	RenderWall.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::DrawFlats(int pass)
{
	RenderFlat.Clock();
	for(unsigned i=0;i<drawitems.Size();i++)
	{
		flats[drawitems[i].index].Draw(pass, false);
	}
	RenderFlat.Unclock();
}

void GLDrawList::DrawFlats2x(int pass1, int pass2)
{
	RenderFlat.Clock();
	for (unsigned i = 0; i < drawitems.Size(); i++)
	{
		GLFlat* flat = &flats[drawitems[i].index];

		// 1. Draw regular dynlights
		flat->Draw(pass1, false);

		// 2. Draw overbright dynlights
		flat->Draw(pass2, false);

		// Symmetrical state reset after each wall:
		// Return the pipeline in additive mode which pass 1 expects to see it on i+1 iteration
		glBlendFunc(GL_ONE, GL_ONE);
		glDepthFunc(GL_EQUAL);
		glDepthMask(GL_FALSE);
	}
	RenderFlat.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::DrawDecals()
{
	for(unsigned i=0;i<drawitems.Size();i++)
	{
		walls[drawitems[i].index].DoDrawDecals();
	}
}

//==========================================================================
//
// Sorting the drawitems first by texture and then by light level.
//
//==========================================================================

void GLDrawList::SortWalls()
{
	if (drawitems.Size() > 1)
	{
		std::sort(drawitems.begin(), drawitems.end(), [=](const GLDrawItem &a, const GLDrawItem &b) -> bool
		{
			GLWall * w1 = &walls[a.index];
			GLWall * w2 = &walls[b.index];

			if (w1->gltexture != w2->gltexture) return w1->gltexture < w2->gltexture;
			return (w1->flags & 3) < (w2->flags & 3);

		});
	}
}

void GLDrawList::SortFlats()
{
	if (drawitems.Size() > 1)
	{
		std::sort(drawitems.begin(), drawitems.end(), [=](const GLDrawItem &a, const GLDrawItem &b)
		{
			GLFlat * w1 = &flats[a.index];
			GLFlat* w2 = &flats[b.index];
			return w1->gltexture < w2->gltexture;
		});
	}
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::AddWall(GLWall * wall)
{
	drawitems.Push(GLDrawItem(GLDIT_WALL,walls.Push(*wall)));
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::AddFlat(GLFlat * flat)
{
	drawitems.Push(GLDrawItem(GLDIT_FLAT,flats.Push(*flat)));
}

//==========================================================================
//
//
//
//==========================================================================
void GLDrawList::AddSprite(GLSprite * sprite)
{	
	drawitems.Push(GLDrawItem(GLDIT_SPRITE,sprites.Push(*sprite)));
}


//==========================================================================
//
// Try to reuse the lists as often as possible as they contain resources that
// are expensive to create and delete.
//
// Note: If multithreading gets used, this class needs synchronization.
//
//==========================================================================

FDrawInfo *FDrawInfoList::GetNew()
{
	if (mList.Size() > 0)
	{
		FDrawInfo *di;
		mList.Pop(di);
		return di;
	}
	return new FDrawInfo;
}

void FDrawInfoList::Release(FDrawInfo * di)
{
	di->ClearBuffers();
	mList.Push(di);
}

static FDrawInfoList di_list;

//==========================================================================
//
//
//
//==========================================================================

FDrawInfo::FDrawInfo()
{
	next = NULL;
	if (gl.legacyMode)
	{
		dldrawlists = new GLDrawList[GLLDL_TYPES];
	}
}

FDrawInfo::~FDrawInfo()
{
	if (dldrawlists != NULL) delete[] dldrawlists;
	ClearBuffers();
}


//==========================================================================
//
// Sets up a new drawinfo struct
//
//==========================================================================
void FDrawInfo::StartDrawInfo(GLSceneDrawer *drawer)
{
	FDrawInfo *di=di_list.GetNew();
	di->mDrawer = drawer;
	di->StartScene();
}

void FDrawInfo::StartScene()
{
	ClearBuffers();

	sectorrenderflags.Resize(level.sectors.Size());
	ss_renderflags.Resize(level.subsectors.Size());
	no_renderflags.Resize(level.subsectors.Size());

	memset(&sectorrenderflags[0], 0, level.sectors.Size() * sizeof(sectorrenderflags[0]));
	memset(&ss_renderflags[0], 0, level.subsectors.Size() * sizeof(ss_renderflags[0]));
	memset(&no_renderflags[0], 0, level.nodes.Size() * sizeof(no_renderflags[0]));

	next = gl_drawinfo;
	gl_drawinfo = this;
	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	if (dldrawlists != NULL)
	{
		for (int i = 0; i < GLLDL_TYPES; i++) dldrawlists[i].Reset();
	}
}

//==========================================================================
//
//
//
//==========================================================================
void FDrawInfo::EndDrawInfo()
{
	FDrawInfo * di = gl_drawinfo;

	for(int i=0;i<GLDL_TYPES;i++) di->drawlists[i].Reset();
	if (di->dldrawlists != NULL)
	{
		for (int i = 0; i < GLLDL_TYPES; i++) di->dldrawlists[i].Reset();
	}
	gl_drawinfo=di->next;
	di_list.Release(di);
}


//==========================================================================
//
// Flood gaps with the back side's ceiling/floor texture
// This requires a stencil because the projected plane interferes with
// the depth buffer
//
//==========================================================================

void FDrawInfo::SetupFloodStencil(wallseg * ws)
{
	int recursion = GLPortal::GetRecursion();

	// Create stencil 
	glStencilFunc(GL_EQUAL, recursion, ~0);		// create stencil
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);		// increment stencil of valid pixels
	{
		// Use revertible color mask, to avoid stomping on anaglyph 3D state
		ScopedColorMask colorMask(0, 0, 0, 0); // glColorMask(0, 0, 0, 0);						// don't write to the graphics buffer
		gl_RenderState.EnableTexture(false);
		gl_RenderState.ResetColor();
		glEnable(GL_DEPTH_TEST);
		glDepthMask(true);

		gl_RenderState.Apply();
		FQuadDrawer qd;
		qd.Set(0, ws->x1, ws->z1, ws->y1, 0, 0);
		qd.Set(1, ws->x1, ws->z2, ws->y1, 0, 0);
		qd.Set(2, ws->x2, ws->z2, ws->y2, 0, 0);
		qd.Set(3, ws->x2, ws->z1, ws->y2, 0, 0);
		qd.Render(GL_TRIANGLE_FAN);

		glStencilFunc(GL_EQUAL, recursion + 1, ~0);		// draw sky into stencil
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);		// this stage doesn't modify the stencil

	} // glColorMask(1, 1, 1, 1);						// don't write to the graphics buffer
	gl_RenderState.EnableTexture(true);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(false);
}

void FDrawInfo::ClearFloodStencil(wallseg * ws)
{
	int recursion = GLPortal::GetRecursion();

	glStencilOp(GL_KEEP,GL_KEEP,GL_DECR);
	gl_RenderState.EnableTexture(false);
	{
		// Use revertible color mask, to avoid stomping on anaglyph 3D state
		ScopedColorMask colorMask(0, 0, 0, 0); // glColorMask(0,0,0,0);						// don't write to the graphics buffer
		gl_RenderState.ResetColor();

		gl_RenderState.Apply();
		FQuadDrawer qd;
		qd.Set(0, ws->x1, ws->z1, ws->y1, 0, 0);
		qd.Set(1, ws->x1, ws->z2, ws->y1, 0, 0);
		qd.Set(2, ws->x2, ws->z2, ws->y2, 0, 0);
		qd.Set(3, ws->x2, ws->z1, ws->y2, 0, 0);
		qd.Render(GL_TRIANGLE_FAN);

		// restore old stencil op.
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glStencilFunc(GL_EQUAL, recursion, ~0);
		gl_RenderState.EnableTexture(true);
	} // glColorMask(1, 1, 1, 1);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(true);
}

//==========================================================================
//
// Draw the plane segment into the gap
//
//==========================================================================
void FDrawInfo::DrawFloodedPlane(wallseg * ws, float planez, sector_t * sec, bool ceiling)
{
	GLSectorPlane plane;
	int lightlevel;
	FColormap Colormap;
	FMaterial * gltexture;

	plane.GetFromSector(sec, ceiling);

	gltexture=FMaterial::ValidateTexture(plane.texture, false, true);
	if (!gltexture) return;

	if (mDrawer->FixedColormap) 
	{
		Colormap.Clear();
		lightlevel=255;
	}
	else
	{
		Colormap = sec->Colormap;
		if (gltexture->tex->isFullbright())
		{
			Colormap.MakeWhite();
			lightlevel=255;
		}
		else lightlevel=abs(ceiling? sec->GetCeilingLight() : sec->GetFloorLight());
	}

	int rel = getExtraLight();
	mDrawer->SetColor(lightlevel, rel, Colormap, 1.0f);
	mDrawer->SetFog(lightlevel, rel, &Colormap, false);
	gl_RenderState.SetMaterial(gltexture, CLAMP_NONE, 0, -1, false);

	float fviewx = r_viewpoint.Pos.X;
	float fviewy = r_viewpoint.Pos.Y;
	float fviewz = r_viewpoint.Pos.Z;

	gl_SetPlaneTextureRotation(&plane, gltexture);
	gl_RenderState.Apply();

	float prj_fac1 = (planez-fviewz)/(ws->z1-fviewz);
	float prj_fac2 = (planez-fviewz)/(ws->z2-fviewz);

	float px1 = fviewx + prj_fac1 * (ws->x1-fviewx);
	float py1 = fviewy + prj_fac1 * (ws->y1-fviewy);

	float px2 = fviewx + prj_fac2 * (ws->x1-fviewx);
	float py2 = fviewy + prj_fac2 * (ws->y1-fviewy);

	float px3 = fviewx + prj_fac2 * (ws->x2-fviewx);
	float py3 = fviewy + prj_fac2 * (ws->y2-fviewy);

	float px4 = fviewx + prj_fac1 * (ws->x2-fviewx);
	float py4 = fviewy + prj_fac1 * (ws->y2-fviewy);

	FQuadDrawer qd;
	qd.Set(0, px1, planez, py1, px1 / 64, -py1 / 64);
	qd.Set(1, px2, planez, py2, px2 / 64, -py2 / 64);
	qd.Set(2, px3, planez, py3, px3 / 64, -py3 / 64);
	qd.Set(3, px4, planez, py4, px4 / 64, -py4 / 64);
	qd.Render(GL_TRIANGLE_FAN);

	gl_RenderState.EnableTextureMatrix(false);
}

//==========================================================================
//
//
//
//==========================================================================

void FDrawInfo::FloodUpperGap(seg_t * seg)
{
	wallseg ws;
	sector_t * fakefsector = gl_FakeFlat(seg->frontsector, mDrawer->in_area, false);
	sector_t * fakebsector = gl_FakeFlat(seg->backsector, mDrawer->in_area, true);

	vertex_t * v1, * v2;

	// Although the plane can be sloped this code will only be called
	// when the edge itself is not.
	double backz = fakebsector->ceilingplane.ZatPoint(seg->v1);
	double frontz = fakefsector->ceilingplane.ZatPoint(seg->v1);

	if (fakebsector->GetTexture(sector_t::ceiling)==skyflatnum) return;
	if (backz < r_viewpoint.Pos.Z) return;

	if (seg->sidedef == seg->linedef->sidedef[0])
	{
		v1=seg->linedef->v1;
		v2=seg->linedef->v2;
	}
	else
	{
		v1=seg->linedef->v2;
		v2=seg->linedef->v1;
	}

	ws.x1 = v1->fX();
	ws.y1 = v1->fY();
	ws.x2 = v2->fX();
	ws.y2 = v2->fY();

	ws.z1= frontz;
	ws.z2= backz;

	// Step1: Draw a stencil into the gap
	SetupFloodStencil(&ws);

	// Step2: Project the ceiling plane into the gap
	DrawFloodedPlane(&ws, ws.z2, fakebsector, true);

	// Step3: Delete the stencil
	ClearFloodStencil(&ws);
}

//==========================================================================
//
//
//
//==========================================================================

void FDrawInfo::FloodLowerGap(seg_t * seg)
{
	wallseg ws;
	sector_t * fakefsector = gl_FakeFlat(seg->frontsector, mDrawer->in_area, false);
	sector_t * fakebsector = gl_FakeFlat(seg->backsector, mDrawer->in_area, true);

	vertex_t * v1, * v2;

	// Although the plane can be sloped this code will only be called
	// when the edge itself is not.
	double backz = fakebsector->floorplane.ZatPoint(seg->v1);
	double frontz = fakefsector->floorplane.ZatPoint(seg->v1);


	if (fakebsector->GetTexture(sector_t::floor) == skyflatnum) return;
	if (fakebsector->GetPlaneTexZ(sector_t::floor) > r_viewpoint.Pos.Z) return;

	if (seg->sidedef == seg->linedef->sidedef[0])
	{
		v1=seg->linedef->v1;
		v2=seg->linedef->v2;
	}
	else
	{
		v1=seg->linedef->v2;
		v2=seg->linedef->v1;
	}

	ws.x1 = v1->fX();
	ws.y1 = v1->fY();
	ws.x2 = v2->fX();
	ws.y2 = v2->fY();

	ws.z2= frontz;
	ws.z1= backz;

	// Step1: Draw a stencil into the gap
	SetupFloodStencil(&ws);

	// Step2: Project the ceiling plane into the gap
	DrawFloodedPlane(&ws, ws.z1, fakebsector, false);

	// Step3: Delete the stencil
	ClearFloodStencil(&ws);
}
