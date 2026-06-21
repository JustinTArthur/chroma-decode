# Local overlay of the wrapdb sqlite3 package (patch_directory in sqlite3.wrap).
# This is not the upstream build: it's trimmed to what libchromadec uses.
# libchromadec embeds this sqlite (link_whole on the bundled-build path)
# and exports only its own C ABI, so we build a single static library, core SQL
# only, symbols hidden, and install nothing. Upstream wrapdb build forces
# options we don't need (every loadable extension, column-metadata,
# dbstat vtab, unlock-notify, secure-delete, public SQLITE_API visibility,
# installed standalone libsqlite3/.pc/header); none of that survives here.
project('sqlite3', 'c',
  license: 'blessing',
  version: '3.53.2',
  meson_version: '>=0.56.0')

cc = meson.get_compiler('c')
libm = cc.find_library('m', required: false)
threads_dep = dependency('threads')

# Core SQL only. libchromadec issues plain prepared statements and exec against
# TBC metadata sidecars: no FTS, R*Tree, dbstat, column-metadata,
# unlock-notify, or loadable extensions. Omitting load-extension removes libdl
# dependency.
sqlite_args = [
  '-DSQLITE_THREADSAFE=1',
  '-DSQLITE_OMIT_LOAD_EXTENSION',
  '-DSQLITE_DQS=0',
  '-DSQLITE_DEFAULT_MEMSTATUS=0',
]

# Host-adaptation probes (carried from upstream) let sqlite use the host libc's
# fixed-width types and faster allocator/string primitives where present.
foreach h : ['inttypes.h', 'malloc.h', 'stdint.h']
  if cc.has_header(h)
    sqlite_args += '-DHAVE_' + h.to_upper().underscorify()
  endif
endforeach
foreach signed : ['', 'u']
  foreach width : ['8', '16', '32']
    typ = signed + 'int' + width + '_t'
    if cc.has_type(typ, prefix: '#include <stdint.h>')
      sqlite_args += '-DHAVE_' + typ.to_upper()
    endif
  endforeach
endforeach
foreach f : ['malloc_usable_size', 'posix_fallocate', 'strchrnul', 'strerror_r']
  if cc.has_function(f)
    sqlite_args += '-DHAVE_' + f.to_upper()
  endif
endforeach
if cc.has_function('isnan', dependencies: libm)
  sqlite_args += '-DSQLITE_HAVE_ISNAN'
endif

libsqlite3 = static_library('sqlite3', files('sqlite3.c'),
  c_args: sqlite_args,
  dependencies: [libm, threads_dep],
  install: false,
  gnu_symbol_visibility: 'hidden')

sqlite3_dep = declare_dependency(
  include_directories: include_directories('.'),
  link_with: libsqlite3)
meson.override_dependency('sqlite3', sqlite3_dep)