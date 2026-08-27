add_executable(test_reverb
  test_reverb.cpp
)

target_link_libraries(test_reverb PRIVATE rvrbotron_config)
target_compile_features(test_reverb PRIVATE cxx_std_17)

add_test(NAME empty_composition_identity COMMAND test_reverb)

add_executable(test_positional_random
  test_positional_random.cpp
)

target_link_libraries(test_positional_random PRIVATE rvrbotron_config)
target_compile_features(test_positional_random PRIVATE cxx_std_17)

add_test(NAME positional_random_vectors COMMAND test_positional_random)

add_executable(test_mix_matrix_resolution
  test_mix_matrix_resolution.cpp
)

target_link_libraries(test_mix_matrix_resolution PRIVATE rvrbotron_config)
target_compile_features(test_mix_matrix_resolution PRIVATE cxx_std_17)

add_test(NAME mix_matrix_resolution COMMAND test_mix_matrix_resolution)

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
  NAME reference_diffusion_render
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_reference_diffusion_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/reference-diffusion-render
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME feedback_loop_render
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_feedback_loop_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/feedback-loop-render
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME split_strategy_render
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_split_strategy_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_SOURCE_DIR}/tools/analyze_diffusion.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-right-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/split-strategy-render
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

add_test(
  NAME analyzer_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_analyzer_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_render.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/matrix/identity-mono-48000-float32.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/matrix/identity-stereo-48000-float32.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/matrix/identity-mono-48000-pcm16.wav
    ${CMAKE_CURRENT_BINARY_DIR}/analyzer-contract
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME diffusion_analyzer_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_diffusion_analyzer_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_diffusion.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/diffusion-analyzer-contract
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME diffusion_equivalence_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_diffusion_equivalence_cli.py
    ${CMAKE_SOURCE_DIR}/tools/extract_diffusion_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/compare_diffusion_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_diffusion.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/diffusion-equivalence-contract
)

add_test(
  NAME benchmark_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_benchmark_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/benchmark-contract
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME diffusion_catalog_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_diffusion_catalog_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_SOURCE_DIR}/tools/analyze_diffusion.py
    ${CMAKE_SOURCE_DIR}/tools/run_diffusion_catalog.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio
    ${CMAKE_CURRENT_BINARY_DIR}/diffusion-catalog-contract
)
