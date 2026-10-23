if(build_target == 'arm')
    nitrowifi_prebuilt += fs.copyfile('prebuilt' / 'libcps.a')
else
    subdir('src')
    libcps = library('cps',
        dependencies: [nitrosdk_dep],
        c_args: nitrowifi_build_args,
        include_directories: [nitrowifi_includes],
        pic: build_target == 'nx',
        sources: libcps_srcs,
        native: (build_target == 'linux' or build_target == 'win64')
    )

    nitrowifi_libs += libcps
endif
