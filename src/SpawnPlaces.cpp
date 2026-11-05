#include "SpawnMenu.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <map>

// The Locations and Weather tabs: GECK-style tables (sortable, resizable columns) of every cell and weather the loaded plugins
// hold. A click on a cell travels there (coc for interiors, cow worldspace x y for exterior cells); a click on a weather forces
// it. The Weather tab also holds the time of day, the time scale and live edits to the current weather (not saved: the game
// reads the weather form again on the next load).
namespace SpawnMenu {
namespace {

bool Matches(const std::string& hay, const std::vector<std::string>& words) {
	for (const std::string& w : words)
		if (hay.find(w) == std::string::npos) return false;
	return true;
}

int Compare(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()); }
int Compare(unsigned a, unsigned b) { return a < b ? -1 : a > b ? 1 : 0; }
int Compare(int a, int b) { return a < b ? -1 : a > b ? 1 : 0; }

// Sorts rows (indices into items) by the table's sort specs; none = the list's own order (GECK order, see PlacesBuild).
template <typename T, typename Cmp>
void SortRows(std::vector<int>& rows, const std::vector<T>& items, const ImGuiTableSortSpecs* specs, Cmp compareColumn) {
	if (!specs || specs->SpecsCount == 0) {
		std::sort(rows.begin(), rows.end());
		return;
	}
	std::stable_sort(rows.begin(), rows.end(), [&](int ia, int ib) {
		for (int s = 0; s < specs->SpecsCount; s++) {
			const int d = compareColumn(items[ia], items[ib], (int)specs->Specs[s].ColumnUserID);
			if (d != 0) return specs->Specs[s].SortDirection == ImGuiSortDirection_Ascending ? d < 0 : d > 0;
		}
		return ia < ib;
	});
}

const ImGuiTableFlags kTableFlags = ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_SortTristate |
	ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
	ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

// ---- locations ---------------------------------------------------------------------------------------------------------------

bool gShowInteriors = true;
bool gShowExteriors = true;
bool gShowUnnamed = false;		// exterior cells with no editor ID and no name: thousands of plain wilderness
std::vector<int> gPlaceRows;
std::string gPlaceKey;			// the filter gPlaceRows was made for
const void* gPlaceData = nullptr;
size_t gPlaceCount = 0;
bool gPlaceSortDirty = true;

enum PlaceColumn { kPC_EditorID, kPC_Name, kPC_FormID, kPC_Type, kPC_World, kPC_X, kPC_Y, kPC_Plugin, kPC_Count };

bool CanTravel(const Place& p) { return p.interior ? !p.editorID.empty() : !p.worldEditorID.empty(); }

void Travel(const Place& p) {
	char line[300];
	if (p.interior) snprintf(line, sizeof(line), "coc %s", p.editorID.c_str());
	else snprintf(line, sizeof(line), "cow %s %d %d", p.worldEditorID.c_str(), p.x, p.y);
	ConsoleLine(line);
	Logger::Log("Spawn menu: travel to %08X: %s", p.formID, line);
	Open = false;	// a loading screen follows
}

std::string TravelCommand(const Place& p) {
	char line[300];
	if (p.interior) snprintf(line, sizeof(line), "coc %s", p.editorID.c_str());
	else snprintf(line, sizeof(line), "cow %s %d %d", p.worldEditorID.c_str(), p.x, p.y);
	return line;
}

void PlaceMenu(const Place& p) {
	if (!ImGui::BeginPopupContextItem("##placemenu")) return;
	if (ImGui::MenuItem("Go there", nullptr, false, CanTravel(p))) Travel(p);
	ImGui::Separator();
	if (CanTravel(p) && ImGui::MenuItem("Copy console command")) ImGui::SetClipboardText(TravelCommand(p).c_str());
	if (!p.editorID.empty() && ImGui::MenuItem("Copy editor ID")) ImGui::SetClipboardText(p.editorID.c_str());
	if (ImGui::MenuItem("Copy form ID")) {
		char id[16];
		snprintf(id, sizeof(id), "%08X", p.formID);
		ImGui::SetClipboardText(id);
	}
	ImGui::EndPopup();
}

// ---- weathers ----------------------------------------------------------------------------------------------------------------

std::vector<int> gWeatherRows;
std::string gWeatherKey;
const void* gWeatherData = nullptr;
size_t gWeatherCount = 0;
bool gWeatherSortDirty = true;

enum WeatherColumn { kWC_EditorID, kWC_FormID, kWC_Kind, kWC_Plugin, kWC_Count };

const char* KindName(unsigned char kind) {
	if (kind & TESWeather::kType_Snow) return "Snow";
	if (kind & TESWeather::kType_Rainy) return "Rainy";
	if (kind & TESWeather::kType_Cloudy) return "Cloudy";
	if (kind & 0x1) return "Pleasant";
	return "None";
}

std::string WeatherLabel(const WeatherEntry& w) {
	if (!w.editorID.empty()) return w.editorID;
	char id[24];
	snprintf(id, sizeof(id), "(weather %08X)", w.formID);
	return id;
}

void ForceWeather(const WeatherEntry& w, bool blend) {
	char line[48];
	snprintf(line, sizeof(line), blend ? "sw %08X" : "fw %08X", w.formID);
	ConsoleLine(line);
	Logger::Log("Spawn menu: weather %s (%s)", line, w.editorID.c_str());
}

// The live edits: what each edited weather held before, so it can be put back.
struct WeatherValues {
	TESWeather::FogInfo fogDay, fogNight;
	UInt8 wind, glare, cloudLower, cloudUpper, transDelta, sunDamage;
};
std::map<unsigned int, WeatherValues> gOriginals;

WeatherValues ValuesOf(TESWeather* w) {
	WeatherValues v;
	v.fogDay = w->fogDay;
	v.fogNight = w->fogNight;
	v.wind = w->windSpeed;
	v.glare = w->sunGlare;
	v.cloudLower = w->cloudSpeedLower;
	v.cloudUpper = w->cloudSpeedUpper;
	v.transDelta = w->transDelta;
	v.sunDamage = w->sunDamage;
	return v;
}

void PutValues(TESWeather* w, const WeatherValues& v) {
	w->fogDay = v.fogDay;
	w->fogNight = v.fogNight;
	w->windSpeed = v.wind;
	w->sunGlare = v.glare;
	w->cloudSpeedLower = v.cloudLower;
	w->cloudSpeedUpper = v.cloudUpper;
	w->transDelta = v.transDelta;
	w->sunDamage = v.sunDamage;
}

// A 0-255 byte as a slider; true when it changed.
bool SliderByte(const char* label, UInt8* value) {
	int v = *value;
	if (!ImGui::SliderInt(label, &v, 0, 255)) return false;
	*value = (UInt8)v;
	return true;
}

// Time of day, time scale, release, and the current weather's values.
void DrawWeatherControls() {
	TimeGlobals* time = TimeGlobals::Get();
	if (time && time->GameHour && time->TimeScale) {
		static float hour = 12.0f;
		if (!ImGui::IsAnyItemActive()) hour = time->GameHour->data;
		ImGui::SetNextItemWidth(220.0f);
		ImGui::SliderFloat("Time of day", &hour, 0.0f, 23.99f, "%.2f h");
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			char line[48];
			snprintf(line, sizeof(line), "set gamehour to %.2f", hour);
			ConsoleLine(line);
		}
		ImGui::SameLine();
		static int scale = 30;
		if (!ImGui::IsAnyItemActive()) scale = (int)time->TimeScale->data;
		ImGui::SetNextItemWidth(110.0f);
		ImGui::InputInt("Time scale", &scale);
		if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemEdited() && !ImGui::IsItemActive())) {
			if (scale < 0) scale = 0;
			char line[48];
			snprintf(line, sizeof(line), "set timescale to %d", scale);
			ConsoleLine(line);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Game minutes per real minute (30 by default; 0 stops the clock)");
		ImGui::SameLine();
	}
	if (ImGui::Button("Release forced weather")) {
		ConsoleLine("rw");
		Logger::Log("Spawn menu: weather released (rw)");
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hands the weather back to the region and climate");

	Sky* sky = Sky::Get();
	TESWeather* current = sky ? sky->firstWeather : nullptr;
	if (!current) {
		ImGui::TextDisabled("No current weather (interiors have none).");
		return;
	}
	const WeatherEntry* entry = nullptr;
	for (const WeatherEntry& w : Weathers())
		if (w.formID == current->refID) { entry = &w; break; }
	char header[160];
	snprintf(header, sizeof(header), "Edit current weather: %s###weatheredit", entry ? WeatherLabel(*entry).c_str() : "?");
	if (!ImGui::CollapsingHeader(header)) return;

	const WeatherValues before = ValuesOf(current);
	bool changed = false;
	ImGui::PushItemWidth(260.0f);
	changed |= ImGui::DragFloatRange2("Fog day (near / far)", &current->fogDay.nearFog, &current->fogDay.farFog, 50.0f, -10000.0f, 1000000.0f, "%.0f");
	changed |= ImGui::DragFloatRange2("Fog night (near / far)", &current->fogNight.nearFog, &current->fogNight.farFog, 50.0f, -10000.0f, 1000000.0f, "%.0f");
	changed |= ImGui::SliderFloat("Fog power (day)", &current->fogDay.power, 0.0f, 10.0f);
	changed |= ImGui::SliderFloat("Fog power (night)", &current->fogNight.power, 0.0f, 10.0f);
	changed |= SliderByte("Wind speed", &current->windSpeed);
	changed |= SliderByte("Sun glare", &current->sunGlare);
	changed |= SliderByte("Sun damage", &current->sunDamage);
	changed |= SliderByte("Cloud speed (lower)", &current->cloudSpeedLower);
	changed |= SliderByte("Cloud speed (upper)", &current->cloudSpeedUpper);
	changed |= SliderByte("Transition speed", &current->transDelta);
	ImGui::PopItemWidth();
	if (changed && gOriginals.find(current->refID) == gOriginals.end()) gOriginals[current->refID] = before;

	auto saved = gOriginals.find(current->refID);
	if (saved != gOriginals.end()) {
		if (ImGui::Button("Undo edits to this weather")) {
			PutValues(current, saved->second);
			gOriginals.erase(saved);
		}
		ImGui::SameLine();
	}
	ImGui::TextDisabled("Edits are live and last until the game is restarted.");
}

}	// namespace

void DrawPlaces(const std::vector<std::string>& words, const std::string& plugin) {
	const std::vector<Place>& places = Places();
	bool filterChanged = false;
	filterChanged |= ImGui::Checkbox("Interiors", &gShowInteriors);
	ImGui::SameLine();
	filterChanged |= ImGui::Checkbox("Exterior cells", &gShowExteriors);
	ImGui::SameLine();
	filterChanged |= ImGui::Checkbox("Unnamed exterior cells", &gShowUnnamed);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Wilderness cells with no editor ID and no name (there are thousands)");

	// The rows are redone when the search, plugin, checkboxes or the list itself changed
	std::string key = plugin + '\x1';
	for (const std::string& w : words) key += w + '\x1';
	key += gShowInteriors ? 'i' : '-';
	key += gShowExteriors ? 'e' : '-';
	key += gShowUnnamed ? 'u' : '-';
	if (filterChanged || key != gPlaceKey || gPlaceData != places.data() || gPlaceCount != places.size()) {
		gPlaceKey = key;
		gPlaceData = places.data();
		gPlaceCount = places.size();
		gPlaceRows.clear();
		for (size_t i = 0; i < places.size(); i++) {
			const Place& p = places[i];
			if (p.interior ? !gShowInteriors : !gShowExteriors) continue;
			if (!p.interior && !gShowUnnamed && p.editorID.empty() && p.name.empty()) continue;
			if (!plugin.empty() && p.plugin != plugin) continue;
			if (!Matches(p.hay, words)) continue;
			gPlaceRows.push_back((int)i);
		}
		gPlaceSortDirty = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%d shown of %d cells. Click: go there", (int)gPlaceRows.size(), (int)places.size());

	if (places.empty()) {
		ImGui::TextDisabled("No cells listed yet. Press Refresh once a save is loaded.");
		return;
	}
	if (!ImGui::BeginTable("##placelist", kPC_Count, kTableFlags)) return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("Editor ID", ImGuiTableColumnFlags_NoHide, 240.0f, kPC_EditorID);
	ImGui::TableSetupColumn("Name", 0, 200.0f, kPC_Name);
	ImGui::TableSetupColumn("Form ID", 0, 80.0f, kPC_FormID);
	ImGui::TableSetupColumn("Type", 0, 70.0f, kPC_Type);
	ImGui::TableSetupColumn("Worldspace", 0, 150.0f, kPC_World);
	ImGui::TableSetupColumn("X", 0, 40.0f, kPC_X);
	ImGui::TableSetupColumn("Y", 0, 40.0f, kPC_Y);
	ImGui::TableSetupColumn("Plugin", 0, 170.0f, kPC_Plugin);
	ImGui::TableHeadersRow();

	if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
		if (specs->SpecsDirty || gPlaceSortDirty) {
			SortRows(gPlaceRows, places, specs, [](const Place& a, const Place& b, int column) {
				switch (column) {
				case kPC_EditorID:	return Compare(a.editorID, b.editorID);
				case kPC_Name:		return Compare(a.name, b.name);
				case kPC_FormID:	return Compare(a.formID, b.formID);
				case kPC_Type:		return Compare((int)b.interior, (int)a.interior);
				case kPC_World:		return Compare(a.world, b.world);
				case kPC_X:			return Compare(a.x, b.x);
				case kPC_Y:			return Compare(a.y, b.y);
				case kPC_Plugin:	return Compare(a.plugin, b.plugin);
				}
				return 0;
			});
			specs->SpecsDirty = false;
			gPlaceSortDirty = false;
		}
	}

	ImGuiListClipper clip;
	clip.Begin((int)gPlaceRows.size());
	while (clip.Step()) {
		for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
			const int idx = gPlaceRows[row];
			const Place& p = places[idx];
			const bool travel = CanTravel(p);
			ImGui::PushID(idx);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(kPC_EditorID);
			if (!travel) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			const char* label = !p.editorID.empty() ? p.editorID.c_str() : p.interior ? "(no editor ID)" : "(wilderness)";
			if (ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && travel) Travel(p);
			if (!travel) ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) {
				if (travel) ImGui::SetTooltip("Click: %s\nRight-click: more", TravelCommand(p).c_str());
				else ImGui::SetTooltip("The game did not keep this cell's editor ID, so there is no way to coc there");
			}
			PlaceMenu(p);
			if (ImGui::TableSetColumnIndex(kPC_Name)) ImGui::TextUnformatted(p.name.c_str());
			if (ImGui::TableSetColumnIndex(kPC_FormID)) ImGui::Text("%08X", p.formID);
			if (ImGui::TableSetColumnIndex(kPC_Type)) ImGui::TextUnformatted(p.interior ? "Interior" : "Exterior");
			if (!p.interior) {
				if (ImGui::TableSetColumnIndex(kPC_World)) ImGui::TextUnformatted(p.world.c_str());
				if (ImGui::TableSetColumnIndex(kPC_X)) ImGui::Text("%d", p.x);
				if (ImGui::TableSetColumnIndex(kPC_Y)) ImGui::Text("%d", p.y);
			}
			if (ImGui::TableSetColumnIndex(kPC_Plugin)) ImGui::TextUnformatted(p.plugin.c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

void DrawWeathers(const std::vector<std::string>& words, const std::string& plugin) {
	DrawWeatherControls();
	ImGui::Separator();

	const std::vector<WeatherEntry>& weathers = Weathers();
	std::string key = plugin + '\x1';
	for (const std::string& w : words) key += w + '\x1';
	if (key != gWeatherKey || gWeatherData != weathers.data() || gWeatherCount != weathers.size()) {
		gWeatherKey = key;
		gWeatherData = weathers.data();
		gWeatherCount = weathers.size();
		gWeatherRows.clear();
		for (size_t i = 0; i < weathers.size(); i++) {
			if (!plugin.empty() && weathers[i].plugin != plugin) continue;
			if (!Matches(weathers[i].hay, words)) continue;
			gWeatherRows.push_back((int)i);
		}
		gWeatherSortDirty = true;
	}
	ImGui::TextDisabled("%d shown of %d weathers. Click: force it now    Right-click: blend into it", (int)gWeatherRows.size(), (int)weathers.size());
	if (weathers.empty()) return;

	Sky* sky = Sky::Get();
	const unsigned int currentID = sky && sky->firstWeather ? sky->firstWeather->refID : 0;

	if (!ImGui::BeginTable("##weatherlist", kWC_Count, kTableFlags)) return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn("Editor ID", ImGuiTableColumnFlags_NoHide, 300.0f, kWC_EditorID);
	ImGui::TableSetupColumn("Form ID", 0, 80.0f, kWC_FormID);
	ImGui::TableSetupColumn("Classification", 0, 100.0f, kWC_Kind);
	ImGui::TableSetupColumn("Plugin", 0, 200.0f, kWC_Plugin);
	ImGui::TableHeadersRow();

	if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
		if (specs->SpecsDirty || gWeatherSortDirty) {
			SortRows(gWeatherRows, weathers, specs, [](const WeatherEntry& a, const WeatherEntry& b, int column) {
				switch (column) {
				case kWC_EditorID:	return Compare(a.editorID, b.editorID);
				case kWC_FormID:	return Compare(a.formID, b.formID);
				case kWC_Kind:		return Compare(std::string(KindName(a.kind)), std::string(KindName(b.kind)));
				case kWC_Plugin:	return Compare(a.plugin, b.plugin);
				}
				return 0;
			});
			specs->SpecsDirty = false;
			gWeatherSortDirty = false;
		}
	}

	ImGuiListClipper clip;
	clip.Begin((int)gWeatherRows.size());
	while (clip.Step()) {
		for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
			const int idx = gWeatherRows[row];
			const WeatherEntry& w = weathers[idx];
			ImGui::PushID(idx);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(kWC_EditorID);
			const std::string label = WeatherLabel(w);
			if (ImGui::Selectable(label.c_str(), w.formID == currentID, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
				ForceWeather(w, false);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(w.formID == currentID ? "The current weather" : "Click: force it now (fw)\nRight-click: more");
			if (ImGui::BeginPopupContextItem("##weathermenu")) {
				if (ImGui::MenuItem("Force it now (fw)")) ForceWeather(w, false);
				if (ImGui::MenuItem("Blend into it (sw)")) ForceWeather(w, true);
				ImGui::Separator();
				if (!w.editorID.empty() && ImGui::MenuItem("Copy editor ID")) ImGui::SetClipboardText(w.editorID.c_str());
				if (ImGui::MenuItem("Copy form ID")) {
					char id[16];
					snprintf(id, sizeof(id), "%08X", w.formID);
					ImGui::SetClipboardText(id);
				}
				ImGui::EndPopup();
			}
			if (ImGui::TableSetColumnIndex(kWC_FormID)) ImGui::Text("%08X", w.formID);
			if (ImGui::TableSetColumnIndex(kWC_Kind)) ImGui::TextUnformatted(KindName(w.kind));
			if (ImGui::TableSetColumnIndex(kWC_Plugin)) ImGui::TextUnformatted(w.plugin.c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

}	// namespace SpawnMenu
