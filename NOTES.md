# Notes for upstream PRs

Things discovered during the macOS port that should be sent back upstream once stabilized.

## luajit portfile: hardcoded `x64-linux-rel`/`x64-linux-dbg` build subdir

**File:** `vcpkg-ports/ports/luajit/2025-07-24_1/portfile.cmake` (lines ~104–119)

**Bug:** PR #98's symlink fix (the `foreach(BUILDTYPE ...)` block that replaces the broken circular `bin/luajit -> luajit` symlink with the real binary from the build tree) hardcodes the build directory name to `x64-linux-rel` and `x64-linux-dbg`. On any other triplet (e.g. `arm64-osx`) the `EXISTS "${REAL_BIN}"` check is false, the `file(COPY)` never runs, and `vcpkg_copy_tools(TOOL_NAMES luajit ...)` then fails because `bin/luajit` is still the broken symlink the install step left behind:

```
CMake Error at scripts/cmake/vcpkg_copy_tools.cmake:32 (message):
  Couldn't find tool "luajit":
      neither ".../bin/luajit" nor ".../bin/luajit.app" exists
```

**Fix:** use the `TARGET_TRIPLET` variable that vcpkg already provides in port context:

```cmake
set(BUILD_SUBDIR "${TARGET_TRIPLET}-rel")  # was: "x64-linux-rel"
set(BUILD_SUBDIR "${TARGET_TRIPLET}-dbg")  # was: "x64-linux-dbg"
```

This is generic — it fixes Linux too (any non-x64-linux Linux triplet would have hit the same issue) and unlocks macOS/etc.

**Scope:** the fix is small and platform-agnostic. Worth either folding into PR #98 directly or sending as a follow-up PR after #98 lands.

**Port-version:** locally bumped 3 → 4 (in `vcpkg.json`, `versions/l-/luajit.json`, `versions/baseline.json`) to force vcpkg to pick up the change. Upstream PR doesn't need a new port-version bump if it amends PR #98 before merge.

## lcurl `luaL_setfuncs` duplicate symbol — proper fix instead of GNU-ld workaround

**Files:**
- `libs/Lua-cURLv3/src/l52util.c` (submodule — needs to become a `.patch` applied at build time, or upstreamed to Lua-cURLv3, or carried in a fork)
- `CMakeLists.txt` (lcurl section, ~line 305)

**Bug:** PR #98 worked around the `luaL_setfuncs` duplicate-symbol collision (LuaJIT 2.1 exports it via its 5.2 compat layer; Lua-cURLv3's `l52util.c` also defines it under its `LUA_VERSION_NUM < 502` branch because LuaJIT reports `LUA_VERSION_NUM == 501`) by passing `-Wl,--allow-multiple-definition` to lcurl's link. That flag is **GNU ld–only**; ld64 (Apple) rejects it as `unknown options: --allow-multiple-definition` and the link fails outright on macOS. (mold and lld both accept it.)

**Proper fix (platform-agnostic):** patch `l52util.c` so the duplicate `luaL_setfuncs` definition is gated behind `!LUAJIT_VERSION_NUM || LUAJIT_VERSION_NUM < 20100`, with a conditional `#include <luajit.h>` (via `__has_include`) so the macro is in scope. Verified via `nm -gU libluajit-5.1.a` that `luaL_setfuncs` is the *only* symbol that actually collides — `lua_rawgetp`, `lua_rawsetp`, `lua_absindex` are NOT in LuaJIT 2.1 and still need the polyfills, so the patch is narrow.

With the source fix in place, the `target_link_options(lcurl PRIVATE "-Wl,--allow-multiple-definition")` block in CMakeLists.txt becomes unnecessary on **all** platforms and was removed entirely.

**Scope:** the `l52util.c` patch is genuinely upstream-able to Lua-cURLv3 (it's a correctness fix, not platform-specific). The CMakeLists.txt cleanup folds into PR #98. Worth doing in that order so the CMake change has a clean reason ("the upstream submodule no longer needs the workaround").

**Submodule cleanliness:** the patch is currently applied directly in the `libs/Lua-cURLv3` worktree, which leaves the submodule dirty. Before merging to master, convert it to either (a) a `.patch` file applied at configure time, (b) a fork of Lua-cURLv3 we point the submodule at, or (c) get it upstreamed first.

## CMakeLists.txt: `SIMPLEGRAPHIC_PLATFORM_SOURCES` clobbered on Apple

**File:** `CMakeLists.txt` (~lines 79–95)

**Bug:** pre-existing, not introduced by PR #98. The block reads:

```cmake
set (SIMPLEGRAPHIC_PLATFORM_SOURCES)
if (APPLE)
    set (SIMPLEGRAPHIC_PLATFORM_SOURCES "engine/system/win/sys_macos.mm")
endif()

if (WIN32)
    set (SIMPLEGRAPHIC_PLATFORM_SOURCES "engine/system/win/sys_console.cpp" "SimpleGraphic.rc")
else()
    set (SIMPLEGRAPHIC_PLATFORM_SOURCES "engine/system/win/sys_console_unix.cpp")
endif()
```

On macOS the first block sets `sys_macos.mm`, then the second block's `else()` branch fires (because `APPLE && !WIN32`) and **`set()`s it again**, overwriting the earlier value with just `sys_console_unix.cpp`. Net result on macOS: `sys_macos.mm` is silently dropped, `PlatformOpenURL` (its only function) is never compiled, and `libSimpleGraphic.dylib` fails to link. The `if(APPLE)` block was apparently never actually exercised by whoever added it.

**Fix:** change both `set()` calls in the conditional branches to `list(APPEND ...)` so the contributions stack instead of clobber. The leading `set(SIMPLEGRAPHIC_PLATFORM_SOURCES)` already initializes the variable to empty, which is exactly what `list(APPEND)` needs.

**Scope:** small, platform-agnostic correctness fix. Folds naturally into PR #98 (since #98 didn't touch this block but it's part of the same general "make non-Windows builds work" theme), or as a tiny standalone PR.
