add_executable(loopz-route-edit-tests route_edit_tests.cpp)
target_link_libraries(loopz-route-edit-tests PRIVATE LoopZ::Core)
loopz_test_target(loopz-route-edit-tests)
add_test(NAME loopz-route-edits COMMAND loopz-route-edit-tests)
if(LOOPZ_BUILD_MACOS_BACKEND)
  add_test(NAME loopz-distribution-signing COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/distribution_signing_tests.py")
  add_test(NAME loopz-notarize-release COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/notarize_release_tests.py")
  add_executable(loopz-installation-setup-tests installation_setup_tests.mm)
  loopz_macos_objc(loopz-installation-setup-tests)
  loopz_test_target(loopz-installation-setup-tests)
  target_link_libraries(loopz-installation-setup-tests PRIVATE loopz_desktop_setup)
  add_test(NAME loopz-installation-setup COMMAND loopz-installation-setup-tests)
  add_test(NAME loopz-driver-runtime COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/driver_runtime_tests.py")
  set_tests_properties(loopz-driver-runtime PROPERTIES TIMEOUT 30)
  add_executable(loopz-driver-host-fixture driver_host_fixture.cpp)
  loopz_test_target(loopz-driver-host-fixture)
  foreach(version IN ITEMS 1 2)
    add_library(loopz-driver-image-${version} SHARED driver_image_fixture.cpp)
    target_compile_definitions(loopz-driver-image-${version} PRIVATE LOOPZ_FIXTURE_VERSION=${version})
    loopz_test_target(loopz-driver-image-${version})
  endforeach()
  add_test(NAME loopz-broker-host COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/broker_host_tests.py")
  set_tests_properties(loopz-broker-host PROPERTIES TIMEOUT 30)
  add_test(NAME loopz-suite-transaction COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/suite_transaction_tests.py")
  set_tests_properties(loopz-suite-transaction PROPERTIES TIMEOUT 120)
  add_test(NAME loopz-suite-files COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/suite_files_tests.py")
  set_tests_properties(loopz-suite-files PROPERTIES TIMEOUT 90)
  add_test(NAME loopz-suite-update COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/suite_update_tests.py")
  set_tests_properties(loopz-suite-update PROPERTIES TIMEOUT 30)
  add_test(NAME loopz-uninstall COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/uninstall_tests.py")
  set_tests_properties(loopz-uninstall PROPERTIES TIMEOUT 30)
  add_test(NAME loopz-uninstall-journal COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/uninstall_journal_tests.py")
  set_tests_properties(loopz-uninstall-journal PROPERTIES TIMEOUT 120)
  add_test(NAME loopz-uninstall-sessions COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/uninstall_session_tests.py")
  set_tests_properties(loopz-uninstall-sessions PROPERTIES TIMEOUT 30)
  add_test(NAME loopz-uninstall-recovery COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/uninstall_recovery_tests.py")
  set_tests_properties(loopz-uninstall-recovery PROPERTIES TIMEOUT 45)
  add_test(NAME loopz-desktop-install COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/desktop_install_tests.py")
  add_test(NAME loopz-continuity-guard COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/continuity_guard_tests.py")
  add_test(NAME loopz-driver-update COMMAND /usr/bin/python3 -E -s -S
    "${PROJECT_SOURCE_DIR}/tests/driver_update_tests.py")
  add_test(NAME loopz-routing-timing-check COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tests/platform/macos/routing_timing_check_tests.py"
    "$<TARGET_FILE:loopz-engine>")
  add_executable(loopz-session-tests session_tests.cpp)
  loopz_test_target(loopz-session-tests)
  target_link_libraries(loopz-session-tests PRIVATE loopz_desktop_session)
  add_test(NAME loopz-session COMMAND loopz-session-tests)
  set_tests_properties(loopz-session PROPERTIES TIMEOUT 15)
  add_executable(loopz-microphone-setup-tests microphone_setup_tests.mm)
  loopz_macos_objc(loopz-microphone-setup-tests)
  loopz_test_target(loopz-microphone-setup-tests)
  target_link_libraries(loopz-microphone-setup-tests PRIVATE loopz_desktop_permissions Threads::Threads)
  add_test(NAME loopz-microphone-setup COMMAND loopz-microphone-setup-tests)
  set_tests_properties(loopz-microphone-setup PROPERTIES TIMEOUT 15)
  add_executable(loopz-session-fixture session_fixture.cpp)
  loopz_test_target(loopz-session-fixture)
  add_executable(loopz-session-host-runner session_host_runner.mm)
  loopz_macos_objc(loopz-session-host-runner)
  loopz_test_target(loopz-session-host-runner)
  target_link_libraries(loopz-session-host-runner PRIVATE loopz_desktop_session)
  add_test(NAME loopz-session-process COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/session_process_tests.py"
    "$<TARGET_FILE:loopz-session-host-runner>" "$<TARGET_FILE:loopz-session-fixture>")
  set_tests_properties(loopz-session-process PROPERTIES TIMEOUT 90)
  add_executable(loopz-editor-initialization-tests editor_initialization_tests.mm)
  loopz_macos_objc(loopz-editor-initialization-tests)
  loopz_test_target(loopz-editor-initialization-tests)
  target_link_libraries(loopz-editor-initialization-tests PRIVATE loopz_engine_runtime Threads::Threads)
  add_test(NAME loopz-editor-initialization COMMAND loopz-editor-initialization-tests)
  add_executable(loopz-desktop-resource-tests desktop_resource_tests.mm ../../native/platform/macos/desktop/resources.mm)
  loopz_macos_objc(loopz-desktop-resource-tests)
  loopz_test_target(loopz-desktop-resource-tests)
  target_link_libraries(loopz-desktop-resource-tests PRIVATE loopz_editor_service "-framework WebKit" "-framework Security")
  add_test(NAME loopz-desktop-resources COMMAND loopz-desktop-resource-tests)
  # Explicit acceptance runner: requires a window server, private directory and
  # generated scenarios, so it is not part of the headless CTest suite.
  add_executable(loopz-webkit-engine-tests webkit_engine_tests.mm
    ../../native/platform/macos/desktop/editing_actions.mm
    ../../native/platform/macos/desktop/audio_maintenance_client.mm
    ../../native/platform/macos/desktop/bridge.mm
    ../../native/platform/macos/desktop/release_updates.mm
    ../../native/platform/macos/desktop/resources.mm
    ../../native/platform/macos/desktop/login.mm)
  loopz_macos_objc(loopz-webkit-engine-tests)
  loopz_test_target(loopz-webkit-engine-tests)
  target_link_libraries(loopz-webkit-engine-tests PRIVATE loopz_editor_service
    loopz_desktop_permissions loopz_desktop_setup "-framework WebKit" "-framework Security"
    "-framework UniformTypeIdentifiers" "-framework ServiceManagement" "-framework CoreServices")
  add_executable(loopz-desktop-inventory-tests desktop_inventory_tests.mm)
  loopz_macos_objc(loopz-desktop-inventory-tests)
  loopz_test_target(loopz-desktop-inventory-tests)
  target_link_libraries(loopz-desktop-inventory-tests PRIVATE loopz_device_inventory)
  add_test(NAME loopz-desktop-inventory COMMAND loopz-desktop-inventory-tests)
  add_executable(loopz-device-settings-tests device_settings_tests.mm)
  loopz_macos_objc(loopz-device-settings-tests)
  loopz_test_target(loopz-device-settings-tests)
  target_link_libraries(loopz-device-settings-tests PRIVATE loopz_device_settings)
  add_test(NAME loopz-device-settings COMMAND loopz-device-settings-tests)
  add_test(NAME loopz-desktop-signature COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/desktop_signature_tests.py" "$<TARGET_BUNDLE_DIR:loopz-desktop>")
  add_executable(loopz-editor-controller-tests editor_controller_tests.mm)
  loopz_macos_objc(loopz-editor-controller-tests)
  loopz_test_target(loopz-editor-controller-tests)
  target_link_libraries(loopz-editor-controller-tests PRIVATE loopz_editor_controller)
  add_test(NAME loopz-editor-controller COMMAND loopz-editor-controller-tests)
  add_executable(loopz-editor-protocol-tests editor_protocol_tests.mm)
  loopz_macos_objc(loopz-editor-protocol-tests)
  loopz_test_target(loopz-editor-protocol-tests)
  target_link_libraries(loopz-editor-protocol-tests PRIVATE loopz_editor_protocol)
  add_test(NAME loopz-editor-protocol COMMAND loopz-editor-protocol-tests)
  add_executable(loopz-editor-reducer-fixture editor_reducer_fixture.mm)
  loopz_macos_objc(loopz-editor-reducer-fixture)
  loopz_test_target(loopz-editor-reducer-fixture)
  target_link_libraries(loopz-editor-reducer-fixture PRIVATE loopz_editor_protocol)
  add_executable(loopz-editor-service-tests editor_service_tests.mm)
  add_executable(loopz-live-editor-client ../../native/tools/live_editor_client.mm)
  loopz_macos_objc(loopz-live-editor-client)
  target_link_libraries(loopz-live-editor-client PRIVATE loopz_editor_service)
  loopz_macos_objc(loopz-editor-service-tests)
  loopz_test_target(loopz-editor-service-tests)
  target_link_libraries(loopz-editor-service-tests PRIVATE loopz_editor_service Threads::Threads "-framework Security")
  add_test(NAME loopz-editor-service COMMAND loopz-editor-service-tests)
  set_tests_properties(loopz-editor-service PROPERTIES TIMEOUT 75)
  add_test(NAME loopz-editor-service-process COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/editor_service_process_tests.py" "$<TARGET_FILE:loopz-editor-service-tests>")
  set_tests_properties(loopz-editor-service-process PROPERTIES TIMEOUT 100)
  add_test(NAME loopz-editor-cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/editor_cli_tests.py" "$<TARGET_FILE:loopz-engine>")
  set_tests_properties(loopz-editor-cli PROPERTIES TIMEOUT 60)
  add_test(NAME loopz-desktop-model COMMAND "${LOOPZ_NODE_EXECUTABLE}" --test
    "${PROJECT_SOURCE_DIR}/interface/tests/model.test.mjs")
endif()
if(LOOPZ_BUILD_MACOS_BACKEND)
  add_executable(loopz-release-updates-tests release_updates_tests.mm
    ../../native/platform/macos/desktop/release_updates.mm)
  loopz_macos_objc(loopz-release-updates-tests)
  loopz_test_target(loopz-release-updates-tests)
  target_link_libraries(loopz-release-updates-tests PRIVATE "-framework AppKit")
  add_test(NAME loopz-release-updates COMMAND loopz-release-updates-tests)
endif()
if(LOOPZ_BUILD_MACOS_BACKEND)
  add_test(NAME loopz-audio-setup-contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/audio_setup_contract_tests.py" "$<TARGET_FILE:loopz-audio-setup>")
  add_executable(loopz-audio-maintenance-tests audio_maintenance_tests.mm)
  loopz_macos_objc(loopz-audio-maintenance-tests)
  loopz_test_target(loopz-audio-maintenance-tests)
  target_link_libraries(loopz-audio-maintenance-tests PRIVATE loopz_audio_maintenance)
  add_test(NAME loopz-audio-maintenance COMMAND loopz-audio-maintenance-tests)
endif()
