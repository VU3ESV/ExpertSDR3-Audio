#pragma once

// The name people see. Settings, profiles and the bundle id keep their own fixed identifiers
// (Settings.cpp, Profiles.cpp), so this can change without anyone losing their data.
namespace esa {
inline constexpr char kAppDisplayName[] = "ExpertSDR3 Audio";
// The mark the Exciter page draws where AetherSDR draws its own "AetherVoice™". This app
// called the stage AetherVoice before it took AetherRX's look; the trade mark sign is
// AetherSDR's claim, and is not repeated here.
inline constexpr char kExciterWordmark[] = "AetherVoice";
} // namespace esa
