# Voice chat keeps the original DS archive and native implementation.
if build_target == 'arm'
libvct = fs.copyfile('prebuilt' / 'libvct.a')
endif

if build_target == 'arm'
libvct_dep = declare_dependency(
    dependencies: nitrosdk_dep,
    include_directories: include_directories('include'),
    link_with: libvct
)
else
subdir('src')
libvct = static_library('vct',
    sources: libvct_srcs,
    include_directories: include_directories('include'),
    dependencies: [nitrosdk_dep],
    pic: true,
    native: (build_target == 'linux' or build_target == 'win64')
)
libvct_dep = declare_dependency(
    dependencies: nitrosdk_dep,
    include_directories: include_directories('include'),
    link_with: libvct
)
endif

meson.override_dependency('libvct', libvct_dep)
