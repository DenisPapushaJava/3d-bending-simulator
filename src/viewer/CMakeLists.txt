add_library(bending_viewer STATIC ViewerPlaceholder.cpp)
add_library(Bending::Viewer ALIAS bending_viewer)
target_include_directories(bending_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_viewer PUBLIC Qt6::Widgets PRIVATE bending_options)
