/*
 * TUI based UI frontend for NeoVIM
 *
 * Missing / basic things:
 *  - default / background option attribute not set
 *  - clipboard action
 *  - default color bug with external terminal colors
 *  - scrolling- garbage screen contents
 *
 * - options to explore:
 * - multiple grids
 *
 * - external popups:
 *   - option: ext_popupmenu
 *   - notifications: popupmenu_show (items, selected, row, col, grid)
 *                    items : array (word, kind, menu, info)
 *                    - probe availability by first trying to get a subwindow
 *
 */
#include <arcan_shmif.h>
#include <arcan_tui.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <msgpack.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <ctype.h>
#include "uthash.h"

#ifndef COUNT_OF
#define COUNT_OF(x) \
	((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
#endif

struct hl_state {
	struct tui_screen_attr attr;
	bool got_fg, got_bg;
	uint64_t id;
	UT_hash_handle hh;
};

static struct hl_state* highlights;

static struct {
/*
 * multiple grids will be dealt with in a serial manner through _process,
 * which means that [out] should not need a mutex for protection - but
 * there is a 'near' impossible edge where multiple contexts gets
 * multipart paste operations, though that would require some disturbing
 * behavior on the WM side */
	msgpack_packer* out;
	uint32_t reqid;

	struct tui_context* grids[32];
	size_t n_grids;

/* multigrid feature requires much more WM integration - safer to have
 * that as an opt-in rather than default */
	bool multigrid;

/* externalized popups */
	bool popups;

/* externalize input prompt */
	bool messages;

/* need to track which, if any, grid is in multipart- paste state */
	ssize_t paste_lock;

/*
 * used for synching - there is an input thread for data coming from nvim
 * and a render thread for processing each active context. If there is input
 * that causes tui_writes while it is in a processing state, there is the
 * possibility of race condition causing cell contents to go out of synch
 *
 * due to the io-multiplex nature, we go with a synch-fd for waking up
 * and two mutexes to get the cvar-producer-consumer setup going - sigfd
 * accepts 'q' (quit) and 'l' (lock), synch guards the context and hold
 * keeps the main thread from starving lock on synch.
 */
	pthread_mutex_t synch;
	pthread_mutex_t hold;
	int sigfd;
	int lock_level;

	FILE* trace_out;
} nvim = {
	.synch = PTHREAD_MUTEX_INITIALIZER,
	.hold = PTHREAD_MUTEX_INITIALIZER,
	.paste_lock = -1
};

struct nvim_meta {
	int cx, cy;
	int grid_id;
	int button_mask;
};

static inline void trace(const char* msg, ...)
{
	if (!nvim.trace_out)
		return;

	va_list args;
	va_start( args, msg );
		vfprintf(nvim.trace_out,  msg, args );
	va_end( args);
	fputs("\n", nvim.trace_out);
}

static void trace_obj_array(const msgpack_object_array* arg)
{
	if (!nvim.trace_out)
		return;

	struct msgpack_object obj = {
		.type = MSGPACK_OBJECT_ARRAY,
		.via.array = *arg
	};

	msgpack_object_print(nvim.trace_out, obj);
	fprintf(nvim.trace_out, "\n");
}

static uint32_t nvim_request_str(const char* str, size_t sz)
{
	uint32_t id = nvim.reqid++;
	msgpack_pack_array(nvim.out, 4);
	msgpack_pack_int(nvim.out, 0);
	msgpack_pack_uint32(nvim.out, id);
	msgpack_pack_bin(nvim.out, sz);
	msgpack_pack_bin_body(nvim.out, str, sz);
/* out is already tied to our FILE- and that will flush for us,
 * possible hashtable on ID and add ourselves there */
	return id;
}

static uint32_t nvim_set_key_i(const char* key, int val)
{
	uint32_t id = nvim.reqid++;
	size_t klen = strlen(key);

	msgpack_pack_array(nvim.out, 2);
	msgpack_pack_str(nvim.out, klen);
	msgpack_pack_str_body(nvim.out, key, klen);
	msgpack_pack_int(nvim.out, val);

	return id;
}

static bool nvim_str_match(
	const msgpack_object_str* instr, const char* msg)
{
	size_t msg_sz = strlen(msg);
	return (instr->size == msg_sz && memcmp(instr->ptr, msg, msg_sz) == 0);
}

static bool query_label(struct tui_context* ctx,
	size_t ind, const char* country, const char* lang,
	struct tui_labelent* dstlbl, void* t)
{
	trace("query_label(%zu for %s:%s)\n",
		ind, country ? country : "unknown(country)",
		lang ? lang : "unknown(language)");

	return false;
}

static void update_cval(uint64_t val, uint8_t rgb[static 3])
{
	if ((uint64_t)-1 == val){
		struct tui_screen_attr attr = arcan_tui_defattr(nvim.grids[0], NULL);
		rgb[0] = attr.fc[0];
		rgb[1] = attr.fc[1];
		rgb[2] = attr.fc[2];
	}
	else {
		rgb[2] = (val & 0x000000ff);
		rgb[1] = (val & 0x0000ff00) >>  8;
		rgb[0] = (val & 0x00ff0000) >> 16;
	}
}

static bool on_label(struct tui_context* c, const char* label, bool act, void* t)
{
	trace("label(%s)", label);
	return false;
}

static bool on_alabel(struct tui_context* c, const char* label,
		const int16_t* smpls, size_t n, bool rel, uint8_t datatype, void* t)
{
	trace("a-label(%s)", label);
	return false;
}

static void build_mouse_packet(
	const char* button, const char* action,
	const char* mod, int grid, int row, int col)
{
	size_t button_sz = strlen(button) + 1;
	size_t action_sz = strlen(action) + 1;
	size_t mod_sz = strlen(mod) + 1;
	if (mod_sz == 1)
		mod_sz = 0;

	const char mouse_cmd[] = "nvim_input_mouse";
	nvim_request_str(mouse_cmd, sizeof(mouse_cmd) - 1);
	msgpack_pack_array(nvim.out, 6);
	msgpack_pack_str(nvim.out, button_sz);
	msgpack_pack_str_body(nvim.out, button, button_sz);
	msgpack_pack_str(nvim.out, action_sz);
	msgpack_pack_str_body(nvim.out, action, action_sz);
	msgpack_pack_str(nvim.out, mod_sz);
	msgpack_pack_str_body(nvim.out, mod, mod_sz);
	msgpack_pack_int(nvim.out, grid);
	msgpack_pack_int(nvim.out, row);
	msgpack_pack_int(nvim.out, col);
}

/*
 * api complains when we attempt to do this, might be some other way
 */
static void request_buffer_contents()
{
	const char lines_cmd[] = "nvim_input_get_lines";
	nvim_request_str(lines_cmd, sizeof(lines_cmd) - 1);
	msgpack_pack_array(nvim.out, 5);
	msgpack_pack_int(nvim.out, 0); /* ch */
	msgpack_pack_int(nvim.out, 0); /* buffer */
	msgpack_pack_int(nvim.out, 0); /* start */
	msgpack_pack_int(nvim.out, -1); /* end */
	msgpack_pack_int(nvim.out, 0); /* overflow? */
}

static void on_mouse_button(struct tui_context* c,
	int last_x, int last_y, int button, bool active, int modifiers, void* t)
{
	trace("mouse_btn(%d:%d, mods:%d, index: %d");
	struct nvim_meta* m = t;

/* don't consider release for wheel action */
	if (!active && button >= TUIBTN_WHEEL_UP)
		return;

	const char* action, (* btn);

	/* 1: {button} */
	bool wheel = false;
	switch(button){
	case TUIBTN_LEFT:
		btn = "left";
	break;
	case TUIBTN_RIGHT:
		btn = "right";
	break;
	case TUIBTN_MIDDLE:
		btn = "middle";
	break;
	case TUIBTN_WHEEL_UP:
	case TUIBTN_WHEEL_DOWN:
	default:
		wheel = true;
		btn = "wheel";
	break;
	}

/* 2: {action} */
	if (wheel){
		if (button == TUIBTN_WHEEL_UP){
			action = "up";
		}
		else {
			action = "down";
		}
	}
	else if (active){
		action = "press";
		m->button_mask |= 1 << TUIBTN_LEFT;
	}
	else {
		m->button_mask &= ~(1 << TUIBTN_LEFT);
		action = "release";
	}

/* modifier to button follows same rule as for normal input,
 * i.e. C-A (though not as <Ca> */

	build_mouse_packet(btn, action, "", m->grid_id, last_y, last_x);
}

static void on_mouse(struct tui_context* c,
	bool relative, int x, int y, int modifiers, void* t)
{
	struct nvim_meta* m = t;
	if (!m->button_mask || relative)
		return;

	const char* btn = "left";
	if (TUIBTN_LEFT & m->button_mask)
		btn = "left";
	else if (TUIBTN_RIGHT & m->button_mask)
		btn = "right";
	else if (TUIBTN_MIDDLE & m->button_mask)
		btn = "middle";
	else
		return;

	build_mouse_packet(btn, "drag", "", m->grid_id, y, x);
}

static void on_key(struct tui_context* c, uint32_t ksym,
	uint8_t scancode, uint8_t mods, uint16_t subid, void* t)
{
	trace("unknown_key(%"PRIu32",%"PRIu8",%"PRIu16")", ksym, scancode, subid);

	char str[16];
	size_t ofs = 0;

	str[ofs++] = '<';

	if (mods & (TUIM_LCTRL | TUIM_RCTRL)){
		str[ofs++] = 'C';
		str[ofs++] = '-';
	}
	if (mods & (TUIM_LALT | TUIM_RALT)){
		str[ofs++] = 'A';
		str[ofs++] = '-';
	}
	if (mods & (TUIM_LSHIFT | TUIM_RSHIFT)){
		str[ofs++] = 'S';
		str[ofs++] = '-';
	}
	if (mods & (TUIM_LMETA | TUIM_RMETA)){
		str[ofs++] = 'M';
		str[ofs++] = '-';
	}


/* is the keysym part of the visible set? then just add it like that */
/* otherwise follow the special treatment for various things */
	if (isprint(ksym)){
		str[ofs++] = ksym;
	}
	else {
		switch(ksym){
		case TUIK_F1:
			str[ofs++] = 'F';
			str[ofs++] = '1';
		break;
		case TUIK_F2:
			str[ofs++] = 'F';
			str[ofs++] = '2';
		break;
		case TUIK_F3:
			str[ofs++] = 'F';
			str[ofs++] = '3';
		break;
		case TUIK_F4:
			str[ofs++] = 'F';
			str[ofs++] = '4';
		break;
		case TUIK_F5:
			str[ofs++] = 'F';
			str[ofs++] = '5';
		break;
		case TUIK_F6:
			str[ofs++] = 'F';
			str[ofs++] = '6';
		break;
		case TUIK_F7:
			str[ofs++] = 'F';
			str[ofs++] = '7';
		break;
		case TUIK_F8:
			str[ofs++] = 'F';
			str[ofs++] = '8';
		break;
		case TUIK_F9:
			str[ofs++] = 'F';
			str[ofs++] = '9';
		break;
		case TUIK_F10:
			str[ofs++] = 'F';
			str[ofs++] = '1';
			str[ofs++] = '0';
		break;
		case TUIK_F11:
			str[ofs++] = 'F';
			str[ofs++] = '1';
			str[ofs++] = '1';
		break;
		case TUIK_F12:
			str[ofs++] = 'F';
			str[ofs++] = '1';
			str[ofs++] = '2';
		break;
		case TUIK_ESCAPE:
			str[ofs++] = 'E';
			str[ofs++] = 'S';
			str[ofs++] = 'C';
		break;
		case TUIK_LEFT:
			str[ofs++] = 'L';
			str[ofs++] = 'e';
			str[ofs++] = 'f';
			str[ofs++] = 't';
		break;
		case TUIK_RIGHT:
			str[ofs++] = 'R';
			str[ofs++] = 'i';
			str[ofs++] = 'g';
			str[ofs++] = 'h';
			str[ofs++] = 't';
		break;
		case TUIK_UP:
			str[ofs++] = 'U';
			str[ofs++] = 'p';
		break;
		case TUIK_DOWN:
			str[ofs++] = 'D';
			str[ofs++] = 'o';
			str[ofs++] = 'w';
			str[ofs++] = 'n';
		break;
		case TUIK_PAGEDOWN:
			str[ofs++] = 'P';
			str[ofs++] = 'a';
			str[ofs++] = 'g';
			str[ofs++] = 'e';
			str[ofs++] = 'D';
			str[ofs++] = 'o';
			str[ofs++] = 'w';
			str[ofs++] = 'n';
		break;
		case TUIK_PAGEUP:
			str[ofs++] = 'P';
			str[ofs++] = 'a';
			str[ofs++] = 'g';
			str[ofs++] = 'e';
			str[ofs++] = 'U';
			str[ofs++] = 'p';
		break;
		case TUIK_HOME:
			str[ofs++] = 'H';
			str[ofs++] = 'o';
			str[ofs++] = 'm';
			str[ofs++] = 'e';
		break;
		case TUIK_END:
			str[ofs++] = 'E';
			str[ofs++] = 'n';
			str[ofs++] = 'd';
		break;
		case TUIK_INSERT:
			str[ofs++] = 'I';
			str[ofs++] = 'n';
			str[ofs++] = 's';
			str[ofs++] = 'e';
			str[ofs++] = 'r';
			str[ofs++] = 't';
		break;
		case TUIK_DELETE:
			str[ofs++] = 'D';
			str[ofs++] = 'e';
			str[ofs++] = 'l';
		break;
		default:
			fprintf(stderr, "missing key %d\n", ksym);
		return;
		}
	}

	str[ofs++] = '>';
	const char cmd[] = "nvim_input";
	nvim_request_str(cmd, sizeof(cmd) - 1);
	msgpack_pack_array(nvim.out, 1);
	msgpack_pack_str(nvim.out, ofs);
	msgpack_pack_str_body(nvim.out, str, ofs);
	str[ofs++] = 0;
}

static bool on_u8(struct tui_context* c, const char* u8, size_t len, void* t)
{
	uint8_t buf[5] = {0};
	memcpy(buf, u8, len >= 5 ? 4 : len);
	trace("on_u8(%zu:%s)", len, buf);

	const char cmd[] = "nvim_input";
	nvim_request_str(cmd, sizeof(cmd) - 1);
	msgpack_pack_array(nvim.out, 1);

	if (*u8 == '<'){
		msgpack_pack_str(nvim.out, 4);
		msgpack_pack_str_body(nvim.out, "<LT>", 4);
	}
	else {
		msgpack_pack_str(nvim.out, len);
		msgpack_pack_str_body(nvim.out, u8, len);
	}

	return true;
}

static void on_misc(struct tui_context* c, const arcan_ioevent* ev, void* t)
{
	trace("on_ioevent()");
}

static void on_state(struct tui_context* c, bool input, int fd, void* t)
{
	trace("on-state(in:%d)", (int)input);
}

static void on_bchunk(struct tui_context* c,
	bool input, uint64_t size, int fd, void* t)
{
	trace("on_bchunk(%"PRIu64", in:%d)", size, (int)input);
}

static void on_vpaste(struct tui_context* c,
		shmif_pixel* vidp, size_t w, size_t h, size_t stride, void* t)
{
	trace("on_vpaste(%zu, %zu str %zu)", w, h, stride);
/* nvim_paste, data:string or binary, :crlf, :phase: 1start, 2 cont, 3end */
}

static void on_apaste(struct tui_context* c,
	shmif_asample* audp, size_t n_samples, size_t frequency, size_t nch, void* t)
{
	trace("on_apaste(%zu @ %zu:%zu)", n_samples, frequency, nch);
}

static void on_tick(struct tui_context* c, void* t)
{
/* ignore this, rather noise:	trace("[tick]"); */
}

static void on_utf8_paste(struct tui_context* c,
	const uint8_t* str, size_t len, bool cont, void* t)
{
	trace("utf8-paste(%s):%d", str, (int) cont);
	/* nvim_paste(data, cont ? phase == -1 single, 1: first, 2: cont, 3: end */
	const char cmd[] = "nvim_paste";
	struct nvim_meta* nvim_grid = t;

/*
 * -1 : single
 *  1 : first in multipart
 *  2 : part in multipart
 *  3 : end of multipart
 */
	int mode = -1;

	if (-1 == nvim.paste_lock){
		if (cont){
			mode = 1;
			nvim.paste_lock = nvim_grid->grid_id;
		}
	}

/* ignore the paste, already busy here - if it becomes a problem, possibly
 * dup and queue - the thing is that paste does not carry a grid id so screwed
 * anyhow without modifying nvim */
	else if (nvim.paste_lock != nvim_grid->grid_id){
		return;
	}
	else {
		if (cont){
			mode = 2;
		}
		else {
			mode = 3;
			nvim.paste_lock = -1;
		}
	}

	nvim_request_str(cmd, sizeof(cmd) - 1);

	msgpack_pack_array(nvim.out, 3);
	msgpack_pack_str(nvim.out, len);
	msgpack_pack_str_body(nvim.out, str, len);
/* should possibly expose as a label to get controls for CR/LF, CRLF, LF */
	msgpack_pack_int(nvim.out, true);
	msgpack_pack_int(nvim.out, mode);
}

static void on_resize(struct tui_context* c,
	size_t neww, size_t newh, size_t col, size_t row, void* t)
{
	trace("resize(%zu(%zu),%zu(%zu))", neww, col, newh, row);
	struct nvim_meta* m = t;
	if (!nvim.out)
		return;

	const char cmd[] = "nvim_ui_try_resize_grid";
	nvim_request_str(cmd, sizeof(cmd) - 1);
	msgpack_pack_array(nvim.out, 3);
	msgpack_pack_int(nvim.out, m->grid_id);
	msgpack_pack_int64(nvim.out, col);
	msgpack_pack_int64(nvim.out, row);
}

struct nvim_cmd {
	const char* lbl;
	bool (*ptr)(const msgpack_object_array* arg);
};

/* this comes from the notifications, so it expects it to be of
 * [cmd, [grid, ...]] */
static struct tui_context* nvim_grid_to_tui(const msgpack_object_array* arg)
{
	if (arg->size < 2 ||
		arg->ptr[1].type != MSGPACK_OBJECT_ARRAY ||
		arg->ptr[1].via.array.size < 1 ||
		arg->ptr[1].via.array.ptr[0].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return NULL;

/* find matching and if not, grab first free and set the grid-id, request
 * a new tui window for it - since this is asynch and we need a context
 * directly to make things easier
 *
 * The solution to that is to extend tui with the option of making a
 * 'connectionless- tui' window that behaves like a normal one, but won't
 * refresh / transfer / do anything. Then when we get a subwindow event, we can
 * quickly transfer this with a 'window_bind' call.
 */
	if (nvim.multigrid){
		uint64_t grid_id = arg->ptr[1].via.array.ptr[0].via.u64;
		for (size_t i = 0; i < nvim.n_grids; i++){
		}
	}

/* incomplete, need to map grid to active context */
	return nvim.grids[0];
}

static bool draw_resize(const msgpack_object_array* arg)
{
/* arcan_tui_wndhint */
	return true;
}

static bool draw_line(int gid,
	unsigned row, unsigned offset, const msgpack_object_array* line)
{
	struct tui_context* grid = nvim.grids[0];
	struct tui_cbcfg cbcfg;
	arcan_tui_update_handlers(grid, NULL, &cbcfg, sizeof(cbcfg));
	struct nvim_meta* grid_meta = cbcfg.tag;
	arcan_tui_move_to(grid, offset, row);

/* format depends on individual line size:
 * 1 item  : [ch]
 * 2 items : [ch, hlid]
 * 3 items : [ch, hlid, repeat]
 *
 * if hlid is not set, grab the last defined one - global */
	struct tui_screen_attr defattr = arcan_tui_defattr(grid, NULL);
	struct tui_screen_attr cattr = defattr;
	struct hl_state* hl = NULL;

	for (size_t i = 0; i < line->size; i++){
		if (line->ptr[i].type != MSGPACK_OBJECT_ARRAY)
			return false;
		const msgpack_object_array* cell = &line->ptr[i].via.array;

		if (cell->ptr[0].type != MSGPACK_OBJECT_STR)
			return false;

		if (cell->size > 1){
			struct hl_state* new;
			HASH_FIND_INT(highlights, &cell->ptr[1].via.u64, new);
			if (new)
				hl = new;
		}

		if (hl){
			cattr = hl->attr;
			if (!hl->got_fg)
				memcpy(cattr.fc, defattr.fc, 3);
			if (!hl->got_bg)
				memcpy(cattr.bc, defattr.bc, 3);
		}
		else {
			trace("missing highlight attribute");
		}

		size_t count = 1;
		if (cell->size == 3)
			count = cell->ptr[2].via.u64;

		for (size_t i = 0; i < count; i++)
			arcan_tui_writeu8(grid,
				(uint8_t*) cell->ptr[0].via.str.ptr,
				cell->ptr[0].via.str.size, &cattr
			);
	}

/* restore known cursor position, not doing this caused the
 * cursor to sometimes look like it was stuck at end of line */
	arcan_tui_move_to(grid, grid_meta->cx, grid_meta->cy);
	return true;
}

static bool draw_lines(const msgpack_object_array* arg)
{
/* arg is array with command as first element,
 * all other elements are arrays representing a line */
	for (size_t line = 1; line < arg->size; line++){
		if (arg->ptr[line].type != MSGPACK_OBJECT_ARRAY)
			continue;

		const msgpack_object_array* l = &arg->ptr[line].via.array;
		if (l->size != 4)
			return false;

/* invalid grid number argument */
		if (l->ptr[0].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
			return false;

		uint64_t grid = l->ptr[0].via.u64;

/* invalid start row argument */
		if (l->ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
			return false;

		uint64_t row = l->ptr[1].via.u64;

/* invalid start row argument */
		if (l->ptr[2].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
			return false;

		uint64_t col = l->ptr[2].via.u64;

		if (!draw_line(grid, row, col, &l->ptr[3].via.array))
			return false;
/* rest is array of characters */
	}

	return true;
}

static bool grid_clear(const msgpack_object_array* arg)
{
	struct tui_context* tui = nvim_grid_to_tui(arg);
	if (!tui)
		return false;

	arcan_tui_erase_screen(tui, false);
	return true;
}

static bool draw_destroy(const msgpack_object_array* arg)
{
	return true;
}

static void copy_row(
	struct tui_context* T, size_t l, size_t r, size_t src, size_t dst)
{
	arcan_tui_move_to(T, l, dst);
	for (size_t col = l; col < r; col++){
		struct tui_cell cell = arcan_tui_getxy(T, col, src, true);
/* this might be a bug with tui, investigate - sometimes cells with
 * zero content won't get cleared / updated, this might be tied to some
 * terminal emulator visual leftovers we have had in the past */
		if (!cell.ch)
			cell.ch = ' ';

		arcan_tui_write(T, cell.ch, &cell.attr);
	}
}

static bool grid_scroll(const msgpack_object_array* arg)
{
	struct tui_context* grid = nvim_grid_to_tui(arg);
	if (arg->size != 2 || arg->ptr[1].via.array.size != 7)
		return false;

/* id, top, bottom, left, right, rows, cols */
	const msgpack_object_array* args = &arg->ptr[1].via.array;
	int64_t t, b, l, r, rows, cols;

	if (args->ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	t = args->ptr[1].via.u64;

	if (args->ptr[2].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	b = args->ptr[2].via.u64;

	if (args->ptr[3].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	l = args->ptr[3].via.u64;

	if (args->ptr[4].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	r = args->ptr[4].via.u64;

	if (args->ptr[5].type != MSGPACK_OBJECT_POSITIVE_INTEGER &&
			args->ptr[5].type != MSGPACK_OBJECT_NEGATIVE_INTEGER)
		return false;
	rows = args->ptr[5].via.i64;

	if (args->ptr[6].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	cols = args->ptr[6].via.i64;

/* this was reserved according to the documentation */
	if (cols != 0){
		trace("non-zero cols");
	}

/* scroll down */
	if (rows > 0){
		for (int64_t crow = t; crow + rows < b; crow++){
			copy_row(grid, l, r, crow + rows, crow);
		}
	}
	else{
		for (int64_t crow = b - 1; crow + rows >= t; --crow){
			copy_row(grid, l, r, crow + rows, crow);
		}
	}

	return true;
}

static bool grid_goto(const msgpack_object_array* arg)
{
	struct tui_context* grid = nvim_grid_to_tui(arg);
	struct tui_cbcfg cbcfg;
	arcan_tui_update_handlers(grid, NULL, &cbcfg, sizeof(cbcfg));
	struct nvim_meta* grid_meta = cbcfg.tag;

/* can now assume [cmd, [gid, ...] structure */

	const msgpack_object_array* gargs = &arg->ptr[1].via.array;
	if (gargs->size != 3)
		return false;

	if (gargs->ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	uint64_t row = gargs->ptr[1].via.u64;

	if (gargs->ptr[2].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
		return false;
	uint64_t col = gargs->ptr[2].via.u64;

	grid_meta->cx = col;
	grid_meta->cy = row;
	arcan_tui_move_to(grid, col, row);
	return true;
}

static bool highlight_attribute(const msgpack_object_array* arg)
{
	for (size_t i = 1; i < arg->size; i++){
		struct hl_state* state;
		const msgpack_object_array* ci = &arg->ptr[i].via.array;
		uint64_t attrid = ci->ptr[0].via.u64;

/* fetch or add, set default */
		HASH_FIND_INT(highlights, &attrid, state);
		if (!state){
			state = malloc(sizeof(struct hl_state));
			state->id = attrid;
			HASH_ADD_INT(highlights, id, state);
		}
		state->attr = arcan_tui_defattr(nvim.grids[0], NULL);
		state->got_fg = false;
		state->got_bg = false;

/* should be size [4],
 * id (u64), rgb (use this), cterm (ignore this), info (use this) */
		if (ci->size != 4){
			trace("hl_attr_define expected [id, rgb, term, info], got: %zu", ci->size);
			continue;
		}

		if (ci->ptr[1].type != MSGPACK_OBJECT_MAP){
			trace("hl_attr_define [rgb] not a map");
			continue;
		}

/* foreground or background? */
		const msgpack_object_map* cm = &ci->ptr[1].via.map;

		for (size_t j = 0; j < cm->size; j++){
			if (cm->ptr[j].key.type != MSGPACK_OBJECT_STR)
				continue;

			if (nvim_str_match(&cm->ptr[j].key.via.str, "foreground")){
				uint64_t val = cm->ptr[j].val.via.u64;
				update_cval(val, state->attr.fc);
				state->got_fg = true;
			}
			else if (nvim_str_match(&cm->ptr[j].key.via.str, "background")){
				uint64_t val = cm->ptr[j].val.via.u64;
				update_cval(val, state->attr.bc);
				state->got_bg = true;
			}
			else if (nvim_str_match(&cm->ptr[j].key.via.str, "reverse")){
				state->attr.aflags |= TUI_ATTR_INVERSE;
			}
			else if (nvim_str_match(&cm->ptr[j].key.via.str, "bold")){
				state->attr.aflags |= TUI_ATTR_BOLD;
			}
			else if (nvim_str_match(&cm->ptr[j].key.via.str, "underline")){
				state->attr.aflags |= TUI_ATTR_UNDERLINE;
			}
			else if (nvim_str_match(&cm->ptr[j].key.via.str, "italic")){
				state->attr.aflags |= TUI_ATTR_ITALIC;
			}
			else if (nvim_str_match(&cm->ptr[j].key.via.str, "strikethrough")){
				state->attr.aflags |= TUI_ATTR_STRIKETHROUGH;
			}
/* Special: can't be done atm, lacks a way to express it in TUI */
/* Undercurl: missing attribute in TUI, possible but we are out of bits */
/* Blend: could be done but so far used only for all backgrounds regardless */
			else {
			}
		}
	}

	return true;
}

static bool highlight_defcol(const msgpack_object_array* arg)
{
/*default colors: rgb_fg, rgb_bg, rgb_sp, cterm_fg, cterm_bg */
	if (arg->ptr[1].type != MSGPACK_OBJECT_ARRAY)
		return false;

	uint64_t fgc = arg->ptr[1].via.array.ptr[0].via.u64;
	uint64_t bgc = arg->ptr[1].via.array.ptr[1].via.u64;
	struct hl_state* state;
	uint64_t id = 0;

	HASH_FIND_INT(highlights, &id, state);
	if (!state){
		state = malloc(sizeof(struct hl_state));
		state->attr = arcan_tui_defattr(nvim.grids[0], NULL);
		state->id = 0;
		HASH_ADD_INT(highlights, id, state);
	}

	struct tui_screen_attr attr = {
	};
	update_cval(fgc, attr.fc);
	update_cval(bgc, attr.bc);
	update_cval(fgc, state->attr.fc);
	update_cval(bgc, state->attr.bc);

	for (size_t i = 0; i < COUNT_OF(nvim.grids); i++){
		if (!nvim.grids[i])
			continue;

		arcan_tui_set_color(nvim.grids[i], TUI_COL_PRIMARY, attr.fc);
		arcan_tui_set_bgcolor(nvim.grids[i], TUI_COL_PRIMARY, attr.bc);

		arcan_tui_set_color(nvim.grids[i], TUI_COL_TEXT, attr.fc);
		arcan_tui_set_bgcolor(nvim.grids[i], TUI_COL_TEXT, attr.bc);

		arcan_tui_set_bgcolor(nvim.grids[i], TUI_COL_BG, attr.bc);
		arcan_tui_set_color(nvim.grids[i], TUI_COL_BG, attr.bc);
		arcan_tui_defattr(nvim.grids[i], &attr);
	}

	return true;
}

static bool option_set(const msgpack_object_array* arg)
{
/* any options we really need? */
	return true;
}

static bool set_icon(const msgpack_object_array* arg)
{
/* IDENT doesn't really have an iconified summary
 * (except icons which isn't the same at all)  */
	return true;
}

static bool set_title(const msgpack_object_array* arg)
{
	const msgpack_object_array* gargs = &arg->ptr[1].via.array;
	if (gargs->size != 1 || gargs->ptr[0].type != MSGPACK_OBJECT_STR)
		return false;

	msgpack_object_str str = gargs[0].ptr[0].via.str;
	if (str.size == 0){
		arcan_tui_ident(nvim.grids[0], "");
	}
	else {
		char* buf = malloc(str.size + 1);
		if (!buf)
			return true;
		memcpy(buf, str.ptr, str.size);
		buf[str.size] = 0;
		arcan_tui_ident(nvim.grids[0], buf);
		free(buf);
	}

	return true;
}

static bool release_locks(const msgpack_object_array* arg)
{
/* this is a bit problematic in the sense that we may well get multiple redraw
 * calls on one frame, so the synch() is needed to align against the refresh,
 * but that lowers the responsiveness for resize etc. only real workaround for
 * that is to have an intermediate buffer for the grid - skipping that for the
 * time being */
	if (!nvim.lock_level)
		return false;

	pthread_mutex_unlock(&nvim.synch);
	if (nvim.lock_level == 2){
		pthread_mutex_unlock(&nvim.hold);
	}

	nvim.lock_level = 0;

	return true;
}

static const struct nvim_cmd redraw_cmds[] = {
	{"grid_resize", draw_resize},
	{"grid_line", draw_lines},
	{"grid_destroy", draw_destroy},
	{"grid_clear", grid_clear},
	{"grid_cursor_goto", grid_goto},
	{"hl_attr_define", highlight_attribute},
	{"default_colors_set", highlight_defcol},
	{"grid_scroll", grid_scroll},
	{"option_set", option_set},
	{"set_icon", set_icon},
	{"set_title", set_title},
	{"flush", release_locks}
/*
 * set_scroll_region [top, bottom, left, right]
 * hl_group_set
 * mode_info_set (cursor-shape, cursor-size
 * mode_change
 * flush [release mutex / allow refresh ]
 * mouse_on,
 * busy_start
 * busy_stop
 * bell / visual_bell -> alert
 */
};

static void nvim_redraw(const msgpack_object_array* arg)
{
	trace("redraw");
/* format should be an array of arrays where each inner array is
 * cmd -> arguments */
	for (size_t i = 0; i < arg->size; i++){
		if (arg->ptr[i].type != MSGPACK_OBJECT_ARRAY)
			continue;
		const msgpack_object_array* iarg = &arg->ptr[i].via.array;

		if (iarg->ptr[0].type != MSGPACK_OBJECT_STR){
			trace("bad arg");
			continue;
		}

		const msgpack_object_str* str = &iarg->ptr[0].via.str;
		bool found = false;
		for (size_t j = 0; j < COUNT_OF(redraw_cmds); j++){
			if (nvim_str_match(str, redraw_cmds[j].lbl)){
				found = true;

				if (!redraw_cmds[j].ptr(iarg)){
					trace("parsing failed on redraw:%.*s", str->size, str->ptr);
				}

				break;
			}
		}

		if (!found){
			trace("missing command: %.*s", str->size, str->ptr);
		}
	}
}

static void on_notification(msgpack_object_str* cmd, const msgpack_object_array* arg)
{
	if (nvim_str_match(cmd, "redraw")){

/* first try without the slowpath, if it can't be done, send the wakeup
 * command to break out of poll, and then the render thread will stick to
 * the hold- lock, process the command and then release all held locks */
		if (!nvim.lock_level){
			nvim.lock_level = 1;

			if (0 != pthread_mutex_trylock(&nvim.synch)){
				pthread_mutex_lock(&nvim.hold);
				nvim.lock_level = 2;
				char cmd = 'l';
				write(nvim.sigfd, &cmd, 1);
				pthread_mutex_lock(&nvim.synch);
			}
		}

		nvim_redraw(arg);
	}
/* win-close, win-hide : find grid, close it (unless primary) */
	else{
		trace("unhandled-notification: %s", cmd->ptr);
	}
}

static int mpack_to_nvim(void* data, const char* buf, unsigned long buf_out)
{
	FILE* fpek = data;
	fwrite(buf, buf_out, 1, fpek);
	fflush(fpek);
	return 0;
}

static void* thread_input(void* data)
{
	int* fdbuf = data;
	int fdin = fdbuf[0];

	static msgpack_unpacker unpack;
	msgpack_unpacker_init(&unpack, MSGPACK_UNPACKER_INIT_BUFFER_SIZE);

/* looking at the unpacked code, it seems to self- invoke destroy on
 * unpacker_next calls, afact this should then release the zones from
 * unpack */
	msgpack_unpacked result;
	msgpack_unpacked_init(&result);

	for(;;){
	/* make sure we can accomodate ~64k more, otherwise grow - since we are
	 * running in RPC like mode we don't really know how much data there is
	 * without parsing */
		ssize_t sz = msgpack_unpacker_buffer_capacity(&unpack);
		if (sz < 65536){
			if (!msgpack_unpacker_reserve_buffer(&unpack, 65536))
				break;
			sz = msgpack_unpacker_buffer_capacity(&unpack);
		}

		void* buffer = msgpack_unpacker_buffer(&unpack);
		ssize_t nr;
		if (-1 == (nr = read(fdin, buffer, sz))){
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}

/* pipe dead */
		if (0 == nr)
			break;

		msgpack_unpacker_buffer_consumed(&unpack, nr);

		int res;
		while (MSGPACK_UNPACK_SUCCESS ==
			(res = msgpack_unpacker_next(&unpack, &result))){
			const msgpack_object* const o = &result.data;
			const msgpack_object_array* const args = &(o->via.array);

			if (nvim.trace_out){
				msgpack_object_print(nvim.trace_out, *o);
				trace_obj_array(args);
				fputs("\n", nvim.trace_out);
				fflush(nvim.trace_out);
			}

 			if (args->size != 3 && args->size != 4){
				fprintf(nvim.trace_out, "invalid object size");
				continue;
			}
			switch(args->ptr[0].via.u64){
			case 0:
				trace("request");
			break;
			case 1:
				trace("response");
			break;
			case 2:
 			if (args->ptr[1].type == MSGPACK_OBJECT_STR &&
						args->ptr[2].type == MSGPACK_OBJECT_ARRAY){
					on_notification(&args->ptr[1].via.str, &args->ptr[2].via.array);
				}
				else
					fprintf(stderr, "unknown notification format\n");
			break;
			default:
				fprintf(stderr, "unknown identifier: %"PRIu64, args->ptr[0].via.u64);
			break;
			}
		}
	}

/* release the ui thread */
	close(fdin);
	char cmd = 'q';
	write(nvim.sigfd, &cmd, 1);

	return NULL;
}

static bool setup_nvim_process(int argc, char** argv, int* in, FILE** out)
{
/* pipe-pair and map to new process stdin/stdout, wrap around FILE abstractions
 * for use here - process input in one pipe, output in the other */
	int pipe_input[2];
	int pipe_output[2];

	if (-1 == pipe(pipe_output))
		return false;

	*in = pipe_output[0];

	if (-1 == pipe(pipe_input)){
		close(*in);
		close(pipe_output[1]);
		return false;
	}
	if (!(*out = fdopen(pipe_input[1], "w"))){
		close(*in);
		close(pipe_input[0]);
		close(pipe_input[1]);
		return false;
	}

	pid_t nvim_pid = fork();

	if (0 == nvim_pid){
		close(*in);
		fclose(*out);

		if (-1 == dup2(pipe_input[0], STDIN_FILENO) ||
			-1 == dup2(pipe_output[1], STDOUT_FILENO)){
			return EXIT_FAILURE;
		}

		char* out_argv[argc+4];
		size_t ofs = 0;

		out_argv[ofs++] = "nvim";
		out_argv[ofs++] = "--embed";

		for (size_t i = 0; i < argc; i++){
			out_argv[ofs++] = argv[i];
		}
		out_argv[ofs++] = NULL;

		execvp("nvim", out_argv);
		exit(EXIT_FAILURE);
	}

	close(pipe_input[0]);
	close(pipe_output[1]);

	if (-1 == nvim_pid){
		close(*in);
		fclose(*out);
		return false;
	}

	return true;
}

static void setup_nvim_ui()
{
	char msg[] = "nvim_ui_attach";
	nvim_request_str(msg, sizeof(msg)-1);
	msgpack_pack_array(nvim.out, 3);
	msgpack_pack_int64(nvim.out, 128);
	msgpack_pack_int64(nvim.out, 32);

	size_t n_opts = 2;
	if (nvim.multigrid)
		n_opts++;

	if (nvim.messages)
		n_opts++;

	if (nvim.popups)
		n_opts++;

	msgpack_pack_map(nvim.out, n_opts);

/* truecolor of course */
	{
	msgpack_pack_str(nvim.out, 3);
	msgpack_pack_str_body(nvim.out, "rgb", 3);
	msgpack_pack_true(nvim.out);
	}

/* more recent grid_line drawing method */
	{
	msgpack_pack_str(nvim.out, 12);
	msgpack_pack_str_body(nvim.out, "ext_linegrid", 12);
	msgpack_pack_true(nvim.out);
	}

/* we can deal with multiple grids, either composed or split */
	if (nvim.multigrid){
		msgpack_pack_str(nvim.out, 13);
		msgpack_pack_str_body(nvim.out, "ext_multigrid", 13);
		msgpack_pack_true(nvim.out);
	}

/*
 * ext_messages to avoid a grid being used for that,
 * enables msg_show events:
 * kind, content, replace_last - we can put those as alerts with shmif, but tui
 * does not have a way of exposing it currently.
 *
 * and msg_clear: (remove all)
 * and msg_showmode, msg_content, msg_ruler, msg_history_show
 */
	if (nvim.messages){
	msgpack_pack_str(nvim.out, 12);
	msgpack_pack_str_body(nvim.out, "ext_messages", 12);
	msgpack_pack_true(nvim.out);
	}

/* enables popupmenu_select (ind),
 * popupmenu_show (items, selected, row, col, grid)
 * if grid is -1 it is tied to the command-line and col is byte-pos
 * and popupmenu_hide */
	if (nvim.popups){
	msgpack_pack_str(nvim.out, 13);
	msgpack_pack_str_body(nvim.out, "ext_popupmenu", 13);
	msgpack_pack_true(nvim.out);
	}
}

static struct tui_cbcfg setup_nvim(int id)
{
	struct nvim_meta* nvim_grid = malloc(sizeof(struct nvim_meta));
	*nvim_grid = (struct nvim_meta){
		.grid_id = id
	};

	struct tui_cbcfg cbcfg = {
		.query_label = query_label,
		.input_label = on_label,
		.input_alabel = on_alabel,
		.input_mouse_motion = on_mouse,
		.input_mouse_button = on_mouse_button,
		.input_utf8 = on_u8,
		.input_key = on_key,
		.input_misc = on_misc,
		.state = on_state,
		.bchunk = on_bchunk,
		.vpaste = on_vpaste,
		.apaste = on_apaste,
		.tick = on_tick,
		.utf8 = on_utf8_paste,
		.resized = on_resize,
		.tag = nvim_grid
	};

	return cbcfg;
}

int main(int argc, char** argv)
{
	arcan_tui_conn* conn = arcan_tui_open_display("NeoVim", "");
	struct tui_cbcfg cbcfg = setup_nvim(1);
	nvim.grids[0] = arcan_tui_setup(conn, NULL, &cbcfg, sizeof(cbcfg));
	nvim.n_grids = 1;
	arcan_tui_set_flags(nvim.grids[0], TUI_MOUSE_FULL);

	if (!nvim.grids[0]){
		fprintf(stderr, "failed to setup TUI connection\n");
		return EXIT_FAILURE;
	}

	const char* tracefn = getenv("NVIM_ARCAN_TRACE");
	if (tracefn){
		if (strcmp(tracefn, "-") == 0)
			nvim.trace_out = stderr;
		else
			nvim.trace_out = fopen(tracefn, "w");
	}

	FILE* data_out;
	size_t argv_pos = 1;
	while (argc > argv_pos){
		if (strcmp("--multigrid", argv[argv_pos]) == 0){
			nvim.multigrid = true;
		}
		else if (strcmp("--popup", argv[argv_pos]) == 0){
			nvim.popups = true;
		}
		else if (strcmp("--messages", argv[argv_pos]) == 0){
			nvim.messages = true;
		}
/* forward to nvim at first unknown position */
		else
			break;

		argv_pos++;
	}

	int data_in[2] = {-1};

	if (!setup_nvim_process(argc-1, &argv[argv_pos], &data_in[0], &data_out)){
		arcan_tui_destroy(nvim.grids[0], "couldn't spawn neovim");
		return EXIT_FAILURE;
	}

	int pipes[2];
	if (-1 == pipe(pipes)){
		arcan_tui_destroy(nvim.grids[0], "signal pipe allocation failure");
		return EXIT_FAILURE;
	}
	nvim.sigfd = pipes[1];
	int signalfd = pipes[0];

	nvim.out = msgpack_packer_new(data_out, mpack_to_nvim);

	setup_nvim_ui();

/* create our input parsing thread */
	pthread_t pth;
	pthread_attr_t pthattr;
	pthread_attr_init(&pthattr);
	pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);

	if (-1 == pthread_create(&pth, &pthattr, thread_input, data_in)){
		arcan_tui_destroy(nvim.grids[0], "input thread creation failed");
		return EXIT_FAILURE;
	}

	while (1){
		pthread_mutex_lock(&nvim.synch);
		struct tui_process_res res =
			arcan_tui_process(nvim.grids, nvim.n_grids, &signalfd, 1, -1);

/* sweep the result bitmap and synch the grids that have changed */
		if (res.errc == TUI_ERRC_OK){
			if (-1 == arcan_tui_refresh(nvim.grids[0]) && errno == EINVAL)
				break;
		}
		else
			break;

		pthread_mutex_unlock(&nvim.synch);

		if (res.ok){
			char cmd;
			if (read(signalfd, &cmd, 1) == 1){
				if (cmd == 'q')
					break;
				else if (cmd == 'l'){
					pthread_mutex_lock(&nvim.hold);
					trace("synch");
					pthread_mutex_unlock(&nvim.hold);
				}
			}
		}
	}

	for (size_t i = 0; i < nvim.n_grids; i++){
		if (!nvim.grids[i])
			continue;

		arcan_tui_destroy(nvim.grids[i], NULL);
	}

	return EXIT_SUCCESS;
}
