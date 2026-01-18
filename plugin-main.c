/* simple_frame_logger_mt_macos.c — macOS pthread+dispatch 版 */

#include <stdlib.h>
#if defined(__APPLE__)
#  include <libkern/OSByteOrder.h>
#  define htole32(x) OSSwapHostToLittleInt32(x)
#elif defined(__linux__)
#  include <endian.h>
#endif

#include <obs-module.h>
#include <util/platform.h>
#include <graphics/graphics.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <errno.h>     /* errno 全局变量 */
#include <string.h>    /* strerror() 函数 */
#include <signal.h>          /* ★ 忽略 SIGPIPE */

#include <sys/socket.h>      /* ★ socket */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>          /* ★ TCP_NODELAY 等宏在这里 */

#define DEFAULT_LOG_INTERVAL_MS 2000
#define MSEC_TO_NSEC            1000000ULL
#define SETTING_LOG_INTERVAL_MS "log_interval_ms"
#define TEXT_LOG_INTERVAL_MS    "Log Interval (ms)"
#define FRAMES_DIR              "frames"

#define TCP_PORT 27183         /* ★ 127.0.0.1:27183 <=> 手机端 */


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("my-frame-logger", "en-US")


/* ─────── 全局 socket 句柄 ────────────────────────────────── */
static int sock_fd = -1;       /* ★ -1 表示未连接 */

/* ─────── 打开 / 关闭 / 循环写 ────────────────────────────── */
static bool sock_connect(void)                        /* ★ 连接一次 */
{
	if (sock_fd != -1) return true;

	sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		blog(LOG_ERROR, "socket() fail1 (%s)", strerror(errno));
		return false;
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port   = htons(TCP_PORT);
	inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

	if (connect(sock_fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
		blog(LOG_ERROR, "connect() fail2 (%s)", strerror(errno));
		close(sock_fd); sock_fd = -1;
		return false;
	}
	/* ── ★ 3. 调大发送缓冲 & 关 Nagle ────────────────────────── */
    int snd_sz = 4 * 1024 * 1024;                             /* 4 MB */
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF,
               &snd_sz, sizeof snd_sz);

    int one = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY,
               &one, sizeof one);

    /* 读取实际生效值（内核可能取不到 4 MB 时会调小） */
    socklen_t optlen = sizeof snd_sz;
    getsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF,
               &snd_sz, &optlen);

    blog(LOG_INFO, "socket connected  (sndbuf=%d bytes)", snd_sz);
    return true;
}

static void sock_close(void)                          /* ★ 断开 */
{
	if (sock_fd != -1) {
		close(sock_fd);
		sock_fd = -1;
		blog(LOG_INFO, "socket closed");
	}
}

static bool write_full_sock(const uint8_t *p, size_t len)   /* ★ 循环 send */
{
	while (len) {
		ssize_t n = send(sock_fd, p, len, 0);
		if (n <= 0) {
			blog(LOG_ERROR, "send fail (%s)", strerror(errno));
			return false;
		}
		p += n; len -= n;
	}
	return true;
}

/* ─────── 写一帧：4B 长度 + NV21 数据 ─────────────────────── */
static void tcp_stream_write(const uint8_t *buf, uint32_t size)   /* ★ */
{
	if (!sock_connect()) return;

	uint32_t le = htole32(size);
	if (!write_full_sock((const uint8_t*)&le, 4) ||
	    !write_full_sock(buf, size))
	{
		blog(LOG_ERROR, "socket broken, will reconnect next frame");
		sock_close();                           /* 下帧重连 */
	}
	else{
		blog(LOG_INFO, "frame sent");
	}
}

/* ---------- 工具：毫秒级时间戳 ---------- */
static inline uint64_t epoch_ms(void)
{
	struct timeval tv; gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

/* ---------- Job 定义 ---------- */
struct frame_job {
	uint8_t *pixels;
	uint32_t w, h, pitch;
	uint64_t ts_ms;
	struct frame_job *next;
};

/* ---------- 插件实例 ---------- */
struct logger {
	/* 原有 */
	obs_source_t   *ctx;
	uint64_t        last_ns, interval_ns;
	gs_texrender_t *tex;
	gs_stagesurf_t *stage;
	char           *dir;

	/* 多线程 */
	pthread_t        worker;
	dispatch_semaphore_t sem;
	pthread_mutex_t  mtx;
	struct frame_job *head, *tail;
	bool             stop;

	/* ★ 统计用 */
    uint64_t         enq_cnt;     /* 已入队帧序号（累积） */
    uint32_t         qlen;        /* 当前队列长度        */
};

/* ---------- 前向 ---------- */
static void *worker_fn(void *arg);
static void write_job_nv21(struct frame_job *j, const char *dir);

/* ---------- mkdir ---------- */
static void ensure_dir(const char *p) { os_mkdirs(p); }

/* ---------- create ---------- */
static void *logger_create(obs_data_t *s, obs_source_t *ctx)
{
	signal(SIGPIPE, SIG_IGN);            /* ★ 防止 send 触发崩溃 */
	struct logger *lg = bzalloc(sizeof(*lg));
	lg->ctx = ctx;
	lg->enq_cnt = 0;                 /* ★ */
	lg->qlen    = 0;                 /* ★ */
	/* 路径 */
	const char *base = obs_get_module_data_path(obs_current_module());
	size_t len = strlen(base)+strlen(FRAMES_DIR)+2;
	lg->dir = bzalloc(len);
	snprintf(lg->dir, len, "%s/%s", base, FRAMES_DIR);
	ensure_dir(lg->dir);

	/* 纹理 */
	lg->tex   = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	/* 间隔 */
	uint64_t iv = obs_data_get_int(s, SETTING_LOG_INTERVAL_MS);
	if (!iv) iv = DEFAULT_LOG_INTERVAL_MS;
	lg->interval_ns = iv * MSEC_TO_NSEC;

	/* 队列同步 */
	pthread_mutex_init(&lg->mtx, NULL);
	lg->sem  = dispatch_semaphore_create(0);
	lg->head = lg->tail = NULL;
	lg->stop = false;
	pthread_create(&lg->worker, NULL, worker_fn, lg);

	return lg;
}

/* ---------- destroy ---------- */
static void logger_destroy(void *data)
{
	struct logger *lg = data;
	/* 通知线程退出 */
	lg->stop = true;
	dispatch_semaphore_signal(lg->sem);
	pthread_join(lg->worker, NULL);

	/* 清队列 */
	pthread_mutex_lock(&lg->mtx);
	while (lg->head) {
		struct frame_job *n = lg->head->next;
		bfree(lg->head->pixels); bfree(lg->head); lg->head = n;
	}
	pthread_mutex_unlock(&lg->mtx);

	dispatch_release(lg->sem);
	pthread_mutex_destroy(&lg->mtx);

	if (lg->stage) gs_stagesurface_destroy(lg->stage);
	if (lg->tex)   gs_texrender_destroy(lg->tex);
    sock_close();                        /* ★ */
	bfree(lg->dir); bfree(lg);
}

/* ---------- update ---------- */
static void logger_update(void *d, obs_data_t *s)
{
	struct logger *lg = d;
	uint64_t iv = obs_data_get_int(s, SETTING_LOG_INTERVAL_MS);
	if (!iv) iv = DEFAULT_LOG_INTERVAL_MS;
	lg->interval_ns = iv * MSEC_TO_NSEC;
}

/* ---------- props/defaults ---------- */
static obs_properties_t *logger_props(void *d)
{
	UNUSED_PARAMETER(d);
	obs_properties_t *ps = obs_properties_create();
	obs_property_t *p = obs_properties_add_int(ps,
	    SETTING_LOG_INTERVAL_MS, TEXT_LOG_INTERVAL_MS, 1, 10000, 1);
	obs_property_int_set_suffix(p, " ms");
	return ps;
}
static void logger_defaults(obs_data_t *s)
{ obs_data_set_default_int(s, SETTING_LOG_INTERVAL_MS, DEFAULT_LOG_INTERVAL_MS); }

/* ---------- render ---------- */
static void logger_render(void *data, gs_effect_t *eff)
{
	struct logger *lg = data;
	uint64_t now = obs_get_video_frame_time();
	UNUSED_PARAMETER(eff);

	if (now - lg->last_ns < lg->interval_ns) {
		obs_source_skip_video_filter(lg->ctx); return;
	}
	lg->last_ns = now;

	uint32_t w = obs_source_get_width(lg->ctx);
	uint32_t h = obs_source_get_height(lg->ctx);
	if (!w||!h) { obs_source_skip_video_filter(lg->ctx); return; }

	/* stage surface */
	if (!lg->stage ||
	    gs_stagesurface_get_width(lg->stage)!=w ||
	    gs_stagesurface_get_height(lg->stage)!=h) {
		if (lg->stage) gs_stagesurface_destroy(lg->stage);
		lg->stage = gs_stagesurface_create(w,h,GS_RGBA);
	}
	if (!lg->tex) lg->tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	gs_texrender_reset(lg->tex);
	if (!gs_texrender_begin(lg->tex, w,h)) {
		obs_source_skip_video_filter(lg->ctx); return;
	}
	struct vec4 clr; vec4_zero(&clr);
	gs_clear(GS_CLEAR_COLOR,&clr,0.f,0);
	gs_ortho(0.f,(float)w,0.f,(float)h,-100.f,100.f);
	obs_source_t *tgt = obs_filter_get_target(lg->ctx);
	if (tgt) obs_source_video_render(tgt);
	gs_texrender_end(lg->tex);

	gs_texture_t *tex = gs_texrender_get_texture(lg->tex);
	if (!tex) { obs_source_skip_video_filter(lg->ctx); return; }

	gs_stage_texture(lg->stage, tex);
	uint8_t *src; uint32_t pitch;
	if (!gs_stagesurface_map(lg->stage,&src,&pitch)) {
		obs_source_skip_video_filter(lg->ctx); return;
	}

	size_t sz = (size_t)pitch*h;
	uint8_t *dup = bmalloc(sz); memcpy(dup, src, sz);
	gs_stagesurface_unmap(lg->stage);

	/* enqueue */
	struct frame_job *j = bmalloc(sizeof(*j));
	j->pixels=dup; j->w=w; j->h=h; j->pitch=pitch; j->ts_ms=epoch_ms(); j->next=NULL;

	pthread_mutex_lock(&lg->mtx);
	if (lg->tail) lg->tail->next=j; else lg->head=j;
	lg->tail=j;
	pthread_mutex_unlock(&lg->mtx);
	dispatch_semaphore_signal(lg->sem);
	
	/* ★ 计数 + 日志 */
	lg->enq_cnt++;
	lg->qlen++;
	blog(LOG_INFO, "frame enqueued #%llu  (queue=%u)",
     (unsigned long long)lg->enq_cnt, lg->qlen);

	obs_source_skip_video_filter(lg->ctx);
}

/* ---------- worker ---------- */
static void *worker_fn(void *arg)
{
	struct logger *lg = arg;
	while (1) {
		dispatch_semaphore_wait(lg->sem, DISPATCH_TIME_FOREVER);
		if (lg->stop) break;

		pthread_mutex_lock(&lg->mtx);
		struct frame_job *j = lg->head;
		if (j) { lg->head = j->next; if (!lg->head) lg->tail=NULL; }
		pthread_mutex_unlock(&lg->mtx);
		if (!j) continue;

        write_job_nv21(j, lg->dir);

		bfree(j->pixels); bfree(j);
	}
	return NULL;
}

/* ---------- 写文件 ---------- */

static void rgba_to_nv21(uint8_t *rgba, int width, int height, int pitch, uint8_t *y_plane, uint8_t *vu_plane)
{
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			uint8_t *px = rgba + y * pitch + x * 4;
			uint8_t R = px[0], G = px[1], B = px[2];

			// BT.601 conversion
			uint8_t Y = (uint8_t)(( 66 * R + 129 * G +  25 * B + 128) >> 8) + 16;
			uint8_t U = (uint8_t)((-38 * R -  74 * G + 112 * B + 128) >> 8) + 128;
			uint8_t V = (uint8_t)((112 * R -  94 * G -  18 * B + 128) >> 8) + 128;

			// 写入Y
			y_plane[y * width + x] = Y;

			// 每2x2像素写入一个VU对（NV21: interleaved VU）
			if ((y % 2 == 0) && (x % 2 == 0)) {
				int vu_index = (y / 2) * width + x;
				vu_plane[vu_index + 0] = V;
				vu_plane[vu_index + 1] = U;
			}
		}
	}
}

static void write_job_nv21(struct frame_job *j, const char *unused_dir)
{
	UNUSED_PARAMETER(unused_dir);  /* dir 不再需要 */

	uint32_t width  = j->w;
	uint32_t height = j->h;
	uint8_t *argb   = j->pixels;

	size_t y_size = width * height;
	size_t vu_size = y_size / 2;
	size_t total   = y_size + vu_size;

	uint8_t *nv21 = (uint8_t *)malloc(total);
	if (!nv21) { blog(LOG_ERROR, "malloc nv21 fail"); return; }

	uint8_t *y_plane  = nv21;
	uint8_t *vu_plane = nv21 + y_size;
	rgba_to_nv21(argb, width, height, j->pitch, y_plane, vu_plane);

	tcp_stream_write(nv21, (uint32_t)total);   /* ★ */

	free(nv21);
}


/* ---------- Source 定义 ---------- */
static const char *name_cb(void *d){ UNUSED_PARAMETER(d); return "Frame Logger (MT)"; }
const char *obs_module_name(void){ return "SimpleFrameLoggerMT"; }

static struct obs_source_info info ={
	.id="simple_frame_logger_mt",
	.type=OBS_SOURCE_TYPE_FILTER,
	.output_flags=OBS_SOURCE_VIDEO,
	.get_name=name_cb,
	.create=logger_create,
	.destroy=logger_destroy,
	.update=logger_update,
	.get_properties=logger_props,
	.get_defaults=logger_defaults,
	.video_render=logger_render,
};

bool obs_module_load(void)
{
	obs_register_source(&info);
	blog(LOG_INFO,"[SimpleFrameLoggerMT] loaded");
	return true;
}
