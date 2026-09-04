/*
**  Postprocessing framework
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
**  gl_postprocessstate.cpp
**  Render state maintenance
**
**/

#include "templates.h"
#include "gl/system/gl_system.h"
#include "gl/system/gl_interface.h"
#include "gl/data/gl_data.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/system/gl_cvars.h"
#include "gl/shaders/gl_shader.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_postprocessstate.h"

//-----------------------------------------------------------------------------
//
// Saves state modified by post processing shaders
//
//-----------------------------------------------------------------------------

FGLPostProcessState::FGLPostProcessState()
{
	if (!gl.gl1path) // GL2+ path
	{
		glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
		glActiveTexture(GL_TEXTURE0);
		SaveTextureBindings(1);
	}
	else // GL1x path
	{
		// Force unbind any multi-texturing footprint and lock unit 0
		activeTex = GL_TEXTURE0;

		// Secure legacy texture state tracking
		GLint texture;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		textureBinding.Push(texture);
	}

	glGetBooleanv(GL_BLEND, &blendEnabled);
	glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);
	glGetBooleanv(GL_DEPTH_TEST, &depthEnabled);
	glGetBooleanv(GL_MULTISAMPLE, &multisampleEnabled);

	// Do NOT call modern context creation states in GL1x mode.
	// Prevent 'blendSrcAlpha' and 'blendEquation' from filling arrays with garbage
	if (!gl.gl1path)
	{
		// GL2x+ constructor
		glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
		glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_RGB, &blendDestRgb);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDestAlpha);
	}
	else
	{
		// GL1x constructor fallback
		currentProgram = 0;
		blendEquationRgb = 0;
		blendEquationAlpha = 0;

		// Query strictly the single-channel factors supported natively by hardware
		glGetIntegerv(GL_BLEND_SRC, &blendSrcRgb);
		glGetIntegerv(GL_BLEND_DST, &blendDestRgb);

		blendSrcAlpha = blendSrcRgb;
		blendDestAlpha = blendDestRgb;
	}

	glDisable(GL_MULTISAMPLE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
}

void FGLPostProcessState::SaveTextureBindings(unsigned int numUnits)
{
	while (textureBinding.Size() < numUnits)
	{
		unsigned int i = textureBinding.Size();

		GLint texture;
		glActiveTexture(GL_TEXTURE0 + i);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
		glBindTexture(GL_TEXTURE_2D, 0);
		textureBinding.Push(texture);

		if (gl.flags & RFL_SAMPLER_OBJECTS)
		{
			GLint sampler;
			glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
			glBindSampler(i, 0);
			samplerBinding.Push(sampler);
		}
	}
	glActiveTexture(GL_TEXTURE0);
}

//-----------------------------------------------------------------------------
//
// GL1 handling
//
//-----------------------------------------------------------------------------

// Maps blend modes to a single (Src, Dest) pair required by glBlendFunc (GL1.x)
struct GL1BlendFuncEntry
{
	int blendSrcRgb;
	int blendDestRgb;
	int mapSrcRgb;    // The blendSrcRgb we are looking up
	int mapDestRgb;   // The blendDestRgb we are looking up
};

// Predefined fallbacks for common GL1.x blend modes
static const GL1BlendFuncEntry GL1BlendModeMap[] =
{
	// Standard Alpha Blending (Used for STYLE_Translucent, STYLE_Normal)
	{ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA },

	// Additive Blending (Used for STYLE_Add)
	{ GL_SRC_ALPHA, GL_ONE, GL_SRC_ALPHA, GL_ONE },

	// Reverse Subtract (Used for STYLE_SoulTrans, STYLE_Subtract)
	{ GL_SRC_ALPHA, GL_ONE, GL_SRC_ALPHA, GL_ONE }, // Often maps to Additive in old HW if RevSub isn't supported

	// Specific mappings based on what *usually* works in 1.5
	// Note: We are mapping the stored blendSrcRgb/blendDestRgb values 
	// (which are GL constants) to the GL1.x function calls.

	// Placeholder for complex blending (e.g., Multiply/Inverse) 
	// If the hardware truly doesn't support the requested function, we default.
};

static const int GL1BlendModeCount = 4; // Let's use a small, known list for simplicity initially

//-----------------------------------------------------------------------------
//
// Restores state at the end of post processing
//
//-----------------------------------------------------------------------------

FGLPostProcessState::~FGLPostProcessState()
{
	if (blendEnabled)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);

	if (scissorEnabled)
		glEnable(GL_SCISSOR_TEST);
	else
		glDisable(GL_SCISSOR_TEST);

	if (depthEnabled)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);

	if (multisampleEnabled)
		glEnable(GL_MULTISAMPLE);
	else
		glDisable(GL_MULTISAMPLE);

	// Clean tableless fallback routing
	if (gl.gl1path)
	{
		// Force direct restoral of the factors cached safely inside constructor
		glBlendFunc(blendSrcRgb, blendDestRgb);
	}
	else // Modern path for GL2+ architectures
	{
		glBlendEquationSeparate(blendEquationRgb, blendEquationAlpha);
		glBlendFuncSeparate(blendSrcRgb, blendDestRgb, blendSrcAlpha, blendDestAlpha);
	}

	// **GL1.x: Safe single-texture unit 0 sanitation restoral loop**
	if (!gl.gl1path)
	{
		glUseProgram(currentProgram);

		for (unsigned int i = 0; i < textureBinding.Size(); i++)
		{
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		for (unsigned int i = 0; i < samplerBinding.Size(); i++)
		{
			glBindSampler(i, samplerBinding[i]);
		}

		for (unsigned int i = 0; i < textureBinding.Size(); i++)
		{
			glActiveTexture(GL_TEXTURE0 + i);
			glBindTexture(GL_TEXTURE_2D, textureBinding[i]);
		}

		glActiveTexture(activeTex);
	}
	else // GL1x CLEANUP
	{
		// Force unit 0 active state to ensure sprites buffer coordinates align
		glActiveTexture(GL_TEXTURE0);

		if (textureBinding.Size() > 0 && textureBinding[0] != 0)
		{
			glBindTexture(GL_TEXTURE_2D, textureBinding[0]);
		}

		// Reset states to default modulate settings to keep menus alive
		gl_RenderState.SetTextureMode(TM_MODULATE);
		gl_RenderState.SetObjectColor(0xffffffff);
		gl_RenderState.Apply();
	}
}
