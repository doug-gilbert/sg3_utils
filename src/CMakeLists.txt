
MESSAGE(STATUS ">> Entering src/CMakeLists.txt" )

if ( PT_DUMMY )
    MESSAGE( STATUS "<<>> PT_DUMMY configured, no HAL and some utils not built")
endif ( PT_DUMMY )

if ( BUILD_SHARED_LIBS )
    MESSAGE( STATUS ">> Build utilities based on sgutils2 library (default)" )
else ( BUILD_SHARED_LIBS )
    MESSAGE( STATUS ">> Build static utilities" )
    add_definitions ( -static )
endif ( BUILD_SHARED_LIBS )


function(build_one util_name source_files)

    if ( NEED_GETOPT_LONG OR NEED_GETOPT_H )
        add_executable( ${util_name} ${source_files} ${CMAKE_SOURCE_DIR}/lib/getopt_long.c )
    else ( )
        add_executable( ${util_name} ${source_files} )
    endif ( )

    if ( ENABLE_DEBUG )
        target_compile_definitions(${util_name} PRIVATE DEBUG)
        target_compile_options(${util_name} PRIVATE
            -Wextra -Wmisleading-indentation -Wduplicated-cond -Wlogical-op
            -Wnull-dereference -Wshadow -Wunused -Wsizeof-array-argument
            -Wduplicated-branches -Wjump-misses-init -Wparentheses)
    endif ( ENABLE_DEBUG )

    target_include_directories(${util_name} PRIVATE ${CMAKE_BINARY_DIR})
    target_include_directories(${util_name} PUBLIC ${CMAKE_SOURCE_DIR}/include)
    if ( SG_LIB_FREEBSD )
	target_include_directories( ${util_name} PRIVATE /usr/local/include )
        target_link_libraries( ${util_name} PRIVATE cam )
    endif ( SG_LIB_FREEBSD )
    if ( NEED_GETOPT_H )
        target_include_directories(${util_name} PRIVATE ${CMAKE_SOURCE_DIR}/lib)
    endif ( NEED_GETOPT_H )

    if ( NOT BUILD_SHARED_LIBS )
        target_link_options(${util_name} PRIVATE -static)
    endif ( NOT BUILD_SHARED_LIBS )

    target_link_libraries( ${util_name} PRIVATE sgutils2 )

    install(TARGETS ${util_name}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT utilities
    )
endfunction()

set(SG3_UTILS
	sg_bg_ctl
	sg_compare_and_write
	sg_decode_sense
	sg_format
	sg_get_config
	sg_get_elem_status
	sg_get_lba_status
	sg_ident
	# needs sg_vpd_common.c ++ : sg_inq
	# needs sg_logs_vendor.c : sg_logs
	# needed for sg_logs : sg_logs_vendor
	sg_luns
	sg_modes
	sg_opcodes
	sg_persist
	sg_prevent
	sg_raw
	sg_rdac
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
	# want to build sg_scan : sg_scan_linux.c
	# want to build sg_scan : sg_scan_win32.c
	sg_seek
	sg_senddiag
	sg_ses
	sg_ses_microcode
	sg_start
	sg_stpg
	sg_stream_ctl
	sg_sync
	sg_timestamp
	sg_turs
	sg_unmap
	sg_verify
	# needs sg_vpd_common.c ++ : sg_vpd
	# part of sg_inq+sg_vpd: sg_vpd_common
	# part of sg_inq+sg_vpd: sg_vpd_vendor
	sg_wr_mode
	sg_write_attr
	sg_write_buffer
	sg_write_long
	sg_write_same
	sg_write_verify
	sg_write_x
	sg_z_act_query
	sg_zone
)

set ( SG3_UTILS_LINUX_ONLY
	# *_dd utilities are better implemented in a separate package called
	# ddpt. ddpt can run on multiple platforms, not just Linux.
	sg_copy_results
	sg_dd
	sg_emc_trespass
	sginfo
	sgm_dd
	sg_map
	sg_map26
	sgp_dd
	sg_rbuf
	sg_read
	sg_reset
	sg_test_rwbuf
	sg_xcopy
    )

set ( sg_inq_source
	sg_inq.c
	sg_vpd_common.c
    )
# Double quotes make arg2 a semi-colon separated list
build_one( sg_inq "${sg_inq_source}" )

set ( sg_logs_source
	sg_logs.c
	sg_logs_vendor.c
    )
build_one( sg_logs "${sg_logs_source}" )

set ( sg_vpd_source
	sg_vpd.c
	sg_vpd_common.c
	sg_vpd_vendor.c
    )
build_one( sg_vpd "${sg_vpd_source}" )


# For the rest, each utility corresponds to one source file of the same
# name (apart from the ".c" at the end of the source file).
foreach(util ${SG3_UTILS})
    build_one( ${util} ${util}.c )
endforeach()


if (SG_LIB_LINUX)
    if (NOT PT_DUMMY)
        foreach(util ${SG3_UTILS_LINUX_ONLY})
            build_one( ${util} ${util}.c )
        endforeach()

        build_one( sg_scan sg_scan_linux.c )
    endif (NOT PT_DUMMY)

elseif (SG_LIB_WIN32)
    if (NOT PT_DUMMY)
        build_one( sg_scan sg_scan_win32.c )
    endif (NOT PT_DUMMY)
endif (SG_LIB_LINUX)

# Per-utility link dependencies matching autotools (src/Makefile.am):
#   sgp_dd needs pthread, sg_seek and sg_turs need rt (for clock_gettime)
if (SG_LIB_LINUX AND NOT PT_DUMMY)
    if (Threads_FOUND)
        target_link_libraries(sgp_dd PRIVATE Threads::Threads)
    endif ()
endif ()

if (NEED_RT_LIB)
    target_link_libraries(sg_seek PRIVATE rt)
    target_link_libraries(sg_turs PRIVATE rt)
endif ()

