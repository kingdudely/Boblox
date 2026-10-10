// datetime.cpp — DateTime value type (see value_types.h).
//
// Docs-verified surface: now/fromUnixTimestamp/fromUnixTimestampMillis/
// fromUniversalTime/fromLocalTime (all args default 1970-01-01T00:00:00.000)/
// fromIsoDate + UnixTimestamp/UnixTimestampMillis + ToUniversalTime/
// ToLocalTime (dictionaries) + ToIsoDate. Format* (locale-dependent) skipped.
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace rbx {

namespace {

// Days-from-civil (Howard Hinnant's algorithm): exact, TZ-independent.
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468; // days since 1970-01-01
}

int64_t utc_millis(int y, int mo, int d, int h, int mi, int s, int ms) {
    return (days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s) * 1000 + ms;
}

void push_ymdhms(lua_State* L, int y, int mo, int d, int h, int mi, int s, int ms) {
    lua_newtable(L);
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "Year");
    lua_pushinteger(L, mo);
    lua_setfield(L, -2, "Month");
    lua_pushinteger(L, d);
    lua_setfield(L, -2, "Day");
    lua_pushinteger(L, h);
    lua_setfield(L, -2, "Hour");
    lua_pushinteger(L, mi);
    lua_setfield(L, -2, "Minute");
    lua_pushinteger(L, s);
    lua_setfield(L, -2, "Second");
    lua_pushinteger(L, ms);
    lua_setfield(L, -2, "Millisecond");
}

int dt_index(lua_State* L) {
    const DateTime* v = static_cast<const DateTime*>(luaL_checkudata(L, 1, "DateTime"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "UnixTimestamp") == 0 || strcmp(k, "unixTimestamp") == 0) {
        const int64_t s = v->millis / 1000 - (v->millis % 1000 < 0 ? 1 : 0);
        lua_pushnumber(L, (double)s);
        return 1;
    }
    if (strcmp(k, "UnixTimestampMillis") == 0 || strcmp(k, "unixTimestampMillis") == 0) {
        lua_pushnumber(L, (double)v->millis);
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

void split_utc(int64_t millis, int& y, int& mo, int& d, int& h, int& mi, int& s,
               int& mss) {
    int64_t days = millis / 86400000;
    int64_t rem = millis % 86400000;
    if (rem < 0) {
        rem += 86400000;
        days -= 1;
    }
    // civil-from-days (inverse of days_from_civil)
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)(yoe + era * 400);
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    mo = (int)(mp + (mp < 10 ? 3 : -9));
    y += mo <= 2 ? 1 : 0;
    h = (int)(rem / 3600000);
    mi = (int)((rem / 60000) % 60);
    s = (int)((rem / 1000) % 60);
    mss = (int)(rem % 1000);
}

int dt_touniversal(lua_State* L) {
    const DateTime v = check_datetime(L, 1);
    int y, mo, d, h, mi, s, mss;
    split_utc(v.millis, y, mo, d, h, mi, s, mss);
    push_ymdhms(L, y, mo, d, h, mi, s, mss);
    return 1;
}

int dt_tolocal(lua_State* L) {
    const DateTime v = check_datetime(L, 1);
    std::time_t t = (std::time_t)(v.millis / 1000);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    push_ymdhms(L, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec, (int)(v.millis % 1000));
    return 1;
}

int dt_toiso(lua_State* L) {
    const DateTime v = check_datetime(L, 1);
    int y, mo, d, h, mi, s, mss;
    split_utc(v.millis, y, mo, d, h, mi, s, mss);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, h,
                  mi, s, mss);
    lua_pushstring(L, buf);
    return 1;
}

int dt_now(lua_State* L) {
    const auto now = std::chrono::system_clock::now();
    DateTime v;
    v.millis = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch())
                   .count();
    push_datetime(L, v);
    return 1;
}

int dt_fromunix(lua_State* L) {
    DateTime v;
    v.millis = (int64_t)(luaL_checknumber(L, 1) * 1000.0);
    push_datetime(L, v);
    return 1;
}

int dt_fromunixms(lua_State* L) {
    DateTime v;
    // lua_Integer is 32-bit: millis since 1970 far exceeds it, so read the
    // (exactly-representable) double instead of luaL_checkinteger.
    v.millis = (int64_t)luaL_checknumber(L, 1);
    push_datetime(L, v);
    return 1;
}

int dt_fromymd(lua_State* L, bool local) {
    const int y = (int)luaL_optnumber(L, 1, 1970.0);
    const int mo = (int)luaL_optnumber(L, 2, 1.0);
    const int d = (int)luaL_optnumber(L, 3, 1.0);
    const int h = (int)luaL_optnumber(L, 4, 0.0);
    const int mi = (int)luaL_optnumber(L, 5, 0.0);
    const int s = (int)luaL_optnumber(L, 6, 0.0);
    const int ms = (int)luaL_optnumber(L, 7, 0.0);
    DateTime v;
    if (local) {
        std::tm tmv{};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = h;
        tmv.tm_min = mi;
        tmv.tm_sec = s;
        tmv.tm_isdst = -1;
        v.millis = (int64_t)std::mktime(&tmv) * 1000 + ms;
    } else {
        v.millis = utc_millis(y, mo, d, h, mi, s, ms);
    }
    push_datetime(L, v);
    return 1;
}

int dt_fromuniversal(lua_State* L) {
    return dt_fromymd(L, false);
}

int dt_fromlocal(lua_State* L) {
    return dt_fromymd(L, true);
}

int dt_fromiso(lua_State* L) {
    size_t len = 0;
    const char* str = luaL_checklstring(L, 1, &len);
    // Strict "YYYY-MM-DDTHH:MM:SS[.mmm]Z" (the ToIsoDate format).
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
    bool bad = len != 20 && len != 24;
    bad = bad || str[4] != '-' || str[7] != '-' || str[10] != 'T' || str[13] != ':' ||
          str[16] != ':';
    bad = bad || (len == 20 ? str[19] != 'Z' : (str[19] != '.' || str[23] != 'Z'));
    int n = 0;
    if (!bad) {
        n = std::sscanf(str, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s);
        bad = n < 6;
    }
    if (!bad && len == 24) {
        n = std::sscanf(str + 21, "%3d", &ms); // digits after the '.'
        bad = n < 1;
    }
    bad = bad || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 ||
          mi > 59 || s < 0 || s > 59 || ms < 0 || ms > 999;
    if (bad) {
        luaL_error(L, "invalid ISO date");
        return 0;
    }
    DateTime v;
    v.millis = utc_millis(y, mo, d, h, mi, s, ms);
    push_datetime(L, v);
    return 1;
}

int dt_eq(lua_State* L) {
    const DateTime a = check_datetime(L, 1);
    const DateTime b = check_datetime(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_datetime(lua_State* L, const DateTime& v) {
    void* p = lua_newuserdata(L, sizeof(DateTime));
    *static_cast<DateTime*>(p) = v;
    if (luaL_newmetatable(L, "DateTime")) { // first time: fill it
        lua_newtable(L);
        lua_pushcfunction(L, dt_touniversal, "ToUniversalTime");
        lua_setfield(L, -2, "ToUniversalTime");
        lua_pushcfunction(L, dt_tolocal, "ToLocalTime");
        lua_setfield(L, -2, "ToLocalTime");
        lua_pushcfunction(L, dt_toiso, "ToIsoDate");
        lua_setfield(L, -2, "ToIsoDate");
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, dt_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, dt_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "DateTime"); // typeof() == "DateTime" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

DateTime check_datetime(lua_State* L, int idx) {
    return *static_cast<const DateTime*>(luaL_checkudata(L, idx, "DateTime"));
}

void create_datetime_class(lua_State* L) {
    push_datetime(L, DateTime{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, dt_now, "now");
    lua_setfield(L, -2, "now");
    lua_pushcfunction(L, dt_fromunix, "fromUnixTimestamp");
    lua_setfield(L, -2, "fromUnixTimestamp");
    lua_pushcfunction(L, dt_fromunixms, "fromUnixTimestampMillis");
    lua_setfield(L, -2, "fromUnixTimestampMillis");
    lua_pushcfunction(L, dt_fromuniversal, "fromUniversalTime");
    lua_setfield(L, -2, "fromUniversalTime");
    lua_pushcfunction(L, dt_fromlocal, "fromLocalTime");
    lua_setfield(L, -2, "fromLocalTime");
    lua_pushcfunction(L, dt_fromiso, "fromIsoDate");
    lua_setfield(L, -2, "fromIsoDate");
    lua_setglobal(L, "DateTime");
}

} // namespace rbx
