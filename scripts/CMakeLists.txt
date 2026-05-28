
MESSAGE( STATUS ">> Entering scripts/CMakeLists.txt" )

set(SG3_UTILS_SCRIPTS
    rescan-scsi-bus.sh
    scsi_logging_level
    scsi_mandat
    scsi_readcap
    scsi_ready
    scsi_satl
    scsi_start
    scsi_stop
    scsi_temperature
)

foreach(script ${SG3_UTILS_SCRIPTS})
    install(FILES ${CMAKE_SOURCE_DIR}/scripts/${script}
        PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
	COMPONENT utilities)
endforeach()
