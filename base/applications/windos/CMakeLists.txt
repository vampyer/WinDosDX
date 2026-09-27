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

# Optional: a local folder of DOS programs to put on the live CD as
# \DOSGames, for trying them without installing. It is never part of the
# source tree (the programs are the user's own); unset, nothing is added.
set(WINDOSDX_DOS_FILES_DIR "" CACHE PATH
    "Local folder of DOS programs to copy to the live CD as DOSGames")
if(WINDOSDX_DOS_FILES_DIR)
    file(GLOB_RECURSE _dos_files LIST_DIRECTORIES false
         RELATIVE "${WINDOSDX_DOS_FILES_DIR}" "${WINDOSDX_DOS_FILES_DIR}/*")
    foreach(_file IN LISTS _dos_files)
        get_filename_component(_dir "${_file}" DIRECTORY)
        add_cd_file(FILE "${WINDOSDX_DOS_FILES_DIR}/${_file}"
                    DESTINATION "DOSGames/${_dir}" FOR livecd)
    endforeach()
    list(LENGTH _dos_files _count)
    message(STATUS "WinDosDX: ${_count} DOS files from ${WINDOSDX_DOS_FILES_DIR} go on the live CD")
endif()
