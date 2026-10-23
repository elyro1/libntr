subdir('src')

libdwcaccount = library('dwcaccount',
    dependencies: [nitrosdk_dep, nitrosystem_dep, nitrowifi_dep],
    c_args: nitrodwc_build_args + nitrodwc_args,
    include_directories: nitrodwc_includes,
    pic: true,
    sources: libdwcaccount_srcs,
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrodwc_libs += libdwcaccount
