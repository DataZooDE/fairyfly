# Release or development build (include/build_version.h). A release is a build of the commit its tag
# v<FAIRYFLY_VERSION_STRING> points at; it shows the plain base version. Every other build shows
# <base>-dev+<first 7 hex digits of the commit> (or <base>-dev without git), in `fairyfly --version`, the trays, doctor,
# the log header, the link's pairing/hello version and the exe's ProductVersion string. The version files and all
# numeric versions (FILEVERSION/PRODUCTVERSION, FileVersion string, MSI, MSIX) keep the base version.
#
# FAIRYFLY_BUILD_KIND: auto (default) = release when HEAD carries the tag v<version>, or when GitHub Actions runs on
# refs/tags/v<version> for this very commit (release.yml checks out github.sha, without the tag); release / dev force
# it (release.yml and winget.yml pass release).
set(FAIRYFLY_BUILD_KIND "auto" CACHE STRING "auto (release only from the tag v<version>), release or dev")
set_property(CACHE FAIRYFLY_BUILD_KIND PROPERTY STRINGS auto release dev)
if(NOT FAIRYFLY_BUILD_KIND MATCHES "^(auto|release|dev)$")
    message(FATAL_ERROR "FAIRYFLY_BUILD_KIND is \"${FAIRYFLY_BUILD_KIND}\": auto, release or dev")
endif()

# fairyfly_build_kind(<version> <out_release> <out_commit> <out_display>)
function(fairyfly_build_kind version out_release out_commit out_display)
    find_package(Git QUIET)
    set(commit "")
    set(tags "")
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE commit RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT rc EQUAL 0 OR NOT commit MATCHES "^[0-9a-f]+$")
            set(commit "")
        endif()
    endif()
    if(commit)
        execute_process(COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REPLACE "\n" ";" tags "${tags}")
        # configure again when HEAD moves (a commit, a checkout): the commit is baked into the build
        set(watch HEAD packed-refs)
        execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(ref)
            list(APPEND watch "${ref}")
        endif()
        foreach(p IN LISTS watch)
            execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path "${p}"
                            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE file OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(file AND EXISTS "${file}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
            endif()
        endforeach()
    endif()

    if(FAIRYFLY_BUILD_KIND STREQUAL "release")
        set(release TRUE)
    elseif(FAIRYFLY_BUILD_KIND STREQUAL "dev")
        set(release FALSE)
    else()
        set(release FALSE)
        if("v${version}" IN_LIST tags)
            set(release TRUE)
        elseif("$ENV{GITHUB_REF}" STREQUAL "refs/tags/v${version}" AND commit AND "$ENV{GITHUB_SHA}" STREQUAL commit)
            set(release TRUE)
        endif()
    endif()

    # the same rule as fairyfly::compose_display_version (src/build_version.cpp); build.yml checks both agree
    if(release)
        set(display "${version}")
    else()
        string(LENGTH "${commit}" n)
        if(n GREATER_EQUAL 7)
            string(SUBSTRING "${commit}" 0 7 short)
            set(display "${version}-dev+${short}")
        else()
            set(display "${version}-dev")
        endif()
    endif()
    set(${out_release} ${release} PARENT_SCOPE)
    set(${out_commit} "${commit}" PARENT_SCOPE)
    set(${out_display} "${display}" PARENT_SCOPE)
endfunction()
