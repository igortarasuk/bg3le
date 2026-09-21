// Upstream compiles GameDefinitions/Stats/StatsObject.inl as a translation
// unit: BG3Extender.vcxproj lists it under ClCompile and nothing includes it.
// CMake will not generate a compile rule for an .inl, so this wrapper gives it
// one. It defines the stats::Object members.
//
// The included code is by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); only this wrapper is ours.
#include <GameDefinitions/Stats/StatsObject.inl>
