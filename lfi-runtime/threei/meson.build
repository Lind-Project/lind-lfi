# LFI 3i: cages, grates and process semantics. The host side is built into
# liblfi-linux (see linux/meson.build); this builds the guest-side library and
# registers the test suite.

install_headers(
  'guest/include/lfi_3i_guest.h',
  'guest/include/lind_syscall.h',
  'include/lfi_3i_abi.h',
)

if lficc.found() and cpu == 'x86_64'
  # liblfi3i.a: link it into programs that use the grate API
  # (register_handler, make_threei_call, copy_data_between_cages, ...). It also
  # makes the program able to serve as a grate.
  lfi3i_obj = custom_target('lfi3i.o',
    output: 'lfi3i.o',
    input: 'guest/lfi3i.c',
    command: [lficc, '-O2', '-c', '-I' + meson.current_source_dir() / 'include',
              '-I' + meson.current_source_dir() / 'guest/include',
              '-o', '@OUTPUT@', '@INPUT@'],
  )
  ar = find_program('ar', required: false)
  if ar.found()
    lfi3i_lib = custom_target('liblfi3i.a',
      output: 'liblfi3i.a',
      input: lfi3i_obj,
      command: [ar, 'rcs', '@OUTPUT@', '@INPUT@'],
      install: true,
      install_dir: get_option('libdir'),
    )
  endif

  make = find_program('make', required: false)
  if make.found()
    test('3i-suite', make,
      args: ['-C', meson.current_source_dir() / 'tests', 'check',
             'RUN=' + lfi_3i_run.full_path(),
             'LFICC=' + lficc.full_path()],
      depends: [lfi_3i_run],
      timeout: 900,
      suite: ['3i'],
    )
  endif
endif
