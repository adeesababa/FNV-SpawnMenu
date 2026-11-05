#pragma once
#pragma warning (disable: 4244)	// int / float / double narrowing in the shared game headers

// The plugin's forced include (every file but ImGui's): Windows, D3D9, the game's class layouts and the few game globals the
// spawn files use. The layouts are Vegas Shaders' headers, read at build time only: this DLL does not need Vegas Shaders installed.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <string>
#include <vector>
#include <stack>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <d3d9.h>
#include <d3dx9.h>
#include <dinput.h>
#include "../game/base/Types.h"
#include "../game/NewVegas/Plugin.h"
#include "../game/NewVegas/nvse/DIHookControl.h"
#include "../game/NewVegas/nvse/GameNi.h"
#include "../game/NewVegas/nvse/Game.h"

namespace Logger {
	void Log(const char* format, ...);		// one line in SpawnMenu.log (game folder)
}

namespace SpawnGame {
	// Game globals by address (WallSoGB's map: TESDataHandler::pSingleton, ModelLoader::pSingleton, NiRenderer::ms_pkRenderer,
	// InterfaceManager::pSingleton, PlayerCharacter singleton).
	inline MainDataHandler*	DataHandler()		{ return *(MainDataHandler**)0x011C3F2C; }
	inline void*			ModelLoader()		{ return *(void**)0x011C3B3C; }
	inline UInt8*			RendererBase()		{ return *(UInt8**)0x011F4748; }
	inline IDirect3DDevice9* Device()			{ UInt8* r = RendererBase(); return r ? *(IDirect3DDevice9**)(r + 0x288) : nullptr; }
	inline HWND				Window()			{ UInt8* r = RendererBase(); return r ? *(HWND*)(r + 0x3C0) : nullptr; }
	inline const UInt32*	MenuStack()			{ UInt8* m = *(UInt8**)0x011D8A80; return m ? (const UInt32*)(m + 0x114) : nullptr; }
	inline void*			Player()			{ return *(void**)0x011DEA3C; }

	std::string	GameRoot();							// the game's folder (no trailing backslash)
	void		ConsoleLater(const char* line);		// runs on the next game loop, outside any render call
}
