
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

spec2def(shcore.dll shcore.spec ADD_IMPORTLIB)

add_definitions(-DCONST_VTABLE)

# The DPI functions call user32's *Internal helpers, exported from 0xA00 on.
if(DLL_EXPORT_VERSION GREATER_EQUAL 0xA00)
    add_definitions(-DWINDOSDX_DPI_API)
    set(SHCORE_USER32 user32)
endif()

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/shcore.def
    ${CMAKE_CURRENT_BINARY_DIR}/shcore_stubs.c
)

add_library(shcore MODULE ${SOURCE})
set_module_type(shcore win32dll ENTRYPOINT 0)
if(MSVC)
    target_compile_options(shcore PRIVATE /wd4267) # warning C4267: '=': conversion from 'size_t' to 'ULONG', possible loss of data
endif()
target_link_libraries(shcore crtheap wine wine_dll_canunload)
add_importlibs(shcore combase ${SHCORE_USER32} advapi32 advapi32_vista kernel32 ntdll)
add_dependencies(shcore psdk)
add_cd_file(TARGET shcore DESTINATION reactos/system32 FOR all)
set_wine_module(shcore)
