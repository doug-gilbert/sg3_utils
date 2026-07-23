
MESSAGE( STATUS ">> Entering include/CMakeLists.txt" )

install(
    FILES
        sg_lib.h
        sg_unaligned.h
	sg_cmds.h
	sg_cmds_basic.h
	sg_cmds_extra.h
	sg_cmds_mmc.h
	sg_json.h
	sg_json_sg_lib.h
	sg_lib.h
	sg_lib_data.h
	sg_lib_names.h
	sg_nvme.h
	sg_pr2serr.h
	sg_pt.h
	sg_snt.h
	sg_unaligned.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/scsi
    COMPONENT development
)

if (OS_LINUX)
    install(
	FILES
	    sg_linux_inc.h
	    sg_io_linux.h
	    sg_pt_linux.h
	DESTINATION
	    ${CMAKE_INSTALL_INCLUDEDIR}/scsi
	COMPONENT development
    )
endif (OS_LINUX)

if (OS_WIN32)
    install(
	FILES
	    sg_pt_win32.h
	DESTINATION
	    ${CMAKE_INSTALL_INCLUDEDIR}/scsi
	COMPONENT development
   )
endif (OS_WIN32)

#
# Install headers
#

# install(
#     DIRECTORY .
#     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
#     COMPONENT development
#     FILES_MATCHING PATTERN "*.h"
# )
