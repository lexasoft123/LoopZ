# Core Audio, XPC, Mach scheduling and installation tools stay in this backend.
add_library(loopz_macos_headers INTERFACE)
target_include_directories(loopz_macos_headers INTERFACE
  "${PROJECT_SOURCE_DIR}/native/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

function(loopz_macos_objc target)
  target_compile_options(${target} PRIVATE -fblocks -fobjc-arc)
  loopz_strict_target(${target})
endfunction()

add_library(loopz_bridge STATIC bridge_client.mm)
loopz_macos_objc(loopz_bridge)
target_link_libraries(loopz_bridge PUBLIC loopz_macos_headers "-framework Foundation")

add_executable(loopz-broker broker/main.mm)
loopz_macos_objc(loopz-broker)
target_link_libraries(loopz-broker PRIVATE loopz_macos_headers
  "-framework Foundation" "-framework SystemConfiguration")

add_library(LoopZAudio MODULE driver/LoopZAudio.c driver/driver_bridge.cpp)
target_include_directories(LoopZAudio PRIVATE driver)
target_compile_options(LoopZAudio PRIVATE -fblocks)
target_link_libraries(LoopZAudio PRIVATE loopz_bridge
  "-framework CoreAudio" "-framework CoreFoundation")
set_target_properties(LoopZAudio PROPERTIES
  BUNDLE TRUE BUNDLE_EXTENSION driver PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/driver.plist")

add_executable(loopz-engine MACOSX_BUNDLE engine/main.mm capture.mm bridge_status.mm)
loopz_macos_objc(loopz-engine)
target_link_libraries(loopz-engine PRIVATE LoopZ::Core loopz_bridge SingZ::zcore_device
  "-framework Foundation" "-framework AppKit" "-framework AVFoundation" "-framework CoreAudio")
set_target_properties(loopz-engine PROPERTIES OUTPUT_NAME "LoopZ Engine"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/engine.plist")

add_executable(loopz-probe probe/main.mm)
loopz_macos_objc(loopz-probe)
target_link_libraries(loopz-probe PRIVATE LoopZ::Core loopz_macos_headers
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")

add_executable(loopz-test-source MACOSX_BUNDLE probe/main.mm)
loopz_macos_objc(loopz-test-source)
target_link_libraries(loopz-test-source PRIVATE loopz_macos_headers
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")
set_target_properties(loopz-test-source PROPERTIES OUTPUT_NAME "LoopZ Test Source"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/test-source.plist")

add_executable(loopz-install-check install/check.mm)
loopz_macos_objc(loopz-install-check)
target_link_libraries(loopz-install-check PRIVATE loopz_macos_headers
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")
