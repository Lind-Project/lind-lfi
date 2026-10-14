if cpu == 'x86_64'
  lfi_3i_run = executable(
    'lfi-3i-run',
    files('main.c'),
    c_args: c_args_warnings,
    dependencies: [lfi_linux],
    install: true,
  )
endif
