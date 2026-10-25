# LG NetCast 4.x build, e.g. with the openlgtv buildroot-nc4 toolchain:
#
#   cmake -B build-nc4 -DNC4=ON \
#       -DCMAKE_TOOLCHAIN_FILE=<buildroot>/output/host/share/buildroot/toolchainfile.cmake
#   cmake --build build-nc4 && cmake --install build-nc4 --prefix <usb>/nc4-homebrew/vncserver
#
# libvncserver and libdbus come from the buildroot staging directory.

find_package(PkgConfig REQUIRED)
pkg_check_modules(VNCSERVER REQUIRED libvncserver)

add_executable(webos-vncserver
    src/main.c
    src/server.c
    src/uinput.c
    src/log.c
    src/settings_env.c
)
target_compile_definitions(webos-vncserver PRIVATE NC4)
target_include_directories(webos-vncserver PRIVATE ${VNCSERVER_INCLUDE_DIRS})
target_link_libraries(webos-vncserver ${VNCSERVER_LDFLAGS} pthread dl)
set_target_properties(webos-vncserver PROPERTIES INSTALL_RPATH "$ORIGIN:$ORIGIN/lib")

install(TARGETS webos-vncserver RUNTIME DESTINATION .)

# The TV has none of these: bundle them under their SONAME
install(CODE "
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES \"\${CMAKE_INSTALL_PREFIX}/webos-vncserver\"
        RESOLVED_DEPENDENCIES_VAR deps
        DIRECTORIES \"${CMAKE_SYSROOT}/usr/lib\"
        PRE_EXCLUDE_REGEXES \"^lib(c|m|dl|pthread|rt|gcc_s)\\\\.so\" \"^ld-linux\"
        POST_INCLUDE_REGEXES \"^${CMAKE_SYSROOT}/usr/lib/\"
        POST_EXCLUDE_REGEXES \".*\")
    foreach(dep \${deps})
        execute_process(COMMAND \"${CMAKE_OBJDUMP}\" -p \"\${dep}\" OUTPUT_VARIABLE dump)
        string(REGEX MATCH \"SONAME +([^\\n]+)\" _ \"\${dump}\")
        file(COPY_FILE \"\${dep}\" \"\${CMAKE_INSTALL_PREFIX}/lib/\${CMAKE_MATCH_1}\")
    endforeach()
")
