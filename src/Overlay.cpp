#include "SpawnMenu.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <share.h>

// The plugin itself: the xNVSE entry points, a hook on the device's Present (and Reset) that draws the spawn window over the
// finished frame, its own ImGui, the L key, and the game's input switched off while the window is open.
//
// Input follows the same rules as Vegas Shaders' menu (its CLAUDE.md, DXVK): keys, buttons and the cursor are polled with
// GetAsyncKeyState / GetCursorPos every frame; the wheel comes from the DirectInput mouse buffer. No window messages are used.
// Closed, the Present hook only polls the L key.

namespace {

const UInt32 kPluginVersion = 2;

// The menu key (Options > Change menu key; L by default) and its DirectInput code, which another overlay that holds the
// keyboard has disabled. VK -> scan code with MAPVK_VK_TO_VSC_EX gives the E0 prefix of extended keys (Home, arrows...),
// which DirectInput writes as the 0x80 bit.
int ToggleKey() { return SpawnMenu::Opt.menuKey; }
UInt32 ToggleDik() {
	const UINT scan = MapVirtualKeyA((UINT)ToggleKey(), MAPVK_VK_TO_VSC_EX);
	return (scan & 0x7F) | ((scan & 0xFF00) ? 0x80 : 0);
}

NVSEConsoleInterface*	gConsole = nullptr;
DIHookControl*			gDIHook = nullptr;
FILE*					gLog = nullptr;

// ---- log, folders, deferred console lines ------------------------------------------------------------------------------------

std::string ExeFolder() {
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string folder = path;
	const size_t slash = folder.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : folder.substr(0, slash);
}

std::deque<std::string> gConsoleLines;	// filled in Present, run by the next game loop (both on the main thread)

void RunConsoleLines() {
	while (!gConsoleLines.empty()) {
		const std::string line = gConsoleLines.front();
		gConsoleLines.pop_front();
		if (gConsole) gConsole->RunScriptLine(line.c_str(), nullptr);
	}
}

// ---- game input while the window is open -------------------------------------------------------------------------------------
// Mouse movement: DIHookControl cannot block it, so the mouse's IDirectInputDevice8::GetDeviceState (vtable slot 9) is wrapped
// and its DIMOUSESTATE2 cleared while the window is open; the wheel is read from it first. Keys and buttons: DIHookControl.

typedef HRESULT(WINAPI* GetDeviceStateFn)(IDirectInputDevice8*, DWORD, LPVOID);
GetDeviceStateFn RealGetDeviceState = nullptr;
float gWheel = 0.0f;

HRESULT WINAPI MouseStateHook(IDirectInputDevice8* device, DWORD size, LPVOID data) {
	const HRESULT result = RealGetDeviceState(device, size, data);
	if (SUCCEEDED(result) && size == sizeof(DIMOUSESTATE2) && SpawnMenu::Open) {
		gWheel += ((DIMOUSESTATE2*)data)->lZ / (float)WHEEL_DELTA;
		memset(data, 0, size);
	}
	return result;
}

// Once per session: the slot belongs to the device class, and another plugin (Vegas Shaders, CacheUI) may wrap it after us
// and chain to our hook; wrapping again would save that hook as "the real one" and the two would call each other forever.
void HookMouseOnce() {
	if (RealGetDeviceState) return;
	UInt8* input = *(UInt8**)0x011F35CC;	// OSInputGlobals
	IDirectInputDevice8* mouse = input ? *(IDirectInputDevice8**)(input + 0x30) : nullptr;
	if (!mouse) return;
	void** slots = *(void***)mouse;
	RealGetDeviceState = (GetDeviceStateFn)slots[9];
	DWORD protection;
	VirtualProtect(&slots[9], sizeof(void*), PAGE_READWRITE, &protection);
	slots[9] = (void*)MouseStateHook;
	VirtualProtect(&slots[9], sizeof(void*), protection, &protection);
}

// Shift, Ctrl and Alt stay with the game: xNVSE tracks tap and hold on them, and switching one off and on again breaks that for
// mods bound to it (sprint on Shift).
bool IsModifierCode(UInt32 code) {
	switch (code) {
	case 0x2A: case 0x36: case 0x1D: case 0x9D: case 0x38: case 0xB8: return true;
	}
	return false;
}

// What the game is kept from seeing: nothing; only the left mouse button (the tool gun is out, so a click places instead of
// firing); or everything but the modifiers (the window is open).
enum Block { kBlock_None, kBlock_LeftClick, kBlock_All };
Block gBlock = kBlock_None;

void SetGameInputBlock(Block want) {
	if (!gDIHook) return;
	if (want == gBlock) {
		// Another overlay closing (Vegas Shaders' menu) gives every key back, the left button included: claim it again
		if (want == kBlock_LeftClick) gDIHook->SetKeyDisableState(kMacro_MouseButtonOffset, true, DIHookControl::kDisable_User);
		return;
	}
	for (UInt32 code = 0; code < kMaxMacros; code++) {
		const bool off = want == kBlock_All ? !IsModifierCode(code) : want == kBlock_LeftClick && code == kMacro_MouseButtonOffset;
		gDIHook->SetKeyDisableState(code, off, DIHookControl::kDisable_User);
	}
	gBlock = want;
}

// Another overlay (Vegas Shaders' menu) has the keyboard when it has switched our key off for the game.
bool OtherOverlayHasInput() {
	if (!gDIHook) return false;
	const UInt8* keys = (const UInt8*)gDIHook + 4;	// DIHookControl: vtable, then 7 bytes per key, userDisable at +5
	const UInt32 dik = ToggleDik();
	return dik != 0 && keys[dik * 7 + 5] != 0;
}

// ---- ImGui -------------------------------------------------------------------------------------------------------------------

bool gImGuiReady = false;
bool gWasOpen = false;
BYTE gKeyWas[256] = {};

bool KeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The game is in front: the foreground window belongs to this process (the renderer's focus window may be a child or owner of it).
bool GameInFront() {
	DWORD process = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &process);
	return process == GetCurrentProcessId();
}

void InitImGui(IDirect3DDevice9* device, HWND window) {
	static const std::string layout = ExeFolder() + "\\Data\\SpawnMenu\\SpawnMenu.imgui.ini";	// ImGui keeps the pointer
	CreateDirectoryA((ExeFolder() + "\\Data\\SpawnMenu").c_str(), nullptr);
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = layout.c_str();
	// No ImGuiConfigFlags_NavEnableKeyboard: arrows, Home and End would move through the widgets
	ImGui::StyleColorsDark();
	ImGui_ImplWin32_Init(window);
	ImGui_ImplDX9_Init(device);
	gImGuiReady = true;
	Logger::Log("Spawn menu: ImGui ready (window %p)", window);
}

ImGuiKey ImGuiKeyFor(int vk) {
	if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
	if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
	static const struct { int vk; ImGuiKey key; } kOthers[] = {
		{ VK_TAB, ImGuiKey_Tab }, { VK_BACK, ImGuiKey_Backspace }, { VK_RETURN, ImGuiKey_Enter }, { VK_ESCAPE, ImGuiKey_Escape },
		{ VK_SPACE, ImGuiKey_Space }, { VK_LEFT, ImGuiKey_LeftArrow }, { VK_RIGHT, ImGuiKey_RightArrow }, { VK_UP, ImGuiKey_UpArrow },
		{ VK_DOWN, ImGuiKey_DownArrow }, { VK_HOME, ImGuiKey_Home }, { VK_END, ImGuiKey_End }, { VK_DELETE, ImGuiKey_Delete },
		{ VK_LSHIFT, ImGuiKey_LeftShift }, { VK_RSHIFT, ImGuiKey_RightShift }, { VK_LCONTROL, ImGuiKey_LeftCtrl },
		{ VK_RCONTROL, ImGuiKey_RightCtrl }, { VK_LMENU, ImGuiKey_LeftAlt }, { VK_RMENU, ImGuiKey_RightAlt },
	};
	for (const auto& entry : kOthers)
		if (entry.vk == vk) return entry.key;
	return ImGuiKey_None;
}

// Key presses and releases for ImGui, and the characters they type (with Shift / Ctrl / Alt / Caps Lock).
void FeedKeyboard() {
	ImGuiIO& io = ImGui::GetIO();
	for (int vk = 1; vk < 256; vk++) {
		const bool now = KeyHeld(vk);
		if (now == ((gKeyWas[vk] & 0x80) != 0)) continue;
		gKeyWas[vk] = now ? 0x80 : 0;
		const ImGuiKey key = ImGuiKeyFor(vk);
		if (key != ImGuiKey_None) io.AddKeyEvent(key, now);
		if (!now) continue;
		BYTE keyboard[256] = {};
		keyboard[VK_SHIFT] = KeyHeld(VK_SHIFT) ? 0x80 : 0;
		keyboard[VK_CONTROL] = KeyHeld(VK_CONTROL) ? 0x80 : 0;
		keyboard[VK_MENU] = KeyHeld(VK_MENU) ? 0x80 : 0;
		keyboard[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 0x01 : 0;
		keyboard[vk] = 0x80;
		WCHAR typed[4] = {};
		const int count = ToUnicode(vk, MapVirtualKey(vk, MAPVK_VK_TO_VSC), keyboard, typed, 4, 0);
		for (int i = 0; i < count; i++) io.AddInputCharacterUTF16((ImWchar16)typed[i]);
	}
}

// The cursor, the buttons and the wheel. The cursor is given again after the Win32 backend (the last position queued wins), so
// a click back into the window never leaves it frozen.
void FeedMouse(HWND window) {
	ImGuiIO& io = ImGui::GetIO();
	POINT cursor;
	RECT client;
	if (GetCursorPos(&cursor) && GetClientRect(window, &client)) {
		ScreenToClient(window, &cursor);
		if (cursor.x >= client.left && cursor.x < client.right && cursor.y >= client.top && cursor.y < client.bottom)
			io.AddMousePosEvent((float)cursor.x, (float)cursor.y);
	}
	static const int kButtons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
	for (int i = 0; i < 3; i++) io.AddMouseButtonEvent(i, KeyHeld(kButtons[i]));
	if (gWheel != 0.0f) {
		io.AddMouseWheelEvent(0.0f, gWheel);
		gWheel = 0.0f;
	}
}

// A cursor outside the window (left on another monitor) goes to the window's centre, so the window has a usable mouse at once.
void BringCursorIntoWindow(HWND window) {
	RECT client;
	POINT local;
	if (!GetCursorPos(&local) || !GetClientRect(window, &client)) return;
	ScreenToClient(window, &local);
	if (local.x >= client.left && local.x < client.right && local.y >= client.top && local.y < client.bottom) return;
	POINT centre = { (client.left + client.right) / 2, (client.top + client.bottom) / 2 };
	ClientToScreen(window, &centre);
	SetCursorPos(centre.x, centre.y);
}

void OnOpened(HWND window) {
	for (int vk = 0; vk < 256; vk++) gKeyWas[vk] = KeyHeld(vk) ? 0x80 : 0;	// keys already down are not presses
	HookMouseOnce();
	ClipCursor(nullptr);
	ImGuiIO& io = ImGui::GetIO();
	io.MouseDrawCursor = true;
	io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
	io.ClearInputKeys();
	BringCursorIntoWindow(window);
}

void OnClosed() {
	if (gImGuiReady) {
		ImGuiIO& io = ImGui::GetIO();
		io.MouseDrawCursor = false;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;	// the tool gun's hint frames must not bring the Windows cursor back
	}
	SpawnMenu::RebindingKey = false;	// closed while waiting for a new menu key: keep the old one
	SpawnMenu::DrawWindow();	// closed: it frees the pictures (nothing was drawn with them since the last frame's Present)
}

// Game mode: no game menu (the console included) and no other overlay holding the keyboard.
bool InGameMode() {
	const UInt32* menus = SpawnGame::MenuStack();
	return menus && menus[0] == Menu::kMenuType_None && SpawnGame::Player() && !OtherOverlayHasInput();
}

// Open: everything. Closed: everything until the keys still down from closing are up (else the game sees them pressed), then
// only the left button while the tool gun is out in game mode.
void FollowInputBlock() {
	if (SpawnMenu::Open) {
		SetGameInputBlock(kBlock_All);
		return;
	}
	if (gBlock == kBlock_All && (KeyHeld(ToggleKey()) || KeyHeld(VK_LBUTTON))) return;
	if (gBlock != kBlock_All && OtherOverlayHasInput()) {
		gBlock = kBlock_None;	// another overlay holds every key now (ours included): leave them to it, claim the button after
		return;
	}
	SetGameInputBlock(SpawnMenu::ToolEntry() && InGameMode() ? kBlock_LeftClick : kBlock_None);
}

// The tool gun: a fresh left click in game mode, with the game in front, places the armed entry. The button state is followed
// every frame, so the click that closed the window (Spawn with tool gun) is not a placement.
void PollToolClick() {
	static bool wasDown = false;
	const bool down = KeyHeld(VK_LBUTTON);
	const bool rose = down && !wasDown;
	wasDown = down;
	if (rose && !SpawnMenu::Open && SpawnMenu::ToolEntry() && GameInFront() && InGameMode()) SpawnMenu::ToolClick();
}

// Draws the frame's ImGui output on the back buffer, inside its own scene, with render target 0 and the viewport put back.
void DrawToBackBuffer(IDirect3DDevice9* device) {
	if (FAILED(device->BeginScene())) return;
	IDirect3DSurface9* previous = nullptr;
	IDirect3DSurface9* backBuffer = nullptr;
	device->GetRenderTarget(0, &previous);
	if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer))) {
		D3DVIEWPORT9 viewport;
		device->GetViewport(&viewport);
		device->SetRenderTarget(0, backBuffer);
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		if (previous) device->SetRenderTarget(0, previous);
		device->SetViewport(&viewport);
		backBuffer->Release();
	}
	if (previous) previous->Release();
	device->EndScene();
}

// Options > Change menu key: the next key that goes down becomes the menu key; Esc keeps the old one. Mouse buttons, Shift, Ctrl,
// Alt and the Windows keys are not taken (the click on the menu item, and xNVSE's tap-and-hold on the modifiers).
// Returns true while it is waiting (the menu key does nothing meanwhile).
bool PollRebind() {
	static bool armed = false;
	static BYTE was[256];
	if (!SpawnMenu::RebindingKey) {
		armed = false;
		return false;
	}
	if (!armed) {
		for (int vk = 0; vk < 256; vk++) was[vk] = KeyHeld(vk) ? 1 : 0;	// keys already down are not the new key
		armed = true;
		return true;
	}
	for (int vk = 0x08; vk < 0xFF; vk++) {
		const bool down = KeyHeld(vk);
		const bool rose = down && !was[vk];
		was[vk] = down ? 1 : 0;
		if (!rose) continue;
		if ((vk >= VK_SHIFT && vk <= VK_MENU) || (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN) continue;
		if (vk != VK_ESCAPE && MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC_EX) == 0) continue;	// no scan code: not a real key
		SpawnMenu::RebindingKey = false;
		armed = false;
		if (vk == VK_ESCAPE) {
			Logger::Log("Spawn menu: menu key change cancelled, still %s", SpawnMenu::KeyName(ToggleKey()).c_str());
			return true;
		}
		SpawnMenu::Opt.menuKey = vk;
		SpawnMenu::OptionsSave();
		Logger::Log("Spawn menu: menu key is now %s (VK %02X, DIK %02X)", SpawnMenu::KeyName(vk).c_str(), vk, ToggleDik());
		return true;
	}
	return true;
}

// The menu key (L by default) opens the window in game mode (no game menu, the console included, and no other overlay holding
// the keyboard) and closes it again, unless a text box is typing.
void PollToggleKey(HWND window) {
	static bool wasDown = false;
	if (PollRebind()) {
		wasDown = true;		// the key just bound is held now: that is not a press
		return;
	}
	const bool down = KeyHeld(ToggleKey());
	const bool rose = down && !wasDown;
	wasDown = down;
	if (!rose) return;
	const bool front = GameInFront();
	const std::string key = SpawnMenu::KeyName(ToggleKey());
	if (SpawnMenu::Open) {
		if (!(gImGuiReady && ImGui::GetIO().WantTextInput)) SpawnMenu::Open = false;
		Logger::Log("Spawn menu: %s pressed while open: %s", key.c_str(), SpawnMenu::Open ? "typing, stays open" : "closing");
		return;
	}
	const UInt32* menus = SpawnGame::MenuStack();
	const UInt32 topMenu = menus ? menus[0] : 0xFFFFFFFF;
	const bool other = OtherOverlayHasInput();
	const bool open = front && topMenu == Menu::kMenuType_None && SpawnGame::Player() && !other;
	// Each press says why it did or did not open (the first test of 11-05 opened nothing and left no trace)
	Logger::Log("Spawn menu: %s pressed: %s (game in front %d, top menu %u, player %p, another overlay has the keys %d, window %p, foreground %p)",
		key.c_str(), open ? "opening" : "not opening", front, topMenu, SpawnGame::Player(), other, window, GetForegroundWindow());
	if (open) SpawnMenu::Open = true;
}

// Every Present: the L key; open, one ImGui frame (the window, its picture bakes) drawn on the back buffer.
void OnPresent(IDirect3DDevice9* device) {
	const HWND window = SpawnGame::Window();
	if (!window) return;
	PollToggleKey(window);

	// A game menu over the game, or the game in the background, closes the window
	if (SpawnMenu::Open) {
		const UInt32* menus = SpawnGame::MenuStack();
		if (!menus || menus[0] != Menu::kMenuType_None || !GameInFront()) SpawnMenu::Open = false;
	}
	if (!SpawnMenu::Open && gWasOpen) {
		gWasOpen = false;
		OnClosed();
	}
	FollowInputBlock();
	PollToolClick();

	// Closed: nothing more, unless the tool gun is out (its hint at the top of the screen)
	if (!SpawnMenu::Open) {
		if (!SpawnMenu::ToolEntry() || !gImGuiReady || device->TestCooperativeLevel() != D3D_OK) return;
		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		SpawnMenu::ToolDrawHud();
		ImGui::Render();
		DrawToBackBuffer(device);
		return;
	}
	if (!gImGuiReady) InitImGui(device, window);
	if (!gWasOpen) {
		gWasOpen = true;
		OnOpened(window);
	}
	if (device->TestCooperativeLevel() != D3D_OK) return;

	FeedKeyboard();
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	FeedMouse(window);
	ImGui::NewFrame();
	SpawnMenu::DrawWindow();	// bakes a few pictures in their own scene (SpawnRender puts every device state back), then the window
	ImGui::Render();
	DrawToBackBuffer(device);
}

// ---- device hooks ------------------------------------------------------------------------------------------------------------

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* SwapPresentFn)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
PresentFn RealPresent = nullptr;
ResetFn RealReset = nullptr;
SwapPresentFn RealSwapPresent = nullptr;
bool gInPresent = false;

// Draws once per frame whichever way the game presents (the device, or its swap chain directly); a Present that calls the other
// one inside it draws only once.
void PresentOnce(IDirect3DDevice9* device, const char* path) {
	if (gInPresent) return;
	static bool logged = false;
	if (!logged) {
		logged = true;
		Logger::Log("Spawn menu: first frame presented through the %s", path);
	}
	gInPresent = true;
	OnPresent(device);
	gInPresent = false;
}

HRESULT STDMETHODCALLTYPE PresentHook(IDirect3DDevice9* device, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
	PresentOnce(device, "device");
	return RealPresent(device, a, b, w, r);
}

HRESULT STDMETHODCALLTYPE SwapPresentHook(IDirect3DSwapChain9* chain, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD flags) {
	IDirect3DDevice9* device = nullptr;
	if (SUCCEEDED(chain->GetDevice(&device)) && device) {
		PresentOnce(device, "swap chain");
		device->Release();
	}
	return RealSwapPresent(chain, a, b, w, r, flags);
}

// Every D3DPOOL_DEFAULT object goes before the reset (the pictures and the bake's depth surface, ImGui's buffers).
HRESULT STDMETHODCALLTYPE ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
	SpawnMenu::ThumbRelease();
	if (gImGuiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
	const HRESULT result = RealReset(device, parameters);
	if (gImGuiReady && SUCCEEDED(result)) ImGui_ImplDX9_CreateDeviceObjects();
	return result;
}

void PatchSlot(void** slots, int slot, void* hook) {
	DWORD protection;
	VirtualProtect(&slots[slot], sizeof(void*), PAGE_READWRITE, &protection);
	slots[slot] = hook;
	VirtualProtect(&slots[slot], sizeof(void*), protection, &protection);
}

// Once the game has its device (the first game loops): Present is slot 17, Reset slot 16.
void HookDeviceOnce() {
	if (RealPresent) return;
	IDirect3DDevice9* device = SpawnGame::Device();
	if (!device) return;
	void** slots = *(void***)device;
	RealPresent = (PresentFn)slots[17];
	RealReset = (ResetFn)slots[16];
	PatchSlot(slots, 16, (void*)ResetHook);
	PatchSlot(slots, 17, (void*)PresentHook);
	IDirect3DSwapChain9* chain = nullptr;
	if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain) {	// IDirect3DSwapChain9::Present is slot 3
		void** chainSlots = *(void***)chain;
		RealSwapPresent = (SwapPresentFn)chainSlots[3];
		PatchSlot(chainSlots, 3, (void*)SwapPresentHook);
		chain->Release();
	}
	Logger::Log("Spawn menu: hooked Present (device %p%s) and Reset; %s opens the menu in game", device, RealSwapPresent ? " and swap chain" : "",
		SpawnMenu::KeyName(ToggleKey()).c_str());
}

void OnNVSEMessage(NVSEMessagingInterface::Message* message) {
	if (message->type != NVSEMessagingInterface::kMessage_MainGameLoop) return;
	HookDeviceOnce();
	RunConsoleLines();
	SpawnMenu::ToolGameLoop();
}

}	// namespace

// ---- shared helpers (SpawnGame.h) ----------------------------------------------------------------------------------------------

void Logger::Log(const char* format, ...) {
	if (!gLog) return;
	va_list args;
	va_start(args, format);
	vfprintf(gLog, format, args);
	va_end(args);
	fputc('\n', gLog);
	fflush(gLog);
}

std::string SpawnGame::GameRoot() {
	static const std::string root = ExeFolder();
	return root;
}

void SpawnGame::ConsoleLater(const char* line) { gConsoleLines.push_back(line); }

// ---- xNVSE entry points --------------------------------------------------------------------------------------------------------

extern "C" {

__declspec(dllexport) bool NVSEPlugin_Query(const PluginInterface* nvse, PluginInfo* info) {
	info->InfoVersion = PluginInfo::kInfoVersion;
	info->Name = "SpawnMenu";
	info->Version = kPluginVersion;
	return !nvse->IsEditor;
}

__declspec(dllexport) bool NVSEPlugin_Load(const PluginInterface* nvse) {
	if (nvse->IsEditor) return false;
	gLog = _fsopen((ExeFolder() + "\\SpawnMenu.log").c_str(), "w", _SH_DENYWR);
	Logger::Log("Spawn menu %u: loading", kPluginVersion);
	SpawnMenu::OptionsLoad();
	Logger::Log("Spawn menu: menu key %s, %s, %s names, equip on add %d", SpawnMenu::KeyName(ToggleKey()).c_str(),
		SpawnMenu::Opt.listMode ? "list" : "pictures", SpawnMenu::Opt.editorNames ? "editor ID" : "in-game", SpawnMenu::Opt.autoEquip);

	gConsole = (NVSEConsoleInterface*)nvse->QueryInterface(kInterface_Console);
	NVSEDataInterface* data = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data);
	if (data) gDIHook = (DIHookControl*)data->GetSingleton(NVSEDataInterface::kNVSEData_DIHookControl);
	NVSEMessagingInterface* messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	if (!gConsole || !messaging || !messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", OnNVSEMessage)) {
		Logger::Log("Spawn menu: xNVSE did not give the console or messaging interface: not loaded");
		return false;
	}
	if (!gDIHook) Logger::Log("Spawn menu: no DIHookControl from xNVSE: the game will still see keys while the menu is open");
	return true;
}

}
