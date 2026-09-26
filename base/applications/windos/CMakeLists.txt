# WinDosDX DOS machine: windos.exe, the DOSBox-derived DOS 6.22 machine.
#
# Built by default. Windows starts DOS programs in it (kernel32 CreateProcess,
# unless HKLM\SOFTWARE\WinDosDX\DOS Engine = "ntvdm"), and the Start menu
# has a "DOS 6.22" entry. It is not part of the OS boot path.

option(WINDOSDX_BUILD_DOS_CORE "Build the WinDosDX DOSBox core" ON)
option(WINDOSDX_BUILD_DOS_HOST "Build windos.exe, the WinDosDX DOS 6.22 machine" ON)

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
    target_link_libraries(windos_devhost windos_core)
    add_importlibs(windos_devhost user32 gdi32 winmm comctl32 shell32 ole32 msvcrt kernel32)
    add_cd_file(TARGET windos_devhost DESTINATION reactos FOR all)
elseif(WINDOSDX_BUILD_DOS_CORE)
    # Preserve an explicit target for developers who opt in to the core.
    add_custom_target(windos DEPENDS windos_core)
endif()
