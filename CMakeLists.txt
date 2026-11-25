# CMakeLists.txt - owns the compiler flags, dependency lookup and source list; the Makefile
# (docs/style.md section 8) shells out to this for everything but lint/format/the
# comment-length check, which have no CMake equivalent (docs/style.md section 11).
cmake_minimum_required(VERSION 3.16)
project(nhpm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# For `make lint`'s clang-tidy step (-p), not a build requirement.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# What `nhpm --version` reports: -DVERSION=... on the command line (the Makefile always
# passes one, computed the same way below; a direct cmake invocation falls back to this
# itself), else the nearest git tag/commit, else plain "0.1" outside a git checkout.
if(NOT VERSION)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE GIT_DESCRIBE_RESULT)
        if(NOT GIT_DESCRIBE_RESULT EQUAL 0)
            set(VERSION "")
        endif()
    endif()
endif()
if(NOT VERSION)
    set(VERSION "1.0.0")
endif()

# Homebrew's ncurses is keg-only; panelw.pc isn't on the default pkg-config path
# (docs/tools-libraries.md section 3), so add it when present.
find_program(BREW_EXECUTABLE brew)
if(BREW_EXECUTABLE)
    execute_process(
        COMMAND ${BREW_EXECUTABLE} --prefix ncurses
        OUTPUT_VARIABLE BREW_NCURSES_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE BREW_PREFIX_RESULT)
    if(BREW_PREFIX_RESULT EQUAL 0 AND BREW_NCURSES_PREFIX)
        set(ENV{PKG_CONFIG_PATH} "${BREW_NCURSES_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

# M1 adds libcurl/libxml-2.0, M7 adds ncursesw/panelw; linking against all of them now
# matches the project's final shape (docs/nhpm-plan.md section 1).
find_package(PkgConfig)
set(NHPM_DEPS_FOUND FALSE)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(NHPM_PKGS QUIET IMPORTED_TARGET ncursesw panelw libcurl libxml-2.0)
    set(NHPM_DEPS_FOUND ${NHPM_PKGS_FOUND})
endif()

add_library(nhpm_deps INTERFACE)
if(NHPM_DEPS_FOUND)
    target_link_libraries(nhpm_deps INTERFACE PkgConfig::NHPM_PKGS)
else()
    message(STATUS "pkg-config couldn't find ncursesw/panelw/libcurl/libxml-2.0; "
                    "falling back to -lncursesw -lpanelw -lcurl -lxml2")
    target_link_libraries(nhpm_deps INTERFACE ncursesw panelw curl xml2)
endif()

# Must compile warning-clean on current GCC/Clang with these flags (docs/style.md 6.1);
# CI adds -Werror, local builds don't. `make debug`'s sanitizer flags come in through
# CMAKE_C_FLAGS/CMAKE_EXE_LINKER_FLAGS instead -- no separate toggle needed here.
set(NHPM_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
    -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings -Wnull-dereference -Wdouble-promotion)

file(GLOB NHPM_LIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c
     ${CMAKE_SOURCE_DIR}/src/ui/*.c)
list(REMOVE_ITEM NHPM_LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/main.c)

add_library(nhpm_core STATIC ${NHPM_LIB_SOURCES})
target_include_directories(nhpm_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(nhpm_core PUBLIC ${NHPM_WARNINGS})
target_compile_definitions(nhpm_core PUBLIC NHPM_VERSION="${VERSION}")
target_link_libraries(nhpm_core PUBLIC nhpm_deps)

add_executable(nhpm src/main.c)
target_link_libraries(nhpm PRIVATE nhpm_core)
# `make`/README promise a `./nhpm` at the repo root, same as the old direct-compile recipe.
set_target_properties(nhpm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

enable_testing()
# One target so `make test` can build just the tests, same split as the old $(TEST_BINS).
add_custom_target(nhpm_tests)
file(GLOB NHPM_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.c)
foreach(test_src ${NHPM_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE nhpm_core)
    add_dependencies(nhpm_tests ${test_name})
    # Fixture paths in tests/*.c (e.g. "tests/fixtures/...") are relative to the repo root.
    add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()

include(GNUInstallDirs)
install(TARGETS nhpm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
