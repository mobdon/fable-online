#pragma once
// Calls into Fable's own script interface (CGameScriptInterface), the object its quest and
// cutscene scripts drive the engine through. Addresses and call shapes:
// tools/re-notes/04-script-interface.md and tools/re-notes/05-spawn-and-scripted-control.md.
//
// Every call checks the pointers it is about to use and refuses rather than faulting; one
// refusal disables the whole interface for the session so a bad build cannot keep crashing
// the game (see disabled()).
#include <cstdint>
#include <string>

namespace fo::game::script {

/** A reference to a thing, the 12-byte intelligent pointer the script interface hands out. */
class ThingRef {
  public:
    ThingRef() = default;
    ~ThingRef();
    ThingRef(ThingRef&& other) noexcept;
    ThingRef& operator=(ThingRef&& other) noexcept;
    ThingRef(const ThingRef&) = delete;
    ThingRef& operator=(const ThingRef&) = delete;

    /** True once it points at something; the thing itself may still have gone. */
    bool held() const { return held_; }
    /** Asks the engine whether the thing behind this reference is still alive. */
    bool alive() const;
    /** The CThing behind the reference, or 0 (also once the engine has freed it). */
    uintptr_t thing() const;
    /** The script wrapper the reference holds (CGameScriptThing), for its own slots. */
    uintptr_t script_thing() const;
    void release();

    void* raw() { return storage_; }

  private:
    friend bool hero(ThingRef& out);
    friend bool by_name(const char* name, ThingRef& out);
    friend bool create_near(const char* def, const char* script_name, float radius, ThingRef& out);
    void adopt(void* source);  // copy-construct from the engine's own reference

    alignas(4) unsigned char storage_[12] = {};
    bool held_ = false;
};

/** True when the script interface is up (it is null on the front end and while loading). */
bool ready();
/** True after a pointer check failed: every call then does nothing. */
bool disabled();
/** Turns the interface off for the rest of the session, with a reason for the log. */
void disable(const char* why);

/** This player's own hero. */
bool hero(ThingRef& out);
/** A thing by the script name it was created with. */
bool by_name(const char* name, ThingRef& out);

/**
 * Creates `def` near the hero, the way a quest script's CreateNear does, and names it
 * `script_name` so it can be found again. The caller then moves it where it belongs:
 * the engine decides the exact spot, which is only ever within `radius` of the hero.
 */
bool create_near(const char* def, const char* script_name, float radius, ThingRef& out);

/** Drops every action a thing is running, so it stops walking where it was told to. */
void clear_commands(ThingRef& t);
/** Whether the local game may draw the thing. Used to park a puppet instead of freeing it. */
void set_drawable(ThingRef& t, bool on);
/** Whether the local game lets the player target and hurt it: off for server creatures. */
void set_attackable(ThingRef& t, bool on);
void set_damageable(ThingRef& t, bool on);
/**
 * Puts the creature in Fable's scripted mode (`.AddScriptedMode`): its own brain stops picking
 * actions (no targeting, no combat moves), so only the mod's actions run. False when the thing
 * has no mode stack (heroes, objects) and nothing was done.
 */
bool add_scripted_mode(ThingRef& t, const char* label);
/** Releases a thing from being bound to a script. */
void set_free(ThingRef& t);
/** Turns the thing to face `yaw` radians. */
void look_in_direction(ThingRef& t, float yaw);

/**
 * Queues an animation on a creature's scripted-control component, the way a quest script's
 * `.PlayLoopingAnim` does, without needing the script runner's own handle for the thing.
 * `anim` is a name from the creature's animation map: "ST_IDLE", "ST_WALK", "ST_RUN" and
 * so on resolve to that creature's own animations. `clear_first` drops whatever the
 * component was running. Returns false (and changes nothing) when the thing has no
 * scripted-control component or any pointer looks wrong.
 */
bool play_looping_anim(ThingRef& t, const char* anim, int loops, bool clear_first, uint32_t sequence);
/** The same, played once the way `.PlayAnimation` does. Queues behind anything already queued. */
bool play_anim_once(ThingRef& t, const char* anim, bool clear_first, uint32_t sequence);
/** Logs, read-only, the fields the animation path depends on, once per thing. */
void describe_for_anims(ThingRef& t, const char* label);

/** True when the thing's scripted-control queue is empty (or the thing has none). */
bool scripted_queue_empty(ThingRef& t);
/** Why the last play_* call returned false ("" after a success). */
const char* last_anim_refusal();
/** Drops everything queued on the thing's scripted-control component. */
void clear_scripted(ThingRef& t);

/**
 * Adds `amount` (may be negative) to the hero's purse through the script interface's GiveGold
 * (slot +0x1F8), the call the dev kit's gold button uses; Fable clamps the purse at 0 and at its
 * own maximum. Game thread only. False when the interface is not up.
 */
bool give_gold(int amount);

/** Hero health, through the same calls the scripts use. Returns false if not readable. */
bool hero_health(float& current, float& max);

}  // namespace fo::game::script
