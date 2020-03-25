project('arcan-trayicon', 'c', default_options : ['c_std=c11'])
shmif = dependency('arcan-shmif')
tui = dependency('arcan-shmif-tui')
thread = dependency('threads')
msgpack = dependency('msgpack')
cc = meson.get_compiler('c')
math = cc.find_library('m', required : false)
executable('nvim-arcan',
	['src/main.c'],
	install : true, dependencies : [shmif, tui, math, thread, msgpack])
