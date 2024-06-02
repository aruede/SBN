###########################################################
#
# SBN platform build setup
#
# This file is evaluated as part of the "prepare" stage
# and can be used to set up prerequisites for the build,
# such as generating header files
#
###########################################################

# The list of header files that control the SBN configuration
set(SBN_PLATFORM_CONFIG_FILE_LIST
  sbn_platform_cfg.h
  sbn_perfids.h
  sbn_msgids.h
)

# Create wrappers around the all the config header files
# This makes them individually overridable by the missions, without modifying
# the distribution default copies
foreach(SBN_CFGFILE ${SBN_PLATFORM_CONFIG_FILE_LIST})
  get_filename_component(CFGKEY "${SBN_CFGFILE}" NAME_WE)
  if (DEFINED SBN_CFGFILE_SRC_${CFGKEY})
    set(DEFAULT_SOURCE GENERATED_FILE "${SBN_CFGFILE_SRC_${CFGKEY}}")
  else()
    set(DEFAULT_SOURCE FALLBACK_FILE "${CMAKE_CURRENT_LIST_DIR}/fsw/platform_inc/${SBN_CFGFILE}")
  endif()
  generate_config_includefile(
    FILE_NAME           "${SBN_CFGFILE}"
    ${DEFAULT_SOURCE}
  )
endforeach()
