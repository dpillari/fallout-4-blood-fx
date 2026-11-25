// Render side: plain Windows SDK + D3D11. No CommonLib headers in this file.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "Bridge.h"
#include "Shaders.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace BloodFX
{
	// ------------------------------------------------------------------
	// Settings + logging
	// ------------------------------------------------------------------
	namespace
	{
		Settings g_settings;
		LogFn    g_log = nullptr;

		template <class... Args>
		void Log(int a_level, const char* a_fmt, Args... a_args)
		{
			if (!g_log) return;
			char buf[1024];
			std::snprintf(buf, sizeof(buf), a_fmt, a_args...);
			g_log(a_level, buf);
		}
	}

	Settings& GetSettings() { return g_settings; }
	void      SetLogger(LogFn a_fn) { g_log = a_fn; }

	void LoadSettings(void* a_module)
	{
		HMODULE module = static_cast<HMODULE>(a_module);
		if (!module) {   // default: this DLL
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&LoadSettings), &module);
		}
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(module, path, MAX_PATH);
		std::wstring ini = path;
		ini = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"BloodFX.ini";

		if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
			Log(1, "BloodFX.ini not found next to the DLL, using defaults");
			return;
		}

		auto str = [&](const wchar_t* s, const wchar_t* k, const wchar_t* d) {
			wchar_t buf[512]{};
			GetPrivateProfileStringW(s, k, d, buf, 512, ini.c_str());
			return std::wstring(buf);
		};
		auto f = [&](const wchar_t* s, const wchar_t* k, float d) {
			auto v = str(s, k, L"");
			return v.empty() ? d : std::wcstof(v.c_str(), nullptr);
		};
		auto i = [&](const wchar_t* s, const wchar_t* k, int d) {
			return static_cast<int>(GetPrivateProfileIntW(s, k, d, ini.c_str()));
		};

		auto& c = g_settings;
		c.enabled          = i(L"General", L"bEnabled", 1) != 0;
		c.debugLog         = i(L"General", L"bDebugLog", 0) != 0;
		c.maxParticles     = static_cast<std::uint32_t>(std::clamp(i(L"Performance", L"iMaxParticles", 4096), 256, 65536));
		c.dropsPerHit      = static_cast<std::uint32_t>(std::max(0, i(L"Performance", L"iDropsPerHit", 24)));
		c.dropsPerKill     = static_cast<std::uint32_t>(std::max(0, i(L"Performance", L"iDropsPerKill", 64)));
		c.spraySpeed       = f(L"Physics", L"fSpraySpeed", c.spraySpeed);
		c.spread           = f(L"Physics", L"fSpread", c.spread);
		c.gravity          = f(L"Physics", L"fGravity", c.gravity);
		c.drag             = f(L"Physics", L"fDrag", c.drag);
		c.stickSpeed       = f(L"Physics", L"fStickSpeed", c.stickSpeed);
		c.bounce           = f(L"Physics", L"fBounce", c.bounce);
		c.collideThickness = f(L"Physics", L"fCollideThickness", c.collideThickness);
		c.dropSize         = f(L"Look", L"fDropSize", c.dropSize);
		c.splatSize        = f(L"Look", L"fSplatSize", c.splatSize);
		c.splatGrow        = f(L"Look", L"fSplatGrow", c.splatGrow);
		c.flyLife          = f(L"Look", L"fFlyLife", c.flyLife);
		c.splatLife        = f(L"Look", L"fSplatLife", c.splatLife);
		c.color[0]         = f(L"Look", L"fColorR", c.color[0]);
		c.color[1]         = f(L"Look", L"fColorG", c.color[1]);
		c.color[2]         = f(L"Look", L"fColorB", c.color[2]);
		c.color[3]         = f(L"Look", L"fOpacity", c.color[3]);
		c.depthTarget      = i(L"Advanced", L"iDepthTarget", -1);

		auto kw = str(L"Filter", L"sNoBloodKeywords", L"ActorTypeRobot,ActorTypeTurret");
		c.noBloodKeywords.clear();
		std::wstringstream ss(kw);
		for (std::wstring item; std::getline(ss, item, L',');) {
			item.erase(std::remove_if(item.begin(), item.end(), ::iswspace), item.end());
			if (!item.empty()) {
				std::string ascii;   // keyword editor IDs are ASCII
				for (wchar_t ch : item) ascii.push_back(static_cast<char>(ch));
				c.noBloodKeywords.push_back(std::move(ascii));
			}
		}
		Log(0, "Settings loaded: %u particles, %u drops/hit", c.maxParticles, c.dropsPerHit);
	}

	// ------------------------------------------------------------------
	// GPU simulation
	// ------------------------------------------------------------------
	namespace
	{
		struct Burst
		{
			float         pos[3];
			std::uint32_t start;
			float         dir[3];
			std::uint32_t count;
			float         speed;
			float         spread;
			std::uint32_t seed;
			float         scale;
		};
		static_assert(sizeof(Burst) == 48);

		struct alignas(16) FrameCB
		{
			float         viewProj[4][4];
			float         invViewProj[4][4];
			float         camPos[3];    float dt;
			float         posAdjust[3]; std::uint32_t reversedZ;
			float         screenSize[2]; float gravity; float drag;
			std::uint32_t maxParticles; std::uint32_t burstCount; float stickSpeed; float bounce;
			float         bloodColor[4];
			float         dropSize; float splatSize; float flyLife; float splatLife;
			float         splatGrow; float collideThickness; float time; float pad0;
		};
		static_assert(sizeof(FrameCB) == 240);

		ComPtr<ID3DBlob> Compile(const char* a_body, const char* a_target)
		{
			std::string      src = std::string(Shaders::kCommon) + a_body;
			ComPtr<ID3DBlob> code, err;
			HRESULT hr = D3DCompile(src.data(), src.size(), "BloodFX", nullptr, nullptr,
				"main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
			if (FAILED(hr)) {
				Log(2, "Shader compile failed (%s): %s", a_target,
					err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
				return nullptr;
			}
			return code;
		}

		// Saves/restores all D3D state we touch so the game never notices us.
		struct StateBackup
		{
			ComPtr<ID3D11RenderTargetView>    rtv;
			ComPtr<ID3D11DepthStencilView>    dsv;
			ComPtr<ID3D11BlendState>          blend;
			float                             blendFactor[4]{};
			UINT                              sampleMask = 0;
			ComPtr<ID3D11RasterizerState>     raster;
			ComPtr<ID3D11DepthStencilState>   depth;
			UINT                              stencilRef = 0;
			D3D11_VIEWPORT                    vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT                              vpCount = 0;
			ComPtr<ID3D11VertexShader>        vs;
			ComPtr<ID3D11PixelShader>         ps;
			ComPtr<ID3D11ComputeShader>       cs;
			ComPtr<ID3D11InputLayout>         layout;
			D3D11_PRIMITIVE_TOPOLOGY          topo{};
			ComPtr<ID3D11Buffer>              vsCB, psCB, csCB;
			static constexpr UINT             kSlots = 6;
			ID3D11ShaderResourceView*         vsSRV[kSlots]{}, *psSRV[kSlots]{}, *csSRV[kSlots]{};
			ComPtr<ID3D11SamplerState>        psSamp, csSamp;
			ComPtr<ID3D11UnorderedAccessView> csUAV;

			void Save(ID3D11DeviceContext* c)
			{
				c->OMGetRenderTargets(1, &rtv, &dsv);
				c->OMGetBlendState(&blend, blendFactor, &sampleMask);
				c->OMGetDepthStencilState(&depth, &stencilRef);
				c->RSGetState(&raster);
				vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
				c->RSGetViewports(&vpCount, vp);
				c->VSGetShader(&vs, nullptr, nullptr);
				c->PSGetShader(&ps, nullptr, nullptr);
				c->CSGetShader(&cs, nullptr, nullptr);
				c->IAGetInputLayout(&layout);
				c->IAGetPrimitiveTopology(&topo);
				c->VSGetConstantBuffers(0, 1, &vsCB);
				c->PSGetConstantBuffers(0, 1, &psCB);
				c->CSGetConstantBuffers(0, 1, &csCB);
				c->VSGetShaderResources(0, kSlots, vsSRV);
				c->PSGetShaderResources(0, kSlots, psSRV);
				c->CSGetShaderResources(0, kSlots, csSRV);
				c->PSGetSamplers(0, 1, &psSamp);
				c->CSGetSamplers(0, 1, &csSamp);
				c->CSGetUnorderedAccessViews(0, 1, &csUAV);
			}

			void Restore(ID3D11DeviceContext* c)
			{
				ID3D11RenderTargetView* r = rtv.Get();
				c->OMSetRenderTargets(1, &r, dsv.Get());
				c->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
				c->OMSetDepthStencilState(depth.Get(), stencilRef);
				c->RSSetState(raster.Get());
				c->RSSetViewports(vpCount, vp);
				c->VSSetShader(vs.Get(), nullptr, 0);
				c->PSSetShader(ps.Get(), nullptr, 0);
				c->CSSetShader(cs.Get(), nullptr, 0);
				c->IASetInputLayout(layout.Get());
				c->IASetPrimitiveTopology(topo);
				ID3D11Buffer* b;
				b = vsCB.Get(); c->VSSetConstantBuffers(0, 1, &b);
				b = psCB.Get(); c->PSSetConstantBuffers(0, 1, &b);
				b = csCB.Get(); c->CSSetConstantBuffers(0, 1, &b);
				c->VSSetShaderResources(0, kSlots, vsSRV);
				c->PSSetShaderResources(0, kSlots, psSRV);
				c->CSSetShaderResources(0, kSlots, csSRV);
				for (auto* s : vsSRV) if (s) s->Release();
				for (auto* s : psSRV) if (s) s->Release();
				for (auto* s : csSRV) if (s) s->Release();
				ID3D11SamplerState* sm;
				sm = psSamp.Get(); c->PSSetSamplers(0, 1, &sm);
				sm = csSamp.Get(); c->CSSetSamplers(0, 1, &sm);
				ID3D11UnorderedAccessView* u = csUAV.Get();
				c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
			}
		};

		// clip = M * float4(p, 1), M given row by row
		XMFLOAT4 ProjectRows(const XMFLOAT4X4& m, float x, float y, float z)
		{
			XMFLOAT4 r;
			r.x = m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3];
			r.y = m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3];
			r.z = m.m[2][0] * x + m.m[2][1] * y + m.m[2][2] * z + m.m[2][3];
			r.w = m.m[3][0] * x + m.m[3][1] * y + m.m[3][2] * z + m.m[3][3];
			return r;
		}

		// Distance from the camera to the surface stored in a depth sample.
		float SurfaceDistance(const FrameCB& a_cb, float a_u, float a_v, float a_depth)
		{
			XMFLOAT4X4 inv;
			std::memcpy(&inv, a_cb.invViewProj, sizeof(inv));
			auto w = ProjectRows(inv, a_u * 2.0f - 1.0f, 1.0f - a_v * 2.0f, a_depth);
			if (std::fabs(w.w) < 1e-12f) return -1.0f;
			const float x = w.x / w.w + a_cb.posAdjust[0] - a_cb.camPos[0];
			const float y = w.y / w.w + a_cb.posAdjust[1] - a_cb.camPos[1];
			const float z = w.z / w.w + a_cb.posAdjust[2] - a_cb.camPos[2];
			return std::sqrt(x * x + y * y + z * z);
		}

		class Sim
		{
		public:
			void QueueBurst(const float a_pos[3], const float a_dir[3], std::uint32_t a_count, float a_speedScale)
			{
				if (!g_settings.enabled || a_count == 0) return;
				std::scoped_lock l(_lock);
				if (_pending.size() >= kMaxBursts) return;   // perf guard in huge firefights

				Burst b{};
				std::memcpy(b.pos, a_pos, sizeof(b.pos));
				std::memcpy(b.dir, a_dir, sizeof(b.dir));
				b.count  = std::min(a_count, g_settings.maxParticles);
				b.speed  = g_settings.spraySpeed * a_speedScale;
				b.spread = g_settings.spread;
				b.seed   = _seed = _seed * 1664525u + 1013904223u;
				b.scale  = 1.0f;
				b.start  = _ringHead;
				_ringHead = (_ringHead + b.count) % g_settings.maxParticles;
				_pending.push_back(b);
			}

			void Frame(IDXGISwapChain* a_swap, const FrameInput& a_in)
			{
				if (_failed || !g_settings.enabled) return;

				ComPtr<ID3D11Device> d;
				if (FAILED(a_swap->GetDevice(IID_PPV_ARGS(&d)))) return;
				ComPtr<ID3D11DeviceContext> c;
				d->GetImmediateContext(&c);

				if (!_ready) {
					_ready = Init(d.Get());
					if (!_ready) {
						_failed = true;
						Log(2, "BloodFX disabled: GPU init failed");
						return;
					}
				}

				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				float dt = std::min(static_cast<float>(now.QuadPart - _last.QuadPart) / static_cast<float>(_freq.QuadPart), 1.0f / 20.0f);
				_last = now;
				if (a_in.hide) return;

				ComPtr<ID3D11Texture2D> bb;
				if (FAILED(a_swap->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
				D3D11_TEXTURE2D_DESC bbd;
				bb->GetDesc(&bbd);

				auto depthSRV = PickDepth(a_in, bbd.Width, bbd.Height);
				if (!depthSRV) return;

				FrameCB cb{};
				if (!SetupCamera(a_in, cb)) return;

				if (!_rtv || _rtvTex != bb.Get()) {
					_rtv.Reset();
					if (FAILED(d->CreateRenderTargetView(bb.Get(), nullptr, &_rtv))) return;
					_rtvTex = bb.Get();
				}

				_time += dt;
				std::vector<Burst> bursts;
				{
					std::scoped_lock l(_lock);
					bursts.swap(_pending);
				}

				std::memcpy(cb.camPos, a_in.camPos, sizeof(cb.camPos));
				cb.dt               = dt;
				cb.screenSize[0]    = static_cast<float>(bbd.Width);
				cb.screenSize[1]    = static_cast<float>(bbd.Height);
				cb.gravity          = g_settings.gravity;
				cb.drag             = g_settings.drag;
				cb.maxParticles     = g_settings.maxParticles;
				cb.burstCount       = static_cast<std::uint32_t>(bursts.size());
				cb.stickSpeed       = g_settings.stickSpeed;
				cb.bounce           = g_settings.bounce;
				std::memcpy(cb.bloodColor, g_settings.color, sizeof(cb.bloodColor));
				cb.dropSize         = g_settings.dropSize;
				cb.splatSize        = g_settings.splatSize;
				cb.flyLife          = g_settings.flyLife;
				cb.splatLife        = g_settings.splatLife;
				cb.splatGrow        = g_settings.splatGrow;
				cb.collideThickness = g_settings.collideThickness;
				cb.time             = _time;

				StateBackup saved;
				saved.Save(c.Get());

				D3D11_MAPPED_SUBRESOURCE m;
				if (SUCCEEDED(c->Map(_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					std::memcpy(m.pData, &cb, sizeof(cb));
					c->Unmap(_cb.Get(), 0);
				}

				// Unbind outputs so the depth texture can be read as SRV.
				c->OMSetRenderTargets(0, nullptr, nullptr);

				if (_clearRequested.exchange(false)) {
					const UINT zero[4]{};
					c->ClearUnorderedAccessViewUint(_particlesUAV.Get(), zero);   // state 0 = dead
				}

				ID3D11Buffer*              cbs = _cb.Get();
				ID3D11SamplerState*        smp = _pointClamp.Get();
				ID3D11UnorderedAccessView* uav = _particlesUAV.Get();
				ID3D11UnorderedAccessView* nullUAV = nullptr;

				// --- Emit ---
				if (!bursts.empty()) {
					if (SUCCEEDED(c->Map(_bursts.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
						std::memcpy(m.pData, bursts.data(), bursts.size() * sizeof(Burst));
						c->Unmap(_bursts.Get(), 0);
					}
					std::uint32_t maxCount = 0;
					for (auto& b : bursts) maxCount = std::max(maxCount, b.count);

					ID3D11ShaderResourceView* srvs[2] = { nullptr, _burstsSRV.Get() };
					c->CSSetShader(_emitCS.Get(), nullptr, 0);
					c->CSSetConstantBuffers(0, 1, &cbs);
					c->CSSetShaderResources(0, 2, srvs);
					c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
					c->Dispatch(static_cast<UINT>(bursts.size()), (maxCount + 63) / 64, 1);
				}

				// --- Update ---
				{
					ID3D11ShaderResourceView* srvs[2] = { depthSRV, nullptr };
					c->CSSetShader(_updateCS.Get(), nullptr, 0);
					c->CSSetConstantBuffers(0, 1, &cbs);
					c->CSSetShaderResources(0, 2, srvs);
					c->CSSetSamplers(0, 1, &smp);
					c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
					c->Dispatch((g_settings.maxParticles + 255) / 256, 1, 1);
					c->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
					ID3D11ShaderResourceView* none[2]{};
					c->CSSetShaderResources(0, 2, none);
				}

				if (g_settings.debugLog) {
					Probe(c.Get(), a_in, depthSRV, cb, now);
				}

				// --- Render ---
				{
					D3D11_VIEWPORT vpt{ 0, 0, cb.screenSize[0], cb.screenSize[1], 0, 1 };
					ID3D11RenderTargetView*   r = _rtv.Get();
					ID3D11ShaderResourceView* srvs[2] = { depthSRV, _particlesSRV.Get() };
					float                     bf[4]{};
					c->OMSetRenderTargets(1, &r, nullptr);
					c->OMSetBlendState(_blend.Get(), bf, 0xFFFFFFFF);
					c->OMSetDepthStencilState(_noDepth.Get(), 0);
					c->RSSetState(_raster.Get());
					c->RSSetViewports(1, &vpt);
					c->IASetInputLayout(nullptr);
					c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					c->VSSetShader(_vs.Get(), nullptr, 0);
					c->PSSetShader(_ps.Get(), nullptr, 0);
					c->VSSetConstantBuffers(0, 1, &cbs);
					c->PSSetConstantBuffers(0, 1, &cbs);
					c->VSSetShaderResources(0, 2, srvs);
					c->PSSetShaderResources(0, 2, srvs);
					c->PSSetSamplers(0, 1, &smp);
					c->Draw(g_settings.maxParticles * 6, 0);
				}

				saved.Restore(c.Get());
			}

			void RequestClear() { _clearRequested = true; }

			void OnResize()
			{
				_rtv.Reset();
				_rtvTex = nullptr;
			}

		private:
			static constexpr std::uint32_t kMaxBursts = 64;
			static constexpr UINT          kProbeCount = 22;

			void Probe(ID3D11DeviceContext* c, const FrameInput& a_in, ID3D11ShaderResourceView* a_depth, const FrameCB& a_cb, LARGE_INTEGER a_now)
			{
				if (!_probeCS) return;

				if (_probePending) {
					D3D11_MAPPED_SUBRESOURCE m;
					if (c->Map(_probeStaging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
						std::uint32_t raw[kProbeCount];
						std::memcpy(raw, m.pData, sizeof(raw));
						c->Unmap(_probeStaging.Get(), 0);
						_probePending = false;

						static const float pts[5][2] = { { 0.5f, 0.5f }, { 0.5f, 0.85f }, { 0.25f, 0.5f }, { 0.75f, 0.5f }, { 0.5f, 0.15f } };
						std::string line = "Probe:";
						for (int t = 0; t < 4; ++t) {
							char buf[256];
							std::snprintf(buf, sizeof(buf), " | target %d:", _probeTargets[t]);
							line += buf;
							for (int i = 0; i < 5; ++i) {
								float d;
								std::memcpy(&d, &raw[t * 5 + i], sizeof(d));
								std::snprintf(buf, sizeof(buf), " %.5f(%.0fu)", d, SurfaceDistance(_probeCB, pts[i][0], pts[i][1], d));
								line += buf;
							}
						}
						char buf[128];
						std::snprintf(buf, sizeof(buf), " | flying %u stuck %u", raw[20], raw[21]);
						line += buf;
						Log(0, "%s", line.c_str());
						Log(0, "  (points: centre, lower-middle, left, right, upper-middle; u = game units from camera)");
					}
					return;
				}

				if (static_cast<float>(a_now.QuadPart - _lastProbe.QuadPart) / static_cast<float>(_freq.QuadPart) < 1.0f) return;
				_lastProbe = a_now;

				// slot 0 = the target we collide with, then up to 3 other screen-size targets
				ID3D11ShaderResourceView* extra[3] = { a_depth, a_depth, a_depth };
				_probeTargets[0] = _depthIndex;
				_probeTargets[1] = _probeTargets[2] = _probeTargets[3] = _depthIndex;
				int n = 0;
				for (int idx : _screenTargets) {
					if (idx == _depthIndex || n >= 3) continue;
					if (auto srv = static_cast<ID3D11ShaderResourceView*>(a_in.depthSRVs[idx])) {
						extra[n] = srv;
						_probeTargets[1 + n] = idx;
						++n;
					}
				}

				const UINT zero[4]{};
				c->ClearUnorderedAccessViewUint(_probeUAV.Get(), zero);
				ID3D11ShaderResourceView*  first = a_depth;
				ID3D11ShaderResourceView*  rest[4] = { extra[0], extra[1], extra[2], _particlesSRV.Get() };
				ID3D11UnorderedAccessView* uav = _probeUAV.Get();
				ID3D11UnorderedAccessView* nullUAV = nullptr;
				ID3D11Buffer*              cbs = _cb.Get();
				ID3D11SamplerState*        smp = _pointClamp.Get();
				c->CSSetShader(_probeCS.Get(), nullptr, 0);
				c->CSSetConstantBuffers(0, 1, &cbs);
				c->CSSetSamplers(0, 1, &smp);
				c->CSSetShaderResources(0, 1, &first);
				c->CSSetShaderResources(2, 4, rest);
				c->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				c->Dispatch((g_settings.maxParticles + 255) / 256, 1, 1);
				c->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
				ID3D11ShaderResourceView* none[6]{};
				c->CSSetShaderResources(0, 6, none);
				c->CopyResource(_probeStaging.Get(), _probeBuf.Get());
				_probeCB = a_cb;
				_probePending = true;
			}

			void InitProbe(ID3D11Device* d)
			{
				auto code = Compile(Shaders::kProbeCS, "cs_5_0");
				if (!code || FAILED(d->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &_probeCS))) {
					_probeCS.Reset();
					return;
				}
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth           = kProbeCount * 4;
				bd.Usage               = D3D11_USAGE_DEFAULT;
				bd.BindFlags           = D3D11_BIND_UNORDERED_ACCESS;
				bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				bd.StructureByteStride = 4;
				if (FAILED(d->CreateBuffer(&bd, nullptr, &_probeBuf)) ||
					FAILED(d->CreateUnorderedAccessView(_probeBuf.Get(), nullptr, &_probeUAV))) {
					_probeCS.Reset();
					return;
				}
				D3D11_BUFFER_DESC sd{};
				sd.ByteWidth      = kProbeCount * 4;
				sd.Usage          = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(d->CreateBuffer(&sd, nullptr, &_probeStaging))) {
					_probeCS.Reset();
					return;
				}
				Log(0, "Debug probe enabled (logs once per second)");
			}

			bool Init(ID3D11Device* d)
			{
				auto emit = Compile(Shaders::kEmitCS, "cs_5_0");
				auto upd  = Compile(Shaders::kUpdateCS, "cs_5_0");
				auto vs   = Compile(Shaders::kRenderVS, "vs_5_0");
				auto ps   = Compile(Shaders::kRenderPS, "ps_5_0");
				if (!emit || !upd || !vs || !ps) return false;

				if (FAILED(d->CreateComputeShader(emit->GetBufferPointer(), emit->GetBufferSize(), nullptr, &_emitCS)) ||
					FAILED(d->CreateComputeShader(upd->GetBufferPointer(), upd->GetBufferSize(), nullptr, &_updateCS)) ||
					FAILED(d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &_vs)) ||
					FAILED(d->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &_ps))) {
					return false;
				}

				const UINT n = g_settings.maxParticles;
				{
					D3D11_BUFFER_DESC bd{};
					bd.ByteWidth           = n * 48;
					bd.Usage               = D3D11_USAGE_DEFAULT;
					bd.BindFlags           = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
					bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					bd.StructureByteStride = 48;
					std::vector<std::uint8_t> zero(bd.ByteWidth, 0);   // state 0 = dead
					D3D11_SUBRESOURCE_DATA    init{ zero.data(), 0, 0 };
					if (FAILED(d->CreateBuffer(&bd, &init, &_particles))) return false;
					if (FAILED(d->CreateUnorderedAccessView(_particles.Get(), nullptr, &_particlesUAV))) return false;
					if (FAILED(d->CreateShaderResourceView(_particles.Get(), nullptr, &_particlesSRV))) return false;
				}
				{
					D3D11_BUFFER_DESC bd{};
					bd.ByteWidth           = kMaxBursts * sizeof(Burst);
					bd.Usage               = D3D11_USAGE_DYNAMIC;
					bd.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
					bd.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
					bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					bd.StructureByteStride = sizeof(Burst);
					if (FAILED(d->CreateBuffer(&bd, nullptr, &_bursts))) return false;
					if (FAILED(d->CreateShaderResourceView(_bursts.Get(), nullptr, &_burstsSRV))) return false;
				}
				{
					D3D11_BUFFER_DESC bd{};
					bd.ByteWidth      = sizeof(FrameCB);
					bd.Usage          = D3D11_USAGE_DYNAMIC;
					bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
					bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
					if (FAILED(d->CreateBuffer(&bd, nullptr, &_cb))) return false;
				}

				D3D11_SAMPLER_DESC sd{};
				sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
				sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				sd.MaxLOD   = D3D11_FLOAT32_MAX;
				d->CreateSamplerState(&sd, &_pointClamp);

				D3D11_BLEND_DESC bl{};
				auto&            rt = bl.RenderTarget[0];
				rt.BlendEnable           = TRUE;
				rt.SrcBlend              = D3D11_BLEND_ONE;   // premultiplied alpha
				rt.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
				rt.BlendOp               = D3D11_BLEND_OP_ADD;
				rt.SrcBlendAlpha         = D3D11_BLEND_ZERO;
				rt.DestBlendAlpha        = D3D11_BLEND_ONE;
				rt.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
				rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
				d->CreateBlendState(&bl, &_blend);

				D3D11_RASTERIZER_DESC rs{};
				rs.FillMode        = D3D11_FILL_SOLID;
				rs.CullMode        = D3D11_CULL_NONE;
				rs.DepthClipEnable = TRUE;
				d->CreateRasterizerState(&rs, &_raster);

				D3D11_DEPTH_STENCIL_DESC ds{};   // depth test is done by hand in the pixel shader
				d->CreateDepthStencilState(&ds, &_noDepth);

				if (g_settings.debugLog) InitProbe(d);

				QueryPerformanceFrequency(&_freq);
				QueryPerformanceCounter(&_last);
				_lastProbe = _last;
				Log(0, "BloodFX GPU sim ready (%u particles)", n);
				return true;
			}

			ID3D11ShaderResourceView* PickDepth(const FrameInput& a_in, UINT a_w, UINT a_h)
			{
				if (_depthIndex >= 0 && _depthIndex < a_in.depthCount && a_in.depthSRVs[_depthIndex] && _depthW == a_w && _depthH == a_h) {
					return static_cast<ID3D11ShaderResourceView*>(a_in.depthSRVs[_depthIndex]);
				}

				_depthIndex = -1;
				_screenTargets.clear();
				for (int i = 0; i < a_in.depthCount; ++i) {
					auto srv = static_cast<ID3D11ShaderResourceView*>(a_in.depthSRVs[i]);
					if (!srv) continue;
					ComPtr<ID3D11Resource> res;
					srv->GetResource(&res);
					ComPtr<ID3D11Texture2D> tex;
					if (!res || FAILED(res.As(&tex))) continue;
					D3D11_TEXTURE2D_DESC td;
					tex->GetDesc(&td);
					const bool match = td.Width == a_w && td.Height == a_h;
					if (match) _screenTargets.push_back(i);
					if (!_depthLogged) {
						Log(0, "Depth target %d: %ux%u fmt %d%s", i, td.Width, td.Height, static_cast<int>(td.Format), match ? " (screen size)" : "");
					}
					if (g_settings.depthTarget == i || (g_settings.depthTarget < 0 && match && _depthIndex < 0)) {
						_depthIndex = i;
					}
				}
				_depthLogged = true;
				_depthW = a_w;
				_depthH = a_h;

				if (_depthIndex < 0) {
					if (!_depthWarned) Log(2, "No usable depth target found - set iDepthTarget in BloodFX.ini");
					_depthWarned = true;
					return nullptr;
				}
				Log(0, "Using depth target %d", _depthIndex);
				return static_cast<ID3D11ShaderResourceView*>(a_in.depthSRVs[_depthIndex]);
			}

			// The engine's viewProj is stored as 4 SSE rows, but whether those rows are the
			// matrix or its transpose depends on the build. Work it out from the camera's
			// forward vector: a point straight ahead must land in the centre of the screen.
			bool SetupCamera(const FrameInput& a_in, FrameCB& a_cb)
			{
				XMFLOAT4X4 stored;
				std::memcpy(&stored, a_in.viewProj, sizeof(stored));
				XMFLOAT4X4 transposed;
				XMStoreFloat4x4(&transposed, XMMatrixTranspose(XMLoadFloat4x4(&stored)));

				const float* f = a_in.viewDir;
				// A point straight ahead must land in the centre of the screen, both near and far.
				auto centred = [&](const XMFLOAT4X4& m) {
					for (float dist : { 500.0f, 5000.0f }) {
						auto c = ProjectRows(m, f[0] * dist, f[1] * dist, f[2] * dist);
						if (!(c.w > 1.0f && std::fabs(c.x / c.w) < 0.05f && std::fabs(c.y / c.w) < 0.05f)) return false;
					}
					return true;
				};

				const bool okStored     = centred(stored);
				const bool okTransposed = centred(transposed);
				int        candidate    = -1;
				if (okStored && !okTransposed) candidate = 0;
				else if (okTransposed && !okStored) candidate = 1;

				// Hysteresis: once settled, only switch after 60 consecutive frames agree.
				if (_camOrientation < 0) {
					if (candidate < 0) {
						if (!_camWarned) Log(2, "Could not interpret camera matrix yet (will keep trying)");
						_camWarned = true;
						return false;
					}
					_camOrientation = candidate;
				} else if (candidate >= 0 && candidate != _camOrientation) {
					if (candidate == _camPendingOrientation) {
						if (++_camPendingFrames >= 60) {
							_camOrientation = candidate;
							_camPendingFrames = 0;
						}
					} else {
						_camPendingOrientation = candidate;
						_camPendingFrames = 1;
					}
				} else {
					_camPendingFrames = 0;
				}

				const XMFLOAT4X4* m = _camOrientation == 1 ? &transposed : &stored;
				const bool isTransposed = _camOrientation == 1;
				auto       nearC = ProjectRows(*m, f[0] * 50.0f, f[1] * 50.0f, f[2] * 50.0f);
				auto       farC  = ProjectRows(*m, f[0] * 20000.0f, f[1] * 20000.0f, f[2] * 20000.0f);
				const bool reversed = (nearC.z / nearC.w) > (farC.z / farC.w);

				if (!_camLogged || isTransposed != _lastTransposed || reversed != _lastReversed) {
					Log(0, "Camera matrix: %s, depth: %s (z at 50u = %.5f, at 20000u = %.5f)", isTransposed ? "transposed" : "as stored",
						reversed ? "reversed-Z" : "standard Z", nearC.z / nearC.w, farC.z / farC.w);
					_camLogged = true;
					_lastTransposed = isTransposed;
					_lastReversed = reversed;
				}

				std::memcpy(a_cb.viewProj, m, sizeof(a_cb.viewProj));
				XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(m));
				XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(a_cb.invViewProj), inv);
				std::memcpy(a_cb.posAdjust, a_in.posAdjust, sizeof(a_cb.posAdjust));
				a_cb.reversedZ = reversed ? 1u : 0u;
				return true;
			}

			bool _ready = false;
			bool _failed = false;

			ComPtr<ID3D11ComputeShader>       _emitCS, _updateCS;
			ComPtr<ID3D11VertexShader>        _vs;
			ComPtr<ID3D11PixelShader>         _ps;
			ComPtr<ID3D11Buffer>              _particles, _bursts, _cb;
			ComPtr<ID3D11UnorderedAccessView> _particlesUAV;
			ComPtr<ID3D11ShaderResourceView>  _particlesSRV, _burstsSRV;
			ComPtr<ID3D11SamplerState>        _pointClamp;
			ComPtr<ID3D11BlendState>          _blend;
			ComPtr<ID3D11RasterizerState>     _raster;
			ComPtr<ID3D11DepthStencilState>   _noDepth;
			ComPtr<ID3D11RenderTargetView>    _rtv;
			ID3D11Texture2D*                  _rtvTex = nullptr;

			std::mutex         _lock;
			std::vector<Burst> _pending;
			std::uint32_t      _ringHead = 0;
			std::uint32_t      _seed = 1;
			LARGE_INTEGER      _freq{}, _last{};
			float              _time = 0.0f;

			ComPtr<ID3D11ComputeShader> _probeCS;
			ComPtr<ID3D11Buffer>        _probeBuf, _probeStaging;
			ComPtr<ID3D11UnorderedAccessView> _probeUAV;
			std::atomic<bool>           _clearRequested{ false };
			bool                        _probePending = false;
			LARGE_INTEGER               _lastProbe{};
			FrameCB                     _probeCB{};
			int                         _probeTargets[4]{};
			std::vector<int>            _screenTargets;
			int                         _camOrientation = -1;   // 0 as stored, 1 transposed
			int                         _camPendingOrientation = -1;
			int                         _camPendingFrames = 0;

			int  _depthIndex = -1;
			UINT _depthW = 0, _depthH = 0;
			bool _depthLogged = false, _depthWarned = false;
			bool _camLogged = false, _camWarned = false, _lastTransposed = false, _lastReversed = false;
		};

		Sim          g_sim;
		FrameInputFn g_frameInput = nullptr;

		// ------------------------------------------------------------------
		// Present / ResizeBuffers hooks (IDXGISwapChain vtable slots 8 and 13)
		// ------------------------------------------------------------------
		using Present_t       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		Present_t       g_origPresent = nullptr;
		ResizeBuffers_t g_origResize = nullptr;

		HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_this, UINT a_sync, UINT a_flags)
		{
			if (g_frameInput && !(a_flags & DXGI_PRESENT_TEST)) {
				FrameInput in;
				if (g_frameInput(in)) {
					g_sim.Frame(a_this, in);
				}
			}
			return g_origPresent(a_this, a_sync, a_flags);
		}

		HRESULT STDMETHODCALLTYPE HookedResize(IDXGISwapChain* a_this, UINT a_count, UINT a_w, UINT a_h, DXGI_FORMAT a_fmt, UINT a_flags)
		{
			g_sim.OnResize();   // drop our back-buffer view or the resize fails
			return g_origResize(a_this, a_count, a_w, a_h, a_fmt, a_flags);
		}

		template <class T>
		T PatchVtable(void* a_obj, std::size_t a_index, T a_hook)
		{
			auto  vtbl = *reinterpret_cast<std::uintptr_t**>(a_obj);
			DWORD old;
			VirtualProtect(&vtbl[a_index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
			auto orig = reinterpret_cast<T>(vtbl[a_index]);
			vtbl[a_index] = reinterpret_cast<std::uintptr_t>(a_hook);
			VirtualProtect(&vtbl[a_index], sizeof(void*), old, &old);
			return orig;
		}
	}

	bool InstallPresentHook(void* a_swapChain, FrameInputFn a_frameInput)
	{
		if (!a_swapChain || g_origPresent) return false;
		g_frameInput   = a_frameInput;
		g_origPresent  = PatchVtable(a_swapChain, 8, &HookedPresent);
		g_origResize   = PatchVtable(a_swapChain, 13, &HookedResize);
		Log(0, "Present hooked");
		return true;
	}

	void QueueBurst(const float a_pos[3], const float a_dir[3], std::uint32_t a_count, float a_speedScale)
	{
		g_sim.QueueBurst(a_pos, a_dir, a_count, a_speedScale);
	}

	void ClearAll()
	{
		g_sim.RequestClear();
	}
}
