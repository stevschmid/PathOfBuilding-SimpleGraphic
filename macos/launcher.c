// macOS launcher for SimpleGraphic / Path of Building
// Loads libSimpleGraphic.dylib from the same directory and calls RunLuaFileAsWin.
//
// Mirrors linux/launcher.c, with two macOS-specific differences:
//   1. Self-path resolution uses _NSGetExecutablePath instead of /proc/self/exe.
//      The path returned may contain symlinks or "..", so we realpath() it.
//   2. The engine library is libSimpleGraphic.dylib and Lua C modules are .dylib.

#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*RunLuaFileAsWin_t)(int argc, char** argv);

int main(int argc, char** argv)
{
    // Resolve our own executable's directory.
    // _NSGetExecutablePath gives us a path that may not be canonical
    // (can contain symlinks or relative components), so realpath it.
    char rawExePath[PATH_MAX];
    uint32_t rawExePathSize = sizeof(rawExePath);
    if (_NSGetExecutablePath(rawExePath, &rawExePathSize) != 0) {
        fprintf(stderr, "_NSGetExecutablePath: buffer too small (%u)\n", rawExePathSize);
        return 1;
    }

    char exePath[PATH_MAX];
    if (realpath(rawExePath, exePath) == NULL) {
        perror("realpath(exePath)");
        return 1;
    }

    // POSIX dirname() may return a pointer to static storage that subsequent
    // dirname() calls will overwrite (and macOS does exactly this). Always
    // copy the result into an owned buffer immediately, before any further
    // dirname() call clobbers it.
    char exeDirSrc[PATH_MAX];
    strncpy(exeDirSrc, exePath, sizeof(exeDirSrc));
    char dir[PATH_MAX];
    strncpy(dir, dirname(exeDirSrc), sizeof(dir));
    dir[sizeof(dir) - 1] = '\0';

    // Pre-load ANGLE's GLES + EGL shared libraries from our own directory.
    // GLFW's EGL backend later does dlopen("libEGL.dylib", ...) by leaf name,
    // which dyld will satisfy from the already-loaded image table once we've
    // loaded these by absolute path. (dyld does NOT search LC_RPATH or
    // @loader_path for bare-name dlopen() calls, so this is the cleanest way
    // to make GLFW find ANGLE without polluting DYLD_LIBRARY_PATH.)
    char libGLESPath[PATH_MAX];
    snprintf(libGLESPath, sizeof(libGLESPath), "%s/libGLESv2.dylib", dir);
    if (!dlopen(libGLESPath, RTLD_LAZY | RTLD_GLOBAL)) {
        fprintf(stderr, "Failed to pre-load %s: %s\n", libGLESPath, dlerror());
        return 1;
    }

    char libEGLPath[PATH_MAX];
    snprintf(libEGLPath, sizeof(libEGLPath), "%s/libEGL.dylib", dir);
    if (!dlopen(libEGLPath, RTLD_LAZY | RTLD_GLOBAL)) {
        fprintf(stderr, "Failed to pre-load %s: %s\n", libEGLPath, dlerror());
        return 1;
    }

    // Load libSimpleGraphic.dylib from the same directory.
    char libPath[PATH_MAX];
    snprintf(libPath, sizeof(libPath), "%s/libSimpleGraphic.dylib", dir);

    void* lib = dlopen(libPath, RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "Failed to load %s: %s\n", libPath, dlerror());
        return 1;
    }

    RunLuaFileAsWin_t runLua = (RunLuaFileAsWin_t)dlsym(lib, "RunLuaFileAsWin");
    if (!runLua) {
        fprintf(stderr, "dlsym RunLuaFileAsWin: %s\n", dlerror());
        return 1;
    }

    // argv[0] for RunLuaFileAsWin must be the Lua script path.
    // Skip our own argv[0] (the launcher binary).
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <script.lua> [args...]\n", argv[0]);
        return 1;
    }

    // Resolve script path to absolute so basePath resolution inside SimpleGraphic
    // works correctly. basePath is derived from the launcher's exe path, so a
    // relative script path would otherwise be resolved against the launcher dir
    // rather than the current working directory.
    char scriptAbs[PATH_MAX];
    if (realpath(argv[1], scriptAbs) == NULL) {
        perror(argv[1]);
        return 1;
    }
    argv[1] = scriptAbs;

    // Derive the PoB root from the script location:
    //   script = <pob_root>/src/Launch.lua
    //   script_dir = <pob_root>/src
    //   pob_root = <pob_root>
    // (the runtime Lua modules live at <pob_root>/runtime/lua/)
    char scriptDirBuf[PATH_MAX];
    strncpy(scriptDirBuf, scriptAbs, sizeof(scriptDirBuf));
    char scriptDir[PATH_MAX];
    strncpy(scriptDir, dirname(scriptDirBuf), sizeof(scriptDir));
    scriptDir[sizeof(scriptDir) - 1] = '\0';

    char pobRoot[PATH_MAX];
    snprintf(pobRoot, sizeof(pobRoot), "%s/..", scriptDir);
    char pobRootAbs[PATH_MAX];
    if (!realpath(pobRoot, pobRootAbs))
        strncpy(pobRootAbs, pobRoot, sizeof(pobRootAbs));

    // Tell SimpleGraphic where the runtime data (fonts, etc.) lives.
    // When installed, SimpleGraphic/Fonts/ sits alongside the launcher binary,
    // so the launcher's own directory serves as the base path.
    if (!getenv("SG_BASE_PATH"))
        setenv("SG_BASE_PATH", dir, 1);

    // Set LUA_PATH to include the PoB runtime Lua directory.
    // Work around pob-wide-crt.patch bug: _lua_getenvcopy() calls strdup(getenv(name))
    // which crashes on NULL, so we always set these even if we're not overriding.
    if (!getenv("LUA_PATH")) {
        char luaPath[PATH_MAX * 4];
        snprintf(luaPath, sizeof(luaPath),
            "%s/runtime/lua/?.lua;%s/runtime/lua/?/init.lua;;",
            pobRootAbs, pobRootAbs);
        setenv("LUA_PATH", luaPath, 1);
    }

    // Set LUA_CPATH to include the launcher dir (where our .dylib Lua modules
    // live without lib prefix).
    if (!getenv("LUA_CPATH")) {
        char luaCPath[PATH_MAX * 4];
        snprintf(luaCPath, sizeof(luaCPath), "%s/?.dylib;;", dir);
        setenv("LUA_CPATH", luaCPath, 1);
    }

    return runLua(argc - 1, argv + 1);
}
