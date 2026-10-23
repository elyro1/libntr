# NitroSystem is built and versioned with NitroSDK. Keep its toolchain
# overrides on these targets: DS games may use different SDK library versions.
nitrosystem_includes = include_directories('include')
nitrosystem_build_args = []
if build_target == 'arm'
    # NitroSystem never enabled NitroSDK's legacy conditional-assembly bug.
    if get_option('sp1p3_conditionals')
        nitrosystem_build_args += '-USP1P3_BUG_FOR_CONDITIONAL_ASM_INSTRUCTIONS'
    endif
    system_mwrap_ver = get_option('nitrosystem_mwrap_ver')
    if system_mwrap_ver != 'default'
        nitrosystem_build_args += ['-wrap:ver', system_mwrap_ver]
    endif
    system_mwrap_sdk = get_option('nitrosystem_mwrap_sdk')
    if system_mwrap_sdk != 'default'
        nitrosystem_build_args += ['-wrap:sdk', system_mwrap_sdk]
    endif
endif

# These simulator extensions are part of this SDK revision. There is no
# separately versioned NitroSystem checkout to probe for compatibility.
nitrosystem_args = []
if build_target == 'linux' or build_target == 'win64'
    nitrosystem_args += [
        '-DLIBNTR_HAS_EXTENDED_TEXTURES',
        '-DLIBNTR_HAS_PLAYER_FAST_FORWARD',
    ]
endif

subdir('libraries')

nitrosystem_dep = declare_dependency(
    compile_args: nitrosystem_args,
    include_directories: nitrosystem_includes,
    dependencies: nitrosdk_dep,
    link_with: nitrosystem_libs,
)
meson.override_dependency('NitroSystem', nitrosystem_dep)
