cmake_minimum_required(VERSION 3.18)
project(carma2 C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deps/SDL2-2.30.9 SDL2)

set(GEN_FLAGS -O2 -fno-strict-aliasing -fwrapv -fsigned-char -fvisibility=hidden
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
    -Wno-parentheses-equality -Wno-tautological-compare -Wno-constant-logical-operand -Wno-unused-value)

# The recompiled game code (libgame.so) is built from the user's own copy of the game. The release
# template APK has no game code: the APK builder (builder/) compiles libgame.so with Zig and adds it.
# For development it can be built here: ./gradlew -PwithGame=ON ...
option(C2_WITH_GAME "also build libgame.so from gen/ (development)" OFF)
if(C2_WITH_GAME)
    foreach(MOD exe bdd)
        file(GLOB SRC_${MOD} ${ROOT}/gen/${MOD}/*.c)
        add_library(gen_${MOD} OBJECT ${SRC_${MOD}})
        set_property(TARGET gen_${MOD} PROPERTY POSITION_INDEPENDENT_CODE ON)
        target_include_directories(gen_${MOD} PRIVATE ${ROOT}/runtime ${ROOT}/gen/${MOD})
        target_compile_options(gen_${MOD} PRIVATE ${GEN_FLAGS})
    endforeach()
    add_library(game SHARED ${ROOT}/runtime/game_glue.c $<TARGET_OBJECTS:gen_exe> $<TARGET_OBJECTS:gen_bdd>)
    target_include_directories(game PRIVATE ${ROOT}/runtime)
    target_compile_options(game PRIVATE ${GEN_FLAGS})
    target_link_options(game PRIVATE -Wl,-z,max-page-size=16384 -Wl,--no-undefined -Wl,-Bsymbolic)
    target_link_libraries(game m)
endif()

file(GLOB WIN32_SRC ${ROOT}/win32/*.c)
set(RT_SRC
    ${ROOT}/runtime/rt.c ${ROOT}/runtime/heap.c ${WIN32_SRC}
    ${ROOT}/platform/plat_sdl.c ${ROOT}/platform/settings.c ${ROOT}/platform/touch.c ${ROOT}/platform/audio.c
    ${ROOT}/platform/main_android.c)

add_library(main SHARED ${RT_SRC})
target_compile_definitions(main PRIVATE C2_DYNGAME)
target_include_directories(main PRIVATE ${ROOT}/runtime)
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
    -Wno-parentheses-equality -Wno-tautological-compare -Wno-constant-logical-operand)
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384)
target_link_libraries(main SDL2 GLESv3 log android m z dl)
