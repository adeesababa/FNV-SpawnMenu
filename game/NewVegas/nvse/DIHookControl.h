#pragma once

// xNVSE's DirectInput key filter, as NVR uses it to keep the game from seeing input while the overlay is open.
// The object belongs to xNVSE (NVSEDataInterface::GetSingleton(kNVSEData_DIHookControl)); NVR only writes the
// two "disabled" flags of a key, so only the layout up to the key table is mirrored here.
//
// Key codes: 0..255 DirectInput scan codes, then 8 mouse buttons (256..263), then wheel up / wheel down (264, 265).

enum {
	kMacro_MouseButtonOffset = 0x100, kMacro_MouseWheelOffset = 0x108, kMaxMacros = 0x10A,
};

class DIHookControl {	// 0x74C bytes
public:
	// xNVSE derives it from a singleton base with a virtual destructor, so there is a vtable pointer at +0.
	virtual ~DIHookControl() = default;

	enum {
		kDisable_User	= 0x1,		// set by plugins like NVR
		kDisable_Script	= 0x2,		// set by scripts (DisableKey)
		kDisable_All	= kDisable_User | kDisable_Script,	// both
	};

	// Marks `keycode` as disabled (or enabled again) for the given sources; mask 0 means both.
	void SetKeyDisableState(UInt32 keycode, bool bDisable, UInt32 mask = 0) {
		if (keycode >= kMaxMacros) return;		// not a key code xNVSE knows
		const UInt32 sources = mask ? mask : (UInt32)kDisable_All;
		KeyInfo& key = m_keys[keycode];
		if (sources & kDisable_User) key.userDisable = bDisable;
		if (sources & kDisable_Script) key.scriptDisable = bDisable;
	}

private:
	// One entry per key code: 7 bools, packed (xNVSE's KeyInfo).
	struct KeyInfo {	// 0x7 bytes
		bool rawState, gameState, insertedState;
		bool hold, tap;
		bool userDisable, scriptDisable;
	};
	KeyInfo m_keys[kMaxMacros];	// +0x04
};
