cmake_minimum_required(VERSION 2.8.12)
project(SBN_F_REMAP C)

if(NOT(IS_DIRECTORY ${SBN_APP_SOURCE_DIR}))
    message(FATAL_ERROR "SBN_APP_SOURCE_DIR not defined, is sbn in the target list before this module?")
endif()

include_directories(fsw/platform_inc)

include_directories(${SBN_APP_SOURCE_DIR}/fsw/platform_inc)

aux_source_directory(fsw/src LIB_SRC_FILES)

aux_source_directory(fsw/tables APP_TBL_FILES)
add_cfe_tables(sbn_f_remap ${APP_TBL_FILES})

# Create the app module
add_cfe_app(sbn_f_remap ${LIB_SRC_FILES})
