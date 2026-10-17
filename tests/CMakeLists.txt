add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE Bending::Core bending_options)
add_test(NAME core.sequence COMMAND core_tests)
set_tests_properties(core.sequence PROPERTIES LABELS "unit")

if(BENDING_BUILD_CAD)
    add_executable(cad_smoke cad_smoke.cpp)
    target_include_directories(cad_smoke SYSTEM PRIVATE "${OpenCASCADE_INCLUDE_DIR}")
    target_link_libraries(cad_smoke PRIVATE Bending::Cad bending_options TKPrim TKBRep TKTopAlgo)
    add_test(NAME cad.kernel COMMAND cad_smoke)
    set_tests_properties(cad.kernel PROPERTIES LABELS "integration")
endif()
if(BENDING_BUILD_APP)
    add_executable(viewer_tests viewer_tests.cpp)
    target_link_libraries(viewer_tests PRIVATE Bending::Viewer bending_options)
    add_test(NAME viewer.unavailable COMMAND viewer_tests --unavailable)
    set_tests_properties(viewer.unavailable PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20 LABELS "integration")
    if(UNIX AND NOT APPLE)
        find_program(BENDING_XVFB_RUN xvfb-run)
        if(BENDING_XVFB_RUN)
            add_test(NAME viewer.render
                COMMAND "${BENDING_XVFB_RUN}" -a -s "-screen 0 1600x1200x24" $<TARGET_FILE:viewer_tests>)
            add_test(NAME viewer.render.hidpi
                COMMAND "${BENDING_XVFB_RUN}" -a -s "-screen 0 1600x1200x24" $<TARGET_FILE:viewer_tests>)
            set_tests_properties(viewer.render viewer.render.hidpi PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=xcb;LIBGL_ALWAYS_SOFTWARE=1" TIMEOUT 60 LABELS "graphics")
            set_property(TEST viewer.render.hidpi APPEND PROPERTY ENVIRONMENT "QT_SCALE_FACTOR=2")
        endif()
    endif()
    add_test(NAME app.startup COMMAND bending_simulator --smoke-test)
    set_tests_properties(app.startup PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20 LABELS "integration")
    add_test(NAME app.graphics-unavailable COMMAND bending_simulator --graphics-smoke-test)
    set_tests_properties(app.graphics-unavailable PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" WILL_FAIL TRUE TIMEOUT 20 LABELS "integration")
endif()
