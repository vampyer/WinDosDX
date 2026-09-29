
# CNG key storage (ncrypt.dll): no key storage provider yet; the BCrypt
# functions it re-exports are forwarded to bcrypt.dll.

spec2def(ncrypt.dll ncrypt.spec ADD_IMPORTLIB)

add_library(ncrypt MODULE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/ncrypt_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/ncrypt.def)

set_module_type(ncrypt win32dll ENTRYPOINT 0)
add_importlibs(ncrypt bcrypt kernel32 ntdll)
add_cd_file(TARGET ncrypt DESTINATION reactos/system32 FOR all)
