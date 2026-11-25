// portal-mod E55: My Mods (scripts\mod_manager.script, the main menu's window) switches the engine's side of a feature
// off: getting into a helicopter or a car (and a player seated in one is put out), the replay's keys and recorder, the
// destruction's damage. What a switched-off feature leaves is kept: its objects, a building's pieces as they lie, the
// holes, the takes; it only stops acting. The switches are read from $app_data_root$\mod_manager.ltx (the file the window
// writes) when a level is made, and set from Lua (level.pm_feature_set) at start and on Apply. Unknown: on.
#pragma once

class CObject;
class CActor;

namespace portal_mod_feat
{
enum EFeature
{
    heli, // E44, E47, E50: the Mi-24 the player flies
    vehicles, // E46, E51: cars and the motorbike
    replay, // E53: Insert / Home / End, the recorder
    destruct, // E54: chips, holes, the building's damage, falling pieces' crush
    count
};
inline bool on[count] = {true, true, true, true};
inline bool enabled(EFeature f) { return on[f]; }
void load(); // level_script.cpp: $app_data_root$\mod_manager.ltx
} // namespace portal_mod_feat

bool portal_mod_holder_on(CObject* o); // a car or a helicopter whose feature is on (anything else: true)
void portal_mod_feature_eject(CActor* a); // seated in a car or a helicopter whose feature is off: out
