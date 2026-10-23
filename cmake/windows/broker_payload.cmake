# The signed maintenance executable carries the exact service bytes and digest.
# Runtime never trusts a writable sidecar manifest or an IPC-supplied digest.
function(loopz_embed_broker_payload target broker)
  set(directory "${CMAKE_CURRENT_BINARY_DIR}/broker-payload/$<CONFIG>/${target}")
  set(resource "${directory}/broker.rc")
  add_custom_command(OUTPUT "${resource}"
    BYPRODUCTS "${directory}/broker.bin" "${directory}/broker.sha256"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:${broker}>" "-DOUTPUT=${resource}"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/generate_broker_payload.cmake"
    DEPENDS ${broker} "$<TARGET_FILE:${broker}>"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/generate_broker_payload.cmake"
    VERBATIM)
  target_sources(${target} PRIVATE "${resource}")
endfunction()
