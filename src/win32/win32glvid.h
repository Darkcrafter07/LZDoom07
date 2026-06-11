#ifndef WIN32GLVID_H
#define WIN32GLVID_H

#pragma once

#include "win32iface.h"
#include "optwin32.h"

extern HWND Window;
EXTERN_CVAR(Bool, vid_hdr)

//==========================================================================
//
// 
//
//==========================================================================

class Win32GLVideo : public IVideo
{
public:
	Win32GLVideo(int parm);
	virtual ~Win32GLVideo();

	EDisplayType GetDisplayType() { return DISPLAY_Both; }
	void SetWindowedScale(float scale);
	void StartModeIterator(int bits, bool fs);
	bool NextMode(int *width, int *height, bool *letterbox);
	bool GoFullscreen(bool yes);
	DFrameBuffer *CreateFrameBuffer (int width, int height, bool bgra, bool fs, DFrameBuffer *old);
	void RecreateOpenGLContext(int width, int height, int bits);
	virtual bool SetResolution(int width, int height, int bits);
	void DumpAdapters();
	bool InitHardware(HWND Window, int multisample);
	void Shutdown();
	bool SetFullscreen(const char *devicename, int w, int h, int bits, int hz);

	HDC m_hDC;

protected:
	struct ModeInfo
	{
		ModeInfo(int inX, int inY, int inBits, int inRealY, int inRefresh)
			: next(NULL),
			width(inX),
			height(inY),
			bits(inBits),
			refreshHz(inRefresh),
			realheight(inRealY)
		{}
		ModeInfo *next;
		int width, height, bits, refreshHz, realheight;
	} *m_Modes;

	ModeInfo *m_IteratorMode;
	int m_IteratorBits;
	bool m_IteratorFS;
	bool m_IsFullscreen;
	int m_trueHeight;
	int m_DisplayWidth, m_DisplayHeight, m_DisplayBits, m_DisplayHz;
	HMODULE hmRender;

	char m_DisplayDeviceBuffer[CCHDEVICENAME];
	char *m_DisplayDeviceName;
	HMONITOR m_hMonitor;

	HWND m_Window;
	HGLRC m_hRC;

	HWND InitDummy();
	void ShutdownDummy(HWND dummy);
	bool SetPixelFormat();
	bool SetupPixelFormat(int multisample);

	void GetDisplayDeviceName();
	void MakeModesList();
	void AddMode(int x, int y, int bits, int baseHeight, int refreshHz);
	void FreeModes();
public:
	int GetTrueHeight() { return m_trueHeight; }

};



#endif // WIN32GLVID_H