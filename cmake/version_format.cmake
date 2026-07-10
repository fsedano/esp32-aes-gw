# lc_version_string — the VERSION_STRING format contract, kept free of any
# git probing so host_tests/tests/test_version_format.cmake can pin it.
#
#   exact tag, clean tree   ->  <tag>                      e.g. v0.1.0
#   anything else           ->  bump the tag's patch and mark it a dev
#                               pre-release:
#                               v<M>.<m>.<p+1>-dev.<count>.g<sha>[.dirty]
#
# Rationale (aes-gw2/fwrelease/version.go ordering): the gateway parses
# "v0.1.0-<anything>" as a PRE-release of 0.1.0, i.e. BELOW the bare tag —
# with the product floor at MinFwVersion v0.1.0 every such build would be
# parked. Bumping the patch makes a dev build order ABOVE the tag it grew
# from and BELOW the next release:
#   v0.1.0  <  v0.1.1-dev.3.g12345678[.dirty]  <  v0.1.1
# A dirty tree is never allowed to masquerade as a release: even exactly at
# a tag it gets the dev form (count 0) plus the .dirty marker.
#
# at_tag / dirty are booleans, tag is the base tag ("v1.2.3", or "v0.0.0"
# when the repo has no tags), count is commits since the tag, sha is the
# short commit hash.
function(lc_version_string at_tag tag count sha dirty out_var)
    if(at_tag AND NOT dirty)
        set(${out_var} "${tag}" PARENT_SCOPE)
        return()
    endif()
    if(tag MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        math(EXPR _patch "${CMAKE_MATCH_3} + 1")
        set(_base "v${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${_patch}")
    else()
        set(_base "v0.0.1")
    endif()
    set(_v "${_base}-dev.${count}.g${sha}")
    if(dirty)
        string(APPEND _v ".dirty")
    endif()
    set(${out_var} "${_v}" PARENT_SCOPE)
endfunction()
