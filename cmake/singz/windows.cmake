include("${CMAKE_CURRENT_LIST_DIR}/device_callback.cmake")
add_library(zcore_device STATIC
  "${SINGZ_ROOT}/zcore/src/audio/audio_input_transport.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_input_wake.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_fifo.cpp"
  "${SINGZ_ROOT}/zcore/src/device/audio_host_unsupported.cpp"
  "${SINGZ_ROOT}/zcore/platform/ios/audio_input_ios_session.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_host_windows.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_host_windows_helpers.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_host_windows_provider.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_host_asio.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_input_windows.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_input_windows_helpers.cpp"
  "${SINGZ_ROOT}/zcore/platform/windows/audio_input_wake_windows.cpp")
add_library(SingZ::zcore_device ALIAS zcore_device)
target_compile_definitions(zcore_device PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(zcore_device PUBLIC zcore_device_callback Threads::Threads avrt ole32 uuid)
