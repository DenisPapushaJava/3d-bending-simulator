add_library(bending_ui STATIC MainWindow.cpp ImportController.cpp
    include/bending/ui/MainWindow.h include/bending/ui/ImportController.h)
add_library(Bending::Ui ALIAS bending_ui)
set_target_properties(bending_ui PROPERTIES AUTOMOC ON)
target_include_directories(bending_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_ui PUBLIC Qt6::Widgets Bending::Application PRIVATE Bending::Viewer bending_options)
