// BloodFX - GPU blood spray & pooling for Fallout 4 (F4SE / CommonLibF4)
// Game side: events, camera, depth targets. Rendering lives in Renderer.cpp.

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include "Bridge.h"

namespace
{
	void LogBridge(int a_level, const char* a_msg)
	{
		switch (a_level) {
		case 0:  REX::INFO("{}", a_msg); break;
		case 1:  REX::WARN("{}", a_msg); break;
		default: REX::ERROR("{}", a_msg); break;
		}
	}

	bool MenuBlocks()
	{
		auto ui = RE::UI::GetSingleton();
		if (!ui) return true;
		static const RE::BSFixedString menus[] = {
			"PipboyMenu", "PauseMenu", "LoadingMenu", "LooksMenu", "Console",
			"MainMenu", "WorkshopMenu", "BarterMenu", "ContainerMenu",
			"ExamineMenu", "TerminalMenu", "FaderMenu", "CookingMenu", "PowerArmorModMenu"
		};
		for (auto& m : menus) {
			if (ui->GetMenuOpen(m)) return true;
		}
		return false;
	}

	// Called from the Present hook every frame.
	bool GetFrameInput(BloodFX::FrameInput& a_out)
	{
		auto state = RE::BSGraphics::State::GetSingleton();
		auto rd    = RE::BSGraphics::GetRendererData();
		auto cam   = RE::Main::WorldRootCamera();
		if (!state || !rd || !cam) return false;

		const auto& view = state->cameraState.camViewData;
		std::memcpy(a_out.viewProj, view.viewProjMat, sizeof(a_out.viewProj));
		float dir[4];
		std::memcpy(dir, &view.viewDir, sizeof(dir));
		a_out.viewDir[0] = dir[0];
		a_out.viewDir[1] = dir[1];
		a_out.viewDir[2] = dir[2];

		const auto& adj = state->cameraState.posAdjust;
		a_out.posAdjust[0] = adj.x;
		a_out.posAdjust[1] = adj.y;
		a_out.posAdjust[2] = adj.z;

		const auto& pos = cam->world.translate;
		a_out.camPos[0] = pos.x;
		a_out.camPos[1] = pos.y;
		a_out.camPos[2] = pos.z;

		constexpr int kCount = static_cast<int>(std::size(rd->depthStencilTargets));
		static_assert(kCount <= 16);
		a_out.depthCount = kCount;
		for (int i = 0; i < kCount; ++i) {
			a_out.depthSRVs[i] = rd->depthStencilTargets[i].srViewDepth;
		}

		a_out.hide = MenuBlocks();
		return true;
	}

	// ------------------------------------------------------------------
	// Events -> blood bursts
	// ------------------------------------------------------------------
	bool Bleeds(RE::Actor* a_actor)
	{
		auto race = a_actor->race;
		if (!race) return true;
		for (auto& kw : BloodFX::GetSettings().noBloodKeywords) {
			if (race->HasKeywordString(kw)) return false;
		}
		return true;
	}

	RE::NiPoint3 ChestPos(RE::Actor* a_actor)
	{
		auto p = a_actor->GetPosition();
		p.z += 80.0f;   // rough chest height for humans
		return p;
	}

	void Queue(const RE::NiPoint3& a_pos, const RE::NiPoint3& a_dir, std::uint32_t a_count, float a_speed)
	{
		const float pos[3]{ a_pos.x, a_pos.y, a_pos.z };
		const float dir[3]{ a_dir.x, a_dir.y, a_dir.z };
		BloodFX::QueueBurst(pos, dir, a_count, a_speed);
	}

	class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
		{
			auto target = a_event.target.get();
			auto actor  = target ? target->As<RE::Actor>() : nullptr;
			if (!actor || !Bleeds(actor)) return RE::BSEventNotifyControl::kContinue;

			const auto&  hd = a_event.hitData;
			RE::NiPoint3 pos{ hd.impactData.location.x, hd.impactData.location.y, hd.impactData.location.z };
			RE::NiPoint3 dir{ hd.impactData.velocity.x, hd.impactData.velocity.y, hd.impactData.velocity.z };
			if (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f) pos = ChestPos(actor);
			if (dir.Length() < 0.01f) dir = { 0.0f, 0.0f, 1.0f };
			dir.Unitize();
			dir.z += 0.35f;   // exit spray continues along the shot with a little lift
			dir.Unitize();

			const float scale = std::clamp(hd.totalDamage / 40.0f, 0.5f, 2.5f);
			Queue(pos, dir, static_cast<std::uint32_t>(BloodFX::GetSettings().dropsPerHit * scale), scale);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent& a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
		{
			if (!a_event.dead) return RE::BSEventNotifyControl::kContinue;   // fires for dying and dead
			auto ref   = a_event.actorDying.get();
			auto actor = ref ? ref->As<RE::Actor>() : nullptr;
			if (!actor || !Bleeds(actor)) return RE::BSEventNotifyControl::kContinue;

			Queue(ChestPos(actor), { 0.0f, 0.0f, 1.0f }, BloodFX::GetSettings().dropsPerKill, 0.6f);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	HitSink   g_hitSink;
	DeathSink g_deathSink;

	void OnMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type != F4SE::MessagingInterface::kGameDataReady) return;

		if (auto src = RE::TESHitEvent::GetEventSource()) src->RegisterSink(&g_hitSink);
		if (auto src = RE::TESDeathEvent::GetEventSource()) src->RegisterSink(&g_deathSink);

		auto rd = RE::BSGraphics::GetRendererData();
		void* swap = rd ? rd->renderWindow[0].swapChain : nullptr;
		if (!BloodFX::InstallPresentHook(swap, &GetFrameInput)) {
			REX::ERROR("No swap chain - BloodFX inactive");
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	BloodFX::SetLogger(&LogBridge);
	BloodFX::LoadSettings(nullptr);

	if (auto m = F4SE::GetMessagingInterface()) {
		m->RegisterListener(OnMessage);
	}
	REX::INFO("BloodFX loaded");
	return true;
}
