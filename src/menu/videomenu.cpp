/*
** videomenu.cpp
** The video modes menu
**
**---------------------------------------------------------------------------
** Copyright 2001-2010 Randy Heit
** Copyright 2010 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <float.h>

#include "menu/menu.h"
#include "c_dispatch.h"
#include "w_wad.h"
#include "sc_man.h"
#include "v_font.h"
#include "g_level.h"
#include "d_player.h"
#include "v_video.h"
#include "gi.h"
#include "i_system.h"
#include "c_bind.h"
#include "v_palette.h"
#include "d_event.h"
#include "d_gui.h"
#include "i_music.h"
#include "m_joy.h"
#include "sbar.h"
#include "hardware.h"
#include "vm.h"
#include "r_videoscale.h"
#include "i_time.h"

#include "win32glvid.h"

/*=======================================
 *
 * Video Modes Menu
 *
 *=======================================*/
static void BuildModesList (int hiwidth, int hiheight, int hi_id);
static bool GetSelectedSize (int *width, int *height);
static void SetModesMenu (int w, int h, int bits);
DOptionMenuDescriptor *GetVideoModeMenu();

extern bool setmodeneeded;
extern int NewWidth, NewHeight, NewBits;
extern int DisplayBits;

EXTERN_CVAR (Int, vid_defwidth)
EXTERN_CVAR (Int, vid_defheight)
EXTERN_CVAR (Int, vid_defbits)
EXTERN_CVAR (Bool, fullscreen)

int testingmode;		// Holds time to revert to old mode
int OldWidth, OldHeight, OldBits;
static FIntCVar DummyDepthCvar (NULL, 0, 0);
static uint8_t BitTranslate[32];

CUSTOM_CVAR (Int, menu_screenratios, -1, CVAR_ARCHIVE)
{
	if (self < -1 || self > 6)
	{
		self = -1;
	}
	else
	{
		BuildModesList (screen->VideoWidth, screen->VideoHeight, DisplayBits);
	}
}


//=============================================================================
//
//
//
//=============================================================================

struct OptionMenuItemScreenResolution	// temporary workaround
{
	enum EValues
	{
		SRL_INDEX = 0x30000,
		SRL_SELECTION = 0x30003,
		SRL_HIGHLIGHT = 0x30004,
	};
};

//=============================================================================
//
//
//
//=============================================================================

DOptionMenuDescriptor *GetVideoModeMenu()
{
	DMenuDescriptor **desc = MenuDescriptors.CheckKey(NAME_VideoModeMenu);
	if (desc != NULL && (*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor)))
	{
		return (DOptionMenuDescriptor *)*desc;
	}
	return NULL;
}

//=============================================================================
//
//		Set some stuff up for the video modes menu
//
//=============================================================================

static void BuildModesList(int hiwidth, int hiheight, int hi_bits)
{
	char strtemp[32];
	int	 i, c;
	int	 width, height, showbits;
	bool letterbox = false;
	int  ratiomatch;

	if (menu_screenratios >= 0 && menu_screenratios <= 6)
	{
		ratiomatch = menu_screenratios;
	}
	else
	{
		ratiomatch = -1;
	}
	showbits = BitTranslate[DummyDepthCvar];

	// If screen is NULL during mid-game resolution switches, 
	// calling screen->IsFullscreen() triggers a fatal Access Violation crash (0xC0000005).
	// Replace it with a safe ternary operator checking against the standalone
	// global 'fullscreen' CVAR or checking the state-machine fallback template
	if (Video != NULL)
	{
		// Safe pointer check fork: if screen is null, fall back to the native global 'fullscreen' token
		bool isCurrentlyFullscreen = (screen != nullptr) ? screen->IsFullscreen() : (bool)fullscreen;

		Video->StartModeIterator(showbits, isCurrentlyFullscreen); // Shouldn't crash again
	}

	DOptionMenuDescriptor *opt = GetVideoModeMenu();
	if (opt != NULL)
	{
		for (i = NAME_res_0; i <= NAME_res_9; i++)
		{
			DMenuItemBase *it = opt->GetItem((ENamedName)i);
			if (it != NULL)
			{
				it->SetValue(OptionMenuItemScreenResolution::SRL_HIGHLIGHT, -1);
				for (c = 0; c < 3; c++)
				{
					bool haveMode = false;

					if (Video != NULL)
					{
						while ((haveMode = Video->NextMode(&width, &height, &letterbox)) &&
							ratiomatch >= 0)
						{
							int ratio;
							CheckRatio(width, height, &ratio);
							if (ratio == ratiomatch)
								break;
						}
					}

					if (haveMode)
					{
						if (width == hiwidth && height == hiheight)
						{
							it->SetValue(OptionMenuItemScreenResolution::SRL_SELECTION, c);
							it->SetValue(OptionMenuItemScreenResolution::SRL_HIGHLIGHT, c);
						}

						mysnprintf(strtemp, countof(strtemp), "%dx%d%s", width, height, letterbox ? TEXTCOLOR_BROWN" LB" : "");
						it->SetString(OptionMenuItemScreenResolution::SRL_INDEX + c, strtemp);
					}
					else
					{
						it->SetString(OptionMenuItemScreenResolution::SRL_INDEX + c, "");
					}
				}
			}
		}
	}
}


//=============================================================================
//
//
//
//=============================================================================

void M_RestoreMode()
{
	// We are going to restore the old resolution immediately
	NewWidth = OldWidth;
	NewHeight = OldHeight;
	NewBits = OldBits;

	// Recreate the context RIGHT NOW, don't wait for the next frame.
	// If screen is null, we've lost the context. Restore using the old values.
	if (!screen)
	{
		Printf("R_OPENGL: Screen is null. Recreating OpenGL context with old resolution.\n");
		static_cast<Win32GLVideo*>(Video)->RecreateOpenGLContext(OldWidth, OldHeight, OldBits);
	}

	// Now that the context is (hopefully) restored, update the menu
	setmodeneeded = true; // This will be handled by D_ProcessEvents next frame, but the critical work is done.
	testingmode = 0;
	SetModesMenu(OldWidth, OldHeight, OldBits);
}

void M_SetDefaultMode()
{
	// We are going to apply the new resolution immediately
	NewWidth = screen ? screen->VideoWidth : vid_defwidth; // Fallback
	NewHeight = screen ? screen->VideoHeight : vid_defheight;
	NewBits = DisplayBits;

	// Recreate the context RIGHT NOW.
	// If screen is null, we've lost the context. Use the intended resolution.
	if (!screen)
	{
		Printf("R_OPENGL: Screen is null. Recreating OpenGL context with last known resolution.\n");
		static_cast<Win32GLVideo*>(Video)->RecreateOpenGLContext(NewWidth, NewHeight, NewBits);
	}

	// Now update the defaults and the menu
	vid_defwidth = NewWidth;
	vid_defheight = NewHeight;
	vid_defbits = NewBits;
	testingmode = 0;
	SetModesMenu(NewWidth, NewHeight, DisplayBits);
}



//=============================================================================
//
//
//
//=============================================================================

void M_RefreshModesList ()
{
	BuildModesList (screen->VideoWidth, screen->VideoHeight, DisplayBits);
}

void M_InitVideoModesMenu ()
{
	int dummy1, dummy2;
	size_t currval = 0;

	M_RefreshModesList();

	for (unsigned int i = 1; i <= 32 && currval < countof(BitTranslate); i++)
	{
		Video->StartModeIterator (i, screen->IsFullscreen());
		if (Video->NextMode (&dummy1, &dummy2, NULL))
		{
			BitTranslate[currval++] = i;
		}
	}

	/* It doesn't look like this can be anything but DISPLAY_Both, regardless of any other settings.
	switch (Video->GetDisplayType ())
	{
	case DISPLAY_FullscreenOnly:
	case DISPLAY_WindowOnly:
		// todo: gray out fullscreen option
	default:
		break;
	}
	*/
}

//=============================================================================
//
//
//
//=============================================================================

static bool GetSelectedSize (int *width, int *height)
{
	DOptionMenuDescriptor *opt = GetVideoModeMenu();
	if (opt != NULL && (unsigned)opt->mSelectedItem < opt->mItems.Size())
	{
		int line = opt->mSelectedItem;
		int hsel;
		DMenuItemBase *it = opt->mItems[line];
		if (it->GetValue(OptionMenuItemScreenResolution::SRL_SELECTION, &hsel))
		{
			char buffer[32];
			char *breakpt;
			if (it->GetString(OptionMenuItemScreenResolution::SRL_INDEX+hsel, buffer, sizeof(buffer)))
			{
				*width = (int)strtoll (buffer, &breakpt, 10);
				*height = (int)strtoll (breakpt+1, NULL, 10);
				return true;
			}
		}
	}
	return false;
}

DEFINE_ACTION_FUNCTION(DVideoModeMenu, SetSelectedSize)
{
	if (!GetSelectedSize (&NewWidth, &NewHeight))
	{
		NewWidth = screen->VideoWidth;
		NewHeight = screen->VideoHeight;
		ACTION_RETURN_BOOL(false);
	}
	else
	{
		OldWidth = screen->VideoWidth;
		OldHeight = screen->VideoHeight;
		OldBits = DisplayBits;
		NewBits = BitTranslate[DummyDepthCvar];
		setmodeneeded = true;
		testingmode = I_GetTime() + 5 * TICRATE;
		SetModesMenu (NewWidth, NewHeight, NewBits);
		ACTION_RETURN_BOOL(true);
	}
}	

//=============================================================================
//
//
//
//=============================================================================

void M_SetVideoMode()
{
	if (!GetSelectedSize (&NewWidth, &NewHeight))
	{
		NewWidth = screen->VideoWidth;
		NewHeight = screen->VideoHeight;
	}
	else
	{
		testingmode = 1;
		setmodeneeded = true;
		NewBits = BitTranslate[DummyDepthCvar];
	}
	SetModesMenu (NewWidth, NewHeight, NewBits);
}

DEFINE_ACTION_FUNCTION(DMenu, SetVideoMode)
{
	M_SetVideoMode();
	return 0;
}
//=============================================================================
//
//
//
//=============================================================================

static int FindBits (int bits)
{
	int i;

	for (i = 0; i < 22; i++)
	{
		if (BitTranslate[i] == bits)
			return i;
	}

	return 0;
}

static void SetModesMenu (int w, int h, int bits)
{
	DummyDepthCvar = FindBits (bits);

	DOptionMenuDescriptor *opt = GetVideoModeMenu();
	if (opt != NULL)
	{
		DMenuItemBase *it;
		if (testingmode <= 1)
		{
			it = opt->GetItem(NAME_VMEnterText);
			if (it != NULL) it->SetValue(0, 0);
			it = opt->GetItem(NAME_VMTestText);
			if (it != NULL) it->SetValue(0, 0);
		}
		else
		{

			it = opt->GetItem(NAME_VMTestText);
			if (it != NULL) it->SetValue(0, 1);
			it = opt->GetItem(NAME_VMEnterText);
			if (it != NULL) 
			{
				char strtemp[64];
				mysnprintf (strtemp, countof(strtemp), "TESTING %dx%dx%d", w, h, bits);
				it->SetValue(0, 1);
				it->SetString(0, strtemp);
			}
		}
	}
	BuildModesList (w, h, bits);
}

//void M_InitVideoModes()
//{
//	SetModesMenu (screen->VideoWidth, screen->VideoHeight, DisplayBits);
//}

void M_InitVideoModes()
{
	// If screen is NULL during mid-game resolution switches, 
	// accessing screen->VideoWidth triggers a fatal Access Violation crash (0xC0000005).
	// Intercept it with a safe fallback parsing stable global DisplayWidth / DisplayHeight.
	int safeWidth = 640;   // Safe fallback setup baseline width
	int safeHeight = 400;  // Safe fallback setup baseline height

	if (screen != nullptr) // If the primary framebuffer object pointer is alive and active in RAM...
	{
		safeWidth = screen->VideoWidth;   // Use vanilla property
		safeHeight = screen->VideoHeight; // Use vanilla property
	}
	else // If screen is currently NULL during live OpenGL context recreation...
	{
		// Extract the physical window sizes directly from the un-corrupted global display registry
		safeWidth = (DisplayWidth > 0) ? DisplayWidth : vid_defwidth;
		safeHeight = (DisplayHeight > 0) ? DisplayHeight : vid_defheight;
	}

	// Force execute SetModesMenu using verified hardware layout parameters
	SetModesMenu(safeWidth, safeHeight, DisplayBits);
}


