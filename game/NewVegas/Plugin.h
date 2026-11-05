#pragma once

// The parts of the xNVSE plugin interface NVR talks to. These structs are filled in by xNVSE and passed to the
// plugin, so their member order is the xNVSE ABI; the message and interface numbers are xNVSE's.

typedef UInt32 PluginHandle;	// opaque

class CommandInfo; class TESObjectREFR;	// Game.h

// IDs for PluginInterface::QueryInterface.
enum {
	kInterface_Console			= 1,	// NVSEConsoleInterface
	kInterface_Messaging		= 2,	// NVSEMessagingInterface
	kInterface_Data				= 7,	// NVSEDataInterface
	kInterface_PlayerControls	= 10,	// NVSETogglePlayerControlsInterface
};

// Handed to NVSEPlugin_Query / NVSEPlugin_Load.
class PluginInterface {	// 0x20 bytes
public:
	UInt32	Version, GameVersion, EditorVersion;	// xNVSE, runtime and editor versions
	UInt32	IsEditor;								// loaded by the GECK

	bool			(*RegisterCommand)(CommandInfo* command);	// gets the next free opcode
	void			(*SetOpcodeBase)(UInt32 firstOpcode);
	void*			(*QueryInterface)(UInt32 interfaceID);		// kInterface_*
	PluginHandle	(*GetPluginHandle)();
};

// Filled in by the plugin in NVSEPlugin_Query.
class PluginInfo {	// 0xC bytes
public:
	enum { kInfoVersion = 2 };

	UInt32		InfoVersion;	// kInfoVersion
	const char*	Name;			// the plugin's name for xNVSE
	UInt32		Version;		// the plugin's own version
};

struct NVSEMessagingInterface {	// 0xC bytes
	struct Message {	// 0x10 bytes
		const char*	sender;			// plugin name, "NVSE" for xNVSE's own
		UInt32		type, dataLen;	// kMessage_* (from "NVSE"), payload size
		void*		data;			// payload
	};
	typedef void (*EventCallback)(Message* msg);

	// Message types sent by "NVSE" (numbering fixed by xNVSE).
	enum {
		kMessage_PostLoad			= 0,	// all plugins loaded
		kMessage_ExitGame			= 1,	// quit from the main or pause menu
		kMessage_ExitToMainMenu		= 2,	// back to the main menu
		kMessage_SaveGame			= 4,	// data: save path
		kMessage_PreLoadGame		= 6,	// data: save path, before the game reads it
		kMessage_ExitGame_Console	= 7,	// qqq
		kMessage_PostLoadGame		= 8,	// data: bool, the load succeeded
		kMessage_NewGame			= 14,	// a new game starts
		kMessage_DeferredInit		= 18,	// the game has finished starting up
		kMessage_MainGameLoop		= 20,	// every game loop
	};

	UInt32	version;	// 4
	bool	(*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
	bool	(*Dispatch)(PluginHandle sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};

struct NVSEDataInterface {	// 0x14 bytes
	enum { kNVSEData_DIHookControl = 1 };	// GetSingleton IDs

	UInt32	version;	// 3
	void*	(*GetSingleton)(UInt32 id);		// kNVSEData_*
	void*	(*GetFunc)(UInt32 id);
	void*	(*GetData)(UInt32 id);
	void	(*ClearScriptDataCache)();		// xNVSE 6.x
};

struct NVSEConsoleInterface {	// 0x8 bytes
	UInt32	version;	// 1
	void	(*RunScriptLine)(const char* line, TESObjectREFR* target);	// as if typed into the console
};

// TogglePlayerControlsAlt (flags are xNVSE's DisabledControlsFlags, "how" its CheckDisabledHow).
struct NVSETogglePlayerControlsInterface {	// 0x10 bytes
	void	(__fastcall* DisablePlayerControlsAlt)(UInt32 flags, const char* mod);
	void	(__fastcall* EnablePlayerControlsAlt)(UInt32 flags, const char* mod);
	bool	(__cdecl* GetPlayerControlsDisabledAlt)(UInt8 how, UInt32 flags, const char* mod);
	UInt32	(__fastcall* GetDisabledPlayerControls)(UInt8 how, const char* mod);
};
