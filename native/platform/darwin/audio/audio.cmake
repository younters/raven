add_library(raven_darwin_audio STATIC ${RAVEN_DARWIN_DIR}/audio/output.cpp)
target_link_libraries(raven_darwin_audio PUBLIC "-framework AudioToolbox" "-framework CoreAudio")
target_compile_options(raven_darwin_audio PRIVATE -Wall -Wextra)
target_link_libraries(raven_cpu PUBLIC raven_darwin_audio)
add_executable(darwin_audio_test ${RAVEN_DARWIN_DIR}/audio/output_test.cpp)
target_link_libraries(darwin_audio_test PRIVATE raven_darwin_audio)
add_test(NAME native_darwin_audio_offline COMMAND darwin_audio_test)
set_tests_properties(native_darwin_audio_offline PROPERTIES TIMEOUT 30)
# No microphone and no audible samples. Skip on machines without an output device.
add_test(NAME native_darwin_audio_device COMMAND darwin_audio_test --device)
set_tests_properties(native_darwin_audio_device PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
if(RAVEN_MINGW64)
    foreach(width IN ITEMS 64 32)
        if(width EQUAL 64)
            set(audio_cc ${RAVEN_MINGW64})
        else()
            set(audio_cc ${RAVEN_MINGW32})
        endif()
        if(audio_cc)
            set(audio_fixture ${CMAKE_CURRENT_BINARY_DIR}/darwin-wave${width}.exe)
            add_custom_command(OUTPUT ${audio_fixture}
                COMMAND ${audio_cc} -O2 -nostdlib -fno-builtin -fno-stack-protector -Wl,--entry,raven_entry
                    -o ${audio_fixture} ${RAVEN_DARWIN_DIR}/audio/wave_client.c -lwinmm -lkernel32
                DEPENDS ${RAVEN_DARWIN_DIR}/audio/wave_client.c VERBATIM)
            add_custom_target(darwin_wave${width}_fixture ALL DEPENDS ${audio_fixture})
            add_test(NAME native_darwin_wave_x${width} COMMAND raven --run-process --execution=native --timeout-ms=10000 ${audio_fixture})
            set_tests_properties(native_darwin_wave_x${width} PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Native entry returned 0 "
                SKIP_REGULAR_EXPRESSION "Native entry returned 77 "
                ENVIRONMENT "RAVEN_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/darwin-wave-prefix${width};RAVEN_AUDIO_TRACE=1")
        endif()
    endforeach()
endif()
