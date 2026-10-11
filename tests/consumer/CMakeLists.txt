# A minimal downstream project that consumes RVRBoTron's DSP core the way a
# plugin does (issue #161): as a FetchContent subproject, linking only the
# namespaced DSP and configuration targets. CI fetches the commit under
# test from its GitHub archive URL; locally, point
# FETCHCONTENT_SOURCE_DIR_RVRBOTRON at a checkout instead.
cmake_minimum_required(VERSION 3.25)

project(RVRBoTronConsumer LANGUAGES CXX)

# Most consumers have BUILD_TESTING on; RVRBoTron must not follow it.
include(CTest)

set(RVRBOTRON_URL "" CACHE STRING "Archive URL of the RVRBoTron revision to consume")
if(NOT RVRBOTRON_URL AND NOT FETCHCONTENT_SOURCE_DIR_RVRBOTRON)
  message(FATAL_ERROR
    "Set RVRBOTRON_URL or FETCHCONTENT_SOURCE_DIR_RVRBOTRON")
endif()

include(FetchContent)
FetchContent_Declare(
  rvrbotron
  URL
    ${RVRBOTRON_URL}
  DOWNLOAD_EXTRACT_TIMESTAMP
    TRUE
)
FetchContent_MakeAvailable(rvrbotron)

# RVRBoTron is the only dependency fetched, and it defines nothing but the
# DSP core and its typed configuration: no tool or test targets, and no
# subdirectories such as tests/ or a fetched JSON library.
foreach(dependency IN ITEMS dr_libs nlohmann_json)
  FetchContent_GetProperties(${dependency})
  if(${dependency}_POPULATED)
    message(FATAL_ERROR "RVRBoTron fetched ${dependency} as a subproject")
  endif()
endforeach()
get_property(rvrbotron_targets
  DIRECTORY ${rvrbotron_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
get_property(rvrbotron_subdirectories
  DIRECTORY ${rvrbotron_SOURCE_DIR} PROPERTY SUBDIRECTORIES)
if(NOT rvrbotron_targets STREQUAL "rvrbotron_dsp;rvrbotron_config"
    OR rvrbotron_subdirectories)
  message(FATAL_ERROR
    "RVRBoTron defined targets [${rvrbotron_targets}] and subdirectories "
    "[${rvrbotron_subdirectories}] as a subproject")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE rvrbotron::config rvrbotron::dsp)

add_test(NAME consumer_processes_one_block COMMAND consumer)
