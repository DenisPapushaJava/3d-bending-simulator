add_library(bending_project STATIC ProjectFiles.cpp)
add_library(Bending::Project ALIAS bending_project)
target_include_directories(bending_project PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_project PUBLIC Bending::Application Bending::Cad PRIVATE Qt6::Core bending_options)
