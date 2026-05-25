
MESSAGE( STATUS ">> Entering lib/CMakeLists.txt" )

# Select Hardware Abstraction Layer (HAL)
if ( PT_DUMMY )
    set ( sgutils2_lib_hal
	sg_pt_dummy.c
    )
    MESSAGE( STATUS "<<>> PT_DUMMY configured, no HAL")
elseif ( SG_LIB_LINUX )
    set ( sgutils2_lib_hal
	sg_io_linux.c
	sg_pt_linux.c
	sg_pt_linux_nvme.c
    )
elseif ( SG_LIB_FREEBSD )
    set ( sgutils2_lib_hal
	sg_pt_freebsd.c
    )
elseif ( SG_LIB_WIN32 )
    set ( sgutils2_lib_hal
	sg_pt_win32.c
    )
elseif ( SG_LIB_HAIKU )
    set ( sgutils2_lib_hal
	sg_pt_haiku.c
    )
elseif ( SG_LIB_NETBSD )
    set ( sgutils2_lib_hal
	sg_pt_netbsd.c
    )
elseif ( SG_LIB_SOLARIS )
    set ( sgutils2_lib_hal
	sg_pt_solaris.c
    )
else ( )
    MESSAGE( STATUS "<<>> PT_DUMMY set due to fall through, no HAL")
    set ( sgutils2_lib_hal
	sg_pt_dummy.c
    )
endif ( PT_DUMMY )

set ( sgutils2_lib_common
	sg_cmds_basic.c
	sg_cmds_basic2.c
	sg_cmds_extra.c
	sg_cmds_mmc.c
	sg_json.c
	sg_json_builder.c
	sg_json_sg_lib.c
	sg_lib.c
	sg_lib_data.c
	sg_lib_names.c
	sg_pr2serr.c
	sg_pt_common.c
	sg_snt.c
)

if ( BUILD_SHARED_LIBS )
    MESSAGE( STATUS ">> Build shared library(s) (default)" )
    add_library(sgutils2 SHARED ${sgutils2_lib_common} ${sgutils2_lib_hal} )
    set_target_properties(sgutils2 PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
else ( BUILD_SHARED_LIBS )
    MESSAGE( STATUS ">> Build a static library" )
    add_definitions ( -static )
    add_library(sgutils2 STATIC ${sgutils2_lib_common} ${sgutils2_lib_hal} )
endif ( BUILD_SHARED_LIBS )

if ( ENABLE_DEBUG )
    MESSAGE( STATUS ">> Compile lib/ with DEBUG set" )

    target_compile_definitions(sgutils2 PRIVATE DEBUG)
    target_compile_options(sgutils2 PRIVATE
        -Wextra -Wmisleading-indentation -Wduplicated-cond -Wlogical-op
        -Wnull-dereference -Wshadow -Wunused -Wsizeof-array-argument
        -Wduplicated-branches -Wjump-misses-init -Wparentheses)
endif ( ENABLE_DEBUG )


target_include_directories(sgutils2
    PUBLIC
	$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

target_include_directories(sgutils2 PRIVATE ${CMAKE_BINARY_DIR})

# cmake in win32 doesn't like the following
# target_compile_features(sgutils2 PUBLIC c_std_99)

set_target_properties(sgutils2 PROPERTIES
	VERSION ${PROJECT_VERSION}
	SOVERSION 1
)

#
# Install runtime components
#

install(TARGETS sgutils2
    EXPORT sgutils2Targets

    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT runtime

    NAMELINK_COMPONENT development
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/sgutils2ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

