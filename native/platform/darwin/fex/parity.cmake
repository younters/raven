# Use identical PE bytes and isolated prefixes under both execution engines.
if(RAVEN_FEX_MODHOST AND TARGET fex_process_test)
    foreach(mode nt-service unaligned)
        add_test(NAME native_${mode}_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${mode})
        set_tests_properties(native_${mode}_fex PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
            LABELS "fex;fex-parity" ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
        add_test(NAME native_${mode}_parallel_fex COMMAND fex_process_test ${FEX_ENTRY_FIXTURE} ${mode})
        set_tests_properties(native_${mode}_parallel_fex PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
            LABELS "fex;fex-parity" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
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
                    --raven $<TARGET_FILE:raven> --backend ${backend}
                    --fixture ${CMAKE_CURRENT_BINARY_DIR}/${client}-fex.exe --modules "${RAVEN_FEX_MODULE_ROOT}")
                set_tests_properties(${name} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex-parity;process")
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
    foreach(contract contention context worker-seh apc lifecycle negatives)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-${contract}.exe)
        if(contract STREQUAL "context")
            set(fixture ${parity_context})
        endif()
        foreach(backend native fex)
            add_test(NAME native_parity_${contract}_${backend} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --raven $<TARGET_FILE:raven>
                --backend ${backend} --fixture ${fixture}
                --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(native_parity_${contract}_${backend} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
                LABELS "fex-parity;parallel" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
        endforeach()
    endforeach()
endif()
