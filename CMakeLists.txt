cmake_minimum_required(VERSION 2.8.12)
project(SBN_APP C)

# workaround until these definitions are exposed by cfe_sb
include_directories(${CFECORE_SOURCE_DIR}/src/sb)

include_directories(fsw/platform_inc)

aux_source_directory(fsw/src APP_SRC_FILES)

# Create the app module
add_cfe_app(sbn ${APP_SRC_FILES})

aux_source_directory(fsw/tables APP_TBL_FILES)
add_cfe_tables(sbn ${APP_TBL_FILES})

if (ENABLE_UNIT_TESTS)
  add_subdirectory(ut-stubs)
  add_subdirectory(unit-test)
endif (ENABLE_UNIT_TESTS)
