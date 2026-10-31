# Fixed main images must survive host allocations before normal PE loading.
if(RAVEN_FEX_PLACEMENT_TEST_HOOKS AND RAVEN_FEX_MODHOST AND RAVEN_MINGW64 AND RAVEN_MINGW32 AND RAVEN_DXMT_ARM64_ROOT)
    foreach(bits 64 32)
        if(bits EQUAL 64)
            set(base 0x140000000)
            set(entry raven_entry)
        else()
            set(base 0x400000)
            set(entry _raven_entry)
        endif()
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/fex-fixed-default${bits}.exe)
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${RAVEN_MINGW${bits}} -O2 -nostdlib -fno-stack-protector
                -Wl,--entry,${entry},--image-base,${base},--disable-reloc-section,--no-insert-timestamp
                -o ${fixture} ${RAVEN_DARWIN_DIR}/fex/fixed_default_client.c -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/fex/fixed_default_client.c VERBATIM)
        add_custom_target(fex_fixed_default${bits}_fixture DEPENDS ${fixture})
        add_dependencies(raven fex_fixed_default${bits}_fixture)
        set(placement_test native_fixed_default${bits}_pressure_fex)
        if(bits EQUAL 32)
            set(placement_test native_fixed_default32_window_pressure_fex)
        endif()
        add_test(NAME ${placement_test}
            COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
                --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}"
                --fixed-base ${base})
        set_tests_properties(${placement_test} PROPERTIES
            ENVIRONMENT "RAVEN_FEX_PLACEMENT_PRESSURE=${RAVEN_DXMT_ARM64_ROOT}/winemetal.so"
            TIMEOUT 45 SKIP_RETURN_CODE 77 LABELS "fex;placement")
        if(bits EQUAL 64)
            add_test(NAME native_fixed_default64_pool_disabled_fex
                COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/test-fex-process.py
                    --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}"
                    --fixed-base ${base} --disable-pool)
            set_tests_properties(native_fixed_default64_pool_disabled_fex PROPERTIES
                TIMEOUT 45 SKIP_RETURN_CODE 77 LABELS "fex;placement")
        endif()
    endforeach()
endif()
