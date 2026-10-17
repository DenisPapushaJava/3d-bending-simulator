qt_add_executable(bending_simulator main.cpp)
set_target_properties(bending_simulator PROPERTIES OUTPUT_NAME "3d-bending-simulator")
if(WIN32)
    set_target_properties(bending_simulator PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_include_directories(bending_simulator PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(bending_simulator PRIVATE Bending::Ui Bending::Cad Bending::Viewer Bending::Project bending_options)
install(TARGETS bending_simulator RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/examples/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/3d-bending-simulator/examples"
    FILES_MATCHING PATTERN "*.step" PATTERN "*.stp" PATTERN "README.md")
install(FILES "${PROJECT_SOURCE_DIR}/VERSION" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DOCDIR}")
