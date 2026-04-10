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

## r_font.cpp: text visibility cull uses logical window size, not framebuffer size

**File:** `engine/render/r_font.cpp` (line ~325, `r_font_c::DrawTextLine`)

**Bug:** the per-line "is this even visible?" early-out reads:

```cpp
if (pos[Y] >= renderer->sys->video->vid.size[1] || pos[Y] <= -height) {
    // process color codes only, then return without drawing
}
```

`pos[Y]` is in the same coordinate space as `VirtualScreenWidth/Height()`, which in `apiDpiAware` mode returns `vid.fbSize` (physical framebuffer pixels). But the comparison upper bound is `vid.size[1]` — the **logical** window height. On any HiDPI display where `vid.size[1] != vid.fbSize[1]` (macOS Retina, HiDPI Wayland, fractional Windows DPI scales), this is half (or some fraction) of the framebuffer height. Every text draw whose `pos[Y]` lands in the bottom half of the screen short-circuits to the no-draw path silently. Checkboxes and other geometry drawn through `r_layer_c::Quad` still appear because they use the correct `VirtualScreenWidth()` cull a few lines down (line ~384), so the symptom is "rectangles fine, labels missing in the lower half of the screen / inside any popup positioned there / inside hover tooltips".

**Fix:** use `renderer->VirtualScreenHeight()` for the upper-bound check, matching the coordinate space of `pos[Y]` and the existing X-axis cull at line 384:

```cpp
if (pos[Y] >= renderer->VirtualScreenHeight() || pos[Y] <= -height) {
```

**Scope:** one-line fix, platform-agnostic, fixes a serious-looking visual bug on every HiDPI display (Linux Wayland users with fractional scaling are likely affected too — they just may not have noticed because Linux PoB usage is rare). High upstream value.

## sys_video.cpp: cursor coordinates on macOS are in logical points, not pixels

**File:** `engine/system/win/sys_video.cpp` (`sys_video_c::GetRelativeCursor`)

**Bug:** GLFW's `glfwGetCursorPos` returns logical "screen coordinates" (points) on macOS Cocoa but physical pixels on Win32 (because Win32's `GetCursorPos` is in pixels under per-monitor-DPI-aware-V2, which GLFW enables). The rest of the engine assumes physical pixels — `vid.fbSize` is in pixels, `VirtualScreenWidth/Height()` returns pixels in `apiDpiAware` mode, the Lua `GetCursorPos` binding does `VirtualMap(cursorX) / dpiScale` on the assumption that `cursorX` is already in pixels. On macOS Retina the cursor reports at half (or a fraction) of its visible position, so hit-testing is offset by exactly the DPI scale factor.

**Fix:** normalize at the source — multiply the GLFW cursor position by `vid.dpiScale` on Apple so the rest of the engine sees a consistent coordinate system:

```cpp
glfwGetCursorPos(wnd, &xpos, &ypos);
#ifdef __APPLE__
    xpos *= vid.dpiScale;
    ypos *= vid.dpiScale;
#endif
```

**Scope:** Apple-only #ifdef around a one-line scaling. The proper long-term fix is probably to normalize the engine's internal coordinate space and stop mixing logical/physical anywhere — but this `#ifdef` is the minimal change that unblocks macOS without touching Windows. Worth proposing alongside the r_font.cpp fix as part of a "HiDPI cleanup" mini-series.

## launcher (Linux + macOS): `dirname()` may return static storage, second call clobbers first

**Files:** `linux/launcher.c`, `macos/launcher.c`

**Bug:** POSIX `dirname(3)` is allowed to return either a pointer to a modified version of the input or a pointer to a **static buffer that subsequent `dirname()` calls will overwrite**. macOS libc takes the static-buffer path. The original `linux/launcher.c` from PR #98 calls `dirname()` twice (once on the launcher exe path, once on the script path) and stores both return values as `char*` aliases — on macOS, the second call silently rewrites the first call's value, so `setenv("SG_BASE_PATH", dir, 1)` ends up writing the script directory instead of the launcher directory. The downstream effect is that `FindBasePath()` returns the wrong directory and the engine can't find its data files (fonts, etc.).

The Linux launcher dodges this only because glibc's `dirname()` happens to modify in place and never returns static storage. POSIX permits both behaviors; relying on glibc's specific implementation is technically a latent bug there too.

**Fix:** copy each `dirname()` return into an owned buffer immediately:

```c
char dir[PATH_MAX];
strncpy(dir, dirname(exeDirSrc), sizeof(dir));
dir[sizeof(dir) - 1] = '\0';
```

**Scope:** the macOS launcher (`macos/launcher.c`) we're adding will have this baked in from the start. The Linux launcher in PR #98 should also be patched to use the same pattern — defends against any future libc change and matches POSIX intent.

## CMakeLists: `$<TARGET_RUNTIME_DLLS:>` is effectively Windows-only on macOS

**File:** `CMakeLists.txt` (install rules around `SimpleGraphic`)

**Bug:** the install rule uses `install(FILES $<TARGET_RUNTIME_DLLS:SimpleGraphic> DESTINATION ".")` to copy all transitively-linked shared libraries next to the launcher. On Windows this captures every `.dll` from imported targets in the link closure. On macOS the generator expression evaluates to **empty** despite imported targets being correctly defined as `SHARED IMPORTED` with valid `IMPORTED_LOCATION_RELEASE` paths. The result: when a vcpkg dep ships a `.dylib` (e.g. our `angle` overlay built dynamically), the install step silently leaves it behind.

**Fix:** for Apple, supplement with `install(IMPORTED_RUNTIME_ARTIFACTS ...)` listing each imported target whose runtime side needs to ship. CMake 3.21+:

```cmake
if (APPLE)
    install(IMPORTED_RUNTIME_ARTIFACTS
        unofficial::angle::libEGL
        unofficial::angle::libGLESv2
        LIBRARY DESTINATION "."
        RUNTIME DESTINATION "."
    )
endif()
```

**Scope:** affects any non-Windows build that links a shared imported target from vcpkg. On Linux, PR #98 sidesteps this by linking everything statically (`VCPKG_LIBRARY_LINKAGE static` is the default for `arm64-osx`/`x64-linux`). The macOS port hits it because we *have* to build ANGLE shared (GLFW's EGL backend `dlopen`s `libEGL.dylib` by leaf name and won't find symbols statically linked into our own dylib). Worth noting in PR #98 as a known limitation, or fixing if anyone wants shared-linkage Linux builds.

## ANGLE port (overlay): `liblibEGL.dylib` from double `lib` prefix when built shared on macOS

**File:** `vcpkg-ports/ports/angle/cmake-buildsystem/CMakeLists.txt` (around the `OUTPUT_NAME` set on the `EGL` and `GLESv2` targets)

**Bug:** the overlay angle CMakeLists already has a Windows path that sets `OUTPUT_NAME libGLESv2` / `libEGL` (full leading `lib`). For non-Windows it has a single fallback that sets `OUTPUT_NAME libGLESv2_angle` / `libEGL_angle` to avoid colliding with system OpenGL on Linux. CMake on macOS adds an automatic `lib` prefix to shared libraries (unlike Windows DLLs), so when angle is built as a `.dylib`, the auto prefix stacks with the leading `lib` in OUTPUT_NAME and you get `liblibEGL_angle.dylib`, `liblibGLESv2_angle.dylib`. Plus, on macOS there's no system `libEGL.dylib` to collide with anyway, so the `_angle` suffix is both unnecessary *and* breaks GLFW which `dlopen`s `libEGL.dylib` by its bare leaf name.

**Fix:** branch on `APPLE` in addition to `VCPKG_TARGET_IS_WINDOWS`. On Apple, set `OUTPUT_NAME libGLESv2/libEGL` AND `PREFIX ""` so the final filename is `libGLESv2.dylib` / `libEGL.dylib`:

```cmake
if(APPLE)
    set_target_properties(GLESv2 PROPERTIES OUTPUT_NAME libGLESv2 PREFIX "")
elseif(NOT VCPKG_TARGET_IS_WINDOWS)
    set_target_properties(GLESv2 PROPERTIES OUTPUT_NAME libGLESv2_angle)
endif()
```

(Same for `EGL`.)

Two related portfile/forwarding bugs: the overlay portfile only forwards `-DVCPKG_TARGET_IS_WINDOWS=...` to the angle build, not `VCPKG_TARGET_IS_OSX`. So inside the angle CMakeLists, conditioning on `VCPKG_TARGET_IS_OSX` silently does nothing — use CMake's native `APPLE` variable instead.

**Scope:** only matters when building ANGLE shared on macOS, which is forced by our overlay triplet. Worth folding into the angle port if we ever upstream the overlay, otherwise stays as a permanent macOS-port-local patch.

## vcpkg overlay triplet for per-port linkage override

**Files:** `vcpkg-triplets/arm64-osx-pob.cmake`, `CMakeLists.txt` (Apple branch before vcpkg.cmake `include()`)

**Not a bug, but an architectural note worth recording.** The default `arm64-osx` triplet uses `VCPKG_LIBRARY_LINKAGE static`, which is the right global default — but ANGLE specifically must be built shared on macOS (so GLFW's EGL backend can `dlopen` it at runtime). vcpkg supports per-port linkage overrides via the `if(PORT STREQUAL "...")` block inside a triplet file, but the stock `arm64-osx` triplet lives in the vcpkg submodule and we shouldn't edit it. The clean solution is a project-local overlay triplet at `vcpkg-triplets/arm64-osx-pob.cmake` with a per-port override, plus `VCPKG_OVERLAY_TRIPLETS` and `VCPKG_TARGET_TRIPLET` set in CMakeLists.txt before the vcpkg.cmake include.

**One subtle gotcha worth documenting**: also set `VCPKG_HOST_TRIPLET` to the same custom triplet name. Otherwise vcpkg's `host_triplet != target_triplet` check considers the build a cross-compile (even though both are native arm64-osx), and the luajit portfile's `if(VCPKG_CROSSCOMPILING)` branch fires and passes `BUILDVM_X=` to luajit's Makefile but doesn't set `HOST_CC` for `host/minilua`. The luajit Makefile then links `host/minilua` with `:` (the noop shell builtin) instead of `cc`, silently produces no binary, and the install fails further down. Setting host==target avoids this.

This isn't a bug to upstream — it's the intended way to use custom triplets — but it's the kind of lore that costs an hour to rediscover, so worth a note.
