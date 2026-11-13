add_library(bending_project STATIC ProjectFiles.cpp ToolCatalog.cpp DxfToolImporter.cpp)
add_library(Bending::Project ALIAS bending_project)
target_sources(bending_project PRIVATE GeometryCacheFiles.cpp ProjectDrawingJson.cpp ProjectDxfCbor.cpp)
target_include_directories(bending_project PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(bending_project PUBLIC Bending::Application Bending::Cad PRIVATE Qt6::Core bending_options)
qt_add_resources(bending_project starter_tools PREFIX "/tools" BASE "data"
    FILES data/starter-tools-v1.json)
