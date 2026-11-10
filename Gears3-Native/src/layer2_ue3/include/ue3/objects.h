// Layer 2 (Unreal Engine 3): reading the engine's objects in guest memory - names, classes, properties - for code that
// works with them by name (layer 3's mod chapters and content packages). From KiltedKernel's Gears 3 port
// (runtime/ue3/reflection.cpp there), found on Gears 3 memory dumps. Our code (MIT).
//   FNameEntry  {u32 Index (= n * 2), u32 HashNext, char Name[]}; GNames = array of entry pointers
//   UObject     +0 vtable, +8 ObjectFlags (64-bit; low word +12), +32 ObjectInternalInteger (its GObjObjects slot),
//               +40 Outer, +44 Name (index, number), +52 Class
//   UStruct     +64 super struct, +68 first child (UField; UField +60 Next)
//   UProperty   +64 ArrayDim, +68 ElementSize, +84 Offset; UBoolProperty +104 BitMask
// Every read is checked (an address that isn't committed reads as 0); safe from any thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue3::obj {

// Finds GNames and GObjObjects: from the engine's two globals (`names_var`, `objects_var`: TArray {data, count, max},
// per game version), else by searching memory. Call from a worker thread until it returns true (cheap to retry).
bool Init(uint32_t names_var, uint32_t objects_var);
bool Ready();

uint32_t Read32(uint32_t address);  // big-endian guest word; 0 when unreadable
float ReadFloat(uint32_t address);
bool Readable(uint32_t address, uint32_t size);
bool Write32(uint32_t address, uint32_t value);
bool Write8(uint32_t address, uint8_t value);

std::string Name(uint32_t index);
std::string ObjectName(uint32_t object);              // "Name" or "Name_<n>"
uint32_t ClassOf(uint32_t object);
uint32_t FindClass(const char* name);                 // the UClass object
bool IsA(uint32_t object, uint32_t cls);              // object's class is cls or derives from it
uint32_t FindObject(const char* name);                // the first object of that name (Default__X included)
std::vector<uint32_t> FindInstancesOf(uint32_t cls);  // every non-default instance of cls or a subclass
// The same over table slots [first, first + count) only, so a caller can spread a scan over several frames (0.4.6
// Gears 3 menu freeze: a full scan per call stalled the game thread). *total = the table's slot count now.
std::vector<uint32_t> FindInstancesOfRange(uint32_t cls, uint32_t first, uint32_t count, uint32_t* total);
std::vector<uint32_t> AllObjects();                   // every object in GObjObjects
int PropertyOffset(uint32_t cls, const char* property);  // -1 when not found
int PropertySize(uint32_t cls, const char* property);    // ElementSize, -1 when not found
uint32_t BoolMask(uint32_t cls, const char* property);   // 0 for any other property type

}  // namespace ue3::obj
