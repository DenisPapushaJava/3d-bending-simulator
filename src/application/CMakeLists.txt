add_library(bending_application STATIC DocumentSession.cpp Thickness.cpp)
add_library(Bending::Application ALIAS bending_application)
target_include_directories(bending_application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_application PRIVATE bending_options)
