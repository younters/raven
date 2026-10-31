# Use identical PE bytes and isolated prefixes under both execution engines.
if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    foreach(mode nt-service unaligned debug-print)
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
            add_custom_command(OUTPUT ${fixture}
                COMMAND ${compiler} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
                    -Wl,--entry,${entry},--no-insert-timestamp -o ${fixture}
                    ${RAVEN_DARWIN_DIR}/fex/tls_slots_client.c -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/fex/tls_slots_client.c VERBATIM)
            add_custom_target(fex_tls_slots${width}_fixture DEPENDS ${fixture})
            add_dependencies(raven fex_tls_slots${width}_fixture)
            foreach(engine native fex)
                foreach(parallel 0 1)
                    add_test(NAME native_tls_slots_x${width}_${engine}_${parallel} COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
                        --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 30000)
                    set_tests_properties(native_tls_slots_x${width}_${engine}_${parallel} PROPERTIES TIMEOUT 45
                        LABELS "engine-parity;tls")
                    if(parallel)
                        set_tests_properties(native_tls_slots_x${width}_${engine}_${parallel} PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1")
                    else()
                        set_tests_properties(native_tls_slots_x${width}_${engine}_${parallel} PROPERTIES ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
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
        set_tests_properties(native_${module}_host_abi_mismatch_fex PROPERTIES TIMEOUT 15 LABELS "fex;contract;tls"
            PASS_REGULAR_EXPRESSION "FEX host ABI v2 rejects v1 and truncated host tables")
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
            add_custom_command(OUTPUT ${fixture}
                COMMAND ${compiler} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
                    -Wl,--entry,${entry},--no-insert-timestamp -o ${fixture}
                    ${RAVEN_DARWIN_DIR}/fex/jit_stress_client.c -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/fex/jit_stress_client.c VERBATIM)
            add_custom_target(fex_jit_stress${width}_fixture DEPENDS ${fixture})
            add_dependencies(raven fex_jit_stress${width}_fixture)
            foreach(engine native fex)
                foreach(parallel 0 1)
                    add_test(NAME native_jit_stress_x${width}_${engine}_${parallel} COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --require-modules --backend ${engine}
                        --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}" --timeout-ms 60000)
                    set_tests_properties(native_jit_stress_x${width}_${engine}_${parallel} PROPERTIES TIMEOUT 75
                        LABELS "engine-parity;jit;stress")
                    if(parallel)
                        set_tests_properties(native_jit_stress_x${width}_${engine}_${parallel} PROPERTIES ENVIRONMENT "RAVEN_PARALLEL=1")
                    else()
                        set_tests_properties(native_jit_stress_x${width}_${engine}_${parallel} PROPERTIES ENVIRONMENT_MODIFICATION "RAVEN_PARALLEL=unset:")
                    endif()
                endforeach()
            endforeach()
        endif()
    endforeach()
endif()
if(TARGET kernel-ansi_fex_fixture)
    foreach(width 32 64)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-ansi-fex.exe)
        if(width EQUAL 32)
            set(fixture ${CMAKE_BINARY_DIR}/native/kernel-ansi-fex32.exe)
        endif()
        foreach(engine native fex)
            add_test(NAME native_fresh_prefix_x${width}_${engine} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-prefix.py $<TARGET_FILE:raven>
                ${fixture} "${RAVEN_FEX_MODULE_ROOT}" ${engine} ${width})
            set_tests_properties(native_fresh_prefix_x${width}_${engine} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
                LABELS "engine-parity;process;registration")
        endforeach()
    endforeach()
endif()
