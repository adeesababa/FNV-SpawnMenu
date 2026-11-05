#pragma once

// Bethesda side of Fallout: New Vegas (forms, references, sky, menus, input), as far as NVR reads or calls it.
//
// The layout facts (offsets, sizes, virtual slots, function and global addresses) come from community reverse
// engineering, the NVSE / xNVSE projects and WallSoGB's symbol map, and are restated here in NVR's own form. See
// GameNi.h for the conventions (padXX, vslot_N); nvse/LayoutCheck.h checks every size and named offset.

class TESForm; class TESObjectREFR; class TESObjectCELL; class TESWaterForm; class TESWorldSpace;
class TESRegion; class TESObjectWEAP; class TESGlobal; class BGSLightingTemplate; class Script;
class ScriptEventList; class SkinInfo; class BaseProcess; class Menu; class PlayerCharacter; class MagicItem;
class INISettingCollection; class TESSaveLoadGame; class DetectorWindow; class SkyShader;

// ---------------------------------------------------------------------------------------------------------------------
// Script commands (the NVSE command table)
// ---------------------------------------------------------------------------------------------------------------------

class CommandParam {	// 0xC bytes
public:
	enum ParamType {	// the ones NVR's commands take
		kParamType_String = 0x00, kParamType_Integer = 0x01, kParamType_Float = 0x02,
		kParamType_ObjectRef = 0x04,	// a reference
	};

	const char*	typeStr;	// +0x00
	UInt32		typeID;		// +0x04 ParamType
	UInt32		isOptional;	// +0x08
};

class CommandArgs {	// 0x20 bytes
public:
	CommandParam*		paramInfo;		// +0x00
	void*				arg1;			// +0x04
	TESObjectREFR*		thisObj;		// +0x08
	TESObjectREFR*		Obj;			// +0x0C
	Script*				scriptObj;		// +0x10
	ScriptEventList*	eventList;		// +0x14
	double*				result;			// +0x18
	UInt32*				opcodeOffset;	// +0x1C
};

class CommandInfo {	// 0x28 bytes
public:
	const char*		longName;		// +0x00
	const char*		shortName;		// +0x04
	UInt32			opcode;			// +0x08
	const char*		helpText;		// +0x0C
	UInt16			needsParent;	// +0x10
	UInt16			numParams;		// +0x12
	CommandParam*	params;			// +0x14
	void*			execute;		// +0x18
	void*			parse;			// +0x1C
	void*			eval;			// +0x20
	UInt32			flags;			// +0x24
};

// ---------------------------------------------------------------------------------------------------------------------
// Small game types
// ---------------------------------------------------------------------------------------------------------------------

// The game's singly linked list; the head entry is stored inline.
template <typename T> class TList {
public:
	struct Entry {	// 0x4 bytes
		T*		item;	// +0x00
		Entry*	next;	// +0x04
	};

	Entry First;	// +0x00
};

class RGBA {	// 0x4 bytes
public:
	UInt8 r, g, b, a;

	D3DXVECTOR4 toD3DXVECTOR4() { return D3DXVECTOR4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
};

class BSString {	// 0x8 bytes
public:
	const char*	m_data;		// +0x00
	UInt16		m_dataLen;	// +0x04
	UInt16		m_bufLen;	// +0x06

	bool Set(const char* text) { return ThisCall(0x004037F0, this, text, 0); }	// copies the text
};

class BSFixedString {	// 0x4 bytes
public:
	const char* m_data;		// +0x00
};

template <typename Data>
class DNode {
public:
	DNode<Data>*	next;	// +0x00
	DNode<Data>*	prev;	// +0x04
	Data*			data;	// +0x08
};

template <typename Data>
class DList {
public:
	DNode<Data>*	first;	// +0x00
	DNode<Data>*	last;	// +0x04
	UInt32			count;	// +0x08
};

// ---------------------------------------------------------------------------------------------------------------------
// Extra data
// ---------------------------------------------------------------------------------------------------------------------

class BSExtraData {	// 0xC bytes
public:
	enum ExtraDataType {	// BSExtraData::type
		kExtraData_RefractionProperty = 0x48, kExtraData_DismemberedLimbs = 0x5F,
	};

	NVR_VSLOT(0);
	UInt8			type;		// +0x04 ExtraDataType
	UInt8			pad05[3];	// +0x05
	BSExtraData*	next;		// +0x08
};

class InventoryChanges : public BSExtraData {	// 0x10 bytes
public:
	class EntryData {	// 0xC bytes
	public:
		void*		extendData;	// +0x00 TList<ExtraDataList>*
		SInt32		countDelta;	// +0x04
		TESForm*	type;		// +0x08
	};

	void* data;		// +0x0C
};

class ExtraRefractionProperty : public BSExtraData {	// 0x10 bytes
public:
	float refractionAmount;	// +0x0C 0..1
};

class ExtraDataList {	// 0x20 bytes
public:
	NVR_VSLOT(0);
	BSExtraData*	m_data;					// +0x04
	UInt8			m_presenceBitfield[0x15];	// +0x08 one bit per ExtraDataType
	UInt8			pad1D[3];

	BSExtraData* GetExtraData(BSExtraData::ExtraDataType type) { return (BSExtraData*)ThisCall(0x00410220, this, type); }
};

// ---------------------------------------------------------------------------------------------------------------------
// Form components and forms
// ---------------------------------------------------------------------------------------------------------------------

class BaseFormComponent {	// 0x4 bytes
public:
	NVR_VSLOT(0); NVR_VSLOT(1); NVR_VSLOT(2); NVR_VSLOT(3);
};

class TESFullName : public BaseFormComponent {	// 0xC bytes
public:
	BSString name;		// +0x04
};

class TESTexture : public BaseFormComponent {	// 0xC bytes
public:
	BSString ddsPath;	// +0x04
};

class TESModel : public BaseFormComponent {	// 0x18 bytes
public:
	BSString	nifPath;	// +0x04
	UInt32		pad0C[3];
};

class TESModelTextureSwap : public TESModel {	// 0x20 bytes
public:
	UInt32 pad18[2];	// +0x18 texture swap list
};

class TESForm : public BaseFormComponent {	// 0x18 bytes
public:
	enum FormType {		// TESForm::formType
		kFormType_Activator = 0x15, kFormType_TalkingActivator = 0x16, kFormType_Terminal = 0x17,
		kFormType_Book = 0x19, kFormType_Container = 0x1B, kFormType_Door = 0x1C,
		kFormType_Light = 0x1E, kFormType_Misc = 0x1F, kFormType_Stat = 0x20,
		kFormType_StaticCollection = 0x21, kFormType_MoveableStatic = 0x22, kFormType_PlaceableWater = 0x23,
		kFormType_Grass = 0x24, kFormType_Tree = 0x25, kFormType_Flora = 0x26,
		kFormType_Furniture = 0x27, kFormType_NPC = 0x2A, kFormType_Creature = 0x2B,
		kFormType_LeveledCreature = 0x2C, kFormType_IdleMarker = 0x30, kFormType_Weather = 0x35,
		kFormType_Land = 0x42, kFormType_ImageSpaceModifier = 0x54, kFormType_Apparatus = 0xFE,
	};
	enum FormFlags {	// TESForm::flags
		kFormFlags_NotCastShadows = 0x200,
	};

	NVR_VSLOT(4); NVR_VSLOT(5); NVR_VSLOT(6); NVR_VSLOT(7); NVR_VSLOT(8); NVR_VSLOT(9);
	NVR_VSLOT10(1); NVR_VSLOT10(2); NVR_VSLOT10(3); NVR_VSLOT10(4); NVR_VSLOT10(5);
	NVR_VSLOT(60); NVR_VSLOT(61); NVR_VSLOT(62); NVR_VSLOT(63);
	virtual bool		IsActor();			// 64
	NVR_VSLOT(65); NVR_VSLOT(66); NVR_VSLOT(67); NVR_VSLOT(68); NVR_VSLOT(69);
	NVR_VSLOT(70); NVR_VSLOT(71); NVR_VSLOT(72); NVR_VSLOT(73); NVR_VSLOT(74); NVR_VSLOT(75);
	virtual const char*	GetEditorName();	// 76 (empty for most forms at run time)
	NVR_VSLOT(77);

	UInt8		formType;	// +0x04 FormType
	UInt8		pad05[3];	// +0x05 (unused)
	UInt32		flags;		// +0x08 FormFlags
	UInt32		refID;		// +0x0C
	UInt32		pad10[2];	// mod list
};

class TESGlobal : public TESForm {	// 0x28 bytes
public:
	BSString	name;		// +0x18
	UInt8		type;		// +0x20
	UInt8		pad21[3];
	float		data;		// +0x24
};

class TESWeather : public TESForm {	// 0x36C bytes
public:
	enum { kNumColorTypes = 10, kNumTimeOfDay = 4 };	// colour kinds, times of day
	enum {
		eColor_SkyUpper = 0, eColor_Fog = 1, eColor_Sunlight = 4,
		eColor_Sun = 5, eColor_Lighting = 10,
	};
	enum {
		eTime_Sunrise = 0, eTime_Day = 1, eTime_Sunset = 2,
		eTime_Night = 3,	// ColorData::colors index
	};
	enum WeatherType {	// classification flags (weatherType)
		kType_Cloudy = 0x2, kType_Rainy = 0x4, kType_Snow = 0x8,
	};

	struct ColorData {	// 0x18 bytes
		RGBA colors[6];		// by eTime_*
	};
	struct FogInfo {	// 0xC bytes
		float nearFog;		// +0x00
		float farFog;		// +0x04
		float power;		// +0x08
	};

	UInt32			pad18[7];				// image space modifiers
	TESTexture		textureLayers[4];		// +0x34 cloud layers
	UInt8			cloudSpeed[4];			// +0x64
	UInt32			cloudColor[4][6];		// +0x68
	TESModel		model;					// +0xC8
	UInt8			windSpeed;				// +0xE0
	UInt8			cloudSpeedLower;		// +0xE1
	UInt8			cloudSpeedUpper;		// +0xE2
	UInt8			transDelta;				// +0xE3
	UInt8			sunGlare;				// +0xE4
	UInt8			sunDamage;				// +0xE5
	UInt8			padE6[5];				// precipitation and lightning fades
	UInt8			weatherType;			// +0xEB
	UInt32			padEC;					// lightning colour
	FogInfo			fogDay;					// +0xF0
	FogInfo			fogNight;				// +0xFC
	ColorData		colors[kNumColorTypes];	// +0x108
	UInt32			pad1F8[2];				// sounds
	float			hdrInfo[14];			// +0x200
	UInt32			pad238[77];

	float GetFogDayNear() { return this->fogDay.nearFog; }		// fog planes, day and night
	float GetFogDayFar() { return this->fogDay.farFog; }
	float GetFogNightNear() { return this->fogNight.nearFog; }
	float GetFogNightFar() { return this->fogNight.farFog; }
	UInt8 GetSunGlare() { return this->sunGlare; }
	UInt8 GetTransDelta() { return this->transDelta; }
	UInt8 GetWindSpeed() { return this->windSpeed; }
	UInt8 GetWeatherType() { return this->weatherType; }
	UInt8 GetSunDamage() { return this->sunDamage; }
	UInt8 GetCloudSpeedLower() { return this->cloudSpeedLower; }
	UInt8 GetCloudSpeedUpper() { return this->cloudSpeedUpper; }
	float GetHDR(int i) { return hdrInfo[i]; }

	void SetFogDayNear(float v) { fogDay.nearFog = v; }
	void SetFogDayFar(float v) { fogDay.farFog = v; }
	void SetFogNightNear(float v) { fogNight.nearFog = v; }
	void SetFogNightFar(float v) { fogNight.farFog = v; }
	void SetSunGlare(UInt8 v) { sunGlare = v; }
	void SetTransDelta(UInt8 v) { transDelta = v; }
	void SetWindSpeed(UInt8 v) { windSpeed = v; }
	void SetSunDamage(UInt8 v) { sunDamage = v; }
	void SetCloudSpeedLower(UInt8 v) { cloudSpeedLower = v; }
	void SetCloudSpeedUpper(UInt8 v) { cloudSpeedUpper = v; }
};

// NVR allocates its weathers with a second colour set appended (the game copies forms by size from the record).
class TESWeatherEx : public TESWeather {	// 0x45C bytes
public:
	ColorData colorsb[kNumColorTypes];	// +0x36C
};

class TESWaterForm : public TESForm {	// 0x194 bytes
public:
	enum WaterType {	// GetWaterType (always Normal here)
		kWaterType_Blood = 0, kWaterType_Lava = 1, kWaterType_Normal = 2,
	};

	struct Properties {	// 0xE4 bytes
		UInt32	pad00[2];
		float	unk08;				// +0x08
		UInt32	pad0C[7];
		RGBA	shallowColor;		// +0x28
		RGBA	deepColor;			// +0x2C
		RGBA	reflectionColor;	// +0x30
		UInt8	unk34;				// +0x34
		UInt8	pad35[3];
		float	unk38;				// +0x38
		UInt32	pad3C[20];
		float	fogAmountUW;		// +0x8C
		float	fogNearUW;			// +0x90
		float	fogFarUW;			// +0x94
		float	pad98;
		float	specularPower;		// +0x9C
		UInt32	padA0[17];
	};

	UInt32		pad18[27];
	Properties	properties;		// +0x84
	UInt32		pad168[11];

	UInt32 GetWaterType() { return kWaterType_Normal; }
	RGBA* GetShallowColor() { return &this->properties.shallowColor; }
	RGBA* GetDeepColor() { return &this->properties.deepColor; }
};

class TESWorldSpace : public TESForm {	// 0xEC bytes
public:
	UInt32			pad18[7];
	TESObjectCELL*	cell;				// +0x34 the persistent cell
	UInt32			pad38[5];
	UInt8			flags;				// +0x4C
	UInt8			pad4D;
	UInt16			parentFlags;		// +0x4E which data comes from the parent world
	UInt32			pad50[8];
	TESWorldSpace*	parent;				// +0x70
	TESWaterForm*	waterFormFirst;		// +0x74
	TESWaterForm*	waterFormLast;		// +0x78 LOD water
	float			waterHeight;		// +0x7C
	UInt32			pad80[27];
};

class TESClimate : public TESForm {	// 0x58 bytes
public:
	UInt32		pad18[15];
	UInt8		pad54;
	UInt8		phaseLength;	// +0x55 moon phase length (low 6 bits)
	UInt8		pad56[2];
};

class TESRegion : public TESForm {	// 0x38 bytes
public:
	UInt32			pad18[2];		// data and area entries
	TESWorldSpace*	worldSpace;		// +0x20
	TESWeather*		weather;		// +0x24
	UInt32			pad28[4];
};

class TESObjectSTAT : public TESForm {	// 0x58 bytes
public:
	UInt32					pad18[6];	// bound object
	TESModelTextureSwap		model;		// +0x30
	UInt32					pad50[2];
};

class TESObjectLIGH : public TESForm {	// 0xC8 bytes
public:
	enum LightFlags {	// lightFlags
		kLightFlags_CanCarry = 0x2,
	};

	UInt32					pad18[9];		// bound object, name
	TESModelTextureSwap		modelSwap;		// +0x3C
	UInt32					pad5C[16];		// icon, message icon, script, weight, value, destructible
	SInt32					time;			// +0x9C
	UInt32					radius;			// +0xA0
	UInt8					red;			// +0xA4
	UInt8					green;			// +0xA5
	UInt8					blue;			// +0xA6
	UInt8					padA7;
	UInt32					lightFlags;		// +0xA8 LightFlags
	float					falloffExp;		// +0xAC
	float					FOV;			// +0xB0
	UInt32					padB4[5];
};

class TESObjectWEAP : public TESForm {	// 0x388 bytes
public:
	enum WeaponType {	// weaponType (animation type)
		kWeapType_HandToHandMelee = 0x0, kWeapType_OneHandMelee = 0x1, kWeapType_TwoHandMelee = 0x2,
		kWeapType_OneHandGrenade = 0xA, kWeapType_OneHandMine = 0xB, kWeapType_OneHandLunchboxMine = 0xC,
		kWeapType_OneHandThrown = 0xD,
	};

	UInt32		pad18[55];
	UInt8		weaponType;		// +0xF4 WeaponType
	UInt8		padF5[7];
	float		reach;			// +0xFC
	UInt32		pad100[162];
};

// ---------------------------------------------------------------------------------------------------------------------
// Cells and references
// ---------------------------------------------------------------------------------------------------------------------

class TESObjectCELL : public TESForm {	// 0xE0 bytes
public:
	enum {
		kFlags0_Interior = 1 << 0, kFlags0_BehaveLikeExterior = 1 << 7,
	};
	enum CellNodes : UInt32 {	// children of the cell's root node
		kCellNode_Land = 0x2,	// land
	};

	struct CellCoordinates {	// 0x8 bytes
		SInt32 x, y;
	};
	struct LightingData {	// 0x2C bytes
		RGBA	ambient;		// +0x00
		RGBA	directional;	// +0x04
		RGBA	fog;			// +0x08
		float	fogNear;		// +0x0C
		float	fogFar;			// +0x10
		UInt32	pad14[4];
		float	fogPower;		// +0x24
		UInt32	pad28;
	};
	// Only its first field is known: the cell's root node.
	struct StructC4 {	// 0x64 bytes
		NiNode*	niNode;		// +0x00
		UInt32	pad04[24];	// reference lists and maps
	};

	TESFullName				fullName;			// +0x18
	UInt8					flags0;				// +0x24
	UInt8					pad25[3];
	ExtraDataList			extraDataList;		// +0x28
	union {
		CellCoordinates*	coords;				// +0x48 exterior
		LightingData*		lighting;			// +0x48 interior
	};
	UInt32					pad4C;
	float					waterHeight;		// +0x50
	UInt32					pad54[22];
	TList<TESObjectREFR>	objectList;			// +0xAC
	UInt32					padB4[3];
	TESWorldSpace*			worldSpace;			// +0xC0
	StructC4*				structC4;			// +0xC4
	UInt32					padC8[4];
	BGSLightingTemplate*	lightingTemplate;	// +0xD8
	UInt32					unk0DC;				// +0xDC

	TESWaterForm* GetWaterForm() { return reinterpret_cast<TESWaterForm*>(ThisCall(0x00547770, this)); }
	float GetWaterHeight() { return ThisCallF(0x005471E0, this); }	// the cell's water level
	NiNode* GetNode() { return structC4 ? structC4->niNode : nullptr; }
	NiNode* GetChildNode(CellNodes aeNode);		// Hooks/Game.cpp
	bool IsInterior() { return (flags0 & kFlags0_Interior) != 0; }	// interior cell
};

class TESObjectREFRData {	// 0x18 bytes
public:
	UInt32		pad00[5];
	NiNode*		niNode;		// +0x14
};

class TESObjectREFR : public TESForm {	// 0x68 bytes
public:
	NVR_VSLOT(78); NVR_VSLOT(79); NVR_VSLOT10(8); NVR_VSLOT10(9); NVR_VSLOT10(10);
	NVR_VSLOT(110); NVR_VSLOT(111); NVR_VSLOT(112); NVR_VSLOT(113); NVR_VSLOT(114); NVR_VSLOT(115);
	virtual NiNode*		GetNiNode();			// 116
	NVR_VSLOT(117); NVR_VSLOT(118); NVR_VSLOT(119); NVR_VSLOT(120); NVR_VSLOT(121); NVR_VSLOT(122); NVR_VSLOT(123);
	NVR_VSLOT(124); NVR_VSLOT(125); NVR_VSLOT(126); NVR_VSLOT(127); NVR_VSLOT(128); NVR_VSLOT(129); NVR_VSLOT(130);
	NVR_VSLOT(131); NVR_VSLOT(132); NVR_VSLOT(133); NVR_VSLOT(134); NVR_VSLOT(135); NVR_VSLOT(136); NVR_VSLOT(137); NVR_VSLOT(138);
	virtual UInt8		GetDead(UInt8 arg);		// 139
	NVR_VSLOT(140); NVR_VSLOT(141); NVR_VSLOT(142); NVR_VSLOT(143);

	UInt32				pad18[2];		// child cell
	TESForm*			baseForm;		// +0x20
	NiPoint3			rot;			// +0x24
	NiPoint3			pos;			// +0x30
	float				scale;			// +0x3C
	TESObjectCELL*		parentCell;		// +0x40
	ExtraDataList		extraDataList;	// +0x44
	TESObjectREFRData*	renderData;		// +0x64

	NiNode* GetNode() { return renderData ? renderData->niNode : nullptr; }
};

// Actor processes: only these two virtuals are called.
class BaseProcess {	// 0x30 bytes
public:
	NVR_VSLOT100(0); NVR_VSLOT100(1);
	NVR_VSLOT10(20); NVR_VSLOT10(21); NVR_VSLOT10(22); NVR_VSLOT10(23); NVR_VSLOT10(24);
	NVR_VSLOT(250); NVR_VSLOT(251); NVR_VSLOT(252); NVR_VSLOT(253); NVR_VSLOT(254); NVR_VSLOT(255); NVR_VSLOT(256);
	virtual bool IsAiming();		// 257
	NVR_VSLOT(258); NVR_VSLOT(259); NVR_VSLOT10(26);
	NVR_VSLOT(270); NVR_VSLOT(271); NVR_VSLOT(272); NVR_VSLOT(273); NVR_VSLOT(274); NVR_VSLOT(275); NVR_VSLOT(276);
	virtual bool IsWeaponOut();		// 277

	UInt32 pad04[11];
};

class MiddleHighProcess : public BaseProcess {	// 0x25C bytes
public:
	UInt32							pad30[58];
	InventoryChanges::EntryData*	ammoInfo;	// +0x118
	UInt32							pad11C[80];
};

class MobileObject : public TESObjectREFR {	// 0x88 bytes
public:
	BaseProcess*	process;	// +0x68
	UInt32			pad6C[7];
};

class MagicTarget {	// 0x10 bytes
public:
	NVR_VSLOT(0);
	UInt32 pad04[3];
};

class ActorValuesOwner {	// 0x4 bytes
public:
	virtual UInt32 GetBase(UInt32 code);	// 0
	NVR_VSLOT(1);
	virtual UInt32 GetCurrent(UInt32 code);	// 2
};

class Actor : public MobileObject {	// 0x1B4 bytes
public:
	enum ActorVal {	// actor value codes
		kActorVal_Health = 16, kActorVal_Stamina = 22,
	};

	UInt32				pad88[3];		// magic caster
	MagicTarget			magicTarget;	// +0x94
	ActorValuesOwner	avOwner;		// +0xA4
	UInt32				padA8[24];
	UInt32				lifeState;		// +0x108
	UInt32				pad10C[16];
	UInt8				inWater;		// +0x14C
	UInt8				pad14D[3];
	UInt32				pad150[25];

	UInt32 GetBaseActorValue(ActorVal code) { return avOwner.GetBase(code); }
	UInt32 GetActorValue(ActorVal code) { return avOwner.GetCurrent(code); }
};

class Character : public Actor {	// 0x1C8 bytes
public:
	SkinInfo*	ActorSkinInfo;	// +0x1B4
	UInt32		pad1B8[4];
};

class PlayerCharacter : public Character {	// 0xE50 bytes
public:
	UInt32		pad1C8[289];
	UInt8		isThirdPerson;			// +0x64C
	UInt8		pad64D[3];
	UInt32		pad650[8];
	float		worldFoV;				// +0x670
	float		firstPersonFoV;			// +0x674
	UInt32		pad678[5];
	SkinInfo*	firstPersonSkinInfo;	// +0x68C
	UInt32		pad690;
	NiNode*		firstPersonNiNode;		// +0x694
	UInt32		pad698[50];
	TESRegion*	currentRegion;			// +0x760
	UInt32		pad764[443];

	bool IsAiming() { return process->IsWeaponOut() && process->IsAiming(); }
	TESWorldSpace* GetWorldSpace() { return parentCell ? parentCell->worldSpace : nullptr; }
	TESRegion* GetRegion() { return this->currentRegion; }
	bool IsAlive() { return GetDead(1) == 0; }
	bool IsFirstPerson() { return isThirdPerson == 0; }
	bool IsVanity() { return *(bool*)0x011E07B8; }		// vanity (orbiting) camera
	// Sets the world FoV and both FoV settings (fDefaultWorldFOV 0x1203160, fDefault1stPersonFOV 0x120316C).
	void SetFoV(float FoV) { worldFoV = *(float*)0x01203160 = *(float*)0x0120316C = FoV; }
	float GetFoV(bool IsSpecialView) { return IsSpecialView ? firstPersonFoV : worldFoV; }
	void ResetCamera() {}	// nothing to do on New Vegas
};

// The bone nodes and equipped models of an actor's skeleton.
class SkinInfo {	// 0x2B4 bytes
public:
	UInt32			pad000[2];
	NiNode*			HeadNode;		// +0x08
	UInt32			pad00C;
	NiNode*			WeaponNode;		// +0x10
	UInt32			pad014[3];
	NiNode*			Spine2Node;		// +0x20
	UInt32			pad024[22];
	TESObjectWEAP*	WeaponForm;		// +0x7C
	UInt32			pad080[3];
	TESForm*		LightForm;		// +0x8C the Pip-Boy
	UInt32			pad090[137];
};

// ---------------------------------------------------------------------------------------------------------------------
// Sky
// ---------------------------------------------------------------------------------------------------------------------

class SkyObject {	// 0x8 bytes
public:
	NVR_VSLOT(0); NVR_VSLOT(1); NVR_VSLOT(2); NVR_VSLOT(3);
	NiNode* RootNode;	// +0x04
};

class Sun : public SkyObject {	// 0x2C bytes
public:
	UInt32 pad08[9];
};

class Stars : public SkyObject {	// 0x10 bytes
public:
	UInt32 pad08[2];
};

class Clouds : public SkyObject {	// 0x5C bytes
public:
	NiTriStrips*		layers[4];		// +0x08
	NiSourceTexture*	textures[4];	// +0x18
	NiPoint3			layerPos[4];	// +0x28
	UInt16				numLayers;		// +0x58
	UInt8				pad5A[2];
};

class Moon : public SkyObject {	// 0x7C bytes
public:
	NiNode*		MoonNode;		// +0x08
	NiNode*		ShadowNode;		// +0x0C
	NiGeometry*	MoonMesh;		// +0x10
	NiGeometry*	ShadowMesh;		// +0x14
	UInt32		pad18[25];
};

class NiFogProperty : public NiProperty {	// 0x2C bytes
public:
	UInt16		m_flags;	// +0x18
	float		m_fDepth;	// +0x1C
	NiColor		m_kColor;	// +0x20
};

class BSFogProperty : public NiFogProperty {	// 0x64 bytes
public:
	float		fStartDistance;			// +0x2C
	float		fEndDistance;			// +0x30
	float		fStartWaterDistance;	// +0x34
	float		fEndWaterDistance;		// +0x38
	NiPlane		kPlane;					// +0x3C
	float		fFalloff;				// +0x4C
	float		fHeight;				// +0x50
	NiColor		kWaterColor;			// +0x54
	float		fPower;					// +0x60
};

class Sky {	// 0x138 bytes
public:
	enum Flags : UInt32 {
		kSkyFlag_IsUnderwater = 0x4,	// the camera is under water
	};

	void**			_vtbl;				// +0x000
	NiNode*			nodeSkyRoot;		// +0x004
	UInt32			pad008;
	TESClimate*		firstClimate;		// +0x00C
	TESWeather*		firstWeather;		// +0x010 current
	TESWeather*		secondWeather;		// +0x014 fading out during a transition
	UInt32			pad018[4];
	Sun*			sun;				// +0x028
	Clouds*			clouds;				// +0x02C
	Moon*			masserMoon;			// +0x030
	UInt32			pad034[2];
	NiColor			skyUpper;			// +0x03C
	NiColor			fogColor;			// +0x048
	NiColor			pad054;
	NiColor			sunAmbient;			// +0x060
	NiColor			sunDirectional;		// +0x06C
	NiColor			SunColor;			// +0x078
	NiColor			pad084;
	NiColor			SkyLower;			// +0x090
	NiColor			Horizon;			// +0x09C
	NiColor			pad0A8[3];
	float			windSpeed;			// +0x0CC
	float			windDirection;		// +0x0D0
	float			fogNearPlane;		// +0x0D4
	float			fogFarPlane;		// +0x0D8
	UInt32			unk0DC;				// +0x0DC
	UInt32			pad0E0[2];
	float			fogPower;			// +0x0E8
	UInt32			pad0EC[2];
	float			weatherPercent;		// +0x0F4
	UInt32			pad0F8[8];
	UInt32			flags;				// +0x118 Flags
	UInt32			pad11C[7];

	static Sky* Get() { return *(Sky**)0x11DEA20; }		// the TES sky
	void ForceWeather(TESWeather* weather) { ThisCall(0x0063D0E0, this, weather, 0); }
	float GetSunriseBegin() { return (float)ThisCallD(0x595EA0, this); }
	float GetSunriseEnd() { return (float)ThisCallD(0x595F50, this); }
	float GetSunsetBegin() { return (float)ThisCallD(0x595FC0, this); }
	float GetSunsetEnd() { return (float)ThisCallD(0x596030, this); }
	float GetSunriseColorBegin() { return (float)ThisCallD(0x63B9B0, this); }
	float GetSunsetColorEnd() { return (float)ThisCallD(0x63BA30, this); }
	bool GetIsUnderWater() { return flags & kSkyFlag_IsUnderwater; }
};

// ---------------------------------------------------------------------------------------------------------------------
// The loaded world
// ---------------------------------------------------------------------------------------------------------------------

class GridCellArray {	// 0x28 bytes
public:
	NVR_VSLOT(0);
	SInt32				worldX;		// +0x04
	SInt32				worldY;		// +0x08
	UInt8				size;		// +0x0C cells per side (uGridsToLoad)
	UInt8				pad0D[3];
	TESObjectCELL**		gridCells;	// +0x10 size * size
	UInt32				pad14[5];

	TESObjectCELL* GetCell(UInt32 x, UInt32 y) { return *reinterpret_cast<TESObjectCELL**>(ThisCall(0x004BA490, this, x, y)); }	// by grid position
	TESObjectCELL* GetCell(UInt32 i) { return gridCells[i]; }
};

struct WaterGroup {	// 0xB0 bytes
	TESWaterForm*			waterForm;		// +0x00
	NiVector4				vector04;		// +0x04
	NiVector4				vector14;		// +0x14
	DList<TESObjectREFR>	waterPlanes;	// +0x24
	UInt32					pad30[11];
	UInt8					byte5C;			// +0x5C
	UInt8					byte5D;			// +0x5D
	UInt8					byte5E;			// +0x5E
	UInt8					byte5F;			// +0x5F
	UInt8					byte60;			// +0x60
	UInt8					pad61[3];
	UInt32					pad64[19];
};

class WaterManager {	// 0xA0 bytes
public:
	UInt32				pad00[9];
	float				unk24;			// +0x24
	UInt32				pad28[3];
	UInt8				unk34;			// +0x34
	UInt8				pad35[3];
	float				unk38;			// +0x38
	DList<WaterGroup>	waterGroups;	// +0x3C
	UInt32				pad48[20];
	float				unk98;			// +0x98
	UInt8				unk9C;			// +0x9C
	UInt8				pad9D[3];
};

class TES {	// 0xC4 bytes
public:
	NVR_VSLOT(0);
	UInt32					pad04;
	GridCellArray*			gridCellArray;		// +0x08
	UInt32					pad0C[4];
	NiDirectionalLight*		directionalLight;	// +0x1C
	UInt32					pad20[17];
	WaterManager*			waterManager;		// +0x64
	Sky*					sky;				// +0x68
	TList<void>				activeIMODs;		// +0x6C image space modifier instances
	UInt32					pad74[5];
	TESWorldSpace*			worldSpace;			// +0x88
	UInt32					pad8C[14];

	float GetWaterHeight(PlayerCharacter* Ref, SceneGraph* WorldSceneGraph, TESWaterForm** waterForm);
};

// ---------------------------------------------------------------------------------------------------------------------
// Menus, mods, input, main loop
// ---------------------------------------------------------------------------------------------------------------------

class Menu {	// 0x28 bytes
public:
	enum MenuType {	// menu IDs (also the menu stack entries)
		kMenuType_None			= 0,		// game mode
		kMenuType_BigFour		= 1,		// F1 - F4
		kMenuType_Console = 3, kMenuType_Main = 4, kMenuType_Loading = 0x3EF,
		kMenuType_Dialog = 0x3F1, kMenuType_Start = 0x3F5, kMenuType_LockPick = 0x3F6,
		kMenuType_Persuasion = 0x40A, kMenuType_Surgery = 0x41E, kMenuType_Hacking = 0x41F,
		kMenuType_VATS = 0x420, kMenuType_Computers = 0x421, kMenuType_SlotMachine = 0x438,
		kMenuType_Blackjack = 0x439, kMenuType_Roulette = 0x43A, kMenuType_Caravan = 0x43B,
	};

	NVR_VSLOT(0);
	UInt32 pad04[9];
};

class LockPickMenu : public Menu {	// 0x28 bytes
public:
	static LockPickMenu* GetSingleton() { return *(LockPickMenu**)0x11DA204; }
};

class ModInfo {	// 0x42C bytes
public:
	UInt32	pad000[8];
	char	name[0x104];	// +0x020 file name
	UInt32	pad124[194];
};

class ModList {	// 0x408 bytes
public:
	UInt32		pad00[2];
	UInt32		loadedModCount;		// +0x08
	ModInfo*	loadedMods[0xFF];	// +0x0C
};

class MainDataHandler {	// 0x63C bytes
public:
	UInt32				pad000[12];
	TList<TESWeather>	weathers;	// +0x030
	UInt32				pad038[118];
	ModList				modList;	// +0x210
	UInt32				pad618[9];

	// Editor names of every weather (other form types: an empty list).
	void FillNames(std::vector<std::string>* names, UInt32 formType) {
		names->clear();
		if (formType != TESForm::kFormType_Weather) return;
		for (TList<TESWeather>::Entry* entry = &weathers.First; entry && entry->item; entry = entry->next)
			names->push_back(entry->item->GetEditorName());
	}

	// The weather with this editor name (other form types, or no match: null).
	TESForm* GetFormByName(const char* editorName, UInt32 formType) {
		if (formType != TESForm::kFormType_Weather) return nullptr;
		for (TList<TESWeather>::Entry* entry = &weathers.First; entry && entry->item; entry = entry->next)
			if (!strcmp(editorName, entry->item->GetEditorName())) return entry->item;
		return nullptr;
	}
};

class InputControl {	// 0x1C04 bytes
public:
	UInt32					pad0000[12];
	IDirectInputDevice8*	mouseInterface;				// +0x0030 c_dfDIMouse2
	UInt32					pad0034[1585];
	UInt8					CurrentKeyState[0x100];		// +0x18F8 DirectInput scan codes
	UInt8					PreviousKeyState[0x100];	// +0x19F8
	UInt32					pad1AF8[11];
	DIMOUSESTATE2			CurrentMouseState;			// +0x1B24
	DIMOUSESTATE2			PreviousMouseState;			// +0x1B38
	UInt32					pad1B4C[18];
	UInt8					KeyboardInputControls[28];	// +0x1B94 key bound to each control
	UInt8					MouseInputControls[28];		// +0x1BB0 button bound to each control
	UInt32					pad1BCC[14];
};

class Main {	// 0xA4 bytes
public:
	UInt8	pad00[6];
	UInt8	FlyCam;		// +0x06 tfc
	UInt8	pad07[0x9D];

	InputControl* GetInputControl() { return *(InputControl**)0x011F35CC; }	// OSInputGlobals

	// Codes 0..255 are keys, 256 and up mouse buttons; 255 is "unbound". Down = pressed this frame, Pressed = held.
	bool OnKeyDown(UInt16 KeyCode) {
		if (KeyCode >= 256) return OnMouseDown(KeyCode - 256);
		const InputControl* input = GetInputControl();
		return KeyCode != 255 && input->CurrentKeyState[KeyCode] && !input->PreviousKeyState[KeyCode];
	}
	bool OnKeyPressed(UInt16 KeyCode) {
		if (KeyCode >= 256) return OnMousePressed(KeyCode - 256);
		const InputControl* input = GetInputControl();
		return KeyCode != 255 && input->CurrentKeyState[KeyCode] && input->PreviousKeyState[KeyCode];
	}
	bool OnMouseDown(UInt8 ButtonIndex) {
		const InputControl* input = GetInputControl();
		return ButtonIndex != 255 && input->CurrentMouseState.rgbButtons[ButtonIndex] && !input->PreviousMouseState.rgbButtons[ButtonIndex];
	}
	bool OnMousePressed(UInt8 ButtonIndex) {
		const InputControl* input = GetInputControl();
		return ButtonIndex != 255 && input->CurrentMouseState.rgbButtons[ButtonIndex] && input->PreviousMouseState.rgbButtons[ButtonIndex];
	}
};

class MenuInterfaceManager {	// 0x580 bytes
public:
	UInt32		pad000[3];
	UInt8		currentMode;		// +0x00C 1 game mode, 2 menu mode
	UInt8		pad00D[3];
	UInt32		pad010[65];
	UInt32		menuStack[10];		// +0x114 Menu::MenuType, top first
	UInt32		pad13C[12];
	void*		pRenderedMenu;		// +0x16C
	UInt32		pad170[211];
	UInt32		pipBoyMode;			// +0x4BC
	UInt32		pad4C0[48];

	bool IsActive(UInt32 MenuType) {
		const UInt32* s = menuStack;
		if (MenuType != Menu::kMenuType_Main) return s[0] == MenuType;
		// the main menu shows as Loading over Start, or with the console open on top of both
		return (s[0] == Menu::kMenuType_Loading && s[1] == Menu::kMenuType_Start) ||
			   (s[0] == Menu::kMenuType_Console && s[1] == Menu::kMenuType_Loading && s[2] == Menu::kMenuType_Start);
	}
	// Corner notification, 2 seconds (0x7052F0).
	void ShowMessage(const char* Message) { CdeclCall<bool>(0x007052F0, Message, (UInt32)0, (const char*)nullptr, (const char*)nullptr, 2.0f, false); }
	UInt8 IsPipBoyOpen() { return pipBoyMode == 3; }
};

class TimeGlobals {	// 0x18 bytes
public:
	TESGlobal*	GameYear;			// +0x00
	TESGlobal*	GameMonth;			// +0x04
	TESGlobal*	GameDay;			// +0x08
	TESGlobal*	GameHour;			// +0x0C
	TESGlobal*	GameDaysPassed;		// +0x10
	TESGlobal*	TimeScale;			// +0x14

	static TimeGlobals* Get() { return reinterpret_cast<TimeGlobals*>(0x11DE7B8); }
	static float GetGameTime() { return Get()->GameHour->data * 60.0f * 60.0f; }	// seconds into the day
};

class QueuedModelLoader {	// 0x1C bytes
public:
	UInt32 pad00[7];
};

class GameSetting {	// 0xC bytes
public:
	void*		vftbl;		// +0x00
	union {
		char*	pValue;		// +0x04
		int		iValue;		// i settings
		float	fValue;		// f settings
	};
	char*		Name;		// +0x08
};

// The game's muzzle flash state of one weapon shot.
struct MuzzleFlash {	// 0x20 bytes
	bool			bEnabled;	// +0x00
	UInt8			pad01[11];
	NiNode*			node;		// +0x0C
	NiPointLight*	light;		// +0x10
	UInt32			pad14[3];
};

// ---------------------------------------------------------------------------------------------------------------------
// Game globals and functions used directly
// ---------------------------------------------------------------------------------------------------------------------

namespace Pointers {	// raw game globals and functions
	namespace Generic {		// globals
		static auto MPF						= (float*)0x00000000;		// placeholder, never read
		static auto CameraWorldTranslate	= (NiPoint3*)0x011F474C;
		static auto CameraLocation			= (NiPoint3*)0x011F426C;
		static auto DetectorWindowNode		= (NiNode**)0x011FA008;
	}
	namespace Functions {	// functions called through these pointers
		static auto MemoryAlloc			= (void* (__cdecl*)(size_t))0x00AA13E0;		// game operator new
		static auto FormMemoryAlloc		= (void* (__cdecl*)(size_t))0x00401000;
		static auto ExtractArgs			= (bool (__cdecl*)(CommandParam*, void*, UInt32*, TESObjectREFR*, TESObjectREFR*, Script*, ScriptEventList*, ...))0x005ACCB0;
		static auto PrintToConsole		= (void (*)(const char*, ...))0x00703C00;
		static auto GetPassDescription	= (char* (__cdecl*)(UInt32))0x00B4F9D0;	// render pass name
	}
	namespace VirtualTables {	// for vtable compares
		static auto BSDismemberSkinInstance = (const void*)0x01069A84;
	}
	namespace Settings {		// INI values
		static auto GrassWindMagnitudeMin = (float*)0x01200500;
		static auto GrassWindMagnitudeMax = (float*)0x01200504;
	}
	namespace ShaderParams {		// SpeedTree constants
		static auto RockParams		= (float*)0x01200658;
		static auto RustleParams	= (float*)0x01200668;
		static auto WindMatrixes	= (float*)0x01200688;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Shader properties
// ---------------------------------------------------------------------------------------------------------------------

// Bit numbers for BSShaderProperty::GetFlag (0..31 in ulFlags[0], 32..63 in ulFlags[1]).
enum BSShaderPropertyFlag {
	BSSP_REFRACTION = 15, BSSP_FIRE_REFRACTION = 16, BSSP_EYE_ENVIRONMENT_MAPPING = 17,
	BSSP_DECAL = 26, BSSP_DYNAMIC_DECAL = 27,
};

class BSShaderProperty : public NiShadeProperty {	// 0x60 bytes
public:
	enum BSShaderFlags : UInt32 {
		Specular = 0x1, FaceGen = 0x400, Parallax_Shader = 0x800,
		Model_Space_Normals = 0x1000, Landscape = 0x4000, Hair = 0x40000,
		Tree_Billboard = 0x400000, Parallax_Occlusion = 0x10000000,
	};
	enum BSShaderFlags2 : UInt32 {
		Vertex_Colors = 0x20, Skip_Normal_Maps = 0x100000,
	};

	UInt32		ulFlags[2];			// +0x20 BSShaderFlags, BSShaderFlags2
	float		fAlpha;				// +0x28
	float		fFadeAlpha;			// +0x2C
	UInt32		pad30[3];
	UInt32		pRenderPassArray;	// +0x3C
	UInt32		pad40[6];
	UInt32		uiShaderIndex;		// +0x58 the shader definition (1 shadow light, 0xF parallax, 0x1D lighting30, ...)
	float		fDepthBias;			// +0x5C

	bool IsLightingProperty() { return uiShaderIndex == 0x1 || uiShaderIndex == 0x1D || uiShaderIndex == 0xF; }
	bool GetFlag(uint32_t flag) const { return (ulFlags[flag >> 5] & (1 << (flag % 32u))) != 0; }
};

class BSShaderPPLightingProperty : public BSShaderProperty {	// 0x104 bytes
public:
	UInt32				pad60[8];			// light list
	float				fMorphDistance;		// +0x80
	UInt32				pad84[9];
	UInt16				usLandscapeTextures;	// +0xA8
	UInt16				padAA;
	NiSourceTexture**	ppTextures[6];		// +0xAC
	UInt32				padC4[16];
};

class SpeedTreeLeafShaderProperty : public BSShaderProperty {	// 0x8C bytes
public:
	struct LeafData {	// 0xC bytes
		UInt32	unk00;		// +0x00
		UInt32	unk04;		// +0x04
		float*	leafBase;	// +0x08 float4[48]
	};

	UInt32		pad60[10];	// light list
	LeafData*	leafData;	// +0x88
};

class SkyShaderProperty : public BSShaderProperty {	// 0x90 bytes
public:
	enum SkyObject : uint32_t {	// which sky part a sky shader property draws
		SO_SUN = 0, SO_SUN_GLARE = 1, SO_ATMOSPHERE = 2,
		SO_CLOUDS = 3, SO_SKYQUAD = 4, SO_STARS = 5,
		SO_MOON = 6, SO_MOON_SHADOW = 7,
	};

	NiColorAlpha	kVertexColor;		// +0x60
	NiTexture*		spBaseTexture;		// +0x70
	BSString		strTextureName;		// +0x74
	UInt32			eClampMode;			// +0x7C
	NiTexture*		spBlendTexture;		// +0x80
	float			fBlendValue;		// +0x84
	UInt16			usCloudLayer;		// +0x88
	UInt16			pad8A;
	SkyObject		eSkyObjectType;		// +0x8C
};

class BGSTerrainManager {	// LOD roots
public:
	static NiNode* GetRootLandLODNode() { return *(NiNode**)0x11D86A8; }
	static NiNode* GetRootObjectLODNode() { return *(NiNode**)0x11D8690; }
};

class TESMain {	// the main loop's globals
public:
	static bool IsMenuBackgroundReady() { return *(bool*)0x11DEA29; }
};
