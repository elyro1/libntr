
libsubpsyscall_arm7 = static_library('subpsyscall_arm7',
    c_args: nitrosdk_quiet_args,
    sources: 'src/subpsyscall.s',
    nasm_args: ['-proc', 'arm4t', '-16', '-gccinc'],
    pic: false,
    include_directories: nitrosdk_includes,
)

nitrosdk_arm7_libs += libsubpsyscall_arm7
