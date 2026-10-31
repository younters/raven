# SPDX-License-Identifier: MIT
# Included only by the opt-in module-host subtree. Shared Darwin defaults live elsewhere.
if(NOT RAVEN_MINGW32 OR NOT TARGET raven_cpu)
    return()
endif()
set(M4_ENTRY ${CMAKE_BINARY_DIR}/native/fex-entry32.exe)
add_custom_command(OUTPUT ${M4_ENTRY}
    COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
        -Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
        -o ${M4_ENTRY} ${RAVEN_MODHOST_REPO}/testdata/native_cpu/entry.c
    DEPENDS ${RAVEN_MODHOST_REPO}/testdata/native_cpu/entry.c VERBATIM)
set(M4_OUTPUTS ${M4_ENTRY})
add_test(NAME native_entry32_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
    --raven $<TARGET_FILE:raven> --fixture ${M4_ENTRY} --modules "${RAVEN_FEX_MODULE_ROOT}" --entry)
set_tests_properties(native_entry32_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m4;entry")
foreach(client ansi file threads)
    set(fixture ${CMAKE_BINARY_DIR}/native/kernel-${client}-fex32.exe)
    set(source ${RAVEN_MODHOST_REPO}/testdata/native_loader/kernel_${client}_client.c)
    if(client STREQUAL "file")
        set(source ${RAVEN_MODHOST_REPO}/testdata/native_loader/kernel_files_client.c)
    endif()
    add_custom_command(OUTPUT ${fixture}
        COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
            -Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
            -o ${fixture} ${source} -lkernel32
        DEPENDS ${source} VERBATIM)
    list(APPEND M4_OUTPUTS ${fixture})
    add_test(NAME native_kernel_${client}32_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
        --raven $<TARGET_FILE:raven> --fixture ${fixture} --modules "${RAVEN_FEX_MODULE_ROOT}")
    set_tests_properties(native_kernel_${client}32_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m4;process")
endforeach()
add_custom_target(fex_m4_fixtures DEPENDS ${M4_OUTPUTS})
add_executable(fex_wow64_process_test ${CMAKE_CURRENT_LIST_DIR}/wow64_process_test.cpp)
target_link_libraries(fex_wow64_process_test PRIVATE raven_cpu)
add_dependencies(fex_wow64_process_test fex_m4_fixtures)
foreach(contract fs fault timeout host-smc guest-smc stdcall cdecl)
    add_test(NAME native_wow64_${contract}_fex COMMAND fex_wow64_process_test ${M4_ENTRY} ${contract})
    set_tests_properties(native_wow64_${contract}_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77 LABELS "fex;m4;engine"
        ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
endforeach()
set(M4_SEH ${CMAKE_BINARY_DIR}/native/kernel-seh-fex32.exe)
add_custom_command(OUTPUT ${M4_SEH}
    COMMAND ${RAVEN_MINGW32} -O1 -nostdlib -fno-stack-protector -fno-builtin -mno-stack-arg-probe
        -Wl,--entry,_raven_entry,--image-base,0x400000,--no-insert-timestamp
        -o ${M4_SEH} ${CMAKE_CURRENT_LIST_DIR}/wow64_seh_client.c -lkernel32
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/wow64_seh_client.c VERBATIM)
add_custom_target(fex_m4_seh_fixture DEPENDS ${M4_SEH})
add_test(NAME native_kernel_seh32_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
    --raven $<TARGET_FILE:raven> --fixture ${M4_SEH} --modules "${RAVEN_FEX_MODULE_ROOT}")
set_tests_properties(native_kernel_seh32_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m4;process;seh")
add_test(NAME native_kernel_threads32_parallel_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-wow64.py
    --raven $<TARGET_FILE:raven> --fixture ${CMAKE_BINARY_DIR}/native/kernel-threads-fex32.exe --modules "${RAVEN_FEX_MODULE_ROOT}" --parallel)
set_tests_properties(native_kernel_threads32_parallel_fex PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fex;m4;process;parallel")
add_dependencies(fex_wow64_process_test fex_m4_seh_fixture)
foreach(contract fault timeout host-smc guest-smc)
    add_test(NAME native_wow64_${contract}_parallel_fex COMMAND fex_wow64_process_test ${M4_ENTRY} ${contract})
    set_tests_properties(native_wow64_${contract}_parallel_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77 LABELS "fex;m4;engine;parallel"
        ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT};RAVEN_PARALLEL=1;RAVEN_SLICE_MS=0")
endforeach()

add_test(NAME native_wow64_native_brk_rejection_fex COMMAND ${Python3_EXECUTABLE} ${RAVEN_MODHOST_REPO}/scripts/test-fex-rejection.py
    --host $<TARGET_FILE:fex_wow64_process_test> --fixture ${M4_ENTRY} --modules "${RAVEN_FEX_MODULE_ROOT}" --wow64)
set_tests_properties(native_wow64_native_brk_rejection_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77 LABELS "fex;m4;fault")
add_test(NAME native_nt_service_fex COMMAND fex_process_test ${CMAKE_BINARY_DIR}/native/fex-entry.exe nt-service)
set_tests_properties(native_nt_service_fex PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77 LABELS "fex;m4;contract"
    ENVIRONMENT "RAVEN_FEX_MODULE_ROOT=${RAVEN_FEX_MODULE_ROOT}")
