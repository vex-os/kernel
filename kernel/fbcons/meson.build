if kconfig.get('CONFIG_FBCONS', 'n') == 'y'
    # The values in .config keep their quotes
    fbcons_font_file = kconfig.get('CONFIG_FBCONS_FONT_FILE').strip('"')
    fbcons_font_script = files(meson.project_source_root() / 'scripts' / 'fbcons' / 'bin_to_ch.py')

    fbcons_font = custom_target('generated.fbcons_font',
        input: files('fonts' / fbcons_font_file),
        output: ['generated.fbcons_font.c', 'generated.fbcons_font.h'],
        command: [python, fbcons_font_script, '@INPUT@', '@OUTPUT0@', '@OUTPUT1@'],
        depend_files: fbcons_font_script,
    )

    # fbcons.c includes the generated header from the build directory
    kernel_includes += include_directories('.')

    kernel_sources += files('fbcons.c')
    kernel_sources += fbcons_font
endif
