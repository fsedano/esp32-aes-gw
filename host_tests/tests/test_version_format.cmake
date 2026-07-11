# Pins the VERSION_STRING format contract (cmake/version_format.cmake).
#
# The gateway (aes-gw2/fwrelease/version.go) orders "v0.1.0-<x>" BELOW
# "v0.1.0" (pre-release semantics) and parks anything under the product's
# MinFwVersion floor (v0.1.0) — so a dev build must bump the patch:
#   v0.1.0  <  v0.1.1-dev.<count>.g<sha>[.dirty]  <  v0.1.1
# Verified against fwrelease.Parse/Compare; keep these cases in sync with
# that ordering.
#
# Run: cmake -P test_version_format.cmake   (wired into ctest)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/version_format.cmake)

set(FAILED 0)

function(expect at_tag tag count sha dirty want)
    lc_version_string("${at_tag}" "${tag}" "${count}" "${sha}" "${dirty}" got)
    if(NOT got STREQUAL want)
        message(SEND_ERROR "lc_version_string(at_tag=${at_tag} tag=${tag} "
            "count=${count} sha=${sha} dirty=${dirty}) = '${got}', want '${want}'")
        set(FAILED 1 PARENT_SCOPE)
    endif()
endfunction()

# Exact tag, clean tree: the bare tag (release build).
expect(TRUE  "v0.1.0" 0  "12345678" FALSE "v0.1.0")
# Past a tag: patch bump + dev pre-release (orders above v0.1.0, below v0.1.1).
expect(FALSE "v0.1.0" 1  "e5e01e46" FALSE "v0.1.1-dev.1.ge5e01e46")
expect(FALSE "v0.1.0" 42 "12345678" FALSE "v0.1.1-dev.42.g12345678")
# Dirty tree is marked, even exactly at a tag (never masquerades as a release).
expect(FALSE "v0.1.0" 42 "12345678" TRUE  "v0.1.1-dev.42.g12345678.dirty")
expect(TRUE  "v0.1.0" 0  "deadbeef" TRUE  "v0.1.1-dev.0.gdeadbeef.dirty")
# No tags in the repo: base v0.0.0, with or without the leading 'v'.
expect(FALSE "v0.0.0" 7  "abcdef12" FALSE "v0.0.1-dev.7.gabcdef12")
expect(FALSE "0.0.0"  7  "abcdef12" FALSE "v0.0.1-dev.7.gabcdef12")
# Unparseable base tag degrades to v0.0.1.
expect(FALSE "garbage" 3 "abcdef12" FALSE "v0.0.1-dev.3.gabcdef12")

if(FAILED)
    message(FATAL_ERROR "version_format: FAILED")
endif()
message(STATUS "version_format: all cases OK")
