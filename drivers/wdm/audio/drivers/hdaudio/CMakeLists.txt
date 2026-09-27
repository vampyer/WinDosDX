
list(APPEND SOURCE
    adapter.cpp
    codec.cpp
    topo.cpp
    wave.cpp
    hdacodec.h)

add_library(hdaudio MODULE
    ${SOURCE}
    hdaudio.rc)

target_link_libraries(hdaudio stdunk libcntpr uuid)
set_module_type(hdaudio wdmdriver)
add_importlibs(hdaudio portcls hal ntoskrnl)
add_cd_file(TARGET hdaudio DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(hdaudio hdaudio.inf)
