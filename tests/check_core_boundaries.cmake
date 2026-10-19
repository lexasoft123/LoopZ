# The portable layer must remain usable without platform SDK headers or flags.
file(GLOB_RECURSE CORE_FILES "${LOOPZ_ROOT}/native/include/*.hpp"
  "${LOOPZ_ROOT}/native/core/*.cpp")
foreach(SOURCE IN LISTS CORE_FILES)
  file(READ "${SOURCE}" CONTENT)
  if(CONTENT MATCHES "#[ \t]*(include|import)[ \t]*[<\"](loopz/macos/|mach/|xpc/|dispatch/|CoreAudio/|AudioUnit/|AudioToolbox/|Foundation/|AppKit/|AVFoundation/|TargetConditionals|windows[.]h|sys/mman[.]h|unistd[.]h)"
      OR CONTENT MATCHES "__APPLE__|_WIN32|#import")
    message(FATAL_ERROR "Platform dependency in portable core: ${SOURCE}")
  endif()
endforeach()
file(READ "${LOOPZ_ROOT}/native/core/CMakeLists.txt" CORE_BUILD)
if(CORE_BUILD MATCHES "framework|OBJCXX|loopz_bridge|zcore_device")
  message(FATAL_ERROR "Platform build dependency in portable core")
endif()
