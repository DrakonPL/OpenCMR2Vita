# Portable fixed-point helpers vs the original inline assembly. Needs a
# 32-bit x86 build: clang compiles the MSVC-style assembly with -fasm-blocks.
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86")
    add_executable(fixedpoint_equiv fixedpoint_equiv.cpp)
    target_include_directories(fixedpoint_equiv PRIVATE ${PROJECT_SOURCE_DIR}/game ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(fixedpoint_equiv PRIVATE
        -include ${PROJECT_SOURCE_DIR}/src/port/types.h -fms-extensions -fasm-blocks -Wno-everything)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The original assembly uses absolute addresses (offset g_atanTable).
        # Ubuntu's Clang defaults to PIE, which cannot encode that operand.
        target_compile_options(fixedpoint_equiv PRIVATE -fno-pic -fno-pie)
        target_link_options(fixedpoint_equiv PRIVATE -no-pie)
    endif()
    add_test(NAME fixedpoint_equiv COMMAND fixedpoint_equiv)
endif()

add_executable(net_session_test net_session.cpp)
target_link_libraries(net_session_test PRIVATE opencmr2_net)
target_compile_options(net_session_test PRIVATE "SHELL:-include \"${PROJECT_SOURCE_DIR}/src/port/types.h\"")
add_test(NAME net_session COMMAND net_session_test)
set_tests_properties(net_session PROPERTIES TIMEOUT 60)

add_executable(bink_decoder_test bink_decoder.cpp)
target_link_libraries(bink_decoder_test PRIVATE opencmr2_bink)
add_test(NAME bink_decoder COMMAND bink_decoder_test)

add_executable(movie_playback_test movie_playback.cpp ${PROJECT_SOURCE_DIR}/src/video/movie.cpp)
target_link_libraries(movie_playback_test PRIVATE opencmr2_bink)
add_test(NAME movie_playback COMMAND movie_playback_test)

add_executable(movie_render_state_test movie_render_state.cpp
    ${PROJECT_SOURCE_DIR}/src/diagnostics/observer.cpp
    ${PROJECT_SOURCE_DIR}/src/video/movie.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_core.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_small_batch.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_batch_worker.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_threaded.cpp)
target_link_libraries(movie_render_state_test PRIVATE opencmr2_bink SDL3::SDL3)
add_test(NAME movie_render_state COMMAND movie_render_state_test)

# FTOI vs a real x87 FISTP (the /QIfist conversion of the original).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86|AMD64|amd64")
    add_executable(ftoi_fistp_test ftoi_fistp.cpp)
    target_include_directories(ftoi_fistp_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
    target_compile_options(ftoi_fistp_test PRIVATE "SHELL:-include \"${PROJECT_SOURCE_DIR}/src/port/types.h\"")
    add_test(NAME ftoi_fistp COMMAND ftoi_fistp_test)
endif()

# Tests that run game code, on every target.
add_executable(math_tables_test math_tables.cpp)
target_link_libraries(math_tables_test PRIVATE opencmr2_platform)
add_test(NAME math_tables COMMAND math_tables_test
    ${CMAKE_CURRENT_SOURCE_DIR}/reference/x87-single/math-tables.bin)
add_executable(game_data_setup_test game_data_setup.cpp)
target_link_libraries(game_data_setup_test PRIVATE opencmr2_platform)
add_test(NAME game_data_setup COMMAND game_data_setup_test)
set_tests_properties(game_data_setup PROPERTIES TIMEOUT 30)
add_executable(car_scheduler_test car_scheduler.cpp)
target_link_libraries(car_scheduler_test PRIVATE opencmr2_platform)
add_test(NAME car_scheduler COMMAND car_scheduler_test)
add_executable(stage_probe stage_probe.cpp)
target_link_libraries(stage_probe PRIVATE opencmr2_platform)
# The monkey test's watchdog uses POSIX alarm().
if(NOT WIN32)
    add_executable(monkey_probe monkey_probe.cpp)
    target_link_libraries(monkey_probe PRIVATE opencmr2_platform)
endif()
add_executable(preview_cleanup_test preview_cleanup.cpp)
target_include_directories(preview_cleanup_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
target_link_libraries(preview_cleanup_test PRIVATE opencmr2_platform)
add_test(NAME preview_cleanup COMMAND preview_cleanup_test)
add_executable(service_park_weather_test service_park_weather.cpp)
target_link_libraries(service_park_weather_test PRIVATE opencmr2_platform)
add_test(NAME service_park_weather COMMAND service_park_weather_test)
add_executable(input_actions_test input_actions.cpp ${PROJECT_SOURCE_DIR}/src/input/vita_controls.cpp)
target_include_directories(input_actions_test PRIVATE ${PROJECT_SOURCE_DIR}/game ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(input_actions_test PRIVATE opencmr2_platform)
add_test(NAME input_actions COMMAND input_actions_test)

add_executable(frontend_language_test frontend_language.cpp)
target_link_libraries(frontend_language_test PRIVATE opencmr2_platform)
add_test(NAME frontend_language COMMAND frontend_language_test)
set_tests_properties(frontend_language PROPERTIES TIMEOUT 30)

add_executable(country_flags_test country_flags.cpp)
target_link_libraries(country_flags_test PRIVATE opencmr2_platform)
add_test(NAME country_flags COMMAND country_flags_test ${CMAKE_CURRENT_SOURCE_DIR}/reference/menu-flags.txt)

add_executable(vector_sprites_test vector_sprites.cpp)
target_link_libraries(vector_sprites_test PRIVATE opencmr2_platform)
add_test(NAME vector_sprites COMMAND vector_sprites_test)

add_executable(outline_font_test outline_font.cpp)
target_include_directories(outline_font_test PRIVATE ${PROJECT_BINARY_DIR}/src/generated ${PROJECT_SOURCE_DIR}/third_party/stb)
target_link_libraries(outline_font_test PRIVATE opencmr2_platform)
add_test(NAME outline_font COMMAND outline_font_test)
set_tests_properties(outline_font PROPERTIES TIMEOUT 30)

add_executable(saved_game_test saved_game.cpp)
target_link_libraries(saved_game_test PRIVATE opencmr2_platform)
add_test(NAME saved_game COMMAND saved_game_test)
set_tests_properties(saved_game PROPERTIES TIMEOUT 30)

add_executable(bink_decode bink_decode.cpp)
target_link_libraries(bink_decode PRIVATE opencmr2_bink)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME reference_capture_format
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/reference_capture.py)
    add_test(NAME raw_access_audit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/raw_access_audit.py)
    add_test(NAME vita_shot_compare
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vita_shot_compare.py)
endif()

set(OPENCMR2_BINK_TEST_DATA_DIR "" CACHE PATH "Optional original game data for movie and physics regression tests")
if(OPENCMR2_BINK_TEST_DATA_DIR)
    if(Python3_Interpreter_FOUND AND TARGET stage_probe)
        add_test(NAME physics_original_parity COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/physics_parity.py --probe $<TARGET_FILE:stage_probe>
            --data ${OPENCMR2_BINK_TEST_DATA_DIR})
        add_test(NAME runtime_scenarios COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/runtime_scenarios.py --probe $<TARGET_FILE:stage_probe>
            --data ${OPENCMR2_BINK_TEST_DATA_DIR})
        set_tests_properties(runtime_scenarios PROPERTIES TIMEOUT 300)
        add_test(NAME cpu_runs COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/runtime_scenarios.py --probe $<TARGET_FILE:stage_probe>
            --data ${OPENCMR2_BINK_TEST_DATA_DIR} --cpu-runs)
        set_tests_properties(cpu_runs PROPERTIES TIMEOUT 900)
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME lan_game COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/lan_game.py --host-probe $<TARGET_FILE:stage_probe>
                --data ${OPENCMR2_BINK_TEST_DATA_DIR})
            set_tests_properties(lan_game PROPERTIES TIMEOUT 120)
            # Cars from 2 have no view or event slot of their own.
            add_test(NAME lan_game_3 COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/lan_game.py --host-probe $<TARGET_FILE:stage_probe>
                --data ${OPENCMR2_BINK_TEST_DATA_DIR} --players 3)
            set_tests_properties(lan_game_3 PROPERTIES TIMEOUT 150)
        endif()
        add_test(NAME stage_lighting COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/stage_lighting.py --probe $<TARGET_FILE:stage_probe>
            --data ${OPENCMR2_BINK_TEST_DATA_DIR})
        set_tests_properties(stage_lighting PROPERTIES TIMEOUT 300)
    endif()
    file(GLOB_RECURSE BINK_TEST_FILES "${OPENCMR2_BINK_TEST_DATA_DIR}/*.bik" "${OPENCMR2_BINK_TEST_DATA_DIR}/*.BIK")
    foreach(movie IN LISTS BINK_TEST_FILES)
        get_filename_component(name "${movie}" NAME_WE)
        add_test(NAME "bink_file_${name}" COMMAND bink_decode "${movie}")
    endforeach()
endif()

# The shared game path resolver on a fake file system, without SDL.
add_executable(sys_paths_test sys_paths.cpp ${PROJECT_SOURCE_DIR}/src/platform/sys_paths.cpp)
target_include_directories(sys_paths_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME sys_paths COMMAND sys_paths_test)

# The default settings of the PS Vita and its language map, without SDL.
add_executable(vita_settings_test vita_settings.cpp ${PROJECT_SOURCE_DIR}/src/platform/vita_settings.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/sys_common.cpp ${PROJECT_SOURCE_DIR}/src/platform/sys_paths.cpp)
target_include_directories(vita_settings_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME vita_settings COMMAND vita_settings_test)

# The answer of a text dialog typed into a game entry, without SDL.
add_executable(text_entry_test text_entry.cpp ${PROJECT_SOURCE_DIR}/src/platform/text_entry.cpp)
target_include_directories(text_entry_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME text_entry COMMAND text_entry_test)

# The keys of a draw's fragment program and pipeline, without SDL.
add_executable(shader_key_test shader_key.cpp)
target_include_directories(shader_key_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME shader_key COMMAND shader_key_test)
add_executable(gpu_sub_allocator_test gpu_sub_allocator.cpp)
target_include_directories(gpu_sub_allocator_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gpu_sub_allocator COMMAND gpu_sub_allocator_test)
add_executable(texture_tiles_test texture_tiles.cpp)
target_include_directories(texture_tiles_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME texture_tiles COMMAND texture_tiles_test)
add_executable(gfx_sky_plan_test gfx_sky_plan.cpp)
target_include_directories(gfx_sky_plan_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gfx_sky_plan COMMAND gfx_sky_plan_test)
add_executable(gfx_box_view_test gfx_box_view.cpp)
target_include_directories(gfx_box_view_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gfx_box_view COMMAND gfx_box_view_test)
add_executable(frame_pacing_test frame_pacing.cpp)
target_include_directories(frame_pacing_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME frame_pacing COMMAND frame_pacing_test)
add_executable(relight_budget_test relight_budget.cpp)
target_include_directories(relight_budget_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME relight_budget COMMAND relight_budget_test)
add_executable(eye_plane_test eye_plane.cpp)
target_include_directories(eye_plane_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME eye_plane COMMAND eye_plane_test)
add_executable(deferred_order_test deferred_order.cpp)
target_include_directories(deferred_order_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME deferred_order COMMAND deferred_order_test)
add_executable(batch_runs_test batch_runs.cpp)
target_include_directories(batch_runs_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME batch_runs COMMAND batch_runs_test)
add_executable(tree_cull_test tree_cull.cpp)
target_include_directories(tree_cull_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/game)
add_test(NAME tree_cull COMMAND tree_cull_test)
add_executable(ground_merge_test ground_merge.cpp)
target_include_directories(ground_merge_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME ground_merge COMMAND ground_merge_test)
add_executable(midpoint_tex_gen_test midpoint_tex_gen.cpp)
target_include_directories(midpoint_tex_gen_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/game)
add_test(NAME midpoint_tex_gen COMMAND midpoint_tex_gen_test)
add_executable(fade_reach_test fade_reach.cpp)
target_include_directories(fade_reach_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME fade_reach COMMAND fade_reach_test)
add_executable(fade_side_test fade_side.cpp)
target_include_directories(fade_side_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME fade_side COMMAND fade_side_test)
add_executable(sector_triangle_test sector_triangle.cpp)
target_include_directories(sector_triangle_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME sector_triangle COMMAND sector_triangle_test)
add_executable(fixed_quotient_test fixed_quotient.cpp)
target_include_directories(fixed_quotient_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME fixed_quotient COMMAND fixed_quotient_test)
add_executable(yuv_convert_test yuv_convert.cpp)
target_include_directories(yuv_convert_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME yuv_convert COMMAND yuv_convert_test)
add_executable(hw_movie_handle_test hw_movie_handle.cpp)
target_include_directories(hw_movie_handle_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME hw_movie_handle COMMAND hw_movie_handle_test)
add_executable(hw_movie_clock_test hw_movie_clock.cpp)
target_include_directories(hw_movie_clock_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME hw_movie_clock COMMAND hw_movie_clock_test)
add_executable(gxm_deferred_free_test gxm_deferred_free.cpp)
target_include_directories(gxm_deferred_free_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_deferred_free COMMAND gxm_deferred_free_test)
add_executable(gxm_texture_layout_test gxm_texture_layout.cpp)
target_include_directories(gxm_texture_layout_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_texture_layout COMMAND gxm_texture_layout_test)
add_executable(gxm_viewport_test gxm_viewport.cpp)
target_include_directories(gxm_viewport_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_viewport COMMAND gxm_viewport_test)
add_executable(gxm_bounds_test gxm_bounds.cpp)
target_include_directories(gxm_bounds_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_bounds COMMAND gxm_bounds_test)
add_executable(gxm_alpha_test gxm_alpha.cpp)
target_include_directories(gxm_alpha_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_alpha COMMAND gxm_alpha_test)
add_executable(gxm_vertex_uniforms_test gxm_vertex_uniforms.cpp)
target_include_directories(gxm_vertex_uniforms_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_vertex_uniforms COMMAND gxm_vertex_uniforms_test)
# The x87 rounding of the game's float operations: the fast way against the general way.
add_executable(x87_round_test x87_round.cpp)
target_include_directories(x87_round_test PRIVATE ${PROJECT_SOURCE_DIR}/game)
add_test(NAME x87_round COMMAND x87_round_test)
add_executable(texture_atlas_test texture_atlas.cpp)
target_include_directories(texture_atlas_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME texture_atlas COMMAND texture_atlas_test)
add_executable(gxm_key_cache_test gxm_key_cache.cpp)
target_include_directories(gxm_key_cache_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_key_cache COMMAND gxm_key_cache_test)
add_executable(gxm_digits_test gxm_digits.cpp)
target_include_directories(gxm_digits_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_digits COMMAND gxm_digits_test)
add_executable(gxm_fragment_uniforms_test gxm_fragment_uniforms.cpp)
target_include_directories(gxm_fragment_uniforms_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME gxm_fragment_uniforms COMMAND gxm_fragment_uniforms_test)
# The recorder: the states, the joined draws and the vertex copies of a frame.
add_executable(gfx_record_test gfx_record.cpp
    ${PROJECT_SOURCE_DIR}/src/diagnostics/observer.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_core.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_small_batch.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_batch_worker.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_threaded.cpp
    ${PROJECT_SOURCE_DIR}/src/render/texture_mips.cpp)
target_include_directories(gfx_record_test PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/third_party/stb)
find_package(Threads REQUIRED)
target_link_libraries(gfx_record_test PRIVATE SDL3::SDL3 Threads::Threads)
add_test(NAME gfx_record COMMAND gfx_record_test)
# The software mixer of the PS Vita's sound.
add_executable(audio_mixer_test audio_mixer.cpp ${PROJECT_SOURCE_DIR}/src/audio/audio_mixer.cpp)
target_include_directories(audio_mixer_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME audio_mixer COMMAND audio_mixer_test)
# The read-ahead of the streamed music (asynchronous reads on the PS Vita).
add_executable(read_ahead_test read_ahead.cpp)
target_include_directories(read_ahead_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME read_ahead COMMAND read_ahead_test)
# The lines of the PS Vita's log between two writes to the file.
add_executable(log_buffer_test log_buffer.cpp)
target_include_directories(log_buffer_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME log_buffer COMMAND log_buffer_test)
# The threaded executor: the frames on a worker, one at a time, in their order.
add_executable(gfx_threaded_test gfx_threaded.cpp
    ${PROJECT_SOURCE_DIR}/src/render/gfx_threaded.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_small_batch.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_batch_worker.cpp)
target_include_directories(gfx_threaded_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gfx_threaded_test PRIVATE SDL3::SDL3 Threads::Threads)
add_test(NAME gfx_threaded COMMAND gfx_threaded_test)
# The function profile of a profiling build: the game's thread only.
add_executable(function_profile_test function_profile.cpp ${PROJECT_SOURCE_DIR}/src/diagnostics/function_profile.cpp)
target_include_directories(function_profile_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(function_profile_test PRIVATE Threads::Threads)
add_test(NAME function_profile COMMAND function_profile_test)
# The batch jobs of the recorder on a thread of their own.
add_executable(gfx_batch_worker_test gfx_batch_worker.cpp
    ${PROJECT_SOURCE_DIR}/src/render/gfx_small_batch.cpp ${PROJECT_SOURCE_DIR}/src/render/gfx_batch_worker.cpp)
target_include_directories(gfx_batch_worker_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gfx_batch_worker_test PRIVATE SDL3::SDL3 Threads::Threads)
add_test(NAME gfx_batch_worker COMMAND gfx_batch_worker_test)
# The game jobs on a thread of their own, and the tyre marks as one of them.
add_executable(game_jobs_test game_jobs.cpp ${PROJECT_SOURCE_DIR}/src/platform/game_jobs.cpp)
target_include_directories(game_jobs_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(game_jobs_test PRIVATE Threads::Threads)
add_test(NAME game_jobs COMMAND game_jobs_test)
add_executable(tyre_mark_job_test tyre_mark_job.cpp)
target_link_libraries(tyre_mark_job_test PRIVATE opencmr2_platform)
add_test(NAME tyre_mark_job COMMAND tyre_mark_job_test)
add_executable(sector_cull_job_test sector_cull_job.cpp)
target_link_libraries(sector_cull_job_test PRIVATE opencmr2_platform)
add_test(NAME sector_cull_job COMMAND sector_cull_job_test)
add_executable(readback_pack_test readback_pack.cpp)
target_include_directories(readback_pack_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME readback_pack COMMAND readback_pack_test)
add_executable(bmp_file_test bmp_file.cpp)
target_include_directories(bmp_file_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME bmp_file COMMAND bmp_file_test)
add_executable(input_math_test input_math.cpp ${PROJECT_SOURCE_DIR}/src/input/input_math.cpp)
target_include_directories(input_math_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME input_math COMMAND input_math_test)
add_executable(shader_variants_test shader_variants.cpp)
target_include_directories(shader_variants_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME shader_variants COMMAND shader_variants_test
    ${PROJECT_SOURCE_DIR}/src/render/shaders/cg/variants.txt)

# The Vita backend's event queue, without SDL.
add_executable(sys_events_test sys_events.cpp)
target_include_directories(sys_events_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME sys_events COMMAND sys_events_test)

add_executable(replay_disk_test replay_disk.cpp)
target_link_libraries(replay_disk_test PRIVATE opencmr2_platform)
add_test(NAME replay_disk COMMAND replay_disk_test)

add_executable(replay_events_test replay_events.cpp)
target_link_libraries(replay_events_test PRIVATE opencmr2_platform)
add_test(NAME replay_events COMMAND replay_events_test)

# The game's dirt stamps on a texture: a compressed one stays on the GPU.
add_executable(texture_stamp_test texture_stamp.cpp texture_stamp_access.cpp)
target_include_directories(texture_stamp_test PRIVATE ${PROJECT_SOURCE_DIR}/game ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(texture_stamp_test PRIVATE opencmr2_platform)
add_test(NAME texture_stamp COMMAND texture_stamp_test)

add_executable(sector_culling_test sector_culling.cpp)
target_link_libraries(sector_culling_test PRIVATE opencmr2_platform)
add_test(NAME sector_culling COMMAND sector_culling_test)

add_executable(sector_geometry_test sector_geometry.cpp)
target_link_libraries(sector_geometry_test PRIVATE opencmr2_platform)
add_test(NAME sector_geometry COMMAND sector_geometry_test)

add_executable(headlight_collision_test headlight_collision.cpp)
target_link_libraries(headlight_collision_test PRIVATE opencmr2_platform)
add_test(NAME headlight_collision COMMAND headlight_collision_test)

add_library(high_address_fixture STATIC high_address.c)
add_executable(net_frontend_test net_frontend.cpp)
target_link_libraries(net_frontend_test PRIVATE opencmr2_platform high_address_fixture)
add_test(NAME net_frontend COMMAND net_frontend_test)
set_tests_properties(net_frontend PROPERTIES TIMEOUT 30)
target_link_libraries(headlight_collision_test PRIVATE high_address_fixture)
target_link_libraries(sector_culling_test PRIVATE high_address_fixture)

if(WIN32)
    # Windows searches beside each test executable for SDL3.dll. Copy once
    # before linking the tests, including when only stage_probe is requested.
    add_custom_target(opencmr2_test_runtime
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3-shared> ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS SDL3::SDL3-shared)
    get_property(test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS test_targets)
        get_target_property(kind ${target} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            add_dependencies(${target} opencmr2_test_runtime)
        endif()
    endforeach()
endif()
