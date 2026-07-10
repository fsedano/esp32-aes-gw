# Generate a version header from the latest git tag + commit SHA.
# Writes <OUTPUT_FILE> with:
#   #define VERSION_TAG     "v1.2.3"
#   #define VERSION_COMMIT  "a1b2c3d"
#   #define VERSION_STRING  "v1.2.3"            (HEAD exactly at a tag)
#                       or  "v1.2.3-a1b2c3d"    (untagged build: closest tag)
#
# Release builds (HEAD at a tag) get the bare tag so upgrade tooling can
# compare against the published release version verbatim; untagged builds
# use the closest ancestor tag plus a SHA suffix so they can't be mistaken
# for a release.
#
# If no tags exist, falls back to "0.0.0" as the tag base.

find_package(Git QUIET)

if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --exact-match HEAD
        OUTPUT_VARIABLE GIT_DESCRIBE_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE RES_DESCRIBE
    )
    if(RES_DESCRIBE EQUAL 0 AND GIT_DESCRIBE_TAG)
        set(_VERSION_TAG "${GIT_DESCRIBE_TAG}")
        set(_AT_TAG TRUE)
    else()
        set(_AT_TAG FALSE)
        # Not exactly at a tag: use the closest ancestor tag as the base.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 HEAD
            OUTPUT_VARIABLE GIT_CLOSEST_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE RES_CLOSEST
        )
        if(RES_CLOSEST EQUAL 0 AND GIT_CLOSEST_TAG)
            set(_VERSION_TAG "${GIT_CLOSEST_TAG}")
        else()
            set(_VERSION_TAG "0.0.0")
        endif()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=8 HEAD
        OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(_VERSION_TAG "0.0.0")
    set(_AT_TAG FALSE)
    set(GIT_COMMIT "unknown")
endif()

if(NOT GIT_COMMIT OR GIT_COMMIT STREQUAL "")
    set(GIT_COMMIT "unknown")
endif()

set(VERSION_TAG "${_VERSION_TAG}")
set(VERSION_COMMIT "${GIT_COMMIT}")
if(_AT_TAG)
    set(VERSION_STRING "${_VERSION_TAG}")
else()
    set(VERSION_STRING "${_VERSION_TAG}-${GIT_COMMIT}")
endif()

message(STATUS "Version: ${VERSION_STRING} (tag=${VERSION_TAG}, commit=${VERSION_COMMIT})")
