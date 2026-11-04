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

/** What 0x00662D80 reads first: thing+0x70 must be an object with a vtable (0x0042B0A2). */
constexpr uint32_t kThingAnimSource = 0x70;
/** What a script reference holds: a CGameScriptThing, with the CThingPtr at +0x0C. */
constexpr uint32_t kGameScriptThingVtable = 0x012852CC;
constexpr uint32_t kGameScriptThingPtr = 0x0C;
/** The second object it reads (+0x134, then its vfunc +0x10 through 0x0073A6E0). */
constexpr uint32_t kThingAnimTables = 0x134;

// CThing slot the CreateNear sequence reads the "near" value from.
constexpr unsigned kThingNearValue = 0x18;

std::atomic<bool> g_disabled{false};
const char* g_lastRefusal = "";  // why the last animation call returned false (game thread only)

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

uintptr_t ThingRef::script_thing() const {
    if (!held_) return 0;
    uint32_t t = 0;
    std::memcpy(&t, storage_ + 4, 4);
    return readable(t, 4) ? t : 0;
}

uintptr_t ThingRef::thing() const {
    // The reference holds a CGameScriptThing, the script runner's wrapper. The CThing is
    // behind its CThingPtr at +0x0C: {vtable, control*}, and the control block's first
    // dword is the thing (0 once the engine has freed it), as 0x00A01B50 reads it.
    uintptr_t gst = script_thing();
    if (!gst || !readable(gst, kGameScriptThingPtr + 8) || read_ptr(gst) != kGameScriptThingVtable) return 0;
    uintptr_t ctrl = read_ptr(gst + kGameScriptThingPtr + 4);
    if (!ctrl || !readable(ctrl, 4)) return 0;
    uintptr_t thing = read_ptr(ctrl);
    return thing && readable(thing, 0x110) ? thing : 0;
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
    // The "near" value comes from the script wrapper's own slot, as the script runner asks it.
    uintptr_t thing = near_hero.script_thing();
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

// The MSVC class name behind an object's vtable (".?AVCThingCreatureBase@@"), read through
// its RTTI locator at vtable-4. False for anything that is not a live C++ object of
// Fable.exe: garbage, freed memory and plain data all fail one of these checks.
bool class_name(uintptr_t obj, char* out, size_t n) {
    if (n < 8) return false;
    out[0] = 0;
    if (!obj || !readable(obj, 4)) return false;
    uintptr_t vt = read_ptr(obj);
    if (vt < 0x01200000 || vt >= 0x01400000 || !readable(vt - 4, 8)) return false;
    uintptr_t col = read_ptr(vt - 4);
    if (col < 0x01200000 || col >= 0x01400000 || !readable(col, 20) || read_ptr(col) != 0) return false;
    uintptr_t td = read_ptr(col + 12);
    if (td < 0x01200000 || td >= 0x01440000 || !readable(td + 8, 4)) return false;
    size_t i = 0;
    for (; i + 1 < n; ++i) {
        if (!readable(td + 8 + i, 1)) return false;
        char c = *reinterpret_cast<const char*>(td + 8 + i);
        out[i] = c;
        if (!c) break;
    }
    out[i < n ? i : n - 1] = 0;
    return out[0] == '.' && out[1] == '?' && out[2] == 'A';
}

// 0x00662D80's second read: [[[thing+0x134]+0x0C]]+0x08 (0x0073A6E0), whose vfunc +0x10 it calls.
uintptr_t anim_tables(uintptr_t thing) {
    uintptr_t a = read_ptr(thing + kThingAnimTables);
    if (!a || !readable(a + 0x0C, 4)) return 0;
    uintptr_t b = read_ptr(a + 0x0C);
    if (!b || !readable(b, 4)) return 0;
    uintptr_t c = read_ptr(b);
    if (!c || !readable(c + 8, 4)) return 0;
    return read_ptr(c + 8);
}

// Everything 0x00662D80 dereferences before it reaches the animation tables: the thing must
// be a live creature object, +0x70 its def holder and +0x134 another live object. A refusal
// names the first check that failed, for the log.
const char* anim_target_problem(uintptr_t thing) {
    char name[96];
    if (!class_name(thing, name, sizeof(name))) return "thing is not a live object";
    if (!std::strstr(name, "Creature")) return "thing is not a creature";
    if (!class_name(read_ptr(thing + kThingAnimSource), name, sizeof(name))) return "def holder (+0x70) not set";
    if (!class_name(anim_tables(thing), name, sizeof(name))) return "anim tables (+0x134) not set";
    return nullptr;
}

bool queue_empty_at(uintptr_t comp) {
    uintptr_t head = read_ptr(comp + 0x14);  // std::list sentinel
    return !head || read_ptr(head) == head;
}
}  // namespace

void describe_for_anims(ThingRef& t, const char* label) {
    uintptr_t thing = t.thing();
    if (!thing || !readable(thing, 0x110)) {
        fo::log("anims: %s thing unreadable", label);
        return;
    }
    uintptr_t src = read_ptr(thing + kThingAnimSource);
    char cls[96], srcCls[96], tabCls[96];
    if (!class_name(thing, cls, sizeof(cls))) std::strcpy(cls, "?");
    if (!class_name(src, srcCls, sizeof(srcCls))) std::strcpy(srcCls, "?");
    if (!class_name(anim_tables(thing), tabCls, sizeof(tabCls))) std::strcpy(tabCls, "?");
    const char* problem = anim_target_problem(thing);
    fo::log("anims: %s is %s, +0x70 %s, +0x134 %s: %s", label, cls, srcCls, tabCls,
            problem ? problem : "can animate");
    fo::log("anims: %s thing %08X vt %08X flags20 %08X %08X %08X %08X +6C %08X +70 %08X (vt %08X) +100 %08X",
            label, static_cast<unsigned>(thing), static_cast<unsigned>(read_ptr(thing)),
            static_cast<unsigned>(read_ptr(thing + 0x20)), static_cast<unsigned>(read_ptr(thing + 0x24)),
            static_cast<unsigned>(read_ptr(thing + 0x28)), static_cast<unsigned>(read_ptr(thing + 0x2C)),
            static_cast<unsigned>(read_ptr(thing + 0x6C)), static_cast<unsigned>(src),
            static_cast<unsigned>(src && readable(src, 4) ? read_ptr(src) : 0),
            static_cast<unsigned>(read_ptr(thing + 0x100)));
}

bool scripted_queue_empty(ThingRef& t) {
    // The creature's own action slot: empty, or holding an action that has finished.
    uintptr_t thing = t.thing();
    if (!thing || !readable(thing, 0x110)) return true;
    uintptr_t cur = read_ptr(thing + 0x100);
    if (!cur || !readable(cur + 0x61, 1)) return true;
    uint8_t done = 0;
    std::memcpy(&done, reinterpret_cast<const void*>(cur + 0x61), 1);
    return done != 0;
}

void clear_scripted(ThingRef& t) {
    uintptr_t comp = scripted_control(t);
    if (!comp) return;
    as<void(__thiscall*)(void*)>(reinterpret_cast<void*>(kQueueClear))(reinterpret_cast<void*>(comp));
}

namespace {
// What CActionPlayAnimation::Update (0x00712900) does once the scripted-control queue
// starts it, done here directly: a CREATURE_HERO stand-in has no CTCScriptedControl
// (the component comes from the def, and the hero's def does not list it), but every
// creature has its own action slot at thing+0x100 and queue at thing+0x108.
constexpr uintptr_t kCreatureIsBusy = 0x0085F0C0;    // CThing: true = no new actions now
constexpr uintptr_t kCreatureAnimByName = 0x00662D80;  // CThing: (CCharString*, int -1) -> anim
constexpr uintptr_t kCreatureAnimActionCtor = 0x008425E0;  // 11 args, object 0xB4 bytes
constexpr size_t kCreatureAnimActionSize = 0xB4;
constexpr uintptr_t kCreatureSetAction = 0x006644F0;   // CThing: (action*) -> bool, clones it
constexpr uintptr_t kCreatureQueueAction = 0x00664490; // CThing: (action*), clones it
constexpr uint32_t kCreatureCurrentAction = 0x100;

bool creature_ok(uintptr_t thing) {
    if (!thing || !readable(thing, 0x110)) return false;
    uint8_t dead = 0;
    std::memcpy(&dead, reinterpret_cast<const void*>(thing + kThingDeadFlags), 1);
    if (dead & 1) return false;
    return (read_ptr(thing + kThingCreatureFlags) & 8) != 0;
}

bool vtable_like(uintptr_t obj) {
    if (!obj || !readable(obj, 4)) return false;
    uintptr_t vt = read_ptr(obj);
    return vt >= 0x01200000 && vt < 0x01400000;
}


bool queue_anim(ThingRef& t, const char* anim, bool looping, int loops, bool clear_first, uint32_t sequence) {
    g_lastRefusal = "no reference";
    if (!anim || !*anim || !iface() || !t.held()) return false;
    uintptr_t thing = t.thing();
    g_lastRefusal = "dead or not a creature";
    if (!creature_ok(thing)) return false;
    // online-7 crashed inside 0x0042B0A2 with thing+0x70 pointing at string data: never
    // call into the animation lookup unless that field looks like a real object.
    if (const char* problem = anim_target_problem(thing)) {
        static int logged = 0;
        if (logged < 20) {
            ++logged;
            fo::log("anims: refused %s on %08X: %s", anim, static_cast<unsigned>(thing), problem);
        }
        g_lastRefusal = problem;
        return false;
    }
    if (read_ptr(kCreatureAnimActionCtor) != 0x2C24448B) {  // mov eax, [esp+0x2C]: the ctor's first bytes
        disable("creature animation action does not match this Fable.exe");
        return false;
    }
    void* self = reinterpret_cast<void*>(thing);
    g_lastRefusal = "busy (in a mode that takes no new actions)";
    if (as<bool(__thiscall*)(void*)>(reinterpret_cast<void*>(kCreatureIsBusy))(self)) return false;
    g_lastRefusal = "out of memory";

    Str name(anim);
    void* handle = as<void*(__thiscall*)(void*, void*, int)>(reinterpret_cast<void*>(kCreatureAnimByName))(
        self, name.raw(), -1);
    void* action = as<void*(__cdecl*)(size_t)>(reinterpret_cast<void*>(kOperatorNew))(kCreatureAnimActionSize);
    if (!action) return false;
    uint8_t global = 0;
    if (readable(kScriptGlobalFlag, 1)) std::memcpy(&global, reinterpret_cast<const void*>(kScriptGlobalFlag), 1);
    alignas(4) uint32_t out[2] = {0, 0};
    // (creature, anim, 0, looping, loops, &out, 0, global, 0, 0, seq): the arguments the
    // scripted action passes for `.PlayLoopingAnim` / `.PlayAnimation` defaults.
    as<void*(__thiscall*)(void*, void*, void*, int, int, int, void*, int, int, int, int, int)>(
        reinterpret_cast<void*>(kCreatureAnimActionCtor))(action, self, handle, 0, looping ? 1 : 0,
                                                          looping ? loops : -1, out, 0, global, 0, 0,
                                                          static_cast<int>(sequence));
    bool ok = true;
    uintptr_t cur = read_ptr(thing + kCreatureCurrentAction);
    if (!clear_first && cur) {
        as<void(__thiscall*)(void*, void*)>(reinterpret_cast<void*>(kCreatureQueueAction))(self, action);
    } else {
        ok = as<bool(__thiscall*)(void*, void*)>(reinterpret_cast<void*>(kCreatureSetAction))(self, action);
    }
    // Both calls keep a copy (action vtable +0x2C clones), so ours is deleted, as the engine does.
    uintptr_t vt = read_ptr(reinterpret_cast<uintptr_t>(action));
    as<void(__thiscall*)(void*, int)>(reinterpret_cast<void*>(read_ptr(vt)))(action, 1);
    g_lastRefusal = ok ? "" : "the creature turned the action down";
    return ok;
}
}  // namespace

const char* last_anim_refusal() { return g_lastRefusal; }

bool play_looping_anim(ThingRef& t, const char* anim, int loops, bool clear_first, uint32_t sequence) {
    return queue_anim(t, anim, true, loops, clear_first, sequence);
}

bool play_anim_once(ThingRef& t, const char* anim, bool clear_first, uint32_t sequence) {
    return queue_anim(t, anim, false, -1, clear_first, sequence);
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
