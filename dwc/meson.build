# DWC uses its own SDK library version and optional Pokemon lobby hooks.
nitrodwc_build_args = []
if get_option('sp1p3_conditionals')
    nitrodwc_build_args += '-USP1P3_BUG_FOR_CONDITIONAL_ASM_INSTRUCTIONS'
endif
if build_target == 'arm'
    dwc_mwrap_ver = get_option('nitrodwc_mwrap_ver')
    if dwc_mwrap_ver != 'default'
        nitrodwc_build_args += ['-wrap:ver', dwc_mwrap_ver]
    endif
    dwc_mwrap_sdk = get_option('nitrodwc_mwrap_sdk')
    if dwc_mwrap_sdk != 'default'
        nitrodwc_build_args += ['-wrap:sdk', dwc_mwrap_sdk]
    endif
endif

nitrodwc_args = ['-D_NITRO']
if get_option('nitrodwc_link_ppwlobby')
    nitrodwc_args += '-DLINK_PPWLOBBY'
endif
nitrodwc_includes = include_directories('include', 'include/gs', 'include/base')
subdir('libraries')

nitrodwc_dep = declare_dependency(
    compile_args: nitrodwc_args,
    dependencies: nitrosdk_dep,
    include_directories: nitrodwc_includes,
    link_with: nitrodwc_libs + nitrodwc_prebuilt,
)
meson.override_dependency('NitroDWC', nitrodwc_dep)
