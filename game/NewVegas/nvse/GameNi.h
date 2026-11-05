#pragma once

// Gamebryo side of Fallout: New Vegas, as far as NVR reads or calls it.
//
// Every class here mirrors an object that lives in the running game, so its byte offsets, size and virtual slot
// numbers are facts about FalloutNV.exe and must never move. Those facts come from community reverse engineering
// (the NVSE / xNVSE projects and WallSoGB's symbol map); this file restates them in NVR's own form. Only what NVR
// uses is named: the rest of each object is padding (padXX = the bytes from offset XX on) and numbered placeholder
// virtuals (vslot_N = vtable slot N). nvse/LayoutCheck.h checks every size and named offset when the DLL is built.

// A game virtual NVR never calls, declared only so that the virtuals after it keep their slot number. The name holds
// the slot number, so a derived class can never override one by accident.
#define NVR_VSLOT(n)		virtual void vslot_##n()
#define NVR_VSLOT10(t)		NVR_VSLOT(t##0); NVR_VSLOT(t##1); NVR_VSLOT(t##2); NVR_VSLOT(t##3); NVR_VSLOT(t##4); \
							NVR_VSLOT(t##5); NVR_VSLOT(t##6); NVR_VSLOT(t##7); NVR_VSLOT(t##8); NVR_VSLOT(t##9)
#define NVR_VSLOT100(h)		NVR_VSLOT10(h##0); NVR_VSLOT10(h##1); NVR_VSLOT10(h##2); NVR_VSLOT10(h##3); NVR_VSLOT10(h##4); \
							NVR_VSLOT10(h##5); NVR_VSLOT10(h##6); NVR_VSLOT10(h##7); NVR_VSLOT10(h##8); NVR_VSLOT10(h##9)

class NiNode; class NiTimeController; class NiProperty; class NiAlphaProperty; class NiShadeProperty;
class NiMaterialProperty; class NiStencilProperty; class NiGeometryData; class NiTexture; class NiGeometry;
class NiFrustumPlanes; class NiTriStrips; class NiD3DShader; class NiDX9Renderer; class NiDX9RenderState;
class NiDepthStencilBuffer; class NiRenderTargetGroup; class BSFadeNode; class BSMultiBoundNode;
class BSMultiBoundShape; class BSFogProperty; class ShadowSceneNode; class ShaderRecord;

// ---------------------------------------------------------------------------------------------------------------------
// Run-time type info and the game's allocator
// ---------------------------------------------------------------------------------------------------------------------

struct NiRTTI {	// 0x8 bytes
	const char*		name;		// +0x00
	const NiRTTI*	parent;		// +0x04

	// True when this type is `type` or derives from it (walks the parent chain).
	bool IsKindOf(const NiRTTI* type) const {
		const NiRTTI* walk = this;
		while (walk && walk != type) walk = walk->parent;
		return walk != nullptr;
	}
	bool IsKindOf(const NiRTTI& type) const { return IsKindOf(&type); }
};

// The allocator behind NiMemManager (singleton at 0x11F6080); NVR only calls Allocate (slot 1).
class NiAllocator {	// 0x4 bytes
public:
	NVR_VSLOT(0);
	virtual void* Allocate(size_t& size, size_t& alignment, UInt32 eventType, bool exactSizeOnFree, const char* file, int line, const char* function);
};

// The game's operator new (0xAA13E0): 0 bytes counts as 1, 4-byte alignment, event type 1 (operator new).
inline void* NiNew(size_t size) {
	size_t bytes = size ? size : 1;
	size_t alignment = 4;
	NiAllocator* allocator = **(NiAllocator***)0x11F6080;
	return allocator->Allocate(bytes, alignment, 1, true, nullptr, -1, nullptr);
}

// A T on the game heap, built by the game's constructor at Constructor (zero filled when there is none).
template <typename T, const UInt32 Constructor = 0, typename... Args>
T* NiCreate(Args&&... args) {
	void* memory = NiNew(sizeof(T));
	if constexpr (Constructor != 0)
		ThisStdCall(Constructor, memory, std::forward<Args>(args)...);
	else
		memset(memory, 0, sizeof(T));
	return static_cast<T*>(memory);
}

// ---------------------------------------------------------------------------------------------------------------------
// Maths
// ---------------------------------------------------------------------------------------------------------------------

class NiPoint2 {	// 0x8 bytes
public:
	float x, y;
};

class NiPoint3 {	// 0xC bytes
public:
	float x, y, z;	// +0x00 +0x04 +0x08

	NiPoint3() : x(0.0f), y(0.0f), z(0.0f) {}
	NiPoint3(const float ax, const float ay, const float az) : x(ax), y(ay), z(az) {}

	float operator * (const NiPoint3 other) const { return Dot(other); }
	NiPoint3 operator - (const NiPoint3& other) const { return { x - other.x, y - other.y, z - other.z }; }

	__forceinline float Dot(const NiPoint3& o) const { return x * o.x + y * o.y + z * o.z; }
	__forceinline float Length() const { return std::sqrt(Dot(*this)); }
	__forceinline NiPoint3 Cross(const NiPoint3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }

	// Scales to length 1 and returns the old length; anything not longer than 1e-6 becomes the zero vector (returns 0).
	__forceinline float Unitize() {	// in place
		const float length = Length();
		if (!(length > 1e-06f)) {
			x = y = z = 0.0f;
			return 0.0f;
		}
		const float inverse = 1.0f / length;
		x *= inverse; y *= inverse; z *= inverse;
		return length;
	}

	void GetLookAt(NiPoint3* target, NiPoint3* angles);

	D3DXVECTOR3 toD3DXVEC3() { return D3DXVECTOR3(x, y, z); }
	D3DXVECTOR4 toD3DXVEC4() { return D3DXVECTOR4(x, y, z, 1.0f); }
};

class NiPoint4 {	// 0x10 bytes
public:
	float x, y, z, w;
};

class NiVector4 {	// 0x10 bytes
public:
	float x, y, z, w;

	void Normalize();	// xyz only
};

class NiMatrix33 {	// 0x24 bytes
public:
	float data[3][3];	// [row][column]

	NiMatrix33() {}		// uninitialised
	// The nine values in memory order (row 0 first).
	NiMatrix33(float a0, float a1, float a2, float b0, float b1, float b2, float c0, float c1, float c2) {
		float* out = &data[0][0];
		out[0] = a0; out[1] = a1; out[2] = a2;
		out[3] = b0; out[4] = b1; out[5] = b2;
		out[6] = c0; out[7] = c1; out[8] = c2;
	}

	NiPoint3 operator * (const NiPoint3 p) const {
		return { data[0][0] * p.x + data[0][1] * p.y + data[0][2] * p.z,
				 data[1][0] * p.x + data[1][1] * p.y + data[1][2] * p.z,
				 data[2][0] * p.x + data[2][1] * p.y + data[2][2] * p.z };
	}
	NiMatrix33 operator * (const NiMatrix33 m) const {
		NiMatrix33 out;
		for (int c = 0; c < 3; c++)
			for (int r = 0; r < 3; r++)
				out.data[r][c] = data[r][0] * m.data[0][c] + data[r][1] * m.data[1][c] + data[r][2] * m.data[2][c];
		return out;
	}

	void GenerateRotationMatrixZXY(NiPoint3* angles, bool degrees);
};

class NiTransform {	// 0x34 bytes
public:
	NiMatrix33	rot;		// +0x00
	NiPoint3	pos;		// +0x24
	float		scale;		// +0x30
};

class NiPlane {	// 0x10 bytes
public:
	enum { NoSide = 0, PositiveSide = 1, NegativeSide = 2 };

	NiPoint3	Normal;		// +0x00
	float		Constant;	// +0x0C

	NiPlane() : Normal(), Constant(0.0f) {}
	NiPlane(const NiPoint3& normal, float constant) : Normal(normal), Constant(constant) {}
	NiPlane(const NiPoint3& normal, const NiPoint3& point) : Normal(normal), Constant(normal.Dot(point)) {}

	float Distance(const NiPoint3& point) const { return Normal * point - Constant; }
};

class NiBound {	// 0x10 bytes
public:
	NiPoint3	Center;		// +0x00
	float		Radius;		// +0x0C

	static NiBound* GetGlobalWorldBound() { return (NiBound*)0x11F4288; }

	// NegativeSide when the sphere is wholly behind the plane, else 1 when wholly in front and 0 when it crosses.
	int WhichSide(const NiPlane& plane) const {
		const float distance = plane.Distance(Center);
		if (!(distance > -Radius)) return NiPlane::NegativeSide;
		return Radius <= distance;
	}
	bool WithinFrustum(NiFrustumPlanes* planes);
};

class NiFrustum {	// 0x1C bytes
public:
	float	Left;		// +0x00
	float	Right;		// +0x04
	float	Top;		// +0x08
	float	Bottom;		// +0x0C
	float	Near;		// +0x10
	float	Far;		// +0x14
	bool	Ortho;		// +0x18

	NiFrustum() : NiFrustum(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, false) {}
	NiFrustum(float l, float r, float t, float b, float n, float f, bool ortho) : Left(l), Right(r), Top(t), Bottom(b), Near(n), Far(f), Ortho(ortho) {}
};

class NiFrustumPlanes {	// 0x64 bytes
public:
	enum { NearPlane, FarPlane, LeftPlane, RightPlane, TopPlane, BottomPlane, MaxPlanes };

	NiPlane	CullingPlanes[MaxPlanes];	// +0x00
	UInt32	ActivePlanes;				// +0x60 one bit per plane

	void Set(const NiFrustum& frustum, const NiTransform& transform) { ThisCall(0xA74E10, this, &frustum, &transform); }
	const NiPlane& GetPlane(UInt32 plane) const { return CullingPlanes[plane]; }
	bool IsPlaneActive(UInt32 plane) const { return (ActivePlanes & (1 << plane)) != 0; }
	bool IsAnyPlaneActive() const { return ActivePlanes != 0; }
	void EnablePlane(UInt32 plane) { ActivePlanes |= 1 << plane; }
	void DisablePlane(UInt32 plane) { ActivePlanes &= ~(1 << plane); }
	void SetActivePlaneState(UInt32 state) { ActivePlanes = state; }
};

class NiViewport {	// 0x10 bytes
public:
	float l, r, t, b;
};

class NiColor {	// 0xC bytes
public:
	float r, g, b;
};

class NiColorAlpha {	// 0x10 bytes
public:
	float r, g, b, a;
};

// ---------------------------------------------------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------------------------------------------------

template <typename T> class NiTArray {
public:
	NVR_VSLOT(0);
	T*		data;		// +0x04
	UInt16	capacity;	// +0x08
	UInt16	end;		// +0x0A first free slot of the tail block
	UInt16	numObjs;	// +0x0C
	UInt16	growSize;	// +0x0E

	UInt16 Add(T* item) { return (UInt16)ThisCall(0x00A5EB20, this, item); }
};

template <typename T> class NiTList {
public:
	struct Entry {	// 0x4 bytes
		Entry*	next;	// +0x00
		Entry*	prev;	// +0x04
		T*		data;	// +0x08
	};

	Entry*	start;		// +0x00
	Entry*	end;		// +0x04
	UInt32	numItems;	// +0x08
};

// ---------------------------------------------------------------------------------------------------------------------
// Scene graph
// ---------------------------------------------------------------------------------------------------------------------

class NiRefObject {	// 0x8 bytes
public:
	NVR_VSLOT(0);			// destructor
	NVR_VSLOT(1);
	UInt32	m_uiRefCount;	// +0x04
};

class NiObject : public NiRefObject {	// 0x8 bytes
public:
	virtual const NiRTTI*		GetRTTI() const;			// 2
	virtual NiNode*				IsNiNode();					// 3
	virtual BSFadeNode*			IsFadeNode();				// 4
	virtual BSMultiBoundNode*	IsMultiBoundNode() const;	// 5
	virtual NiGeometryData*		IsGeometry();				// 6
	NVR_VSLOT(7);
	virtual NiTriStrips*		IsTriStrips();				// 8
	NVR_VSLOT(9); NVR_VSLOT10(1); NVR_VSLOT10(2); NVR_VSLOT(30); NVR_VSLOT(31); NVR_VSLOT(32); NVR_VSLOT(33); NVR_VSLOT(34);

	static inline const NiRTTI* const ms_RTTI = (NiRTTI*)0x11F4418;

	bool IsKindOf(const NiRTTI& type) const { return GetRTTI()->IsKindOf(type); }
	bool IsKindOf(const NiRTTI* const type) const { return GetRTTI()->IsKindOf(type); }
	template <class T> bool IsKindOf() const { return IsKindOf(T::ms_RTTI); }
	bool IsExactKindOf(const NiRTTI* const type) const { return GetRTTI() == type; }
};

class NiObjectNET : public NiObject {	// 0x18 bytes
public:
	const char*			m_pcName;		// +0x08
	NiTimeController*	m_controller;	// +0x0C
	UInt32				pad10[2];

	// The game interns the string (0xA5B690) and makes it the object's name (0xA5B950).
	void SetName(const char* Name) {
		char* interned = CdeclCall<char*>(0x00A5B690, Name);
		ThisCall(0x00A5B950, this, &interned);
	}
};

class NiAVObject : public NiObjectNET {	// 0x9C bytes
public:
	NVR_VSLOT(35); NVR_VSLOT(36); NVR_VSLOT(37);
	virtual NiAVObject* GetObjectByName(const char* Name);	// 38
	NVR_VSLOT(39); NVR_VSLOT10(4); NVR_VSLOT(50); NVR_VSLOT(51); NVR_VSLOT(52); NVR_VSLOT(53); NVR_VSLOT(54);

	enum NiFlags : UInt32 {	// m_flags
		APP_CULLED = 0x1,	// not drawn
	};

	NiNode*			m_parent;			// +0x18
	UInt32			pad1C;
	NiBound*		m_kWorldBound;		// +0x20
	UInt32			pad24[3];			// property list
	UInt32			m_flags;			// +0x30 NiFlags
	NiTransform		m_localTransform;	// +0x34
	NiTransform		m_worldTransform;	// +0x68

	float GetDistance(NiPoint3* Point);	// from the world position
	NiBound* GetWorldBound() const { return m_kWorldBound ? m_kWorldBound : NiBound::GetGlobalWorldBound(); }
	float GetWorldBoundRadius() { return m_kWorldBound ? m_kWorldBound->Radius : 0.0f; }
	bool WithinFrustum(NiFrustumPlanes* planes) { return GetWorldBound()->WithinFrustum(planes); }
};

class NiNode : public NiAVObject {	// 0xAC bytes
public:
	virtual void AddObject(NiAVObject* child, UInt8 firstAvailable);	// 55

	NiTArray<NiAVObject*>	m_children;	// +0x9C

	void New(UInt16 Children) { ThisCall(0x00A5ECB0, this, Children); }	// the game's constructor
};

class BSMultiBound : public NiObject {	// 0x10 bytes
public:
	UInt32				pad08;
	BSMultiBoundShape*	spShape;	// +0x0C
};

class BSMultiBoundShape : public NiObject {	// 0xC bytes
public:
	NVR_VSLOT(35); NVR_VSLOT(36); NVR_VSLOT(37); NVR_VSLOT(38);
	virtual bool WithinFrustum(NiFrustumPlanes& planes) const;	// 39

	UInt32 pad08;
};

class BSMultiBoundNode : public NiNode {	// 0xB4 bytes
public:
	BSMultiBound*	spMultiBound;	// +0xAC
	UInt32			padB0;
};

class NiSwitchNode : public NiNode {	// 0xCC bytes
public:
	UInt16		m_usFlags;		// +0xAC
	SInt32		m_iIndex;		// +0xB0 active child
	UInt32		padB4[6];

	static inline const NiRTTI* const ms_RTTI = (NiRTTI*)0x11F5EB4;
};

class NiCamera : public NiAVObject {	// 0x114 bytes
public:
	UInt32			pad9C[16];	// world to camera matrix
	NiFrustum		Frustum;	// +0xDC
	UInt32			padF8[2];
	NiViewport		ViewPort;	// +0x100
	float			LODAdjust;	// +0x110
};

class SceneGraph : public NiNode {	// 0xC0 bytes
public:
	NiCamera*		camera;		// +0xAC
	UInt32			padB0[3];
	float			cameraFOV;	// +0xBC

	void UpdateParticleShaderFoV(float fov);
	void SetNearDistance(float Distance) { *(float*)0x01203148 = Distance; }
};

class NiDynamicEffect : public NiAVObject {	// 0xC4 bytes
public:
	enum EffectTypes {	// EffectType
		POINT_LIGHT = 2, SPOT_LIGHT = 4,
	};

	UInt8		On;				// +0x9C
	UInt8		EffectType;		// +0x9D EffectTypes
	UInt8		CastShadows;	// +0x9E
	UInt8		CanCarry;		// +0x9F
	UInt32		Index;			// +0xA0
	UInt32		padA4[8];
};

class NiLight : public NiDynamicEffect {	// 0xF0 bytes
public:
	float		Dimmer;		// +0xC4
	NiColor		Amb;		// +0xC8
	NiColor		Diff;		// +0xD4
	NiColor		Spec;		// +0xE0
	UInt32		padEC;
};

class NiPointLight : public NiLight {	// 0xFC bytes
public:
	float		Atten0;		// +0xF0
	float		Atten1;		// +0xF4
	float		Atten2;		// +0xF8
};

class NiDirectionalLight : public NiLight {	// 0xFC bytes
public:
	NiPoint3	direction;	// +0xF0
};

class NiSpotLight : public NiPointLight {	// 0x114 bytes
public:
	NiPoint3	direction;		// +0xFC
	float		OuterSpotAngle;	// +0x108
	float		InnerSpotAngle;	// +0x10C
	float		SpotExponent;	// +0x110

	static NiSpotLight* CreateObject() { return StdCall<NiSpotLight*>(0xA94E20); }
};

// ---------------------------------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------------------------------

class NiVBChip {	// 0x20 bytes
public:
	UInt32					Index;		// +0x00
	UInt32					pad04;
	IDirect3DVertexBuffer9*	VB;			// +0x08
	UInt32					Offset;		// +0x0C
	UInt32					LockFlags;	// +0x10
	UInt32					Size;		// +0x14
	NiVBChip*				Next;		// +0x18
	NiVBChip*				Prev;		// +0x1C
};

class NiGeometryBufferData {	// 0x54 bytes
public:
	NVR_VSLOT(0);
	NVR_VSLOT(1);
	UInt32							Flags;				// +0x04
	UInt32							pad08;
	UInt32							FVF;				// +0x0C
	IDirect3DVertexDeclaration9*	VertexDeclaration;	// +0x10
	UInt32							pad14;
	UInt32							VertCount;			// +0x18
	UInt32							MaxVertCount;		// +0x1C
	UInt32							StreamCount;		// +0x20
	UInt32*							VertexStride;		// +0x24 per stream
	NiVBChip**						VBChip;				// +0x28 per stream
	UInt32							IndexCount;			// +0x2C
	UInt32							pad30;
	IDirect3DIndexBuffer9*			IB;					// +0x34
	UInt32							BaseVertexIndex;	// +0x38
	D3DPRIMITIVETYPE				PrimitiveType;		// +0x3C
	UInt32							TriCount;			// +0x40
	UInt32							pad44;
	UInt32							NumArrays;			// +0x48 strips
	UInt16*							ArrayLengths;		// +0x4C
	UInt16*							IndexArray;			// +0x50
};

class NiGeometryData : public NiObject {	// 0x40 bytes
public:
	NVR_VSLOT(35); NVR_VSLOT(36);
	virtual UInt32 IsStripsData() const;	// 37

	enum KeepFlags {	// m_ucKeepFlags: CPU copies the game keeps
		KEEP_XYZ = 1 << 0,	// positions
	};

	UInt16					m_usVertices;		// +0x08
	UInt16					pad0A;
	UInt16					m_usDataFlags;		// +0x0C
	UInt16					m_usDirtyFlags;		// +0x0E
	NiBound					m_kBound;			// +0x10
	NiPoint3*				m_pkVertex;			// +0x20
	NiPoint3*				m_pkNormal;			// +0x24
	NiColorAlpha*			m_pkColor;			// +0x28
	NiPoint2*				m_pkTexture;		// +0x2C
	UInt32					pad30;
	NiGeometryBufferData*	m_pkBuffData;		// +0x34
	UInt8					m_ucKeepFlags;		// +0x38 KeepFlags
	UInt8					pad39[7];
};

class NiSkinPartition : public NiObject {	// 0x10 bytes
public:
	class Partition {	// 0x2C bytes
	public:
		void**					_vtbl;			// +0x00
		UInt16*					pBones;			// +0x04
		float*					Weights;		// +0x08
		UInt32					pad0C[4];
		UInt16					Vertices;		// +0x1C
		UInt16					Triangles;		// +0x1E
		UInt16					Bones;			// +0x20
		UInt16					Strips;			// +0x22
		UInt16					BonesPerVertex;	// +0x24
		UInt16					pad26;
		NiGeometryBufferData*	BuffData;		// +0x28
	};

	UInt32		PartitionsCount;	// +0x08
	Partition*	Partitions;			// +0x0C
};

class NiSkinData : public NiObject {	// 0x48 bytes
public:
	class BoneData {	// 0x4C bytes
	public:
		NiTransform		SkinToBone;	// +0x00
		NiBound			Bound;		// +0x34
		void*			pad44;
		UInt16			Verts;		// +0x48
		UInt16			pad4A;
	};

	NiSkinPartition*	SkinPartition;	// +0x08
	NiTransform			pad0C;			// root parent to skin
	BoneData*			BoneData;		// +0x40
	UInt32				Bones;			// +0x44
};

class NiSkinInstance : public NiObject {	// 0x34 bytes
public:
	NiSkinData*			SkinData;				// +0x08
	NiSkinPartition*	SkinPartition;			// +0x0C
	NiAVObject*			RootParent;				// +0x10
	NiAVObject**		BoneObjects;			// +0x14
	UInt32				pad18;
	UInt32				Bones;					// +0x1C
	UInt32				pad20[2];
	void*				BoneMatrixes;			// +0x28 one D3DMATRIX per bone
	void*				SkinToWorldWorldToSkin;	// +0x2C D3DMATRIX
	UInt32				pad30;
};

class DismemberPartition {	// 0x4 bytes
public:
	UInt8	Enabled;	// +0x00
	UInt8	StartCap;	// +0x01
	UInt16	bodyPart;	// +0x02
};

class BSDismemberSkinInstance : public NiSkinInstance {	// 0x40 bytes
public:
	UInt32				partitionNumber;	// +0x34
	DismemberPartition*	partitions;			// +0x38
	UInt8				IsRenderable;		// +0x3C the OR of every partition's Enabled
	UInt8				pad3D[3];

	static inline const NiRTTI* const ms_RTTI = (NiRTTI*)0x11F49D8;
};

// ---------------------------------------------------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------------------------------------------------

class NiProperty : public NiObjectNET {	// 0x18 bytes
public:
	enum PropertyType {	// index into NiPropertyState
		kType_Alpha = 0, kType_Material = 2, kType_Shade = 3,
		kType_Stencil = 4,
	};
};

class NiStencilProperty : public NiProperty {	// 0x24 bytes
public:
	enum { TEST_MAX = 8, ACTION_MAX = 6 };
	enum { ENABLE_MASK = 0x1, DRAWMODE_MASK = 0xC00, DRAWMODE_POS = 10 };
	enum DrawMode {	// which faces are drawn
		DRAW_CCW_OR_BOTH = 0, DRAW_CCW = 1, DRAW_CW = 2,
		DRAW_BOTH = 3,	// no culling
		DRAW_MAX
	};

	Bitfield16	m_usFlags;	// +0x18
	UInt32		m_uiRef;	// +0x1C
	UInt32		m_uiMask;	// +0x20

	DrawMode GetDrawMode() const { return (DrawMode)m_usFlags.GetField(DRAWMODE_MASK, DRAWMODE_POS); }
};

class NiPropertyState {	// 0x1C bytes
public:
	enum { MAX = 7 };
	union {
		struct {
			NiAlphaProperty*	m_spAlphaProperty;		// +0x00
			NiProperty*			m_spCullingProperty;	// +0x04
			NiMaterialProperty*	m_spMaterialProperty;	// +0x08
			NiShadeProperty*	m_spShadeProperty;		// +0x0C
			NiStencilProperty*	m_spStencilProperty;	// +0x10
			NiProperty*			m_spTextureProperty;	// +0x14
			NiProperty*			m_spUnknownProperty;	// +0x18
		};
		NiProperty*	m_aspProps[MAX];	// by NiProperty::PropertyType
	};

	template <class T> T* GetShadeProperty() const { return static_cast<T*>(m_spShadeProperty); }
};

class NiGeometry : public NiAVObject {	// 0xC4 bytes
public:
	NiPropertyState		propertyState;	// +0x9C
	NiGeometryData*		geomData;		// +0xB8
	NiSkinInstance*		skinInstance;	// +0xBC
	NiD3DShader*		shader;			// +0xC0

	NiProperty* GetProperty(NiProperty::PropertyType Type) { return propertyState.m_aspProps[Type]; }
};

class NiAlphaProperty : public NiProperty {	// 0x1C bytes
public:
	enum AlphaFlags {	// flags
		ALPHA_BLEND_MASK = 0x0001, TEST_ENABLE_MASK = 0x0200,
	};

	UInt16	flags;			// +0x18 AlphaFlags
	UInt8	alphaTestRef;	// +0x1A
	UInt8	pad1B;
};

class NiMaterialProperty : public NiProperty {	// 0x4C bytes
public:
	UInt32		pad18;
	NiColor		spec;		// +0x1C
	NiColor		emit;		// +0x28
	UInt32		pad34;
	float		fShine;		// +0x38
	float		fAlpha;		// +0x3C
	float		fEmitMult;	// +0x40
	UInt32		pad44[2];
};

class NiShadeProperty : public NiProperty {	// 0x20 bytes
public:
	enum ShaderPropType : UInt32 {
		kProp_Lighting = 0x1, kProp_SpeedTreeLeaf = 0x6, kProp_PPLighting = 0x8,
		kProp_Hair = 0x9, kProp_SpeedTreeBranch = 0xA, kProp_Lighting30 = 0xC,
		kProp_Water = 0xE,	// m_eShaderType
	};
	enum ExtraFlags {	// m_usFlags
		kFirstPerson	= 1 << 1,	// the player's view model
		kThirdPerson	= 1 << 2,	// the player's world model
	};

	Bitfield16		m_usFlags;		// +0x18 ExtraFlags
	ShaderPropType	m_eShaderType;	// +0x1C
};

// ---------------------------------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------------------------------

class NiDX9TextureData : public NiObject {	// 0x6C bytes
public:
	NiTexture*				parent;		// +0x08
	UInt32					width;		// +0x0C
	UInt32					height;		// +0x10
	UInt32					pad14[20];	// pixel format, renderer
	IDirect3DBaseTexture9*	dTexture;	// +0x64
	UInt32					Levels;		// +0x68
};

class NiDX9SourceTextureData : public NiDX9TextureData {	// 0x84 bytes
public:
	UInt32 pad6C[6];
};

class NiTexture : public NiObjectNET {	// 0x30 bytes
public:
	struct FormatPrefs {	// 0xC bytes
		UInt32	pixelLayout;	// +0x00
		UInt32	alphaFormat;	// +0x04
		UInt32	mipmapFormat;	// +0x08
	};

	FormatPrefs			formatPrefs;	// +0x18
	NiDX9TextureData*	rendererData;	// +0x24
	NiTexture*			nextTex;		// +0x28
	NiTexture*			prevTex;		// +0x2C

	NiDX9TextureData* GetDX9RendererData() { return rendererData; }
};

class NiSourceTexture : public NiTexture {	// 0x48 bytes
public:
	char*		ddsPath1;	// +0x30
	char*		ddsPath2;	// +0x34
	UInt32		unk38;		// +0x38
	UInt32		pad3C[3];
};

class NiRenderedTexture : public NiTexture {	// 0x40 bytes
public:
	void*		buffer;		// +0x30 Ni2DBuffer*
	UInt32		pad34[3];
};

// ---------------------------------------------------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------------------------------------------------

class NiShaderDeclaration : public NiObject {	// 0x8 bytes
public:
	enum ShaderParameter : UInt32 {
		SHADERPARAM_NI_POSITION = 0x0, SHADERPARAM_NI_NORMAL = 0x3, SHADERPARAM_NI_COLOR = 0x4,
		SHADERPARAM_NI_TEXCOORD0 = 0x5, SHADERPARAM_EXTRA_DATA_MASK = 0x80000000,
	};
	enum ShaderParameterType {
		SPTYPE_FLOAT1 = 0x0, SPTYPE_FLOAT2 = 0x1, SPTYPE_FLOAT3 = 0x2,
		SPTYPE_FLOAT4 = 0x3,	// D3D float1..float4
	};
	enum ShaderParameterTesselator {
		SPTESS_DEFAULT = 0x0,	// no tessellation
	};
	enum ShaderParameterUsage {
		SPUSAGE_TEXCOORD = 0x5,	// D3DDECLUSAGE_TEXCOORD
	};

	NVR_VSLOT(35);	// destructor
	virtual bool SetEntry(UInt32 entry, UInt32 reg, ShaderParameter input, ShaderParameterType type, UInt32 stream);	// 36
};

class NiD3DShaderDeclaration : public NiShaderDeclaration {	// 0x2C bytes
public:
	NVR_VSLOT(37);
	virtual bool SetEntryAlt(UInt32 stream, UInt32 entry, UInt32 reg, ShaderParameter input, ShaderParameterType type,
							 ShaderParameterUsage usage, UInt32 index, ShaderParameterTesselator tesselation);	// 38
	NVR_VSLOT(39); NVR_VSLOT(40); NVR_VSLOT(41); NVR_VSLOT(42); NVR_VSLOT(43); NVR_VSLOT(44); NVR_VSLOT(45);
	virtual LPDIRECT3DVERTEXDECLARATION9 GetD3DDeclaration();	// 46

	UInt32				pad08[2];
	IDirect3DDevice9*	Device;			// +0x10
	UInt32				pad14[3];
	UInt32				StreamCount;	// +0x20
	UInt32				pad24[2];
};

class NiDX9ShaderDeclaration : public NiD3DShaderDeclaration {	// 0xD4 bytes
public:
	UInt32 pad2C[42];

	static NiDX9ShaderDeclaration* Create(NiDX9Renderer* renderer, UInt32 maxStreamEntries, UInt32 streams) {
		return CdeclCall<NiDX9ShaderDeclaration*>(0xE76700, renderer, maxStreamEntries, streams);
	}
};

class NiD3DShaderProgram : public NiRefObject {	// 0x2C bytes
public:
	UInt8					ucProgramType;		// +0x08 0 vertex, 1 pixel
	UInt8					pad09[2];
	bool					Enabled;			// +0x0B
	const char* const		Name;				// +0x0C
	ShaderRecord*			ShaderProg[3];		// +0x10 NVR's records for this program
	IUnknown*				ShaderHandleBackup;	// +0x1C the game's own D3D shader
	IDirect3DDevice9*		Device;				// +0x20
	NiDX9Renderer*			Renderer;			// +0x24
	NiDX9RenderState*		RenderState;		// +0x28

	ShaderRecord* GetShaderRecord(int Type) { return ShaderProg[Type]; }
	void SetName(const char* name) { ThisCall(0xBE0920, this, name); }
};

class NiD3DVertexShader : public NiD3DShaderProgram {	// 0x3C bytes
public:
	UInt8							SoftwareVP;		// +0x2C
	UInt8							pad2D[3];
	UInt32							Usage;			// +0x30
	IDirect3DVertexShader9*			ShaderHandle;	// +0x34
	IDirect3DVertexDeclaration9*	Declaration;	// +0x38
};

class NiD3DPixelShader : public NiD3DShaderProgram {	// 0x30 bytes
public:
	IDirect3DPixelShader9*	ShaderHandle;	// +0x2C
};

class NiD3DPass {	// 0x68 bytes
public:
	void**					_vtbl;			// +0x00
	char					Name[16];		// +0x04
	UInt32					pad14;
	UInt32					StageCount;		// +0x18
	UInt32					pad1C[10];
	NiD3DPixelShader*		PixelShader;	// +0x44
	UInt32					pad48[5];
	NiD3DVertexShader*		VertexShader;	// +0x5C
	UInt32					pad60[2];
};

class NiShader : public NiRefObject {	// 0x14 bytes
public:
	NVR_VSLOT(2);	// destructor
	NVR_VSLOT(3); NVR_VSLOT(4); NVR_VSLOT(5); NVR_VSLOT(6); NVR_VSLOT(7); NVR_VSLOT(8); NVR_VSLOT(9); NVR_VSLOT10(1);
	virtual void Initialize();	// 20
	NVR_VSLOT(21); NVR_VSLOT(22); NVR_VSLOT(23); NVR_VSLOT(24); NVR_VSLOT(25); NVR_VSLOT(26); NVR_VSLOT(27); NVR_VSLOT(28);
	NVR_VSLOT(29); NVR_VSLOT10(3);
	NVR_VSLOT(40); NVR_VSLOT(41); NVR_VSLOT(42); NVR_VSLOT(43); NVR_VSLOT(44); NVR_VSLOT(45); NVR_VSLOT(46); NVR_VSLOT(47);

	char*	Name;		// +0x08
	UInt32	pad0C[2];
};

class NiD3DShaderInterface : public NiShader {	// 0x24 bytes
public:
	NVR_VSLOT(48);
	virtual void SetShaderDecl(NiD3DShaderDeclaration* declaration);	// 49

	IDirect3DDevice9*	D3DDevice;	// +0x14
	UInt32				pad18[3];
};

class NiD3DShader : public NiD3DShaderInterface {	// 0x3C bytes
public:
	UInt32					pad24;
	NiD3DShaderDeclaration*	ShaderDeclaration;	// +0x28
	UInt32					pad2C[4];
};

class BSShader : public NiD3DShader {	// 0x6C bytes
public:
	NVR_VSLOT10(5); NVR_VSLOT10(6); NVR_VSLOT(70); NVR_VSLOT(71);
	virtual void InitShaderConstants();	// 72

	UInt32		pad3C[8];	// default shader state
	NiD3DPass*	pass;		// +0x5C
	UInt32		pad60[3];
};

class ShadowLightShader : public BSShader {	// 0x8C bytes
public:
	UInt32 pad6C[8];

	static void EnableEyePositionForAllPasses();	// GameNi.cpp
};

class GeometryDecalShader : public BSShader {	// 0x90 bytes
public:
	UInt32 pad6C[9];

	static GeometryDecalShader* __stdcall CreateShader() { return StdCall<GeometryDecalShader*>(0xBCC0A0); }
};

class TallGrassShader : public BSShader {	// 0x1E0 bytes
public:
	NVR_VSLOT(73); NVR_VSLOT(74); NVR_VSLOT(75); NVR_VSLOT(76); NVR_VSLOT(77); NVR_VSLOT(78); NVR_VSLOT(79);
	virtual void InitShaders();	// 80
	virtual void InitPasses();	// 81

	UInt32			pad6C[78];			// passes, wind, shaders
	NiPoint4*		pInstanceData;		// +0x1A4 the per-tuft instance table
	UInt32			pad1A8;
	UInt16			ucInstanceCount;	// +0x1AC
	UInt16			pad1AE;
	UInt32			pad1B0[12];

	static TallGrassShader* Create(NiDX9ShaderDeclaration* declaration) { return NiCreate<TallGrassShader, 0xBACD00>(declaration); }
	static TallGrassShader* __stdcall CreateShader();	// game 0xBACF00, restated
	static TallGrassShader* GetShader();					// the game's instance (made on first use)
};

// ---------------------------------------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------------------------------------

class NiDX92DBufferData : public NiRefObject {	// 0x18 bytes
public:
	UInt32				pad08[3];
	IDirect3DSurface9*	Surface;	// +0x14
};

class Ni2DBuffer : public NiObject {	// 0x14 bytes
public:
	UInt32				width;		// +0x08
	UInt32				height;		// +0x0C
	NiDX92DBufferData*	data;		// +0x10
};

class NiRenderTargetGroup : public NiObject {	// 0x28 bytes
public:
	UInt32			pad08;
	Ni2DBuffer*		RenderTargets[4];	// +0x0C
	UInt32			pad1C[3];
};

class NiDX9RenderState : public NiRefObject {	// 0x1248 bytes
public:
	NVR_VSLOT(2); NVR_VSLOT(3); NVR_VSLOT(4); NVR_VSLOT(5); NVR_VSLOT(6); NVR_VSLOT(7); NVR_VSLOT(8); NVR_VSLOT(9);
	NVR_VSLOT10(1); NVR_VSLOT(20); NVR_VSLOT(21); NVR_VSLOT(22); NVR_VSLOT(23); NVR_VSLOT(24); NVR_VSLOT(25);
	virtual void					SetRenderState(D3DRENDERSTATETYPE state, UInt32 value, UInt8 arg3, UInt8 arg4);			// 26
	virtual UInt32					GetRenderState(D3DRENDERSTATETYPE state);											// 27
	NVR_VSLOT(28); NVR_VSLOT(29); NVR_VSLOT(30);
	virtual void					SetPixelShader(IDirect3DPixelShader9* shader, UInt8 backUp);						// 31
	virtual IDirect3DPixelShader9*	GetPixelShader();																	// 32
	NVR_VSLOT(33); NVR_VSLOT(34);
	virtual void					SetVertexShader(IDirect3DVertexShader9* shader, UInt8 backUp);						// 35
	virtual IDirect3DVertexShader9*	GetVertexShader();																	// 36
	NVR_VSLOT(37); NVR_VSLOT(38);
	virtual void					SetFVF(UInt32 fvf, UInt8 backUp);													// 39
	NVR_VSLOT(40); NVR_VSLOT(41); NVR_VSLOT(42);
	virtual void					SetVertexDeclaration(IDirect3DVertexDeclaration9* declaration, UInt8 backUp);		// 43
	NVR_VSLOT(44); NVR_VSLOT(45); NVR_VSLOT(46); NVR_VSLOT(47);
	virtual void					SetTextureStageState(UInt32 stage, D3DTEXTURESTAGESTATETYPE type, UInt32 value, UInt8 backUp);	// 48
	virtual UInt32					GetTextureStageState(UInt32 stage, D3DTEXTURESTAGESTATETYPE type);					// 49
	NVR_VSLOT(50);
	virtual HRESULT					SetSamplerState(UInt32 sampler, D3DSAMPLERSTATETYPE type, UInt32 value, UInt8 backUp);	// 51
	virtual UInt32					GetSamplerState(UInt32 sampler, D3DSAMPLERSTATETYPE type);							// 52
	NVR_VSLOT(53); NVR_VSLOT(54);
	virtual void					SetTexture(UInt32 sampler, IDirect3DBaseTexture9* texture);							// 55
	virtual IDirect3DBaseTexture9*	GetTexture(UInt32 sampler);															// 56

	struct NiRenderStateSetting {	// 0x8 bytes
		UInt32 CurrentValue;	// +0x00
		UInt32 PreviousValue;	// +0x04
	};

	UInt32					Flags;						// +0x08
	UInt32					pad0C[50];
	D3DCULL					m_auiCullModeMapping[4][2];	// +0xD4 [NiStencilProperty::DrawMode][LeftHanded]
	UInt32					LeftHanded;					// +0xF4
	UInt32					padF8[10];
	NiRenderStateSetting	RenderStateSettings[256];	// +0x120
	UInt32					pad920[480];
	UInt32					unk10A0[20];				// +0x10A0 NVR notes the bound D3D textures here (NoteDeviceTexture)
	UInt32					pad10F0[2];
	IDirect3DDevice9*		Device;						// +0x10F8
	UInt32					pad10FC[7];
	D3DCAPS9				Caps;						// +0x1118

	void SetCullMode(NiStencilProperty::DrawMode mode) { SetRenderState(D3DRS_CULLMODE, m_auiCullModeMapping[mode][LeftHanded], 0, 0); }
	void NoteDeviceTexture(UInt32 stage, IDirect3DBaseTexture9* texture) { if (stage < 16) unk10A0[stage] = (UInt32)texture; }
};

class NiRenderer : public NiObject {	// 0x280 bytes
public:
	NVR_VSLOT(35); NVR_VSLOT(36); NVR_VSLOT(37); NVR_VSLOT(38); NVR_VSLOT(39);
	NVR_VSLOT10(4); NVR_VSLOT10(5); NVR_VSLOT10(6); NVR_VSLOT10(7); NVR_VSLOT10(8);
	NVR_VSLOT(90); NVR_VSLOT(91); NVR_VSLOT(92); NVR_VSLOT(93); NVR_VSLOT(94); NVR_VSLOT(95); NVR_VSLOT(96); NVR_VSLOT(97);
	virtual void Clear(float* rect, UInt32 flags);	// 98

	enum ClearFlags {	// Clear
		kClear_ZBUFFER = 0x4,	// depth
	};

	UInt32 pad08[158];
};

class NiDX9Renderer : public NiRenderer {	// 0xB80 bytes
public:
	UInt32					pad280[2];
	IDirect3DDevice9*		device;				// +0x288
	UInt32					pad28C[77];
	HWND					m_kWndFocus;		// +0x3C0
	UInt32					pad3C4[136];
	float					m_fZClear;			// +0x5E4
	UInt32					pad5E8[31];
	NiBound					m_kWorldBound;		// +0x664
	UInt32					pad674[22];
	D3DVIEWPORT9			m_kD3DPort;			// +0x6CC
	UInt32					pad6E4[105];
	NiRenderTargetGroup*	currentRTGroup;		// +0x888
	UInt32					pad88C[11];
	NiDX9RenderState*		renderState;		// +0x8B8
	UInt32					pad8BC[33];
	D3DXMATRIXA16			worldMatrix;		// +0x940
	D3DXMATRIX				viewMatrix;			// +0x980
	D3DXMATRIX				projMatrix;			// +0x9C0
	D3DXMATRIX				padA00;				// view-projection
	D3DXMATRIX				invViewMatrix;		// +0xA40
	UInt32					padA80[6];
	UInt32					width;				// +0xA98
	UInt32					height;				// +0xA9C
	UInt32					flags;				// +0xAA0
	UInt32					padAA4[55];

	void SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) { renderState->SetSamplerState(sampler, type, value, false); }
	void PackGeometryBuffer(NiGeometryBufferData*, NiGeometryData*, NiSkinInstance*, NiD3DShaderDeclaration*) {}	// empty on purpose
	void CalculateBoneMatrixes(NiSkinInstance* SkinInstance, NiTransform* WorldTrasform) { ThisCall(0x00E6FE30, this, SkinInstance, WorldTrasform, false, 3, true); }

	static NiDX9Renderer* GetSingleton() { return *(NiDX9Renderer**)0x11C73B4; }
};

class BSRenderedTexture : public NiObject {	// 0x40 bytes
public:
	UInt32				pad08[10];				// render target groups, type
	NiRenderedTexture*	RenderedTextures[4];	// +0x30

	NiRenderedTexture* GetTexture(const UInt32 index) { return RenderedTextures[index]; }
	IDirect3DTexture9* GetD3DTexture(const UINT32 index) { return (IDirect3DTexture9*)GetTexture(index)->GetDX9RendererData()->dTexture; }
};

class BSShaderManager {	// statics only
public:
	enum SceneGraphType : UInt32 {	// GetShadowSceneNode
		BSSM_SSN_WORLD = 0, BSSM_SSN_MENU_OBJECT_3D = 1, BSSM_SSN_UNK_2 = 2,
		BSSM_SSN_MENU_PLAYER_3D = 3, BSSM_SSN_COUNT = 4,
	};
	enum ShaderType : UInt32 {	// pspShaders index
		BSSM_SHADER_TALL_GRASS = 2, BSSM_SHADER_GEOM_DECAL = 16, BSSM_SHADER_VOLUMETRIC_FOG = 34,
	};

	static inline float* const		fLODLandDrop			= (float*)0x11AD808;
	static inline NiPoint3* const	kCameraPos				= (NiPoint3*)0x11F474C;
	static inline NiPoint4* const	kLoadedRange			= (NiPoint4*)0x11F95F4;
	static inline float* const		fSpecularLODStartFade	= (float*)0x11F9454;
	static inline float* const		fSpecularLODEnd			= (float*)0x11F9458;
	static inline BSShader** const	pspShaders				= (BSShader**)0x11F9548;	// by ShaderType

	static ShadowSceneNode* GetShadowSceneNode(UInt32 type) { return ((ShadowSceneNode**)0x11F91C8)[type]; }
	static NiDX9Renderer* GetRenderer() { return *(NiDX9Renderer**)0x11F9508; }
	static BSShader* GetShader(ShaderType type);
};

// ---------------------------------------------------------------------------------------------------------------------
// Bethesda nodes and lights
// ---------------------------------------------------------------------------------------------------------------------

class BSFadeNode : public NiNode {	// 0xE4 bytes
public:
	UInt32		padAC[3];
	float		FadeAlpha;		// +0xB8
	float		BoundRadius;	// +0xBC
	UInt32		padC0[9];
};

class BSTreeModel : public NiRefObject {	// 0x50 bytes
public:
	UInt32				pad08[10];
	NiSourceTexture*	LeavesTexture;	// +0x30
	UInt32				pad34[7];
};

class BSTreeNode : public BSFadeNode {	// 0xF8 bytes
public:
	BSTreeModel*	TreeModel;	// +0xE4
	UInt32			padE8[4];
};

class ShadowSceneLight : public NiRefObject {	// 0x250 bytes
public:
	UInt32					pad08[50];
	float					fLODDimmer;		// +0xD0
	float					fFade;			// +0xD4
	UInt32					padD8[2];
	NiTList<NiGeometry>		kGeometryList;	// +0xE0
	UInt32					padEC[2];
	bool					bPointLight;	// +0xF4
	UInt8					padF5[3];
	NiPointLight*			sourceLight;	// +0xF8
	UInt32					padFC[5];
	UInt16					bIsEnabled;		// +0x110
	UInt16					pad112;
	UInt32					pad114[79];
};

class ShadowSceneNode : public NiNode {	// 0x200 bytes
public:
	UInt32						padAC[2];
	NiTList<ShadowSceneLight>	lights;				// +0xB4
	UInt32						padC0[8];
	ShadowSceneLight*			directionalLight;	// +0xE0
	UInt32						padE4[20];
	BSFogProperty*				fogProperty;		// +0x134
	UInt32						pad138[50];
};
