#!/bin/sh

echo "Entering src/cmake_del_artifacts.sh"

rm -rf \
	*.o \
	CMakeFiles \
	cmake_install.cmake \
	Makefile

rm -f   sg_bg_ctl
rm -f   sg_compare_and_write
rm -f   sg_copy_results
rm -f   sg_decode_sense
rm -f   sg_emc_trespass
rm -f   sg_format
rm -f   sg_get_config
rm -f   sg_get_elem_status
rm -f   sg_get_lba_status
rm -f   sg_ident
rm -f   sg_inq
rm -f   sg_logs
rm -f   sg_luns
rm -f   sg_map
rm -f   sg_map26
rm -f   sg_modes
rm -f   sg_opcodes
rm -f   sg_persist
rm -f   sg_prevent
rm -f   sg_raw
rm -f   sg_rbuf
rm -f   sg_rdac
rm -f   sg_read
rm -f   sg_read_attr
rm -f   sg_read_block_limits
rm -f   sg_read_buffer
rm -f   sg_read_long
rm -f   sg_readcap
rm -f   sg_reassign
rm -f   sg_referrals
rm -f   sg_rem_rest_elem
rm -f   sg_rep_density
rm -f   sg_rep_pip
rm -f   sg_rep_zones

rm -f   sg_requests
rm -f   sg_reset
rm -f   sg_reset_wp
rm -f   sg_rmsn
rm -f   sg_rtpg
rm -f   sg_safte
rm -f   sg_sanitize
rm -f   sg_sat_datetime
rm -f   sg_sat_identify
rm -f   sg_sat_phy_event
rm -f   sg_sat_read_gplog
rm -f   sg_sat_set_features
rm -f   sg_scan
rm -f   sg_seek
rm -f   sg_senddiag
rm -f   sg_ses
rm -f   sg_ses_microcode
rm -f   sg_start
rm -f   sg_stpg
rm -f   sg_stream_ctl
rm -f   sg_sync
rm -f   sg_test_rwbuf
rm -f   sg_timestamp
rm -f   sg_turs
rm -f   sg_unmap
rm -f   sg_verify
rm -f   sg_vpd
rm -f   sg_wr_mode
rm -f   sg_write_attr
rm -f   sg_write_buffer
rm -f   sg_write_long
rm -f   sg_write_same
rm -f   sg_write_verify
rm -f   sg_write_x
rm -f   sg_xcopy
rm -f   sg_z_act_query
rm -f   sg_zone

rm -f   sg_dd
rm -f   sginfo
rm -f   sgp_dd
rm -f   sgm_dd


