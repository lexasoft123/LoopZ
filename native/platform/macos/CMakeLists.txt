# Core Audio, XPC, Mach scheduling and installation tools stay in this backend.
add_library(loopz_macos_headers INTERFACE)
target_include_directories(loopz_macos_headers INTERFACE
  "${PROJECT_SOURCE_DIR}/native/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

function(loopz_macos_objc target)
  target_compile_options(${target} PRIVATE -fblocks -fobjc-arc)
  loopz_strict_target(${target})
endfunction()

add_executable(loopz-callback-timing-probe ../../tools/callback_timing_probe.mm)
loopz_macos_objc(loopz-callback-timing-probe)
target_link_libraries(loopz-callback-timing-probe PRIVATE loopz_macos_headers
  "-framework Foundation" "-framework CoreAudio")

add_library(loopz_device_transport STATIC device_transport.cpp transport_pool.cpp)
loopz_strict_target(loopz_device_transport)
target_link_libraries(loopz_device_transport PUBLIC LoopZ::Core loopz_macos_headers)

add_library(loopz_transport_message STATIC transport_message.mm)
loopz_macos_objc(loopz_transport_message)
target_link_libraries(loopz_transport_message PUBLIC loopz_device_transport "-framework Foundation")

add_library(loopz_route_store STATIC route_json.mm route_store.cpp)
loopz_macos_objc(loopz_route_store)
target_link_libraries(loopz_route_store PUBLIC LoopZ::Core loopz_macos_headers
  "-framework Foundation")

add_library(loopz_configuration_controller STATIC configuration_controller.cpp driver_configuration.cpp)
loopz_strict_target(loopz_configuration_controller)
target_link_libraries(loopz_configuration_controller PUBLIC loopz_route_store loopz_device_transport)

add_library(loopz_device_inventory STATIC device_inventory.mm)
loopz_macos_objc(loopz_device_inventory)
target_link_libraries(loopz_device_inventory PUBLIC loopz_macos_headers SingZ::zcore_device
  "-framework Foundation" "-framework CoreAudio")

add_library(loopz_capture STATIC capture.mm capture_layout.mm application_routes.mm
  application_ownership.mm permissions.mm)
loopz_macos_objc(loopz_capture)
target_link_libraries(loopz_capture PUBLIC LoopZ::Core loopz_macos_headers SingZ::zcore_device
  "-framework Foundation" "-framework AppKit" "-framework AVFoundation" "-framework CoreAudio" "-framework AudioToolbox")

add_library(loopz_capture_sources STATIC capture_sources.mm)
loopz_macos_objc(loopz_capture_sources)
target_link_libraries(loopz_capture_sources PUBLIC loopz_capture)

add_library(loopz_scene_renderer STATIC scene_renderer.cpp public_timing.mm public_timing_observer.mm input_timing.cpp)
set_source_files_properties(public_timing.mm public_timing_observer.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-fblocks")
loopz_strict_target(loopz_scene_renderer)
target_link_libraries(loopz_scene_renderer PUBLIC loopz_configuration_controller)
target_link_libraries(loopz_scene_renderer PUBLIC "-framework Foundation" "-framework CoreAudio")

add_library(loopz_monitor_outputs STATIC monitor_outputs.cpp)
loopz_strict_target(loopz_monitor_outputs)
target_link_libraries(loopz_monitor_outputs PUBLIC LoopZ::Core loopz_macos_headers SingZ::zcore_device loopz_capture)
add_executable(loopz-monitor-probe ../../tools/monitor_probe.mm)
loopz_macos_objc(loopz-monitor-probe)
target_link_libraries(loopz-monitor-probe PRIVATE loopz_monitor_outputs "-framework Foundation")
add_executable(loopz-acoustic-latency-probe ../../tools/acoustic_latency_probe.mm)
loopz_macos_objc(loopz-acoustic-latency-probe)
target_link_libraries(loopz-acoustic-latency-probe PRIVATE SingZ::zcore_device
  "-framework Foundation" "-framework AVFoundation")
add_executable(loopz-recording-latency-probe ../../tools/recording_latency_probe.mm)
loopz_macos_objc(loopz-recording-latency-probe)
target_link_libraries(loopz-recording-latency-probe PRIVATE SingZ::zcore_device
  "-framework Foundation" "-framework AVFoundation" "-framework AudioUnit" "-framework CoreAudio")

add_library(loopz_engine_runtime STATIC render_thread.cpp engine_runtime.cpp)
loopz_strict_target(loopz_engine_runtime)
target_link_libraries(loopz_engine_runtime PUBLIC loopz_scene_renderer loopz_capture_sources loopz_monitor_outputs)
add_library(loopz_device_settings STATIC device_settings.mm)
loopz_macos_objc(loopz_device_settings)
target_link_libraries(loopz_device_settings PUBLIC loopz_device_inventory loopz_route_store)
target_link_libraries(loopz_engine_runtime PUBLIC loopz_device_settings)
add_executable(loopz-input-delivery-probe ../../tools/input_delivery_probe.mm)
loopz_macos_objc(loopz-input-delivery-probe)
target_link_libraries(loopz-input-delivery-probe PRIVATE loopz_editor_service loopz_engine_runtime "-framework Foundation")
add_executable(loopz-physical-path-probe ../../tools/physical_path_probe.mm)
loopz_macos_objc(loopz-physical-path-probe)
target_link_libraries(loopz-physical-path-probe PRIVATE loopz_engine_runtime "-framework Foundation")

add_library(loopz_routing_service STATIC routing_service/connection.mm
  routing_service/broker.mm routing_service/client.mm routing_service/installed_endpoint.mm routing_service/installed_link.mm)
loopz_macos_objc(loopz_routing_service)
target_link_libraries(loopz_routing_service PUBLIC loopz_configuration_controller loopz_transport_message
  "-framework Foundation" "-framework Security")

add_library(loopz_routing_runtime_cli STATIC engine/routing_main.mm)
add_library(loopz_recovery_events STATIC recovery_events.mm)
loopz_macos_objc(loopz_recovery_events)
target_link_libraries(loopz_recovery_events PUBLIC loopz_macos_headers
  "-framework AppKit" "-framework CoreAudio")
add_executable(loopz-input-device-recovery-probe ../../tools/input_device_recovery_probe.mm)
loopz_macos_objc(loopz-input-device-recovery-probe)
target_link_libraries(loopz-input-device-recovery-probe PRIVATE loopz_editor_service loopz_engine_runtime loopz_recovery_events "-framework Foundation" "-framework CoreAudio")
loopz_macos_objc(loopz_routing_runtime_cli)
target_link_libraries(loopz_routing_runtime_cli PUBLIC loopz_engine_runtime loopz_routing_service loopz_recovery_events)

add_executable(loopz-routing-broker routing_service/main.mm)
loopz_macos_objc(loopz-routing-broker)
target_link_libraries(loopz-routing-broker PRIVATE loopz_routing_service "-framework SystemConfiguration")

add_executable(loopz-routing-pin-check ../../tools/routing_pin_check.mm)
loopz_macos_objc(loopz-routing-pin-check)
target_link_libraries(loopz-routing-pin-check PRIVATE loopz_routing_service)

add_executable(loopz-config config/main.mm)
loopz_macos_objc(loopz-config)
target_link_libraries(loopz-config PRIVATE loopz_route_store)

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

# Separately named while native lifecycle and broker integration are validated.
# It is deliberately absent from the current installer component list.
add_library(LoopZRoutingAudio MODULE driver/routing/state.mm driver/routing/properties.cpp
  driver/routing/interface.mm driver/routing/transactions.mm driver/routing/service.mm)
loopz_macos_objc(LoopZRoutingAudio)
target_link_libraries(LoopZRoutingAudio PRIVATE loopz_routing_service
  "-framework CoreAudio" "-framework CoreFoundation")
set_target_properties(LoopZRoutingAudio PROPERTIES
  BUNDLE TRUE BUNDLE_EXTENSION driver PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/routing-driver.plist")

# An explicit migration candidate for the already-discovered legacy HAL slot.
# Building it never replaces the installed driver or changes the normal bundle.
add_library(LoopZRoutingMigration MODULE driver/routing/state.mm driver/routing/properties.cpp
  driver/routing/interface.mm driver/routing/transactions.mm driver/routing/service.mm)
loopz_macos_objc(LoopZRoutingMigration)
target_compile_definitions(LoopZRoutingMigration PRIVATE LOOPZ_LEGACY_DRIVER_SLOT=1)
target_link_libraries(LoopZRoutingMigration PRIVATE loopz_routing_service
  "-framework CoreAudio" "-framework CoreFoundation")
set_target_properties(LoopZRoutingMigration PROPERTIES
  BUNDLE TRUE BUNDLE_EXTENSION driver PREFIX "" OUTPUT_NAME "LoopZAudio"
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/routing-migration"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/routing-migration-driver.plist")

add_library(loopz_virtual_input_probe STATIC probe/main.mm probe/multi_output_source.mm)
loopz_macos_objc(loopz_virtual_input_probe)
target_compile_definitions(loopz_virtual_input_probe PRIVATE LOOPZ_EMBEDDED_PROBE=1)
target_link_libraries(loopz_virtual_input_probe PRIVATE loopz_capture SingZ::zcore_device)

add_executable(loopz-engine MACOSX_BUNDLE engine/main.mm engine/connections.mm bridge_status.mm
  probe/routing_io_check.mm probe/routing_recovery_check.mm probe/routing_signal_check.mm
  probe/routing_timing_check.mm)
loopz_macos_objc(loopz-engine)
target_link_libraries(loopz-engine PRIVATE LoopZ::Core loopz_bridge loopz_capture loopz_routing_runtime_cli loopz_virtual_input_probe SingZ::zcore_device
  "-framework Foundation" "-framework AppKit" "-framework AVFoundation" "-framework CoreAudio")
set_target_properties(loopz-engine PROPERTIES OUTPUT_NAME "LoopZ Engine"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/engine.plist")

add_executable(loopz-probe probe/main.mm probe/multi_output_source.mm)
loopz_macos_objc(loopz-probe)
target_link_libraries(loopz-probe PRIVATE LoopZ::Core loopz_macos_headers SingZ::zcore_device
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")

add_executable(loopz-test-source MACOSX_BUNDLE probe/main.mm probe/multi_output_source.mm)
loopz_macos_objc(loopz-test-source)
target_link_libraries(loopz-test-source PRIVATE LoopZ::Core loopz_macos_headers SingZ::zcore_device
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")
set_target_properties(loopz-test-source PROPERTIES OUTPUT_NAME "LoopZ Test Source"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/test-source.plist")

add_executable(loopz-install-check install/check.mm)
loopz_macos_objc(loopz-install-check)
target_link_libraries(loopz-install-check PRIVATE loopz_macos_headers
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")
add_executable(loopz-routing-install-check install/check.mm)
loopz_macos_objc(loopz-routing-install-check)
target_link_libraries(loopz-routing-install-check PRIVATE loopz_macos_headers
  "-framework Foundation" "-framework CoreAudio" "-framework AudioToolbox")
