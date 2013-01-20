/* Arcan-fe, scriptable front-end engine
 *
 * Arcan-fe is the legal property of its developers, please refer
 * to the COPYRIGHT file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#ifndef _HAVE_ARCAN_FRAMESERVER
#define _HAVE_ARCAN_FRAMESERVER

#define ARCAN_FRAMESERVER_VCACHE_LIMIT 8 
#define ARCAN_FRAMESERVER_ACACHE_LIMIT 24
#define ARCAN_FRAMESERVER_DEFAULT_VTHRESH_SKIP 60
#define ARCAN_FRAMESERVER_ABUFFER_SIZE 8 * 1024 
#define ARCAN_FRAMESERVER_IGNORE_SKIP_THRESH 450
#define ARCAN_FRAMESERVER_PRESILENCE 16024 
#define VID_FD 3
#define AUD_FD 4
#define CTRL_FD 5

struct arcan_ffmpeg_context;

enum arcan_playstate {
	ARCAN_PASSIVE = 0,
	ARCAN_PLAYING = 1,
	ARCAN_PAUSED = 2,
	ARCAN_FINISHED = 3,
	ARCAN_SUSPENDED = 4
};

enum arcan_frameserver_kinds {
	ARCAN_FRAMESERVER_INPUT,
	ARCAN_FRAMESERVER_OUTPUT,
	ARCAN_FRAMESERVER_INTERACTIVE,
	ARCAN_FRAMESERVER_NETCL,
	ARCAN_FRAMESERVER_NETSRV,
	ARCAN_HIJACKLIB
};

/* FIXME: the interfaces to all the frameserver callbacks should really
 * be harmonized to pass this struct rather than the current win32ish arghell */
typedef struct {
	/* video */
	uint16_t width;
	uint16_t height;
	uint16_t bpp;
	uint8_t sformat;
	uint8_t dformat;
	uint16_t vskipthresh;

	/* audio */
	unsigned samplerate;
	uint8_t channels;
	uint8_t format;
	uint16_t vfthresh;

/* transfer */
	bool pbo_transfer;
	unsigned pbo_index;
	unsigned upload_pbo[2];
	
} arcan_frameserver_meta;

typedef struct arcan_frameserver {
/* video / audio properties used */
	arcan_frameserver_meta desc;
	frame_queue vfq, afq;
	struct arcan_evctx inqueue, outqueue;
	
/* original resource, needed for reloading */
	char* source;
	
/*  OS- specific, defined in general.h */
	shm_handle shm;
	sem_handle vsync, async, esync; 
	file_handle sockout_fd;
	
	arcan_aobj_id aid;
/* for monitoring hooks, NULL term. */
	arcan_aobj_id* alocks;
	arcan_vobj_id vid;

/* used for playing, pausing etc. */
	enum arcan_playstate playstate;
	int64_t lastpts;
	int64_t starttime;
	int64_t launchedtime;
	bool loop, autoplay, nopts;

	enum arcan_frameserver_kinds kind;
	
/* timing, only relevant if nopts == false */
	uint32_t vfcount;
	double bpms;
	double audioclock;

	process_handle child;
	long long childp;

	bool child_alive;

/* not all scenarios dictate a use of PBOs, as the flip-flop approach use
 * introduces a possible 1-frame latency from upload to display, which requires
 * an active frameserver for the last frame to be visible */
	bool use_pbo;

/* precalc offsets into mapped shmpage, calculated at resize */
	uint8_t* vidp, (* audp);

/* temporary buffer for aligning queue/dequeue events in audio */
	size_t sz_audb;
	off_t ofs_audb;
	uint8_t* audb;
	SDL_mutex* lock_audb;

/* usual hack, similar to load_asynchimage */
	intptr_t tag;
	uint16_t watch_const; 
} arcan_frameserver;

/* contains both structures for managing movie- playback,
 * both video and audio support functions */

struct frameserver_envp {
	bool use_builtin;
	bool custom_feed;
	
	union {

		struct {
			const char* const resource;
			const char* const mode; /* movie, libretro, record, net-cl, net-srv */
		} builtin;

		struct {
			char* fname; /* program to execute */
			char** argv; /* references to ARCAN_SHMKEY, ARCAN_SHMSIZE will be replaced, NULL terminated */
			char** envv; /* key with ARCAN_SHMKEY, ARCAN_SHMSIZE will have its value replaced, key=val, NULL terminated */
		} external;
		
	} args;
};

/* this will either launch the configured frameserver (use_builtin),
 * or act as a more generic execv of a program that supposedly implements the
 * same shmpage interface and protocol. */
arcan_errc arcan_frameserver_spawn_server(arcan_frameserver* dst, struct frameserver_envp);

/* allocate heap memory, reset all members to an empty state and then enforce defaults */
arcan_frameserver* arcan_frameserver_alloc();

/* enable the forked process to start decoding */
arcan_errc arcan_frameserver_playback(arcan_frameserver*);
arcan_errc arcan_frameserver_pause(arcan_frameserver*, bool syssusp);
arcan_errc arcan_frameserver_resume(arcan_frameserver*);

/* frameserver_pushfd send the file_handle into the process controlled by the specified frameserver
 * and emits a corresponding event into the eventqueue of the frameserver. 
 * returns !ARCAN_OK if the socket isn't connected, wrong type, OS can't handle transfer or the FD can't be transferred (e.g. stdin) 
 * fd will always be closed in this function. */ 
arcan_errc arcan_frameserver_pushfd(arcan_frameserver*, int fd);

/* take the argument event and add it to the event queue of the target, returns a failure if the event queue 
 * in the child is full */
arcan_errc arcan_frameserver_pushevent(arcan_frameserver*, arcan_event*);

/* poll the frameserver out eventqueue and push it unto the evctx, filter events that 
 * are outside the accepted category / kind */ 
void arcan_frameserver_pollevent(arcan_frameserver*, arcan_evctx*);

/* symbol should only be used by the backend to reach OS specific implementations (_unix.c / win32 )*/
void arcan_frameserver_dropsemaphores(arcan_frameserver*);
void arcan_frameserver_dropsemaphores_keyed(char*);

/* check if the frameserver is still alive, that the shared memory page is intact and look for any state-changes,
 * e.g. resize (which would require a recalculation of shared memory layout */
void arcan_frameserver_tick_control(arcan_frameserver*);

/* override the default queue opts (may be necessary for some frame-server sources */
void arcan_frameserver_queueopts_override(unsigned short vcellcount, unsigned short abufsize, unsigned short acellcount, unsigned short presilence);
void arcan_frameserver_queueopts(unsigned short* vcellcount, unsigned short* acellcount, unsigned short* abufsize, unsigned short* presilence);

/* default implementations for shared memory framequeue readers, two with separate sync (vidcb audcb) and one where
 * sync is locked to vid only but transfer audio frames into intermediate buffer as well */
ssize_t arcan_frameserver_shmvidcb(int fd, void* dst, size_t ntr);
ssize_t arcan_frameserver_shmaudcb(int fd, void* dst, size_t ntr);
ssize_t arcan_frameserver_shmvidaudcb(int fd, void* dst, size_t ntr);

/* used for streaming data to the frameserver, audio / video interleaved in one synch */
int8_t arcan_frameserver_avfeedframe(enum arcan_ffunc_cmd cmd, uint8_t* buf, uint32_t s_buf, uint16_t width, uint16_t height, uint8_t bpp, unsigned int mode, vfunc_state state);

/* used as monitor hook for frameserver audio feeds */
void arcan_frameserver_avfeedmon(arcan_aobj_id src, uint8_t* buf, size_t buf_sz, unsigned channels, unsigned frequency, void* tag);

/* return a callback function for retrieving appropriate video-feeds */
int8_t arcan_frameserver_videoframe(enum arcan_ffunc_cmd cmd, uint8_t* buf, uint32_t s_buf, uint16_t width, uint16_t height, uint8_t bpp, unsigned int mode, vfunc_state state);
int8_t arcan_frameserver_emptyframe(enum arcan_ffunc_cmd cmd, uint8_t* buf, uint32_t s_buf, uint16_t width, uint16_t height, uint8_t bpp, unsigned int mode, vfunc_state state); 

/* return a callback function for retrieving appropriate audio-feeds */
arcan_errc arcan_frameserver_audioframe(struct arcan_aobj* aobj, arcan_aobj_id id, unsigned buffer, void* tag);

/* simplified versions of the above that ignores PTS/DTS and doesn't use the framequeue */
int8_t arcan_frameserver_videoframe_direct(enum arcan_ffunc_cmd cmd, uint8_t* buf, uint32_t s_buf, uint16_t width, uint16_t height, uint8_t bpp, unsigned int mode, vfunc_state state);
arcan_errc arcan_frameserver_audioframe_direct(struct arcan_aobj* aobj, arcan_aobj_id id, unsigned buffer, void* tag);

/* PROCEED WITH EXTREME CAUTION, _configure, _spawn, _free etc. are among the more complicated functions
 * in the entire project, any changes should be thoroughly tested for regressions.
 * 
 * part of the spawn_server that is shared between the unix and the win32 implementations,
 * assumes that shared memory, semaphores etc. are already in place. */
void arcan_frameserver_configure(arcan_frameserver* ctx, struct frameserver_envp setup);

/* stop playback and free resources associated with a movie,
 * in some cases, this will clear the container. As the frameservers are supposed to be used in conjunction
 * with their VID reference, refrain from any other manual memory management or aliasing */
arcan_errc arcan_frameserver_free(arcan_frameserver*, bool loop);
#endif
