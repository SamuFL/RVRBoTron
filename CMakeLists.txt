cmake_minimum_required(VERSION 3.25)

project(RVRBoTron VERSION 0.1.0 LANGUAGES CXX)

option(BUILD_TESTING "Build tests" ON)

include(FetchContent)
FetchContent_Declare(
  dr_libs
  URL
    https://github.com/mackron/dr_libs/archive/50bb723e6a459dbb781e26cefee4fd9ca6714d6a.tar.gz
  URL_HASH
    SHA256=ebf2a141056247b4c022c513adcb162ac61c43ba3fc2f7b60153db819b5b9bad
  DOWNLOAD_EXTRACT_TIMESTAMP
    TRUE
)
FetchContent_MakeAvailable(dr_libs)

add_library(rvrbotron_dsp
  src/dsp/Composition.cpp
)

target_include_directories(rvrbotron_dsp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(rvrbotron_dsp PUBLIC cxx_std_17)

add_library(rvrbotron_wav
  src/io/WavStream.cpp
)

target_include_directories(rvrbotron_wav
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${dr_libs_SOURCE_DIR}
)

target_link_libraries(rvrbotron_wav PUBLIC rvrbotron_dsp)
target_compile_features(rvrbotron_wav PUBLIC cxx_std_17)

add_executable(rvrbotron
  src/cli/main.cpp
)

target_link_libraries(rvrbotron PRIVATE rvrbotron_wav)
target_compile_features(rvrbotron PRIVATE cxx_std_17)
target_compile_definitions(
  rvrbotron
  PRIVATE
    RVRBOTRON_VERSION="${PROJECT_VERSION}"
)

if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
