#include "SpawnMenu.h"
#include <deque>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// UNOFFICIAL 2026-11-04 Spawn menu pictures. Per entry: a 128 x 128 texture from the disk cache (Data\VegasShaders\SpawnThumbs\),
// else baked from its model (SpawnRender.cpp) and saved there, else the Pip-Boy icon (loose file, else from the archives).
// Everything runs on the main thread inside the render call; the bake and the archive read are wrapped in SEH.
namespace SpawnMenu {
namespace {

	const int kLoadSize = 128;				// pictures are loaded at this size (D3DX scales the kThumbSize bake down on load)
	const unsigned kMaxTextures = 600;		// picture and icon textures held at once
	const unsigned kBakePerFrame = 2;		// models drawn per frame
	const unsigned kGiveUpFrames = 120;		// a model that is still loading after this many frames becomes its icon
	const double kBudgetMs = 3.0;			// time per frame for ThumbFrame
	const unsigned kEvictMinAge = 2;		// a picture asked for within this many frames is never freed (ImGui may still draw it)
	const unsigned kStaleFrames = 30;		// a queued entry not asked for in this many frames scrolled away: dropped, asked again on return

	// Game functions (1.4.0.525), by address. Addresses: WallSoGB's map (FalloutNV.exe map.map) names FileFinder__GetFile at
	// 0x006FCF20 (+0x401000 = 0xAFDF20), BSFile__GetFileSize at 0x006FE610 (0xAFF610) and BSFile__ReadF at 0x006FF650
	// (0xB00650, BSFile::Read(void*, uint) in the decomp). The game's font loader reads a texture-archive file the same way:
	// GetFile(path, READ_ONLY, 0x4000, 2), Read(buffer, Size()), delete. Not yet run in a live game.
	typedef void* (__cdecl* GameGetFileFn)(const char* path, int mode, unsigned bufferSize, unsigned archiveType);
	typedef unsigned (__thiscall* GameGetSizeFn)(void* file);
	typedef unsigned (__thiscall* GameReadFn)(void* file, void* dst, unsigned count);
	const uintptr_t kFileFinderGetFile = 0x00AFDF20;
	const uintptr_t kBSFileGetFileSize = 0x00AFF610;
	const uintptr_t kBSFileRead = 0x00B00650;

	enum PicState { kQueued, kLoaded, kFailed };

	struct Pic {
		Entry info;						// a copy: the bake and the icon need the model and the icon path
		PicState state = kQueued;
		IDirect3DTexture9* tex = nullptr;	// kLoaded: the picture (owned here)
		IDirect3DTexture9* icon = nullptr;	// kFailed: the icon (owned by g_icons; nullptr = none)
		unsigned lastFrame = 0;				// the frame it was last asked for
		unsigned firstBakeFrame = 0;		// when the bake first said "retry" (0 = not yet)
		std::string cachePath;				// relative to the game folder
	};

	std::unordered_map<unsigned, Pic> g_pics;						// by formID; absent = not requested (or freed)
	std::deque<unsigned> g_queue;									// formIDs waiting; a stale id is skipped when reached
	std::unordered_map<std::string, IDirect3DTexture9*> g_icons;	// by normalised icon path; nullptr = looked for, not found
	unsigned g_frame = 0;
	unsigned g_textures = 0;			// picture and icon textures held
	bool g_renderOff = false;			// RenderBegin said no: pictures come from the cache and icons for the session
	bool g_archiveOff = false;			// the archive read threw: icons from loose files only for the session

	enum LogKind { kLogLoad, kLogSave, kLogRender, kLogArchive, kLogKinds };
	bool g_logged[kLogKinds] = {};

	// One log line per session per kind of failure; a per-frame message would fill the log.
	void LogOnce(LogKind kind, const char* format, ...) {
		if (g_logged[kind]) return;
		g_logged[kind] = true;
		char text[256];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		text[sizeof(text) - 1] = 0;
		Logger::Log("SpawnThumbs: %s", text);
	}

	IDirect3DDevice9* Device() { return SpawnGame::Device(); }

	// The game folder + a path relative to it (the current folder can change later).
	std::string AbsPath(const std::string& relative) { return SpawnGame::GameRoot() + "\\" + relative; }

	bool FileExists(const std::string& absolute) {
		const DWORD attributes = GetFileAttributesA(absolute.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	// Creates the folder of a relative file path (once per folder, so the disk is not asked again every frame).
	void EnsureFolder(const std::string& relativeFile) {
		static std::unordered_map<std::string, bool> made;
		const std::string absolute = AbsPath(relativeFile);
		const std::string folder = std::filesystem::path(absolute).parent_path().string();
		if (made.count(folder)) return;
		made[folder] = true;
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
	}

	// Path separators as the game writes them, lower case: the key for icons.
	std::string NormalPath(const std::string& path) {
		std::string out;
		out.reserve(path.size());
		for (unsigned char c : path) out.push_back(c == '/' ? '\\' : static_cast<char>(std::tolower(c)));
		return out;
	}

	// FNV-1a of the model path: a mod that swaps the model gets a new picture; the load order byte is not part of it.
	unsigned HashModel(const std::string& model) {
		unsigned hash = 2166136261u;
		for (unsigned char c : NormalPath(model)) {
			hash ^= c;
			hash *= 16777619u;
		}
		return hash;
	}

	std::string SafeFolderName(const std::string& plugin) {
		std::string out = plugin;
		for (char& c : out)
			if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
		return out.empty() ? std::string("Unknown") : out;
	}

	std::string CachePathFor(const Entry& entry) {
		char name[32];
		// "_side3": weapons are baked as a side profile since 11-05 (the first tries looked down the barrel, "_side", and at
		// the underside, "_side2"), so their older pictures are not reused
		snprintf(name, sizeof(name), "%06X_%08X%s.dds", entry.formID & 0xFFFFFF, HashModel(entry.model),
			entry.category == kCat_Weapon ? "_side3" : "");
		return ThumbCacheFolder() + SafeFolderName(entry.plugin) + "\\" + name;
	}

	// The archive read, in its own function: SEH cannot share a function with C++ objects that need unwinding.
	// Returns 0 (data read), 1 (not in the archives), 2 (bad size or short read), 3 (exception).
	int ReadArchiveRaw(const char* path, unsigned char** outData, unsigned* outSize) {
		*outData = nullptr;
		*outSize = 0;
		__try {
			// Archive type 2 = textures (the game's font loader asks for its .tex files the same way); 0x20 is shaders.
			void* file = reinterpret_cast<GameGetFileFn>(kFileFinderGetFile)(path, 0, 0x4000, 2);
			if (!file) return 1;
			const unsigned size = reinterpret_cast<GameGetSizeFn>(kBSFileGetFileSize)(file);
			int result = 2;
			if (size > 0 && size <= (16u << 20)) {
				unsigned char* buffer = static_cast<unsigned char*>(malloc(size));
				if (buffer) {
					if (reinterpret_cast<GameReadFn>(kBSFileRead)(file, buffer, size) == size) {
						*outData = buffer;
						*outSize = size;
						result = 0;
					} else {
						free(buffer);
					}
				}
			}
			// Slot 0 of the NiFile vtable is the virtual destructor; the scalar deleting form (flag 1) frees it with the game's allocator.
			void** vtable = *static_cast<void***>(file);
			reinterpret_cast<void(__thiscall*)(void*, unsigned)>(vtable[0])(file, 1);
			return result;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return 3;
		}
	}

	// The bake in its own function, behind SEH: a model that breaks the draw becomes a failed picture, not a dead game.
	// Returns 0, or 1 when it threw (then *out is nullptr and *retry false).
	int BakeRaw(const Entry* entry, bool* retry, void** out) {
		*retry = false;
		*out = nullptr;
		__try {
			*out = RenderEntry(*entry, retry);
			return 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			*retry = false;
			*out = nullptr;
			return 1;
		}
	}

	// Frees picture textures asked for longest ago until there is room for one more. Only those not asked for in the last few
	// frames: the window may still hand their pointers to ImGui for this frame.
	bool MakeRoom() {
		while (g_textures >= kMaxTextures) {
			auto oldest = g_pics.end();
			for (auto it = g_pics.begin(); it != g_pics.end(); ++it) {
				if (it->second.state != kLoaded || it->second.lastFrame + kEvictMinAge >= g_frame) continue;
				if (oldest == g_pics.end() || it->second.lastFrame < oldest->second.lastFrame) oldest = it;
			}
			if (oldest == g_pics.end()) return false;
			if (oldest->second.tex) oldest->second.tex->Release();
			g_textures--;
			g_pics.erase(oldest);	// back to not requested: the next Thumbnail call queues it again
		}
		return true;
	}

	// An icon (relative to Data\Textures): a loose file first, else the archives. Loaded once; nullptr when there is none.
	IDirect3DTexture9* GetIcon(const std::string& icon) {
		if (icon.empty()) return nullptr;
		const std::string key = NormalPath(icon);
		auto found = g_icons.find(key);
		if (found != g_icons.end()) return found->second;

		IDirect3DDevice9* device = Device();
		if (!device) return nullptr;
		if (!MakeRoom()) return nullptr;	// not cached: asked again later

		IDirect3DTexture9* tex = nullptr;
		std::string loose = std::string("Data\\Textures\\") + icon;
		for (char& c : loose)
			if (c == '/') c = '\\';
		const std::string looseAbs = AbsPath(loose);
		if (FileExists(looseAbs)) {
			D3DXCreateTextureFromFileExA(device, looseAbs.c_str(), kLoadSize, kLoadSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
				D3DX_DEFAULT, D3DX_DEFAULT, 0, nullptr, nullptr, &tex);
		} else if (!g_archiveOff) {
			unsigned char* data = nullptr;
			unsigned size = 0;
			const std::string inArchive = std::string("Textures\\") + key;	// the game's own path: relative to Data
			const int result = ReadArchiveRaw(inArchive.c_str(), &data, &size);
			if (result == 3) {
				g_archiveOff = true;
				LogOnce(kLogArchive, "reading an icon from the archives threw; icons from loose files only for this session");
			}
			if (result == 0) {
				D3DXCreateTextureFromFileInMemoryEx(device, data, size, kLoadSize, kLoadSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
					D3DX_DEFAULT, D3DX_DEFAULT, 0, nullptr, nullptr, &tex);
				free(data);
			}
		}
		if (tex) g_textures++;
		g_icons[key] = tex;
		return tex;
	}

	// The picture from its cache file. False when it cannot be loaded (the caller falls back).
	bool LoadPicture(Pic& pic) {
		IDirect3DDevice9* device = Device();
		if (!device || !MakeRoom()) return false;
		IDirect3DTexture9* tex = nullptr;
		if (FAILED(D3DXCreateTextureFromFileExA(device, AbsPath(pic.cachePath).c_str(), kLoadSize, kLoadSize, 1, 0, D3DFMT_A8R8G8B8,
				D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, nullptr, nullptr, &tex)) || !tex) {
			LogOnce(kLogLoad, "a cached picture would not load (e.g. %s)", pic.cachePath.c_str());
			return false;
		}
		pic.tex = tex;
		pic.state = kLoaded;
		g_textures++;
		return true;
	}

	// Copies the baked render target to system memory, saves it as the cache file and releases it. False when the save failed.
	bool SaveBake(IDirect3DTexture9* rt, const std::string& cachePath) {
		IDirect3DDevice9* device = Device();
		IDirect3DSurface9* src = nullptr;
		IDirect3DSurface9* sys = nullptr;
		bool ok = false;
		if (device && rt && SUCCEEDED(rt->GetSurfaceLevel(0, &src))) {
			D3DSURFACE_DESC desc = {};
			src->GetDesc(&desc);
			if (SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
					SUCCEEDED(device->GetRenderTargetData(src, sys))) {
				EnsureFolder(cachePath);
				ok = SUCCEEDED(D3DXSaveSurfaceToFileA(AbsPath(cachePath).c_str(), D3DXIFF_DDS, sys, nullptr, nullptr));
				if (!ok) LogOnce(kLogSave, "saving a baked picture failed (e.g. %s)", cachePath.c_str());
			}
		}
		if (sys) sys->Release();
		if (src) src->Release();
		if (rt) rt->Release();
		return ok;
	}

	// Gives the entry its icon and makes it final (nothing more to try).
	void Fail(Pic& pic) {
		pic.state = kFailed;
		pic.icon = GetIcon(pic.info.icon);
	}

}

// ---- the window's calls ----

std::string ThumbCacheFolder() {
	static bool made = false;
	const std::string folder = std::string("Data\\SpawnMenu\\Thumbs\\");
	if (!made) {
		made = true;
		std::error_code ec;
		std::filesystem::create_directories(AbsPath(folder), ec);
	}
	return folder;
}

void* Thumbnail(const Entry& entry) {
	auto it = g_pics.find(entry.formID);
	if (it == g_pics.end()) {
		Pic pic;
		pic.info = entry;
		pic.lastFrame = g_frame;
		pic.cachePath = CachePathFor(entry);
		it = g_pics.emplace(entry.formID, pic).first;
		g_queue.push_back(entry.formID);
	}
	Pic& pic = it->second;
	pic.lastFrame = g_frame;
	if (pic.state == kLoaded) return pic.tex;
	if (pic.state == kFailed) return pic.icon;
	return nullptr;	// queued: ThumbFrame gets to it
}

void ThumbFrame() {
	++g_frame;
	if (g_queue.empty()) return;

	LARGE_INTEGER frequency = {}, start = {}, now = {};
	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&start);
	auto overBudget = [&]() {
		QueryPerformanceCounter(&now);
		return (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart > kBudgetMs;
	};

	// Each queued entry, in order: the disk cache; else a bake (at most kBakePerFrame, one RenderBegin/RenderEnd pair); else
	// the icon when it has no model, is an NPC, or cannot be baked. Whatever is left over stays queued.
	std::deque<unsigned> next;
	bool stop = false;
	bool began = false;
	bool rendering = false;
	unsigned baked = 0;
	while (!g_queue.empty()) {
		const unsigned id = g_queue.front();
		g_queue.pop_front();
		auto it = g_pics.find(id);
		if (it == g_pics.end() || it->second.state != kQueued) continue;
		// Off screen for a while (scrolled past): no model load for it. A queued entry holds no texture, so it is simply forgotten.
		if (it->second.lastFrame + kStaleFrames < g_frame) {
			g_pics.erase(it);
			continue;
		}
		if (stop || overBudget()) {
			stop = true;
			next.push_back(id);
			continue;
		}
		Pic& pic = it->second;

		if (FileExists(AbsPath(pic.cachePath)) && LoadPicture(pic)) continue;

		const bool bakeable = !pic.info.model.empty() && pic.info.category != kCat_NPC && !g_renderOff;
		if (!bakeable) {
			Fail(pic);
			continue;
		}
		if (baked >= kBakePerFrame) {
			next.push_back(id);
			continue;
		}
		if (!began) {
			began = true;
			if (!RenderBegin()) {
				g_renderOff = true;
				LogOnce(kLogRender, "the model renderer is not available; pictures from the cache and icons only");
				Fail(pic);
				continue;
			}
			rendering = true;
		}

		bool retry = false;
		void* rt = nullptr;
		++baked;
		if (BakeRaw(&pic.info, &retry, &rt) != 0) {
			LogOnce(kLogRender, "drawing a model threw an exception (e.g. %s); its icon is used", pic.info.model.c_str());
			retry = false;
			rt = nullptr;
		}
		if (rt) {
			if (SaveBake(static_cast<IDirect3DTexture9*>(rt), pic.cachePath) && LoadPicture(pic)) continue;
			Fail(pic);
			continue;
		}
		if (retry) {
			if (pic.firstBakeFrame == 0) pic.firstBakeFrame = g_frame;
			if (g_frame - pic.firstBakeFrame < kGiveUpFrames) {
				next.push_back(id);
				continue;
			}
		}
		Fail(pic);
	}
	if (rendering) RenderEnd();
	g_queue.swap(next);
}

void ThumbRelease() {
	for (auto& kv : g_pics)
		if (kv.second.tex) kv.second.tex->Release();
	for (auto& kv : g_icons)
		if (kv.second) kv.second->Release();
	g_pics.clear();
	g_icons.clear();
	g_queue.clear();
	g_textures = 0;
	RenderRelease();	// the bake's depth surface is D3DPOOL_DEFAULT: a lost device cannot be reset while it lives
}

}
