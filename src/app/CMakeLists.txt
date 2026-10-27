qt_add_executable(bending_simulator main.cpp)
set_target_properties(bending_simulator PROPERTIES OUTPUT_NAME "3d-bending-simulator")
if(WIN32)
    enable_language(RC)
    set_target_properties(bending_simulator PROPERTIES WIN32_EXECUTABLE TRUE)
    configure_file(application.rc.in "${PROJECT_BINARY_DIR}/generated/application.rc" @ONLY)
    target_sources(bending_simulator PRIVATE "${PROJECT_BINARY_DIR}/generated/application.rc")
    set_source_files_properties("${PROJECT_BINARY_DIR}/generated/application.rc" PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/application.ico")
endif()
set(bending_icon_sizes 16 24 32 48 64 128 256)
foreach(size IN LISTS bending_icon_sizes)
    list(APPEND bending_icon_files "data/icons/${size}.png")
endforeach()
qt_add_resources(bending_simulator application_icons PREFIX "/app" BASE "data" FILES ${bending_icon_files})
target_include_directories(bending_simulator PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(bending_simulator PRIVATE Bending::Ui Bending::Cad Bending::Viewer Bending::Project bending_options)
install(TARGETS bending_simulator RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(UNIX AND NOT APPLE)
    install(FILES data/3d-bending-simulator.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(size IN LISTS bending_icon_sizes)
        install(FILES "data/icons/${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME "3d-bending-simulator.png")
    endforeach()
    install(FILES data/icons/application.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME "3d-bending-simulator.svg")
endif()
install(DIRECTORY "${PROJECT_SOURCE_DIR}/examples/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/3d-bending-simulator/examples"
    FILES_MATCHING PATTERN "*.step" PATTERN "*.stp" PATTERN "README.md")
install(FILES "${PROJECT_SOURCE_DIR}/VERSION" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES "${PROJECT_SOURCE_DIR}/src/ui/data/fonts/OFL.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses/fonts")
