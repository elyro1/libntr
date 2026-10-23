# Preserve NitroWiFi's toolchain selection without changing core SDK flags.
nitrowifi_build_args = []
if get_option('nitrowifi_sp1p3_conditionals')
    nitrowifi_build_args += '-DSP1P3_BUG_FOR_CONDITIONAL_ASM_INSTRUCTIONS'
elif get_option('sp1p3_conditionals')
    nitrowifi_build_args += '-USP1P3_BUG_FOR_CONDITIONAL_ASM_INSTRUCTIONS'
endif
if build_target == 'arm'
    wifi_mwrap_ver = get_option('nitrowifi_mwrap_ver')
    if wifi_mwrap_ver != 'default'
        nitrowifi_build_args += ['-wrap:ver', wifi_mwrap_ver]
    endif
    wifi_mwrap_sdk = get_option('nitrowifi_mwrap_sdk')
    if wifi_mwrap_sdk != 'default'
        nitrowifi_build_args += ['-wrap:sdk', wifi_mwrap_sdk]
    endif
endif

nitrowifi_includes = include_directories('include')
subdir('libraries')

nitrowifi_dep = declare_dependency(
    dependencies: nitrosdk_dep,
    include_directories: nitrowifi_includes,
    link_with: nitrowifi_libs + nitrowifi_prebuilt,
)
meson.override_dependency('NitroWiFi', nitrowifi_dep)
