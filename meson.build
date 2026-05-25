project('diction', 'c',
  version: '0.1.0',
  meson_version: '>= 0.59.0',
  default_options: [ 'warning_level=2', 'c_std=gnu11', 'buildtype=debugoptimized' ],
)

add_project_arguments('-include', 'safe-fs.h', language: 'c')

cc = meson.get_compiler('c')

gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
webkit_dep = dependency('webkitgtk-6.0', required: false)
if not webkit_dep.found()
  webkit_dep = dependency('webkit2gtk-4.1')
endif
zlib_dep = dependency('zlib')
xml_dep = dependency('libxml-2.0')
json_dep = dependency('json-glib-1.0')
archive_dep = dependency('libarchive')
zstd_dep = dependency('libzstd')
lzma_dep = dependency('liblzma')
bz2_dep = cc.find_library('bz2', required: true)
lzo_dep = dependency('lzo2')
sqlite3_dep = dependency('sqlite3')

engine_srcs = [
  'src/splay-tree.c',
  'src/flat-index.c',
  'src/dict-cache.c',
  'src/dict-chunked.c',
  'src/dict-cache-builder.c',
  'src/resource-reader.c',
  'src/dict-mmap.c',
  'src/dict-dsl.c',
  'src/dict-loader.c',
  'src/dict-fts-index.c',
  'src/dict-mdx.c',
  'src/dict-slob.c',
  'src/dict-bgl.c',
  'src/dict-ifo.c',
  'src/dict-xdxf.c',
  'src/ripemd128.c',
  'src/search-utils.c',
  'src/settings.c',
  'src/langpair.c',
  'src/dictzip.c',
  'src/mdx-decompress.c',
  'src/dict-dictd.c',
  'src/text-utils.c',
  'src/dict-sdict.c',
  'src/dict-dsl-scanner.c',
  'src/dict-dsl-index.c',
  'src/safe-fs.c'
]

srcs = [
  'src/main.c',
  'src/audio-playback.c',
  'src/dict-render.c',
  'src/global-shortcut.c',
  'src/settings-dialog.c',
  'src/scan-popup.c',
  'src/startup-splash.c',
  'src/tray-icon.c',
  'src/json-theme.c',
  'src/dict-stardict-render.c'
] + engine_srcs

gnome = import('gnome')
search_provider_dbus = gnome.gdbus_codegen('search-provider-generated',
  'data/org.gnome.Shell.SearchProvider2.xml',
  interface_prefix: 'org.gnome.Shell.',
  namespace: 'DictionSearchProvider'
)

executable('diction-search-provider',
  ['src/search-provider.c', search_provider_dbus] + engine_srcs,
  include_directories: include_directories('src'),
  c_args: '-DDICTION_NO_GTK',
  dependencies: [ dependency('glib-2.0'), dependency('gio-unix-2.0'), gdk_pixbuf_dep, zlib_dep, json_dep, archive_dep, zstd_dep, lzma_dep, bz2_dep, lzo_dep, xml_dep, sqlite3_dep ],
  install: true
)

executable('diction', srcs,
  include_directories: include_directories('src'),
  dependencies: [ gtk_dep, adw_dep, webkit_dep, zlib_dep, json_dep, archive_dep, zstd_dep, lzma_dep, bz2_dep, lzo_dep, xml_dep, sqlite3_dep ],
  install: true,
)


# Install desktop file
install_data(
  'data/io.github.fastrizwaan.diction.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications')
)

conf_data = configuration_data()
conf_data.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

# Install D-Bus service file
configure_file(
  input: 'data/io.github.fastrizwaan.diction.service.in',
  output: 'io.github.fastrizwaan.diction.service',
  configuration: conf_data,
  install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services')
)

configure_file(
  input: 'data/io.github.fastrizwaan.diction.SearchProvider.service.in',
  output: 'io.github.fastrizwaan.diction.SearchProvider.service',
  configuration: conf_data,
  install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services')
)

install_data(
  'data/io.github.fastrizwaan.diction.search-provider.ini',
  install_dir: join_paths(get_option('datadir'), 'gnome-shell', 'search-providers')
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

# Install symbolic icons
install_data(
  'data/icons/hicolor/scalable/actions/dice3-symbolic.svg',
  'data/icons/hicolor/scalable/actions/search-dictionary-symbolic.svg',
  'data/icons/hicolor/scalable/actions/dictionary-symbolic.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'actions')
)

# Install VSCode Themes
install_subdir(
  'vscode-themes',
  install_dir: join_paths(get_option('datadir'), 'diction', 'themes'),
  strip_directory: true
)
