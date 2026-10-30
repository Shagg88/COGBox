include_guard(GLOBAL)

option(GN_WITH_TRACY "Build on-demand Tracy zones into the custom renderer, including Release" OFF)

function(cogbox_enable_tracy renderer_target executable_target)
    if(NOT GN_WITH_TRACY)
        return()
    endif()
    if(TARGET rex::TracyClient)
        set(tracy_target rex::TracyClient)
    elseif(TARGET TracyClient)
        set(tracy_target TracyClient)
    else()
        message(FATAL_ERROR "GN_WITH_TRACY needs an SDK built with REXGLUE_ENABLE_TRACY=ON")
    endif()
    # Keep this private: enabling renderer zones must not enable the SDK's
    # per-indirect-call counters in the generated game code.
    target_link_libraries(${renderer_target} PRIVATE ${tracy_target})
    target_compile_definitions(${renderer_target} PRIVATE GN_HAVE_TRACY=1)
    if(WIN32)
        add_custom_command(TARGET ${executable_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${tracy_target}> $<TARGET_FILE_DIR:${executable_target}>
            COMMENT "Copying the matching Tracy client next to ${executable_target}")
    endif()
    message(STATUS "COGBox: on-demand Tracy renderer profiling enabled (all configurations)")
endfunction()
