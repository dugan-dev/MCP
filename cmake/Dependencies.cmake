include_guard(GLOBAL)

# vcpkg-configuration.json supplies the default Boost.Process profile to both
# manifest installs and direct vcpkg commands. Caller overlay settings stay intact.

# Dependency selection happens before project(), while explicit toolchains and
# prefixes remain available for packaged or system-provided dependencies.
if(NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{CMAKE_TOOLCHAIN_FILE}" STREQUAL ""
   AND NOT CMAKE_PREFIX_PATH AND "$ENV{CMAKE_PREFIX_PATH}" STREQUAL "")
    if(VCPKG_ROOT)
        set(_mcp_vcpkg_root "${VCPKG_ROOT}")
    else()
        set(_mcp_vcpkg_root "$ENV{VCPKG_ROOT}")
    endif()
    if(EXISTS "${_mcp_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
        file(TO_CMAKE_PATH "${_mcp_vcpkg_root}/scripts/buildsystems/vcpkg.cmake" _mcp_toolchain)
        set(CMAKE_TOOLCHAIN_FILE "${_mcp_toolchain}" CACHE FILEPATH
            "Dependency toolchain selected from VCPKG_ROOT")
    endif()
    unset(_mcp_vcpkg_root)
    unset(_mcp_toolchain)
endif()
