add_library(bending_core STATIC BendSequence.cpp BendTarget.cpp PressTools.cpp)
add_library(Bending::Core ALIAS bending_core)
target_include_directories(bending_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_core PRIVATE bending_options)
