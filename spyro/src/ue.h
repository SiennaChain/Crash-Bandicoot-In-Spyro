// Just enough of Unreal Engine 4.19's object system to find things in Spyro
// by name: an object's class, a class's functions, a function's parameters.
//
// Everything is read with mem::Read, so a wrong offset or a stale pointer
// gives "not found" and not a crash. None of this is fast; look a thing up
// once and keep the answer.
//
// Layouts are from the engine's own 4.19 source (Class.h, UnrealType.h,
// NameTypes.h); docs/PHASE0.md says which have been seen to hold in the game.
#pragma once

#include <cstdint>
#include <string>

namespace csp::ue {

// UObject
constexpr uint32_t kObjectClass = 0x10;
constexpr uint32_t kObjectName = 0x18;  // FName: index, then number
constexpr uint32_t kObjectOuter = 0x20;
// UField
constexpr uint32_t kFieldNext = 0x28;
// UStruct
constexpr uint32_t kStructSuper = 0x30;
constexpr uint32_t kStructChildren = 0x38;
// UProperty
constexpr uint32_t kPropertyOffset = 0x44;

// `namesRva`: where in the exe the pointer to the table of names is kept.
void Init(uint32_t namesRva);

// The number the engine knows `text` by, or -1. Slow (reads the whole
// table): for the handful of names the bridge compares against every frame.
int32_t FindName(const char* text);

// The text of name number `index`; empty if there is none.
std::string NameOf(int32_t index);
std::string NameOfObject(uintptr_t object);

uintptr_t ClassOf(uintptr_t object);

// The function called `name` that `object` answers to, looked for in its
// class and then in each class that one is built on. 0 if none.
uintptr_t FindFunction(uintptr_t object, const char* name);

// How far into a function's block of parameters the one called `name` is,
// or -1. A function's return value is a parameter called "ReturnValue".
int32_t FindParam(uintptr_t function, const char* name);

// How far into `object` its property called `name` is, or -1.
int32_t FindProperty(uintptr_t object, const char* name);

}  // namespace csp::ue
