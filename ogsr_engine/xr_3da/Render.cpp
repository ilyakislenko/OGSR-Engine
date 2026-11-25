#include "stdafx.h"
#include "render.h"

IRender_interface::~IRender_interface(){};

// ENGINE_API	IRender_interface*	Render		= NULL;

// resources
IRender_Light::~IRender_Light() { ::Render->light_destroy(this); }

ENGINE_API ShExports shader_exports{};

ENGINE_API GRASS_SHADER_DATA grass_shader_data{};
ENGINE_API GRASS_SHADER_DATA_OLD grass_shader_data_old{};
ENGINE_API PORTAL_MOD_DRAW portal_mod_draw[2]{}; // portal-mod
ENGINE_API bool portal_mod_pass{}; // portal-mod E2
ENGINE_API bool portal_mod_view_prev{}; // portal-mod E19
ENGINE_API bool portal_mod_drone_pass{}; // portal-mod E45
ENGINE_API float portal_mod_pass_zoom{1.f}; // portal-mod E45
ENGINE_API Fmatrix portal_mod_full_std{};
ENGINE_API bool portal_mod_replay{}; // portal-mod E53
ENGINE_API float portal_mod_replay_shader_time{}; // portal-mod E53
ENGINE_API u32 portal_mod_replay_rain_ms{}; // portal-mod E53b
ENGINE_API u32 portal_mod_replay_rain_dms{};
ENGINE_API float portal_mod_replay_rain_dt{};
ENGINE_API Fvector portal_mod_replay_wind{};
