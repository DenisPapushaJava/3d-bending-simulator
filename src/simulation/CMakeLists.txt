add_library(bending_simulation STATIC SingleBend.cpp FlatToolSetup.cpp PressMotion.cpp ToolProfiles.cpp)
add_library(Bending::Simulation ALIAS bending_simulation)
target_include_directories(bending_simulation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_simulation PUBLIC Bending::Core PRIVATE bending_options)
