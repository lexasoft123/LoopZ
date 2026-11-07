# The upstream single header is immutable and verified before compilation.
file(SHA256 "${PROJECT_SOURCE_DIR}/vendor/nlohmann/json.hpp" _loopz_json_sha256)
if(NOT _loopz_json_sha256 STREQUAL
   "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6")
  message(FATAL_ERROR "Pinned nlohmann JSON 3.11.3 header checksum mismatch")
endif()
# Portable control-domain serialization, deliberately separate from realtime DSP.
add_library(loopz_document_json STATIC
  "${PROJECT_SOURCE_DIR}/native/core/document_json.cpp")
add_library(LoopZ::DocumentJSON ALIAS loopz_document_json)
target_include_directories(loopz_document_json PUBLIC
  "${PROJECT_SOURCE_DIR}/native/include")
# Preserve the upstream single-header layout without rewriting vendor bytes.
target_include_directories(loopz_document_json SYSTEM PUBLIC
  "${PROJECT_SOURCE_DIR}/vendor")
target_link_libraries(loopz_document_json PUBLIC LoopZ::Core)
loopz_strict_target(loopz_document_json)
if(LOOPZ_BUILD_TESTS)
  add_executable(loopz-document-json-tests
    "${PROJECT_SOURCE_DIR}/tests/document_json_tests.cpp")
  target_link_libraries(loopz-document-json-tests PRIVATE LoopZ::DocumentJSON)
  loopz_test_target(loopz-document-json-tests)
  add_test(NAME loopz-document-json COMMAND loopz-document-json-tests)
endif()
