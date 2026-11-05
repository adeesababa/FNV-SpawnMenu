#include "SpawnMenu.h"

// 2026-11-04 Spawn menu, the bake: draws one form's model into a kThumbSize x kThumbSize picture (see SpawnMenu.h).
// Does: loads the model with the game's ModelLoader::LoadFile (0x00447080, synchronous on the calling thread, so a queued
// load is never waited on and retry is never asked), walks its NiNode tree (hidden nodes and collision skipped), draws every
// NiGeometry in bind pose with its own vertex and index buffers through a small shader (diffuse texture from the lighting
// property, else flat grey, times a key, fill and ambient light) from a 3/4 view fitted to the union of the bounds. Each
// RenderEntry returns a new D3DPOOL_DEFAULT render target; the caller releases it.
// Does not handle: NPCs (no picture), alpha blending and cutouts (drawn opaque), skin bones (bind pose), LOD and particle
// geometry (skipped unless the game packed buffers for it), geometry without buffers yet (skipped and counted in the log),
// and holds on the model: LoadFile takes no reference here (abDontIncrementRefCount), so the game may unload the model
// later; the picture is taken in the same call as the load, so it never reads a freed model.
// Game memory is only touched inside the SEH helpers (LoadAndCollectSEH, DrawSEH); no C++ objects live in those functions.

namespace {
	const char* kVertexSource =
		"float4 R0 : register(c0);\n"
		"float4 R1 : register(c1);\n"
		"float4 R2 : register(c2);\n"
		"float4 R3 : register(c3);\n"
		"float4 N0 : register(c4);\n"
		"float4 N1 : register(c5);\n"
		"float4 N2 : register(c6);\n"
		"struct VIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };\n"
		"struct VOut { float4 pos : POSITION; float3 nrm : TEXCOORD0; float2 uv : TEXCOORD1; };\n"
		"VOut main(VIn IN) {\n"
		"  VOut o; float4 p = float4(IN.pos, 1.0f);\n"
		"  o.pos = p.x * R0 + p.y * R1 + p.z * R2 + p.w * R3;\n"	// row vectors: position times the rows of world * view * projection
		"  o.nrm = IN.nrm.x * N0.xyz + IN.nrm.y * N1.xyz + IN.nrm.z * N2.xyz;\n"
		"  o.uv = IN.uv; return o; }\n";
	const char* kPixelSource =
		"sampler2D Diffuse : register(s0);\n"
		"float4 Key : register(c0);\n"		// xyz direction to the light, w intensity
		"float4 Fill : register(c1);\n"
		"float4 Ambient : register(c2);\n"	// x ambient level
		"float4 Flags : register(c3);\n"	// x 1 when a texture is bound
		"float4 Plain : register(c4);\n"	// the colour when there is none
		"struct PIn { float3 nrm : TEXCOORD0; float2 uv : TEXCOORD1; };\n"
		"float4 main(PIn IN) : COLOR0 {\n"
		"  float3 n = IN.nrm; float l2 = dot(n, n);\n"
		"  n = l2 > 1e-8f ? n * rsqrt(l2) : float3(0.0f, 0.0f, 1.0f);\n"
		"  float4 tex = tex2D(Diffuse, IN.uv);\n"
		"  float3 base = lerp(Plain, tex, Flags.x).rgb;\n"
		"  float light = Ambient.x + Key.w * abs(dot(n, Key.xyz)) + Fill.w * abs(dot(n, Fill.xyz));\n"
		"  return float4(base * light, 1.0f); }\n";

	const int kMaxGeo = 512;	// geometry per model the bake keeps (more than this: the rest are not drawn)

	struct GeoItem {
		NiGeometry*	geo;
		D3DXMATRIX	toRoot;	// the geometry's local space to the model's root (row vectors)
		float		center[3];	// its bound, in the root's space
		float		radius;
	};
	struct Job {
		GeoItem	items[kMaxGeo];
		int		count;
		int		skipped;	// geometry with no buffers yet
	};
	struct Lights {
		float key[4], fill[4], ambient[4], plain[4];
	};

	Job gJob;
	IDirect3DVertexShader9* gVS = nullptr;
	IDirect3DPixelShader9* gPS = nullptr;
	IDirect3DSurface9* gDepth = nullptr;
	IDirect3DStateBlock9* gSaved = nullptr;
	IDirect3DSurface9* gRT0 = nullptr;
	IDirect3DSurface9* gDS = nullptr;
	D3DVIEWPORT9 gVP;
	bool gBegun = false;
	int gEntries = 0;	// bakes so far this session (the first few are logged)
	bool gLoggedFail = false, gLoggedSEH = false;

	// The game's Gamebryo local transform as a row-vector matrix: point times matrix = the transformed point.
	D3DXMATRIX ToMatrix(const NiTransform& t) {
		D3DXMATRIX m;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++) m.m[i][j] = t.rot.data[j][i] * t.scale;
		m.m[0][3] = m.m[1][3] = m.m[2][3] = 0.0f;
		m.m[3][0] = t.pos.x; m.m[3][1] = t.pos.y; m.m[3][2] = t.pos.z; m.m[3][3] = 1.0f;
		return m;
	}

	// Largest axis length of a matrix's rotation part (the scale a bound radius grows by).
	float MaxScale(const D3DXMATRIX& m) {
		float best = 0.0f;
		for (int i = 0; i < 3; i++) {
			const float s = m.m[i][0] * m.m[i][0] + m.m[i][1] * m.m[i][1] + m.m[i][2] * m.m[i][2];
			if (s > best) best = s;
		}
		return sqrtf(best);
	}

	// Depth-first walk from a node: every visible NiGeometry is added with its transform to the root. Plain function, no SEH here.
	void Walk(NiAVObject* object, const D3DXMATRIX& parent, int depth) {
		if (!object || depth > 32 || (object->m_flags & NiAVObject::APP_CULLED)) return;
		D3DXMATRIX local = ToMatrix(object->m_localTransform);
		D3DXMATRIX toRoot;
		D3DXMatrixMultiply(&toRoot, &local, &parent);
		if (object->m_pcName && strstr(object->m_pcName, "Collision")) return;
		NiGeometry* geo = object->IsGeometry() ? static_cast<NiGeometry*>(object) : nullptr;
		if (geo) {
			if (gJob.count < kMaxGeo && geo->geomData) {
				const NiBound& bound = geo->geomData->m_kBound;
				D3DXVECTOR3 c(bound.Center.x, bound.Center.y, bound.Center.z), out;
				D3DXVec3TransformCoord(&out, &c, &toRoot);
				GeoItem& item = gJob.items[gJob.count++];
				item.geo = geo;
				item.toRoot = toRoot;
				item.center[0] = out.x; item.center[1] = out.y; item.center[2] = out.z;
				item.radius = bound.Radius * MaxScale(toRoot);
			}
			return;
		}
		NiNode* node = object->IsNiNode();
		if (node) {
			for (UInt16 i = 0; i < node->m_children.end; i++)
				Walk(node->m_children.data[i], toRoot, depth + 1);
		}
	}

	// Loads the model (synchronous, no reference taken) and collects its geometry. 0 = done, -1 = a game-memory exception.
	int LoadAndCollectSEH(const char* path, NiNode** root) {
		__try {
			typedef NiNode* (__thiscall* LoadFileFn)(void*, const char*, int, bool, bool, bool, bool);
			*root = nullptr;
			if (!SpawnGame::ModelLoader()) return 0;
			// LoadFile(path, lodFadeMult 0, prepareObject true (as the game's own camera-shake load), suppressWarning false,
			// faceGen false, dontIncrementRefCount true). The true is deliberate: the draw happens right here, so nothing can free
			// the model meanwhile; a reference could not be released again without the Model pointer.
			NiNode* loaded = ((LoadFileFn)0x00447080)(SpawnGame::ModelLoader(), path, 0, true, false, false, true);
			if (!loaded) return 0;
			*root = loaded;
			D3DXMATRIX identity;
			D3DXMatrixIdentity(&identity);
			Walk(loaded, identity, 0);
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	// The bake's device state. Set before the Clear: a scissor rect or colour write mask the game left on would clip the Clear too.
	// The state block taken in RenderBegin puts all of it back.
	void SetBakeStates(IDirect3DDevice9* d) {
		d->SetVertexShader(gVS);
		d->SetPixelShader(gPS);
		d->SetRenderState(D3DRS_ZENABLE, TRUE);
		d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		d->SetRenderState(D3DRS_FOGENABLE, FALSE);
		d->SetRenderState(D3DRS_LIGHTING, FALSE);
		d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);		// the water reflection's clip plane would cut the model
		d->SetRenderState(D3DRS_DEPTHBIAS, 0);
		d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
		d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);	// model UVs tile past 0-1
		d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
	}

	// Draws the collected geometry into the current target. Returns the number drawn, or -1 on a game-memory exception.
	int DrawSEH(IDirect3DDevice9* d, const D3DXMATRIX& vp, const Lights& lights) {
		__try {
			int drawn = 0;
			d->SetPixelShaderConstantF(0, lights.key, 1);
			d->SetPixelShaderConstantF(1, lights.fill, 1);
			d->SetPixelShaderConstantF(2, lights.ambient, 1);
			d->SetPixelShaderConstantF(4, lights.plain, 1);

			for (int g = 0; g < gJob.count; g++) {
				const GeoItem& item = gJob.items[g];
				NiGeometry* geo = item.geo;
				// Diffuse: the lighting property's first texture, when the game has one loaded.
				IDirect3DBaseTexture9* texture = nullptr;
				BSShaderPPLightingProperty* property = (BSShaderPPLightingProperty*)geo->GetProperty(NiProperty::kType_Shade);
				if (property && property->IsLightingProperty()) {
					NiSourceTexture** textures = property->ppTextures[0];
					NiSourceTexture* base = textures ? textures[0] : nullptr;
					texture = base && base->rendererData ? base->rendererData->dTexture : nullptr;
				}
				float flags[4] = { texture ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
				d->SetPixelShaderConstantF(3, flags, 1);
				d->SetTexture(0, texture);

				// World-view-projection rows (c0-c3) and the rotation rows for normals (c4-c6).
				D3DXMATRIX wvp;
				D3DXMatrixMultiply(&wvp, &item.toRoot, &vp);
				d->SetVertexShaderConstantF(0, (const float*)&wvp, 4);
				d->SetVertexShaderConstantF(4, (const float*)&item.toRoot, 3);

				// Vertex buffers: the geometry's own, else (skinned) each skin partition's, in bind pose.
				NiGeometryBufferData* buffers[64];
				int bufferCount = 0;
				NiGeometryBufferData* own = geo->geomData ? geo->geomData->m_pkBuffData : nullptr;
				if (own) buffers[bufferCount++] = own;
				else if (geo->skinInstance && geo->skinInstance->SkinPartition) {
					NiSkinPartition* partitions = geo->skinInstance->SkinPartition;
					for (UInt32 p = 0; p < partitions->PartitionsCount && bufferCount < 64; p++)
						if (partitions->Partitions[p].BuffData) buffers[bufferCount++] = partitions->Partitions[p].BuffData;
				}
				if (bufferCount == 0) { gJob.skipped++; continue; }

				bool any = false;
				for (int b = 0; b < bufferCount; b++) {
					NiGeometryBufferData* buffer = buffers[b];
					if (!buffer || !buffer->VBChip || !buffer->IB || !buffer->VertCount) continue;
					bool streamsOk = true;
					for (UInt32 s = 0; s < buffer->StreamCount; s++)
						if (!buffer->VBChip[s] || !buffer->VBChip[s]->VB) streamsOk = false;
					if (!streamsOk) continue;
					for (UInt32 s = 0; s < buffer->StreamCount; s++)
						d->SetStreamSource(s, buffer->VBChip[s]->VB, 0, buffer->VertexStride[s]);
					d->SetIndices(buffer->IB);
					if (buffer->FVF) d->SetFVF(buffer->FVF);
					else if (buffer->VertexDeclaration) d->SetVertexDeclaration(buffer->VertexDeclaration);
					else continue;
					// The buffer's own primitive type, as the game's draw (and RtCore) reads it: skin partitions are often strips
					// even under an NiTriShape, and a strip index buffer drawn as a list reads past its end.
					if (buffer->PrimitiveType == D3DPT_TRIANGLESTRIP) {
						UINT start = 0;	// strips sit in the index buffer one after another
						for (UInt32 a = 0; a < buffer->NumArrays; a++) {
							const UINT length = buffer->ArrayLengths ? buffer->ArrayLengths[a] : buffer->TriCount + 2;
							if (length >= 3) {
								d->DrawIndexedPrimitive(D3DPT_TRIANGLESTRIP, buffer->BaseVertexIndex, 0, buffer->VertCount, start, length - 2);
								any = true;
							}
							start += length;
						}
					}
					else if (buffer->PrimitiveType == D3DPT_TRIANGLELIST && buffer->TriCount) {
						d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, buffer->BaseVertexIndex, 0, buffer->VertCount, 0, buffer->TriCount);
						any = true;
					}
				}
				if (any) drawn++;
				else gJob.skipped++;
			}
			return drawn;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return -1;
		}
	}

	IDirect3DSurface9* gRTMore[3] = {};	// render targets 1-3: unbound for the bake (MRT surfaces must match RT0's size)

	void RestoreTargets() {
		IDirect3DDevice9* d = SpawnGame::Device();
		if (gRT0) d->SetRenderTarget(0, gRT0);
		for (int i = 0; i < 3; i++) if (gRTMore[i]) d->SetRenderTarget(i + 1, gRTMore[i]);
		d->SetDepthStencilSurface(gDS);	// also when it was none: our 256 x 256 depth must not stay bound
		d->SetViewport(&gVP);
	}

	void ReleaseSaved() {
		if (gRT0) { gRT0->Release(); gRT0 = nullptr; }
		for (int i = 0; i < 3; i++) if (gRTMore[i]) { gRTMore[i]->Release(); gRTMore[i] = nullptr; }
		if (gDS) { gDS->Release(); gDS = nullptr; }
	}
}

namespace SpawnMenu {

bool RenderBegin() {
	if (gBegun) return true;
	IDirect3DDevice9* d = SpawnGame::Device();
	if (!d) return false;
	if (!gVS || !gPS) {
		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* errors = nullptr;
		bool ok = SUCCEEDED(D3DXCompileShader(kVertexSource, (UINT)strlen(kVertexSource), NULL, NULL, "main", "vs_3_0", 0, &code, &errors, NULL)) && code &&
			SUCCEEDED(d->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &gVS));
		if (code) code->Release();
		if (errors) errors->Release();
		code = errors = nullptr;
		ok = ok && SUCCEEDED(D3DXCompileShader(kPixelSource, (UINT)strlen(kPixelSource), NULL, NULL, "main", "ps_3_0", 0, &code, &errors, NULL)) && code &&
			SUCCEEDED(d->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &gPS));
		if (code) code->Release();
		if (errors) errors->Release();
		if (!ok) {
			Logger::Log("SpawnRender: the picture shader did not compile or create: no pictures this session");
			return false;
		}
	}
	if (!gDepth && FAILED(d->CreateDepthStencilSurface(kThumbSize, kThumbSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &gDepth, NULL))) {
		Logger::Log("SpawnRender: no %d x %d depth surface: no pictures this session", kThumbSize, kThumbSize);
		return false;
	}
	// The state block keeps the device state for RenderEnd; render target, depth and viewport are outside it, so they are read here.
	if (FAILED(d->GetRenderTarget(0, &gRT0)) || !gRT0) { gRT0 = nullptr; return false; }
	if (FAILED(d->GetDepthStencilSurface(&gDS))) gDS = nullptr;
	for (int i = 0; i < 3; i++) if (FAILED(d->GetRenderTarget(i + 1, &gRTMore[i]))) gRTMore[i] = nullptr;
	d->GetViewport(&gVP);
	if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &gSaved)) || !gSaved) {
		gSaved = nullptr;
		ReleaseSaved();
		return false;
	}
	gSaved->Capture();
	gBegun = true;
	return true;
}

void* RenderEntry(const Entry& entry, bool* retry) {
	if (retry) *retry = false;
	if (!gBegun) return nullptr;
	if (entry.category == kCat_NPC || entry.model.empty()) return nullptr;	// NPCs have no bake; an entry with no model has nothing to draw

	char path[MAX_PATH];
	snprintf(path, sizeof(path), "Meshes\\%s", entry.model.c_str());
	for (char* c = path; *c; c++) if (*c == '/') *c = '\\';

	gJob.count = 0;
	gJob.skipped = 0;
	NiNode* root = nullptr;
	const int loadResult = LoadAndCollectSEH(path, &root);
	if (loadResult < 0) {
		if (!gLoggedSEH) { gLoggedSEH = true; Logger::Log("SpawnRender: the game's model code threw on %s (logged once; that entry gets no picture)", path); }
		return nullptr;
	}
	if (!root || gJob.count == 0) return nullptr;

	IDirect3DDevice9* d = SpawnGame::Device();
	IDirect3DTexture9* texture = nullptr;
	if (FAILED(d->CreateTexture(kThumbSize, kThumbSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL)) || !texture) {
		if (!gLoggedFail) { gLoggedFail = true; Logger::Log("SpawnRender: no render target texture: no pictures this session"); }
		return nullptr;
	}
	IDirect3DSurface9* surface = nullptr;
	texture->GetSurfaceLevel(0, &surface);
	if (!surface) { texture->Release(); return nullptr; }
	d->SetRenderTarget(0, surface);
	surface->Release();	// the device holds its own reference while it is the target
	for (int i = 0; i < 3; i++) if (gRTMore[i]) d->SetRenderTarget(i + 1, NULL);
	d->SetDepthStencilSurface(gDepth);
	D3DVIEWPORT9 vp9 = { 0, 0, (DWORD)kThumbSize, (DWORD)kThumbSize, 0.0f, 1.0f };
	d->SetViewport(&vp9);

	// Bound: a box around the geometry's spheres, then its centre and the farthest reach from it.
	float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	for (int g = 0; g < gJob.count; g++) {
		const GeoItem& item = gJob.items[g];
		for (int k = 0; k < 3; k++) {
			if (item.center[k] - item.radius < lo[k]) lo[k] = item.center[k] - item.radius;
			if (item.center[k] + item.radius > hi[k]) hi[k] = item.center[k] + item.radius;
		}
	}
	D3DXVECTOR3 centre((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f);
	float reach = 0.0f;
	for (int g = 0; g < gJob.count; g++) {
		const GeoItem& item = gJob.items[g];
		D3DXVECTOR3 c(item.center[0], item.center[1], item.center[2]);
		D3DXVECTOR3 offset = c - centre;
		const float r = D3DXVec3Length(&offset) + item.radius;
		if (r > reach) reach = r;
	}
	if (reach < 0.5f) reach = 0.5f;

	// 3/4 view, yaw 35 and pitch 20 degrees, Z up; a 45 degree lens fitted to the reach with a little margin. Right-handed, as
	// the game's space is (ShadowManager builds its views the same way): a left-handed view would mirror the picture.
	// Weapons: a straight side profile instead. Weapon models are not Z up: the barrel runs along +X and the top of the gun
	// faces +Y (seen in game 11-05: a view from +X looked down the barrel, one from -Y at the gun's underside). So the eye sits
	// at +Z with +Y as the picture's up, which puts +X (the muzzle) on the right; framed on the side's own length (X) and
	// height (Y) so a long rifle fills the tile rather than its bounding sphere.
	const bool side = entry.category == kCat_Weapon;
	const float yaw = D3DXToRadian(35.0f), pitch = D3DXToRadian(20.0f), fov = D3DXToRadian(45.0f);
	D3DXVECTOR3 towardEye(cosf(pitch) * sinf(yaw), cosf(pitch) * cosf(yaw), sinf(pitch));
	D3DXVECTOR3 up(0.0f, 0.0f, 1.0f);
	float dist = reach * 1.15f / sinf(fov * 0.5f);
	if (side) {
		towardEye = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
		up = D3DXVECTOR3(0.0f, 1.0f, 0.0f);
		const float halfSide = 0.5f * max(hi[0] - lo[0], hi[1] - lo[1]) * 1.08f;
		dist = halfSide / tanf(fov * 0.5f) + 0.5f * (hi[2] - lo[2]);
		if (dist < reach * 1.05f) dist = reach * 1.05f;	// the eye stays outside the model
	}
	D3DXVECTOR3 eye = centre + towardEye * dist;
	D3DXMATRIX view, proj, vp;
	D3DXMatrixLookAtRH(&view, &eye, &centre, &up);
	D3DXMatrixPerspectiveFovRH(&proj, fov, 1.0f, (dist - reach * 1.5f > 0.5f) ? dist - reach * 1.5f : 0.5f, dist + reach * 1.5f);
	D3DXMatrixMultiply(&vp, &view, &proj);

	// Key and fill from the camera's side, so the visible faces are lit whichever way the model turns.
	D3DXVECTOR3 right, camUp;
	D3DXVec3Cross(&right, &up, &towardEye);
	D3DXVec3Normalize(&right, &right);
	D3DXVec3Cross(&camUp, &towardEye, &right);
	D3DXVECTOR3 keyDir = towardEye * 0.7f + camUp * 0.6f - right * 0.4f;
	D3DXVECTOR3 fillDir = towardEye * 0.5f - right * 0.8f - camUp * 0.1f;
	D3DXVec3Normalize(&keyDir, &keyDir);
	D3DXVec3Normalize(&fillDir, &fillDir);
	Lights lights = {
		{ keyDir.x, keyDir.y, keyDir.z, 0.75f },
		{ fillDir.x, fillDir.y, fillDir.z, 0.30f },
		{ 0.35f, 0.0f, 0.0f, 0.0f },
		{ 0.6f, 0.6f, 0.6f, 1.0f },
	};

	SetBakeStates(d);
	d->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
	// Draws need the game's scene open; the render call normally has it, else open one for this bake.
	const bool ownScene = SUCCEEDED(d->BeginScene());
	const int drawn = DrawSEH(d, vp, lights);
	if (ownScene) d->EndScene();
	gEntries++;
	if (gEntries <= 3 || drawn < 0)
		Logger::Log("SpawnRender: %s: %d geometry drawn, %d skipped (no buffers), %d in the model%s", path, drawn < 0 ? 0 : drawn, gJob.skipped, gJob.count,
			drawn < 0 ? " (the draw threw; no picture)" : "");
	if (drawn <= 0) {
		RestoreTargets();
		texture->Release();
		return nullptr;
	}
	return texture;
}

void RenderEnd() {
	if (!gBegun) return;
	IDirect3DDevice9* d = SpawnGame::Device();
	RestoreTargets();	// SetRenderTarget resets the viewport, so the state block and viewport come after
	if (gSaved) { gSaved->Apply(); gSaved->Release(); gSaved = nullptr; }
	d->SetViewport(&gVP);
	ReleaseSaved();
	gBegun = false;
}

void RenderRelease() {
	if (gBegun) RenderEnd();	// never expected (the bake begins and ends inside one ThumbFrame), but the depth must not stay bound
	if (gDepth) { gDepth->Release(); gDepth = nullptr; }
}

}
