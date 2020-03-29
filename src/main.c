/*
 * TUI based UI frontend for NeoVIM
 *  - 1. highlight
 *  - 2. special input
 *  - 3. scroll region
 *  - 4. 'q' requires step before dying
 *
 * - options to explore:
 *   - msgpack writer to debug window
 *   - split to new window
 */
#include <arcan_shmif.h>
#include <arcan_tui.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <msgpack.h>
#include <pthread.h>
#include <errno.h>

#ifndef COUNT_OF
#define COUNT_OF(x) \
	((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
#endif

static struct {
/* mutex around packer out */
	msgpack_packer* out;
	uint32_t reqid;
	struct tui_context* grids[32];
	size_t n_grids;
} nvim;

struct nvim_meta {
	int grid_id;
};

#define TRACE_ENABLE
static inline void trace(const char* msg, ...)
{
#ifdef TRACE_ENABLE
	va_list args;
	va_start( args, msg );
		vfprintf(stderr,  msg, args );
	va_end( args);
	fprintf(stderr, "\n");
#endif
}

static void trace_obj_array(const msgpack_object_array* arg)
{
#ifdef TRACE_ENABLE
	struct msgpack_object obj = {
		.type = MSGPACK_OBJECT_ARRAY,
		.via.array = *arg
	};
	msgpack_object_print(stderr, obj);
	fprintf(stderr, "\n");
#endif
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

static bool on_label(struct tui_context* c, const char* label, bool act, void* t)
{
	trace("label(%s)", label);
	return true;
}

static bool on_alabel(struct tui_context* c, const char* label,
		const int16_t* smpls, size_t n, bool rel, uint8_t datatype, void* t)
{
	trace("a-label(%s)", label);
	return false;
}

static void on_mouse(struct tui_context* c,
	bool relative, int x, int y, int modifiers, void* t)
{
	trace("mouse(%d:%d, mods:%d, rel: %d", x, y, modifiers, (int) relative);
/* string:
 * nvim_input_mouse(button:left,right,middle,wheel, action:press,drag,ui,down,left,right,
 * modifier:CA - grid:number, row:number, col: number)
 * <%s%s>,<%u,%u>
 */
}

static void on_key(struct tui_context* c, uint32_t xkeysym,
	uint8_t scancode, uint8_t mods, uint16_t subid, void* t)
{
	trace("unknown_key(%"PRIu32",%"PRIu8",%"PRIu16")", xkeysym, scancode, subid);
/*
 * these need to be translated based on sym and mods
 */
}

static bool on_u8(struct tui_context* c, const char* u8, size_t len, void* t)
{
	uint8_t buf[5] = {0};
	memcpy(buf, u8, len >= 5 ? 4 : len);
	trace("on_u8(%zu:%s)", len, buf);

	const char cmd[] = "nvim_input";
	nvim_request_str(cmd, sizeof(cmd) - 1);
	msgpack_pack_array(nvim.out, 1);
	msgpack_pack_str(nvim.out, len);
	msgpack_pack_str_body(nvim.out, u8, len);

/* nvim_put: lines[], tyoe[block, char, line], [after], [follow] */
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
	arcan_tui_move_to(grid, offset, row);

/* format depends on individual line size:
 * 1 item  : [ch]
 * 2 items : [ch, hlid]
 * 3 items : [ch, hlid, repeat]
 *
 * if hlid is not set, grab the last defined one - global */
	for (size_t i = 0; i < line->size; i++){
		if (line->ptr[i].type != MSGPACK_OBJECT_ARRAY)
			return false;
		const msgpack_object_array* cell = &line->ptr[i].via.array;

		if (cell->ptr[0].type != MSGPACK_OBJECT_STR)
			return false;

		size_t count = 1;
		if (cell->size == 3)
			count = cell->ptr[2].via.u64;

		for (size_t i = 0; i < count; i++)
			arcan_tui_writeu8(grid,
				(uint8_t*) cell->ptr[0].via.str.ptr,
				cell->ptr[0].via.str.size, NULL
			);
	}
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

static bool grid_goto(const msgpack_object_array* arg)
{
	struct tui_context* grid = nvim_grid_to_tui(arg);
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

	arcan_tui_move_to(grid, col, row);

	return true;
}

static const struct nvim_cmd redraw_cmds[] = {
	{"grid_resize", draw_resize},
	{"grid_line", draw_lines},
	{"grid_destroy", draw_destroy},
	{"grid_clear", grid_clear},
	{"grid_cursor_goto", grid_goto},
/* option_set
 * default_colors_set
 * hl_attr_define
 * hl_group_set
 * grid_clear
 * set_icon (minimized title)
 * set_titl (normal title)
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

		if (iarg->ptr[0].type != MSGPACK_OBJECT_STR)
			continue;

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
	int fdin = *(int*)data;

	static msgpack_unpacker unpack;
	msgpack_unpacker_init(&unpack, MSGPACK_UNPACKER_INIT_BUFFER_SIZE);

/* doubt the buffering here is correct in the case where we don't get
 * a complete request, buffer_capacity increase */
	for(;;){
		ssize_t sz = msgpack_unpacker_buffer_capacity(&unpack);
		void* buffer = msgpack_unpacker_buffer(&unpack);

		ssize_t nr;
		if (-1 == (nr = read(fdin, buffer, sz))){
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}

		msgpack_unpacker_buffer_consumed(&unpack, nr);

/* read as much as possible, see if we run into a continue */
		msgpack_unpacked result;
		msgpack_unpacked_init(&result);

		int res;
		while ((res = msgpack_unpacker_next(&unpack, &result))){
			const msgpack_object* const o = &result.data;
			const msgpack_object_array* const args = &(o->via.array);
			if (args->size != 3 && args->size != 4){
				fprintf(stderr, "invalid object size");
				continue;
			}
			switch(args->ptr[0].via.u64){
			case 0:
				fprintf(stderr, "request\n");
			break;
			case 1:
				fprintf(stderr, "response\n");
/* pair against pending requests from our side, see if some handler is registered,
 * careful with UAFs against de-allocated segments */
			break;
			case 2:
/*			msgpack_object_print(stdout, *o);
				fflush(stdout); */
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

		msgpack_unpacked_destroy(&result);
	}

	close(fdin);
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

	msgpack_pack_map(nvim.out, 2);

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

/* we can deal with multiple grids (one mapped to a segment)
 *
	{
	msgpack_pack_str(nvim.out, 13);
	msgpack_pack_str_body(nvim.out, "ext_multigrid", 13);
	msgpack_pack_true(nvim.out);
	}
 */

/*
 * ext_messages to avoid a grid being used for that
 *
 */

/* ext_popupmenu? ext_tabline? */
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
	arcan_tui_conn* conn = arcan_tui_open_display("tui-test", "");
	struct tui_cbcfg cbcfg = setup_nvim(1);
	nvim.grids[0] = arcan_tui_setup(conn, NULL, &cbcfg, sizeof(cbcfg));
	nvim.n_grids = 1;

	if (!nvim.grids[0]){
		fprintf(stderr, "failed to setup TUI connection\n");
		return EXIT_FAILURE;
	}

	FILE* data_out;
	int data_in;
	if (!setup_nvim_process(argc-1, &argv[1], &data_in, &data_out)){
		arcan_tui_destroy(nvim.grids[0], "couldn't spawn neovim");
		return EXIT_FAILURE;
	}

	nvim.out = msgpack_packer_new(data_out, mpack_to_nvim);

	setup_nvim_ui();

/* create our input parsing thread */
	pthread_t pth;
	pthread_attr_t pthattr;
	pthread_attr_init(&pthattr);
	pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);

	if (-1 == pthread_create(&pth, &pthattr, thread_input, &data_in)){
		arcan_tui_destroy(nvim.grids[0], "input thread creation failed");
		return EXIT_FAILURE;
	}

	while (1){
		struct tui_process_res res =
			arcan_tui_process(nvim.grids, nvim.n_grids, NULL, 0, -1);

/* sweep the result bitmap and synch the grids that have changed */
		if (res.errc == TUI_ERRC_OK){
			if (-1 == arcan_tui_refresh(nvim.grids[0]) && errno == EINVAL)
				break;
		}
		else
			break;
	}

	for (size_t i = 0; i < nvim.n_grids; i++){
		if (!nvim.grids[i])
			continue;

		arcan_tui_destroy(nvim.grids[i], NULL);
	}

	return EXIT_SUCCESS;
}
