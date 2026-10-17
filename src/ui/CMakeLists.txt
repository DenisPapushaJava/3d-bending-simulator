add_library(bending_ui STATIC MainWindow.cpp)
add_library(Bending::Ui ALIAS bending_ui)
target_include_directories(bending_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_ui PUBLIC Qt6::Widgets PRIVATE Bending::Viewer bending_options)
