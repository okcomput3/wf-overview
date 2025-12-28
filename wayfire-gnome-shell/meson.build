project(
    'wayfire-gnome-shell',
    'cpp',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=0.51.0',
    default_options: [
        'cpp_std=c++17',
        'c_std=c11',
        'warning_level=2',
        'werror=false',
    ],
)

wayfire = dependency('wayfire')
wlroots = dependency('wlroots-0.19', 'wlroots', version: '>=0.19')
wfconfig = dependency('wf-config')
cairo = dependency('cairo')
pango = dependency('pango')
pangocairo = dependency('pangocairo')
glesv2 = dependency('glesv2')

add_project_arguments(['-DWLR_USE_UNSTABLE'], language: ['cpp', 'c'])
add_project_arguments(['-DWAYFIRE_PLUGIN'], language: ['cpp', 'c'])

# Use local prefix instead of wayfire's system paths
plugin_dir = join_paths(get_option('libdir'), 'wayfire')
metadata_dir = join_paths(get_option('datadir'), 'wayfire', 'metadata')

subdir('src')
subdir('metadata')
