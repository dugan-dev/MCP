# Derived from vcpkg baseline 2f1d605400c8727cc00c15797aba796c88ccd523.
# Keep the pinned upstream sources; select Boost's official filesystem profile.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO boostorg/process
    REF boost-${VERSION}
    SHA512 5e7199970509baa84199899f5f47d2114a9f44a4ecd9dc91ed55590fc4ec79ee7ef56b70e79e1a2b44da7e2e47d6a4fb55dcd2e3776f4fd2b279b91b8413ad02
    HEAD_REF master
    PATCHES
        fix-filesystem.patch
        std-filesystem-scan.patch
)

boost_configure_and_install(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS -DBOOST_PROCESS_USE_STD_FS=ON
)
