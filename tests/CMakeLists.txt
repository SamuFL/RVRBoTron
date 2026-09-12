add_executable(test_reverb
  test_reverb.cpp
)

target_link_libraries(test_reverb PRIVATE rvrbotron_config)
target_compile_features(test_reverb PRIVATE cxx_std_17)

add_test(NAME empty_composition_identity COMMAND test_reverb)

add_executable(test_positional_random
  test_positional_random.cpp
)

target_link_libraries(test_positional_random PRIVATE rvrbotron_dsp)
target_compile_features(test_positional_random PRIVATE cxx_std_17)

add_test(NAME positional_random_vectors COMMAND test_positional_random)

add_executable(test_mix_matrix_resolution
  test_mix_matrix_resolution.cpp
)

target_link_libraries(test_mix_matrix_resolution PRIVATE rvrbotron_config)
target_compile_features(test_mix_matrix_resolution PRIVATE cxx_std_17)

add_test(NAME mix_matrix_resolution COMMAND test_mix_matrix_resolution)

# A stand-in renderer for the Research bench hardening contract (issue
# #139): a real compiled executable, not a script, because serve.py
# launches --renderer directly and Windows' CreateProcess cannot run a
# script through its shebang line the way a POSIX exec can.
add_executable(fake_renderer
  fixtures/fake_renderer.cpp
)

target_link_libraries(fake_renderer PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(fake_renderer PRIVATE cxx_std_17)

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
  NAME modulation_render
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_modulation_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/modulation-render
)

add_test(
  NAME diffusion_step_modulation_render
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_diffusion_step_modulation_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/diffusion-step-modulation-render
)

add_test(
  NAME diffuser_loop_render
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_diffuser_loop_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/diffuser-loop-render
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
  NAME wet_branch_capture_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_wet_branch_capture_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/wet-branch-capture-contract
    ${RVRBOTRON_TEST_SAMPLE_BITS}
)

add_test(
  NAME early_support_analyzer_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_early_support_analyzer_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_early_support.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/early-support-analyzer-contract
)

add_test(
  NAME downmix_analyzer_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_downmix_analyzer_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_downmix.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/downmix-analyzer-contract
)

add_test(
  NAME tail_analyzer_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tail_analyzer_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_diffusion.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/tail-analyzer-contract
)

add_test(
  NAME tail_equivalence_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tail_equivalence_cli.py
    ${CMAKE_SOURCE_DIR}/tools/extract_tail_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/compare_tail_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/tail-equivalence-contract
)

add_test(
  NAME tail_analyzer_v2_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tail_analyzer_v2_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail_v2.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/tail-analyzer-v2-contract
)

add_test(
  NAME tail_v2_equivalence_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tail_v2_equivalence_cli.py
    ${CMAKE_SOURCE_DIR}/tools/extract_tail_v2_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/compare_tail_v2_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail_v2.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/tail-v2-equivalence-contract
)

add_test(
  NAME modulation_analyzer_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_modulation_analyzer_cli.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail_v2.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_modulation.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/modulation-analyzer-contract
)

add_test(
  NAME modulation_equivalence_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_modulation_equivalence_cli.py
    ${CMAKE_SOURCE_DIR}/tools/extract_modulation_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/compare_modulation_equivalence.py
    ${CMAKE_SOURCE_DIR}/tools/analyze_modulation.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/modulation-equivalence-contract
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

add_test(
  NAME tail_sweep_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_tail_sweep_cli.py
    ${CMAKE_SOURCE_DIR}/tools/run_tail_sweep.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/tail-sweep-contract
)

add_test(
  NAME damping_sweep_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_damping_sweep_cli.py
    ${CMAKE_SOURCE_DIR}/tools/run_damping_sweep.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_SOURCE_DIR}/tools/analyze_tail_v2.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/damping-sweep-contract
)

add_test(
  NAME modulation_sweep_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_modulation_sweep_cli.py
    ${CMAKE_SOURCE_DIR}/tools/run_modulation_sweep.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_SOURCE_DIR}/tools/analyze_modulation.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/modulation-sweep-contract
)

add_test(
  NAME spatial_sweep_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_spatial_sweep_cli.py
    ${CMAKE_SOURCE_DIR}/tools/run_spatial_sweep.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_SOURCE_DIR}/tools/analyze_downmix.py
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/spatial-sweep-contract
)

add_test(
  NAME allpass_evaluation_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_allpass_evaluation_cli.py
    ${CMAKE_SOURCE_DIR}/tools/evaluate_allpass_interpolation.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/allpass-evaluation-contract
)

add_test(
  NAME render_folder_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_render_folder_cli.py
    ${CMAKE_SOURCE_DIR}/tools/render_folder.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/render-folder-contract
)

add_test(
  NAME research_bench_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_research_bench_cli.py
    ${CMAKE_SOURCE_DIR}/tools/research_bench/serve.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/research-bench-contract
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/matrix
    ${CMAKE_SOURCE_DIR}/tools/research_bench/static/vendor/ace/VENDORING.md
    ${RVRBOTRON_TEST_SAMPLE_BITS}
    ${CMAKE_SOURCE_DIR}/tools/research_bench/static/templates
)

# The test drives a live server it starts itself. If the launcher ever hangs
# before printing its banner, bound the wait here rather than at CTest's
# 25-minute default.
set_tests_properties(research_bench_contract PROPERTIES TIMEOUT 180)

add_test(
  NAME research_bench_hardening_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_research_bench_hardening_cli.py
    ${CMAKE_SOURCE_DIR}/tools/research_bench/serve.py
    $<TARGET_FILE:fake_renderer>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/research-bench-hardening-contract
)

# Several cases drive a deliberately slow fake render, and the suite
# launches its own bench server seven separate times -- each paying full
# process-spawn and startup-probe cost. That cost is small locally but far
# from it on CI: research_bench_contract's own process-spawn overhead
# alone went from well under a second locally to 35+ seconds on a loaded
# macOS runner. Budget generously rather than at CTest's 25-minute default.
set_tests_properties(research_bench_hardening_contract PROPERTIES TIMEOUT 450)

add_test(
  NAME research_bench_templates_contract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_research_bench_templates_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_SOURCE_DIR}/tools/research_bench/static/templates
    ${CMAKE_CURRENT_BINARY_DIR}/research-bench-templates-contract
    ${CMAKE_SOURCE_DIR}/src/cli/ConfigJson.cpp
)
