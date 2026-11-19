# The FEX backend's tests, after the fixtures native/platform/darwin/darwin.cmake builds.

if(TARGET raven_cpu AND RAVEN_MINGW64 AND RAVEN_DXMT_ROOT)
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
        add_test(NAME native_${client}_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}" ${prefix_arguments})
        set_tests_properties(native_${client}_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;process")
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
        add_test(NAME native_kernel-seh_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_kernel-seh_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;process;seh")
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
    add_test(NAME native_placement_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${FEX_PLACEMENT} --modules "${RAVEN_FEX_MODULE_ROOT}")
    add_test(NAME native_fixed_image_preferred_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${FEX_FIXED} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_placement_fex native_fixed_image_preferred_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;placement")
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
    add_test(NAME native_write_av_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${FEX_WRITE_AV} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_write_av_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;seh")
endif()

if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    add_test(NAME fex_process_runner_contract COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process-runner.py)
    set_tests_properties(fex_process_runner_contract PROPERTIES TIMEOUT 20 LABELS "fex;contract")
    add_test(NAME native_internal_brk_rejection_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-rejection.py
        --host $<TARGET_FILE:fex_process_test> --fixture ${FEX_ENTRY_FIXTURE} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_internal_brk_rejection_fex PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 LABELS "fex;fault")
    add_test(NAME native_empty_module_argument_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-ansi-fex.exe --modules "")
    set_tests_properties(native_empty_module_argument_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;contract"
        ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
    if(TEST native_kernel-file_fex)
        add_test(NAME native_kernel_file_persistence_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-file-fex.exe --modules "${RAVEN_FEX_MODULE_ROOT}"
            --prefix ${CMAKE_CURRENT_BINARY_DIR}/fex-kernel-prefix --check-persistence)
        set_tests_properties(native_kernel-file_fex PROPERTIES FIXTURES_SETUP fex_file_store)
        set_tests_properties(native_kernel_file_persistence_fex PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77 LABELS "fex;process"
            DEPENDS native_kernel-file_fex FIXTURES_REQUIRED fex_file_store)
    endif()
endif()

if(RAVEN_FEX_MODHOST AND TARGET kernel_seh_fixture)
    set(FEX_EXCEPTION_SOURCE ${RAVEN_DARWIN_DIR}/fex/exceptions_client.c)
    set(FEX_SEH_SUPPORT ${CMAKE_CURRENT_BINARY_DIR}/fex-seh-support.o)
    add_custom_command(OUTPUT ${FEX_SEH_SUPPORT}
        COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
            -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -Wno-inconsistent-dllimport
            -fno-stack-protector -mno-stack-arg-probe -Draven_entry=raven_seh_support_entry
            -c -o ${FEX_SEH_SUPPORT} ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c
        DEPENDS ${PROJECT_SOURCE_DIR}/testdata/native_loader/kernel_seh_client.c VERBATIM)
    set(exception_case 0)
    set(exception_outputs)
    foreach(contract breakpoint-seh read-av nested-seh unhandled subpage guard-unsupported divide-overflow)
        math(EXPR exception_case "${exception_case} + 1")
        foreach(backend fex native)
            set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-exception-${contract}-${backend}.exe)
            if(backend STREQUAL "native")
                set(expect_subpage 1)
                set(label "rosetta;seh")
            else()
                set(expect_subpage 0)
                set(label "fex;seh")
            endif()
            add_custom_command(OUTPUT ${fixture}
                COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
                    -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -fno-stack-protector -mno-stack-arg-probe
                    -DRAVEN_TEST_EXCEPTION_CASE=${exception_case} -DEXPECT_SUBPAGE_FAULT=${expect_subpage}
                    -c -o ${fixture}.o ${FEX_EXCEPTION_SOURCE}
                COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                    -o ${fixture} ${fixture}.o ${FEX_SEH_SUPPORT} -lkernel32
                DEPENDS ${FEX_EXCEPTION_SOURCE} ${FEX_SEH_SUPPORT} VERBATIM)
            list(APPEND exception_outputs ${fixture})
            set(status_arguments)
            if(contract STREQUAL "unhandled")
                list(APPEND status_arguments --unhandled-status 0xe0424242)
            endif()
            add_test(NAME native_exception_${contract}_${backend} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
                --raven $<TARGET_FILE:raven> --fixture ${fixture} --backend ${backend} --modules "${RAVEN_FEX_MODULE_ROOT}" ${status_arguments})
            set_tests_properties(native_exception_${contract}_${backend} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "${label}")
        endforeach()
    endforeach()
    add_custom_target(fex_exception_fixtures DEPENDS ${exception_outputs})
endif()

# Parallel guest contracts require concurrent FEX workers, with cooperative slices disabled.
if(RAVEN_FEX_MODHOST AND TARGET fex_exception_fixtures)
    set(parallel_case 0)
    set(parallel_outputs)
    foreach(contract contention context worker-seh apc lifecycle worker-unhandled negatives worker-av)
        math(EXPR parallel_case "${parallel_case} + 1")
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-${contract}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
                -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -fno-stack-protector -mno-stack-arg-probe
                -DRAVEN_TEST_PARALLEL_CASE=${parallel_case} -c -o ${fixture}.o ${RAVEN_DARWIN_DIR}/fex/parallel_client.c
            COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
                -o ${fixture} ${fixture}.o ${FEX_SEH_SUPPORT} -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/fex/parallel_client.c ${FEX_SEH_SUPPORT} VERBATIM)
        list(APPEND parallel_outputs ${fixture})
        set(parallel_status)
        if(contract STREQUAL "worker-unhandled")
            list(APPEND parallel_status --unhandled-status 0xe0424242)
        elseif(contract STREQUAL "worker-av")
            list(APPEND parallel_status --unhandled-status 0xc0000005)
        endif()
        add_test(NAME native_parallel_${contract}_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}" ${parallel_status})
        set_tests_properties(native_parallel_${contract}_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;parallel"
            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
    endforeach()
    add_test(NAME native_cooperative_context_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-context.exe --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_cooperative_context_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;threads"
        ENVIRONMENT "RAVEN_SLICE_MS=1" ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
    foreach(mode cooperative parallel)
        add_test(NAME native_thread_sync_contract_${mode} COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
            --raven $<TARGET_FILE:raven> --backend native --fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-negatives.exe)
        set_tests_properties(native_thread_sync_contract_${mode} PROPERTIES TIMEOUT 30 LABELS "rosetta;threads")
    endforeach()
    set_tests_properties(native_thread_sync_contract_cooperative PROPERTIES ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
    set_tests_properties(native_thread_sync_contract_parallel PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1")
    add_custom_target(fex_parallel_fixtures DEPENDS ${parallel_outputs})
    add_test(NAME native_parallel_kernel_threads_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-threads-fex.exe --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_parallel_kernel_threads_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;parallel"
        ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
endif()
if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    foreach(contract timeout selfmodify flush-only wait-unwind)
        add_test(NAME native_parallel_${contract}_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${contract})
        set_tests_properties(native_parallel_${contract}_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
            LABELS "fex;parallel" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
    endforeach()
endif()

# x18 preservation is task-wide, selected from the main executable signature SDK.
# Keep the Cocoa/Rosetta executable modern; only explicit FEX launches exec this sibling.
if(RAVEN_FEX_MODHOST AND TARGET raven_cpu)
    target_sources(raven-runtime PRIVATE ${RAVEN_DARWIN_DIR}/fex/launch.cpp)
    get_target_property(_raven_sources raven-runtime SOURCES)
    add_executable(raven-fex-host ${_raven_sources})
    foreach(property LINK_LIBRARIES LINK_OPTIONS COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
            C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS
            OBJC_STANDARD OBJC_STANDARD_REQUIRED OBJC_EXTENSIONS OBJCXX_STANDARD OBJCXX_STANDARD_REQUIRED OBJCXX_EXTENSIONS)
        get_target_property(value raven-runtime ${property})
        if(NOT value STREQUAL "value-NOTFOUND")
            set_property(TARGET raven-fex-host PROPERTY ${property} "${value}")
        endif()
    endforeach()
    target_sources(raven-fex-host PRIVATE ${RAVEN_DARWIN_DIR}/fex/reserve.s)
    target_link_options(raven-fex-host PRIVATE -Wl,-segaddr,RAVEN_PE,0x110000000 -Wl,-segprot,RAVEN_PE,rw,rw)
    target_compile_definitions(raven-fex-host PRIVATE RAVEN_FEX_COMPANION_HOST=1)
    option(RAVEN_FEX_PLACEMENT_TEST_HOOKS "Enable test-only FEX startup pressure hooks" OFF)
    if(RAVEN_FEX_PLACEMENT_TEST_HOOKS)
        add_executable(raven-fex-placement-host EXCLUDE_FROM_ALL ${_raven_sources} ${RAVEN_DARWIN_DIR}/fex/reserve.s)
        foreach(property LINK_LIBRARIES LINK_OPTIONS COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES
                C_STANDARD C_STANDARD_REQUIRED CXX_STANDARD CXX_STANDARD_REQUIRED OBJCXX_STANDARD OBJCXX_STANDARD_REQUIRED)
            get_target_property(value raven-fex-host ${property})
            if(NOT value STREQUAL "value-NOTFOUND")
                set_property(TARGET raven-fex-placement-host PROPERTY ${property} "${value}")
            endif()
        endforeach()
        target_compile_definitions(raven-fex-placement-host PRIVATE RAVEN_FEX_PLACEMENT_TEST_HOOKS=1)
        target_link_options(raven-fex-placement-host PRIVATE -Wl,-platform_version,macos,11.0,11.0)
    endif()
    target_link_options(raven-fex-host PRIVATE -Wl,-platform_version,macos,11.0,11.0)
    add_dependencies(raven raven-fex-host)
    # FEX fixtures used by registered tests must be available in a normal build.
    foreach(fixture fex_entry_fixture kernel-ansi_fex_fixture kernel-file_fex_fixture kernel-threads_fex_fixture
            kernel_seh_fex_fixture fex_placement_fixtures fex_write_av_fixture fex_exception_fixtures fex_parallel_fixtures)
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

set(RAVEN_MTLD3D_TEST_ROOT "$ENV{RAVEN_MTLD3D_ARM64_ROOT}" CACHE PATH "Private mtld3d overlay for unix range tests")
if(RAVEN_FEX_MODHOST AND TARGET darwin_unix_fault_test AND EXISTS "${RAVEN_FEX_MODULE_ROOT}/xtajit64.dll")
    if(EXISTS "${RAVEN_MTLD3D_TEST_ROOT}/mtld3d.so")
        foreach(width 32 64)
            if(width EQUAL 32 AND NOT EXISTS "${RAVEN_FEX_MODULE_ROOT}/xtajit.dll")
                continue()
            endif()
            add_test(NAME darwin_unix_ranges_mtld3d_${width}_fex COMMAND darwin_unix_fault_test
                $<TARGET_FILE:raven-bootstrap> ${CMAKE_CURRENT_BINARY_DIR}/unix-fault-${width}.exe
                "${RAVEN_MTLD3D_TEST_ROOT}" ${width} mtld3d fex)
            set_tests_properties(darwin_unix_ranges_mtld3d_${width}_fex PROPERTIES TIMEOUT 30
                ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
        endforeach()
    endif()
endif()

if(TARGET fex_process_test)
    add_test(NAME native_vector_context_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} vector-context)
    set_tests_properties(native_vector_context_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;context"
        ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
endif()
if(RAVEN_FEX_MODHOST AND TARGET raven_cpu AND RAVEN_MINGW64)
    # Multiblock call-ret stacks: a thread that entered F through a block link
    # must not return into F's translation after another thread rewrote F.
    set(fixture ${CMAKE_CURRENT_BINARY_DIR}/multiblock-callret-fex.exe)
    add_custom_command(OUTPUT ${fixture}
        COMMAND ${RAVEN_MINGW64} -O2 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
            -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
            -o ${fixture} ${RAVEN_DARWIN_DIR}/fex/multiblock_callret_client.c -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/multiblock_callret_client.c VERBATIM)
    add_custom_target(multiblock_callret_fex_fixture ALL DEPENDS ${fixture})
    add_test(NAME native_multiblock_callret_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
        --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_multiblock_callret_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;process"
        ENVIRONMENT RAVEN_PARALLEL=1)

    # Opt-in code cache: two same-named, same-sized DLL variants and a JIT/SMC client.
    set(code_cache_dir ${CMAKE_CURRENT_BINARY_DIR}/code-cache)
    set(code_cache_outputs ${code_cache_dir}/codecache-client.exe)
    add_custom_command(OUTPUT ${code_cache_dir}/codecache-client.exe
        COMMAND ${CMAKE_COMMAND} -E make_directory ${code_cache_dir}
        COMMAND ${RAVEN_MINGW64} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
            -Wl,--entry,raven_entry,--no-insert-timestamp
            -o ${code_cache_dir}/codecache-client.exe ${RAVEN_DARWIN_DIR}/fex/code-cache/codecache_client.c -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/code-cache/codecache_client.c VERBATIM)
    foreach(variant a b)
        if(variant STREQUAL "a")
            set(number 1)
        else()
            set(number 2)
        endif()
        add_custom_command(OUTPUT ${code_cache_dir}/${variant}/codecache_lib.dll
            COMMAND ${CMAKE_COMMAND} -E make_directory ${code_cache_dir}/${variant}
            COMMAND ${RAVEN_MINGW64} -O1 -shared -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
                -DCODECACHE_VARIANT=${number}
                -Wl,--entry,raven_dll_entry,--image-base,0x130000000,--no-insert-timestamp
                -o ${code_cache_dir}/${variant}/codecache_lib.dll ${RAVEN_DARWIN_DIR}/fex/code-cache/codecache_lib.c -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/fex/code-cache/codecache_lib.c VERBATIM)
        list(APPEND code_cache_outputs ${code_cache_dir}/${variant}/codecache_lib.dll)
    endforeach()
    add_custom_target(code_cache_fex_fixtures ALL DEPENDS ${code_cache_outputs})
    foreach(parallel 0 1)
        add_test(NAME native_code_cache_fex_${parallel} COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-code-cache.py --raven $<TARGET_FILE:raven>
            --client ${code_cache_dir}/codecache-client.exe --library-a ${code_cache_dir}/a/codecache_lib.dll
            --library-b ${code_cache_dir}/b/codecache_lib.dll --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_code_cache_fex_${parallel} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
            LABELS "fex;jit;smc")
        if(parallel)
            set_tests_properties(native_code_cache_fex_${parallel} PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1")
        else()
            set_tests_properties(native_code_cache_fex_${parallel} PROPERTIES ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
        endif()
    endforeach()
endif()
if(TARGET fex_modhost_substrate)
    add_test(NAME darwin_unix_declined_fault COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_LIST_DIR}/scripts/test-unix-declined-fault.py $<TARGET_FILE:fex_modhost_substrate>)
    set_tests_properties(darwin_unix_declined_fault PROPERTIES TIMEOUT 30 LABELS "unix;fex")
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
        add_custom_target(darwin_mtld3d${width}_fixture ALL DEPENDS ${_mtld3d_fixture})
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
            add_custom_target(darwin_mtld3d_runtime${width}_fixture ALL DEPENDS ${_runtime_fixture})
            add_test(NAME darwin_mtld3d_runtime${width}_fex COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/scripts/test-fex-process.py
                --raven $<TARGET_FILE:raven> --fixture ${_runtime_fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(darwin_mtld3d_runtime${width}_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
        endif()
    endif()
endforeach()
include(${RAVEN_DARWIN_DIR}/fex/placement.cmake)
