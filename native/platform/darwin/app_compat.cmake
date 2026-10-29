foreach(width 64 32)
    set(compiler ${RAVEN_MINGW64})
    set(entry raven_entry)
    if(width EQUAL 32)
        set(compiler ${RAVEN_MINGW32})
        set(entry _raven_entry)
    endif()
    if(compiler)
        set(compiler_dir ${CMAKE_CURRENT_BINARY_DIR}/app-compiler${width})
        set(compiler_fixture ${compiler_dir}/client.exe)
        set(compiler_dll ${compiler_dir}/d3dcompiler_47.dll)
        add_custom_command(OUTPUT ${compiler_fixture} ${compiler_dll}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${compiler_dir}
            COMMAND ${compiler} -nostdlib -O1 -shared -DCOMPILER_DLL -Wl,--entry,0,--kill-at
                -o ${compiler_dll} ${RAVEN_DARWIN_DIR}/tests/app_compiler.c
            COMMAND ${compiler} -nostdlib -O1 -Wl,--entry,${entry}
                -o ${compiler_fixture} ${RAVEN_DARWIN_DIR}/tests/app_compiler.c -lkernel32
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/app_compiler.c VERBATIM)
        add_custom_target(darwin_app_compiler${width}_fixture ALL DEPENDS ${compiler_fixture} ${compiler_dll})
        add_test(NAME native_darwin_app_compiler_x${width} COMMAND raven --run-process --execution=native --timeout-ms=5000 ${compiler_fixture})
        set_tests_properties(native_darwin_app_compiler_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
        foreach(area sspi_ansi socket_exclusive processor_count appcontainer_stub)
            set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-${area}${width}.exe)
            add_custom_command(OUTPUT ${fixture}
                COMMAND ${compiler} -nostdlib -O1 -fno-builtin -fno-stack-protector -Wl,--entry,${entry}
                    -o ${fixture} ${RAVEN_DARWIN_DIR}/tests/${area}.c -lsecur32 -luserenv -liphlpapi -lws2_32 -lkernel32 -lgcc
                DEPENDS ${RAVEN_DARWIN_DIR}/tests/${area}.c VERBATIM)
            add_custom_target(darwin_${area}${width}_fixture ALL DEPENDS ${fixture})
            add_test(NAME native_darwin_${area}_x${width} COMMAND raven --run-process --execution=native --timeout-ms=5000 ${fixture})
            set_tests_properties(native_darwin_${area}_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 ")
            if(area STREQUAL "socket_exclusive")
                set_tests_properties(native_darwin_${area}_x${width} PROPERTIES
                    SKIP_REGULAR_EXPRESSION "Native entry returned 21 ")
            endif()
        endforeach()
    endif()
endforeach()

find_program(RAVEN_WIDL widl)
add_test(NAME native_darwin_installer_widl COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/scripts/test-installer-widl.py)
set_tests_properties(native_darwin_installer_widl PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
if(RAVEN_WIDL)
    get_filename_component(widl_directory ${RAVEN_WIDL} DIRECTORY)
    set_tests_properties(native_darwin_installer_widl PROPERTIES ENVIRONMENT "PATH=${widl_directory}:$ENV{PATH}")
endif()
