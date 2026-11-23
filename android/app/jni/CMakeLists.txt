cmake_minimum_required(VERSION 3.18)
project(carma2 C)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../deps/SDL2-2.30.9 SDL2)

# lifted game code: each module's functions share a funcs.h in its own directory
foreach(MOD exe bdd)
    file(GLOB SRC_${MOD} ${ROOT}/gen/${MOD}/*.c)
    add_library(gen_${MOD} OBJECT ${SRC_${MOD}})
    target_include_directories(gen_${MOD} PRIVATE ${ROOT}/runtime ${ROOT}/gen/${MOD})
    target_compile_options(gen_${MOD} PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
        -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
        -Wno-parentheses-equality -Wno-tautological-compare -Wno-constant-logical-operand -Wno-unused-value)
endforeach()

file(GLOB WIN32_SRC ${ROOT}/win32/*.c)
set(RT_SRC
    ${ROOT}/runtime/rt.c ${ROOT}/runtime/heap.c ${WIN32_SRC}
    ${ROOT}/platform/plat_sdl.c ${ROOT}/platform/settings.c ${ROOT}/platform/touch.c ${ROOT}/platform/audio.c
    ${ROOT}/platform/main_android.c)

add_library(main SHARED ${RT_SRC} $<TARGET_OBJECTS:gen_exe> $<TARGET_OBJECTS:gen_bdd>)
target_include_directories(main PRIVATE ${ROOT}/runtime)
target_compile_options(main PRIVATE -O2 -fno-strict-aliasing -fwrapv -fsigned-char
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function
    -Wno-parentheses-equality -Wno-tautological-compare -Wno-constant-logical-operand)
target_link_libraries(main SDL2 GLESv3 log android m z)
