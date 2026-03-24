project('diction', 'c',
  version: '0.1.0',
  meson_version: '>= 0.59.0',
  default_options: [ 'warning_level=2', 'c_std=gnu11' ],
)

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')
webkit_dep = dependency('webkitgtk-6.0', required: false)
if not webkit_dep.found()
  webkit_dep = dependency('webkit2gtk-4.1')
endif
zlib_dep = dependency('zlib')

srcs = [
  'src/main.c',
  'src/splay-tree.c',
  'src/dict-mmap.c',
  'src/dict-loader.c',
  'src/dict-mdx.c',
  'src/dict-bgl.c',
  'src/dict-ifo.c',
  'src/dsl-render.c',
  'src/ripemd128.c'
]

executable('diction', srcs,
  dependencies: [ gtk_dep, adw_dep, webkit_dep, zlib_dep ],
  install: true,
)
