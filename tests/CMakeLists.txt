add_executable(test_composition
  test_composition.cpp
)

target_link_libraries(test_composition PRIVATE rvrbotron_dsp)
target_compile_features(test_composition PRIVATE cxx_std_17)

add_test(NAME composition_identity COMMAND test_composition)

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
