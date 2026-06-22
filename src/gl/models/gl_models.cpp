// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
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
** gl_models.cpp
**
** OpenGL renderer model handling code
**
**/

#include "gl/system/gl_system.h"
#include "w_wad.h"
#include "cmdlib.h"
#include "sc_man.h"
#include "m_crc32.h"
#include "c_console.h"
#include "g_game.h"
#include "doomstat.h"
#include "g_level.h"
#include "r_state.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "r_utility.h"
#include "i_time.h"
//#include "resources/voxels.h"
//#include "gl/gl_intern.h"

#include "gl/system/gl_interface.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/models/gl_models.h"
#include "gl/textures/gl_material.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/shaders/gl_shader.h"

// GL1x/GL2x legacy dynlight includes, externs and vars - START
#include "gl/compatibility/gl_20.h"
#include "gl/dynlights/gl_dynlight.h"
#include "r_data/models/models_ue1.h"
#include "r_data/models/models_obj.h"
	extern FDynLightData modellightdata;
	extern bool g_legacyLightActive;
	extern float g_legacyLightX;
	extern float g_legacyLightZ;
	extern float g_legacyLightY;
	extern float g_legacyLightRadius;
	extern float g_legacyLightR;
	extern float g_legacyLightG;
	extern float g_legacyLightB;

	extern struct FLegacyModelLightCache // defined in gl_spritelight.cpp
	{
		float x, y, z;      // Relative position of the dynamic light source
		float trueX, trueY, trueZ; // Absolute world dynlight coordinates
		float radius;       // Interaction radius of the light
		float r, g, b;      // Evaluated intensity color channels of the flare
	};
	extern bool g_legacyLightActive;
	extern int g_legacyModelSectorLight;
	extern TArray<FLegacyModelLightCache> g_legacyModelLights;
// GL1x/GL2x legacy dynlight includes, externs and vars - FINISH

CVAR(Bool, gl_light_models, true, CVAR_ARCHIVE)

extern int modellightindex;

VSMatrix FGLModelRenderer::GetViewToWorldMatrix()
{
	VSMatrix objectToWorldMatrix;
	gl_RenderState.mViewMatrix.inverseMatrix(objectToWorldMatrix);
	return objectToWorldMatrix;
}

void FGLModelRenderer::BeginDrawModel(AActor *actor, FSpriteModelFrame *smf, const VSMatrix &objectToWorldMatrix, bool mirrored)
{
	// [Darkcrafter07]: Cache the active actor pointer for SetMaterial context usage
	this->currentRenderActor = actor;

	glDepthFunc(GL_LEQUAL);
	gl_RenderState.EnableTexture(true);

	if (!(actor->RenderStyle == LegacyRenderStyles[STYLE_Normal]) && !(smf->flags & MDL_DONTCULLBACKFACES))
	{
		glEnable(GL_CULL_FACE);
		glFrontFace((mirrored ^ GLPortal::isMirrored()) ? GL_CCW : GL_CW);
	}

	gl_RenderState.mModelMatrix = objectToWorldMatrix;
	gl_RenderState.EnableModelMatrix(true);

	// [Darkcrafter07]: CRITICAL RESET FOR SOFT LIGHT LEVEL OVERRIDES
	// We force set the soft light level register cleanly back to maximum 255 default factor.
	// This forces mLightParms[3] to evaluate strictly to 1.0f, completely destroying 
	// random flickering, frame fullbright burnouts, and shading drops on the model hulls!
	if (gl.lightmethod == LM_LEGACY)
	{
		gl_RenderState.SetSoftLightLevel(255);
	}
}

void FGLModelRenderer::EndDrawModel(AActor *actor, FSpriteModelFrame *smf)
{
	// [Darkcrafter07]: GL1x/GL2x Legacy Brightmap Overlay
	if (gl.legacyMode && gl_RenderState.IsBrightmapEnabled() && !(actor->renderflags & RF_FULLBRIGHT))
	{
		for (int i = 0; i < MAX_MODELS_PER_FRAME; i++)
		{
			if (smf->modelIDs[i] == -1) continue;

			FTextureID skinID = smf->skinIDs[i];
			FTexture *skin = skinID.isValid() ? TexMan[skinID] : nullptr;
			FMaterial *baseMat = FMaterial::ValidateTexture(skin, false);

			// Get your colorized/clipped brightmap from gl_material.cpp logic
			FMaterial *bmMat = baseMat ? baseMat->GetBrightmapLegacy() : nullptr;

			if (bmMat)
			{
				// 1. Force additive blending
				gl_RenderState.EnableFog(false);
				gl_RenderState.BlendFunc(GL_ONE, GL_ONE);
				gl_RenderState.SetTextureMode(TM_BRIGHTMAP_LEGACY);

				// 2. Calculate intensity with new formula
				int lightlevel = actor->Sector->lightlevel;
				int rel = 0; // Assuming no extra light by default, adjust if needed

				// Calculate total current light level
				int totalLight = lightlevel + rel;
				if (totalLight > 255) totalLight = 255;
				if (totalLight < 0) totalLight = 0;

				float lightFactor;
				if (totalLight < 96)
				{
					lightFactor = 1.0f; // Full effect below 96 light level
				}
				else
				{
					// Optimized inverse light factor for the 96-255 range
					const float rangeFactorInv = 1.0f / (255.0f - 96.0f);
					float factor = 1.0f - ((float)(totalLight - 96) * rangeFactorInv);

					// Add subtle parabola bump to mid-range
					factor = factor + 0.1f * factor * (1.0f - factor);

					// Scale intensity to drop from 1.0 to 0.01
					lightFactor = (factor * 0.99f) + 0.01f;
				}

				// Clamp the final value
				if (lightFactor > 1.0f) lightFactor = 1.0f;
				if (lightFactor < 0.0f) lightFactor = 0.0f;

				// Apply intensity to the brightmap
				gl_RenderState.SetColor(lightFactor, lightFactor, lightFactor, 1.0f);

				// 3. Setup ONLY the brightmap material
				int translation = (smf->flags & MDL_IGNORETRANSLATION) ? 0 : actor->Translation;
				gl_RenderState.SetMaterial(bmMat, CLAMP_NONE, translation, -1, false);
				gl_RenderState.Apply();

				// 4. Redraw ONLY the geometry with the brightmap texture
				this->RenderFrameModels(smf, actor->state, actor->tics, actor->GetClass(), translation);
			}
		}

		if (gl.legacyMode)
		{
			glActiveTexture(GL_TEXTURE1);
			glDisable(GL_TEXTURE_GEN_S);
			glDisable(GL_TEXTURE_GEN_T);
			glDisable(GL_TEXTURE_2D);
			glActiveTexture(GL_TEXTURE0);
		}

		// 5. Restore standard state
		gl_RenderState.EnableFog(true);
		gl_RenderState.SetTextureMode(TM_MODULATE);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		gl_RenderState.ResetColor();
	}

	// Original cleanup code
	gl_RenderState.EnableModelMatrix(false);
	glDepthFunc(GL_LESS);

	// Reset backface culling if it was enabled for translucency
	if (!(actor->RenderStyle == LegacyRenderStyles[STYLE_Normal]) && !(smf->flags & MDL_DONTCULLBACKFACES))
		glDisable(GL_CULL_FACE);
}

void FGLModelRenderer::BeginDrawHUDModel(AActor *actor, const VSMatrix &objectToWorldMatrix, bool mirrored)
{
	glDepthFunc(GL_LEQUAL);

	// [BB] In case the model should be rendered translucent, do back face culling.
	// This solves a few of the problems caused by the lack of depth sorting.
	// TO-DO: Implement proper depth sorting.
	if (!(actor->RenderStyle == LegacyRenderStyles[STYLE_Normal]))
	{
		glEnable(GL_CULL_FACE);
		glFrontFace((mirrored ^ GLPortal::isMirrored()) ? GL_CW : GL_CCW);
	}

	gl_RenderState.mModelMatrix = objectToWorldMatrix;
	gl_RenderState.EnableModelMatrix(true);
}

void FGLModelRenderer::EndDrawHUDModel(AActor *actor)
{
	// --- Darkcrafter07: Legacy Brightmap Overlay for HUD/Weapon Models ---
	// We check if the actor is valid and if we are in legacy mode with brightmaps enabled
	FSpriteModelFrame *smf;
	AActor *weapon;

	if (actor && actor->player && gl.legacyMode && gl_RenderState.IsBrightmapEnabled() && !(actor->renderflags & RF_FULLBRIGHT))
	{

		// We need to find the specific model frame being rendered for the player's weapon.
		// We can get the current weapon class and the player's sprite state.
		weapon = actor->player->ReadyWeapon;

	}
	smf = FindModelFrame(weapon->GetClass(), actor->sprite, actor->frame, false);

	if (smf)
	{
		for (int i = 0; i < MAX_MODELS_PER_FRAME; i++)
		{
			if (smf->modelIDs[i] == -1) continue;

			// Get the brightmap using your custom colorization logic
			FTextureID skinID = smf->skinIDs[i];
			FTexture *skin = skinID.isValid() ? TexMan[skinID] : nullptr;
			FMaterial *baseMat = FMaterial::ValidateTexture(skin, false);
			FMaterial *bmMat = baseMat ? baseMat->GetBrightmapLegacy() : nullptr;

			if (bmMat)
			{
				// 1. Setup Additive State
				gl_RenderState.EnableFog(false);
				gl_RenderState.BlendFunc(GL_ONE, GL_ONE);
				gl_RenderState.SetTextureMode(TM_BRIGHTMAP_LEGACY);

				// 2. Calculate Intensity (Using weapon-specific lighting logic)
				int lightlevel = actor->Sector ? actor->Sector->lightlevel : 128;
				float lightFactor;
				if (lightlevel < 128) lightFactor = 1.0f;
				else
				{
					lightFactor = 1.0f - (float)(lightlevel - 128) / 127.0f;
					lightFactor = 0.15f + 0.85f * lightFactor;
				}

				gl_RenderState.SetColor(lightFactor, lightFactor, lightFactor, 1.0f);

				// 3. Apply Brightmap and redraw
				int translation = (smf->flags & MDL_IGNORETRANSLATION) ? 0 : actor->Translation;
				gl_RenderState.SetMaterial(bmMat, CLAMP_NONE, translation, -1, false);
				gl_RenderState.Apply();

				// Redraw the weapon geometry as an additive layer
				this->RenderFrameModels(smf, actor->state, actor->tics, weapon->GetClass(), translation);
			}
		}
		// 4. Restore State
		gl_RenderState.EnableFog(true);
		gl_RenderState.SetTextureMode(TM_MODULATE);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		gl_RenderState.ResetColor();
	}

	// Original factory cleanup code inside gl_models.cpp EndDrawModel
	gl_RenderState.EnableModelMatrix(false);
	glDepthFunc(GL_LESS);

	// Reset backface culling if it was enabled for translucency
	if (!(actor->RenderStyle == LegacyRenderStyles[STYLE_Normal]) && !(smf->flags & MDL_DONTCULLBACKFACES))
		glDisable(GL_CULL_FACE);

	// ==============================================================================
	// [Darkcrafter07]: ATOMIC CROSS-SUBSECTOR CONTEXT FLUSH
	// FIXED: We clear the global lights array cache pool strictly at the end of EndDrawModel!
	// This forces multi-pass subsectors to re-compile active lights freshly, 
	// completely destroying the "pop in and out" flickering bug on giant mountain meshes!
	// ==============================================================================
	if (gl.lightmethod == LM_LEGACY)
	{		
		g_legacyModelLights.Clear();
		g_legacyLightActive = false;
	}
}

IModelVertexBuffer *FGLModelRenderer::CreateVertexBuffer(bool needindex, bool singleframe)
{
	return new FModelVertexBuffer(needindex, singleframe);
}

void FGLModelRenderer::SetVertexBuffer(IModelVertexBuffer *buffer)
{
	gl_RenderState.SetVertexBuffer((FModelVertexBuffer*)buffer);
}

void FGLModelRenderer::ResetVertexBuffer()
{
	gl_RenderState.SetVertexBuffer(GLRenderer->mVBO);
}

void FGLModelRenderer::SetInterpolation(double inter)
{
	gl_RenderState.SetInterpolationFactor((float)inter);
}

void FGLModelRenderer::SetMaterial(FTexture *skin, bool clampNoFilter, int translation)
{
	// [Darkcrafter07]: Prevention of texture overwriting during brightmap pass ---
	// If we are currently rendering a legacy brightmap overlay, skip the diffuse
	// texture setup to prevent it from blending over our brightmap layer.
	if (gl.legacyMode && gl_RenderState.GetTextureMode() == TM_BRIGHTMAP_LEGACY)
	{
		gl_RenderState.Apply();
		return;
	}

	// Default modulate rendering path
	FMaterial * tex = FMaterial::ValidateTexture(skin, false);
	if (tex != nullptr)
	{
		gl_RenderState.SetMaterial(tex, clampNoFilter ? CLAMP_NOFILTER : CLAMP_NONE, translation, -1, false);
		gl_RenderState.Apply();

		if (modellightindex != -1) gl_RenderState.ApplyLightIndex(modellightindex);
	}
}

void FGLModelRenderer::DrawArrays(int start, int count)
{
	glDrawArrays(GL_TRIANGLES, start, count);
}

void FGLModelRenderer::DrawElements(int numIndices, size_t offset)
{
	glDrawElements(GL_TRIANGLES, numIndices, GL_UNSIGNED_INT, (void*)(intptr_t)offset);
}

//===========================================================================
//
// Uses a hardware buffer if either single frame (i.e. no interpolation needed)
// or shading is available (interpolation is done by the vertex shader)
//
// If interpolation has to be done on the CPU side this will fall back
// to CPU-side arrays.
//
//===========================================================================

FModelVertexBuffer::FModelVertexBuffer(bool needindex, bool singleframe)
	: FVertexBuffer(singleframe || !gl.legacyMode)
{
	vbo_ptr = nullptr;
	ibo_id = 0;
	if (needindex)
	{
		glGenBuffers(1, &ibo_id);	// The index buffer can always be a real buffer.
	}
}

//===========================================================================
//
//
//
//===========================================================================

void FModelVertexBuffer::BindVBO()
{
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_id);
	glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
	if (!gl.legacyMode)
	{
		glEnableVertexAttribArray(VATTR_VERTEX);
		glEnableVertexAttribArray(VATTR_TEXCOORD);
		glEnableVertexAttribArray(VATTR_VERTEX2);
		glEnableVertexAttribArray(VATTR_NORMAL);
		glDisableVertexAttribArray(VATTR_COLOR);
	}
	else
	{
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_NORMAL_ARRAY);
	}
}

//===========================================================================
//
//
//
//===========================================================================

FModelVertexBuffer::~FModelVertexBuffer()
{
	if (ibo_id != 0)
	{
		glDeleteBuffers(1, &ibo_id);
	}
	if (vbo_ptr != nullptr)
	{
		delete[] vbo_ptr;
	}
}

//===========================================================================
//
//
//
//===========================================================================

FModelVertex *FModelVertexBuffer::LockVertexBuffer(unsigned int size)
{
	if (vbo_id > 0)
	{
		glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
		glBufferData(GL_ARRAY_BUFFER, size * sizeof(FModelVertex), nullptr, GL_STATIC_DRAW);
		if (!gl.legacyMode)
			return (FModelVertex*)glMapBufferRange(GL_ARRAY_BUFFER, 0, size * sizeof(FModelVertex), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
		else
			return (FModelVertex*)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
	}
	else
	{
		if (vbo_ptr != nullptr) delete[] vbo_ptr;
		vbo_ptr = new FModelVertex[size];
		memset(vbo_ptr, 0, size * sizeof(FModelVertex));
		return vbo_ptr;
	}
}

//===========================================================================
//
//
//
//===========================================================================

void FModelVertexBuffer::UnlockVertexBuffer()
{
	if (vbo_id > 0)
	{
		glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
		glUnmapBuffer(GL_ARRAY_BUFFER);
	}
}

//===========================================================================
//
//
//
//===========================================================================

unsigned int *FModelVertexBuffer::LockIndexBuffer(unsigned int size)
{
	if (ibo_id != 0)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_id);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, size * sizeof(unsigned int), NULL, GL_STATIC_DRAW);
		if (!gl.legacyMode)
			return (unsigned int*)glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, size * sizeof(unsigned int), GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
		else
			return (unsigned int*)glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
	}
	else
	{
		return nullptr;
	}
}

//===========================================================================
//
//
//
//===========================================================================

void FModelVertexBuffer::UnlockIndexBuffer()
{
	if (ibo_id > 0)
	{
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_id);
		glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
	}
}


//===========================================================================
//
// Sets up the buffer starts for frame interpolation
// This must be called after gl_RenderState.Apply!
//
//===========================================================================
static TArray<FModelVertex> iBuffer;
TArray<FVector3> legacyNormalsBuffer;
void FModelVertexBuffer::SetupFrame(FModelRenderer *renderer, unsigned int frame1, unsigned int frame2, unsigned int size)
{
	glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
	if (vbo_id > 0)
	{
		if (!gl.legacyMode)
		{
			glVertexAttribPointer(VATTR_VERTEX, 3, GL_FLOAT, false, sizeof(FModelVertex), &VMO[frame1].x);
			glVertexAttribPointer(VATTR_TEXCOORD, 2, GL_FLOAT, false, sizeof(FModelVertex), &VMO[frame1].u);
			glVertexAttribPointer(VATTR_VERTEX2, 3, GL_FLOAT, false, sizeof(FModelVertex), &VMO[frame2].x);
			glVertexAttribPointer(VATTR_NORMAL, 4, GL_INT_2_10_10_10_REV, true, sizeof(FModelVertex), &VMO[frame2].packedNormal);
		}
		else
		{
			glVertexPointer(3, GL_FLOAT, sizeof(FModelVertex), &VMO[frame1].x);
			glTexCoordPointer(2, GL_FLOAT, sizeof(FModelVertex), &VMO[frame1].u);
			glDisableClientState(GL_NORMAL_ARRAY);
		}
	}
	else if (frame1 == frame2 || size == 0 || gl_RenderState.GetInterpolationFactor() == 0.f)
	{
		glVertexPointer(3, GL_FLOAT, sizeof(FModelVertex), &vbo_ptr[frame1].x);
		glTexCoordPointer(2, GL_FLOAT, sizeof(FModelVertex), &vbo_ptr[frame1].u);
		if (gl.legacyMode)
		{
			unsigned int activeVerticesCount = (size > 0) ? size : 2048;
			legacyNormalsBuffer.Resize(activeVerticesCount);
			const float *matrixBuffer = gl_RenderState.mModelMatrix.get();
			float modelX = matrixBuffer[12]; float modelY = matrixBuffer[13]; float modelZ = matrixBuffer[14];

			for (unsigned int i = 0; i < activeVerticesCount; i++)
			{
				uint32_t pn = vbo_ptr[frame1 + i].packedNormal;
				if (pn != 0)
				{
					int tx = int(pn << 22) >> 22; int ty = int(pn << 12) >> 22; int tz = int(pn << 2) >> 22;
					legacyNormalsBuffer[i].X = float(tx) / 511.0f;
					legacyNormalsBuffer[i].Y = float(tz) / 511.0f;
					legacyNormalsBuffer[i].Z = float(ty) / 511.0f;
				}
				else
				{
					float vx = vbo_ptr[frame1 + i].x; float vy = vbo_ptr[frame1 + i].y; float vz = vbo_ptr[frame1 + i].z;
					float length = sqrtf((vx * vx) + (vy * vy) + (vz * vz));
					if (length > 0.001f) { legacyNormalsBuffer[i].X = vx / length; legacyNormalsBuffer[i].Y = vy / length; legacyNormalsBuffer[i].Z = vz / length; }
				}
			}
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_FLOAT, sizeof(FVector3), &legacyNormalsBuffer[0].X);
		}
	}
	else
	{
		// must interpolate
		iBuffer.Resize(size);
		glVertexPointer(3, GL_FLOAT, sizeof(FModelVertex), &iBuffer[0].x);
		glTexCoordPointer(2, GL_FLOAT, sizeof(FModelVertex), &vbo_ptr[frame1].u);
		float frac = gl_RenderState.GetInterpolationFactor();
		for (unsigned i = 0; i < size; i++)
		{
			iBuffer[i].x = vbo_ptr[frame1 + i].x * (1.f - frac) + vbo_ptr[frame2 + i].x * frac;
			iBuffer[i].y = vbo_ptr[frame1 + i].y * (1.f - frac) + vbo_ptr[frame2 + i].y * frac;
			iBuffer[i].z = vbo_ptr[frame1 + i].z * (1.f - frac) + vbo_ptr[frame2 + i].z * frac;
		}
		if (gl.legacyMode)
		{
			legacyNormalsBuffer.Resize(size);
			const float *matrixBuffer = gl_RenderState.mModelMatrix.get();
			float modelX = matrixBuffer[12]; float modelY = matrixBuffer[13]; float modelZ = matrixBuffer[14];
			for (unsigned i = 0; i < size; i++)
			{
				uint32_t pn = vbo_ptr[frame1 + i].packedNormal;
				if (pn != 0)
				{
					int tx = int(pn << 22) >> 22; int ty = int(pn << 12) >> 22; int tz = int(pn << 2) >> 22;
					legacyNormalsBuffer[i].X = float(tx) / 511.0f; legacyNormalsBuffer[i].Y = float(tz) / 511.0f; legacyNormalsBuffer[i].Z = float(ty) / 511.0f;
				}
				else
				{
					float vx = iBuffer[i].x; float vy = iBuffer[i].y; float vz = iBuffer[i].z;
					float length = sqrtf((vx * vx) + (vy * vy) + (vz * vz));
					if (length > 0.001f) { legacyNormalsBuffer[i].X = vx / length; legacyNormalsBuffer[i].Y = vy / length; legacyNormalsBuffer[i].Z = vz / length; }
				}
			}
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_FLOAT, sizeof(FVector3), &legacyNormalsBuffer[0].X);
		}
	}
}

//===========================================================================
//
// gl_RenderModel
//
//===========================================================================

void gl_RenderModel(GLSprite * spr)
{
	FGLModelRenderer renderer;
	renderer.RenderModel(spr->x, spr->y, spr->z, spr->modelframe, spr->actor);
}

//===========================================================================
//
// gl_RenderHUDModel
//
//===========================================================================

void gl_RenderHUDModel(DPSprite *psp, float ofsX, float ofsY)
{
	FGLModelRenderer renderer;
	renderer.RenderHUDModel(psp, ofsX, ofsY);
}
