# WinDosDX DOS machine - optional standalone component.
#
# The DOSBox-derived machine is not linked into userinit or any other OS boot
# path.  Keep the source and target available for the future DOSBox
# application, but leave the core out of the default product build.

option(WINDOSDX_BUILD_DOS_CORE "Build the optional WinDosDX DOSBox core" OFF)
option(WINDOSDX_BUILD_DOS_HOST "Build the optional Win32 DOS development host" OFF)

if(WINDOSDX_BUILD_DOS_CORE)
    add_subdirectory(windos_core)
endif()

if(WINDOSDX_BUILD_DOS_HOST)
    if(NOT WINDOSDX_BUILD_DOS_CORE)
        message(FATAL_ERROR
            "WINDOSDX_BUILD_DOS_HOST requires WINDOSDX_BUILD_DOS_CORE=ON")
    endif()

    list(APPEND SOURCE
        main.c
        windos.rc)
    add_executable(windos_devhost ${SOURCE})
    set_target_properties(windos_devhost PROPERTIES OUTPUT_NAME "windos")
    set_module_type(windos_devhost win32gui UNICODE)
    target_link_libraries(windos_devhost PRIVATE windos_core)
    add_importlibs(windos_devhost user32 gdi32 winmm comctl32 msvcrt kernel32)
    add_cd_file(TARGET windos_devhost DESTINATION reactos FOR all)
elseif(WINDOSDX_BUILD_DOS_CORE)
    # Preserve an explicit target for developers who opt in to the core.
    add_custom_target(windos DEPENDS windos_core)
endif()
