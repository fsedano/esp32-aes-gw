# Derive the firmware version from git state. Sets:
#   VERSION_TAG     "v1.2.3"      base tag (closest ancestor, or v0.0.0)
#   VERSION_COMMIT  "a1b2c3d4"    short commit hash
#   VERSION_STRING  see cmake/version_format.cmake:
#       HEAD exactly at a tag, clean tree  ->  "v0.1.0"
#       otherwise (dev and/or dirty)       ->  "v0.1.1-dev.<count>.g<sha>[.dirty]"
#
# Release builds get the bare tag so upgrade tooling compares against the
# published release version verbatim. Dev/dirty builds bump the patch and
# carry a pre-release token so the gateway (aes-gw2/fwrelease) orders them
# ABOVE the tag they grew from (not parked below the MinFwVersion floor)
# and BELOW the next release.

include(${CMAKE_CURRENT_LIST_DIR}/version_format.cmake)

find_package(Git QUIET)

set(_AT_TAG FALSE)
set(_VERSION_TAG "v0.0.0")
set(_COUNT 0)
set(_DIRTY FALSE)
set(GIT_COMMIT "unknown")

if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --exact-match HEAD
        OUTPUT_VARIABLE GIT_DESCRIBE_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE RES_DESCRIBE
    )
    if(RES_DESCRIBE EQUAL 0 AND GIT_DESCRIBE_TAG)
        set(_AT_TAG TRUE)
        set(_VERSION_TAG "${GIT_DESCRIBE_TAG}")
    else()
        # Not exactly at a tag: closest ancestor tag as the base (v0.0.0
        # when the repo has no tags at all).
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 HEAD
            OUTPUT_VARIABLE GIT_CLOSEST_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE RES_CLOSEST
        )
        if(RES_CLOSEST EQUAL 0 AND GIT_CLOSEST_TAG)
            set(_VERSION_TAG "${GIT_CLOSEST_TAG}")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-list ${GIT_CLOSEST_TAG}..HEAD --count
                OUTPUT_VARIABLE _COUNT_OUT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE RES_COUNT
            )
        else()
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-list HEAD --count
                OUTPUT_VARIABLE _COUNT_OUT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE RES_COUNT
            )
        endif()
        if(RES_COUNT EQUAL 0 AND _COUNT_OUT)
            set(_COUNT "${_COUNT_OUT}")
        endif()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
        OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    # Dirty = staged or unstaged changes to tracked files (same test git
    # describe --dirty uses); previously invisible in the version.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
        RESULT_VARIABLE RES_DIRTY
        ERROR_QUIET
    )
    if(NOT RES_DIRTY EQUAL 0)
        set(_DIRTY TRUE)
    endif()
endif()

if(NOT GIT_COMMIT OR GIT_COMMIT STREQUAL "")
    set(GIT_COMMIT "unknown")
endif()

set(VERSION_TAG "${_VERSION_TAG}")
set(VERSION_COMMIT "${GIT_COMMIT}")
lc_version_string("${_AT_TAG}" "${_VERSION_TAG}" "${_COUNT}" "${GIT_COMMIT}" "${_DIRTY}" VERSION_STRING)

message(STATUS "Version: ${VERSION_STRING} (tag=${VERSION_TAG}, commit=${VERSION_COMMIT}, dirty=${_DIRTY})")
