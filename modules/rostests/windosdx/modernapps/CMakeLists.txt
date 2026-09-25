# Modern-application tests: real Visual Studio 2022 programs, built with the
# host toolset by build.cmd (not by the WinDosDX toolchain) so they exercise
# the same loader, API-set and CRT paths as third-party software.
# Enabled with -DWINDOSDX_MODERNAPP_TESTS=ON; see modules/CMakeLists.txt.

if(ARCH STREQUAL "i386")
    set(_modernapps_arch x86)
elseif(ARCH STREQUAL "amd64")
    set(_modernapps_arch x64)
else()
    return()
endif()

set(_modernapps_dir ${REACTOS_SOURCE_DIR}/output-modernapps/${_modernapps_arch})
if(NOT EXISTS ${_modernapps_dir}/run_all.cmd)
    message(WARNING "Modern-app tests not built: run modules/rostests/windosdx/modernapps/build.cmd ${_modernapps_arch}")
    return()
endif()

file(GLOB _modernapps_files ${_modernapps_dir}/*.exe ${_modernapps_dir}/*.dll ${_modernapps_dir}/*.cmd)
foreach(_file ${_modernapps_files})
    add_cd_file(FILE ${_file} DESTINATION reactos/tests/modern NO_CAB FOR all)
endforeach()
