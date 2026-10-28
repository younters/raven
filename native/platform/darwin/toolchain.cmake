# macOS host settings that every Raven target needs, applied before native/ is added.
#
# The runtime is written against Linux interfaces. Rather than editing the files that use them, the macOS build puts a directory of
# shim headers ahead of the SDK's: each one supplies a Linux header the runtime includes, on top of the macOS facilities it maps to.
# raven_darwin_compat.h is included into every host translation unit for the declarations that have no header of their own.

set(RAVEN_DARWIN_DIR ${CMAKE_CURRENT_LIST_DIR})
set(RAVEN_VULKAN_TEST_ICD /usr/local/share/vulkan/icd.d/MoltenVK_icd.json CACHE FILEPATH "Vulkan ICD for native tests")
set(RAVEN_KOSMICKRISP_ROOT "" CACHE PATH "Opt-in pinned KosmicKrisp root for graphics tests (does not change the runtime default)")
if(RAVEN_KOSMICKRISP_ROOT)
    # Set before native/ registers commands which embed this path, including the x86 compute test.
    set(RAVEN_VULKAN_TEST_ICD "${RAVEN_KOSMICKRISP_ROOT}/share/vulkan/icd.d/libkosmickrisp_icd.json")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "The macOS runtime targets Apple silicon; guest x86 code runs through Rosetta 2")
endif()

include_directories(BEFORE SYSTEM ${RAVEN_DARWIN_DIR}/include)
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:SHELL:-include ${RAVEN_DARWIN_DIR}/include/raven_darwin_compat.h>")

# Windows-target clang finds Homebrew's MinGW-w64 headers through this wrapper.
set(RAVEN_CLANG ${RAVEN_DARWIN_DIR}/bin/raven-clang CACHE FILEPATH "clang for Windows targets")

# The functions behind the shim headers, linked into every host target.
add_library(raven_darwin STATIC ${RAVEN_DARWIN_DIR}/compat/linux.cpp ${RAVEN_DARWIN_DIR}/compat/procfs.cpp
    ${RAVEN_DARWIN_DIR}/compat/sockets.cpp ${RAVEN_DARWIN_DIR}/compat/eventfd.cpp ${RAVEN_DARWIN_DIR}/compat/system.cpp
    ${RAVEN_DARWIN_DIR}/compat/host_launch.cpp)
target_link_libraries(raven_darwin PUBLIC "-framework CoreFoundation" "-framework IOKit" "-framework SystemConfiguration")
target_compile_options(raven_darwin PRIVATE -Wall -Wextra)
link_libraries(raven_darwin)

# Homebrew's default OpenSSL is 4, whose ASN.1 types are opaque; the runtime is written against OpenSSL 3, as Linux distributions ship.
if(NOT OPENSSL_ROOT_DIR AND EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()
# Homebrew links the default one's headers into /opt/homebrew/include, which other packages put on the include path: these go first.
if(OPENSSL_ROOT_DIR)
    include_directories(BEFORE SYSTEM ${OPENSSL_ROOT_DIR}/include)
endif()
# Every static library also carries raven_darwin; ld64 keeps one copy and would say so for each executable.
add_link_options(-Wl,-no_warn_duplicate_libraries)
