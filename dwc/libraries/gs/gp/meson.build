libdwcgsgp = library('dwcgsgp',
    dependencies: [nitrosdk_dep, nitrosystem_dep, nitrowifi_dep],
    c_args: nitrodwc_build_args + nitrodwc_args,
    include_directories: nitrodwc_includes,
    pic: build_target == 'nx',
    sources: files(
        'gp.c',
        'gpi.c',
        'gpiBuddy.c',
        'gpiBuffer.c',
        'gpiCallback.c',
        'gpiConnect.c',
        'gpiInfo.c',
        'gpiOperation.c',
        'gpiPeer.c',
        'gpiProfile.c',
        'gpiSearch.c',
        'gpiTransfer.c',
        'gpiUnique.c',
        'gpiUtility.c'
    ),
    native: (build_target == 'linux' or build_target == 'win64')
)

nitrodwc_libs += libdwcgsgp
