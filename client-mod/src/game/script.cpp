#include "script.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "../log.h"

namespace fo::game::script {
namespace {

// Fable.exe has no ASLR, so every address here is fixed. See tools/re-notes/.
constexpr uintptr_t kInterfacePtr = 0x0143E8F8;  // CGameScriptInterface**
constexpr uintptr_t kStrCtor = 0x0099EBF0;       // CCharString(const char*, int len)
constexpr uintptr_t kStrDtor = 0x0099EAE0;       // ~CCharString()
constexpr uintptr_t kRefCopy = 0x004ABE90;       // reference copy constructor
constexpr uintptr_t kRefDtor = 0x004AA840;       // reference destructor
constexpr uintptr_t kRefAlive = 0x004AB130;      // reference -> is the thing still there

// Script interface slots.
constexpr unsigned kHero = 0x118;            // () -> reference*
constexpr unsigned kByName = 0x120;          // (reference* out, CCharString* name) -> out
constexpr unsigned kCreateNear = 0x170;      // (out, def, nearUid, radius, scriptName, flags)
constexpr unsigned kClearCommands = 0x840;   // (reference*)
constexpr unsigned kDrawable = 0x7FC;        // (reference*, bool)
constexpr unsigned kAttackable = 0x728;      // (reference*, bool)
constexpr unsigned kDamageable = 0x810;      // (reference*, bool)
constexpr unsigned kSetFree = 0x7BC;         // (reference*)
constexpr unsigned kLookInDirection = 0x768; // (reference*, float yaw, bool)
constexpr unsigned kHeroHealth = 0x404;      // () -> float
constexpr unsigned kHeroMaxHealth = 0x408;   // () -> float

// NTCScriptedControl: what the script runner's `.PlayLoopingAnim` ends up calling.
// tools/re-notes/05-spawn-and-scripted-control.md, "Playing animations".
constexpr uintptr_t kOperatorNew = 0x00BFEA1A;     // void* __cdecl operator new(size_t)
constexpr uintptr_t kPlayAnimCtor = 0x009034F0;    // CActionPlayAnimation ctor, 11 args
constexpr uintptr_t kPlayAnimVtable = 0x0128D6F4;  // CActionPlayAnimation vtable
constexpr size_t kPlayAnimSize = 0x28;
constexpr uintptr_t kQueueAction = 0x007137D0;     // CTCScriptedControl::Add(action*)
constexpr uintptr_t kQueueClear = 0x00713630;      // CTCScriptedControl: stop and drop all
constexpr uintptr_t kStopCreatureAction = 0x00662650;  // CThingCreatureBase: end current action
constexpr uintptr_t kScriptGlobalFlag = 0x01375748;    // byte the runner passes through
constexpr uint32_t kScriptedControlVtable = 0x012658E4;
constexpr uint32_t kScriptedControlId = 0x1F;
// CThing layout used to find a component (the inline lookup in every scripted-thing slot).
constexpr uint32_t kThingFlags = 0x20;       // sign bit: has a component table
constexpr uint32_t kThingComponents = 0x44;  // {begin, end} of sorted (id, component*) pairs
constexpr uint32_t kThingDeadFlags = 0x91;   // bit 0: being removed
constexpr uint32_t kThingCreatureFlags = 0x6C;  // bit 3: is a creature with an action list

// CThing slot the CreateNear sequence reads the "near" value from.
constexpr unsigned kThingNearValue = 0x18;

std::atomic<bool> g_disabled{false};

bool readable(uintptr_t addr, size_t n) {
    if (!addr || addr < 0x10000) return false;
    unsigned char probe[16];
    SIZE_T got = 0;
    while (n) {
        size_t chunk = n < sizeof(probe) ? n : sizeof(probe);
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), probe, chunk, &got) ||
            got != chunk)
            return false;
        addr += chunk;
        n -= chunk;
    }
    return true;
}

uintptr_t read_ptr(uintptr_t addr) {
    uint32_t v = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), &v, 4, &got) || got != 4) return 0;
    return v;
}

/** The interface object, or 0 when the game has not built it yet. */
uintptr_t iface() {
    if (g_disabled.load()) return 0;
    uintptr_t obj = read_ptr(kInterfacePtr);
    if (!obj) return 0;
    uintptr_t vt = read_ptr(obj);
    // Fable's vtables all live in .rdata; anything else means we are reading rubbish.
    if (vt < 0x01200000 || vt >= 0x01400000) {
        disable("script interface vtable out of range");
        return 0;
    }
    return obj;
}

/** The function in a slot, checked to be inside the code section. */
void* slot(uintptr_t obj, unsigned offset) {
    uintptr_t fn = read_ptr(read_ptr(obj) + offset);
    if (fn < 0x00401000 || fn >= 0x0122E000) {
        disable("script interface slot out of range");
        return nullptr;
    }
    return reinterpret_cast<void*>(fn);
}

/** Fable's own string type, built and freed with the engine's own functions. */
class Str {
  public:
    explicit Str(const char* s) {
        reinterpret_cast<void(__thiscall*)(Str*, const char*, int)>(kStrCtor)(this, s ? s : "", -1);
        built_ = true;
    }
    ~Str() {
        if (built_) reinterpret_cast<void(__thiscall*)(Str*)>(kStrDtor)(this);
    }
    Str(const Str&) = delete;
    Str& operator=(const Str&) = delete;
    void* raw() { return &rep_; }

  private:
    void* rep_ = nullptr;
    bool built_ = false;
};

template <typename Fn>
Fn as(void* fn) {
    return reinterpret_cast<Fn>(fn);
}

}  // namespace

bool ready() { return iface() != 0; }
bool disabled() { return g_disabled.load(); }

void disable(const char* why) {
    if (!g_disabled.exchange(true)) fo::log("script: disabled (%s). No further engine calls this session.", why);
}

// ---------- ThingRef ----------

void ThingRef::adopt(void* source) {
    release();
    as<void(__thiscall*)(void*, void*)>(reinterpret_cast<void*>(kRefCopy))(storage_, source);
    held_ = true;
}

void ThingRef::release() {
    if (!held_) return;
    held_ = false;
    if (g_disabled.load()) return;
    as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kRefDtor))(storage_);
    std::memset(storage_, 0, sizeof(storage_));
}

ThingRef::~ThingRef() { release(); }

ThingRef::ThingRef(ThingRef&& other) noexcept {
    std::memcpy(storage_, other.storage_, sizeof(storage_));
    held_ = other.held_;
    other.held_ = false;
    std::memset(other.storage_, 0, sizeof(other.storage_));
}

ThingRef& ThingRef::operator=(ThingRef&& other) noexcept {
    if (this != &other) {
        release();
        std::memcpy(storage_, other.storage_, sizeof(storage_));
        held_ = other.held_;
        other.held_ = false;
        std::memset(other.storage_, 0, sizeof(other.storage_));
    }
    return *this;
}

bool ThingRef::alive() const {
    if (!held_ || g_disabled.load()) return false;
    return as<bool(__thiscall*)(const void*)>(reinterpret_cast<void*>(kRefAlive))(storage_);
}

uintptr_t ThingRef::thing() const {
    if (!held_) return 0;
    uint32_t t = 0;
    std::memcpy(&t, storage_ + 4, 4);
    return readable(t, 4) ? t : 0;
}

// ---------- lookups ----------

bool hero(ThingRef& out) {
    uintptr_t obj = iface();
    if (!obj) return false;
    void* fn = slot(obj, kHero);
    if (!fn) return false;
    void* src = as<void*(__thiscall*)(void*)>(fn)(reinterpret_cast<void*>(obj));
    if (!readable(reinterpret_cast<uintptr_t>(src), 12)) return false;
    out.adopt(src);
    return out.alive();
}

bool by_name(const char* name, ThingRef& out) {
    uintptr_t obj = iface();
    if (!obj || !name || !*name) return false;
    void* fn = slot(obj, kByName);
    if (!fn) return false;
    Str str(name);
    alignas(4) unsigned char temp[12] = {};
    void* src = as<void*(__thiscall*)(void*, void*, void*)>(fn)(reinterpret_cast<void*>(obj), temp, str.raw());
    bool ok = readable(reinterpret_cast<uintptr_t>(src), 12);
    if (ok) out.adopt(src);
    as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kRefDtor))(temp);
    return ok && out.alive();
}

bool create_near(const char* def, const char* script_name, float radius, ThingRef& out) {
    uintptr_t obj = iface();
    if (!obj || !def || !*def || !script_name || !*script_name) return false;

    ThingRef near_hero;
    if (!hero(near_hero)) return false;
    uintptr_t thing = near_hero.thing();
    if (!thing) return false;
    void* near_fn = slot(thing, kThingNearValue);
    void* create_fn = slot(obj, kCreateNear);
    if (!near_fn || !create_fn) return false;

    // The script runner's CreateNear sequence: ask the near thing for the value the
    // interface wants, then hand it to the interface with the def and the new name. The
    // three values after it belong to the create call, not to this one (they are read off
    // the stack), so they are passed again below.
    void* near_value = as<void*(__thiscall*)(void*)>(near_fn)(reinterpret_cast<void*>(thing));

    Str def_str(def);
    Str name_str(script_name);
    alignas(4) unsigned char temp[12] = {};
    as<void(__thiscall*)(void*, void*, void*, void*, float, void*, int)>(create_fn)(
        reinterpret_cast<void*>(obj), temp, def_str.raw(), near_value, radius, name_str.raw(), 0);

    bool ok = as<bool(__thiscall*)(const void*)>(reinterpret_cast<void*>(kRefAlive))(temp);
    if (ok) out.adopt(temp);
    as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kRefDtor))(temp);
    return ok && out.alive();
}

// ---------- commands ----------

namespace {
void one_arg(ThingRef& t, unsigned offset) {
    uintptr_t obj = iface();
    if (!obj || !t.held()) return;
    void* fn = slot(obj, offset);
    if (fn) as<void(__thiscall*)(void*, void*)>(fn)(reinterpret_cast<void*>(obj), t.raw());
}

void flag(ThingRef& t, unsigned offset, bool on) {
    uintptr_t obj = iface();
    if (!obj || !t.held()) return;
    void* fn = slot(obj, offset);
    if (fn) as<void(__thiscall*)(void*, void*, int)>(fn)(reinterpret_cast<void*>(obj), t.raw(), on ? 1 : 0);
}
}  // namespace

void clear_commands(ThingRef& t) { one_arg(t, kClearCommands); }
void set_free(ThingRef& t) { one_arg(t, kSetFree); }
void set_drawable(ThingRef& t, bool on) { flag(t, kDrawable, on); }
void set_attackable(ThingRef& t, bool on) { flag(t, kAttackable, on); }
void set_damageable(ThingRef& t, bool on) { flag(t, kDamageable, on); }

void look_in_direction(ThingRef& t, float yaw) {
    uintptr_t obj = iface();
    if (!obj || !t.held()) return;
    void* fn = slot(obj, kLookInDirection);
    if (fn) as<void(__thiscall*)(void*, void*, float, int)>(fn)(reinterpret_cast<void*>(obj), t.raw(), yaw, 1);
}

// ---------- scripted control ----------

namespace {
/** The thing's CTCScriptedControl, or 0. Reads only; never calls the engine. */
uintptr_t scripted_control(ThingRef& t) {
    if (g_disabled.load() || !t.held()) return 0;
    uintptr_t thing = t.thing();
    if (!thing || !readable(thing, 0x100)) return 0;
    uint8_t dead = 0;
    std::memcpy(&dead, reinterpret_cast<const void*>(thing + kThingDeadFlags), 1);
    if (dead & 1) return 0;
    int32_t flags = static_cast<int32_t>(read_ptr(thing + kThingFlags));
    if (flags >= 0) return 0;
    uintptr_t begin = read_ptr(thing + kThingComponents);
    uintptr_t end = read_ptr(thing + kThingComponents + 4);
    if (!begin || end < begin || end - begin > 0x100 * 8 || !readable(begin, end - begin)) return 0;
    for (uintptr_t p = begin; p < end; p += 8) {
        uint32_t id = read_ptr(p);
        if (id != kScriptedControlId) continue;
        uintptr_t comp = read_ptr(p + 4);
        if (comp && readable(comp, 0x20) && read_ptr(comp) == kScriptedControlVtable) return comp;
        return 0;
    }
    return 0;
}

bool queue_empty_at(uintptr_t comp) {
    uintptr_t head = read_ptr(comp + 0x14);  // std::list sentinel
    return !head || read_ptr(head) == head;
}
}  // namespace

bool scripted_queue_empty(ThingRef& t) {
    uintptr_t comp = scripted_control(t);
    return !comp || queue_empty_at(comp);
}

void clear_scripted(ThingRef& t) {
    uintptr_t comp = scripted_control(t);
    if (!comp) return;
    as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kQueueClear))(reinterpret_cast<void*>(comp));
}

bool play_looping_anim(ThingRef& t, const char* anim, int loops, bool clear_first, uint32_t sequence) {
    if (!anim || !*anim || !iface()) return false;
    uintptr_t comp = scripted_control(t);
    if (!comp) return false;
    uintptr_t thing = t.thing();
    if (read_ptr(kPlayAnimVtable) != 0x00905070) {  // first slot: its destructor
        disable("play-animation action vtable does not match this Fable.exe");
        return false;
    }

    // The same order as the slot `.PlayLoopingAnim` lands in (handle vtable +0x50).
    if (clear_first) {
        as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kQueueClear))(reinterpret_cast<void*>(comp));
        if (read_ptr(thing + kThingCreatureFlags) & 8)
            as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kStopCreatureAction))(reinterpret_cast<void*>(thing));
    }

    void* action = as<void*(__cdecl*)(size_t)>(reinterpret_cast<void*>(kOperatorNew))(kPlayAnimSize);
    if (!action) return false;
    uint8_t global = 0;
    if (readable(kScriptGlobalFlag, 1)) std::memcpy(&global, reinterpret_cast<const void*>(kScriptGlobalFlag), 1);
    Str name(anim);
    // (name, 0, looping=1, loops, 0, sequence, 0, 1, global, 0, 0): the runner's defaults.
    as<void*(__thiscall*)(void*, void*, int, int, int, int, int, int, int, int, int, int)>(
        reinterpret_cast<void*>(kPlayAnimCtor))(action, name.raw(), 0, 1, loops, 0, static_cast<int>(sequence), 0, 1,
                                               global, 0, 0);
    as<void(__thiscall*)(void*, void*)>(reinterpret_cast<void*>(kQueueAction))(reinterpret_cast<void*>(comp), action);
    return true;
}

bool hero_health(float& current, float& max) {
    uintptr_t obj = iface();
    if (!obj) return false;
    void* hp = slot(obj, kHeroHealth);
    void* hpMax = slot(obj, kHeroMaxHealth);
    if (!hp || !hpMax) return false;
    current = as<float(__thiscall*)(void*)>(hp)(reinterpret_cast<void*>(obj));
    max = as<float(__thiscall*)(void*)>(hpMax)(reinterpret_cast<void*>(obj));
    return max > 0;
}

}  // namespace fo::game::script
