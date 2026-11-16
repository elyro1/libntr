libsim_srcs = files(
    'config/config_check.c',
    'config/sim_config.c',
    'g3_draw.cpp',
    'g3_handler.cpp',
    'queue.c',
    'sim_assert.c',
    'sim_audio.cpp',
    'sim_crash.cpp',
    'sim_crc32.c',
    'sim_debug.cpp',
    'sim_g2.cpp',
    'sim_g3_CommandProcessor.cpp',
    'sim_gui.cpp',
    'sim_gui_config.cpp',
    'sim_gui_net.cpp',
    'sim_gui_pad.cpp',
    'sim_autopilot.cpp',
    'sim_main.cpp',
    'sim_frame_profile.cpp',
    'sim_net.cpp',
    'sim_RomExtractor.cpp',
    'sim_screenquads.c',
    'sim_shaders.cpp',
)
libsim_vars_srcs = files('sim_vars.c')

# Vendored code, built without warnings.
libsim_vendor_srcs = files(
    'blip-buf/blip_buf.c',
    'config/ini.c',
    'fireflower/nds-extract.cpp',
    'imgui/imgui.cpp',
    'imgui/imgui_demo.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_impl_opengl3.cpp',
    'imgui/imgui_impl_sdl2.cpp',
    'imgui/imgui_tables.cpp',
    'imgui/imgui_widgets.cpp',
    'ImGuiFileDialog/ImGuiFileDialog.cpp',
)

if(build_target == 'linux' or build_target == 'win64')
libsim_vendor_srcs += files('glad.c')
endif
