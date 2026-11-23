// Layer 2 (Unreal Engine 3): reading the engine's objects in guest memory - names, classes, properties - for code that
// works with them by name (Gears 2's and Judgment's DualSense adaptive triggers, GitHub #245). Gears1-Native's ue3/objects.h (the
// engine's object layout as data, found in memory by Discover unless given) with Gears3-Native's slice-at-a-time
// lookups (FindClassRange, FindInstancesOfRange) and PropertyType. From KiltedKernel's Gears 3 port. Our code (MIT).
// Gears 2, Judgment and Gears 1 build this file (the last two from GN_LAYER0_HOME; Gears 3 still has its own,
// fixed-layout copy - see the unify audit). Gears 1 gives its layout (layer3_gow1), as Judgment does.
//   FNameEntry  {u32 Index (= n * 2, bit 0 = 16-bit text), u32 HashNext, char Name[]} (text at Layout::name_text);
//               GNames = TArray of entry pointers, its first names the engine's hardcoded ones (None, ByteProperty...)
//   UObject     ObjectInternalInteger (its own GObjObjects slot), Outer, Name (index, number), Class
//   UStruct     SuperStruct, Children (first UField; UField Next); UProperty ElementSize, Offset; UBoolProperty BitMask
// Gears 2 (2008) [code]: slot +4, Name +44 (index, number), Class +52, SuperStruct +60, FNameEntry text +8 - the rest is
// found at start-up (Discover) and logged ("ue3 objects: layout found: ...").
// Judgment (2013): Gears 3's layout, given by its caller (layer3_gowj/adaptive_triggers.cpp) - Discover is not used
// there (with the slot at +32 its Outer search could take ObjectArchetype at +56).
// Every read is checked (an address that isn't committed reads as 0); safe from any thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue3::obj {

// byte offsets of the fields above (0: not known)
struct Layout {
  uint32_t slot = 0, outer = 0, name = 0, cls = 0;          // UObject
  uint32_t super = 0, children = 0, next = 0;               // UStruct, UField
  uint32_t elem_size = 0, prop_offset = 0, bool_mask = 0;   // UProperty, UBoolProperty
  uint32_t name_text = 8;                                   // FNameEntry's text
  uint32_t name_wide = 0;                                   // 1: the text is 16-bit characters (big-endian)
  uint32_t name_number = 1;                                 // 1: an FName is (index, number); 0: the index alone
};

// Finds GNames and GObjObjects: from the engine's two globals (`names_var`, `objects_var`: TArray {data, count, max})
// when known, else by searching memory; then the layout (`known` when given, else discovered and logged). Discover
// reads every object a few times (about a second): call from a worker thread until it returns true (cheap to retry
// while the engine starts: the tables are checked first).
bool Init(uint32_t names_var, uint32_t objects_var, const Layout* known = nullptr);
bool Ready();
const Layout& CurrentLayout();
std::string LayoutText(const Layout& l);            // "slot,outer,name,class,super,children,next,size,offset,mask,text,wide,number"
bool ParseLayout(const std::string& text, Layout* out);

uint32_t Read32(uint32_t address);  // big-endian guest word; 0 when unreadable
float ReadFloat(uint32_t address);
bool Readable(uint32_t address, uint32_t size);
bool Write32(uint32_t address, uint32_t value);
bool Write8(uint32_t address, uint8_t value);

std::string Name(uint32_t index);
std::string ObjectName(uint32_t object);              // "Name" or "Name_<n>"
uint32_t ClassOf(uint32_t object);
bool IsLive(uint32_t object);                         // still in the object table at its own slot (not freed / reused)
uint32_t SuperOf(uint32_t structure);                 // UStruct's SuperStruct (0 at the top)
uint32_t FindClass(const char* name);                 // the UClass object
// FindClass over table slots [first, first + count) only - for a class that may not be loaded yet, so a caller polling
// for it from the game thread spreads each pass over several frames. A hit is cached like FindClass's. *total = the
// table's slot count now.
uint32_t FindClassRange(const char* name, uint32_t first, uint32_t count, uint32_t* total);
bool IsA(uint32_t object, uint32_t cls);              // object's class is cls or derives from it
bool ClassIsA(uint32_t cls, uint32_t base);       // 0.6.3: the UClass `cls` is `base` or derives from it (a class value, e.g. GameReplicationInfo.GameClass)
uint32_t FindObject(const char* name);                // the first object of that name (Default__X included)
std::vector<uint32_t> FindInstancesOf(uint32_t cls);  // every non-default instance of cls or a subclass
// The same over table slots [first, first + count) only. *total = the table's slot count now.
std::vector<uint32_t> FindInstancesOfRange(uint32_t cls, uint32_t first, uint32_t count, uint32_t* total);
std::vector<uint32_t> AllObjects();                   // every object in GObjObjects
int PropertyOffset(uint32_t cls, const char* property);  // -1 when not found
int PropertySize(uint32_t cls, const char* property);    // ElementSize, -1 when not found
int PropertyArrayDim(uint32_t cls, const char* property);  // static array length (1 = not an array), -1 when not found
uint32_t BoolMask(uint32_t cls, const char* property);   // 0 for any other property type
std::string PropertyType(uint32_t cls, const char* property);   // "IntProperty", "FloatProperty"...; "" when not found

}  // namespace ue3::obj
