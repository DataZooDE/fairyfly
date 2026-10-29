# The MSIX package manifest (FAIRYFLY_PACKAGE=msix, docs/MSIX.md).
#
# fairyfly_msix_version(<out_var> <calendar version>): the package version for a calendar version YYYY.MM.DD[.N]:
# YYYY.MM.(DD*100+N).0, e.g. 2026.10.24 -> 2026.10.2400.0, 2026.10.24.1 -> 2026.10.2401.0. MSIX needs four numeric
# fields (each 0..65535) and the Store requires the fourth to be 0, so N goes into the third field as for the MSI
# (packaging/msi_version.ps1: (YYYY-2000).MM.(DD*100+N)); packaging/msix_version.ps1 prints the same.
#
# fairyfly_render_msix_manifest(<template> <out> <version> <identity name> <publisher> <publisher display name>):
# configures packaging/msix/AppxManifest.xml.in. Every @NAME@ of the template must be one of the values given here and
# non-empty (configure_file would silently put an empty string for any other), the identity name must be what MSIX
# allows ([-.A-Za-z0-9], 3..50 characters) and the publisher a distinguished name (CN=...). The values are XML-escaped.
#
# Script mode, for packaging/msix/tests/Test-AppxManifest.ps1 and packaging/tests/Test-VersionScripts.ps1:
#   cmake -DVERSION=2026.10.24.1 -P cmake/msix_manifest.cmake                  prints "msix version 2026.10.2401.0"
#   cmake -DVERSION=... -DTEMPLATE=<in> -DOUT=<xml> [-DIDENTITY_NAME=... -DPUBLISHER=CN=... -DPUBLISHER_DISPLAY_NAME=...]
#         -P cmake/msix_manifest.cmake                                          renders the manifest (identity values
#                                                                               left out: the defaults below)
include(${CMAKE_CURRENT_LIST_DIR}/calendar_version.cmake)

# fairyfly's product identity in Partner Center (Product management > Product identity; Store ID 9N5TSPSX6Q0V, package
# family name DataZoo.fairyfly_73q587949tsjt). Public, not secret: it is part of every installed package's full name.
# The defaults of the cache variables FAIRYFLY_MSIX_IDENTITY_NAME, FAIRYFLY_MSIX_PUBLISHER and
# FAIRYFLY_MSIX_PUBLISHER_DISPLAY_NAME (CMakeLists.txt), which stay overridable (-D...).
set(FAIRYFLY_MSIX_DEFAULT_IDENTITY_NAME "DataZoo.fairyfly")
set(FAIRYFLY_MSIX_DEFAULT_PUBLISHER "CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC")
set(FAIRYFLY_MSIX_DEFAULT_PUBLISHER_DISPLAY_NAME "DataZoo")

function(fairyfly_msix_version out version)
    fairyfly_check_calendar_version("${version}")
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$" _ "${version}")
    set(y "${CMAKE_MATCH_1}")
    set(m "${CMAKE_MATCH_2}")
    set(d "${CMAKE_MATCH_3}")
    set(n "${CMAKE_MATCH_5}")
    if(n STREQUAL "")
        set(n 0)
    endif()
    # math(EXPR) reads a leading zero as decimal: 2026.10.05 -> 10, 500
    math(EXPR y "${y}")
    math(EXPR m "${m}")
    math(EXPR build "${d} * 100 + ${n}")
    set(${out} "${y}.${m}.${build}.0" PARENT_SCOPE)
endfunction()

function(_fairyfly_xml_escape out text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "\"" "&quot;" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(fairyfly_render_msix_manifest template out version identity_name publisher publisher_display_name)
    if(NOT version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.0$")
        message(FATAL_ERROR "MSIX version \"${version}\" is not Major.Minor.Build.0")
    endif()
    if(NOT identity_name MATCHES "^[-.A-Za-z0-9]+$")
        message(FATAL_ERROR "FAIRYFLY_MSIX_IDENTITY_NAME \"${identity_name}\" may only contain letters, digits, '.' and '-'")
    endif()
    string(LENGTH "${identity_name}" len)
    if(len LESS 3 OR len GREATER 50)
        message(FATAL_ERROR "FAIRYFLY_MSIX_IDENTITY_NAME \"${identity_name}\" must be 3..50 characters long")
    endif()
    if(NOT publisher MATCHES "^CN=.+")
        message(FATAL_ERROR "FAIRYFLY_MSIX_PUBLISHER \"${publisher}\" must be a distinguished name starting with CN=")
    endif()
    string(STRIP "${publisher_display_name}" display)
    if(display STREQUAL "")
        message(FATAL_ERROR "FAIRYFLY_MSIX_PUBLISHER_DISPLAY_NAME must not be empty")
    endif()

    _fairyfly_xml_escape(FAIRYFLY_MSIX_IDENTITY_NAME "${identity_name}")
    _fairyfly_xml_escape(FAIRYFLY_MSIX_PUBLISHER "${publisher}")
    _fairyfly_xml_escape(FAIRYFLY_MSIX_PUBLISHER_DISPLAY_NAME "${publisher_display_name}")
    set(FAIRYFLY_MSIX_VERSION "${version}")
    set(known FAIRYFLY_MSIX_IDENTITY_NAME FAIRYFLY_MSIX_PUBLISHER FAIRYFLY_MSIX_PUBLISHER_DISPLAY_NAME FAIRYFLY_MSIX_VERSION)

    file(READ "${template}" text)
    string(REGEX MATCHALL "@[A-Za-z_][A-Za-z0-9_]*@" placeholders "${text}")
    list(REMOVE_DUPLICATES placeholders)
    foreach(p IN LISTS placeholders)
        string(REGEX REPLACE "^@(.*)@$" "\\1" name "${p}")
        if(NOT name IN_LIST known)
            message(FATAL_ERROR "${template}: placeholder ${p} has no value (known: ${known})")
        endif()
    endforeach()
    configure_file("${template}" "${out}" @ONLY)
    file(READ "${out}" rendered)
    if(rendered MATCHES "@[A-Za-z_][A-Za-z0-9_]*@")
        message(FATAL_ERROR "${out}: unreplaced placeholder ${CMAKE_MATCH_0}")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    fairyfly_msix_version(msix_version "${VERSION}")
    if(DEFINED TEMPLATE)
        foreach(v IDENTITY_NAME PUBLISHER PUBLISHER_DISPLAY_NAME)
            if(NOT DEFINED ${v})
                set(${v} "${FAIRYFLY_MSIX_DEFAULT_${v}}")
            endif()
        endforeach()
        fairyfly_render_msix_manifest("${TEMPLATE}" "${OUT}" "${msix_version}" "${IDENTITY_NAME}" "${PUBLISHER}"
                                      "${PUBLISHER_DISPLAY_NAME}")
    endif()
    message(STATUS "msix version ${msix_version}")
endif()
