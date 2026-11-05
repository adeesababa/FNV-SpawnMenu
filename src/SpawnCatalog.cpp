#include "SpawnMenu.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// The form list for the spawn menu. Walks the game's form-ID map (TESForm::ms_pFormIDMap, the global at 0x011C54C0,
// a NiTPointerMap<UInt32, TESForm*>), keeps the spawnable types and reads each one's names and paths under SEH.
// Addresses and layouts: fallout_shared/tesform.cpp and tesform_TESForm.h (vtable slot 76 = GetName, which the repo calls
// GetEditorName), tesdatahandler.cpp (form type codes, from the type switch at 0x00465110), effectsetting.h (TESFullName,
// TESTexture/TESIcon at +4), fallout/interface/interface_rebase_types_TESCreature.h (TESModelList), build/units.json (the
// RTTI descriptors and __RTDynamicCast at 0x00EC43FB). All read only, from the 1.4.0.525 decomp; the names are its own.
namespace SpawnMenu {
namespace {

// TESForm::formType codes (TESDataHandler type switch).
enum : unsigned char {
	kType_Book = 25, kType_Armor = 24, kType_Clothing = 26, kType_Ingredient = 29, kType_Light = 30, kType_Misc = 31,
	kType_Weapon = 40, kType_Ammo = 41, kType_NPC = 42, kType_Creature = 43, kType_Key = 46, kType_AlchemyItem = 47,
	kType_Note = 49,
};

const unsigned int kFormID_Player = 0x00000007;	// the player's own NPC base form: never listed
const unsigned int kFlag_Deleted = 0x20;		// TESForm::MarkForDeletion sets it

// Game addresses (the decomp's units.json, 1.4.0.525).
const unsigned int kFormIDMapGlobal = 0x011C54C0;	// TESForm::ms_pFormIDMap
const unsigned int kRTTI_TESForm = 0x01183028;
const unsigned int kRTTI_TESFullName = 0x01183158;
const unsigned int kRTTI_TESModel = 0x011831E8;
const unsigned int kRTTI_TESIcon = 0x01183200;
const unsigned int kRTTI_TESModelList = 0x01186B14;
const unsigned int kRTTI_TESBipedModelForm = 0x01183978;	// as kNVSE's GameRTTI_1_4_0_525.inc
typedef void* (__cdecl* DynamicCastFn)(void*, long, void*, void*, int);
DynamicCastFn const DynamicCast = (DynamicCastFn)0x00EC43FB;	// the game's copy: the DLL's own CRT one would use its RTTI

// The component of a form's class, or null when the form does not have it (TESForm is the first base, so offset 0).
void* CastFromForm(const TESForm* form, unsigned int target) {
	return DynamicCast((void*)form, 0, (void*)kRTTI_TESForm, (void*)target, 0);
}

// NiTPointerMap<UInt32, TESForm*>: buckets of chained entries.
struct FormMapEntry {	// 0xC bytes
	FormMapEntry*	next;	// +0x0
	UInt32			key;	// +0x4 form ID
	TESForm*		form;	// +0x8
};
struct FormMap {	// 0x10 bytes
	void*			vtable;		// +0x0
	UInt32			hashSize;	// +0x4
	FormMapEntry**	buckets;	// +0x8
	UInt32			count;		// +0xC
};

// Plain data only: a function with a __try cannot have C++ objects that need unwinding.
struct RawForm {
	unsigned int	formID;
	unsigned char	formType;
	char			name[256];		// TESFullName
	char			editorID[128];	// GetEditorName, often empty
	char			model[260];		// TESModel nif, or the first TESModelList entry for creatures
	char			icon[260];		// TESIcon dds
};

enum ReadResult { kRead_Faulted, kRead_Ignored, kRead_Deleted, kRead_Ok };

Category CategoryOf(unsigned char type) {
	switch (type) {
	case kType_Weapon: return kCat_Weapon;
	case kType_Armor: case kType_Clothing: return kCat_Armor;
	case kType_AlchemyItem: case kType_Ingredient: return kCat_Aid;
	case kType_Ammo: return kCat_Ammo;
	case kType_Misc: case kType_Key: case kType_Note: case kType_Book: case kType_Light: return kCat_Misc;
	case kType_NPC: return kCat_NPC;
	case kType_Creature: return kCat_Creature;
	}
	return kCat_Count;	// not listed
}

void CopyText(char* dst, unsigned int size, const char* src) {
	dst[0] = 0;
	if (!src) return;
	unsigned int i = 0;
	for (; i + 1 < size && src[i]; ++i) dst[i] = src[i];
	dst[i] = 0;
}

// TESModelList: a BSSimpleList<const char*> at +0x4 of the component; each node is {item, next}. The entries are file names
// in the folder of the creature's skeleton (its TESModel, e.g. "Creatures\Radscorpion\Skeleton.NIF"), so a bare name gets
// that folder put in front of it: dst holds the skeleton path on entry.
void CopyFirstModel(void* list, char* dst, unsigned int size) {
	unsigned int folder = 0;	// length of the skeleton's folder, separator included
	for (unsigned int i = 0; dst[i]; ++i)
		if (dst[i] == '\\' || dst[i] == '/') folder = i + 1;
	unsigned char* node = (unsigned char*)list + 4;
	while (node) {
		const char* path = *(const char**)node;
		if (path && *path) {
			bool bare = true;
			for (const char* c = path; *c; ++c)
				if (*c == '\\' || *c == '/') bare = false;
			if (bare && folder) CopyText(dst + folder, size - folder, path);
			else CopyText(dst, size, path);
			return;
		}
		node = *(unsigned char**)(node + 4);
	}
	dst[0] = 0;	// no body model: the skeleton alone draws nothing
}

// TESBipedModelForm (armor): no TESModel or TESIcon base of its own, but the male and female ground models (TESModelTextureSwap,
// 0x20 bytes each, path at +4) at +0x4C and the two inventory icons (TESIcon, 0xC bytes, path at +4) at +0x8C. Layout as
// kNVSE's GameForms.h has it.
const unsigned int kBiped_GroundModel = 0x4C, kBiped_Icon = 0x8C;
void CopyBipedPaths(void* biped, RawForm* raw) {
	unsigned char* base = (unsigned char*)biped;
	for (int i = 0; i < 2 && !raw->model[0]; ++i)
		CopyText(raw->model, sizeof(raw->model), ((const TESModel*)(base + kBiped_GroundModel + i * 0x20))->nifPath.m_data);
	for (int i = 0; i < 2 && !raw->icon[0]; ++i)
		CopyText(raw->icon, sizeof(raw->icon), ((const TESTexture*)(base + kBiped_Icon + i * 0xC))->ddsPath.m_data);
}

// Reads one form under SEH. A bad form returns kRead_Faulted and the walk goes on.
ReadResult ReadForm(TESForm* form, RawForm* raw) {
	__try {
		raw->formID = form->refID;
		raw->formType = form->formType;
		raw->name[0] = raw->editorID[0] = raw->model[0] = raw->icon[0] = 0;
		if (form->flags & kFlag_Deleted) return kRead_Deleted;
		if (raw->formID == kFormID_Player || CategoryOf(raw->formType) == kCat_Count) return kRead_Ignored;

		TESFullName* fullName = (TESFullName*)CastFromForm(form, kRTTI_TESFullName);
		if (fullName) CopyText(raw->name, sizeof(raw->name), fullName->name.m_data);
		CopyText(raw->editorID, sizeof(raw->editorID), form->GetEditorName());

		if (raw->formType == kType_Creature) {
			TESModel* skeleton = (TESModel*)CastFromForm(form, kRTTI_TESModel);
			void* list = CastFromForm(form, kRTTI_TESModelList);
			if (skeleton) CopyText(raw->model, sizeof(raw->model), skeleton->nifPath.m_data);
			if (list) CopyFirstModel(list, raw->model, sizeof(raw->model));
			else raw->model[0] = 0;
		} else if (raw->formType != kType_NPC) {	// NPCs have no single model
			TESModel* model = (TESModel*)CastFromForm(form, kRTTI_TESModel);
			if (model) CopyText(raw->model, sizeof(raw->model), model->nifPath.m_data);
		}

		TESTexture* icon = (TESTexture*)CastFromForm(form, kRTTI_TESIcon);
		if (icon) CopyText(raw->icon, sizeof(raw->icon), icon->ddsPath.m_data);

		// Armor: its model and icons sit inside TESBipedModelForm (the TESModel and TESIcon casts find nothing).
		if (!raw->model[0] || !raw->icon[0]) {
			void* biped = CastFromForm(form, kRTTI_TESBipedModelForm);
			if (biped) CopyBipedPaths(biped, raw);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return kRead_Faulted;
	}
	return kRead_Ok;
}

// Every form pointer in the map, up to cap. Returns how many; *faulted is set when the walk hit a bad pointer.
unsigned int CollectForms(FormMap* map, TESForm** out, unsigned int cap, bool* faulted) {
	unsigned int n = 0;
	__try {
		for (unsigned int i = 0; i < map->hashSize && n < cap; ++i)
			for (FormMapEntry* entry = map->buckets[i]; entry && n < cap; entry = entry->next)
				out[n++] = entry->form;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*faulted = true;
	}
	return n;
}

// The plugin that owns the form's load-order byte (formID >> 24), from the game's mod list.
std::string PluginOf(unsigned int formID) {
	unsigned int index = formID >> 24;
	MainDataHandler* data = SpawnGame::DataHandler();
	if (!data) return "Unknown";
	const ModList& list = data->modList;
	if (index >= list.loadedModCount || index >= 0xFF || !list.loadedMods[index]) return "Unknown";
	return list.loadedMods[index]->name;
}

std::vector<Entry>	sCatalog;

// ---- places and weathers -------------------------------------------------------------------------------------------------------

const unsigned char kType_Weather = 0x35, kType_Cell = 0x39, kType_World = 0x41;
const unsigned int kRecord_Compressed = 0x00040000;

struct RawPlace {
	unsigned int	formID;
	char			editorID[128];
	char			name[256];
	bool			interior;
	bool			hasCoords;
	int				x, y;
	char			worldEditorID[128];
	char			worldName[256];
};

// A cell under SEH. 0 = read, 1 = not a listed place (deleted, or a worldspace's persistent cell with no grid), -1 = faulted.
int ReadCellSEH(TESForm* form, RawPlace* raw) {
	__try {
		TESObjectCELL* cell = (TESObjectCELL*)form;
		raw->formID = form->refID;
		raw->editorID[0] = raw->name[0] = raw->worldEditorID[0] = raw->worldName[0] = 0;
		raw->x = raw->y = 0;
		if (form->flags & kFlag_Deleted) return 1;
		CopyText(raw->editorID, sizeof(raw->editorID), form->GetEditorName());
		CopyText(raw->name, sizeof(raw->name), cell->fullName.name.m_data);
		raw->interior = (cell->flags0 & TESObjectCELL::kFlags0_Interior) != 0;
		raw->hasCoords = false;
		if (!raw->interior) {
			if (!cell->coords) return 1;
			raw->hasCoords = true;
			raw->x = cell->coords->x;
			raw->y = cell->coords->y;
			TESWorldSpace* world = cell->worldSpace;
			if (world) {
				CopyText(raw->worldEditorID, sizeof(raw->worldEditorID), world->GetEditorName());
				TESFullName* worldName = (TESFullName*)CastFromForm(world, kRTTI_TESFullName);
				if (worldName) CopyText(raw->worldName, sizeof(raw->worldName), worldName->name.m_data);
			}
		}
		return 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

// A weather under SEH: editor ID and classification. Same codes as ReadCellSEH.
int ReadWeatherSEH(TESForm* form, char* editorID, unsigned int size, unsigned char* kind, unsigned int* formID) {
	__try {
		editorID[0] = 0;
		*formID = form->refID;
		if (form->flags & kFlag_Deleted) return 1;
		CopyText(editorID, size, form->GetEditorName());
		*kind = ((TESWeather*)form)->weatherType;
		return 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

// The form's type, or -1 when the pointer is bad.
int FormTypeSEH(TESForm* form) {
	__try { return form->formType; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

std::string GameFolder() {
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string folder = path;
	const size_t slash = folder.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : folder.substr(0, slash);
}

// The game drops weathers' editor IDs once loaded, so they are read from the plugin files themselves: each file's top-level WTHR
// group, every record's EDID. File layout (TES4 format, FO3/FNV): 24-byte record and group headers, 6-byte subrecord headers.
// A record's form ID names its file by its top byte, an index into that file's own master list (MAST); the file's own records
// use the index one past its masters. Compressed records are skipped (their EDID is inside the zlib data).
struct RecordHeader {	// 24 bytes; a group (GRUP) uses size as the whole group's size and formID as its label
	char			type[4];
	unsigned int	size;
	unsigned int	flags;		// group: the group type
	unsigned int	formID;		// group: label
	unsigned int	stamp;
	unsigned int	version;
};

void ReadPluginWeatherIDs(const std::string& path, unsigned int fileIndex, const std::vector<std::string>& loadOrder,
	std::unordered_map<unsigned int, std::string>& out) {
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return;
	RecordHeader header;
	if (fread(&header, sizeof(header), 1, file) != 1 || memcmp(header.type, "TES4", 4) != 0 || header.size > (1u << 20)) {
		fclose(file);
		return;
	}
	// Masters, in the file's own order, as load-order indices (-1 = not loaded)
	std::vector<unsigned char> data(header.size);
	std::vector<int> masters;
	if (header.size && fread(data.data(), header.size, 1, file) == 1) {
		for (unsigned int at = 0; at + 6 <= header.size;) {
			const unsigned short length = *(const unsigned short*)&data[at + 4];
			if (at + 6 + length > header.size) break;
			if (memcmp(&data[at], "MAST", 4) == 0) {
				std::string name((const char*)&data[at + 6], strnlen((const char*)&data[at + 6], length));
				int index = -1;
				for (size_t i = 0; i < loadOrder.size(); i++)
					if (_stricmp(loadOrder[i].c_str(), name.c_str()) == 0) { index = (int)i; break; }
				masters.push_back(index);
			}
			at += 6 + length;
		}
	}
	// Top-level groups: only WTHR is read, every other one is skipped in one seek
	while (fread(&header, sizeof(header), 1, file) == 1 && memcmp(header.type, "GRUP", 4) == 0 && header.size >= sizeof(header)) {
		const long long groupEnd = _ftelli64(file) - (long long)sizeof(header) + header.size;
		if (memcmp(&header.formID, "WTHR", 4) == 0) {
			while (_ftelli64(file) + (long long)sizeof(header) <= groupEnd && fread(&header, sizeof(header), 1, file) == 1) {
				const long long recordEnd = _ftelli64(file) + header.size;
				if (memcmp(header.type, "WTHR", 4) == 0 && !(header.flags & kRecord_Compressed) && header.size < (1u << 20)) {
					data.resize(header.size);
					if (header.size && fread(data.data(), header.size, 1, file) == 1 && header.size >= 6 && memcmp(&data[0], "EDID", 4) == 0) {
						const unsigned short length = *(const unsigned short*)&data[4];
						if (6u + length <= header.size) {
							const unsigned int top = header.formID >> 24;
							const int owner = top < masters.size() ? masters[top] : (int)fileIndex;
							if (owner >= 0)
								out[((unsigned int)owner << 24) | (header.formID & 0xFFFFFF)] =
									std::string((const char*)&data[6], strnlen((const char*)&data[6], length));
						}
					}
				}
				_fseeki64(file, recordEnd, SEEK_SET);
			}
		}
		_fseeki64(file, groupEnd, SEEK_SET);
	}
	fclose(file);
}

std::vector<Place>			sPlaces;
std::vector<WeatherEntry>	sWeathers;
std::unordered_map<unsigned int, std::string> sWeatherIDs;	// from the plugin files, read once per load order
std::string sWeatherIDsFor;									// the load order sWeatherIDs was read for

std::string Lower(std::string text) {
	for (char& c : text) c = (char)tolower((unsigned char)c);
	return text;
}

}	// namespace

const std::vector<Entry>& Catalog() { return sCatalog; }
const std::vector<Place>& Places() { return sPlaces; }
const std::vector<WeatherEntry>& Weathers() { return sWeathers; }

void PlacesBuild() {
	LARGE_INTEGER freq, start, end;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);

	// The weather editor IDs from the plugin files, when the load order changed since the last read
	std::vector<std::string> loadOrder;
	std::string orderKey;
	if (MainDataHandler* data = SpawnGame::DataHandler()) {
		const ModList& list = data->modList;
		for (unsigned int i = 0; i < list.loadedModCount && i < 0xFF; i++) {
			loadOrder.push_back(list.loadedMods[i] ? list.loadedMods[i]->name : "");
			orderKey += loadOrder.back();
			orderKey += '|';
		}
	}
	if (orderKey != sWeatherIDsFor) {
		sWeatherIDs.clear();
		const std::string dataFolder = GameFolder() + "\\Data\\";
		for (size_t i = 0; i < loadOrder.size(); i++)
			if (!loadOrder[i].empty()) ReadPluginWeatherIDs(dataFolder + loadOrder[i], (unsigned int)i, loadOrder, sWeatherIDs);
		sWeatherIDsFor = orderKey;
	}

	std::vector<Place> places;
	std::vector<WeatherEntry> weathers;
	unsigned int faulted = 0, fromFiles = 0;
	bool walkFaulted = false;
	FormMap* map = *(FormMap**)kFormIDMapGlobal;
	if (map && map->buckets) {
		const unsigned int cap = map->count + 64;
		std::vector<TESForm*> forms(cap);
		const unsigned int n = CollectForms(map, forms.data(), cap, &walkFaulted);
		RawPlace raw;
		char editorID[128];
		for (unsigned int i = 0; i < n; i++) {
			TESForm* form = forms[i];
			if (!form) continue;
			const int type = FormTypeSEH(form);
			if (type < 0) { faulted++; continue; }
			if (type == kType_Cell) {
				const int read = ReadCellSEH(form, &raw);
				if (read < 0) { faulted++; continue; }
				if (read > 0) continue;
				Place p;
				p.formID = raw.formID;
				p.editorID = raw.editorID;
				p.name = raw.name;
				p.plugin = PluginOf(raw.formID);
				p.interior = raw.interior;
				p.worldEditorID = raw.worldEditorID;
				p.world = raw.worldName[0] ? raw.worldName : raw.worldEditorID;
				p.x = raw.x;
				p.y = raw.y;
				char id[16];
				snprintf(id, sizeof(id), "%08x", p.formID);
				p.hay = Lower(p.editorID + '\n' + p.name + '\n' + p.plugin + '\n' + p.world + '\n' + id);
				places.push_back(std::move(p));
			} else if (type == kType_Weather) {
				WeatherEntry w;
				w.kind = 0;
				w.formID = 0;
				const int read = ReadWeatherSEH(form, editorID, sizeof(editorID), &w.kind, &w.formID);
				if (read < 0) { faulted++; continue; }
				if (read > 0) continue;
				w.editorID = editorID;
				if (w.editorID.empty()) {
					auto found = sWeatherIDs.find(w.formID);
					if (found != sWeatherIDs.end()) { w.editorID = found->second; fromFiles++; }
				}
				w.plugin = PluginOf(w.formID);
				char id[16];
				snprintf(id, sizeof(id), "%08x", w.formID);
				w.hay = Lower(w.editorID + '\n' + w.plugin + '\n' + id);
				weathers.push_back(std::move(w));
			}
		}
	}

	// GECK order: interiors by editor ID, then exteriors by worldspace and grid; weathers by editor ID
	std::sort(places.begin(), places.end(), [](const Place& a, const Place& b) {
		if (a.interior != b.interior) return a.interior;
		if (a.interior) return _stricmp(a.editorID.c_str(), b.editorID.c_str()) < 0;
		const int w = _stricmp(a.world.c_str(), b.world.c_str());
		if (w != 0) return w < 0;
		return a.x != b.x ? a.x < b.x : a.y < b.y;
	});
	std::sort(weathers.begin(), weathers.end(), [](const WeatherEntry& a, const WeatherEntry& b) {
		return _stricmp(a.editorID.c_str(), b.editorID.c_str()) < 0;
	});
	sPlaces.swap(places);
	sWeathers.swap(weathers);

	unsigned int interiors = 0, interiorIDs = 0, exteriorIDs = 0;
	for (const Place& p : sPlaces) {
		if (p.interior) { interiors++; if (!p.editorID.empty()) interiorIDs++; }
		else if (!p.worldEditorID.empty()) exteriorIDs++;
	}
	QueryPerformanceCounter(&end);
	Logger::Log("Spawn menu: places: %u interiors (%u with editor IDs), %u exterior cells (%u with a worldspace editor ID); "
		"%u weathers (%u editor IDs from the plugin files, %u read there in all); %u unreadable%s; %u ms",
		interiors, interiorIDs, (unsigned int)sPlaces.size() - interiors, exteriorIDs, (unsigned int)sWeathers.size(), fromFiles,
		(unsigned int)sWeatherIDs.size(), faulted, walkFaulted ? ", map walk stopped early" : "",
		(unsigned int)((end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart));
}

void CatalogBuild() {
	LARGE_INTEGER freq, start, end;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);

	std::vector<Entry> fresh;
	unsigned int counts[kCat_Count] = {};
	unsigned int deleted = 0, nameless = 0, unreadable = 0;
	bool walkFaulted = false;

	FormMap* map = *(FormMap**)kFormIDMapGlobal;
	if (map && map->buckets) {
		// Room for the whole map, so the walk never runs past it.
		unsigned int cap = map->count + 64;
		std::vector<TESForm*> forms(cap);
		unsigned int n = CollectForms(map, forms.data(), cap, &walkFaulted);

		fresh.reserve(n);
		RawForm raw;
		for (unsigned int i = 0; i < n; ++i) {
			if (!forms[i]) continue;
			ReadResult result = ReadForm(forms[i], &raw);
			if (result == kRead_Faulted) { ++unreadable; continue; }
			if (result == kRead_Deleted) { ++deleted; continue; }
			if (result != kRead_Ok) continue;
			if (!raw.name[0] && !raw.model[0]) { ++nameless; continue; }	// nothing to show it by

			Entry e;
			e.formID = raw.formID;
			e.formType = raw.formType;
			e.category = CategoryOf(raw.formType);
			e.editorID = raw.editorID;
			e.name = raw.name[0] ? raw.name : raw.editorID[0] ? raw.editorID : "";
			if (e.name.empty()) {
				char hex[16];
				std::snprintf(hex, sizeof(hex), "%08X", raw.formID);
				e.name = hex;
			}
			e.plugin = PluginOf(raw.formID);
			e.model = raw.model;
			e.icon = raw.icon;
			++counts[e.category];
			fresh.push_back(e);
		}
	}

	std::sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) {
		if (a.category != b.category) return a.category < b.category;
		return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
	});
	sCatalog.swap(fresh);

	QueryPerformanceCounter(&end);
	unsigned int ms = (unsigned int)((end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart);
	unsigned int skipped = deleted + nameless + unreadable;
	Logger::Log("Spawn menu: catalog %u entries (weapons %u, armor %u, aid %u, ammo %u, misc %u, npcs %u, creatures %u) in %u ms",
		(unsigned int)sCatalog.size(), counts[kCat_Weapon], counts[kCat_Armor], counts[kCat_Aid], counts[kCat_Ammo],
		counts[kCat_Misc], counts[kCat_NPC], counts[kCat_Creature], ms);
	Logger::Log("Spawn menu: skipped %u forms (deleted %u, no name or model %u, unreadable %u%s)",
		skipped, deleted, nameless, unreadable, walkFaulted ? ", map walk stopped early" : "");
	PlacesBuild();
}

}
