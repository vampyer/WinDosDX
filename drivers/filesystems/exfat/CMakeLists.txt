
# exFAT file system driver. Forked from vfatfs: the IRP, FCB, cache and
# locking machinery is vfatfs's; the on-disk layer is exFAT's.
list(APPEND SOURCE
    blockdev.c
    cleanup.c
    close.c
    create.c
    dir.c
    direntry.c
    dirwr.c
    ea.c
    fat.c
    fastio.c
    fcb.c
    finfo.c
    flush.c
    fsctl.c
    iface.c
    misc.c
    pnp.c
    rw.c
    shutdown.c
    string.c
    volume.c
    vfat.h)

add_library(exfat MODULE ${SOURCE} exfat.rc)
set_module_type(exfat kernelmodedriver)
target_link_libraries(exfat ${PSEH_LIB})
add_importlibs(exfat ntoskrnl hal)
add_pch(exfat vfat.h SOURCE)
add_cd_file(TARGET exfat DESTINATION reactos/system32/drivers NO_CAB FOR all)
