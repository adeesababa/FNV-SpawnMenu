#include "SpawnMenu.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

// The spawn window: every catalog entry as a tile in a grid, filtered once per change and drawn through a clipper, so 20,000+
// entries cost only the rows on screen. Runs on the render thread (see SpawnMenu.h).

namespace SpawnMenu {

bool Open = false;
Options Opt;

void ConsoleLine(const char* line) { SpawnGame::ConsoleLater(line); }
bool RebindingKey = false;

// ---- options ------------------------------------------------------------------------------------------------------------------

namespace {
	std::string OptionsPath() {
		char path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		std::string folder = path;
		const size_t slash = folder.find_last_of("\\/");
		folder = slash == std::string::npos ? std::string(".") : folder.substr(0, slash);
		CreateDirectoryA((folder + "\\Data\\SpawnMenu").c_str(), nullptr);
		return folder + "\\Data\\SpawnMenu\\SpawnMenu.ini";
	}
}

void OptionsLoad() {
	const std::string path = OptionsPath();
	const int key = (int)GetPrivateProfileIntA("Options", "MenuKey", 'L', path.c_str());
	Opt.menuKey = (key > 0 && key < 0xFF) ? key : 'L';
	Opt.listMode = GetPrivateProfileIntA("Options", "ListMode", 0, path.c_str()) != 0;
	Opt.editorNames = GetPrivateProfileIntA("Options", "EditorIDNames", 0, path.c_str()) != 0;
	Opt.autoEquip = GetPrivateProfileIntA("Options", "EquipOnAdd", 0, path.c_str()) != 0;
}

void OptionsSave() {
	const std::string path = OptionsPath();
	char value[16];
	snprintf(value, sizeof(value), "%d", Opt.menuKey);
	WritePrivateProfileStringA("Options", "MenuKey", value, path.c_str());
	WritePrivateProfileStringA("Options", "ListMode", Opt.listMode ? "1" : "0", path.c_str());
	WritePrivateProfileStringA("Options", "EditorIDNames", Opt.editorNames ? "1" : "0", path.c_str());
	WritePrivateProfileStringA("Options", "EquipOnAdd", Opt.autoEquip ? "1" : "0", path.c_str());
}

std::string KeyName(int vk) {
	const UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC_EX);
	char name[64] = {};
	const LONG lparam = (LONG)(((scan & 0xFF) << 16) | ((scan & 0xFF00) ? (1 << 24) : 0));
	if (scan && GetKeyNameTextA(lparam, name, sizeof(name)) > 0) return name;
	snprintf(name, sizeof(name), "key %02X", vk);
	return name;
}

namespace {

const char* const kCatNames[] = { "Weapons", "Armor", "Aid", "Ammo", "Misc", "NPCs", "Creatures" };
const int kTabCount = kCat_Count + 1;		// "All" plus one per category
const int kTabPlaces = kTabCount;			// then Locations and Weather (not catalog categories)
const int kTabWeather = kTabCount + 1;

// Items can go in the inventory; NPCs and creatures are only placed.
bool IsItemCategory(Category c) { return c <= kCat_Misc; }

// Menu commands run inside the game's Present; placeatme and additem load, so they run from the next game loop instead.
void ConsoleLater(const char* line) { SpawnGame::ConsoleLater(line); }

// ---- state ------------------------------------------------------------------------------------------------------------------

bool gWasOpen = false;
char gSearch[128] = "";
std::string gPlugin;				// "" = All plugins
int gTab = 0;						// 0 = All, 1 + category
int gCount = 1;
int gTile = 128;
bool gSortDirty = true;				// list mode: gShown must be sorted again by the table's columns
int gEditorIDs = 0;					// entries whose editor ID the game kept (most are dropped at run time)
bool gDirty = true;					// the shown list must be rebuilt before the grid is drawn
unsigned gBuildSerial = 0;			// bumped by every CatalogBuild made from here
unsigned gHaySerial = ~0u;			// the build the search strings and plugin list were made for
std::vector<std::string> gHay;		// per entry, lower case: name, editor ID, plugin, form ID in hex, '\n' between
std::vector<std::string> gPlugins;	// every plugin in the catalog, sorted
std::vector<std::string> gWords;	// the search, split on spaces, lower case
std::vector<int> gShown;			// catalog indices that pass the search, plugin and tab
int gCounts[kTabCount] = {};		// matches per tab before the tab filter (for the tab labels)

// The search strings and plugin list are only redone when the catalog was rebuilt; the filter runs when something changed.
void Rebuild() {
	const std::vector<Entry>& cat = Catalog();
	if (gHaySerial != gBuildSerial) {
		gHaySerial = gBuildSerial;
		gHay.assign(cat.size(), std::string());
		gPlugins.clear();
		gEditorIDs = 0;
		for (const Entry& e : cat) if (!e.editorID.empty()) gEditorIDs++;
		for (size_t i = 0; i < cat.size(); i++) {
			const Entry& e = cat[i];
			char id[16];
			snprintf(id, sizeof(id), "%08x", e.formID);
			std::string& h = gHay[i];
			h = e.name;
			h += '\n';
			h += e.editorID;
			h += '\n';
			h += e.plugin;
			h += '\n';
			h += id;
			for (char& c : h) c = (char)tolower((unsigned char)c);
			gPlugins.push_back(e.plugin);
		}
		std::sort(gPlugins.begin(), gPlugins.end());
		gPlugins.erase(std::unique(gPlugins.begin(), gPlugins.end()), gPlugins.end());
		if (!gPlugin.empty() && !std::binary_search(gPlugins.begin(), gPlugins.end(), gPlugin)) gPlugin.clear();
	}

	// Words split on spaces; a leading "0x" on a word is dropped so a pasted form ID still matches.
	gWords.clear();
	const char* p = gSearch;
	while (*p) {
		while (*p == ' ') p++;
		std::string word;
		while (*p && *p != ' ') word += (char)tolower((unsigned char)*p++);
		if (word.size() > 2 && word[0] == '0' && word[1] == 'x') word.erase(0, 2);
		if (!word.empty()) gWords.push_back(word);
	}

	std::fill(gCounts, gCounts + kTabCount, 0);
	gShown.clear();
	for (size_t i = 0; i < cat.size(); i++) {
		const Entry& e = cat[i];
		if (!gPlugin.empty() && e.plugin != gPlugin) continue;
		bool match = true;
		for (const std::string& w : gWords) {
			if (gHay[i].find(w) == std::string::npos) { match = false; break; }
		}
		if (!match) continue;
		const int tab = 1 + (int)e.category;
		gCounts[0]++;
		if (tab < kTabCount) gCounts[tab]++;
		if (gTab == 0 || gTab == tab) gShown.push_back((int)i);
	}
	gDirty = false;
	gSortDirty = true;
}

// The name shown on tiles and in the list's first column: the editor ID in editor-ID mode when the game kept it, else the
// in-game name.
const std::string& DisplayName(const Entry& e) {
	return (Opt.editorNames && !e.editorID.empty()) ? e.editorID : e.name;
}
const std::string& OtherName(const Entry& e) {
	return Opt.editorNames ? e.name : e.editorID;
}
bool Equips(const Entry& e) { return Opt.autoEquip && (e.category == kCat_Weapon || e.category == kCat_Armor); }

// ---- what a click, a right-click and hovering do (tiles and list rows alike) ---------------------------------------------------

// A click is GMod's: items go straight into the inventory, NPCs and creatures appear in front of you
void Activate(const Entry& e) {
	if (IsItemCategory(e.category)) AddToInventory(e, gCount);
	else Spawn(e, gCount);
}

// The right-click menu of the last item drawn.
void EntryMenu(const Entry& e) {
	if (!ImGui::BeginPopupContextItem("##entrymenu")) return;
	if (IsItemCategory(e.category)) {
		if (ImGui::MenuItem("Add 1 to inventory")) AddToInventory(e, 1);
		if (ImGui::MenuItem("Add 5 to inventory")) AddToInventory(e, 5);
		if (ImGui::MenuItem("Add 10 to inventory")) AddToInventory(e, 10);
		if (ImGui::MenuItem("Drop in front of me")) Spawn(e, gCount);
	} else {
		if (ImGui::MenuItem("Spawn 1")) Spawn(e, 1);
		if (ImGui::MenuItem("Spawn 5")) Spawn(e, 5);
		if (ImGui::MenuItem("Spawn 10")) Spawn(e, 10);
		ImGui::Separator();
		if (ImGui::MenuItem("Spawn with tool gun (left click to place)")) ToolArm(e);
	}
	ImGui::Separator();
	if (ImGui::MenuItem("Copy form ID")) {
		char id[16];
		snprintf(id, sizeof(id), "%08X", e.formID);
		ImGui::SetClipboardText(id);
	}
	if (!e.editorID.empty() && ImGui::MenuItem("Copy editor ID")) ImGui::SetClipboardText(e.editorID.c_str());
	ImGui::EndPopup();
}

void EntryTooltip(const Entry& e) {
	ImGui::BeginTooltip();
	ImGui::Text("%s", e.name.c_str());
	ImGui::Text("Editor ID: %s", e.editorID.empty() ? "(not kept by the game)" : e.editorID.c_str());
	ImGui::Text("Form ID: %08X", e.formID);
	ImGui::Text("Plugin: %s", e.plugin.c_str());
	if (!e.model.empty()) ImGui::Text("Model: %s", e.model.c_str());
	ImGui::TextDisabled(Equips(e) ? "Click: add to inventory and equip    Right-click: more"
		: IsItemCategory(e.category) ? "Click: add to inventory    Right-click: more" : "Click: spawn in front of you    Right-click: tool gun");
	ImGui::EndTooltip();
}

// ---- tiles ------------------------------------------------------------------------------------------------------------------

void DrawTile(int idx, float tile, float textH) {
	const Entry& e = Catalog()[idx];
	ImGui::PushID(idx);
	ImGui::BeginGroup();

	// The picture when there is one, else a plain button of the same size with the category name on it.
	void* tex = Thumbnail(e);
	bool clicked;
	if (tex) {
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));	// so the button is exactly tile x tile
		clicked = ImGui::ImageButton("##pic", tex, ImVec2(tile, tile));
		ImGui::PopStyleVar();
	} else {
		clicked = ImGui::Button(kCatNames[(int)e.category], ImVec2(tile, tile));
	}
	const bool hovered = ImGui::IsItemHovered();
	if (clicked) Activate(e);
	EntryMenu(e);
	if (hovered) EntryTooltip(e);

	// The name under the tile, clipped to the tile's width (text is drawn directly so it cannot wrap or grow the row).
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(tile, textH));
	dl->PushClipRect(p, ImVec2(p.x + tile, p.y + textH), true);
	dl->AddText(p, ImGui::GetColorU32(ImGuiCol_Text), DisplayName(e).c_str());
	dl->PopClipRect();

	ImGui::EndGroup();
	ImGui::PopID();
}

// ---- window -----------------------------------------------------------------------------------------------------------------

// The Options popup, opened by the "Options" button at the right end of the tab row.
void DrawOptionsMenu() {
	if (ImGui::BeginPopup("##spawnoptions")) {
		ImGui::SeparatorText("Menu key");
		const std::string change = "Change menu key (now " + KeyName(Opt.menuKey) + ")...";
		if (ImGui::MenuItem(change.c_str())) RebindingKey = true;
		if (Opt.menuKey != 'L' && ImGui::MenuItem("Back to L")) {
			Opt.menuKey = 'L';
			OptionsSave();
		}

		ImGui::SeparatorText("View");
		if (ImGui::MenuItem("Pictures", nullptr, !Opt.listMode)) { Opt.listMode = false; OptionsSave(); }
		if (ImGui::MenuItem("List (GECK style)", nullptr, Opt.listMode)) { Opt.listMode = true; OptionsSave(); gSortDirty = true; }

		ImGui::SeparatorText("Names");
		if (ImGui::MenuItem("In-game names", nullptr, !Opt.editorNames)) { Opt.editorNames = false; OptionsSave(); gSortDirty = true; }
		if (ImGui::MenuItem("Editor IDs", nullptr, Opt.editorNames)) { Opt.editorNames = true; OptionsSave(); gSortDirty = true; }
		ImGui::TextDisabled("The game kept editor IDs for %d of %d entries;", gEditorIDs, (int)Catalog().size());
		ImGui::TextDisabled("the rest show their in-game name.");

		ImGui::SeparatorText("Inventory");
		if (ImGui::MenuItem("Equip weapons and armor on add", nullptr, Opt.autoEquip)) { Opt.autoEquip = !Opt.autoEquip; OptionsSave(); }
		ImGui::EndPopup();
	}
}

// List mode: gShown sorted by the table's sort columns (none = catalog order: category, then name).
enum ListColumn { kCol_Name, kCol_Other, kCol_FormID, kCol_Type, kCol_Plugin, kCol_Model, kCol_Count };

void SortShown(const ImGuiTableSortSpecs* specs) {
	if (!specs || specs->SpecsCount == 0) {
		std::sort(gShown.begin(), gShown.end());	// catalog indices are already in catalog order
		return;
	}
	const std::vector<Entry>& cat = Catalog();
	std::stable_sort(gShown.begin(), gShown.end(), [&](int ia, int ib) {
		const Entry& a = cat[ia];
		const Entry& b = cat[ib];
		for (int s = 0; s < specs->SpecsCount; s++) {
			const ImGuiTableColumnSortSpecs& spec = specs->Specs[s];
			int d = 0;
			switch (spec.ColumnUserID) {
			case kCol_Name:		d = _stricmp(DisplayName(a).c_str(), DisplayName(b).c_str()); break;
			case kCol_Other:	d = _stricmp(OtherName(a).c_str(), OtherName(b).c_str()); break;
			case kCol_FormID:	d = a.formID < b.formID ? -1 : a.formID > b.formID ? 1 : 0; break;
			case kCol_Type:		d = (int)a.category - (int)b.category; break;
			case kCol_Plugin:	d = _stricmp(a.plugin.c_str(), b.plugin.c_str()); break;
			case kCol_Model:	d = _stricmp(a.model.c_str(), b.model.c_str()); break;
			}
			if (d != 0) return spec.SortDirection == ImGuiSortDirection_Ascending ? d < 0 : d > 0;
		}
		return ia < ib;
	});
}

void DrawList() {
	const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_SortTristate |
		ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
		ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	if (!ImGui::BeginTable("##spawnlist", kCol_Count, flags)) return;
	ImGui::TableSetupScrollFreeze(1, 1);
	ImGui::TableSetupColumn(Opt.editorNames ? "Editor ID###c0" : "Name###c0", ImGuiTableColumnFlags_NoHide, 260.0f, kCol_Name);
	ImGui::TableSetupColumn(Opt.editorNames ? "Name###c1" : "Editor ID###c1", 0, 200.0f, kCol_Other);
	ImGui::TableSetupColumn("Form ID", 0, 80.0f, kCol_FormID);
	ImGui::TableSetupColumn("Type", 0, 80.0f, kCol_Type);
	ImGui::TableSetupColumn("Plugin", 0, 170.0f, kCol_Plugin);
	ImGui::TableSetupColumn("Model", 0, 320.0f, kCol_Model);
	ImGui::TableHeadersRow();

	if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
		if (specs->SpecsDirty || gSortDirty) {
			SortShown(specs);
			specs->SpecsDirty = false;
			gSortDirty = false;
		}
	}

	const std::vector<Entry>& cat = Catalog();
	ImGuiListClipper clip;
	clip.Begin((int)gShown.size());
	while (clip.Step()) {
		for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
			const int idx = gShown[row];
			const Entry& e = cat[idx];
			ImGui::PushID(idx);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(kCol_Name);
			const bool clicked = ImGui::Selectable(DisplayName(e).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
			const bool hovered = ImGui::IsItemHovered();
			if (clicked) Activate(e);
			EntryMenu(e);
			if (hovered) EntryTooltip(e);
			if (ImGui::TableSetColumnIndex(kCol_Other)) ImGui::TextUnformatted(OtherName(e).c_str());
			if (ImGui::TableSetColumnIndex(kCol_FormID)) ImGui::Text("%08X", e.formID);
			if (ImGui::TableSetColumnIndex(kCol_Type)) ImGui::TextUnformatted(kCatNames[(int)e.category]);
			if (ImGui::TableSetColumnIndex(kCol_Plugin)) ImGui::TextUnformatted(e.plugin.c_str());
			if (ImGui::TableSetColumnIndex(kCol_Model)) ImGui::TextUnformatted(e.model.c_str());
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
}

void DrawBody() {
	if (RebindingKey) {
		ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "Press the new menu key (Esc keeps %s)", KeyName(Opt.menuKey).c_str());
		ImGui::Separator();
	}

	// The armed tool gun, with the way to put it away
	if (const Entry* tool = ToolEntry()) {
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Tool gun: %s", tool->name.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Stop")) ToolStop();
		ImGui::Separator();
	}

	// Top row: search, plugin, count, tile size, refresh, shown / total.
	ImGui::SetNextItemWidth(260.0f);
	if (ImGui::InputTextWithHint("##spawnsearch", "Search name, editor ID, plugin or form ID", gSearch, sizeof(gSearch))) gDirty = true;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(170.0f);
	if (ImGui::BeginCombo("##spawnplugin", gPlugin.empty() ? "All plugins" : gPlugin.c_str())) {
		if (ImGui::Selectable("All plugins", gPlugin.empty())) { gPlugin.clear(); gDirty = true; }
		for (const std::string& p : gPlugins) {
			if (ImGui::Selectable(p.c_str(), p == gPlugin)) { gPlugin = p; gDirty = true; }
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(70.0f);
	ImGui::InputInt("Count", &gCount);
	if (gCount < 1) gCount = 1;
	if (gCount > 100) gCount = 100;
	ImGui::SameLine();
	if (ImGui::Checkbox("Equip on add", &Opt.autoEquip)) OptionsSave();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Weapons and armor you add to the inventory are put on right away");
	ImGui::SameLine();
	if (!Opt.listMode) {
		ImGui::SetNextItemWidth(150.0f);
		ImGui::SliderInt("Tile size", &gTile, 64, 256);
		ImGui::SameLine();
	}
	if (ImGui::Button("Refresh")) {
		CatalogBuild();
		gBuildSerial++;
		gDirty = true;
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%d shown of %d", (int)gShown.size(), (int)Catalog().size());

	// Tabs. A change of tab only takes effect on the rebuild below.
	if (ImGui::BeginTabBar("##spawncats")) {
		int picked = -1;
		for (int t = 0; t < kTabCount; t++) {
			char label[48];
			if (t == 0) snprintf(label, sizeof(label), "All (%d)###spawntab0", gCounts[0]);
			else snprintf(label, sizeof(label), "%s (%d)###spawntab%d", kCatNames[t - 1], gCounts[t], t);
			if (ImGui::BeginTabItem(label)) {
				picked = t;
				ImGui::EndTabItem();
			}
		}
		// Not spawnable: the cells to travel to and the weathers (SpawnPlaces.cpp)
		char label[48];
		snprintf(label, sizeof(label), "Locations (%d)###spawntabplaces", (int)Places().size());
		if (ImGui::BeginTabItem(label)) {
			picked = kTabPlaces;
			ImGui::EndTabItem();
		}
		snprintf(label, sizeof(label), "Weather (%d)###spawntabweather", (int)Weathers().size());
		if (ImGui::BeginTabItem(label)) {
			picked = kTabWeather;
			ImGui::EndTabItem();
		}
		// Options sits at the right end of the tab row, under Tile size
		if (ImGui::TabItemButton("Options", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) ImGui::OpenPopup("##spawnoptions");
		DrawOptionsMenu();
		ImGui::EndTabBar();
		if (picked >= 0 && picked != gTab) {
			gTab = picked;
			gDirty = true;
		}
	}

	if (gDirty) Rebuild();

	if (gTab == kTabPlaces) {
		DrawPlaces(gWords, gPlugin);
		return;
	}
	if (gTab == kTabWeather) {
		DrawWeathers(gWords, gPlugin);
		return;
	}

	if (Catalog().empty()) ImGui::TextDisabled("No forms listed yet. Press Refresh once a save is loaded.");
	else if (gShown.empty()) ImGui::TextDisabled("Nothing matches the search.");

	if (Opt.listMode) {
		DrawList();
		return;
	}

	// Grid: as many tiles per row as fit; the clipper only walks the rows on screen.
	ImGui::BeginChild("##spawngrid", ImVec2(0.0f, 0.0f), false);
	const ImGuiStyle& style = ImGui::GetStyle();
	const float tile = (float)gTile;
	const float textH = ImGui::GetTextLineHeight();
	const float avail = ImGui::GetContentRegionAvail().x;
	int cols = (int)((avail + style.ItemSpacing.x) / (tile + style.ItemSpacing.x));
	if (cols < 1) cols = 1;
	const int n = (int)gShown.size();
	const int rows = (n + cols - 1) / cols;

	// A row is the tile, the name line and the spacing a line break adds around the group.
	ImGuiListClipper clip;
	clip.Begin(rows, tile + textH + 2.0f * style.ItemSpacing.y);
	while (clip.Step()) {
		for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
			for (int c = 0; c < cols; c++) {
				const int k = r * cols + c;
				if (k >= n) break;
				if (c > 0) ImGui::SameLine();
				DrawTile(gShown[k], tile, textH);
			}
		}
	}
	clip.End();
	ImGui::EndChild();
}

}	// namespace

// Spawns count of the entry in front of the player, through the deferred console line.
void Spawn(const Entry& entry, int count) {
	char line[64];
	snprintf(line, sizeof(line), "player.placeatme %08X %d", entry.formID, count);
	ConsoleLater(line);
	Logger::Log("SpawnMenu: [Spawn] placeatme %08X x%d '%s' (%s)", entry.formID, count, entry.name.c_str(), entry.plugin.c_str());
}

void AddToInventory(const Entry& entry, int count) {
	char line[64];
	snprintf(line, sizeof(line), "player.additem %08X %d", entry.formID, count);
	ConsoleLater(line);
	Logger::Log("SpawnMenu: [Spawn] additem %08X x%d '%s' (%s)", entry.formID, count, entry.name.c_str(), entry.plugin.c_str());
	// "Equip on add": the equip line runs right after the additem line, on the same game loop (the item is in the inventory by then)
	if (Equips(entry)) {
		snprintf(line, sizeof(line), "player.equipitem %08X", entry.formID);
		ConsoleLater(line);
		Logger::Log("SpawnMenu: [Spawn] equipitem %08X '%s'", entry.formID, entry.name.c_str());
	}
}

void DrawWindow() {
	if (!Open) {
		// Closed. The window closed by its x this frame has already drawn its pictures, so they are freed on the next call
		// (the frame before this one has been rendered by now).
		if (gWasOpen) {
			gWasOpen = false;
			ThumbRelease();
		}
		return;
	}
	if (!gWasOpen) {
		gWasOpen = true;
		CatalogBuild();
		gBuildSerial++;
		gDirty = true;
	}
	ThumbFrame();

	ImGui::SetNextWindowSize(ImVec2(900.0f, 620.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Spawn menu###SpawnMenu", &Open)) DrawBody();
	ImGui::End();
}

}	// namespace SpawnMenu
