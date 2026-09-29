# Open77 VR HUD

RED4ext plugin that draws the Open77 WebUI into the headset while playing Cyberpunk 2077 with R.E.A.L. VR, Virtual Desktop (wireless, cabled mode not tested) or Oculus/Meta Link.

License: [MIT](LICENSE). Use, copy, and modify it freely.

## What it does

Open77 composites its CEF pages onto the monitor after R.E.A.L. VR has already submitted the stereo images to the headset. This plugin hooks `ovr_EndFrame` (the Virtual Desktop LibOVR build, or the Meta Horizon one), reopens Open77's shared page textures on the D3D11 device R.E.A.L. uses, and submits them as a quad layer that follows the R.E.A.L. HUD board.

Each Open77 surface is a ring of three textures. Only the most recently written slot is drawn. The connection screen and the freeroam menu are hidden when the Open77 log says they closed, because those pages do not clear the GPU texture.

## Requirements

- Windows 10 or 11, 64-bit
- Cyberpunk 2077 2.31 + Phantom Liberty DLC
- RED4ext 1.30.0
- Latest Open//77 Client
- R.E.A.L. VR 26.3.0 
- Virtual Desktop, with the OpenXR runtime set to VDXR rather than SteamVR
- An NVIDIA GPU. Tested on an RTX 4070 Ti (12 GB) at `ForcedRes=1232,1344` and about 10 pixels per degree. Higher VR resolutions (over 20) run out of VRAM on 12 GB.
- 32 GB of RAM or a page file managed by Windows of at least 16GB (not tested).


## Install

With the game closed, copy the built `Open77VrHud.dll` and `Open77VrHud.ini` to:

```
red4ext/plugins/Open77VrHud/
```

On Open//77 Launcher go to  `Mods & profiles`, scroll down until you see `Open77VrHud` (the folder you added), activate it and press `Apply changes`.

There may or not be the file `dxgi.dll`, activate it too and `Apply changes`.

Connect the headset in Virtual Desktop or Oculus/Meta Link before launching the game.

Make sure to set `VDXR` as the default OpenXR Runtime in your Virtual Desktop Streamer.

Join a server from the Open77 launcher.

## Build

[Zig](https://ziglang.org/) 0.14.1:

```
zig cc -shared -O2 -target x86_64-windows-gnu -o Open77VrHud.dll Open77VrHud.c -ld3d12 -ld3d11 -ldxgi -lole32 -luuid -luser32 -lpsapi
```

Or run `build.ps1` on Windows.

## Log

`red4ext/plugins/Open77VrHud/Open77VrHud.log`

A working session logs `endframe prologue hooked` or `endframe tail hooked`, then `hud chain ready` and `HUD layer live in headset`.

## Version

The source in this tree is 4.6.0. On top of 4.5.0 it draws the Windows mouse cursor over the pause, admin, wardrobe, and context-menu pages, which do not include a cursor in the captured frame.
