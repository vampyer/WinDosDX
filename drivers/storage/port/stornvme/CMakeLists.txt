
list(APPEND SOURCE
    stornvme.c)

add_library(stornvme MODULE ${SOURCE} stornvme.rc)

set_module_type(stornvme kernelmodedriver)
add_importlibs(stornvme scsiport hal ntoskrnl)
add_cd_file(TARGET stornvme DESTINATION reactos/system32/drivers NO_CAB FOR all)
add_driver_inf(stornvme stornvme.inf)
