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
json_dep = dependency('json-glib-1.0')
archive_dep = dependency('libarchive')
zstd_dep = dependency('libzstd')
lzma_dep = dependency('liblzma')
bz2_dep = cc.find_library('bz2', required: true)

srcs = [
  'src/main.c',
  'src/splay-tree.c',
  'src/flat-index.c',
  'src/dict-cache.c',
  'src/resource-reader.c',
  'src/dict-mmap.c',
  'src/dict-loader.c',
  'src/dict-mdx.c',
  'src/dict-slob.c',
  'src/dict-bgl.c',
  'src/dict-ifo.c',
  'src/dict-render.c',
  'src/ripemd128.c',
  'src/settings.c',
  'src/settings-dialog.c',
  'src/scan-popup.c',
  'src/tray-icon.c'
]

executable('diction', srcs,
  dependencies: [ gtk_dep, adw_dep, webkit_dep, zlib_dep, json_dep, archive_dep, zstd_dep, lzma_dep, bz2_dep ],
  install: true,
)

# Install desktop file
install_data(
  'data/io.github.fastrizwaan.diction.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications')
)

# Install AppData/AppStream metadata
install_data(
  'data/io.github.fastrizwaan.diction.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo')
)

# Install icon
install_data(
  'data/icons/io.github.fastrizwaan.diction.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps')
)

# Install dice3 symbolic icon (Matches your pyproject requirement)
install_data(
  'data/icons/hicolor/scalable/actions/dice3-symbolic.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'actions')
)