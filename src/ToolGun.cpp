#include "SpawnMenu.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>

// The tool gun: an NPC or creature picked in the window ("Spawn with tool gun") is placed where the view ray from the camera
// first hits the world, once per left click, until it is stopped. The click comes from Overlay.cpp (which also keeps the click
// from the player's weapon); the pick and the placement run on the next game loop, outside rendering.
//
// The ray is the game's own scene pick, NiPick (Gamebryo 2.2), against the triangles of the world root (TESMain::spWorldRoot),
// skipping the player's own 3D and the sky. Placement is the game's PlaceAtMe (what the console command runs) on the player,
// then SetPos to the hit point and a heading that faces the camera.
// Addresses: WallSoGB's map; NiPick's layout: the 1.4.0.525 decomp (nipick_decls.h, sizes checked there).

namespace SpawnMenu {
namespace {

const UInt32 kWorldRoot			= 0x011DEB7C;	// TESMain::spWorldRoot (a SceneGraph; its camera at +0xAC)
const UInt32 kNiPickCtor		= 0x00E98F20;	// NiPick::NiPick(results size, grow by)
const UInt32 kNiPickDtor		= 0x00E98FA0;
const UInt32 kNiPickPerform		= 0x00E98E20;	// NiPick::PickObjects(origin, direction, append)
const UInt32 kNiPickSetTarget	= 0x00705FC0;	// NiPick::SetTarget(root): holds a reference to it
const UInt32 kLookupForm		= 0x004839C0;	// TESForm::GetFormByNumericID
const UInt32 kPlaceAtMe			= 0x005C4B30;	// Script::PlaceAtMeFunctionBase(ref, form, count, distance, direction, health)
const UInt32 kSetAngle			= 0x00575700;	// TESObjectREFR: rotation (radians) by value
const UInt32 kSetPos			= 0x00575830;	// TESObjectREFR::SetLocationOnReference(NiPoint3*)

const float kMinHit = 20.0f;		// closer than this is the camera's own surroundings
const float kMaxHit = 30000.0f;		// farther is the horizon: nothing to stand on

Entry gEntry;
bool gArmed = false;
bool gClickWaiting = false;
char gMessage[160] = "";
double gMessageUntil = 0.0;

double Now() { return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0; }

void Say(const char* text) {
	snprintf(gMessage, sizeof(gMessage), "%s", text);
	gMessageUntil = Now() + 2.5;
}

struct Hit {
	NiPoint3 point;
	float heading;		// radians, turning the placed actor to face the camera
	float distance;
};

// True when object is the node or one of its children (walked up through the parents).
bool Under(NiAVObject* object, NiAVObject* node) {
	if (!node) return false;
	for (int depth = 0; object && depth < 64; depth++, object = (NiAVObject*)object->m_parent)
		if (object == node) return true;
	return false;
}

// The view ray's first hit. 0 = hit, 1 = nothing to pick in, 2 = nothing hit, -1 = the game threw (plain data only: SEH).
int PickSEH(Hit* out) {
	__try {
		UInt8* root = *(UInt8**)kWorldRoot;
		NiAVObject* camera = root ? *(NiAVObject**)(root + 0xAC) : nullptr;
		if (!camera) return 1;
		const NiTransform& view = camera->m_worldTransform;
		const NiPoint3 origin = view.pos;
		NiPoint3 dir = { view.rot.data[0][0], view.rot.data[1][0], view.rot.data[2][0] };	// a Gamebryo camera looks down its X axis
		const float length = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		if (length < 1e-4f) return 1;
		dir.x /= length; dir.y /= length; dir.z /= length;

		TESObjectREFR* player = (TESObjectREFR*)SpawnGame::Player();
		NiAVObject* playerNow = player ? (NiAVObject*)player->GetNiNode() : nullptr;	// first or third person, as shown
		NiAVObject* playerBody = player && player->renderData ? (NiAVObject*)player->renderData->niNode : nullptr;
		Sky* sky = Sky::Get();
		NiAVObject* skyRoot = sky ? (NiAVObject*)sky->nodeSkyRoot : nullptr;

		alignas(16) UInt8 pick[64] = {};
		ThisCall(kNiPickCtor, pick, 0u, 8u);
		*(UInt32*)(pick + 0x00) = 0;	// FIND_ALL
		*(UInt32*)(pick + 0x04) = 0;	// SORT (nearest first)
		*(UInt32*)(pick + 0x08) = 1;	// TRIANGLE_INTERSECT
		*(UInt32*)(pick + 0x0C) = 1;	// WORLD_COORDINATES
		pick[0x10] = 1;					// front faces only
		pick[0x11] = 1;					// skip what the game has culled
		ThisCall(kNiPickSetTarget, pick, (void*)root);
		const bool any = (ThisCall(kNiPickPerform, pick, &origin, &dir, false) & 0xFF) != 0;

		int result = 2;
		float best = kMaxHit;
		UInt8** records = *(UInt8***)(pick + 0x18 + 0x04);		// Results::m_pBase
		const UInt32 count = *(UInt32*)(pick + 0x18 + 0x10);	// Results::m_uiValidCount
		for (UInt32 i = 0; any && records && i < count; i++) {
			UInt8* record = records[i];
			if (!record) continue;
			NiAVObject* object = *(NiAVObject**)record;
			const float distance = *(float*)(record + 0x14);
			if (distance < kMinHit || distance >= best) continue;
			if (Under(object, playerNow) || Under(object, playerBody) || Under(object, skyRoot)) continue;
			best = distance;
			out->point = *(NiPoint3*)(record + 0x08);
			out->distance = distance;
			result = 0;
		}
		ThisCall(kNiPickSetTarget, pick, (void*)nullptr);
		ThisCall(kNiPickDtor, pick);
		if (result == 0) out->heading = atan2f(origin.x - out->point.x, origin.y - out->point.y);	// heading 0 = north, clockwise
		return result;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

// Places one of formID at the hit. 0 = placed, 1 = no such form or no player, 2 = PlaceAtMe gave nothing, -1 = the game threw.
int PlaceSEH(UInt32 formID, const Hit* hit) {
	__try {
		void* player = SpawnGame::Player();
		void* form = (void*)CdeclCall<UInt32>(kLookupForm, formID);
		if (!player || !form) return 1;
		void* placed = (void*)CdeclCall<UInt32>(kPlaceAtMe, player, form, 1, 0, 0, 1.0f);
		if (!placed) return 2;
		const NiPoint3 angles = { 0.0f, 0.0f, hit->heading };
		NiPoint3 at = hit->point;
		at.z += 4.0f;	// a little above the surface, so it settles onto it rather than into it
		ThisCall(kSetAngle, placed, angles);
		ThisCall(kSetPos, placed, &at);
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return -1;
	}
}

}	// namespace

void ToolArm(const Entry& entry) {
	gEntry = entry;
	gArmed = true;
	gClickWaiting = false;
	Open = false;	// GMod style: pick the thing, the menu gets out of the way
	char text[160];
	snprintf(text, sizeof(text), "Left click to place. %s, then Stop, to put the tool gun away.", KeyName(Opt.menuKey).c_str());
	Say(text);
	Logger::Log("Spawn menu: tool gun armed with %08X '%s' (%s)", entry.formID, entry.name.c_str(), entry.plugin.c_str());
}

void ToolStop() {
	if (!gArmed) return;
	gArmed = false;
	gClickWaiting = false;
	Logger::Log("Spawn menu: tool gun stopped");
}

const Entry* ToolEntry() { return gArmed ? &gEntry : nullptr; }

void ToolClick() {
	if (gArmed) gClickWaiting = true;
}

void ToolGameLoop() {
	if (!gArmed || !gClickWaiting) return;
	gClickWaiting = false;
	Hit hit = {};
	const int picked = PickSEH(&hit);
	if (picked != 0) {
		Say(picked == 2 ? "Nothing to stand on there (too far or only sky)." : "Could not trace the view ray.");
		if (picked < 0) Logger::Log("Spawn menu: tool gun: the game's pick threw (logged per click)");
		return;
	}
	const int placed = PlaceSEH(gEntry.formID, &hit);
	if (placed == 0) {
		char text[160];
		snprintf(text, sizeof(text), "Placed %s (%.0f units away).", gEntry.name.c_str(), hit.distance);
		Say(text);
		Logger::Log("Spawn menu: tool gun placed %08X at %.1f %.1f %.1f (%.0f units)", gEntry.formID, hit.point.x, hit.point.y, hit.point.z, hit.distance);
	}
	else {
		Say("The game would not place it.");
		Logger::Log("Spawn menu: tool gun could not place %08X (code %d)", gEntry.formID, placed);
	}
}

void ToolDrawHud() {
	if (!gArmed) return;
	const ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 48.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.55f);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	if (ImGui::Begin("##spawntoolhud", nullptr, flags)) {
		ImGui::Text("Tool gun: %s", gEntry.name.c_str());
		ImGui::TextDisabled("Left click: place where you look    %s: menu (Stop)", KeyName(Opt.menuKey).c_str());
		if (gMessage[0] && Now() < gMessageUntil) ImGui::TextUnformatted(gMessage);
	}
	ImGui::End();
}

}	// namespace SpawnMenu
