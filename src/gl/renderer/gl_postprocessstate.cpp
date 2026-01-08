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
	glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
	glActiveTexture(GL_TEXTURE0);
	SaveTextureBindings(1);

	glGetBooleanv(GL_BLEND, &blendEnabled);
	glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);
	glGetBooleanv(GL_DEPTH_TEST, &depthEnabled);
	glGetBooleanv(GL_MULTISAMPLE, &multisampleEnabled);
	glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
	glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_RGB, &blendDestRgb);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDestAlpha);

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

	// GL1.x: Fallback to basic blending
	if (gl.gl1path)
	{
		int srcFactor = GL_SRC_ALPHA;
		int destFactor = GL_ONE_MINUS_SRC_ALPHA;
		bool found = false;

		// *** CRITICAL FIX: Check for common combinations ***
		if (blendSrcRgb == GL_SRC_ALPHA && blendDestRgb == GL_ONE_MINUS_SRC_ALPHA)
		{
			// Standard Translucency: Already set as default
			found = true;
		}
		else if (blendSrcRgb == GL_SRC_ALPHA && blendDestRgb == GL_ONE)
		{
			// Additive: STYLE_Add
			srcFactor = GL_SRC_ALPHA;
			destFactor = GL_ONE;
			found = true;
		}
		// Add more checks here if you know the exact blend modes causing texture issues!

		// If found, apply the simple glBlendFunc
		if (found)
		{
			glBlendFunc(srcFactor, destFactor);
		}
		else
		{
			// If the requested modern blend mode (e.g., GL_DST_COLOR) isn't supported 
			// by glBlendFunc, fall back to standard alpha blending.
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
	}
	else
	{
		// Modern GL: Use full blending
		glBlendEquationSeparate(blendEquationRgb, blendEquationAlpha);
		glBlendFuncSeparate(blendSrcRgb, blendDestRgb, blendSrcAlpha, blendDestAlpha);
	}

	// **GL1.x: Restore textures but skip shaders/samplers**
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
	else
	{
		// **GL1.x: Only bind the first texture (if any)**
		if (textureBinding.Size() > 0 && textureBinding[0] != 0)
		{
			glBindTexture(GL_TEXTURE_2D, textureBinding[0]);
		}

		// **CRITICAL: Restore texture unit 0**
		// Some sprites may assume unit 0 is active
		if (gl.gl1path)
		{
			glActiveTexture(GL_TEXTURE0);
		}
	}
}