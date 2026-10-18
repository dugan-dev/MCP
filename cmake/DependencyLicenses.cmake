include_guard(GLOBAL)

# Resolve notices from the dependency actually selected by find_package(), not
# from a global SDK search. Non-vcpkg packages can supply their original files.
function(mcp_install_dependency_license package key target header)
    set(variable "MCP_${key}_LICENSE_FILES")
    set(${variable} "${${variable}}" CACHE STRING "Original ${package} license/copyright files for installation")
    set(licenses "${${variable}}")
    if(NOT licenses)
        set(include_directories "")
        if(TARGET "${target}")
            get_target_property(include_directories "${target}" INTERFACE_INCLUDE_DIRECTORIES)
        elseif(package STREQUAL "jsoncons")
            set(include_directories "${MCP_JSONCONS_INCLUDE_DIR}")
        endif()
        foreach(directory IN LISTS include_directories)
            # Imported install include roots are literal paths. A source/build
            # expression or unusual layout requires an explicit license file.
            if(directory MATCHES "\\$<" OR NOT EXISTS "${directory}/${header}")
                continue()
            endif()
            cmake_path(CONVERT "${directory}" TO_CMAKE_PATH_LIST directory NORMALIZE)
            if(NOT directory MATCHES "/include/?$")
                continue()
            endif()
            get_filename_component(prefix "${directory}" DIRECTORY)
            foreach(candidate IN ITEMS
                    "${prefix}/share/${package}/copyright"
                    "${prefix}/share/licenses/${package}/LICENSE"
                    "${prefix}/share/licenses/${package}/copyright")
                if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
                    set(licenses "${candidate}")
                    break()
                endif()
            endforeach()
            if(licenses)
                break()
            endif()
        endforeach()
    endif()
    if(NOT licenses)
        message(FATAL_ERROR "The original ${package} license was not found beside the selected ${target} headers. "
            "Set ${variable} to the dependency's original license/copyright file(s), separated by semicolons. "
            "These notices are required in the installed MCP package, including static and header-only dependencies.")
    endif()
    foreach(license IN LISTS licenses)
        if(NOT IS_ABSOLUTE "${license}" OR NOT EXISTS "${license}" OR IS_DIRECTORY "${license}")
            message(FATAL_ERROR "${variable} must name existing absolute license files; invalid path: ${license}")
        endif()
    endforeach()
    install(FILES ${licenses} DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/MCP/dependencies/${package}")
endfunction()

function(mcp_install_dependency_licenses)
    mcp_install_dependency_license(nlohmann-json NLOHMANN_JSON nlohmann_json::nlohmann_json nlohmann/json.hpp)
    mcp_install_dependency_license(jsoncons JSONCONS jsoncons::jsoncons jsoncons/json.hpp)
    # Boost components use the same complete Boost Software License.
    mcp_install_dependency_license(boost-process BOOST Boost::process boost/process/v2/process.hpp)
    mcp_install_dependency_license(curl CURL CURL::libcurl curl/curl.h)
    mcp_install_dependency_license(openssl OPENSSL OpenSSL::SSL openssl/ssl.h)
    # CURL's default vcpkg feature set also links zlib, including static Unix SDKs.
    if(TARGET ZLIB::ZLIB)
        mcp_install_dependency_license(zlib ZLIB ZLIB::ZLIB zlib.h)
    endif()
endfunction()
