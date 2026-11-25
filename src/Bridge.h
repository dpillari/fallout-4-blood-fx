#pragma once
// Plain-C++ interface between the game side (main.cpp, CommonLibF4) and the
// render side (Renderer.cpp, Windows SDK / D3D11). Keeping them in separate
// translation units avoids CommonLib's REX::W32 types clashing with <d3d11.h>.

#include <cstdint>
#include <string>
#include <vector>

namespace BloodFX
{
	struct Settings
	{
		bool        enabled          = true;
		std::uint32_t maxParticles   = 4096;
		std::uint32_t dropsPerHit    = 24;
		std::uint32_t dropsPerKill   = 64;
		float       spraySpeed       = 450.0f;
		float       spread           = 0.45f;
		float       gravity          = 686.0f;
		float       drag             = 0.8f;
		float       stickSpeed       = 120.0f;
		float       bounce           = 0.05f;
		float       collideThickness = 12.0f;
		float       dropSize         = 0.9f;
		float       splatSize        = 3.0f;
		float       splatGrow        = 0.6f;
		float       flyLife          = 2.5f;
		float       splatLife        = 45.0f;
		float       color[4]         = { 0.32f, 0.02f, 0.02f, 0.92f };
		int         depthTarget      = -1;   // -1 = auto (first depth target matching screen size)
		bool        debugLog         = false;
		std::vector<std::string> noBloodKeywords{ "ActorTypeRobot", "ActorTypeTurret" };
	};

	// Filled by the game side every frame.
	struct FrameInput
	{
		float viewProj[16]{};        // engine's viewProjMat, as stored (4 x __m128)
		float viewDir[3]{};          // camera forward
		float posAdjust[3]{};        // engine renders relative to this point
		float camPos[3]{};           // camera world position
		void* depthSRVs[16]{};       // ID3D11ShaderResourceView* per engine depth target
		int   depthCount = 0;
		bool  hide = false;          // menu open -> pause & hide
	};

	using FrameInputFn = bool (*)(FrameInput&);
	using LogFn        = void (*)(int a_level, const char* a_msg);   // 0 info, 1 warn, 2 error

	// Implemented in Renderer.cpp
	Settings& GetSettings();
	void      LoadSettings(void* a_moduleHandle);
	void      SetLogger(LogFn a_fn);
	bool      InstallPresentHook(void* a_swapChain, FrameInputFn a_frameInput);
	void      QueueBurst(const float a_pos[3], const float a_dir[3], std::uint32_t a_count, float a_speedScale);
}
