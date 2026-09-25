# windos_core - optional WinDosDX DOSBox machine + Win32 platform backend
#
# This static library is built only when WINDOSDX_BUILD_DOS_CORE is enabled.
# It is not an OS boot dependency: userinit and Explorer do not link it.  The
# optional application host owns the machine lifecycle and platform backend.

set(WINDOS_CORE_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(WINDOS_DOSBOX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/dosbox)

set(WINDOS_CORE_SOURCE
    src/windos_common.c
    src/windos_video.c
    src/windos_input.c
    src/windos_timer.c
    src/windos_sound.c
    src/windos_fs.c
    src/windos_host.c
    src/dosbox/windos_stubs.cpp
    include/windos_platform.h
    include/windos_dos.h
    src/windos_internal.h)

# ---------------------------------------------------------------------------
# DOSBox machine core
# ---------------------------------------------------------------------------
enable_language(CXX)

file(GLOB_RECURSE DOSBOX_SOURCE CONFIGURE_DEPENDS
    ${WINDOS_DOSBOX_ROOT}/*.cpp)

# Drop the DOSBox frontends.  WinDosDX replaces the SDL GUI entry point
# (gui/sdlmain.cpp, gui/sdl_gui.cpp), the SDL key mapper (gui/sdl_mapper.cpp,
# replaced by windos_mapper.cpp), the platform layer (platform/), the internal
# debugger (debug/) and the bundled helper libraries (libs/).  The remaining
# gui/ files are the SDL-free renderer and MIDI backends, which are kept.
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX "/platform/")
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX "/debug/")
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX "/libs/")
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX
    "/gui/(sdlmain|sdl_gui|sdl_mapper)\\.cpp$")

# Drop the frontends for backends WinDosDX does not provide: the SDL_CD
# physical drive driver (windos_cdrom.cpp replaces it), the ASPI SCSI CD-ROM
# driver (a legacy Adaptec interface whose headers ReactOS does not ship), the
# Linux and OS/2 CD-ROM drivers, SDL networking (IPX), the SDL_net serial
# backends, and the MAME sound chips that pull in the SDL emulator shim.
# CD images are served by cdrom_image.cpp and serial ports are stubbed by
# windos_serial.cpp.
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX
    "/(cdrom|cdrom_aspi_win32|cdrom_ioctl_linux|cdrom_ioctl_os2)\.cpp$")
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX "/(ipx|ipxserver)\.cpp$")
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX "/serialport/")
list(FILTER DOSBOX_SOURCE EXCLUDE REGEX "/(mame|gameblaster|tandy_sound)\\.cpp$")

list(APPEND WINDOS_CORE_SOURCE ${DOSBOX_SOURCE})

add_library(windos_core STATIC ${WINDOS_CORE_SOURCE})

# The DOSBox core uses C++ exceptions (misc/setup.cpp) and typeid, and its
# stlport configuration needs them enabled.
set_target_cpp_properties(windos_core WITH_EXCEPTIONS WITH_RTTI)

# ReactOS stlport resolves the native C and C++ headers through the
# NATIVE_C_INCLUDE and NATIVE_CPP_INCLUDE macros as flat directories.  ReactOS
# has no such directories, so src/dosbox/cxxcompat supplies both; the generated
# headers there forward through the WINDOS_SDK_* path macros below.  That
# directory is deliberately kept off the include path so a forwarder can never
# resolve back to itself.
set(WINDOS_DOSBOX_CXXCOMPAT ${WINDOS_DOSBOX_ROOT}/cxxcompat)
#
# NATIVE_CPP_INCLUDE is deliberately not set here: linking cppstl makes
# stlport's INTERFACE definitions supply it, and a second definition on this
# target would come later on the command line and win.  The generated headers
# in cxxcompat are reached through NATIVE_C_INCLUDE and the WINDOS_SDK_*
# macros instead, and the few C++ headers stlport still resolves from
# NATIVE_CPP_INCLUDE are provided by sdk/include/c++ itself.
#
target_compile_definitions(windos_core PRIVATE
    WIN32=1 NOMINMAX=1 _USE_MATH_DEFINES=1
    NATIVE_C_INCLUDE=${WINDOS_DOSBOX_CXXCOMPAT}
    WINDOS_SDK_CRT=${CMAKE_SOURCE_DIR}/sdk/include/crt
    WINDOS_SDK_UCRT=${CMAKE_SOURCE_DIR}/sdk/include/ucrt
    WINDOS_SDK_VCRUNTIME=${CMAKE_SOURCE_DIR}/sdk/include/vcruntime
    WINDOS_SDK_CXX=${CMAKE_SOURCE_DIR}/sdk/include/c++
    _STLP_USE_EXCEPTIONS=1
    __USE_CRTIMP=1)

# The DOSBox sources are third-party and predate several MSVC /WX warnings.
# 4146 and 5208 are new in recent MSVC releases and fire inside stlport and in
# the core's own C-style code, not in the ReactOS build.
target_compile_options(windos_core PRIVATE
    /wd4244 /wd4267 /wd4305 /wd4310 /wd4996 /wd4146 /wd5208)

# Force-include the compatibility prelude into the DOSBox translation units
# only, so the vendored sources stay as close to upstream as possible.  The
# /FI path is relative to the compiler's working directory, which is the build
# root, so an absolute path is used via a generator expression.
set_source_files_properties(${DOSBOX_SOURCE} PROPERTIES
    COMPILE_OPTIONS "/FI${WINDOS_DOSBOX_ROOT}/windos_msvc_prelude.h")

target_include_directories(windos_core PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk/include/c++/stlport)

# BEFORE: the DOSBox include directory must win over the SDK's, because the
# core has its own <debug.h> and the SDK ships a reactos/debug.h that errors
# out when reached first.
target_include_directories(windos_core BEFORE PRIVATE
    ${WINDOS_DOSBOX_ROOT}
    ${WINDOS_DOSBOX_ROOT}/include)

# The public platform contract is windos_core/include/windos_platform.h.
# src/ is internal to the backend. Import libraries (user32/gdi32/winmm/...)
# are linked by the final windos.exe, not baked into this static archive.
target_include_directories(windos_core PUBLIC ${WINDOS_CORE_INCLUDE})
target_include_directories(windos_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(windos_core PUBLIC cppstl)
