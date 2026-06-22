// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
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

/*
** gl_20.h
**
** Fallback code for ancient hardware
** This file collects everything larger that is only needed for
** OpenGL v1.1 is required (1997+ cards?), the same file for GL2x path.
** The difference GL2 makes is no blurry textures thanks to NPOT support.
**
*/

#ifndef __GL_20_H__
#define __GL_20_H__

#include "gl/system/gl_system.h"
#include "menu/menu.h"
#include "tarray.h"
#include "doomtype.h"
#include "m_argv.h"
#include "zstring.h"
#include "i_system.h"
#include "v_text.h"
#include "r_utility.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "g_levellocals.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/utility/gl_geometric.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_scenedrawer.h"
#include "gl/data/gl_vertexbuffer.h"

#include "gl/shaders/gl_shader.h"



// Menu patching for legacy mode capabilities
void gl_PatchMenu();

// Texture management functions for fixed function pipeline
void gl_SetTextureMode(int type);

// Setup parameters to project one dynamic light onto a wall plane
bool gl_SetupLightWall(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive);

// Setup parameters to project one dynamic light onto a flat plane (floor/ceiling)
bool gl_SetupLightFlat(int group, Plane & p, FDynamicLight * light, FVector3 & nearPt, FVector3 & up, FVector3 & right, float & scale, bool checkside, bool additive);

// Binds the native dynamic light spotlight filter mask texture (glLight) to the render state
bool gl_SetupLightTexture();
bool gl_SetupLightTextureForDynlightLegacy();

// Main rendering loops for legacy multipass fallback path
void gl_FillScreen();

#endif // __GL_20_H__
