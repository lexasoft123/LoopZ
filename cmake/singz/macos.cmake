include("${CMAKE_CURRENT_LIST_DIR}/device_callback.cmake")
add_library(zcore_device STATIC
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_transport.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input_wake.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_fifo.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_unsupported.cpp"
  "${SINGZ_ROOT}/zcore/platform/macos/audio_host_macos.cpp"
  "${SINGZ_ROOT}/zcore/platform/macos/audio_input_macos.cpp"
  "${SINGZ_ROOT}/zcore/platform/macos/audio_input_wake_macos.cpp"
  "${SINGZ_ROOT}/zcore/platform/ios/audio_input_ios_session.cpp")
add_library(SingZ::zcore_device ALIAS zcore_device)
target_link_libraries(zcore_device PUBLIC zcore_device_callback Threads::Threads
  "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation")
target_compile_options(zcore_device PRIVATE -fblocks)
set_target_properties(zcore_device PROPERTIES POSITION_INDEPENDENT_CODE YES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
