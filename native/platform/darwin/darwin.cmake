# macOS adjustments to targets native/CMakeLists.txt defines, applied after it so that file stays as upstream wrote it.

set(_raven_fex_default OFF)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_raven_fex_default ON)
endif()
option(RAVEN_FEX_MODHOST "Build the FEX execution backend and module-host tests" ${_raven_fex_default})
set(RAVEN_FEX_MODULE_ROOT "$ENV{RAVEN_FEX_MODULE_ROOT}" CACHE PATH "Provisioned FEX modules (optional at build time)")
if(NOT RAVEN_FEX_MODULE_ROOT)
    get_filename_component(_components "${PROJECT_SOURCE_DIR}/../raven-components/fex-modules" ABSOLUTE)
    if(EXISTS "${_components}/xtajit64.dll")
        set(RAVEN_FEX_MODULE_ROOT "${_components}" CACHE PATH "Provisioned FEX modules" FORCE)
    endif()
endif()
set(RAVEN_DXMT_ARM64_ROOT "$ENV{RAVEN_DXMT_ARM64_ROOT}" CACHE PATH "arm64 DXMT overlay for FEX (winemetal.so, raven-dxmt.dylib)")
if(NOT RAVEN_DXMT_ARM64_ROOT)
    get_filename_component(_dxmt_arm64 "${PROJECT_SOURCE_DIR}/../raven-components/dxmt-arm64" ABSOLUTE)
    if(EXISTS "${_dxmt_arm64}/winemetal.so")
        set(RAVEN_DXMT_ARM64_ROOT "${_dxmt_arm64}" CACHE PATH "arm64 DXMT overlay for FEX" FORCE)
    endif()
endif()
configure_file(${RAVEN_DARWIN_DIR}/fex/module_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/fex-module-config.h @ONLY)
if(RAVEN_FEX_MODHOST)
    add_subdirectory(${RAVEN_DARWIN_DIR}/fex/modhost ${CMAKE_CURRENT_BINARY_DIR}/fex-modhost)
endif()

if(RAVEN_KOSMICKRISP_ROOT)
    file(READ ${RAVEN_DARWIN_DIR}/graphics/kosmickrisp.json RAVEN_KOSMICKRISP_PIN)
    foreach(component lib/libvulkan_kosmickrisp.dylib share/vulkan/icd.d/libkosmickrisp_icd.json)
        if(NOT EXISTS "${RAVEN_KOSMICKRISP_ROOT}/${component}")
            message(FATAL_ERROR "Missing pinned KosmicKrisp file: ${component}; run scripts/provision-kosmickrisp.py")
        endif()
        string(JSON expected GET "${RAVEN_KOSMICKRISP_PIN}" files "${component}")
        file(SHA256 "${RAVEN_KOSMICKRISP_ROOT}/${component}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "KosmicKrisp file differs from SDK pin: ${component}")
        endif()
    endforeach()
endif()
option(RAVEN_DARWIN_DXVK_TESTS "Register strict DXVK comparison tests, including known unsupported hardware paths" OFF)
if(RAVEN_DARWIN_DXVK_TESTS AND (NOT RAVEN_DXMT_ROOT OR NOT RAVEN_DXVK_PE64_DIR))
    message(FATAL_ERROR "DXVK comparisons need RAVEN_DXMT_ROOT and RAVEN_DXVK_PE64_DIR to build/run matching fixtures")
endif()

add_executable(darwin_idle_wait_queue_test ${RAVEN_DARWIN_DIR}/idle_wait_queue_test.cpp)
add_test(NAME native_darwin_idle_wait_queue COMMAND darwin_idle_wait_queue_test)

# The guest controller: Linux traces an ELF child with ptrace; macOS runs an x86_64 Mach-O agent under Rosetta 2 that the host
# drives through shared memory. Same NativeProcess interface, different implementation file.
if(TARGET raven_cpu)
    if(RAVEN_FEX_MODHOST)
        target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/fex/fex_process.cpp ${RAVEN_DARWIN_DIR}/fex/fex_unsupported.cpp
            ${RAVEN_DARWIN_DIR}/fex/modhost/abi.S ${RAVEN_DARWIN_DIR}/fex/modhost/context.S)
        set_source_files_properties(${RAVEN_DARWIN_DIR}/fex/fex_process.cpp PROPERTIES
            COMPILE_OPTIONS "-ffixed-x18;-ffp-contract=off;-UNDEBUG;-Wno-unused-function")
        target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/fex/module_root.cpp)
        target_include_directories(raven_cpu PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
        target_compile_definitions(raven_cpu PUBLIC RAVEN_FEX_PROCESS=1)
        add_executable(fex_process_test ${RAVEN_DARWIN_DIR}/fex/fex_process_test.cpp)
        target_link_libraries(fex_process_test PRIVATE raven_cpu)
        target_link_options(fex_process_test PRIVATE -Wl,-platform_version,macos,11.0,11.0)
        if(RAVEN_MINGW64)
            set(FEX_ENTRY_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/fex-entry.exe)
            add_custom_command(OUTPUT ${FEX_ENTRY_FIXTURE}
                COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
                    -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                    -o ${FEX_ENTRY_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/native_cpu/entry.c
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_cpu/entry.c VERBATIM)
            add_custom_target(fex_entry_fixture DEPENDS ${FEX_ENTRY_FIXTURE})
            add_dependencies(fex_process_test fex_entry_fixture)
            foreach(contract entry syscall nt-service fault breakpoint divide ud2 timeout selfmodify lifetime served-malformed sparse-reservation)
                add_test(NAME native_${contract}_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${contract})
                set_tests_properties(native_${contract}_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
                    LABELS "fex;m2" ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
            endforeach()
            foreach(sign u s)
                foreach(bits 8 16 32 64)
                    set(cases zero overflow valid fold-zero fold-valid wide-valid)
                    if(sign STREQUAL "s")
                        list(APPEND cases fold-overflow negative-limit positive-limit positive-overflow)
                        if(NOT bits EQUAL 8)
                            list(APPEND cases wide-overflow)
                        endif()
                    endif()
                    foreach(case ${cases})
                        set(contract matrix-${sign}${bits}-${case})
                        add_test(NAME native_${contract}_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${contract})
                        set_tests_properties(native_${contract}_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
                            LABELS "fex;m2;divide" ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
                    endforeach()
                endforeach()
            endforeach()
        endif()
    endif()
    include(${RAVEN_DARWIN_DIR}/audio/audio.cmake)
    get_target_property(RAVEN_CPU_SOURCES raven_cpu SOURCES)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES cpu/native_cpu.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/native_process.cpp ${RAVEN_DARWIN_DIR}/unix_calls.cpp ${RAVEN_DARWIN_DIR}/x87sidecar.cpp)
    list(REMOVE_ITEM RAVEN_CPU_SOURCES desktop/shared_pixels.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/shared_pixels.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/system_api.cpp ${RAVEN_DARWIN_DIR}/nt_file_api.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/desktop_idle.cpp ${RAVEN_DARWIN_DIR}/dcomp/commit.cpp)
    list(APPEND RAVEN_CPU_SOURCES ${RAVEN_DARWIN_DIR}/dpi.cpp)
    set_property(TARGET raven_cpu PROPERTY SOURCES ${RAVEN_CPU_SOURCES})
    set(RAVEN_DXMT_ROOT "" CACHE PATH "Provisioned DXMT root (pe64/ and unix64/); RAVEN_DXMT_ROOT environment overrides it")
    if(RAVEN_DXMT_ROOT)
        set_property(SOURCE ${RAVEN_DARWIN_DIR}/unix_calls.cpp APPEND PROPERTY COMPILE_DEFINITIONS RAVEN_DXMT_ROOT="${RAVEN_DXMT_ROOT}")
    endif()
    # GNU ld only; ld64 keeps every library it is given.
    get_target_property(RAVEN_CPU_LINK_OPTIONS raven_cpu INTERFACE_LINK_OPTIONS)
    if(RAVEN_CPU_LINK_OPTIONS)
        list(REMOVE_ITEM RAVEN_CPU_LINK_OPTIONS "LINKER:--no-as-needed")
        set_property(TARGET raven_cpu PROPERTY INTERFACE_LINK_OPTIONS ${RAVEN_CPU_LINK_OPTIONS})
    endif()
    add_executable(darwin_dcomp_commit_test ${RAVEN_DARWIN_DIR}/tests/dcomp_commit.cpp)
    target_link_libraries(darwin_dcomp_commit_test PRIVATE raven_cpu)
    add_test(NAME native_darwin_dcomp_commit COMMAND darwin_dcomp_commit_test)
    add_executable(darwin_scheduler_wait_test ${RAVEN_DARWIN_DIR}/scheduler_wait_test.cpp)
    target_link_libraries(darwin_scheduler_wait_test PRIVATE raven_cpu)
    add_dependencies(darwin_scheduler_wait_test raven-bootstrap)
    add_test(NAME native_darwin_scheduler_wait COMMAND darwin_scheduler_wait_test $<TARGET_FILE:raven-bootstrap>)
    add_test(NAME native_darwin_scheduler_wait_parallel COMMAND darwin_scheduler_wait_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_scheduler_wait native_darwin_scheduler_wait_parallel PROPERTIES TIMEOUT 30)
    set_tests_properties(native_darwin_scheduler_wait_parallel PROPERTIES ENVIRONMENT RAVEN_PARALLEL=1)
    add_executable(darwin_parallel_test ${RAVEN_DARWIN_DIR}/parallel_test.cpp)
    target_link_libraries(darwin_parallel_test PRIVATE raven_cpu)
    add_dependencies(darwin_parallel_test raven-bootstrap)
    add_test(NAME native_darwin_parallel_contracts COMMAND darwin_parallel_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_parallel_contracts PROPERTIES TIMEOUT 30 ENVIRONMENT RAVEN_PARALLEL=1)
    add_test(NAME native_darwin_private_fallback_contracts COMMAND darwin_parallel_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_private_fallback_contracts PROPERTIES TIMEOUT 30
        ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_DARWIN_DISABLE_PRIVATE_ALIASES=1")
    if(RAVEN_MINGW64)
        set(RAVEN_DARWIN_PARALLEL_BENCHMARK ${CMAKE_CURRENT_BINARY_DIR}/darwin-parallel-benchmark.exe)
        add_custom_command(OUTPUT ${RAVEN_DARWIN_PARALLEL_BENCHMARK}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                -o ${RAVEN_DARWIN_PARALLEL_BENCHMARK} ${RAVEN_DARWIN_DIR}/parallel_benchmark.c -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/parallel_benchmark.c VERBATIM)
        add_custom_target(darwin_parallel_benchmark ALL DEPENDS ${RAVEN_DARWIN_PARALLEL_BENCHMARK})
        set(RAVEN_DARWIN_HANDOFF_BENCHMARK ${CMAKE_CURRENT_BINARY_DIR}/darwin-handoff-benchmark.exe)
        add_custom_command(OUTPUT ${RAVEN_DARWIN_HANDOFF_BENCHMARK}
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
                -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                -o ${RAVEN_DARWIN_HANDOFF_BENCHMARK} ${RAVEN_DARWIN_DIR}/handoff_benchmark.c -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/handoff_benchmark.c VERBATIM)
        add_custom_target(darwin_handoff_benchmark ALL DEPENDS ${RAVEN_DARWIN_HANDOFF_BENCHMARK})
        add_test(NAME native_darwin_handoff COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_handoff.py
            $<TARGET_FILE:raven> --run-process --execution=native --timeout-ms=15000
            --stdio=inherit ${RAVEN_DARWIN_HANDOFF_BENCHMARK})
        add_test(NAME native_darwin_handoff_parallel COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_handoff.py
            $<TARGET_FILE:raven> --run-process --execution=native --timeout-ms=15000
            --stdio=inherit ${RAVEN_DARWIN_HANDOFF_BENCHMARK})
        set_tests_properties(native_darwin_handoff native_darwin_handoff_parallel PROPERTIES TIMEOUT 30)
        add_test(NAME native_darwin_handoff_runner COMMAND ${Python3_EXECUTABLE}
            ${RAVEN_DARWIN_DIR}/tests/handoff_runner_test.py)
        set_tests_properties(native_darwin_handoff_parallel PROPERTIES ENVIRONMENT RAVEN_PARALLEL=1)
    endif()
endif()

# A Windows Vulkan program presents into the actual CAMetalLayer, then replaces its swapchain after a client resize.
if(TARGET vulkan_pe_fixture AND RAVEN_MINGW64)
    set(RAVEN_COCOA_VULKAN_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-fixtures/cocoa-present.exe)
    add_custom_command(OUTPUT ${RAVEN_COCOA_VULKAN_FIXTURE}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
            -idirafter ${Vulkan_INCLUDE_DIRS} -Wl,--entry,raven_entry
            -o ${RAVEN_COCOA_VULKAN_FIXTURE} ${RAVEN_DARWIN_DIR}/tests/vulkan_present.c
            ${VULKAN_FIXTURES}/vulkan.a -lkernel32 -luser32
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/vulkan_present.c ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
    add_custom_target(cocoa_vulkan_fixture ALL DEPENDS ${RAVEN_COCOA_VULKAN_FIXTURE})
    set(HIDDEN_VULKAN_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/vulkan-fixtures/hidden-surface.exe)
    add_custom_command(OUTPUT ${HIDDEN_VULKAN_FIXTURE}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -mno-stack-arg-probe
            -idirafter ${Vulkan_INCLUDE_DIRS} -Wl,--entry,raven_entry
            -o ${HIDDEN_VULKAN_FIXTURE} ${RAVEN_DARWIN_DIR}/tests/vulkan_hidden_surface.c
            ${VULKAN_FIXTURES}/vulkan.a -lkernel32 -luser32
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/vulkan_hidden_surface.c ${VULKAN_FIXTURES}/vulkan.a VERBATIM)
    add_custom_target(cocoa_hidden_vulkan_fixture ALL DEPENDS ${HIDDEN_VULKAN_FIXTURE})
    option(RAVEN_DARWIN_HIDDEN_SURFACE_TEST "Test hidden Vulkan surfaces in a logged-in macOS GUI session" OFF)
    if(RAVEN_DARWIN_HIDDEN_SURFACE_TEST)
        add_test(NAME native_darwin_hidden_vulkan_surface COMMAND raven --run-process --execution=native
            --timeout-ms=15000 --desktop=macos ${HIDDEN_VULKAN_FIXTURE})
        set_tests_properties(native_darwin_hidden_vulkan_surface PROPERTIES TIMEOUT 20
            PASS_REGULAR_EXPRESSION "Native entry returned 0 " ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/hidden-vulkan-prefix)
    endif()

    # This test needs a graphical login and screen capture access, so opt in when running desktop integration tests.
    option(RAVEN_DARWIN_PRESENT_TEST "Test Vulkan presentation on the logged-in macOS desktop (requires Pillow)" OFF)
    if(RAVEN_DARWIN_PRESENT_TEST)
        find_program(RAVEN_DARWIN_PRESENT_PYTHON NAMES python3 REQUIRED)
        add_test(NAME native_vulkan_cocoa_present COMMAND ${RAVEN_DARWIN_PRESENT_PYTHON}
            ${RAVEN_DARWIN_DIR}/tests/vulkan_present.py $<TARGET_FILE:raven> ${RAVEN_COCOA_VULKAN_FIXTURE}
            ${CMAKE_CURRENT_BINARY_DIR}/vulkan-cocoa-present)
        set_tests_properties(native_vulkan_cocoa_present PROPERTIES TIMEOUT 60)
    endif()
endif()

if(TARGET raven_cpu AND RAVEN_MINGW64 AND RAVEN_DXMT_ROOT)
    set(IDENTITY_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-identity.exe)
    add_custom_command(OUTPUT ${IDENTITY_EXE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_identity.c
            -o ${IDENTITY_EXE} -lsetupapi -ladvapi32 -luser32 -ldxgi -ldxguid -lkernel32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_identity.c VERBATIM)
    add_custom_target(darwin_dxmt_identity_fixture ALL DEPENDS ${IDENTITY_EXE})
    add_test(NAME darwin_dxmt_identity COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${IDENTITY_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-identity identity)
    set_tests_properties(darwin_dxmt_identity PROPERTIES TIMEOUT 120 ENVIRONMENT "RAVEN_DXGI_VENDOR=amd")
    set(SELF_SHARE_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-self-share.exe)
    add_custom_command(OUTPUT ${SELF_SHARE_EXE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_self_share.c
            -o ${SELF_SHARE_EXE} -ld3d11 -ldxgi -ldxguid -lkernel32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_self_share.c VERBATIM)
    add_custom_target(darwin_dxmt_self_share_fixture ALL DEPENDS ${SELF_SHARE_EXE})
    add_test(NAME darwin_dxmt_self_share COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${SELF_SHARE_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-self-share self_share)
    set_tests_properties(darwin_dxmt_self_share PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    set(D3D_COMPILE_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-d3d-compile.exe)
    add_custom_command(OUTPUT ${D3D_COMPILE_EXE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/d3d_compile.c
            -o ${D3D_COMPILE_EXE} -lkernel32 -ldxguid -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/d3d_compile.c VERBATIM)
    add_custom_target(darwin_d3d_compile_fixture ALL DEPENDS ${D3D_COMPILE_EXE})
    add_test(NAME darwin_d3d_compile COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${D3D_COMPILE_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/d3d-compile d3d_compile)
    set_tests_properties(darwin_d3d_compile PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    if(TARGET local_library_64_fixture)
        add_test(NAME darwin_d3d_compile_local COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
            $<TARGET_FILE:raven> ${D3D_COMPILE_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/d3d-compile-local
            d3d_compile_local ${CMAKE_CURRENT_BINARY_DIR}/local-library-64/winhttp.dll)
        set_tests_properties(darwin_d3d_compile_local PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()
    set(NT_FILE_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-nt-file.exe)
    add_custom_command(OUTPUT ${NT_FILE_EXE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/nt_file.c
            -o ${NT_FILE_EXE} -lntdll -ladvapi32 -lkernel32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/nt_file.c VERBATIM)
    add_custom_target(darwin_nt_file_fixture ALL DEPENDS ${NT_FILE_EXE})
    add_test(NAME darwin_nt_file COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${NT_FILE_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/nt-file nt_file)
    set_tests_properties(darwin_nt_file PROPERTIES TIMEOUT 120)
    foreach(width IN ITEMS 64 32)
        if(width EQUAL 64)
            set(KEYED_CC ${RAVEN_MINGW64})
        else()
            find_program(RAVEN_KEYED_MINGW32 NAMES i686-w64-mingw32-gcc REQUIRED)
            set(KEYED_CC ${RAVEN_KEYED_MINGW32})
        endif()
        set(KEYED_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-keyed${width}.exe)
        add_custom_command(OUTPUT ${KEYED_EXE}
            COMMAND ${KEYED_CC} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_keyed.c
                -o ${KEYED_EXE} -lgdi32 -lkernel32 -lgcc
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_keyed.c VERBATIM)
        add_custom_target(darwin_dxmt_keyed${width}_fixture ALL DEPENDS ${KEYED_EXE})
        add_test(NAME darwin_dxmt_keyed_x${width} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
            $<TARGET_FILE:raven> ${KEYED_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-keyed${width} keyed)
        set_tests_properties(darwin_dxmt_keyed_x${width} PROPERTIES TIMEOUT 120)
        set(CATALOG_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-socket-catalog${width}.exe)
        add_custom_command(OUTPUT ${CATALOG_EXE}
            COMMAND ${KEYED_CC} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/socket_catalog.c
                -o ${CATALOG_EXE} -lws2_32 -lkernel32 -lgcc
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/socket_catalog.c VERBATIM)
        add_custom_target(darwin_socket_catalog${width}_fixture ALL DEPENDS ${CATALOG_EXE})
        add_test(NAME darwin_socket_catalog_x${width} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
            $<TARGET_FILE:raven> ${CATALOG_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/socket-catalog${width} socket_catalog)
        set_tests_properties(darwin_socket_catalog_x${width} PROPERTIES TIMEOUT 120)
        unset(KEYED_CC)
    endforeach()
    set(RAVEN_DXMT_SHARED ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-shared.exe)
    add_custom_command(OUTPUT ${RAVEN_DXMT_SHARED}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_shared.c
            -o ${RAVEN_DXMT_SHARED} -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_shared.c VERBATIM)
    add_custom_target(darwin_dxmt_shared_fixture ALL DEPENDS ${RAVEN_DXMT_SHARED})
    add_test(NAME darwin_dxmt_shared COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${RAVEN_DXMT_SHARED} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-shared shared)
    set_tests_properties(darwin_dxmt_shared PROPERTIES TIMEOUT 120)
    set(KEYED_TEXTURE_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-shared-keyed.exe)
    add_custom_command(OUTPUT ${KEYED_TEXTURE_EXE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -DDXMT_SHARED_KEYED=1 -Wl,--entry,entry
            ${RAVEN_DARWIN_DIR}/tests/dxmt_shared.c -o ${KEYED_TEXTURE_EXE}
            -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_shared.c VERBATIM)
    add_custom_target(darwin_dxmt_shared_keyed_fixture ALL DEPENDS ${KEYED_TEXTURE_EXE})
    add_test(NAME darwin_dxmt_shared_keyed COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${KEYED_TEXTURE_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-shared-keyed shared_keyed)
    set_tests_properties(darwin_dxmt_shared_keyed PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    set(RAVEN_DXMT_DEVICE10 ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-device10.exe)
    add_custom_command(OUTPUT ${RAVEN_DXMT_DEVICE10}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DARWIN_DIR}/tests/dxmt_device10.c
            -o ${RAVEN_DXMT_DEVICE10} -L${RAVEN_DXMT_ROOT}/pe64 -ld3d10 -ld3d10_1 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/dxmt_device10.c VERBATIM)
    add_custom_target(darwin_dxmt_device10_fixture ALL DEPENDS ${RAVEN_DXMT_DEVICE10})
    add_test(NAME darwin_dxmt_device10 COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
        $<TARGET_FILE:raven> ${RAVEN_DXMT_DEVICE10} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-device10 device10)
    set_tests_properties(darwin_dxmt_device10 PROPERTIES TIMEOUT 120)
    set(RAVEN_DXMT_IMAGE_PYTHON "")
    foreach(candidate ${Python3_EXECUTABLE} /opt/homebrew/bin/python3 /usr/bin/python3)
        if(NOT RAVEN_DXMT_IMAGE_PYTHON AND EXISTS ${candidate})
            execute_process(COMMAND ${candidate} -c "import PIL" RESULT_VARIABLE pillow_result OUTPUT_QUIET ERROR_QUIET)
            if(pillow_result EQUAL 0)
                set(RAVEN_DXMT_IMAGE_PYTHON ${candidate})
            endif()
        endif()
    endforeach()
    if(NOT RAVEN_DXMT_IMAGE_PYTHON)
        message(FATAL_ERROR "DXMT screen tests need a Python interpreter with Pillow")
    endif()
    set(RAVEN_DXMT_WINDOW_INFO ${CMAKE_CURRENT_BINARY_DIR}/dxmt-window-info)
    add_custom_command(OUTPUT ${RAVEN_DXMT_WINDOW_INFO}
        COMMAND /usr/bin/clang -fobjc-arc ${RAVEN_DARWIN_DIR}/tests/window_info.m -framework Foundation -framework CoreGraphics
            -o ${RAVEN_DXMT_WINDOW_INFO}
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/window_info.m VERBATIM)
    add_custom_target(darwin_dxmt_window_info ALL DEPENDS ${RAVEN_DXMT_WINDOW_INFO})
    find_program(RAVEN_MINGW64_CXX x86_64-w64-mingw32-g++)
    if(RAVEN_MINGW64_CXX)
        set(DCOMP_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dcomp.exe)
        add_custom_command(OUTPUT ${DCOMP_EXE}
            COMMAND ${RAVEN_MINGW64_CXX} -nostdlib -O2 -Wl,--entry,entry
                ${RAVEN_DARWIN_DIR}/tests/dcomp.cpp -o ${DCOMP_EXE}
                -ldcomp -ld3d11 -ldxgi -ldxguid -luuid -lkernel32 -luser32 -lgcc
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/dcomp.cpp VERBATIM)
        add_custom_target(darwin_dcomp_fixture ALL DEPENDS ${DCOMP_EXE})
        foreach(kind local foreign local-hidpi foreign-hidpi popup popup-hidpi)
            add_test(NAME darwin_dcomp_${kind} COMMAND ${RAVEN_DXMT_IMAGE_PYTHON} ${RAVEN_DARWIN_DIR}/tests/run_dcomp.py
                $<TARGET_FILE:raven> ${DCOMP_EXE} ${RAVEN_DXMT_ROOT} ${RAVEN_DXMT_WINDOW_INFO}
                ${CMAKE_CURRENT_BINARY_DIR}/dcomp-${kind} ${kind})
            set_tests_properties(darwin_dcomp_${kind} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endforeach()
    endif()

    foreach(workload device pool pacing triangle window deferred geometry tessellation textured msaa compute features fullscreen present_rate)
        if(workload STREQUAL "device")
            set(RAVEN_DXMT_FIXTURE ${RAVEN_DARWIN_DIR}/tests/dxmt_device.c)
        elseif(workload STREQUAL "pool")
            set(RAVEN_DXMT_FIXTURE ${RAVEN_DARWIN_DIR}/tests/dxmt_pool.c)
        elseif(workload STREQUAL "pacing")
            set(RAVEN_DXMT_FIXTURE ${RAVEN_DARWIN_DIR}/tests/dxmt_pacing.c)
        else()
            set(RAVEN_DXMT_FIXTURE ${PROJECT_SOURCE_DIR}/testdata/d3d11/${workload}.c)
        endif()
        set(RAVEN_DXMT_EXE ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-${workload}.exe)
        add_custom_command(OUTPUT ${RAVEN_DXMT_EXE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,entry ${RAVEN_DXMT_FIXTURE}
                -o ${RAVEN_DXMT_EXE} -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
            DEPENDS ${RAVEN_DXMT_FIXTURE} ${PROJECT_SOURCE_DIR}/testdata/d3d11/tri_shaders.h
                ${PROJECT_SOURCE_DIR}/testdata/d3d11/gs_shaders.h ${PROJECT_SOURCE_DIR}/testdata/d3d11/tess_shaders.h
                ${PROJECT_SOURCE_DIR}/testdata/d3d11/tex_shaders.h ${PROJECT_SOURCE_DIR}/testdata/d3d11/features_shaders.h
                ${PROJECT_SOURCE_DIR}/testdata/d3d11/compute_shader.h VERBATIM)
        add_custom_target(darwin_dxmt_${workload}_fixture ALL DEPENDS ${RAVEN_DXMT_EXE})
        if(workload STREQUAL "window")
            add_test(NAME darwin_dxmt_window COMMAND ${RAVEN_DXMT_IMAGE_PYTHON} ${RAVEN_DARWIN_DIR}/tests/run_present.py
                $<TARGET_FILE:raven> ${RAVEN_DXMT_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-window ${RAVEN_DXMT_WINDOW_INFO})
            set_tests_properties(darwin_dxmt_window PROPERTIES RESOURCE_LOCK raven_compositor)
        else()
            add_test(NAME darwin_dxmt_${workload} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                $<TARGET_FILE:raven> ${RAVEN_DXMT_EXE} ${RAVEN_DXMT_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dxmt-${workload} ${workload})
        endif()
        set_tests_properties(darwin_dxmt_${workload} PROPERTIES TIMEOUT 120)
        if(RAVEN_DARWIN_DXVK_TESTS AND workload MATCHES "^(triangle|deferred|geometry|tessellation|textured|msaa|compute|present_rate|pacing)$")
            add_test(NAME darwin_dxvk_${workload} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                $<TARGET_FILE:raven> ${RAVEN_DXMT_EXE} ${RAVEN_DXVK_PE64_DIR} ${CMAKE_CURRENT_BINARY_DIR}/dxvk-${workload} ${workload} DXVK)
            set_tests_properties(darwin_dxvk_${workload} PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
            if(workload STREQUAL "present_rate" OR workload STREQUAL "pacing")
                set_tests_properties(darwin_dxvk_${workload} PROPERTIES RESOURCE_LOCK raven_compositor)
            endif()
        endif()
        if(workload STREQUAL "fullscreen" OR workload STREQUAL "present_rate" OR workload STREQUAL "pacing")
            set_tests_properties(darwin_dxmt_${workload} PROPERTIES RESOURCE_LOCK raven_compositor)
        endif()
    endforeach()
    set_tests_properties(darwin_dxmt_device10 PROPERTIES RESOURCE_LOCK raven_compositor)
    if(RAVEN_FEX_MODHOST)
        add_test(NAME darwin_dxmt_window_fex COMMAND ${RAVEN_DXMT_IMAGE_PYTHON} ${RAVEN_DARWIN_DIR}/tests/run_present.py
            $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-window.exe ${RAVEN_DXMT_ROOT}
            ${CMAKE_CURRENT_BINARY_DIR}/dxmt-window-fex ${RAVEN_DXMT_WINDOW_INFO})
        set_tests_properties(darwin_dxmt_window_fex PROPERTIES TIMEOUT 120 RESOURCE_LOCK raven_compositor
            ENVIRONMENT "RAVEN_DXMT_TEST_ENGINE=fex")
        foreach(width 64 32)
            if(TARGET darwin_dxmt_keyed${width}_fixture)
                add_test(NAME darwin_dxmt_keyed_x${width}_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                    $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-keyed${width}.exe ${RAVEN_DXMT_ROOT}
                    ${CMAKE_CURRENT_BINARY_DIR}/dxmt-keyed${width}-fex keyed)
                set_tests_properties(darwin_dxmt_keyed_x${width}_fex PROPERTIES TIMEOUT 120 ENVIRONMENT "RAVEN_DXMT_TEST_ENGINE=fex")
            endif()
        endforeach()
        foreach(workload identity self_share shared shared_keyed device10 device pool pacing triangle deferred geometry tessellation textured msaa compute features fullscreen present_rate)
            set(fixture_name "${workload}")
            if(workload MATCHES "^(self_share|shared_keyed)$")
                string(REPLACE "_" "-" fixture_name "${workload}")
            endif()
            add_test(NAME darwin_dxmt_${workload}_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                $<TARGET_FILE:raven> ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-${fixture_name}.exe ${RAVEN_DXMT_ROOT}
                ${CMAKE_CURRENT_BINARY_DIR}/dxmt-${workload}-fex ${workload})
            set_tests_properties(darwin_dxmt_${workload}_fex PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
                RESOURCE_LOCK raven_compositor ENVIRONMENT "RAVEN_DXMT_TEST_ENGINE=fex;RAVEN_DXGI_VENDOR=amd")
        endforeach()
    endif()

endif()

# These fixtures use PE DXVK and the host Vulkan bridge; no native WoW64 unix-call table is involved.
if(RAVEN_DARWIN_DXVK_TESTS)
    foreach(width 64 32)
        if(RAVEN_MINGW${width} AND RAVEN_DXVK_PE${width}_DIR)
            foreach(api d3d9 d3d11)
                if(api STREQUAL "d3d9")
                    set(source ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c)
                    set(entry raven_entry)
                    set(libraries -lkernel32 -luser32)
                    set(kind d3d9)
                elseif(width EQUAL 32)
                    set(source ${PROJECT_SOURCE_DIR}/testdata/d3d11/triangle.c)
                    set(entry _entry@0)
                    set(libraries -ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc)
                    set(kind triangle)
                else()
                    continue() # The x64 D3D11 fixture is already registered above.
                endif()
                set(exe ${CMAKE_CURRENT_BINARY_DIR}/darwin-dxvk-${api}-${width}.exe)
                add_custom_command(OUTPUT ${exe}
                    COMMAND ${RAVEN_MINGW${width}} -O2 -nostdlib -fno-builtin -fno-stack-protector
                        -Wl,--entry,${entry},--no-insert-timestamp -o ${exe} ${source} ${libraries}
                    DEPENDS ${source} ${PROJECT_SOURCE_DIR}/testdata/d3d11/tri_shaders.h VERBATIM)
                add_custom_target(darwin_dxvk_${api}_${width}_fixture ALL DEPENDS ${exe})
                add_test(NAME darwin_dxvk_${api}_${width} COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/run_dxmt.py
                    $<TARGET_FILE:raven> ${exe} ${RAVEN_DXVK_PE${width}_DIR} ${CMAKE_CURRENT_BINARY_DIR}/dxvk-${api}-${width} ${kind} DXVK)
                set_tests_properties(darwin_dxvk_${api}_${width} PROPERTIES TIMEOUT 120 ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
                if(api STREQUAL "d3d9")
                    set_tests_properties(darwin_dxvk_${api}_${width} PROPERTIES RESOURCE_LOCK raven_compositor)
                endif()
            endforeach()
        endif()
    endforeach()
endif()

# A real PE caller and a tiny Mach-O unixlib exercise the boundary without Wine or a graphics driver.
if(TARGET raven_cpu AND RAVEN_MINGW64)
    set(RAVEN_UNIX_CALL_TEST_DIR ${RAVEN_DARWIN_DIR}/tests)
    set(RAVEN_UNIX_CALL_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/raven-unix-call-test.so)
    set(RAVEN_UNIX_CALL_PE ${CMAKE_CURRENT_BINARY_DIR}/raven-unix-call-test.exe)
    add_custom_command(OUTPUT ${RAVEN_UNIX_CALL_LIBRARY}
        COMMAND /usr/bin/clang -arch x86_64 -dynamiclib -O2 -Wall -Wextra
            ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_library.c -o ${RAVEN_UNIX_CALL_LIBRARY}
        DEPENDS ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_library.c ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_shared.h VERBATIM)
    add_custom_command(OUTPUT ${RAVEN_UNIX_CALL_PE}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O2 -Wl,--entry,mainCRTStartup
            ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.c ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.s -o ${RAVEN_UNIX_CALL_PE}
        DEPENDS ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.c ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_pe.s
            ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_shared.h VERBATIM)
    add_custom_target(darwin_unix_call_fixtures ALL DEPENDS ${RAVEN_UNIX_CALL_LIBRARY} ${RAVEN_UNIX_CALL_PE})
    add_executable(darwin_unix_call_test ${RAVEN_UNIX_CALL_TEST_DIR}/unix_call_test.cpp)
    target_link_libraries(darwin_unix_call_test PRIVATE raven_cpu)
    add_dependencies(darwin_unix_call_test darwin_unix_call_fixtures raven-bootstrap)
    add_test(NAME darwin_unix_call COMMAND darwin_unix_call_test $<TARGET_FILE:raven-bootstrap>
        ${RAVEN_UNIX_CALL_PE} ${RAVEN_UNIX_CALL_LIBRARY})
    add_test(NAME darwin_unix_call_parallel COMMAND ${CMAKE_COMMAND} -E env RAVEN_PARALLEL=1
        $<TARGET_FILE:darwin_unix_call_test> $<TARGET_FILE:raven-bootstrap>
        ${RAVEN_UNIX_CALL_PE} ${RAVEN_UNIX_CALL_LIBRARY})
    set_tests_properties(darwin_unix_call darwin_unix_call_parallel PROPERTIES TIMEOUT 20)
endif()

# raven-bootstrap keeps its name, so everything that locates it next to raven finds the macOS guest the same way.
if(TARGET raven-bootstrap)
    set_property(TARGET raven-bootstrap PROPERTY SOURCES ${RAVEN_DARWIN_DIR}/guest/agent.c ${RAVEN_DARWIN_DIR}/guest/reserve.s
        ${RAVEN_DARWIN_DIR}/guest/unix_call.s ${RAVEN_DARWIN_DIR}/guest/x87sidecar.c)
    set_property(TARGET raven-bootstrap PROPERTY COMPILE_OPTIONS -O2 -Wall -Wextra -fno-sanitize=all -fno-stack-protector)
    set_property(TARGET raven-bootstrap PROPERTY LINKER_LANGUAGE C)
    set_property(TARGET raven-bootstrap PROPERTY OSX_ARCHITECTURES x86_64)
    # A guest program: none of the host's Linux shims or libraries.
    set_property(TARGET raven-bootstrap PROPERTY INCLUDE_DIRECTORIES "")
    set_property(TARGET raven-bootstrap PROPERTY LINK_LIBRARIES "")
    # Windows images want the low 4 GiB: the agent loads high and a zero-fill segment holds the low range until the host maps it.
    set_property(TARGET raven-bootstrap PROPERTY LINK_OPTIONS -fno-sanitize=all -Wl,-no_pie -Wl,-pagezero_size,0x10000
        -Wl,-image_base,0x700000000000 -Wl,-segaddr,RAVEN_LOW,0x10000 -Wl,-w)
endif()

# The Linux interprocess behaviour of the shims: sequenced packets, abstract socket names and the /proc process files.
add_executable(darwin_compat_test ${RAVEN_DARWIN_DIR}/compat/compat_test.cpp)
add_test(NAME darwin_compat_contract COMMAND darwin_compat_test)

# The Vulkan loader of the LunarG SDK names itself @rpath/libvulkan.1.dylib.
if(TARGET Vulkan::Vulkan AND Vulkan_LIBRARY)
    get_filename_component(RAVEN_VULKAN_LIBRARY_DIRECTORY ${Vulkan_LIBRARY} DIRECTORY)
    set_property(TARGET Vulkan::Vulkan APPEND PROPERTY INTERFACE_LINK_OPTIONS "LINKER:-rpath,${RAVEN_VULKAN_LIBRARY_DIRECTORY}")
endif()

# These exercise capabilities the Darwin agent / MoltenVK do not implement. Keep the distinction visible in CTest.
if(TEST native_vulkan_queued_extensions_x64)
    set_tests_properties(native_vulkan_queued_extensions_x64 PROPERTIES DISABLED TRUE) # Requires parallel guest threads.
endif()
foreach(test native_vulkan_descriptor_buffer_x64 native_vulkan_descriptor_buffer_x32)
    if(TEST ${test})
        set_tests_properties(${test} PROPERTIES SKIP_REGULAR_EXPRESSION "Native entry returned 3 ") # Extension not offered.
    endif()
endforeach()

# Use the SDK ICD consistently, including host reference calls made outside the bridge.
get_property(RAVEN_DARWIN_TESTS DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS RAVEN_DARWIN_TESTS)
    if(test MATCHES "^native_(vulkan_|x86_vulkan_|shared_gpu_memory_|d3d(8|9|11|12)_|x86_d3d)")
        set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "VK_DRIVER_FILES=${RAVEN_VULKAN_TEST_ICD}")
    endif()
endforeach()

# The macOS desktop (--desktop=macos): AppKit windows, Metal surfaces for graphics drivers, and keymaps from xkbcommon.
if(TARGET raven_cpu)
    enable_language(OBJCXX)
    target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/print.mm ${RAVEN_DARWIN_DIR}/loader_api.cpp)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/print.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    target_link_libraries(raven_cpu PUBLIC "-framework AppKit" "-framework ApplicationServices")
    target_sources(raven_cpu PRIVATE ${RAVEN_DARWIN_DIR}/dxmt/presentation.mm ${RAVEN_DARWIN_DIR}/dxmt/kernel_thunks.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/dxmt/presentation.mm ${RAVEN_DARWIN_DIR}/dxmt/kernel_thunks.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_property(TARGET raven_cpu PROPERTY OBJCXX_STANDARD 20)
    target_link_libraries(raven_cpu PUBLIC "-framework IOSurface" "-framework QuartzCore" "-framework Metal")
    target_sources(raven PRIVATE ${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_property(TARGET raven PROPERTY OBJCXX_STANDARD 20)
    target_compile_definitions(raven PRIVATE RAVEN_COCOA=1)
    target_link_options(raven PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${RAVEN_DARWIN_DIR}/runtime-info.plist")
    set_property(TARGET raven APPEND PROPERTY LINK_DEPENDS ${RAVEN_DARWIN_DIR}/runtime-info.plist)
    target_link_libraries(raven PRIVATE "-framework AppKit" "-framework QuartzCore" "-framework Carbon" "-framework Metal")
    if(TARGET PkgConfig::XKBCOMMON)
        target_link_libraries(raven PRIVATE PkgConfig::XKBCOMMON)
    else()
        message(WARNING "xkbcommon is missing: the macOS desktop needs it for keyboard input (brew install libxkbcommon)")
    endif()
endif()

# Skia's macOS build always carries its CoreGraphics image generator.
if(TARGET raven_text)
    target_link_libraries(raven_text PUBLIC "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO")
endif()

if(TARGET native_cpu_test)
    add_executable(darwin_dock_policy_test ${RAVEN_DARWIN_DIR}/dock_policy_test.cpp)
    target_include_directories(darwin_dock_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    add_test(NAME native_darwin_dock_policy COMMAND darwin_dock_policy_test)
    add_executable(darwin_host_launch_test ${RAVEN_DARWIN_DIR}/host_launch_test.cpp)
    target_link_libraries(darwin_host_launch_test PRIVATE raven_darwin)
    add_test(NAME native_darwin_host_launch COMMAND darwin_host_launch_test)
    add_executable(darwin_foreground_test ${RAVEN_DARWIN_DIR}/foreground_test.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    # app_identity.mm is shared by Raven and these contracts and requires ARC in every target.
    set_source_files_properties(${RAVEN_DARWIN_DIR}/app_identity.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-Wall;-Wextra")
    set_source_files_properties(${RAVEN_DARWIN_DIR}/foreground_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(darwin_foreground_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_foreground_test PRIVATE raven_cpu "-framework AppKit")
    add_test(NAME native_darwin_foreground COMMAND darwin_foreground_test)
    set_tests_properties(native_darwin_foreground PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_cocoa_policy_test ${RAVEN_DARWIN_DIR}/cocoa_policy_test.mm
        ${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa_policy_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_property(TARGET darwin_cocoa_policy_test PROPERTY OBJCXX_STANDARD 20)
    target_include_directories(darwin_cocoa_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_cocoa_policy_test PRIVATE raven_cpu PkgConfig::XKBCOMMON "-framework AppKit" "-framework Carbon" "-framework Metal")
    add_test(NAME native_darwin_cocoa_policy COMMAND darwin_cocoa_policy_test)
    set_tests_properties(native_darwin_cocoa_policy PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_cocoa_hidden_surface_test ${RAVEN_DARWIN_DIR}/cocoa_hidden_surface_test.mm
        ${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa_hidden_surface_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_property(TARGET darwin_cocoa_hidden_surface_test PROPERTY OBJCXX_STANDARD 20)
    target_include_directories(darwin_cocoa_hidden_surface_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_cocoa_hidden_surface_test PRIVATE raven_cpu PkgConfig::XKBCOMMON "-framework AppKit" "-framework Carbon" "-framework Metal")
    add_test(NAME native_darwin_cocoa_hidden_surface COMMAND darwin_cocoa_hidden_surface_test)
    set_tests_properties(native_darwin_cocoa_hidden_surface PROPERTIES SKIP_RETURN_CODE 77)
    if(RAVEN_DARWIN_PRESENT_TEST)
        add_test(NAME native_darwin_cocoa_surface_transition COMMAND darwin_cocoa_hidden_surface_test --show-transition)
        set_tests_properties(native_darwin_cocoa_surface_transition PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(darwin_cocoa_input_test ${RAVEN_DARWIN_DIR}/cocoa_input_test.mm
        ${RAVEN_DARWIN_DIR}/cocoa.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    set_source_files_properties(${RAVEN_DARWIN_DIR}/cocoa_input_test.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_property(TARGET darwin_cocoa_input_test PROPERTY OBJCXX_STANDARD 20)
    target_include_directories(darwin_cocoa_input_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_cocoa_input_test PRIVATE raven_cpu PkgConfig::XKBCOMMON "-framework AppKit" "-framework Carbon" "-framework Metal")
    add_test(NAME native_darwin_cocoa_input COMMAND darwin_cocoa_input_test)
    set_tests_properties(native_darwin_cocoa_input PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_reexec_test ${RAVEN_DARWIN_DIR}/reexec_test.mm ${RAVEN_DARWIN_DIR}/app_identity.mm)
    target_include_directories(darwin_reexec_test PRIVATE ${CMAKE_SOURCE_DIR}/native)
    target_link_libraries(darwin_reexec_test PRIVATE raven_cpu "-framework AppKit")
    set_property(TARGET darwin_reexec_test PROPERTY OBJCXX_STANDARD 20)
    add_test(NAME native_darwin_reexec COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/reexec.py $<TARGET_FILE:darwin_reexec_test>)
    set_tests_properties(native_darwin_reexec PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(darwin_controller_bench ${RAVEN_DARWIN_DIR}/controller_bench.cpp)
    target_link_libraries(darwin_controller_bench PRIVATE raven_cpu)
    add_executable(darwin_runtime_test ${RAVEN_DARWIN_DIR}/compat_test.cpp)
    target_link_libraries(darwin_runtime_test PRIVATE raven_cpu)
    add_test(NAME native_darwin_runtime COMMAND darwin_runtime_test $<TARGET_FILE:raven-bootstrap>)
    set_tests_properties(native_darwin_runtime PROPERTIES TIMEOUT 15)
    if(RAVEN_MINGW64)
        set(DARWIN_SYSTEM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/darwin-system.exe)
        add_custom_command(OUTPUT ${DARWIN_SYSTEM_FIXTURE}
            COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x1c7000000,--no-insert-timestamp
                -o ${DARWIN_SYSTEM_FIXTURE} ${RAVEN_DARWIN_DIR}/system_client.c -lkernel32 -ladvapi32
            DEPENDS ${RAVEN_DARWIN_DIR}/system_client.c VERBATIM)
        add_custom_target(darwin_system_fixture ALL DEPENDS ${DARWIN_SYSTEM_FIXTURE})
        add_test(NAME native_darwin_system_api COMMAND raven --run-process --execution=native ${DARWIN_SYSTEM_FIXTURE})
        set_tests_properties(native_darwin_system_api PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
            ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-system-prefix")
    endif()
endif()

if(TARGET raven_cpu AND RAVEN_MINGW64)
    set(THEME_API_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/darwin-theme-api.exe)
    add_custom_command(OUTPUT ${THEME_API_FIXTURE}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -Wl,--entry,raven_entry
            -o ${THEME_API_FIXTURE} ${RAVEN_DARWIN_DIR}/tests/theme_api.c -luxtheme -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/theme_api.c VERBATIM)
    add_custom_target(darwin_theme_api_fixture ALL DEPENDS ${THEME_API_FIXTURE})
    add_test(NAME native_darwin_theme_api COMMAND raven --run-process --execution=native ${THEME_API_FIXTURE})
    set_tests_properties(native_darwin_theme_api PROPERTIES TIMEOUT 20
        PASS_REGULAR_EXPRESSION "Native entry returned 0 " LABELS "rosetta"
        ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-theme-api-prefix")
    set(OLE_PICTURE_BOUNDS_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/darwin-ole-picture-bounds.exe)
    add_custom_command(OUTPUT ${OLE_PICTURE_BOUNDS_FIXTURE}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
            -o ${OLE_PICTURE_BOUNDS_FIXTURE} ${RAVEN_DARWIN_DIR}/tests/ole_picture_bounds.c -loleaut32 -lole32 -luuid -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/ole_picture_bounds.c VERBATIM)
    add_custom_target(darwin_ole_picture_bounds_fixture ALL DEPENDS ${OLE_PICTURE_BOUNDS_FIXTURE})
    add_test(NAME native_darwin_ole_picture_bounds COMMAND raven --run-process --execution=native ${OLE_PICTURE_BOUNDS_FIXTURE})
    set_tests_properties(native_darwin_ole_picture_bounds PROPERTIES TIMEOUT 30
        PASS_REGULAR_EXPRESSION "Native entry returned 0 "
        ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-ole-picture-prefix;RAVEN_PREFIX_SETUP=1")
    set(CURSOR_DPI_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-aware.exe)
    set(CURSOR_UNAWARE_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-unaware.exe)
    set(CURSOR_PM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-per-monitor.exe)
    set(CURSOR_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/resources.o)
    set(CURSOR_PM_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/manifest.o)
    add_custom_command(OUTPUT ${CURSOR_RESOURCES} ${CURSOR_PM_RESOURCES}
        COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/cursor_dpi_resources.py ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources
        COMMAND ${RAVEN_WINDRES} ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/resources.rc ${CURSOR_RESOURCES}
        COMMAND ${RAVEN_WINDRES} -DPER_MONITOR_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-resources/resources.rc ${CURSOR_PM_RESOURCES}
        DEPENDS ${RAVEN_DARWIN_DIR}/cursor_dpi_resources.py VERBATIM)
    foreach(mode IN ITEMS aware unaware per-monitor)
        set(dpi_definitions)
        set(dpi_resources ${CURSOR_RESOURCES})
        if(mode STREQUAL "aware")
            set(dpi_definitions -DAWARE)
        endif()
        if(mode STREQUAL "per-monitor")
            set(dpi_definitions -DAWARE -DPER_MONITOR_MANIFEST)
            set(dpi_resources ${CURSOR_PM_RESOURCES})
        endif()
        add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-${mode}.exe
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector ${dpi_definitions}
                -Wl,--entry,mainCRTStartup -o ${CMAKE_CURRENT_BINARY_DIR}/cursor-dpi-${mode}.exe
                ${RAVEN_DARWIN_DIR}/cursor_dpi_fixture.c ${dpi_resources} -lkernel32 -luser32 -lgdi32
            DEPENDS ${RAVEN_DARWIN_DIR}/cursor_dpi_fixture.c ${dpi_resources} VERBATIM)
    endforeach()
    add_custom_target(cursor_dpi_fixtures ALL DEPENDS ${CURSOR_DPI_FIXTURE} ${CURSOR_UNAWARE_FIXTURE} ${CURSOR_PM_FIXTURE})
    add_executable(cursor_dpi_test ${RAVEN_DARWIN_DIR}/cursor_dpi_test.cpp)
    target_link_libraries(cursor_dpi_test PRIVATE raven_cpu)
    add_dependencies(cursor_dpi_test cursor_dpi_fixtures raven-bootstrap)
    add_test(NAME native_darwin_cursor_dpi COMMAND cursor_dpi_test $<TARGET_FILE:raven-bootstrap> ${CURSOR_DPI_FIXTURE} ${CURSOR_UNAWARE_FIXTURE} ${CURSOR_PM_FIXTURE})
endif()

if(TARGET raven_cpu)
    add_executable(darwin_gdi_presenter_test ${RAVEN_DARWIN_DIR}/gdi_presenter_test.mm ${RAVEN_DARWIN_DIR}/gdi_presenter.mm)
    target_link_libraries(darwin_gdi_presenter_test PRIVATE raven_cpu "-framework Metal")
    set_property(TARGET darwin_gdi_presenter_test PROPERTY OBJCXX_STANDARD 20)
    add_test(NAME native_darwin_gdi_presenter COMMAND darwin_gdi_presenter_test)
endif()

# Host-only shortcut parser and generated bundle ownership/lifecycle contracts.
add_test(NAME native_darwin_apps COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/tests/apps_test.py)

include(${RAVEN_DARWIN_DIR}/app_compat.cmake)

# Both guest widths exercise the same printer contracts and verify the resulting PDF on the host.
# Orphaned children: a parent host killed outright leaves its child a broken pipe, a signalled process handle and a failed
# completion, and a detached child running.
if(TARGET raven AND RAVEN_MINGW64)
    foreach(width 64 32)
        if(width EQUAL 64)
            set(ORPHAN_COMPILER ${RAVEN_MINGW64})
            set(ORPHAN_ENTRY raven_entry)
        else()
            set(ORPHAN_COMPILER ${RAVEN_MINGW32})
            set(ORPHAN_ENTRY _raven_entry)
        endif()
        if(ORPHAN_COMPILER)
            set(TEARDOWN_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/teardown-wait${width}.exe)
            add_custom_command(OUTPUT ${TEARDOWN_FIXTURE}
                COMMAND ${ORPHAN_COMPILER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,${ORPHAN_ENTRY},--no-insert-timestamp -o ${TEARDOWN_FIXTURE}
                    ${RAVEN_DARWIN_DIR}/tests/teardown_wait.c -lkernel32 -lgcc
                DEPENDS ${RAVEN_DARWIN_DIR}/tests/teardown_wait.c VERBATIM)
            add_custom_target(teardown_wait_fixture${width} ALL DEPENDS ${TEARDOWN_FIXTURE})
            add_test(NAME native_darwin_teardown_wait_x${width} COMMAND ${Python3_EXECUTABLE}
                ${RAVEN_DARWIN_DIR}/tests/teardown_wait.py $<TARGET_FILE:raven> ${TEARDOWN_FIXTURE})
            set_tests_properties(native_darwin_teardown_wait_x${width} PROPERTIES TIMEOUT 30)
            add_test(NAME native_darwin_teardown_multi_x${width} COMMAND ${Python3_EXECUTABLE}
                ${RAVEN_DARWIN_DIR}/tests/teardown_wait.py $<TARGET_FILE:raven> ${TEARDOWN_FIXTURE} multi)
            set_tests_properties(native_darwin_teardown_multi_x${width} PROPERTIES TIMEOUT 30)
            set(CREATION_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/process-creation${width}.exe)
            add_custom_command(OUTPUT ${CREATION_FIXTURE}
                COMMAND ${ORPHAN_COMPILER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,${ORPHAN_ENTRY},--no-insert-timestamp -o ${CREATION_FIXTURE}
                    ${RAVEN_DARWIN_DIR}/tests/process_creation.c -ladvapi32 -lkernel32 -lgcc
                DEPENDS ${RAVEN_DARWIN_DIR}/tests/process_creation.c VERBATIM)
            add_custom_target(process_creation_fixture${width} ALL DEPENDS ${CREATION_FIXTURE})
            add_test(NAME native_darwin_process_creation_x${width} COMMAND raven --run-process
                --execution=native --timeout-ms=15000 ${CREATION_FIXTURE})
            set_tests_properties(native_darwin_process_creation_x${width} PROPERTIES TIMEOUT 20
                PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            set(ORPHAN_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/orphan-contract${width}.exe)
            add_custom_command(OUTPUT ${ORPHAN_FIXTURE}
                COMMAND ${ORPHAN_COMPILER} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,${ORPHAN_ENTRY},--no-insert-timestamp -o ${ORPHAN_FIXTURE}
                    ${RAVEN_DARWIN_DIR}/tests/orphan_contract.c -lkernel32 -lgcc
                DEPENDS ${RAVEN_DARWIN_DIR}/tests/orphan_contract.c VERBATIM)
            add_custom_target(orphan_contract_fixture${width} ALL DEPENDS ${ORPHAN_FIXTURE})
            foreach(orphan_mode RANGE 0 3)
                add_test(NAME native_orphan_${orphan_mode}_x${width} COMMAND ${Python3_EXECUTABLE}
                    ${RAVEN_DARWIN_DIR}/tests/orphan_contract.py $<TARGET_FILE:raven> ${ORPHAN_FIXTURE} ${orphan_mode})
                set_tests_properties(native_orphan_${orphan_mode}_x${width} PROPERTIES TIMEOUT 30)
            endforeach()
        endif()
    endforeach()
endif()

if(TARGET raven AND RAVEN_MINGW64)
    foreach(width 64 32)
        if(width EQUAL 64)
            set(PRINT_COMPILER ${RAVEN_MINGW64})
        else()
            set(PRINT_COMPILER ${RAVEN_MINGW32})
        endif()
        if(PRINT_COMPILER)
            set(PRINT_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/printing${width}.exe)
            add_custom_command(OUTPUT ${PRINT_FIXTURE}
                COMMAND ${PRINT_COMPILER} -O2 -nostdlib -fno-builtin -fno-stack-protector
                    -Wl,--entry,raven_entry -o ${PRINT_FIXTURE}
                    ${PROJECT_SOURCE_DIR}/testdata/native_loader/printing_client.c -luser32 -lgdi32 -lwinspool -lcomdlg32 -lkernel32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/printing_client.c VERBATIM)
            add_custom_target(printing_fixture${width} ALL DEPENDS ${PRINT_FIXTURE})
            add_test(NAME native_printing_pdf${width} COMMAND ${Python3_EXECUTABLE}
                ${RAVEN_DARWIN_DIR}/tests/printing.py $<TARGET_FILE:raven> ${PRINT_FIXTURE}
                ${CMAKE_CURRENT_BINARY_DIR}/printing${width})
            set_tests_properties(native_printing_pdf${width} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
endif()

# Worker processes retain their creator's desktop pixels even with a different DPI declaration.
if(TARGET raven AND RAVEN_MINGW64)
    foreach(width 64 32)
        if(width EQUAL 64)
            set(dpi_compiler ${RAVEN_MINGW64})
            set(dpi_entry raven_entry)
        else()
            set(dpi_compiler ${RAVEN_MINGW32})
            set(dpi_entry _raven_entry)
        endif()
        if(dpi_compiler)
            set(dpi_client ${CMAKE_CURRENT_BINARY_DIR}/desktop-dpi${width}.exe)
            add_custom_command(OUTPUT ${dpi_client}
                COMMAND ${dpi_compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector
                    -Wl,--entry,${dpi_entry},--no-insert-timestamp -o ${dpi_client}
                    ${RAVEN_DARWIN_DIR}/tests/desktop_dpi.c -luser32 -lgdi32 -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/tests/desktop_dpi.c VERBATIM)
            add_custom_target(darwin_desktop_dpi${width}_fixture ALL DEPENDS ${dpi_client})
            add_test(NAME native_darwin_desktop_dpi_x${width} COMMAND raven --run-process --execution=native --desktop=macos ${dpi_client})
            set_tests_properties(native_darwin_desktop_dpi_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/desktop-dpi-prefix)
        endif()
    endforeach()
endif()

# The upstream D3D10.1 fixture expects its fallback component to report unsupported hardware. Keep DXMT's
# hardware path for applications and exercise it separately with darwin_dxmt_device10.
if(TEST native_d3d10_1_x64)
    set_tests_properties(native_d3d10_1_x64 PROPERTIES ENVIRONMENT "RAVEN_DXMT_ROOT=")
endif()

# These upstream fixtures require serial scheduling: the provider checks a serial stub yield, and the input backend
# asserts that its first poll follows drawing. A parallel guest's execution wait can poll during initialization. Keep
# the upstream contracts unchanged; native_parallel_served_contract and the Darwin input contract below cover the
# corresponding parallel behavior.
if(TEST native_provider_contract AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.22")
    set_property(TEST native_provider_contract APPEND PROPERTY ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
endif()
if(TEST native_outer_surface_input)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.22")
        set_property(TEST native_outer_surface_input APPEND PROPERTY ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
    else()
        message(STATUS "Run upstream serial provider/input contracts with RAVEN_PARALLEL unset on CMake < 3.22")
    endif()
    add_executable(darwin_outer_input_test ${RAVEN_DARWIN_DIR}/outer_input_test.cpp)
    target_link_libraries(darwin_outer_input_test PRIVATE raven_cpu)
    add_dependencies(darwin_outer_input_test outer_surface_input_fixture raven-bootstrap)
    add_test(NAME native_darwin_parallel_input COMMAND darwin_outer_input_test $<TARGET_FILE:raven-bootstrap> ${OUTER_SURFACE_INPUT})
    set_tests_properties(native_darwin_parallel_input PROPERTIES TIMEOUT 15 ENVIRONMENT "RAVEN_PARALLEL=1")
    if(RAVEN_MINGW32)
        set(input32 ${CMAKE_CURRENT_BINARY_DIR}/darwin-outer-surface-input32.exe)
        add_custom_command(OUTPUT ${input32}
            COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -DVISIBLE_SURFACE -DINPUT_SURFACE
                -Wl,--entry,raven_entry,--image-base,0x1a400000,--no-insert-timestamp
                -o ${input32} ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c -lkernel32 -luser32 -lgdi32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/outer_surface_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/icon_data.h VERBATIM)
        add_custom_target(darwin_outer_input32_fixture ALL DEPENDS ${input32})
        add_dependencies(darwin_outer_input_test darwin_outer_input32_fixture)
        add_test(NAME native_darwin_parallel_input_x32 COMMAND darwin_outer_input_test $<TARGET_FILE:raven-bootstrap> ${input32})
        set_tests_properties(native_darwin_parallel_input_x32 PROPERTIES TIMEOUT 15 ENVIRONMENT "RAVEN_PARALLEL=1")
    endif()
endif()

include(${RAVEN_DARWIN_DIR}/x87sidecar.cmake)

add_test(NAME native_darwin_teardown_ps_rows COMMAND ${Python3_EXECUTABLE}
    ${RAVEN_DARWIN_DIR}/tests/teardown_wait.py --check-ps)

# Exercise the adapter with the x86_64 Metal ABI used under Rosetta.
set(DXMT_POOL_TEST ${CMAKE_CURRENT_BINARY_DIR}/darwin-dxmt-pool-test)
add_custom_command(OUTPUT ${DXMT_POOL_TEST}
    COMMAND ${CMAKE_C_COMPILER} -arch x86_64 -fblocks
        ${RAVEN_DARWIN_DIR}/dxmt/presentation_pool_test.m
        -framework Foundation -framework Metal -framework IOSurface -o ${DXMT_POOL_TEST}
    DEPENDS ${RAVEN_DARWIN_DIR}/dxmt/presentation_pool_test.m
        ${RAVEN_DARWIN_DIR}/dxmt/presentation.m ${RAVEN_DARWIN_DIR}/dxmt/slot_state.h
        ${RAVEN_DARWIN_DIR}/guest/protocol.h
    VERBATIM)
add_custom_target(darwin_dxmt_pool_test ALL DEPENDS ${DXMT_POOL_TEST})
add_test(NAME native_darwin_dxmt_drawable_pool COMMAND ${Python3_EXECUTABLE}
    ${RAVEN_DARWIN_DIR}/tests/run_rosetta.py ${DXMT_POOL_TEST})
set_tests_properties(native_darwin_dxmt_drawable_pool PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)

add_executable(darwin_fd_budget_test ${RAVEN_DARWIN_DIR}/fd_budget_test.cpp)
add_test(NAME native_darwin_fd_budget COMMAND darwin_fd_budget_test)
set_tests_properties(native_darwin_fd_budget PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)

if(TARGET darwin_unix_call_fixtures)
    set(ADAPTER_PROTOCOL_LIBRARIES)
    set(adapter ${CMAKE_CURRENT_BINARY_DIR}/raven-dxmt-test.dylib)
    add_custom_command(OUTPUT ${adapter}
        COMMAND ${CMAKE_C_COMPILER} -arch x86_64 -dynamiclib -fblocks
            ${RAVEN_DARWIN_DIR}/dxmt/unix_adapter.m ${RAVEN_DARWIN_DIR}/dxmt/presentation.m
            -framework Foundation -framework Metal -framework IOSurface -o ${adapter}
        DEPENDS ${RAVEN_DARWIN_DIR}/dxmt/unix_adapter.m ${RAVEN_DARWIN_DIR}/dxmt/presentation.m
            ${RAVEN_DARWIN_DIR}/guest/protocol.h VERBATIM)
    foreach(version 0 4 5)
        set(library ${CMAKE_CURRENT_BINARY_DIR}/adapter-protocol-${version}.dylib)
        set(adapter_options)
        set(adapter_dependencies)
        if(version EQUAL 5)
            set(adapter_options -DREAL_ADAPTER -Wl,-needed_library,${adapter})
            set(adapter_dependencies ${adapter})
        endif()
        add_custom_command(OUTPUT ${library}
            COMMAND ${CMAKE_C_COMPILER} -arch x86_64 -dynamiclib -DADAPTER_VERSION=${version} ${adapter_options}
                ${RAVEN_DARWIN_DIR}/tests/adapter_protocol_library.c -o ${library}
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/adapter_protocol_library.c ${RAVEN_DARWIN_DIR}/guest/protocol.h ${adapter_dependencies} VERBATIM)
        list(APPEND ADAPTER_PROTOCOL_LIBRARIES ${library})
    endforeach()
    add_custom_target(darwin_adapter_protocol_fixtures ALL DEPENDS ${ADAPTER_PROTOCOL_LIBRARIES})
    add_executable(darwin_adapter_protocol_test ${RAVEN_DARWIN_DIR}/tests/adapter_protocol.cpp)
    target_link_libraries(darwin_adapter_protocol_test PRIVATE raven_cpu)
    add_dependencies(darwin_adapter_protocol_test darwin_adapter_protocol_fixtures darwin_unix_call_fixtures raven-bootstrap)
    add_test(NAME native_darwin_adapter_protocol COMMAND ${Python3_EXECUTABLE}
        ${RAVEN_DARWIN_DIR}/tests/run_rosetta.py $<TARGET_FILE:darwin_adapter_protocol_test>
        $<TARGET_FILE:raven-bootstrap> ${RAVEN_UNIX_CALL_PE} ${ADAPTER_PROTOCOL_LIBRARIES})
    set_tests_properties(native_darwin_adapter_protocol PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
else()
    add_test(NAME native_darwin_adapter_protocol COMMAND ${Python3_EXECUTABLE} -c
        "print('adapter protocol test unavailable: guest loader fixture requires raven_cpu and MinGW'); raise SystemExit(77)")
    set_tests_properties(native_darwin_adapter_protocol PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(TARGET raven_cpu)
    add_executable(darwin_watch_lifetime_test ${RAVEN_DARWIN_DIR}/tests/watch_lifetime.cpp)
    target_link_libraries(darwin_watch_lifetime_test PRIVATE raven_cpu)
    add_test(NAME native_darwin_watch_lifetime COMMAND darwin_watch_lifetime_test)
    set_tests_properties(native_darwin_watch_lifetime PROPERTIES TIMEOUT 15)
endif()

add_test(NAME native_darwin_rosetta_runner COMMAND ${Python3_EXECUTABLE}
    ${RAVEN_DARWIN_DIR}/tests/rosetta_runner_test.py)

add_test(NAME native_darwin_fd_budget_low_hard COMMAND darwin_fd_budget_test --low-hard)
set_tests_properties(native_darwin_fd_budget_low_hard PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
add_library(darwin_fd_budget_observer SHARED ${RAVEN_DARWIN_DIR}/tests/fd_budget_observer.c)
add_dependencies(darwin_fd_budget_test raven darwin_fd_budget_observer)
add_test(NAME native_darwin_fd_budget_startup COMMAND ${Python3_EXECUTABLE}
    ${RAVEN_DARWIN_DIR}/tests/fd_budget_startup.py $<TARGET_FILE:raven> $<TARGET_FILE:darwin_fd_budget_observer>)
set_tests_properties(native_darwin_fd_budget_startup PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)

add_executable(darwin_dxmt_slot_state_test ${RAVEN_DARWIN_DIR}/dxmt/slot_state_test.c)
add_test(NAME native_darwin_dxmt_slot_state COMMAND darwin_dxmt_slot_state_test)

# Exercise the upstream LSA translated-name layouts in both guest pointer widths.
foreach(width 64 32)
    set(lsa_compiler ${RAVEN_MINGW${width}})
    if(lsa_compiler)
        set(lsa_fixture ${CMAKE_CURRENT_BINARY_DIR}/lsa-lookup${width}.exe)
        if(width EQUAL 32)
            set(lsa_entry _raven_entry)
        else()
            set(lsa_entry raven_entry)
        endif()
        add_custom_command(OUTPUT ${lsa_fixture}
            COMMAND ${lsa_compiler} -O2 -nostdlib -fno-builtin -fno-stack-protector
                -Wl,--entry,${lsa_entry} -o ${lsa_fixture} ${RAVEN_DARWIN_DIR}/tests/lsa_lookup.c -ladvapi32 -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/lsa_lookup.c VERBATIM)
        add_custom_target(darwin_lsa_lookup${width} ALL DEPENDS ${lsa_fixture})
        add_test(NAME native_darwin_lsa_lookup_x${width} COMMAND raven --run-process --execution=native ${lsa_fixture})
        set_tests_properties(native_darwin_lsa_lookup_x${width} PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "Native entry returned 0")
    endif()
endforeach()
# Real process fixtures for the opt-in arm64 FEX host. These sources are unchanged;
# no-reloc test executables are linked above Darwin's reserved low address space.
if(RAVEN_FEX_MODHOST AND TARGET raven_cpu AND RAVEN_MINGW64)
    foreach(client kernel-ansi kernel-file kernel-threads)
        if(client STREQUAL "kernel-ansi")
            set(source kernel_ansi_client.c)
        elseif(client STREQUAL "kernel-file")
            set(source kernel_files_client.c)
        else()
            set(source kernel_threads_client.c)
        endif()
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/${client}-fex.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
                -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                -o ${fixture} ${PROJECT_SOURCE_DIR}/testdata/native_loader/${source} -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/${source} VERBATIM)
        add_custom_target(${client}_fex_fixture DEPENDS ${fixture})
        set(prefix_arguments)
        if(client STREQUAL "kernel-file")
            list(APPEND prefix_arguments --prefix ${CMAKE_CURRENT_BINARY_DIR}/fex-kernel-prefix)
        endif()
        add_test(NAME native_${client}_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}" ${prefix_arguments})
        set_tests_properties(native_${client}_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m2;process")
    endforeach()
    if(TARGET kernel_seh_fixture)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh-fex.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
                -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -Wno-inconsistent-dllimport
                -fno-stack-protector -mno-stack-arg-probe -c
                -o ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh-fex.o ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c
            COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                -o ${fixture} ${CMAKE_CURRENT_BINARY_DIR}/kernel-seh-fex.o -lkernel32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c VERBATIM)
        add_custom_target(kernel_seh_fex_fixture DEPENDS ${fixture})
        add_test(NAME native_kernel-seh_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_kernel-seh_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m2;process;seh")
    endif()
endif()

if(RAVEN_FEX_MODHOST AND TARGET raven_cpu AND RAVEN_MINGW64)
    set(FEX_PLACEMENT ${CMAKE_CURRENT_BINARY_DIR}/fex-placement.exe)
    add_custom_command(OUTPUT ${FEX_PLACEMENT}
        COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
            -Wl,--entry,raven_entry,--image-base,0x400000,--no-insert-timestamp
            -o ${FEX_PLACEMENT} ${RAVEN_DARWIN_DIR}/fex/placement_client.c -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/placement_client.c VERBATIM)
    set(FEX_FIXED ${CMAKE_CURRENT_BINARY_DIR}/fex-fixed-low.exe)
    add_custom_command(OUTPUT ${FEX_FIXED}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector
            -Wl,--entry,raven_entry,--image-base,0x8000000000,--no-insert-timestamp
            -o ${FEX_FIXED} ${RAVEN_DARWIN_DIR}/fex/fixed_client.c
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/fixed_client.c VERBATIM)
    add_custom_target(fex_placement_fixtures DEPENDS ${FEX_PLACEMENT} ${FEX_FIXED})
    add_test(NAME native_placement_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${FEX_PLACEMENT} --modules "${RAVEN_FEX_MODULE_ROOT}")
    add_test(NAME native_fixed_image_preferred_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${FEX_FIXED} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_placement_fex native_fixed_image_preferred_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m2;placement")
endif()

if(RAVEN_FEX_MODHOST AND TARGET kernel_seh_fixture)
    set(FEX_WRITE_AV ${CMAKE_CURRENT_BINARY_DIR}/fex-write-av.exe)
    add_custom_command(OUTPUT ${FEX_WRITE_AV}
        COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
            -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -fno-stack-protector -mno-stack-arg-probe
            -c -o ${CMAKE_CURRENT_BINARY_DIR}/fex-write-av.o ${RAVEN_DARWIN_DIR}/fex/write_av_client.c
        # Reuse the original fixture's guest scope-table handler, unchanged.
        COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
            -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -Wno-inconsistent-dllimport
            -fno-stack-protector -mno-stack-arg-probe -Draven_entry=raven_seh_support_entry
            -c -o ${CMAKE_CURRENT_BINARY_DIR}/fex-seh-support.o ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c
        COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
            -o ${FEX_WRITE_AV} ${CMAKE_CURRENT_BINARY_DIR}/fex-write-av.o ${CMAKE_CURRENT_BINARY_DIR}/fex-seh-support.o -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/write_av_client.c ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c VERBATIM)
    add_custom_target(fex_write_av_fixture DEPENDS ${FEX_WRITE_AV})
    add_test(NAME native_write_av_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${FEX_WRITE_AV} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_write_av_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m2;seh")
endif()

if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    add_test(NAME fex_process_runner_contract COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process-runner.py)
    set_tests_properties(fex_process_runner_contract PROPERTIES TIMEOUT 20 LABELS "fex;contract")
    add_test(NAME native_internal_brk_rejection_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-rejection.py
        --host $<TARGET_FILE:fex_process_test> --fixture ${FEX_ENTRY_FIXTURE} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_internal_brk_rejection_fex PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 LABELS "fex;m2;fault")
    add_test(NAME native_empty_module_argument_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-ansi-fex.exe --modules "")
    set_tests_properties(native_empty_module_argument_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;contract"
        ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
    if(TEST native_kernel-file_fex)
        add_test(NAME native_kernel_file_persistence_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-file-fex.exe --modules "${RAVEN_FEX_MODULE_ROOT}"
            --prefix ${CMAKE_CURRENT_BINARY_DIR}/fex-kernel-prefix --check-persistence)
        set_tests_properties(native_kernel-file_fex PROPERTIES FIXTURES_SETUP fex_file_store)
        set_tests_properties(native_kernel_file_persistence_fex PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 LABELS "fex;m2;process"
            DEPENDS native_kernel-file_fex FIXTURES_REQUIRED fex_file_store)
    endif()
endif()

if(RAVEN_FEX_MODHOST AND TARGET kernel_seh_fixture)
    set(FEX_REVIEW_SOURCE ${RAVEN_DARWIN_DIR}/fex/review_exceptions_client.c)
    set(FEX_REVIEW_SUPPORT ${CMAKE_CURRENT_BINARY_DIR}/fex-review-seh-support.o)
    add_custom_command(OUTPUT ${FEX_REVIEW_SUPPORT}
        COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
            -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -Wno-inconsistent-dllimport
            -fno-stack-protector -mno-stack-arg-probe -Draven_entry=raven_seh_support_entry
            -c -o ${FEX_REVIEW_SUPPORT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c
        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c VERBATIM)
    set(review_case 0)
    set(review_outputs)
    foreach(contract breakpoint-seh read-av nested-seh unhandled subpage guard-unsupported divide-overflow)
        math(EXPR review_case "${review_case} + 1")
        foreach(backend fex native)
            set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-review-${contract}-${backend}.exe)
            if(backend STREQUAL "native")
                set(expect_subpage 1)
                set(label "m2;rosetta;review")
            else()
                set(expect_subpage 0)
                set(label "fex;m2;review")
            endif()
            add_custom_command(OUTPUT ${fixture}
                COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
                    -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -fno-stack-protector -mno-stack-arg-probe
                    -DREVIEW_CASE=${review_case} -DEXPECT_SUBPAGE_FAULT=${expect_subpage}
                    -c -o ${fixture}.o ${FEX_REVIEW_SOURCE}
                COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                    -o ${fixture} ${fixture}.o ${FEX_REVIEW_SUPPORT} -lkernel32
                DEPENDS ${FEX_REVIEW_SOURCE} ${FEX_REVIEW_SUPPORT} VERBATIM)
            list(APPEND review_outputs ${fixture})
            set(status_arguments)
            if(contract STREQUAL "unhandled")
                list(APPEND status_arguments --unhandled-status 0xe0424242)
            endif()
            add_test(NAME native_review_${contract}_${backend} COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
                --raven $<TARGET_FILE:raven> --fixture ${fixture} --backend ${backend} --modules "${RAVEN_FEX_MODULE_ROOT}" ${status_arguments})
            set_tests_properties(native_review_${contract}_${backend} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "${label}")
        endforeach()
    endforeach()
    add_custom_target(fex_review_fixtures DEPENDS ${review_outputs})
endif()

if(RAVEN_MINGW64)
    set(thread_context_fixture ${CMAKE_CURRENT_BINARY_DIR}/thread-context.exe)
    add_custom_command(OUTPUT ${thread_context_fixture}
        COMMAND ${RAVEN_MINGW64} -nostdlib -O1 -fno-builtin -fno-stack-protector
            -Wl,--entry,raven_entry -o ${thread_context_fixture} ${RAVEN_DARWIN_DIR}/tests/thread_context.c -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/thread_context.c VERBATIM)
    add_custom_target(darwin_thread_context ALL DEPENDS ${thread_context_fixture})
    add_test(NAME native_darwin_thread_context COMMAND raven --run-process --execution=native ${thread_context_fixture})
    set_tests_properties(native_darwin_thread_context PROPERTIES TIMEOUT 30 ENVIRONMENT "RAVEN_PARALLEL=1" PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
endif()

# M3 guest contracts require real concurrent FEX workers, with cooperative slices disabled.
if(RAVEN_FEX_MODHOST AND TARGET fex_review_fixtures)
    set(m3_case 0)
    set(m3_outputs)
    foreach(contract contention context worker-seh apc lifecycle worker-unhandled negatives worker-av)
        math(EXPR m3_case "${m3_case} + 1")
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-${contract}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
                -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -fno-stack-protector -mno-stack-arg-probe
                -DM3_CASE=${m3_case} -c -o ${fixture}.o ${RAVEN_DARWIN_DIR}/fex/parallel_client.c
            COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                -o ${fixture} ${fixture}.o ${FEX_REVIEW_SUPPORT} -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/fex/parallel_client.c ${FEX_REVIEW_SUPPORT} VERBATIM)
        list(APPEND m3_outputs ${fixture})
        set(m3_status)
        if(contract STREQUAL "worker-unhandled")
            list(APPEND m3_status --unhandled-status 0xe0424242)
        elseif(contract STREQUAL "worker-av")
            list(APPEND m3_status --unhandled-status 0xc0000005)
        endif()
        add_test(NAME native_parallel_${contract}_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}" ${m3_status})
        set_tests_properties(native_parallel_${contract}_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m3;parallel"
            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
    endforeach()
    add_test(NAME native_cooperative_context_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-context.exe --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_cooperative_context_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m3"
        ENVIRONMENT "RAVEN_SLICE_MS=1" ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
    foreach(mode cooperative parallel)
        add_test(NAME native_m3_negatives_${mode} COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --backend native --fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-negatives.exe)
        set_tests_properties(native_m3_negatives_${mode} PROPERTIES TIMEOUT 30 LABELS "m3;threads")
    endforeach()
    set_tests_properties(native_m3_negatives_cooperative PROPERTIES ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
    set_tests_properties(native_m3_negatives_parallel PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1")
    add_custom_target(fex_parallel_fixtures DEPENDS ${m3_outputs})
    add_test(NAME native_parallel_kernel_threads_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads-fex.exe --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_parallel_kernel_threads_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m3;parallel"
        ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
endif()
if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    foreach(contract timeout selfmodify wait-unwind)
        add_test(NAME native_parallel_${contract}_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${contract})
        set_tests_properties(native_parallel_${contract}_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
            LABELS "fex;m3;parallel" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
    endforeach()
endif()

# x18 preservation is task-wide, selected from the main executable signature SDK.
# Keep the Cocoa/Rosetta executable modern; only explicit FEX launches exec this sibling.
if(RAVEN_FEX_MODHOST AND TARGET raven_cpu)
    target_sources(raven PRIVATE ${RAVEN_DARWIN_DIR}/fex/launch.cpp)
    get_target_property(_raven_sources raven SOURCES)
    add_executable(raven-fex-host ${_raven_sources})
    foreach(property LINK_LIBRARIES LINK_OPTIONS COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
            C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
            OBJC_STANDARD OBJC_STANDARD_REQUIRED OBJC_EXTENSIONS OBJCXX_STANDARD OBJCXX_STANDARD_REQUIRED OBJCXX_EXTENSIONS)
        get_target_property(value raven ${property})
        if(NOT value STREQUAL "value-NOTFOUND")
            set_property(TARGET raven-fex-host PROPERTY ${property} "${value}")
        endif()
    endforeach()
    target_sources(raven-fex-host PRIVATE ${RAVEN_DARWIN_DIR}/fex/reserve.s)
    target_link_options(raven-fex-host PRIVATE -Wl,-segaddr,RAVEN_PE,0x110000000 -Wl,-segprot,RAVEN_PE,rw,rw)
    target_compile_definitions(raven-fex-host PRIVATE RAVEN_FEX_COMPANION_HOST=1)
    option(RAVEN_FEX_PLACEMENT_TEST_HOOKS "Enable test-only FEX startup pressure hooks" OFF)
    if(RAVEN_FEX_PLACEMENT_TEST_HOOKS)
        target_compile_definitions(raven-fex-host PRIVATE RAVEN_FEX_PLACEMENT_TEST_HOOKS=1)
    endif()
    target_link_options(raven-fex-host PRIVATE -Wl,-platform_version,macos,11.0,11.0)
    add_dependencies(raven raven-fex-host)
    # FEX fixtures used by registered tests must be available in a normal build.
    foreach(fixture fex_entry_fixture kernel-ansi_fex_fixture kernel-file_fex_fixture kernel-threads_fex_fixture
            kernel_seh_fex_fixture fex_placement_fixtures fex_write_av_fixture fex_review_fixtures fex_parallel_fixtures)
        if(TARGET ${fixture})
            add_dependencies(raven ${fixture})
        endif()
    endforeach()
endif()
if(RAVEN_FEX_MODHOST AND TARGET kernel-ansi_fex_fixture)
    add_test(NAME native_fex_default_discovery COMMAND ${Python3_EXECUTABLE}
        ${RAVEN_DARWIN_DIR}/fex/default_build_test.py $<TARGET_FILE:raven>
        ${CMAKE_CURRENT_BINARY_DIR}/kernel-ansi-fex.exe "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_fex_default_discovery PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77 LABELS "fex;contract")
endif()
if(TARGET raven-fex-host)
    add_test(NAME darwin_fex_launch COMMAND ${Python3_EXECUTABLE}
        ${RAVEN_DARWIN_DIR}/fex/launch_test.py $<TARGET_FILE:raven>)
    set_tests_properties(darwin_fex_launch PROPERTIES TIMEOUT 60 LABELS "fex;contract;darwin")
    add_executable(darwin_fex_child_executable_test ${RAVEN_DARWIN_DIR}/fex/child_executable_test.cpp)
    target_include_directories(darwin_fex_child_executable_test PRIVATE ${PROJECT_SOURCE_DIR}/native)
    add_test(NAME darwin_fex_child_executable COMMAND darwin_fex_child_executable_test)
    set_tests_properties(darwin_fex_child_executable PROPERTIES LABELS "fex;contract;darwin")
endif()
include(${RAVEN_DARWIN_DIR}/fex/parity.cmake)

set(RAVEN_MULLION_ROOT "" CACHE PATH "Private Mullion component root; select with RAVEN_D3D12_BACKEND=mullion")
if(RAVEN_MULLION_ROOT)
    set_property(TARGET raven_cpu APPEND PROPERTY COMPILE_DEFINITIONS RAVEN_MULLION_ROOT="${RAVEN_MULLION_ROOT}")
endif()

if(RAVEN_MULLION_ROOT AND RAVEN_MINGW64)
    foreach(workload device clear draw window12)
        set(_fixture "${CMAKE_CURRENT_BINARY_DIR}/mullion-${workload}.exe")
        add_custom_command(OUTPUT "${_fixture}"
            COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin -minline-all-stringops -mno-stack-arg-probe
                -Wl,--entry,entry,--no-insert-timestamp -o "${_fixture}"
                ${PROJECT_SOURCE_DIR}/testdata/d3d12/${workload}.c -ld3d12 -ldxgi -ldxguid -lkernel32 -luser32 -lgcc
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d12/${workload}.c ${PROJECT_SOURCE_DIR}/testdata/d3d12/tri5_shaders.h
            VERBATIM)
        add_custom_target(mullion_${workload}_fixture DEPENDS "${_fixture}")
        add_dependencies(raven mullion_${workload}_fixture)
        foreach(engine native fex)
            add_test(NAME darwin_mullion_${workload}_${engine}
                COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/mullion/run.py
                    $<TARGET_FILE:raven> "${_fixture}" ${RAVEN_MULLION_ROOT}
                    ${CMAKE_CURRENT_BINARY_DIR}/mullion-${workload}-${engine} ${workload} ${engine})
            set_tests_properties(darwin_mullion_${workload}_${engine} PROPERTIES TIMEOUT 100)
        endforeach()
    endforeach()
endif()
# Backend-neutral D3D9 clear/triangle/present/readback workloads for private
# mtld3d qualification. Do not require a DXVK installation or select a driver.
foreach(width 32 64)
    if(RAVEN_MINGW${width})
        set(_mtld3d_fixture ${CMAKE_CURRENT_BINARY_DIR}/raven-mtld3d${width}.exe)
        add_custom_command(OUTPUT ${_mtld3d_fixture}
            COMMAND ${RAVEN_MINGW${width}} -O2 -nostdlib -fno-stack-protector -fno-builtin
                -Wl,--entry,raven_entry,--no-insert-timestamp
                -o ${_mtld3d_fixture} ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c -lkernel32 -luser32
            DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d9/window.c VERBATIM)
        add_custom_target(darwin_mtld3d${width}_fixture DEPENDS ${_mtld3d_fixture})
        if(RAVEN_FEX_MODHOST)
            add_test(NAME darwin_mtld3d${width}_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_DARWIN_DIR}/mtld3d/run_fixture.py
                --raven $<TARGET_FILE:raven> --fixture ${_mtld3d_fixture} --width ${width} --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(darwin_mtld3d${width}_fex PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77 RESOURCE_LOCK raven_compositor)
            set(_runtime_fixture ${CMAKE_CURRENT_BINARY_DIR}/raven-mtld3d-runtime${width}.exe)
            add_custom_command(OUTPUT ${_runtime_fixture}
                COMMAND ${RAVEN_MINGW${width}} -O2 -nostdlib -fno-stack-protector -fno-builtin
                    -Wl,--entry,raven_entry,--no-insert-timestamp -o ${_runtime_fixture}
                    ${PROJECT_SOURCE_DIR}/testdata/d3d9/runtime.c -lkernel32 -luser32
                DEPENDS ${PROJECT_SOURCE_DIR}/testdata/d3d9/runtime.c VERBATIM)
            add_custom_target(darwin_mtld3d_runtime${width}_fixture DEPENDS ${_runtime_fixture})
            add_test(NAME darwin_mtld3d_runtime${width}_fex COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
                --raven $<TARGET_FILE:raven> --fixture ${_runtime_fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(darwin_mtld3d_runtime${width}_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
        endif()
    endif()
endforeach()
include(${RAVEN_DARWIN_DIR}/fex/placement.cmake)

add_executable(prefix_copy_test ${RAVEN_DARWIN_DIR}/tests/prefix_copy.cpp)
target_link_libraries(prefix_copy_test PRIVATE raven_cpu)
target_include_directories(prefix_copy_test PRIVATE ${PROJECT_SOURCE_DIR}/native)
target_compile_options(prefix_copy_test PRIVATE -UNDEBUG)
add_test(NAME native_prefix_copy COMMAND prefix_copy_test)
if(RAVEN_MINGW64)
    set(_crt_locale "${CMAKE_CURRENT_BINARY_DIR}/crt-locale.exe")
    add_custom_command(OUTPUT "${_crt_locale}"
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-builtin -fno-stack-protector
            -Wl,--entry,entry,--no-insert-timestamp -o "${_crt_locale}"
            ${RAVEN_DARWIN_DIR}/mullion/crt_locale.c -lkernel32 -lgcc
        DEPENDS ${RAVEN_DARWIN_DIR}/mullion/crt_locale.c VERBATIM)
    add_custom_target(darwin_crt_locale_fixture DEPENDS "${_crt_locale}")
    add_dependencies(raven darwin_crt_locale_fixture)
    foreach(engine native fex)
        add_test(NAME darwin_crt_locale_${engine} COMMAND raven --run-process --execution=${engine} "${_crt_locale}")
        set_tests_properties(darwin_crt_locale_${engine} PROPERTIES TIMEOUT 60)
    endforeach()
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_buffer_test "${CMAKE_CURRENT_BINARY_DIR}/dxmt-buffer-registry-test")
    add_custom_command(OUTPUT "${_buffer_test}"
        COMMAND /usr/bin/clang -arch arm64 -O2 -fblocks
            ${RAVEN_DARWIN_DIR}/dxmt/buffer_registry_test.m
            -framework Foundation -framework Metal -o "${_buffer_test}"
        DEPENDS ${RAVEN_DARWIN_DIR}/dxmt/buffer_registry_test.m ${RAVEN_DARWIN_DIR}/dxmt/unix_adapter.m VERBATIM)
    add_custom_target(darwin_dxmt_buffer_registry_fixture ALL DEPENDS "${_buffer_test}")
    add_test(NAME darwin_dxmt_buffer_registry COMMAND "${_buffer_test}")
endif()
