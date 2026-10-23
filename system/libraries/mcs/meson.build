subdir('src')

libnnsmcs_includes = include_directories('include')

libnnsmcs = library('nnsmcs',
    dependencies: nitrosdk_dep,
    c_args: nitrosystem_build_args + nitrosystem_args,
    include_directories: [nitrosystem_includes, libnnsmcs_includes],
    pic: build_target == 'nx',
    sources: libnnsmcs_srcs,
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrosystem_libs += libnnsmcs
