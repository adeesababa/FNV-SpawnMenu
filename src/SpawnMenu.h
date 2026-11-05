#pragma once
#include <string>
#include <vector>

// 2026-11-04 Spawn menu, its own xNVSE plugin (SpawnMenu.dll): every spawnable form the loaded plugins hold, by category, with a
// picture of each; click to place one in front of the player. L opens and closes it (Overlay.cpp); nothing runs while it is closed.
//
// Four parts, one file each:
//   SpawnCatalog.cpp  the list of forms (names, plugin, model and icon paths)   - Catalog(), CatalogBuild()
//   SpawnMenuUI.cpp   the window, its tabs, search and grid, and spawning        - Open, DrawWindow(), Spawn()
//   SpawnThumbs.cpp   a picture per entry: disk cache, Pip-Boy icon fallback      - Thumbnail(), ThumbFrame(), ThumbRelease()
//   SpawnRender.cpp   draws one form's model into a picture (the bake)           - RenderBegin(), RenderEntry(), RenderEnd()
//   ToolGun.cpp       left click places the armed NPC or creature where you look - ToolArm(), ToolStop(), ToolGameLoop()
//   Overlay.cpp       the plugin: xNVSE entry, the Present hook, ImGui, the L key and input blocking
// Threads: everything here runs on the game's main thread, inside the device's Present (Overlay.cpp), unless a part says otherwise.
namespace SpawnMenu {

	enum Category {
		kCat_Weapon, kCat_Armor, kCat_Aid, kCat_Ammo, kCat_Misc, kCat_NPC, kCat_Creature, kCat_Count
	};

	struct Entry {
		unsigned int	formID;		// the form's run-time ID (load-order byte included)
		unsigned char	formType;	// TESForm::formType
		Category		category;
		std::string		name;		// full (in-game) name; the editor ID or the form ID in hex when there is none
		std::string		editorID;	// often empty at run time (the game drops most editor IDs)
		std::string		plugin;		// the file that last changed the form ("FalloutNV.esm", "Unknown" when not found)
		std::string		model;		// the model path as the form stores it, relative to Data\Meshes ("" = none)
		std::string		icon;		// the inventory icon path, relative to Data\Textures ("" = none)
	};

	// ---- SpawnCatalog.cpp ----
	const std::vector<Entry>&	Catalog();			// every entry, sorted by category then name; empty until CatalogBuild ran
	void						CatalogBuild();		// (re)reads the loaded forms; cheap enough to run when the window opens

	// ---- SpawnCatalog.cpp: places and weathers (the Locations and Weather tabs) ----
	struct Place {
		unsigned int	formID;
		std::string		editorID;		// what coc takes ("" = the game did not keep it: cannot travel there)
		std::string		name;			// the cell's full name ("" for most exterior cells)
		std::string		plugin;
		bool			interior;
		std::string		world;			// exterior: the worldspace's name, else its editor ID
		std::string		worldEditorID;	// exterior: what cow takes
		int				x, y;			// exterior: the cell grid
		std::string		hay;			// lower case, for the search
	};
	struct WeatherEntry {
		unsigned int	formID;
		std::string		editorID;		// from the game, else read from the plugin files (the game drops weathers' editor IDs)
		std::string		plugin;
		unsigned char	kind;			// TESWeather::weatherType classification flags
		std::string		hay;
	};
	const std::vector<Place>&			Places();
	const std::vector<WeatherEntry>&	Weathers();
	void								PlacesBuild();	// from CatalogBuild

	// ---- SpawnPlaces.cpp ----
	// The two tabs' bodies; words = the search split on spaces (lower case), plugin = "" for all.
	void DrawPlaces(const std::vector<std::string>& words, const std::string& plugin);
	void DrawWeathers(const std::vector<std::string>& words, const std::string& plugin);

	// ---- SpawnMenuUI.cpp ----
	void			ConsoleLine(const char* line);	// runs a console line on the next game loop
	// The window's Options menu, kept in Data\SpawnMenu\SpawnMenu.ini (read at plugin load, written on every change).
	struct Options {
		int		menuKey = 'L';		// virtual-key code that opens and closes the window
		bool	listMode = false;	// a GECK-style table (name, editor ID, form ID, type, plugin, model) instead of pictures
		bool	editorNames = false;// show editor IDs instead of in-game names (falls back to the name where the ID is unknown)
		bool	autoEquip = false;	// "Equip on add": weapons and armor added to the inventory are also equipped
	};
	extern Options	Opt;
	void			OptionsLoad();
	void			OptionsSave();
	extern bool		RebindingKey;					// Options > Change menu key: Overlay takes the next key press as the menu key
	std::string		KeyName(int vk);				// "L", "F7", "Home"...

	extern bool		Open;							// the menu key flips it (Overlay.cpp); the window's x clears it
	void			DrawWindow();					// from MenuTools::DrawWindows, every menu frame (returns at once when closed)
	void			Spawn(const Entry& entry, int count);	// places count of it in front of the player (deferred console line)
	void			AddToInventory(const Entry& entry, int count);	// count of it into the player's inventory (deferred console line)

	// ---- ToolGun.cpp ----
	void			ToolArm(const Entry& entry);	// closes the window; each left click in the game places one where the view ray hits
	void			ToolStop();
	const Entry*	ToolEntry();					// the armed entry; nullptr while the tool gun is put away
	void			ToolClick();					// Overlay: a left click in game mode while armed (placed on the next game loop)
	void			ToolGameLoop();					// Overlay: every game loop, outside rendering
	void			ToolDrawHud();					// Overlay: inside an ImGui frame while armed and the window is closed

	// ---- SpawnThumbs.cpp ----
	// The picture for an entry, as an ImTextureID-ready IDirect3DTexture9* cast to void*; nullptr while it is not ready (the call
	// queues it). Order: rendered picture from the disk cache, else from the bake queue, else the Pip-Boy icon, else nullptr.
	void*			Thumbnail(const Entry& entry);
	void			ThumbFrame();					// once per frame the window is open: loads and bakes a few queued pictures
	void			ThumbRelease();					// frees every texture (window closed, device lost)
	std::string		ThumbCacheFolder();				// Data\VegasShaders\SpawnThumbs\ (created on first use)

	// ---- SpawnRender.cpp ----
	static const int kThumbSize = 256;				// rendered pictures are kThumbSize x kThumbSize
	bool			RenderBegin();					// once per bake batch: makes its render target etc.; false = cannot render at all
	// Draws the entry's model into a new kThumbSize texture (D3DPOOL_MANAGED or a copy that survives device resets is the caller's
	// choice: this returns a D3DPOOL_DEFAULT render-target texture the caller saves to disk and then releases). nullptr = not done:
	// *retry true means "model still loading, ask again next frame", false means "give up on this one".
	void*			RenderEntry(const Entry& entry, bool* retry);
	void			RenderEnd();					// after the batch: puts back every device state RenderEntry touched
	void			RenderRelease();				// frees its D3DPOOL_DEFAULT depth surface (from ThumbRelease: window closed, device lost)
}
