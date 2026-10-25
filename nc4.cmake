# LG NetCast 4.x build, e.g. with the openlgtv buildroot-nc4 toolchain:
#
#   cmake -B build-nc4 -DNC4=ON \
#       -DCMAKE_TOOLCHAIN_FILE=<buildroot>/output/host/share/buildroot/toolchainfile.cmake
#   cmake --build build-nc4 && cmake --install build-nc4 --prefix <usb>/nc4-homebrew/vncserver
#
# libvncserver and libdbus come from the buildroot staging directory.

find_package(PkgConfig REQUIRED)
pkg_check_modules(VNCSERVER REQUIRED libvncserver)
pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(CRYPTO REQUIRED libcrypto)

add_executable(webos-vncserver
    src/main.c
    src/server.c
    src/uinput.c
    src/log.c
    src/settings_env.c
)
target_compile_definitions(webos-vncserver PRIVATE NC4)
target_include_directories(webos-vncserver PRIVATE ${VNCSERVER_INCLUDE_DIRS})
target_link_libraries(webos-vncserver ${VNCSERVER_LDFLAGS} ${CRYPTO_LDFLAGS} pthread dl)
set_target_properties(webos-vncserver PROPERTIES INSTALL_RPATH "$ORIGIN:$ORIGIN/lib")

add_library(capture_nc4 SHARED src/capture_nc4.c)
target_include_directories(capture_nc4 PRIVATE ${DBUS_INCLUDE_DIRS})
target_link_libraries(capture_nc4 ${DBUS_LDFLAGS})
# the TVs' Cortex-A9 has NEON, used to read the uncached surfaces
target_compile_options(capture_nc4 PRIVATE -mfpu=neon)
set_target_properties(capture_nc4 PROPERTIES INSTALL_RPATH "$ORIGIN")

install(TARGETS webos-vncserver capture_nc4 RUNTIME DESTINATION . LIBRARY DESTINATION lib)
install(PROGRAMS nc4/vncserver.sh DESTINATION .)
# OpenSSL 3 legacy provider, for the DES of VNC authentication
install(FILES ${CMAKE_SYSROOT}/usr/lib/ossl-modules/legacy.so DESTINATION lib/ossl-modules)

# The TV has none of these: bundle them under their SONAME
install(CODE "
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES \"\${CMAKE_INSTALL_PREFIX}/webos-vncserver\"
        LIBRARIES \"\${CMAKE_INSTALL_PREFIX}/lib/libcapture_nc4.so\"
        RESOLVED_DEPENDENCIES_VAR deps
        DIRECTORIES \"${CMAKE_SYSROOT}/usr/lib\" \"${CMAKE_SYSROOT}/lib\"
        PRE_EXCLUDE_REGEXES \"^lib(c|m|dl|pthread|rt|gcc_s)\\\\.so\" \"^ld-linux\"
        POST_INCLUDE_REGEXES \"^${CMAKE_SYSROOT}/(usr/)?lib/\"
        POST_EXCLUDE_REGEXES \".*\")
    foreach(dep \${deps})
        execute_process(COMMAND \"${CMAKE_OBJDUMP}\" -p \"\${dep}\" OUTPUT_VARIABLE dump)
        string(REGEX MATCH \"SONAME +([^\\n]+)\" _ \"\${dump}\")
        file(COPY_FILE \"\${dep}\" \"\${CMAKE_INSTALL_PREFIX}/lib/\${CMAKE_MATCH_1}\")
    endforeach()
")
