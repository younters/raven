# Provisioning is optional; no sidecar tests are registered on an ordinary install.
set(RAVEN_X87_SIDECAR "$ENV{HOME}/Documents/Projects/raven-components/x87sidecar/x87sidecar" CACHE FILEPATH "Flat cooperative x87sidecar for tests")
if(EXISTS "${RAVEN_X87_SIDECAR}" AND RAVEN_MINGW32 AND RAVEN_MINGW64)
    foreach(width IN ITEMS 32 64)
        set(fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-x87sidecar-${width}.exe)
        if(width EQUAL 32)
            set(compiler ${RAVEN_MINGW32})
            set(entry _raven_entry)
        else()
            set(compiler ${RAVEN_MINGW64})
            set(entry raven_entry)
        endif()
        add_custom_command(OUTPUT ${fixture}
            COMMAND ${compiler} -O2 -nostdlib -fno-stack-protector -Wl,--entry,${entry},--no-insert-timestamp
                ${RAVEN_DARWIN_DIR}/tests/x87sidecar.c -lkernel32 -o ${fixture}
            DEPENDS ${RAVEN_DARWIN_DIR}/tests/x87sidecar.c VERBATIM)
        list(APPEND x87_fixtures ${fixture})
    endforeach()
    set(child_fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-x87sidecar-32-child.exe)
    add_custom_command(OUTPUT ${child_fixture}
        COMMAND ${RAVEN_MINGW32} -O2 -nostdlib -fno-stack-protector -DRAVEN_X87_CHILD_LAUNCHER
            -Wl,--entry,_raven_entry,--no-insert-timestamp ${RAVEN_DARWIN_DIR}/tests/x87sidecar.c -lkernel32 -o ${child_fixture}
        DEPENDS ${RAVEN_DARWIN_DIR}/tests/x87sidecar.c VERBATIM)
    add_custom_target(darwin_x87sidecar_fixtures ALL DEPENDS ${x87_fixtures} ${child_fixture})
    foreach(case IN ITEMS correctness fallback x64 child)
        add_test(NAME darwin_x87sidecar_${case} COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/scripts/test-x87sidecar.py $<TARGET_FILE:raven> ${RAVEN_X87_SIDECAR}
            ${CMAKE_CURRENT_BINARY_DIR}/darwin-x87sidecar-32.exe ${CMAKE_CURRENT_BINARY_DIR}/darwin-x87sidecar-64.exe ${case})
        set_tests_properties(darwin_x87sidecar_${case} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
