# fairyfly_check_calendar_version(<version>): stops configuring unless <version> is a calendar version YYYY.MM.DD, or
# YYYY.MM.DD.N (N = 1..65535) for a further release on the same day, as the updater (src/update/update_model.cpp,
# parse_calver) accepts it.
# Script mode, for the check in packaging/tests/Test-VersionScripts.ps1:
#   cmake -DVERSION=2026.10.24.1 -P cmake/calendar_version.cmake
function(fairyfly_check_calendar_version version)
    if(NOT version MATCHES "^([0-9][0-9][0-9][0-9])\\.([0-9]+)\\.([0-9]+)(\\.([1-9][0-9]*))?$")
        message(FATAL_ERROR "project VERSION ${version} is not a calendar version YYYY.MM.DD or YYYY.MM.DD.N")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    fairyfly_check_calendar_version("${VERSION}")
    message(STATUS "calendar version ${VERSION} OK")
endif()
