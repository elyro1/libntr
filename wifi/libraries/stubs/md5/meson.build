subdir('src')

libstubsmd5 = library('stubsmd5',
    dependencies: nitrosdk_dep,
    c_args: nitrowifi_build_args,
    include_directories: nitrowifi_includes,
    pic: build_target == 'nx',
    sources: libstubsmd5_srcs,
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrowifi_libs += libstubsmd5
