find_program(LOOPZ_NODE_EXECUTABLE node REQUIRED)
file(GLOB_RECURSE LOOPZ_INTERFACE_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/interface/src/*.jsx"
  "${PROJECT_SOURCE_DIR}/interface/src/*.js"
  "${PROJECT_SOURCE_DIR}/interface/src/*.css")
set(LOOPZ_INTERFACE_OUTPUT "${PROJECT_BINARY_DIR}/interface")
add_custom_command(
  OUTPUT "${LOOPZ_INTERFACE_OUTPUT}/index.html" "${LOOPZ_INTERFACE_OUTPUT}/loopz.js" "${LOOPZ_INTERFACE_OUTPUT}/loopz.css"
  COMMAND "${LOOPZ_NODE_EXECUTABLE}" scripts/build.mjs "${LOOPZ_INTERFACE_OUTPUT}"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/interface"
  DEPENDS ${LOOPZ_INTERFACE_SOURCES} "${PROJECT_SOURCE_DIR}/branding/mark.js" "${PROJECT_SOURCE_DIR}/interface/scripts/build.mjs"
          "${PROJECT_SOURCE_DIR}/interface/package-lock.json" "${PROJECT_SOURCE_DIR}/interface/packages/singz-ui-1.9.5.tgz"
  VERBATIM)
add_custom_target(loopz-interface DEPENDS "${LOOPZ_INTERFACE_OUTPUT}/index.html" "${LOOPZ_INTERFACE_OUTPUT}/loopz.js" "${LOOPZ_INTERFACE_OUTPUT}/loopz.css")
add_library(loopz_desktop_session STATIC session.cpp session_host.mm session_process.cpp)
loopz_macos_objc(loopz_desktop_session)
target_link_libraries(loopz_desktop_session PUBLIC "-framework Foundation" "-framework Security")
add_library(loopz_desktop_permissions STATIC microphone_setup.mm)
loopz_macos_objc(loopz_desktop_permissions)
target_include_directories(loopz_desktop_permissions PUBLIC "${PROJECT_SOURCE_DIR}/native/platform/macos/include")
target_link_libraries(loopz_desktop_permissions PUBLIC loopz_desktop_session)
add_library(loopz_desktop_setup STATIC installation_setup.mm)
loopz_macos_objc(loopz_desktop_setup)
target_link_libraries(loopz_desktop_setup PUBLIC "-framework Foundation" "-framework Security")
add_executable(loopz-login login_main.mm)
loopz_macos_objc(loopz-login)
target_link_libraries(loopz-login PRIVATE loopz_desktop_session)
set_target_properties(loopz-login PROPERTIES OUTPUT_NAME "LoopZLogin")
add_executable(loopz-desktop MACOSX_BUNDLE
  main.mm
  bridge.mm
  resources.mm
  login.mm
  diagnostics.mm)
target_sources(loopz-desktop PRIVATE editing_actions.mm)
loopz_macos_objc(loopz-desktop)
target_link_libraries(loopz-desktop PRIVATE loopz_editor_service loopz_desktop_permissions loopz_desktop_setup "-framework WebKit" "-framework Security" "-framework UniformTypeIdentifiers" "-framework ServiceManagement")
set_target_properties(loopz-desktop PROPERTIES OUTPUT_NAME "LoopZ"
  MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/packaging/desktop.plist")
add_dependencies(loopz-desktop loopz-interface loopz-login)
set_property(TARGET loopz-desktop APPEND PROPERTY LINK_DEPENDS
  "${LOOPZ_INTERFACE_OUTPUT}/index.html" "${LOOPZ_INTERFACE_OUTPUT}/loopz.js" "${LOOPZ_INTERFACE_OUTPUT}/loopz.css"
  "${PROJECT_SOURCE_DIR}/branding/icon.icns"
  "$<TARGET_FILE:loopz-login>" "${PROJECT_SOURCE_DIR}/packaging/login-agent.plist")
add_custom_command(TARGET loopz-desktop POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/Library/LaunchAgents"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:loopz-login>" "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/MacOS/LoopZLogin"
  COMMAND /usr/bin/codesign --force --sign - --identifier com.lexasoft.loopz.login --timestamp=none "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/MacOS/LoopZLogin"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/packaging/login-agent.plist" "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/Library/LaunchAgents/com.lexasoft.loopz.login.plist"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/Resources/interface"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${LOOPZ_INTERFACE_OUTPUT}/index.html" "${LOOPZ_INTERFACE_OUTPUT}/loopz.js" "${LOOPZ_INTERFACE_OUTPUT}/loopz.css"
    "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/Resources/interface"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${PROJECT_SOURCE_DIR}/branding/icon.icns"
    "$<TARGET_BUNDLE_DIR:loopz-desktop>/Contents/Resources/LoopZ.icns"
  COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:loopz-desktop>"
  VERBATIM)
