if(BENDING_BUILD_CAD)
    # DataExchange's exported targets reference OCAF and visualization, even for plain STEP.
    set(bending_occt_components FoundationClasses ModelingData ModelingAlgorithms Visualization ApplicationFramework DataExchange)
    find_package(OpenCASCADE REQUIRED COMPONENTS ${bending_occt_components})
    if(TARGET TKDESTEP)
        set(bending_step_toolkit TKDESTEP) # OCCT 7.8+
    elseif(TARGET TKSTEP)
        set(bending_step_toolkit TKSTEP) # OCCT 7.6/7.7
    else()
        message(FATAL_ERROR "OCCT STEP SDK is missing. On Ubuntu/Mint install libocct-data-exchange-dev (scripts/install-linux-deps.sh); on Windows use the configured vcpkg OCCT build.")
    endif()
    # Older OCCT packages use ExactVersion compatibility; inspect the detected version instead.
    set(BENDING_OCCT_VERSION "${OpenCASCADE_MAJOR_VERSION}.${OpenCASCADE_MINOR_VERSION}.${OpenCASCADE_MAINTENANCE_VERSION}")
    if(BENDING_OCCT_VERSION VERSION_LESS 7.6.0 OR NOT BENDING_OCCT_VERSION VERSION_LESS 8.0.0)
        message(FATAL_ERROR "Use OCCT 7.6–7.9; found ${BENDING_OCCT_VERSION}")
    endif()
endif()
if(BENDING_BUILD_APP)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
endif()
