add_executable(test_reverb
  test_reverb.cpp
)

target_link_libraries(test_reverb PRIVATE rvrbotron_dsp)
target_compile_features(test_reverb PRIVATE cxx_std_17)

add_test(NAME empty_composition_identity COMMAND test_reverb)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME renderer_identity
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_renderer_cli.py
    $<TARGET_FILE:rvrbotron>
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio/impulse-mono-pcm16-48000.wav
    ${CMAKE_CURRENT_BINARY_DIR}/renderer-identity-result
)
