
# The Visual C++ 2015-2022 runtime DLLs programs import. Most functions are
# forwarded to ucrtbase; see the .spec files.

spec2def(vcruntime140.dll vcruntime140.spec)

add_library(vcruntime140 MODULE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140.def)

set_module_type(vcruntime140 win32dll ENTRYPOINT 0)
add_importlibs(vcruntime140 ucrtbase kernel32 ntdll)
add_cd_file(TARGET vcruntime140 DESTINATION reactos/system32 FOR all)

# 64-bit C++ exception handling (__CxxFrameHandler4) lives in its own DLL.
if(ARCH STREQUAL "amd64")
    spec2def(vcruntime140_1.dll vcruntime140_1.spec)

    add_library(vcruntime140_1 MODULE
        ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140_1_stubs.c
        ${CMAKE_CURRENT_BINARY_DIR}/vcruntime140_1.def)

    set_module_type(vcruntime140_1 win32dll ENTRYPOINT 0)
    add_importlibs(vcruntime140_1 ucrtbase ntdll)
    add_cd_file(TARGET vcruntime140_1 DESTINATION reactos/system32 FOR all)
endif()
