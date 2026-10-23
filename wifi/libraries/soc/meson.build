subdir('src')

libsoc = library('soc',
    dependencies: nitrosdk_dep,
    c_args: nitrowifi_build_args,
    include_directories: nitrowifi_includes,
    pic: build_target == 'nx',
    sources: libsoc_srcs,
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrowifi_libs += libsoc
