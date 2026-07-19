# FindeBUS.cmake — locates the eBUS SDK (JAI or generic Pleora edition).
#
# Search order:
#   1. EBUS_SDK_ROOT (CMake cache variable, then environment variable)
#   2. /opt/jai/ebus_sdk/<dist>      (eBUS SDK for JAI)
#   3. /opt/pleora/ebus_sdk/<dist>   (generic Pleora eBUS SDK)
#
# Defines:
#   eBUS_FOUND, EBUS_FOUND
#   eBUS::eBUS               imported interface target (includes + libs)
#   EBUS_SDK_ROOT_DIR        SDK root
#   EBUS_LIB_DIR             <root>/lib
#   EBUS_GENICAM_ROOT_DIR    <root>/lib/genicam
#   EBUS_GENICAM_LIB_DIR     GenICam shared-library dir (Linux64_x64)
#   EBUS_GENICAM_ENV_NAME    e.g. GENICAM_ROOT_V3_4 (parsed from set_puregev_env.sh)

set(_ebus_candidates "")
if(DEFINED EBUS_SDK_ROOT AND NOT "${EBUS_SDK_ROOT}" STREQUAL "")
    list(APPEND _ebus_candidates "${EBUS_SDK_ROOT}")
endif()
if(DEFINED ENV{EBUS_SDK_ROOT} AND NOT "$ENV{EBUS_SDK_ROOT}" STREQUAL "")
    list(APPEND _ebus_candidates "$ENV{EBUS_SDK_ROOT}")
endif()
file(GLOB _ebus_jai_dirs "/opt/jai/ebus_sdk/*")
file(GLOB _ebus_pleora_dirs "/opt/pleora/ebus_sdk/*")
list(APPEND _ebus_candidates ${_ebus_jai_dirs} ${_ebus_pleora_dirs})

set(EBUS_SDK_ROOT_DIR "")
foreach(_cand IN LISTS _ebus_candidates)
    if(EXISTS "${_cand}/include/PvSystem.h" AND EXISTS "${_cand}/lib/libPvBase.so")
        set(EBUS_SDK_ROOT_DIR "${_cand}")
        break()
    endif()
endforeach()

if(EBUS_SDK_ROOT_DIR STREQUAL "")
    set(eBUS_FOUND FALSE)
    set(EBUS_FOUND FALSE)
    if(eBUS_FIND_REQUIRED)
        message(FATAL_ERROR "eBUS SDK not found (searched EBUS_SDK_ROOT, /opt/jai/ebus_sdk/*, /opt/pleora/ebus_sdk/*)")
    endif()
    return()
endif()

set(EBUS_INCLUDE_DIR "${EBUS_SDK_ROOT_DIR}/include")
set(EBUS_LIB_DIR "${EBUS_SDK_ROOT_DIR}/lib")
set(EBUS_GENICAM_ROOT_DIR "${EBUS_SDK_ROOT_DIR}/lib/genicam")

# GenICam runtime library directory (name observed across 6.x releases)
set(EBUS_GENICAM_LIB_DIR "")
foreach(_gdir "bin/Linux64_x64" "bin/Linux64_ARM")
    if(EXISTS "${EBUS_GENICAM_ROOT_DIR}/${_gdir}")
        set(EBUS_GENICAM_LIB_DIR "${EBUS_GENICAM_ROOT_DIR}/${_gdir}")
        break()
    endif()
endforeach()
if(EBUS_GENICAM_LIB_DIR STREQUAL "")
    file(GLOB _gcandidates "${EBUS_GENICAM_ROOT_DIR}/bin/*")
    foreach(_g IN LISTS _gcandidates)
        if(IS_DIRECTORY "${_g}")
            set(EBUS_GENICAM_LIB_DIR "${_g}")
            break()
        endif()
    endforeach()
endif()

# GenICam env var name (GENICAM_ROOT_V3_4 in 6.x; parse from the SDK's own
# environment script to survive minor-version bumps)
set(EBUS_GENICAM_ENV_NAME "GENICAM_ROOT_V3_4")
if(EXISTS "${EBUS_SDK_ROOT_DIR}/bin/set_puregev_env.sh")
    file(STRINGS "${EBUS_SDK_ROOT_DIR}/bin/set_puregev_env.sh" _env_lines REGEX "GENICAM_ROOT_V[0-9_]+")
    if(_env_lines)
        list(GET _env_lines 0 _env_line)
        string(REGEX MATCH "GENICAM_ROOT_V[0-9_]+" _env_name "${_env_line}")
        if(_env_name)
            set(EBUS_GENICAM_ENV_NAME "${_env_name}")
        endif()
    endif()
endif()

# Core libraries (verify against <root>/share/samples Makefiles when a new
# SDK version changes the list). PvBase/PvDevice/PvStream/PvBuffer/PvGenICam/
# PvSystem are required; the rest are linked when present.
set(_ebus_required_libs PvBase PvDevice PvStream PvBuffer PvGenICam PvSystem)
set(_ebus_optional_libs PvPersistence PvSerial PvAppUtils EbTransportLayerLib EbUtilsLib)

set(_ebus_libs "")
set(_ebus_missing "")
foreach(_lib IN LISTS _ebus_required_libs)
    if(EXISTS "${EBUS_LIB_DIR}/lib${_lib}.so")
        list(APPEND _ebus_libs "${EBUS_LIB_DIR}/lib${_lib}.so")
    else()
        list(APPEND _ebus_missing "${_lib}")
    endif()
endforeach()
foreach(_lib IN LISTS _ebus_optional_libs)
    if(EXISTS "${EBUS_LIB_DIR}/lib${_lib}.so")
        list(APPEND _ebus_libs "${EBUS_LIB_DIR}/lib${_lib}.so")
    endif()
endforeach()

if(_ebus_missing)
    message(WARNING "eBUS SDK at ${EBUS_SDK_ROOT_DIR} is missing expected libraries: ${_ebus_missing}")
endif()

if(NOT TARGET eBUS::eBUS)
    add_library(eBUS::eBUS INTERFACE IMPORTED)
    set_target_properties(eBUS::eBUS PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${EBUS_INCLUDE_DIR}"
        INTERFACE_INCLUDE_DIRECTORIES "${EBUS_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${_ebus_libs}"
    )
    # RPATH to the SDK + GenICam lib dirs for every target that links this,
    # transitively (jai_discover, AmigaDrivers): the binaries run without
    # sourcing set_puregev_env.sh.
    set_property(TARGET eBUS::eBUS APPEND PROPERTY INTERFACE_LINK_OPTIONS
        "-Wl,-rpath,${EBUS_LIB_DIR}")
    if(NOT EBUS_GENICAM_LIB_DIR STREQUAL "")
        set_property(TARGET eBUS::eBUS APPEND PROPERTY INTERFACE_LINK_OPTIONS
            "-Wl,-rpath,${EBUS_GENICAM_LIB_DIR}")
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(eBUS
    REQUIRED_VARS EBUS_SDK_ROOT_DIR EBUS_INCLUDE_DIR EBUS_LIB_DIR
)
set(EBUS_FOUND ${eBUS_FOUND})
message(STATUS "eBUS SDK: ${EBUS_SDK_ROOT_DIR}")
message(STATUS "  GenICam lib dir: ${EBUS_GENICAM_LIB_DIR} (env ${EBUS_GENICAM_ENV_NAME})")
