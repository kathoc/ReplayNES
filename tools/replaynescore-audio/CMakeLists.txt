add_executable(replaynescore-audio-capture main.cpp)
target_link_libraries(replaynescore-audio-capture PRIVATE replaynes_engine)
target_include_directories(replaynescore-audio-capture PRIVATE ${PROJECT_SOURCE_DIR}/engine/src)
target_compile_features(replaynescore-audio-capture PRIVATE cxx_std_20)
