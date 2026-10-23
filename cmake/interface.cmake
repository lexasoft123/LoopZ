find_program(LOOPZ_NODE_EXECUTABLE node REQUIRED)
set(LOOPZ_PREBUILT_INTERFACE "" CACHE PATH "Previously built interface assets for offline native builds")
if(LOOPZ_PREBUILT_INTERFACE)
  foreach(ASSET index.html loopz.js loopz.css)
    if(NOT EXISTS "${LOOPZ_PREBUILT_INTERFACE}/${ASSET}")
      message(FATAL_ERROR "Missing prebuilt interface asset: ${ASSET}")
    endif()
  endforeach()
  set(LOOPZ_INTERFACE_OUTPUT "${LOOPZ_PREBUILT_INTERFACE}")
  add_custom_target(loopz-interface)
  return()
endif()
file(GLOB_RECURSE LOOPZ_INTERFACE_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/interface/src/*.jsx"
  "${PROJECT_SOURCE_DIR}/interface/src/*.js"
  "${PROJECT_SOURCE_DIR}/interface/src/*.css"
  "${PROJECT_SOURCE_DIR}/interface/src/*.json")
set(LOOPZ_INTERFACE_OUTPUT "${PROJECT_BINARY_DIR}/interface")
add_custom_command(
  OUTPUT "${LOOPZ_INTERFACE_OUTPUT}/index.html" "${LOOPZ_INTERFACE_OUTPUT}/loopz.js" "${LOOPZ_INTERFACE_OUTPUT}/loopz.css"
  COMMAND "${LOOPZ_NODE_EXECUTABLE}" scripts/build.mjs "${LOOPZ_INTERFACE_OUTPUT}"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/interface"
  DEPENDS ${LOOPZ_INTERFACE_SOURCES} "${PROJECT_SOURCE_DIR}/branding/mark.js" "${PROJECT_SOURCE_DIR}/interface/scripts/build.mjs"
          "${PROJECT_SOURCE_DIR}/interface/package-lock.json" "${PROJECT_SOURCE_DIR}/interface/packages/singz-ui-1.9.5.tgz"
  VERBATIM)
add_custom_target(loopz-interface DEPENDS "${LOOPZ_INTERFACE_OUTPUT}/index.html" "${LOOPZ_INTERFACE_OUTPUT}/loopz.js" "${LOOPZ_INTERFACE_OUTPUT}/loopz.css")
