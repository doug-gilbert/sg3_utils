
MESSAGE( STATUS ">> Entering doc/CMakeLists.txt" )


set(SG3_UTILS
	rescan-scsi-bus.sh
	scsi-rescan
	scsi_logging_level
	scsi_mandat
	scsi_readcap
	scsi_ready
	scsi_satl
	scsi_start
	scsi_stop
	scsi_temperature
	sg3_utils
	sg3_utils_json
	sg_bg_ctl
	sg_compare_and_write
	sg_copy_results
	sg_dd
	sg_decode_sense
	sg_emc_trespass
	sg_format
	sg_get_config
	sg_get_elem_status
	sg_get_lba_status
	sg_ident
	sg_inq
	sg_logs
	sg_luns
	sg_map
	sg_map26
	sg_modes
	sg_opcodes
	sg_persist
	sg_prevent
	sg_raw
	sg_rbuf
	sg_rdac
	sg_read
	sg_read_attr
	sg_read_block_limits
	sg_read_buffer
	sg_read_long
	sg_readcap
	sg_reassign
	sg_referrals
	sg_rem_rest_elem
	sg_rep_density
	sg_rep_pip
	sg_rep_zones
	sg_requests
	sg_reset
	sg_reset_wp
	sg_rmsn
	sg_rtpg
	sg_safte
	sg_sanitize
	sg_sat_datetime
	sg_sat_identify
	sg_sat_phy_event
	sg_sat_read_gplog
	sg_sat_set_features
	sg_seek
	sg_senddiag
	sg_ses
	sg_ses_microcode
	sg_start
	sg_stpg
	sg_stream_ctl
	sg_sync
	sg_test_rwbuf
	sg_timestamp
	sg_turs
	sg_unmap
	sg_verify
	sg_vpd
	sg_wr_mode
	sg_write_attr
	sg_write_buffer
	sg_write_long
	sg_write_same
	sg_write_verify
	sg_write_x
	sg_xcopy
	sg_z_act_query
	sg_zone
	sginfo
	sgm_dd
	sgp_dd
)

foreach(util ${SG3_UTILS})

    file(ARCHIVE_CREATE
         OUTPUT ${CMAKE_BINARY_DIR}/doc/${util}.8.gz
         PATHS doc/${util}.8
         FORMAT raw
         COMPRESSION GZip
    )
    install(FILES ${CMAKE_BINARY_DIR}/doc/${util}.8.gz
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
            COMPONENT utilities
    )

endforeach()

if (OS_LINUX)
    file(ARCHIVE_CREATE
         OUTPUT ${CMAKE_BINARY_DIR}/doc/sg_scan.8.gz
         PATHS doc/sg_scan.8.linux
         FORMAT raw
         COMPRESSION GZip
    )
    install(FILES ${CMAKE_BINARY_DIR}/doc/sg_scan.8.gz
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
            COMPONENT utilities
    )
endif (OS_LINUX)

if (SG_LIB_WIN32)
    file(ARCHIVE_CREATE
         OUTPUT ${CMAKE_BINARY_DIR}/doc/sg_scan.8.gz
         PATHS doc/sg_scan.8.win32
         FORMAT raw
         COMPRESSION GZip
    )
    install(FILES ${CMAKE_BINARY_DIR}/doc/sg_scan.8.gz
            DESTINATION "${CMAKE_INSTALL_MANDIR}/man8"
            COMPONENT utilities
    )
endif (SG_LIB_WIN32)
