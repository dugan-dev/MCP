# Boost.Process std::filesystem profile

This project-local vcpkg overlay derives from `ports/boost-process` in vcpkg
baseline `2f1d605400c8727cc00c15797aba796c88ccd523`. It retains Boost 1.91.0,
the original download SHA512 and `fix-filesystem.patch`. The build passes the
upstream CMake option `BOOST_PROCESS_USE_STD_FS=ON`; the manifest
omits the Boost.Filesystem dependency that the selected configuration does not
use. All other dependency versions and port support restrictions are retained.

The first real configure failed because vcpkg's BoostRoot scans CMake source
text before evaluating the official filesystem option. It tried to find
`boost_filesystem` from the inactive `else()` branch. The CMake-only
`std-filesystem-scan.patch` adds BoostRoot's supported `Boost-Exclude` directive
for that dependency. It leaves the upstream target and both option branches
intact; this dedicated overlay always selects the std::filesystem branch. No
BoostRoot helper or C++ implementation source is changed.

The official Boost target builds its compiled implementation with
`BOOST_PROCESS_USE_STD_FS` and exports the same definition to consumers. This
keeps filesystem path types consistent across the library ABI. MCP rejects a
Boost.Process package without that exported definition, both when building and
when consuming the installed MCP package. Adding a private compile definition
to an application cannot convert a library built with Boost.Filesystem.

This is a dedicated Process V2 profile for MCP. Legacy Process V1 headers remain
in the upstream installation, but their Boost.Filesystem dependency is outside
this profile. Other libraries in the same application must use a compatible
Boost.Process configuration. System package users can build Boost.Process with
the same official CMake option instead of using vcpkg. No implementation source
is patched or duplicated by MCP.

Direct `vcpkg install` and CMake manifest installs select this profile through
`vcpkg-configuration.json`. To override it, pass `--overlay-ports` to vcpkg or
set the CMake variable `VCPKG_OVERLAY_PORTS`, which the toolchain passes as
command-line overlays. Native vcpkg resolution checks command-line overlays,
then configuration-file overlays, then `VCPKG_OVERLAY_PORTS` environment entries.
An overriding Process port must export the compatible filesystem profile or
configuration fails with a repair instruction.

The original vcpkg port is covered by [Microsoft's MIT license](VCPKG-LICENSE.txt).
Boost is covered by the [Boost Software License](BOOST-LICENSE.txt); the normal
vcpkg helper continues installing that dependency license with the package.
