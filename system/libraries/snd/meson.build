subdir('src')

libnnssnd = library('nnssnd',
    dependencies: nitrosdk_dep,
    c_args: nitrosystem_build_args + nitrosystem_args,
    include_directories: nitrosystem_includes,
    pic: build_target == 'nx',
    sources: libnnssnd_srcs,
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrosystem_libs += libnnssnd
