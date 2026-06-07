# Welcome to LZDoom07!

## LZDoom07 is a fork of drfrag's LZDoom v3.88b for WinXP and old sys
LZDoom07 just as LZDoom is based on GZDoom 3.3 which keeps the old DDRAW and D3D backends
and still supports GL1 and GL2 for compatibility. Also runs on older non SSE2 cpus.
LZDoom07 aims to provide even more optimization and new features to LZDoom
while keeping compatibility with Windows XP and old systems.

What it runs on:
1) Really old Fixed-Pipeline cards:
  a) OpenGL 1x cards (tested on nvidia FX5500 128mb);
     Make sure to set resolution BEFORE you run the game as
     it's going to crash. Ingame vsync/texture options are ok;
  b) OpenGL 2x cards (tested on ATI Radeon HD 2400 PRO 256mb);
2) Newer shader based cards:
  c) OpenGL 3x cards (tested on nvidia GTS 450 512mb);
  d) OpenGL 4x cards (tested on nvidia GT 730 2gb) and higher;

GL1 mode is useful if your video card is quite recent and old OS drivers are bad.
Run LZDoom07 with "-glversion 1" command then to force it. If it runs ok, exit
and try again with "-glversion 2" command, does it work? Try "-glversion 3" and
if it doesn't then that's either your faulty video card or drivers.

For sure GL1/GL2x modes are going to look slightly worse but there are some
workarounds: enable hires texture normal2x for GL1 to reduce blurriness and
enable dynlights and then enable feature "spawn camera glow dynlight", switch
to "dark" sector light mode and get vibes of "Software" light mode.
Then it's the only port that has a really well polished "multipass" system
and doesn't require shaders to support dynamic lights and brightmaps.
You are still able to run modern shaders if your video card supports
at least GL3.3 and drivers are good for that.

Copyright (c) 2020-2023 drfrag (original LZDoom v3.88b)
(https://github.com/drfrag666/lzdoom/)

Copyright (c) 1998-2021 ZDoom + GZDoom teams, and contributors
Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses

### Licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html
---

## How to build LZDoom07

To build LZDoom07, please see the [wiki](https://zdoom.org/wiki/)
and see the "Programmer's Corner" on the bottom-right cornerof the page
to build for your platform.

by Darkcrafter07 / Vadim Taranov, (c) 2025
