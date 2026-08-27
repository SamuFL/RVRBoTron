cmake_minimum_required(VERSION 3.25)

project(RVRBoTron VERSION 0.1.0 LANGUAGES CXX)

option(BUILD_TESTING "Build tests" ON)
option(RVRBOTRON_SAMPLE_DOUBLE "Use double-precision DSP samples" OFF)

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

set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
  nlohmann_json
  URL
    https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
  URL_HASH
    SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
  DOWNLOAD_EXTRACT_TIMESTAMP
    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(rvrbotron_dsp
  src/dsp/DiffusionStep.cpp
  src/dsp/Diffuser.cpp
  src/dsp/Downmix.cpp
  src/dsp/MixMatrix.cpp
  src/dsp/Reverb.cpp
  src/dsp/Split.cpp
  src/dsp/SplitStrategy.cpp
)

target_include_directories(rvrbotron_dsp
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(rvrbotron_dsp PUBLIC cxx_std_17)

if(RVRBOTRON_SAMPLE_DOUBLE)
  target_compile_definitions(rvrbotron_dsp PUBLIC RVRBOTRON_SAMPLE_DOUBLE=1)
endif()

add_library(rvrbotron_config
  src/config/MixMatrixResolution.cpp
  src/config/PositionalRandom.cpp
  src/config/ResolveConfig.cpp
  src/config/ResolvedConfigJson.cpp
)

target_include_directories(rvrbotron_config
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  rvrbotron_config
  PUBLIC
    rvrbotron_dsp
  PRIVATE
    nlohmann_json::nlohmann_json
)
target_compile_features(rvrbotron_config PUBLIC cxx_std_17)

add_library(rvrbotron_cli_support
  src/cli/Benchmark.cpp
  src/cli/ConfigJson.cpp
  src/cli/FileSha256.cpp
  src/cli/Platform.cpp
  src/cli/RenderMetadata.cpp
  src/cli/RenderResultTransaction.cpp
)

target_include_directories(rvrbotron_cli_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  rvrbotron_cli_support
  PUBLIC
    rvrbotron_config
  PRIVATE
    nlohmann_json::nlohmann_json
    $<$<PLATFORM_ID:Windows>:psapi>
)
target_compile_features(rvrbotron_cli_support PUBLIC cxx_std_17)
target_compile_definitions(
  rvrbotron_cli_support
  PRIVATE
    RVRBOTRON_VERSION="${PROJECT_VERSION}"
    "RVRBOTRON_BUILD_TYPE=\"$<CONFIG>\""
)

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

target_link_libraries(
  rvrbotron
  PRIVATE
    rvrbotron_cli_support
    rvrbotron_config
    rvrbotron_wav
    nlohmann_json::nlohmann_json
)
target_compile_features(rvrbotron PRIVATE cxx_std_17)

if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
