cmake_minimum_required(VERSION 3.22)
project(preloader LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR
            "preloader is an Android runtime target. "
            "Use it only from an Android toolchain build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PRELOADER_SOURCES
        src/pl/Mod.cpp
        src/pl/PreLoader.cpp
        src/pl/internal/LoadedModRegistry.cpp
        src/pl/internal/ModManifest.cpp
        src/pl/internal/ModManager.cpp
        src/pl/internal/NativeModLifecycle.cpp
        src/pl/legacy/LegacyHook.cpp
        src/pl/legacy/LegacyInput.cpp
        src/pl/legacy/LegacyPatch.cpp
        src/pl/legacy/LegacySignature.cpp
        src/pl/memory/ElfBuildId.cpp
        src/pl/memory/Hook.cpp
        src/pl/memory/HookTarget.cpp
        src/pl/memory/ModulePathMatch.cpp
        src/pl/memory/Patch.cpp
        src/pl/memory/Signature.cpp
        src/pl/memory/Vtable.cpp
        src/pl/cosmetics/geometry/BedrockModelParser.cpp
        src/pl/cosmetics/NativeModelMatrix.cpp
        src/pl/cosmetics/NativeCosmeticData.cpp
        src/pl/cosmetics/NativeCosmeticContext.cpp
        src/pl/cosmetics/NativeRenderHook.cpp
        src/pl/cosmetics/animation/AnimationSolver.cpp
        src/pl/cosmetics/animation/AnimationJni.cpp
        src/pl/cosmetics/network/CosmeticSocketProtocol.cpp
        src/pl/cosmetics/network/CosmeticSocketClient.cpp
        src/pl/cosmetics/network/CosmeticSyncJni.cpp
        src/pl/render/CameraSource.cpp
        src/pl/render/GlesProbe.cpp
        src/pl/render/RenderApi.cpp
        src/pl/hooks/DobbyHookManager.cpp
        src/pl/runtime/GameHookRules.cpp
        src/pl/runtime/GameHooks.cpp
        src/pl/runtime/GameCosmetics.cpp
        src/pl/runtime/GameCosmeticsJni.cpp
        src/pl/runtime/RegisterAwareHook.cpp
        src/pl/runtime/mce_image_hook.cpp
        src/pl/runtime/GameLocalPlayer.cpp
        src/pl/runtime/GamePlayerRender.cpp
        src/pl/runtime/ModuleMap.cpp
        src/pl/runtime/GameResourcePackReload.cpp
        src/pl/runtime/OptifineConfig.cpp
        src/pl/runtime/OptifineMode.cpp
        src/pl/runtime/optifine/OptifineAllocator.cpp
        src/pl/runtime/optifine/OptifineDeviceRules.cpp
        src/pl/runtime/optifine/OptifineProcess.cpp
        src/pl/runtime/optifine/OptifineRenderPriority.cpp
        src/pl/runtime/optifine/OptifineCpuAffinity.cpp
        src/pl/runtime/optifine/OptifineRefreshRate.cpp
        src/pl/runtime/optifine/OptifineGovernor.cpp
        src/pl/runtime/optifine/OptifineHookSupport.cpp
        src/pl/runtime/optifine/OptifineRenderDistance.cpp
        src/pl/runtime/optifine/OptifineCallbackTrimming.cpp
        src/pl/runtime/optifine/OptifineOreUiStripping.cpp
        src/pl/runtime/optifine/OptifineEntityCulling.cpp
        src/pl/runtime/optifine/OptifineParticleCulling.cpp
        src/pl/runtime/OptifineJni.cpp
        src/pl/runtime/ExternalModBridgeJni.cpp
        src/pl/runtime/AntEggLoader.cpp
        src/pl/runtime/AntEggJni.cpp
        src/pl/runtime/InputBridge.cpp
        src/pl/runtime/InputJni.cpp
        src/pl/runtime/JavaRuntime.cpp
        src/pl/runtime/ModMenuBridge.cpp
)

add_library(preloader SHARED ${PRELOADER_SOURCES})

target_include_directories(preloader
        PUBLIC
        include
        PRIVATE
        src)
target_compile_definitions(preloader PRIVATE PRELOADER_EXPORT)
# Optimisation is set explicitly rather than left to the toolchain default, because the Gradle CMake
# arguments do not set a build type and a debug-flavoured default would ship an unoptimised library.
target_compile_options(preloader PRIVATE
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:
            -fvisibility=hidden
            -fvisibility-inlines-hidden
            -ffunction-sections
            -fdata-sections>
        # -O2 always: the Gradle CMake arguments set no build type, so an unoptimised default would
        # otherwise ship. A Release configuration gets -O3 on top.
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-O2>
        $<$<AND:$<CONFIG:Release>,$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>>:-O3>)
target_link_options(preloader PRIVATE
        # Dead code and unused data drop out once every function is in its own section.
        $<$<COMPILE_LANG_AND_ID:CXX,Clang,GNU>:-Wl,--gc-sections>
        # 16 KB page alignment, required by newer Android releases and ignored by older ones.
        $<$<PLATFORM_ID:Android>:-Wl,-z,max-page-size=16384>)

include(FetchContent)
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        # v3.11.3, pinned by commit so the build is reproducible (see NOTICE).
        GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
)
FetchContent_Declare(
        boost_pfr
        GIT_REPOSITORY https://github.com/boostorg/pfr.git
        # boost-1.88.0, pinned by commit (see NOTICE).
        GIT_TAG 3fe5ce61eee743c6da097c28bc0b84bdf29f6cc4
)
FetchContent_Declare(
        magic_enum
        GIT_REPOSITORY https://github.com/Neargye/magic_enum.git
        # v0.9.7, pinned by commit (see NOTICE).
        GIT_TAG e046b69a3736d314fad813e159b1c192eaef92cd
)
FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        # 11.2.0, pinned by commit (see NOTICE).
        GIT_TAG 40626af88bd7df9a5fb80be7b25ac85b122d6c21
)
FetchContent_Declare(
        lunasvg
        GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
        # v3.5.0, pinned by commit (see NOTICE).
        GIT_TAG 83c58df8103dc7dca423dfd824992af94d49bed6
        EXCLUDE_FROM_ALL
)
set(FMT_DOC OFF CACHE BOOL "Disable fmt docs" FORCE)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules" FORCE)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "Disable lunasvg examples" FORCE)
set(LUNASVG_DISABLE_LOAD_SYSTEM_FONTS ON CACHE BOOL "Disable lunasvg system font scan" FORCE)

set(_preloader_build_shared_libs ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(nlohmann_json boost_pfr magic_enum fmt lunasvg)
set(BUILD_SHARED_LIBS ${_preloader_build_shared_libs})

find_library(log-lib log REQUIRED)
find_library(android-lib android REQUIRED)
find_library(egl-lib EGL REQUIRED)
find_library(gles-lib GLESv3 REQUIRED)
find_library(z-lib z REQUIRED)

# arm64-v8a is the only ABI the client ships (see the client's `ndk abiFilters`), and the only one
# GlossHook is vendored for here. A 32-bit build is not supported; refusing it is clearer than
# silently producing a library the client would never load.
if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
            "preloader supports arm64-v8a only; got ${CMAKE_ANDROID_ARCH_ABI}")
endif()
set(GLOSSHOOK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/ARM64/libGlossHook.a")

if(NOT EXISTS "${GLOSSHOOK_PATH}")
    message(FATAL_ERROR
            "GlossHook static library not found for ${CMAKE_ANDROID_ARCH_ABI}: "
            "${GLOSSHOOK_PATH}")
endif()

target_link_libraries(preloader
        PUBLIC
        fmt::fmt
        PRIVATE
        ${GLOSSHOOK_PATH}
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${gles-lib}
        ${z-lib}
        nlohmann_json::nlohmann_json
        Boost::pfr
        magic_enum::magic_enum
        lunasvg::lunasvg
)

install(TARGETS preloader
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

install(DIRECTORY include/pl
        DESTINATION include
        FILES_MATCHING PATTERN "*.hpp")
