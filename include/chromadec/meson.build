version_parts = meson.project_version().split('.')
version_cfg = configuration_data()
version_cfg.set('CHROMADEC_VERSION_MAJOR',  version_parts[0])
version_cfg.set('CHROMADEC_VERSION_MINOR',  version_parts[1])
version_cfg.set('CHROMADEC_VERSION_PATCH',  version_parts[2])
version_cfg.set('CHROMADEC_VERSION_STRING', meson.project_version())

version_h = configure_file(
  input:         'version.h.in',
  output:        'version.h',
  configuration: version_cfg,
  install:       true,
  install_dir:   get_option('includedir') / 'chromadec')

# include_directories('../..') resolves to build/include, so consumers can
# write `<chromadec/version.h>` and it picks up build/include/chromadec/version.h.
version_inc = include_directories('../..')
