add_library(bending_viewer STATIC ViewerWidget.cpp include/bending/viewer/ViewerWidget.h)
add_library(Bending::Viewer ALIAS bending_viewer)
set_target_properties(bending_viewer PROPERTIES AUTOMOC ON)
target_include_directories(bending_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(bending_viewer SYSTEM PRIVATE "${OpenCASCADE_INCLUDE_DIR}")
target_link_libraries(bending_viewer PUBLIC Qt6::Widgets
    PRIVATE bending_options TKOpenGl TKV3d TKService TKPrim)
