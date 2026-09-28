cmake_minimum_required(VERSION 3.16)
project(AngryBirdsDesktop VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Configure 32-bit multilib target
set(ARCH_FLAGS "-m32")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${ARCH_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARCH_FLAGS} -Wall -Wextra")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${ARCH_FLAGS} -rdynamic -Wl,--export-dynamic -L/usr/lib -L/usr/lib/i386-linux-gnu -L/usr/lib/i686-linux-gnu")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${ARCH_FLAGS} -L/usr/lib -L/usr/lib/i386-linux-gnu -L/usr/lib/i686-linux-gnu")

set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
set(CMAKE_PREFIX_PATH /usr/lib /usr)

# Output binaries directly into bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

# Add $ORIGIN to RPATH so shared libraries in bin/ are found automatically
set(CMAKE_BUILD_RPATH "\$ORIGIN")
set(CMAKE_INSTALL_RPATH "\$ORIGIN")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/android
    ${CMAKE_SOURCE_DIR}/src/jni
    ${CMAKE_SOURCE_DIR}/src/fusion
    ${CMAKE_SOURCE_DIR}/src/loader
    ${CMAKE_SOURCE_DIR}/src/common
    ${CMAKE_SOURCE_DIR}/src/patches
    ${CMAKE_SOURCE_DIR}/src/audio
    ${CMAKE_SOURCE_DIR}/funchook-1.1.3/include
)

set(FUNCHOOK_CPU "x86" CACHE STRING "" FORCE)
set(FUNCHOOK_DISASM "distorm" CACHE STRING "" FORCE)
set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(funchook-1.1.3)

find_library(SDL3_LIB NAMES SDL3 PATHS /usr/lib /usr/lib/i386-linux-gnu /usr/lib/i686-linux-gnu NO_DEFAULT_PATH)
find_library(GL_LIB NAMES GL OpenGL PATHS /usr/lib /usr/lib/i386-linux-gnu /usr/lib/i686-linux-gnu NO_DEFAULT_PATH)

# Build Android shims
add_library(log SHARED src/android/log.c)
add_library(android SHARED src/android/asset_manager.c src/android/bionic_shims.c)

set(SOURCES
    src/main.cpp
    src/loader/so_util.c
    src/fusion/fusion.c
    src/jni/jni_fake.c
    src/common/game_config.c
    src/patches/patch_bytecode.c
    src/patches/sprite_patch.c
    src/patches/powerup_patch.c
    src/patches/aes.c
    src/android/bionic_shims.c
    src/android/asset_manager.c
    src/android/log.c
    src/audio/audio.cpp
)

add_executable(angrybirds_desktop ${SOURCES})
add_dependencies(angrybirds_desktop log android)

set_target_properties(angrybirds_desktop PROPERTIES
    BUILD_RPATH "$ORIGIN:$ORIGIN/../bin:/usr/lib:/lib"
    INSTALL_RPATH "$ORIGIN:$ORIGIN/../bin:/usr/lib:/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

target_link_libraries(angrybirds_desktop PRIVATE
    android
    log
    funchook-static
)

if (SDL3_LIB)
    target_link_libraries(angrybirds_desktop PRIVATE ${SDL3_LIB})
else()
    target_link_libraries(angrybirds_desktop PRIVATE SDL3)
endif()

if (GL_LIB)
    target_link_libraries(angrybirds_desktop PRIVATE ${GL_LIB})
else()
    target_link_libraries(angrybirds_desktop PRIVATE GL)
endif()

target_link_libraries(angrybirds_desktop PRIVATE
    dl
    pthread
    m
)

# ---------------------------------------------------------------------------
# angryrift — Angry Birds Classic encrypted-lua decoder ("Angry Rift").
# ---------------------------------------------------------------------------
if (NOT ARCH_FLAGS MATCHES "-m32")
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(LZMA QUIET liblzma)
    endif()
    if (LZMA_FOUND)
        add_library(angryrift STATIC src/angryrift/angryrift.cpp)
        target_include_directories(angryrift PUBLIC ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(angryrift PUBLIC ${LZMA_LIBRARIES})
        target_include_directories(angryrift PUBLIC ${LZMA_INCLUDE_DIRS})
        add_executable(angryrift_cli src/angryrift/angryrift_cli.cpp)
        target_link_libraries(angryrift_cli PRIVATE angryrift)
        set_target_properties(angryrift_cli PROPERTIES OUTPUT_NAME angryrift)
    endif()
endif()
