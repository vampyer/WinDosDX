add_definitions(
    -DNDIS50_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_MINIPORT=1)

list(APPEND SOURCE
    ndis.c
    hardware.c
    info.c
    interrupt.c
    nic.h
    i225hw.h
    debug.c
    debug.h
    send.c)

add_library(i225 MODULE ${SOURCE} i225.rc)
add_pch(i225 nic.h SOURCE)
set_module_type(i225 kernelmodedriver)
add_importlibs(i225 ndis ntoskrnl hal)
add_cd_file(TARGET i225 DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(i225 nei225.inf)
