subdir('src')

libwcm_includes = include_directories('include')

libwcm = library('wcm',
    dependencies: nitrosdk_dep,
    c_args: nitrowifi_build_args,
    include_directories: [nitrowifi_includes, libwcm_includes],
    pic: build_target == 'nx',
    sources: libwcm_srcs,
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrowifi_libs += libwcm
