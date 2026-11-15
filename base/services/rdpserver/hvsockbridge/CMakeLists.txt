# PROJECT:     LiberNT RDP console server
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Bridge Hyper-V socket RDP connections to the local RDP server
# COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193@gmail.com>

include_directories(${REACTOS_SOURCE_DIR}/drivers/bus/vmbus)
add_executable(hvsockbridge hvsockbridge.c hvsockbridge.rc)
set_module_type(hvsockbridge win32cui UNICODE)
add_importlibs(hvsockbridge advapi32 ws2_32 msvcrt kernel32)
add_cd_file(TARGET hvsockbridge DESTINATION reactos/system32/rdpserver FOR livecd preinstall)
