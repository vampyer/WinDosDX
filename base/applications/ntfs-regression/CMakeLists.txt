include_directories(${REACTOS_SOURCE_DIR}/sdk/include/reactos/libs/fmifs)

add_executable(ntfs-regression ntfs-regression.c)
set_module_type(ntfs-regression win32cui UNICODE)
add_importlibs(ntfs-regression fmifs msvcrt user32 kernel32 ntdll)
add_cd_file(TARGET ntfs-regression DESTINATION reactos/system32 FOR all)
