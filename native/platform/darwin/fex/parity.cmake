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
    foreach(contract contention context worker-seh apc lifecycle negatives)
        foreach(backend native fex)
            add_test(NAME native_parity_${contract}_${backend} COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py --raven $<TARGET_FILE:raven>
                --backend ${backend} --fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-parallel-${contract}.exe
                --modules "${RAVEN_FEX_MODULE_ROOT}")
            set_tests_properties(native_parity_${contract}_${backend} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
                LABELS "fex-parity;parallel" ENVIRONMENT "RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
        endforeach()
    endforeach()
endif()
