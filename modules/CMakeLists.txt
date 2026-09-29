
option(ENABLE_ROSAPPS "Enable building the ReactOS extra applications" OFF)
option(ENABLE_ROSTESTS "Enable building the ReactOS test suite" OFF)
option(ENABLE_WALLPAPERS "Integrate wallpapers into the built ISOs" OFF)

if(ENABLE_ROSAPPS)
    add_subdirectory(rosapps)
endif()

if(ENABLE_ROSTESTS)
    add_subdirectory(rostests)
endif()

if(ENABLE_WALLPAPERS)
    add_subdirectory(wallpapers)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/AHK_Tests/)
    add_subdirectory(AHK_Tests)
endif()

## Extra files for the Boot/Live CD. You need to put them under "[boot|live]cd_extras"
## using the same layout as you want them to be in the CD.
## If you change the content of any of the following folders later on,
## run "cmake ." before compiling.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bootcd_extras/)
    file(GLOB_RECURSE EXTRA_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/bootcd_extras/ ${CMAKE_CURRENT_SOURCE_DIR}/bootcd_extras/*)
    foreach(item ${EXTRA_FILES})
        add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/bootcd_extras/${item} DESTINATION extras NO_CAB NAME_ON_CD ${item} FOR bootcd regtest)
    endforeach(item)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/livecd_extras/)
    file(GLOB_RECURSE EXTRA_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/livecd_extras/ ${CMAKE_CURRENT_SOURCE_DIR}/livecd_extras/*)
    foreach(item ${EXTRA_FILES})
        add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/livecd_extras/${item} DESTINATION extras NO_CAB NAME_ON_CD ${item} FOR livecd)
    endforeach(item)
endif()

## Extra files which you need existing on the hard drive after the installation.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/)
    file(GLOB_RECURSE EXTRA_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/ ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/*)
    foreach(item ${EXTRA_FILES})
        add_cd_file(FILE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/${item} DESTINATION reactos/3rdParty NAME_ON_CD ${item} FOR bootcd)
    endforeach(item)
endif()

# Add font file DroidSansFallback.ttf for LiveCD
# See also boot/bootdata/packages/reactos.dff.in
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/optional/DroidSansFallback.ttf)
    add_cd_file(FILE "${CMAKE_CURRENT_SOURCE_DIR}/optional/DroidSansFallback.ttf" DESTINATION reactos/Fonts FOR livecd)
endif()

# WinDosDX: modern (Visual Studio 2015+) application tests on the boot CD.
option(WINDOSDX_MODERNAPP_TESTS "Put the modern-application test programs on the boot CD" OFF)
if(WINDOSDX_MODERNAPP_TESTS)
    add_subdirectory(rostests/windosdx/modernapps)
endif()

# WinDosDX: additionally auto-run the modern-app payload at guest logon
# (disposable-guest regression testing; the payload reports over COM2).
option(WINDOSDX_MODERNAPP_AUTORUN "Auto-run the modern-application tests at logon (disposable guests only)" OFF)
if(WINDOSDX_MODERNAPP_AUTORUN AND NOT WINDOSDX_MODERNAPP_TESTS)
    message(FATAL_ERROR "WINDOSDX_MODERNAPP_AUTORUN requires WINDOSDX_MODERNAPP_TESTS")
endif()
