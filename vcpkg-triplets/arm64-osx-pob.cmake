# PoB-specific overlay triplet for arm64-osx.
#
# Identical to the stock arm64-osx triplet (static library linkage), except
# the `angle` port is forced to build as a shared library so that GLFW's
# EGL backend can dlopen libEGL.dylib / libGLESv2.dylib at runtime. GLFW's
# EGL_CONTEXT_API path looks for these by leaf name on the dyld search path
# and cannot find symbols inside a statically-linked-into-our-dylib ANGLE.

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)

if(PORT STREQUAL "angle")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
