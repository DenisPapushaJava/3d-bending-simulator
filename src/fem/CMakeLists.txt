add_library(bending_fem STATIC DockerFemRunner.cpp)
add_library(Bending::Fem ALIAS bending_fem)
target_include_directories(bending_fem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_fem PUBLIC Bending::Application PRIVATE Qt6::Core bending_options)
# Own MIT scripts only. Solvers, Python and Docker are optional external tools.
set(fem_scripts bend_hole.py results.py report.py viewer.html gui_job.py Dockerfile README.md)
foreach(script IN LISTS fem_scripts)
    configure_file("${PROJECT_SOURCE_DIR}/experiments/bend-hole/${script}"
        "${PROJECT_BINARY_DIR}/share/3d-bending-simulator/fem/${script}" COPYONLY)
    install(FILES "${PROJECT_SOURCE_DIR}/experiments/bend-hole/${script}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/3d-bending-simulator/fem")
endforeach()
