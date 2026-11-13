// tween.cpp — TweenService:Create + per-frame tween playback.
//
//   * Create(instance, TweenInfo, goals) validates every goal against the
//     target's property table and returns a Tween INSTANCE (TweenBase carries
//     PlaybackState, Play/Pause/Cancel and the Completed event).
//   * Play() re-reads the target's CURRENT values as the start (Roblox does
//     this on every Play), honours DelayTime, RepeatCount and Reverses.
//   * The clock is engine::step's frame delta, so a headless run animates the
//     same values the rendered client would.
#include "instance/tween.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/signal.h"
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace rbx {

namespace {

const char* const kTweenKey = "rbx.Tweens";
constexpr double kPi = 3.14159265358979323846;

struct TweenGoal {
    std::string prop;
    Variant target;
    Variant start;
};

struct TweenRun {
    Ref<Instance> target;
    Ref<Instance> tween; // the Tween instance; keeps it alive while playing
    TweenInfo info;
    std::vector<TweenGoal> goals;
    double elapsed = 0.0;    // into the current pass
    double delay_left = 0.0; // DelayTime not yet elapsed
    int pass = 0;            // completed passes
    int passes = 1;          // 1 + RepeatCount
    bool playing = false;
};

using TweenList = std::vector<TweenRun>;

TweenList* tweens(lua_State* L, bool create) {
    lua_getfield(L, LUA_REGISTRYINDEX, kTweenKey);
    auto* v = static_cast<TweenList*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!v && create) {
        v = new TweenList();
        lua_pushlightuserdata(L, v);
        lua_setfield(L, LUA_REGISTRYINDEX, kTweenKey);
    }
    return v;
}

TweenRun* find_run(lua_State* L, Instance* tw) {
    TweenList* v = tweens(L, false);
    if (!v)
        return nullptr;
    for (TweenRun& r : *v)
        if (r.tween.get() == tw)
            return &r;
    return nullptr;
}

void set_playback_state(lua_State* L, TweenRun& r, int state) {
    (void)L;
    // PlaybackState is a readonly dump property written by the engine, which
    // is exactly what set_internal_prop is for (scripts still cannot write it).
    r.tween->set_internal_prop("PlaybackState",
                               Variant(EnumItem{"PlaybackState", state}));
}

// ---- easing ----------------------------------------------------------------
// Standard Penner curves; Enum.EasingStyle value order from the API dump
// (Linear 0, Sine 1, Back 2, Quad 3, Quart 4, Quint 5, Bounce 6, Elastic 7,
// Exponential 8, Circular 9, Cubic 10).
double ease_in(double u, int style) {
    switch (style) {
    case 1:
        return 1.0 - std::cos(u * kPi / 2.0);
    case 2: {
        const double c1 = 1.70158;
        const double c3 = c1 + 1.0;
        return c3 * u * u * u - c1 * u * u;
    }
    case 3:
        return u * u;
    case 4:
        return u * u * u * u;
    case 5:
        return u * u * u * u * u;
    case 6: { // Bounce
        const double n1 = 7.5625;
        const double d1 = 2.75;
        if (u < 1.0 / d1)
            return n1 * u * u;
        if (u < 2.0 / d1) {
            u -= 1.5 / d1;
            return n1 * u * u + 0.75;
        }
        if (u < 2.5 / d1) {
            u -= 2.25 / d1;
            return n1 * u * u + 0.9375;
        }
        u -= 2.625 / d1;
        return n1 * u * u + 0.984375;
    }
    case 7: { // Elastic
        const double c4 = (2.0 * kPi) / 3.0;
        if (u == 0.0)
            return 0.0;
        if (u == 1.0)
            return 1.0;
        return -std::pow(2.0, 10.0 * u - 10.0) *
               std::sin((u * 10.0 - 10.75) * c4);
    }
    case 8:
        return u == 0.0 ? 0.0 : std::pow(2.0, 10.0 * u - 10.0);
    case 9:
        return 1.0 - std::sqrt(std::max(0.0, 1.0 - u * u));
    case 10:
        return u * u * u;
    default:
        return u; // Linear
    }
}

double apply_easing(double u, int style, int dir) {
    u = std::clamp(u, 0.0, 1.0);
    if (style == 0)
        return u;
    if (dir == 0) // In
        return ease_in(u, style);
    if (dir == 1) // Out
        return 1.0 - ease_in(1.0 - u, style);
    // InOut
    return u < 0.5 ? ease_in(u * 2.0, style) / 2.0
                   : 1.0 - ease_in((1.0 - u) * 2.0, style) / 2.0;
}

// ---- interpolation ---------------------------------------------------------

double lerp(double a, double b, double u) {
    return a + (b - a) * u;
}

struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};

Quat quat_from_cframe(const CFrame& c) {
    const double trace = c.r00 + c.r11 + c.r22;
    Quat q;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q.w = 0.25 * s;
        q.x = (c.r21 - c.r12) / s;
        q.y = (c.r02 - c.r20) / s;
        q.z = (c.r10 - c.r01) / s;
    } else if (c.r00 > c.r11 && c.r00 > c.r22) {
        const double s = std::sqrt(1.0 + c.r00 - c.r11 - c.r22) * 2.0;
        q.w = (c.r21 - c.r12) / s;
        q.x = 0.25 * s;
        q.y = (c.r01 + c.r10) / s;
        q.z = (c.r02 + c.r20) / s;
    } else if (c.r11 > c.r22) {
        const double s = std::sqrt(1.0 + c.r11 - c.r00 - c.r22) * 2.0;
        q.w = (c.r02 - c.r20) / s;
        q.x = (c.r01 + c.r10) / s;
        q.y = 0.25 * s;
        q.z = (c.r12 + c.r21) / s;
    } else {
        const double s = std::sqrt(1.0 + c.r22 - c.r00 - c.r11) * 2.0;
        q.w = (c.r10 - c.r01) / s;
        q.x = (c.r02 + c.r20) / s;
        q.y = (c.r12 + c.r21) / s;
        q.z = 0.25 * s;
    }
    return q;
}

CFrame cframe_from_quat(const Quat& q, double x, double y, double z) {
    CFrame c;
    c.x = x;
    c.y = y;
    c.z = z;
    const double ww = q.w * q.w, xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    c.r00 = ww + xx - yy - zz;
    c.r01 = 2.0 * (xy - wz);
    c.r02 = 2.0 * (xz + wy);
    c.r10 = 2.0 * (xy + wz);
    c.r11 = ww - xx + yy - zz;
    c.r12 = 2.0 * (yz - wx);
    c.r20 = 2.0 * (xz - wy);
    c.r21 = 2.0 * (yz + wx);
    c.r22 = ww - xx - yy + zz;
    return c;
}

Quat quat_slerp(const Quat& a, Quat b, double u) {
    double dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0) { // shortest arc
        b.w = -b.w;
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        dot = -dot;
    }
    if (dot > 0.9995) { // nearly parallel: normalized lerp
        Quat r;
        r.w = a.w + (b.w - a.w) * u;
        r.x = a.x + (b.x - a.x) * u;
        r.y = a.y + (b.y - a.y) * u;
        r.z = a.z + (b.z - a.z) * u;
        const double n = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y +
                                   r.z * r.z);
        if (n > 0.0) {
            r.w /= n;
            r.x /= n;
            r.y /= n;
            r.z /= n;
        }
        return r;
    }
    const double theta = std::acos(std::clamp(dot, -1.0, 1.0));
    const double sin_theta = std::sin(theta);
    const double s0 = std::sin((1.0 - u) * theta) / sin_theta;
    const double s1 = std::sin(u * theta) / sin_theta;
    Quat r;
    r.w = a.w * s0 + b.w * s1;
    r.x = a.x * s0 + b.x * s1;
    r.y = a.y * s0 + b.y * s1;
    r.z = a.z * s0 + b.z * s1;
    return r;
}

bool lerp_variant(const Variant& a, const Variant& b, double u, Variant& out) {
    if (const auto* x = std::get_if<double>(&a)) {
        const auto* y = std::get_if<double>(&b);
        if (!y)
            return false;
        out = lerp(*x, *y, u);
        return true;
    }
    if (const auto* x = std::get_if<int64_t>(&a)) {
        const auto* y = std::get_if<int64_t>(&b);
        if (!y)
            return false;
        out = int64_t(std::llround(double(*x) + (double(*y) - double(*x)) * u));
        return true;
    }
    if (const auto* x = std::get_if<Vector3>(&a)) {
        const auto* y = std::get_if<Vector3>(&b);
        if (!y)
            return false;
        out = Vector3{lerp(x->x, y->x, u), lerp(x->y, y->y, u),
                      lerp(x->z, y->z, u)};
        return true;
    }
    if (const auto* x = std::get_if<Vector2>(&a)) {
        const auto* y = std::get_if<Vector2>(&b);
        if (!y)
            return false;
        out = Vector2{lerp(x->x, y->x, u), lerp(x->y, y->y, u)};
        return true;
    }
    if (const auto* x = std::get_if<Color3>(&a)) {
        const auto* y = std::get_if<Color3>(&b);
        if (!y)
            return false;
        out = Color3{lerp(x->r, y->r, u), lerp(x->g, y->g, u),
                     lerp(x->b, y->b, u)};
        return true;
    }
    if (const auto* x = std::get_if<UDim>(&a)) {
        const auto* y = std::get_if<UDim>(&b);
        if (!y)
            return false;
        out = UDim{lerp(x->scale, y->scale, u), lerp(x->offset, y->offset, u)};
        return true;
    }
    if (const auto* x = std::get_if<UDim2>(&a)) {
        const auto* y = std::get_if<UDim2>(&b);
        if (!y)
            return false;
        out = UDim2{UDim{lerp(x->x.scale, y->x.scale, u),
                         lerp(x->x.offset, y->x.offset, u)},
                    UDim{lerp(x->y.scale, y->y.scale, u),
                         lerp(x->y.offset, y->y.offset, u)}};
        return true;
    }
    if (const auto* x = std::get_if<Rect>(&a)) {
        const auto* y = std::get_if<Rect>(&b);
        if (!y)
            return false;
        out = Rect{Vector2{lerp(x->min.x, y->min.x, u),
                           lerp(x->min.y, y->min.y, u)},
                   Vector2{lerp(x->max.x, y->max.x, u),
                           lerp(x->max.y, y->max.y, u)}};
        return true;
    }
    if (const auto* x = std::get_if<CFrame>(&a)) {
        const auto* y = std::get_if<CFrame>(&b);
        if (!y)
            return false;
        const Quat q = quat_slerp(quat_from_cframe(*x), quat_from_cframe(*y), u);
        out = cframe_from_quat(q, lerp(x->x, y->x, u), lerp(x->y, y->y, u),
                               lerp(x->z, y->z, u));
        return true;
    }
    return false; // bool/string/Instance/sequences are not tweenable
}

void apply_goal(lua_State* L, TweenRun& r, TweenGoal& g, double u) {
    const bool backward = r.info.reverses && (r.pass % 2 == 1);
    const Variant& from = backward ? g.target : g.start;
    const Variant& to = backward ? g.start : g.target;
    Variant v;
    if (!lerp_variant(from, to, u, v))
        return;
    std::string err;
    if (!r.target->set_prop(g.prop, v, &err))
        return;
    r.target->changed_signal(g.prop)->fire(L);
}

void finish_run(lua_State* L, TweenRun& r, bool completed) {
    r.playing = false;
    set_playback_state(L, r, completed ? 4 : 5); // Completed / Cancelled
    if (completed && find_event(r.tween->class_info(), "Completed"))
        r.tween->event_signal("Completed")->fire(L);
}

} // namespace

bool tween_gui_properties(
    lua_State* L, Instance* target,
    const std::vector<std::pair<std::string, Variant>>& goals,
    const TweenInfo& info, bool override, std::string* why) {
    if (!target) {
        if (why) *why = "no target";
        return false;
    }
    TweenList* list = tweens(L, true);
    // A legacy GUI tween owns its properties exclusively unless override is
    // true. Check every goal before starting any of them so TweenSizeAndPosition
    // cannot half-start when only one property is already owned.
    for (const auto& g : goals) {
        for (size_t i = 0; i < list->size();) {
            TweenRun& other = (*list)[i];
            bool owns = other.playing && other.target.get() == target;
            if (owns) {
                owns = false;
                for (const TweenGoal& og : other.goals)
                    if (og.prop == g.first) owns = true;
            }
            if (!owns) {
                ++i;
                continue;
            }
            if (!override) {
                if (why)
                    *why = "property '" + g.first +
                           "' is already being tweened";
                return false;
            }
            set_playback_state(L, other, 5); // Cancelled
            list->erase(list->begin() + std::ptrdiff_t(i));
        }
    }

    TweenRun run;
    run.target = Ref<Instance>(target);
    run.info = info;
    for (const auto& g : goals) {
        const PropInfo* pi = find_property(target->class_info(), g.first.c_str());
        if (!pi || pi->readonly) {
            if (why) *why = "property '" + g.first + "' cannot be tweened";
            return false;
        }
        TweenGoal goal;
        goal.prop = g.first;
        goal.target = g.second;
        if (!target->get_prop(goal.prop, goal.start)) {
            if (why) *why = "current value of '" + g.first + "' is unavailable";
            return false;
        }
        Variant probe;
        if (!lerp_variant(goal.target, goal.target, 0.0, probe)) {
            if (why) *why = "property '" + g.first + "' has an untweenable type";
            return false;
        }
        run.goals.push_back(std::move(goal));
    }
    const ClassInfo* cls = find_class("Tween");
    if (!cls) {
        if (why) *why = "Tween class is not registered";
        return false;
    }
    run.tween = instantiate(cls);
    if (!run.tween) {
        if (why) *why = "failed to allocate a Tween";
        return false;
    }
    run.passes = 1;
    run.delay_left = 0.0;
    run.playing = true;
    set_playback_state(L, run, 2); // Playing
    if (info.time <= 0.0) {
        for (TweenGoal& goal : run.goals)
            apply_goal(L, run, goal, 1.0);
        run.pass = run.passes;
        finish_run(L, run, true);
        return true;
    }
    list->push_back(std::move(run));
    if (list->size() > 512) {
        for (size_t i = 0; i < list->size() && list->size() > 384;) {
            if (!(*list)[i].playing && (*list)[i].pass >= (*list)[i].passes)
                list->erase(list->begin() + std::ptrdiff_t(i));
            else
                ++i;
        }
    }
    return true;
}

int m_tween_service_create(lua_State* L) {
    Instance::check(L, 1); // the service
    Instance* target = Instance::check(L, 2);
    const TweenInfo info = check_tweeninfo(L, 3); // raises on a bad type
    luaL_checktype(L, 4, LUA_TTABLE);

    std::vector<TweenGoal> goals;
    lua_pushnil(L);
    while (lua_next(L, 4)) { // [table, key, value]
        if (!lua_isstring(L, -2)) {
            lua_pop(L, 2);
            luaL_error(L, "TweenService:Create goal keys must be property names");
            return 0;
        }
        const std::string prop = lua_tostring(L, -2);
        const PropInfo* pi = find_property(target->class_info(), prop.c_str());
        if (!pi) {
            lua_pop(L, 2);
            luaL_error(L, "TweenService:Create no property named '%s' in class '%s'",
                       prop.c_str(), target->class_name().c_str());
            return 0;
        }
        if (pi->readonly) {
            lua_pop(L, 2);
            luaL_error(L, "TweenService:Create property '%s' is read only",
                       prop.c_str());
            return 0;
        }
        TweenGoal g;
        g.prop = prop;
        g.target = check_variant(L, -1, pi->type, pi->enum_type);
        Variant probe;
        if (!lerp_variant(g.target, g.target, 0.0, probe)) {
            lua_pop(L, 2);
            luaL_error(L,
                       "TweenService:Create property '%s' has a type that "
                       "cannot be tweened",
                       prop.c_str());
            return 0;
        }
        goals.push_back(std::move(g));
        lua_pop(L, 1);
    }
    if (goals.empty()) {
        luaL_error(L, "TweenService:Create requires at least one goal property");
        return 0;
    }

    const ClassInfo* cls = find_class("Tween");
    if (!cls) {
        luaL_error(L, "TweenService:Create Tween class is not registered");
        return 0;
    }
    Ref<Instance> tw = instantiate(cls);
    if (!tw) {
        luaL_error(L, "TweenService:Create failed to allocate a Tween");
        return 0;
    }
    TweenRun run;
    run.target = Ref<Instance>(target);
    run.tween = tw;
    run.info = info;
    run.goals = std::move(goals);
    run.passes = 1 + std::max(0, info.repeat_count);
    run.delay_left = std::max(0.0, info.delay_time);
    set_playback_state(L, run, 0); // Begin
    TweenList* list = tweens(L, true);
    list->push_back(std::move(run));
    // Nothing tells the runtime when a script drops its Tween, so completed
    // one-shot runs are pruned once the table grows; keeping the newest keeps
    // re-Play working for live handles.
    if (list->size() > 512) {
        for (size_t i = 0; i < list->size() && list->size() > 384;) {
            if (!(*list)[i].playing && (*list)[i].pass >= (*list)[i].passes)
                list->erase(list->begin() + std::ptrdiff_t(i));
            else
                ++i;
        }
    }
    Instance::push(L, tw.get());
    return 1;
}

int m_tween_play(lua_State* L) {
    Instance* tw = Instance::check(L, 1);
    TweenRun* r = find_run(L, tw);
    if (!r)
        return 0; // Instance.new("Tween") has no goals: Play is a no-op
    // Roblox re-reads the current values on every Play.
    for (TweenGoal& g : r->goals)
        if (!r->target->get_prop(g.prop, g.start)) {
            r->playing = false;
            return 0;
        }
    r->elapsed = 0.0;
    r->pass = 0;
    r->delay_left = std::max(0.0, r->info.delay_time);
    r->playing = true;
    // A zero-length tween that is not delayed still completes on this frame.
    if (r->delay_left <= 0.0 && r->info.time <= 0.0) {
        for (TweenGoal& g : r->goals)
            apply_goal(L, *r, g, 1.0);
        r->pass = r->passes;
        finish_run(L, *r, true);
        return 0;
    }
    set_playback_state(L, *r, r->delay_left > 0.0 ? 1 : 2); // Delayed/Playing
    return 0;
}

int m_tween_pause(lua_State* L) {
    Instance* tw = Instance::check(L, 1);
    if (TweenRun* r = find_run(L, tw); r && r->playing) {
        r->playing = false;
        set_playback_state(L, *r, 3); // Paused
    }
    return 0;
}

int m_tween_cancel(lua_State* L) {
    Instance* tw = Instance::check(L, 1);
    TweenList* v = tweens(L, false);
    if (!v)
        return 0;
    for (size_t i = 0; i < v->size(); ++i) {
        if ((*v)[i].tween.get() != tw)
            continue;
        set_playback_state(L, (*v)[i], 5); // Cancelled
        v->erase(v->begin() + std::ptrdiff_t(i));
        break;
    }
    return 0;
}

void tween_step(lua_State* L, double dt) {
    TweenList* v = tweens(L, false);
    if (!v)
        return;
    for (size_t i = 0; i < v->size();) {
        TweenRun& r = (*v)[i];
        if (!r.playing) {
            ++i;
            continue;
        }
        if (r.delay_left > 0.0) {
            r.delay_left -= dt;
            if (r.delay_left > 0.0) {
                ++i;
                continue;
            }
            dt = -r.delay_left; // leftover frame time goes into the pass
            r.delay_left = 0.0;
            set_playback_state(L, r, 2); // Playing
        }
        r.elapsed += dt;
        const double dur = r.info.time;
        const double u = dur > 0.0 ? std::min(1.0, r.elapsed / dur) : 1.0;
        const double eased = apply_easing(
            u, r.info.easing_style.value, r.info.easing_direction.value);
        for (TweenGoal& g : r.goals)
            apply_goal(L, r, g, eased);
        if (u >= 1.0) {
            ++r.pass;
            if (r.pass >= r.passes) {
                finish_run(L, r, true);
                // Completed tweens keep their state so re-Play works.
                r.elapsed = 0.0;
                ++i;
                continue;
            }
            // Next pass starts from the beginning of this frame's leftover
            // time (0 unless the frame overran the duration).
            r.elapsed = 0.0;
        }
        ++i;
    }
}

void delete_tweens(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kTweenKey);
    auto* v = static_cast<TweenList*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!v)
        return;
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kTweenKey);
    delete v;
}

} // namespace rbx
