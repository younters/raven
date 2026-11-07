function(add_fex_parity_fixture fixture compiler entry source)
    add_custom_command(OUTPUT ${fixture}
        COMMAND ${compiler} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
            -Wl,--entry,${entry},--no-insert-timestamp -o ${fixture} ${source} -lkernel32
        DEPENDS ${source} VERBATIM)
endfunction()

# Use identical PE bytes and isolated prefixes under both execution engines.
if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    foreach(mode nt-service unaligned debug-print host-data-write)
        add_test(NAME fex_${mode} COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${mode})
        set_tests_properties(fex_${mode} PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
            LABELS "fex" ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
        add_test(NAME fex_${mode}_parallel COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${mode})
        set_tests_properties(fex_${mode}_parallel PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
            LABELS "fex" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
    endforeach()
    foreach(client kernel-ansi kernel-file kernel-threads kernel-seh)
        foreach(backend native fex)
            foreach(parallel 0 1)
                set(name native_parity_${client}_${backend}_${parallel})
                set(parallel_env)
                if(parallel)
                    list(APPEND parallel_env RAVEN_PARALLEL=1)
                endif()
                add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} -E env --unset=RAVEN_PARALLEL
                    ${parallel_env}
                    ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
                    --raven $<TARGET_FILE:raven> --require-modules --backend ${backend}
                    --fixture ${CMAKE_CURRENT_BINARY_DIR}/${client}-fex.exe --modules "${RAVEN_FEX_MODULE_ROOT}")
                set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS "engine-parity;process")
            endforeach()
        endforeach()
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND TARGET fex_parallel_fixtures)
    set(parity_context ${CMAKE_CURRENT_BINARY_DIR}/fex-parity-context.exe)
    add_custom_command(OUTPUT ${parity_context}
        COMMAND ${RAVEN_CLANG} --target=x86_64-w64-mingw32 ${RAVEN_CLANG_MINGW_INCLUDE}
            -O0 -fms-extensions -fseh-exceptions -Xclang -fasync-exceptions -fno-stack-protector -mno-stack-arg-probe
            -DM3_CASE=2 -DM5_PARITY=1 -c -o ${parity_context}.o ${RAVEN_DARWIN_DIR}/fex/parallel_client.c
        COMMAND ${RAVEN_MINGW64} -nostdlib -Wl,--entry,raven_entry,--image-base,0x10000000000,--no-insert-timestamp
            -o ${parity_context} ${parity_context}.o ${FEX_REVIEW_SUPPORT} -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/parallel_client.c ${FEX_REVIEW_SUPPORT} VERBATIM)
    add_custom_target(fex_parity_context_fixture DEPENDS ${parity_context})
    add_dependencies(fex_parallel_fixtures fex_parity_context_fixture)
    add_dependencies(raven fex_parity_context_fixture)
    foreach(contract contention context worker-seh apc lifecycle negatives)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-${contract}.exe)
        if(contract STREQUAL "context")
            set(fixture ${parity_context})
        endif()
        foreach(backend native fex)
            add_test(NAME native_parity_${contract}_${backend} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --raven $<TARGET_FILE:raven>
                --require-modules --backend ${backend} --fixture ${fixture}
                --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(native_parity_${contract}_${backend} PROPERTIES TIMEOUT 30
                LABELS "engine-parity;parallel" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
        endforeach()
    endforeach()
endif()

# Exercise real child creation through modern raven and its FEX redirect for both widths.
if(RAVEN_FEX_MODHOST)
    foreach(width 32 64)
        if(TARGET process_creation_fixture${width})
            add_test(NAME native_fex_child_x${width} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules
                --raven $<TARGET_FILE:raven> --fixture ${CMAKE_CURRENT_BINARY_DIR}/process-creation${width}.exe
                --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 30000)
            set_tests_properties(native_fex_child_x${width} PROPERTIES TIMEOUT 45 LABELS "fex;process;child")
        endif()
    endforeach()
endif()
if(RAVEN_FEX_MODHOST)
    foreach(width 32 64)
        set(compiler ${RAVEN_MINGW${width}})
        if(compiler)
            set(entry raven_entry)
            if(width EQUAL 32)
                set(entry _raven_entry)
            endif()
            set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-tls-slots${width}.exe)
            add_fex_parity_fixture(${fixture} "${compiler}" ${entry}
                ${RAVEN_DARWIN_DIR}/fex/tls_slots_client.c)
            add_custom_target(fex_tls_slots${width}_fixture DEPENDS ${fixture})
            add_dependencies(raven fex_tls_slots${width}_fixture)
            foreach(engine native fex)
                foreach(parallel 0 1)
                    add_test(NAME native_tls_slots_x${width}_${engine}_${parallel} COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
                        --raven $<TARGET_FILE:raven> --fixture ${fixture}
                        --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 30000)
                    set_tests_properties(native_tls_slots_x${width}_${engine}_${parallel} PROPERTIES TIMEOUT 45
                        LABELS "engine-parity;tls")
                    if(parallel)
                        set_tests_properties(native_tls_slots_x${width}_${engine}_${parallel} PROPERTIES
                            ENVIRONMENT "RAVEN_PARALLEL=1")
                    else()
                        set_tests_properties(native_tls_slots_x${width}_${engine}_${parallel} PROPERTIES
                            ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
                    endif()
                endforeach()
            endforeach()
        endif()
    endforeach()
endif()
if(TARGET fex_wow64_boundary_test)
    foreach(module wow64 x64)
        set(dll xtajit64.dll)
        if(module STREQUAL "wow64")
            set(dll xtajit.dll)
        endif()
        add_test(NAME native_${module}_host_abi_mismatch_fex COMMAND fex_wow64_boundary_test
            ${RAVEN_FEX_MODULE_ROOT}/${dll} ${module})
        set_tests_properties(native_${module}_host_abi_mismatch_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
            FAIL_REGULAR_EXPRESSION "registration failed" LABELS "fex;contract;tls")
    endforeach()
endif()
if(RAVEN_FEX_MODHOST)
    foreach(width 32 64)
        set(compiler ${RAVEN_MINGW${width}})
        if(compiler)
            set(entry raven_entry)
            if(width EQUAL 32)
                set(entry _raven_entry)
            endif()
            set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-jit-stress${width}.exe)
            add_fex_parity_fixture(${fixture} "${compiler}" ${entry}
                ${RAVEN_DARWIN_DIR}/fex/jit_stress_client.c)
            add_custom_target(fex_jit_stress${width}_fixture DEPENDS ${fixture})
            add_dependencies(raven fex_jit_stress${width}_fixture)
            foreach(engine native fex)
                foreach(parallel 0 1)
                    add_test(NAME native_jit_stress_x${width}_${engine}_${parallel} COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
                        --raven $<TARGET_FILE:raven> --fixture ${fixture}
                        --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 45000)
                    set_tests_properties(native_jit_stress_x${width}_${engine}_${parallel} PROPERTIES TIMEOUT 75
                        LABELS "engine-parity;jit;stress")
                    if(parallel)
                        set_tests_properties(native_jit_stress_x${width}_${engine}_${parallel} PROPERTIES
                            ENVIRONMENT "RAVEN_PARALLEL=1")
                    else()
                        set_tests_properties(native_jit_stress_x${width}_${engine}_${parallel} PROPERTIES
                            ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
                    endif()
                endforeach()
            endforeach()
        endif()
    endforeach()
endif()
if(TARGET kernel-ansi_fex_fixture)
    foreach(width 32 64)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-fresh-prefix${width}.exe)
        set(entry raven_entry)
        if(width EQUAL 32)
            set(entry _raven_entry)
        endif()
        add_fex_parity_fixture(${fixture} "${RAVEN_MINGW${width}}" ${entry}
            ${RAVEN_DARWIN_DIR}/fex/fresh_prefix_client.c)
        add_custom_target(fex_fresh_prefix${width}_fixture DEPENDS ${fixture})
        add_dependencies(raven fex_fresh_prefix${width}_fixture)
        foreach(engine native fex)
            add_test(NAME native_fresh_prefix_x${width}_${engine} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-prefix.py $<TARGET_FILE:raven>
                ${fixture} "${RAVEN_FEX_MODULE_ROOT}" ${engine} ${width})
            set_tests_properties(native_fresh_prefix_x${width}_${engine} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
                LABELS "engine-parity;process;registration")
        endforeach()
    endforeach()
endif()

if(TARGET fex_process_test AND TARGET fex_wow64_process_test)
    foreach(width 32 64)
        set(host fex_process_test)
        set(fixture ${FEX_ENTRY_FIXTURE})
        if(width EQUAL 32)
            set(host fex_wow64_process_test)
            set(fixture ${CMAKE_BINARY_DIR}/native/fex-entry32.exe)
        endif()
        add_test(NAME native_teb_switch_x${width}_fex COMMAND ${host} ${fixture} teb-switch)
        set_tests_properties(native_teb_switch_x${width}_fex PROPERTIES TIMEOUT 20
            ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
    endforeach()
endif()

if(TARGET fex_wow64_boundary_test)
    add_test(NAME native_module_regressions_fex COMMAND fex_wow64_boundary_test
        ${RAVEN_FEX_MODULE_ROOT}/xtajit64.dll x64-probes)
    set_tests_properties(native_module_regressions_fex PROPERTIES TIMEOUT 30 LABELS "fex;tls;allocator")

    add_test(NAME native_reserved_write_rejection_fex COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/scripts/test-fex-rejection.py
        --host $<TARGET_FILE:fex_wow64_boundary_test>
        --fixture ${RAVEN_FEX_MODULE_ROOT}/xtajit64.dll --modules "${RAVEN_FEX_MODULE_ROOT}"
        --mode x64-reserved --expected-exit 138)
    set_tests_properties(native_reserved_write_rejection_fex PROPERTIES TIMEOUT 15 LABELS "fex;fault")
endif()

if(RAVEN_FEX_MODHOST)
    foreach(width 32 64)
        set(context_groups ${CMAKE_CURRENT_BINARY_DIR}/context-groups${width}.exe)
        if(width EQUAL 32)
            set(context_entry _raven_entry)
        else()
            set(context_entry raven_entry)
        endif()
        add_fex_parity_fixture(${context_groups} "${RAVEN_MINGW${width}}" ${context_entry}
            ${RAVEN_DARWIN_DIR}/tests/context_groups.c)
        add_custom_target(context_groups${width}_fixture DEPENDS ${context_groups})
        foreach(backend native fex)
            add_test(NAME native_context_groups${width}_${backend} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --raven $<TARGET_FILE:raven>
                --require-modules --backend ${backend} --fixture ${context_groups}
                --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(native_context_groups${width}_${backend} PROPERTIES TIMEOUT 30
                ENVIRONMENT "RAVEN_PARALLEL=1" LABELS "engine-parity;context")
        endforeach()
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND RAVEN_MINGW64)
    set(decommit_fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-decommit.exe)
    add_fex_parity_fixture(${decommit_fixture} "${RAVEN_MINGW64}" raven_entry
        ${RAVEN_DARWIN_DIR}/fex/decommit_client.c)
    add_custom_target(fex_decommit_fixture DEPENDS ${decommit_fixture})
    add_dependencies(raven fex_decommit_fixture)
    foreach(engine native fex)
        add_test(NAME native_decommit_mixed_${engine} COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
            --raven $<TARGET_FILE:raven> --fixture ${decommit_fixture}
            --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_decommit_mixed_${engine} PROPERTIES TIMEOUT 30
            LABELS "engine-parity;memory")
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND RAVEN_MINGW32)
    # x87 arithmetic in every precision, bit for bit, under both engines: the host path for 24 and 53 bits must give the x87's results.
    set(x87_precision_fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-x87-precision32.exe)
    add_custom_command(OUTPUT ${x87_precision_fixture}
        COMMAND ${RAVEN_MINGW32} -O1 -march=i686 -mno-sse -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
            -Wl,--entry,_raven_entry,--no-insert-timestamp -o ${x87_precision_fixture} ${RAVEN_DARWIN_DIR}/fex/x87_precision_client.c -lkernel32
        DEPENDS ${RAVEN_DARWIN_DIR}/fex/x87_precision_client.c VERBATIM)
    add_custom_target(fex_x87_precision_fixture DEPENDS ${x87_precision_fixture})
    add_dependencies(raven fex_x87_precision_fixture)
    foreach(engine native fex)
        add_test(NAME native_x87_precision_x32_${engine} COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
            --raven $<TARGET_FILE:raven> --fixture ${x87_precision_fixture}
            --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_x87_precision_x32_${engine} PROPERTIES TIMEOUT 60 LABELS "engine-parity;cpu")
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND RAVEN_MINGW32 AND RAVEN_MINGW64)
    # The calls answered in the guest behave as on Windows and stay there.
    foreach(width 32 64)
        if(width EQUAL 32)
            set(fast_calls_compiler ${RAVEN_MINGW32})
            set(fast_calls_entry _raven_entry)
        else()
            set(fast_calls_compiler ${RAVEN_MINGW64})
            set(fast_calls_entry raven_entry)
        endif()
        set(fast_calls_fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-fast-calls${width}.exe)
        add_fex_parity_fixture(${fast_calls_fixture} "${fast_calls_compiler}" ${fast_calls_entry}
            ${RAVEN_DARWIN_DIR}/fex/fast_calls_client.c)
        add_custom_target(fex_fast_calls${width}_fixture DEPENDS ${fast_calls_fixture})
        add_dependencies(raven fex_fast_calls${width}_fixture)
        foreach(engine native fex)
            add_test(NAME native_fast_calls_x${width}_${engine} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
                --raven $<TARGET_FILE:raven> --fixture ${fast_calls_fixture}
                --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(native_fast_calls_x${width}_${engine} PROPERTIES TIMEOUT 30
                LABELS "engine-parity;cpu")
        endforeach()
        # Guest threads that run side by side contend for one critical section for real.
        add_test(NAME native_fast_calls_x${width}_fex_parallel COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend fex
            --raven $<TARGET_FILE:raven> --fixture ${fast_calls_fixture}
            --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_fast_calls_x${width}_fex_parallel PROPERTIES TIMEOUT 60 ENVIRONMENT RAVEN_PARALLEL=1
            LABELS "engine-parity;cpu")
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND RAVEN_MINGW32)
    set(bit_memory_fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-bit-memory32.exe)
    add_fex_parity_fixture(${bit_memory_fixture} "${RAVEN_MINGW32}" _raven_entry
        ${RAVEN_DARWIN_DIR}/fex/bit_memory_client.c)
    add_custom_target(fex_bit_memory_fixture DEPENDS ${bit_memory_fixture})
    add_dependencies(raven fex_bit_memory_fixture)
    foreach(engine native fex)
        add_test(NAME native_bit_memory_x32_${engine} COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
            --raven $<TARGET_FILE:raven> --fixture ${bit_memory_fixture}
            --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_bit_memory_x32_${engine} PROPERTIES TIMEOUT 30
            LABELS "engine-parity;cpu")
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND RAVEN_MINGW64)
    set(loop_fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-loop-context.exe)
    add_fex_parity_fixture(${loop_fixture} "${RAVEN_MINGW64}" raven_entry
        ${RAVEN_DARWIN_DIR}/fex/loop_context_client.c)
    add_custom_target(fex_loop_context_fixture DEPENDS ${loop_fixture})
    add_dependencies(raven fex_loop_context_fixture)
    foreach(engine native fex)
        add_test(NAME native_loop_context_${engine} COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
            --raven $<TARGET_FILE:raven> --fixture ${loop_fixture}
            --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 40000)
        set_tests_properties(native_loop_context_${engine} PROPERTIES TIMEOUT 60
            LABELS "engine-parity;context")
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND RAVEN_MINGW64)
    set(ymm_fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-ymm-snapshot.exe)
    add_fex_parity_fixture(${ymm_fixture} "${RAVEN_MINGW64}" raven_entry
        ${RAVEN_DARWIN_DIR}/fex/ymm_snapshot_client.c)
    add_custom_target(fex_ymm_snapshot_fixture DEPENDS ${ymm_fixture})
    add_dependencies(raven fex_ymm_snapshot_fixture)
    foreach(engine native fex)
        foreach(parallel 0 1)
            add_test(NAME native_ymm_snapshot_${engine}_${parallel} COMMAND raven
                --run-process --execution=${engine} --timeout-ms=40000 ${ymm_fixture})
            set_tests_properties(native_ymm_snapshot_${engine}_${parallel} PROPERTIES
                TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "engine-parity;context")
            if(parallel)
                set_tests_properties(native_ymm_snapshot_${engine}_${parallel} PROPERTIES
                    ENVIRONMENT "RAVEN_PARALLEL=1")
            else()
                set_tests_properties(native_ymm_snapshot_${engine}_${parallel} PROPERTIES
                    ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
            endif()
        endforeach()
    endforeach()
endif()
if(RAVEN_FEX_MODHOST AND RAVEN_MINGW64)
    set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-retranslation64.exe)
    add_fex_parity_fixture(${fixture} "${RAVEN_MINGW64}" raven_entry
        ${RAVEN_DARWIN_DIR}/fex/retranslation_client.c)
    add_custom_target(fex_retranslation64_fixture DEPENDS ${fixture})
    add_dependencies(raven fex_retranslation64_fixture)
    foreach(parallel 0 1)
        add_test(NAME native_retranslation_fex_${parallel} COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-fex-retranslation.py --raven $<TARGET_FILE:raven>
            --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
        set_tests_properties(native_retranslation_fex_${parallel} PROPERTIES TIMEOUT 45
            LABELS "fex;jit;smc")
        if(parallel)
            set_tests_properties(native_retranslation_fex_${parallel} PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1")
        else()
            set_tests_properties(native_retranslation_fex_${parallel} PROPERTIES ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
        endif()
    endforeach()
endif()
if(RAVEN_FEX_MODHOST)
    foreach(width 32 64)
        set(compiler ${RAVEN_MINGW${width}})
        if(NOT compiler)
            continue()
        endif()
        set(entry raven_entry)
        if(width EQUAL 32)
            set(entry _raven_entry)
        endif()
        foreach(flush 0 1)
            set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-alias-flush${width}-${flush}.exe)
            add_custom_command(OUTPUT ${fixture}
                COMMAND ${compiler} -O2 -nostdlib -fno-stack-protector -DALIAS_FLUSH=${flush}
                    -Wl,--entry,${entry},--no-insert-timestamp
                    -o ${fixture} ${RAVEN_DARWIN_DIR}/fex/alias_flush_client.c -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/fex/alias_flush_client.c VERBATIM)
            add_custom_target(fex_alias_flush${width}_${flush}_fixture DEPENDS ${fixture})
            add_dependencies(raven fex_alias_flush${width}_${flush}_fixture)
            foreach(parallel 0 1)
                set(parallel_env)
                if(parallel)
                    list(APPEND parallel_env RAVEN_PARALLEL=1)
                endif()
                add_test(NAME native_alias_flush_x${width}_fex_${flush}_${parallel}
                    COMMAND ${CMAKE_COMMAND} -E env --unset=RAVEN_PARALLEL
                    ${parallel_env}
                    ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
                    --raven $<TARGET_FILE:raven> --fixture ${fixture} --require-modules
                    --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 45000)
                set_tests_properties(native_alias_flush_x${width}_fex_${flush}_${parallel}
                    PROPERTIES TIMEOUT 65 LABELS "fex;smc;flush;shared")
            endforeach()
        endforeach()
    endforeach()
endif()

if(RAVEN_FEX_MODHOST AND TARGET raven_cpu)
    add_executable(unix_ranges_test ${RAVEN_DARWIN_DIR}/tests/unix_ranges_test.cpp)
    target_include_directories(unix_ranges_test PRIVATE ${RAVEN_DARWIN_DIR})
    add_test(NAME native_darwin_unix_ranges COMMAND unix_ranges_test)
    set_tests_properties(native_darwin_unix_ranges PROPERTIES TIMEOUT 10 LABELS "fex")
endif()
