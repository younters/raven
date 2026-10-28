# Windows API contract at both guest widths, with the normal and deliberately small system budget.
foreach(width 64 32)
    if(width EQUAL 64)
        set(compiler ${RAVEN_MINGW64})
        set(entry raven_entry)
    else()
        set(compiler ${RAVEN_MINGW32})
        set(entry _raven_entry)
    endif()
    if(NOT compiler)
        continue()
    endif()
    set(fixture ${CMAKE_CURRENT_BINARY_DIR}/kernel-memory${width}.exe)
    add_custom_command(OUTPUT ${fixture}
        COMMAND ${compiler} -O2 -nostdlib -fno-stack-protector -fno-builtin
            -Wl,--entry,${entry},--no-insert-timestamp -o ${fixture}
            ${LOADER_SOURCES}/kernel_memory_client.c -lkernel32 -lpsapi
        DEPENDS ${LOADER_SOURCES}/kernel_memory_client.c VERBATIM)
    add_custom_target(kernel_memory${width}_fixture ALL DEPENDS ${fixture})
    foreach(mode default parallel)
        foreach(budget model small)
            set(test native_kernel_memory_x${width}_${mode}_${budget})
            add_test(NAME ${test} COMMAND raven --run-process --execution=native ${fixture})
            set_tests_properties(${test} PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/${test}-prefix")
            if(mode STREQUAL parallel)
                set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT RAVEN_PARALLEL=1)
            endif()
            if(budget STREQUAL small)
                set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT RAVEN_COMMIT_LIMIT_MB=64)
            endif()
        endforeach()
    endforeach()
endforeach()
