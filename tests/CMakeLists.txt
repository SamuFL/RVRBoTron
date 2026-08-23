add_executable(test_reverb
  test_reverb.cpp
)

target_link_libraries(test_reverb PRIVATE rvrbotron_dsp)
target_compile_features(test_reverb PRIVATE cxx_std_17)

add_test(NAME empty_composition_identity COMMAND test_reverb)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(RVRBOTRON_SAMPLE_DOUBLE)
  set(RVRBOTRON_TEST_SAMPLE_BITS 64)
else()
  set(RVRBOTRON_TEST_SAMPLE_BITS 32)
endif()

add_test(
  NAME renderer_identity
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_renderer_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/renderer-identity-result
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME configuration_round_trip
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_configuration_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/configuration-round-trip
)

add_test(
  NAME wav_compatibility
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_wav_compatibility.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/matrix
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/generate_wav_matrix.py
    ${CMAKE_CURRENT_BINARY_DIR}/wav-compatibility
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME render_result_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_render_result_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/matrix/identity-mono-48000-float32.wav
    ${CMAKE_CURRENT_BINARY_DIR}/render-result-contract
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)
