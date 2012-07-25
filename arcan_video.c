/* Arcan-fe, scriptable front-end engine
 *
 * Arcan-fe is the legal property of its developers, please refer
 * to the COPYRIGHT file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
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

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <signal.h>
#include <stddef.h>
#include <math.h>
#include <assert.h>

#define GLEW_STATIC
#define NO_SDL_GLEXT
#include <glew.h>

#define GL_GLEXT_PROTOTYPES 1
#define CLAMP(x, l, h) (((x) > (h)) ? (h) : (((x) < (l)) ? (l) : (x)))

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_opengl.h>
#include <SDL_byteorder.h>
#include <SDL_ttf.h>

#include "arcan_math.h"
#include "arcan_general.h"
#include "arcan_video.h"
#include "arcan_audio.h"
#include "arcan_event.h"
#include "arcan_framequeue.h"
#include "arcan_frameserver_backend.h"
#include "arcan_target_const.h"
#include "arcan_target_launcher.h"
#include "arcan_shdrmgmt.h"
#include "arcan_videoint.h"

#ifndef ARCAN_FONT_CACHE_LIMIT
#define ARCAN_FONT_CACHE_LIMIT 8
#endif

#ifndef RENDERTARGET_LIMIT
#define RENDERTARGET_LIMIT 4
#endif

struct text_format {
	TTF_Font* font;
	SDL_Color col;
	uint8_t alpha;
	uint8_t tab;
	uint8_t newline;
	bool cr;
	int style;
	char* endofs;
};

struct font_entry {
	TTF_Font* data;
	char* identifier;
	uint8_t size;
	uint8_t usecount;
};

long long ARCAN_VIDEO_WORLDID = -1;

static unsigned int font_cache_size = ARCAN_FONT_CACHE_LIMIT;
static struct font_entry font_cache[ARCAN_FONT_CACHE_LIMIT] = {0};

struct arcan_video_display arcan_video_display = {
	.bpp = 0, .width = 0, .height = 0, .conservative = false,
	.deftxs = GL_CLAMP_TO_EDGE, .deftxt = GL_CLAMP_TO_EDGE,
	.screen = NULL, .scalemode = ARCAN_VIMAGE_SCALEPOW2,
	.suspended = false,
	.vsync = true,
	.msasamples = 4,
	.c_ticks = 1,
	.default_vitemlim = 1024,
	.imageproc = imageproc_normal,
    .mipmap = true
};

/* these all represent a subset of the current context that is to be drawn.
 * if (dest != NULL) this means tha the vid actually represents a rendertarget, e.g. FBO or PBO.
 * the mode defines which output buffers (color, depth, ...) that should be stored.
 * readback defines if we want a PBO- or glReadPixels style readback into the .raw buffer of the target.
 * reset defines if any of the intermediate buffers should be cleared beforehand.
 * first refers to the first object in the subset.
 * if first and dest are null, stop processing the list of rendertargets. */

enum rendertarget_mode {
	RENDERTARGET_DEPTH = 0,
	RENDERTARGET_COLOR = 1,
	RENDERTARGET_COLOR_DEPTH = 2,
	RENDERTARGET_COLOR_DEPTH_STENCIL = 3
};

struct rendertarget {
	GLuint fbo, depth; /* depth and stencil are combined as stencil_index formats have poor driver support */
	GLfloat ortho_proj;
	
	bool readback, reset;  
	enum rendertarget_mode mode;
	
	arcan_vobject* color;
	arcan_vobject_litem* first;
};

struct arcan_video_context {
	unsigned vitem_ofs;
	unsigned vitem_limit;
	long int nalive;
	struct text_format curr_style;
	
	arcan_vobject world;
	arcan_vobject* vitems_pool;

	struct rendertarget rtargets[RENDERTARGET_LIMIT];
	size_t n_rtargets;
	
	struct rendertarget stdoutp;
};

arcan_errc arcan_video_attachobject(arcan_vobj_id id);
arcan_errc arcan_video_deleteobject(arcan_vobj_id id);
static arcan_errc arcan_video_getimage(const char* fname, arcan_vobject* dst, arcan_vstorage* dstframe, bool asynchsrc);

static struct arcan_video_context context_stack[CONTEXT_STACK_LIMIT] = {
	{
		.n_rtargets = 0,
		.vitem_ofs = 1,
		.nalive    = 0,
		.curr_style = {
			.col = {.r = 0xff, .g = 0xff, .b = 0xff}
		},
		.world = {
			.current  = {
				.opa = 1.0
			}
		}
	}
};
static unsigned context_ind = 0;

/* a default more-or-less empty context */
static struct arcan_video_context* current_context = context_stack;

static void allocate_and_store_globj(arcan_vobject* dst, unsigned* dstid, unsigned w, unsigned h, void* buf){
	glGenTextures(1, dstid);
	glBindTexture(GL_TEXTURE_2D, *dstid);
	
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, dst->gl_storage.txu);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, dst->gl_storage.txv);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	
	if (arcan_video_display.mipmap){
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
	}
	else{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_FALSE);
	}
		
	glTexImage2D(GL_TEXTURE_2D, 0, GL_PIXEL_FORMAT, w, h, 0, GL_PIXEL_FORMAT, GL_UNSIGNED_BYTE, buf);
}

void arcan_video_default_imageprocmode(enum arcan_imageproc_mode mode)
{
	arcan_video_display.imageproc = mode;
}

/* scan through each cell in use.
 * if we want to clean the context permanently (delete flag)
 * just wrap the call to deleteobject,
 * otherwise we're in a suspend situation (i.e. just clean up some openGL resources,
 * and pause possble movies */
static void deallocate_gl_context(struct arcan_video_context* context, bool delete)
{
	for (int i = 1; i < context->vitem_limit; i++) {
		if (context->vitems_pool[i].flags.in_use){
			arcan_vobject* current = &(context->vitems_pool[i]);

			/* before doing any modification, wait for any async load calls to finish(!) */
			if (current->feed.state.tag == ARCAN_TAG_ASYNCIMG)
				arcan_video_pushasynch(i);
				
			if (delete)
				arcan_video_deleteobject(i); /* will also delink from the render list */
			else {
				glDeleteTextures(1, &current->gl_storage.glid);
				if (current->feed.state.tag == ARCAN_TAG_FRAMESERV && current->feed.state.ptr)
					arcan_frameserver_pause((arcan_frameserver*) current->feed.state.ptr, true);
			}
		}
	}

	if (delete){
		free(context->vitems_pool);
		context->vitems_pool = NULL;
	}
}

static void step_active_frame(arcan_vobject* vobj)
{
	if (!vobj->frameset)
		return;

	do
		vobj->frameset_meta.current = (vobj->frameset_meta.current + 1) % vobj->frameset_meta.capacity;
	while (vobj->frameset[ vobj->frameset_meta.current ] == NULL);
	
	vobj->current_frame = vobj->frameset[ vobj->frameset_meta.current ];
}

/* go through a saved context, and reallocate all resources associated with it */
static void reallocate_gl_context(struct arcan_video_context* context)
{
/* If there's nothing saved, we reallocate */
	if (!context->vitems_pool){
		context->vitems_pool =  (arcan_vobject*) calloc( sizeof(arcan_vobject), arcan_video_display.default_vitemlim);
		context->vitem_ofs = 1;
		context->vitem_limit = arcan_video_display.default_vitemlim;
	} 
	else 
		for (int i = 1; i < context->vitem_limit; i++)
		if (context->vitems_pool[i].flags.in_use){
			arcan_vobject* current = &context->vitems_pool[i];

			if (current->flags.clone)
				continue;

		/* conservative means that we do not keep a copy of the originally decoded memory,
		 * essentially halving memory consumption but increasing cost of pop() and push() */
			if (arcan_video_display.conservative && (char)current->feed.state.tag == ARCAN_TAG_IMAGE){
				assert(current->default_frame.source );
				char* fname = strdup( current->default_frame.source ); /* copy the original filename */
				free(current->default_frame.source);
				arcan_video_getimage(fname, current, &current->default_frame, false);
				free(fname); /* getimage will copy again */
			}
			else
				allocate_and_store_globj(current, &current->gl_storage.glid, current->gl_storage.w, current->gl_storage.h, current->default_frame.raw);

			if (current->feed.state.tag == ARCAN_TAG_FRAMESERV && current->feed.state.ptr) {
				arcan_frameserver* movie = (arcan_frameserver*) current->feed.state.ptr;

				arcan_audio_rebuild(movie->aid);
				arcan_frameserver_resume(movie);
				arcan_audio_play(movie->aid);
			}
		}
}

unsigned arcan_video_nfreecontexts()
{
		return CONTEXT_STACK_LIMIT - 1 - context_ind;
}

signed arcan_video_pushcontext()
{
	struct text_format empty_style = { .col = {.r = 0xff, .g = 0xff, .b = 0xff} };
	arcan_vobject empty_vobj = {.current = {.position = {0}, .opa = 1.0} };

	if (context_ind + 1 == CONTEXT_STACK_LIMIT)
		return -1;
	
	/* copy everything then manually reset some fields */
	memcpy(&context_stack[ ++context_ind ], current_context, sizeof(struct arcan_video_context));
	deallocate_gl_context(current_context, false);
	
	current_context = &context_stack[ context_ind ];
	memset(&current_context->stdoutp, 0, sizeof( struct rendertarget ) );
	current_context->stdoutp.first = NULL;
	current_context->curr_style = empty_style;
	current_context->vitem_ofs = 1;

	current_context->world = empty_vobj;
	current_context->world.current.scale.x = 1.0;
	current_context->world.current.scale.y = 1.0;
	current_context->world.current.scale.z = 1.0;
	current_context->world.current.opa = 1.0;
	current_context->world.current.rotation.quaternion = build_quat_euler(0, 0, 0);
	
	current_context->vitem_limit = arcan_video_display.default_vitemlim;
	current_context->vitems_pool = (arcan_vobject*) calloc(sizeof(arcan_vobject), current_context->vitem_limit);
	current_context->rtargets[0].first = NULL;

	return arcan_video_nfreecontexts();
}

unsigned arcan_video_popcontext()
{
	deallocate_gl_context(current_context, true);
	
	if (context_ind > 0){
		context_ind--;
		current_context = &context_stack[ context_ind ];		
	}
	
	reallocate_gl_context(current_context);

	return (CONTEXT_STACK_LIMIT - 1) - context_ind;
}

arcan_vobj_id arcan_video_allocid(bool* status)
{
	unsigned i = current_context->vitem_ofs;
	*status = false;

/* scan from vofs until full wrap-around */
	while (i != current_context->vitem_ofs - 1){
		if (i == 0) /* 0 is protected */
			i = 1;
		
		if (!current_context->vitems_pool[i].flags.in_use){
			*status = true;
			current_context->nalive++;
			current_context->vitems_pool[i].flags.in_use = true;
			current_context->vitem_ofs = (current_context->vitem_ofs + 1) >= current_context->vitem_limit ? 1 : i + 1;
			return i;
		}

		i = (i + 1) % (current_context->vitem_limit - 1);
	}

	return 0;
}

arcan_vobj_id arcan_video_cloneobject(arcan_vobj_id parent)
{
	arcan_vobject* pobj = arcan_video_getobject(parent);
	arcan_vobj_id rv;
	
	if (pobj == NULL)
		return 0;

	bool status;
	rv = arcan_video_allocid(&status);
    
	if (status){
		surface_properties newprop = {
		.position.x = 0,
		.position.y = 0,
		.scale.x = 1.0,
		.scale.y = 1.0,
		.scale.z = 1.0
		};
		
		arcan_vobject* nobj = arcan_video_getobject(rv);
		memcpy(nobj, pobj, sizeof(arcan_vobject));
		memset(&nobj->default_frame, 0, sizeof( arcan_vstorage ));
		nobj->current = newprop;
		nobj->cellid = rv;
		assert(nobj->cellid > 0);
		nobj->current.rotation.quaternion = build_quat_euler(0.0, 0.0, 0.0);
		nobj->transform = NULL;
		nobj->parent = pobj;
		nobj->flags.clone = true;
		arcan_video_attachobject(rv);
	}

	return rv;
}

static void generate_basic_mapping(float* dst, float st, float tt)
{
	dst[0] = 0.0;
	dst[1] = 0.0;
	dst[2] = st;
	dst[3] = 0.0;
	dst[4] = st;
	dst[5] = tt;
	dst[6] = 0.0;
	dst[7] = tt;
}

static void generate_mirror_mapping(float* dst, float st, float tt)
{
	dst[6] = 0.0;
	dst[7] = 0.0;
	dst[4] = st;
	dst[5] = 0.0;
	dst[2] = st;
	dst[3] = tt;
	dst[0] = 0.0;
	dst[1] = tt;
}

arcan_vobject* arcan_video_newvobject(arcan_vobj_id* id)
{
	arcan_vobject* rv = NULL;
	bool status;
	arcan_vobj_id fid = arcan_video_allocid(&status);
    
	if (status) {
		rv = current_context->vitems_pool + fid;
		rv->current_frame = rv;
		rv->gl_storage.txu = arcan_video_display.deftxs;
		rv->gl_storage.txv = arcan_video_display.deftxt;
		rv->gl_storage.scale = arcan_video_display.scalemode;
		rv->gl_storage.imageproc = arcan_video_display.imageproc;
		rv->flags.cliptoparent = false;
		rv->current.scale.x = 1.0;
		rv->current.scale.y = 1.0;
		rv->current.scale.z = 1.0;
		rv->current.rotation.quaternion = build_quat_euler(0, 0, 0);
		rv->current.opa = 0.0;
		rv->cellid = fid;
		assert(rv->cellid > 0);
		generate_basic_mapping(rv->txcos, 1.0, 1.0);
		rv->parent = &current_context->world;
		rv->mask = MASK_ORIENTATION | MASK_OPACITY | MASK_POSITION;
	}

	if (id != NULL)
		*id = fid;

	return rv;
}

arcan_vobject* arcan_video_getobject(arcan_vobj_id id)
{
	arcan_vobject* rc = NULL;

	if (id > 0 && id < current_context->vitem_limit && current_context->vitems_pool[id].flags.in_use)
		rc = current_context->vitems_pool + id;
	else
		if (id == ARCAN_VIDEO_WORLDID) {
			rc = &current_context->world;
		}

	return rc;
}

static void attach_object(struct rendertarget* dst, arcan_vobject* src)
{
	arcan_vobject_litem* new_litem = malloc(sizeof *new_litem);
	new_litem->next = new_litem->previous = NULL;
	new_litem->elem = src;
	src->owner = new_litem;

	if (!dst->first)
		dst->first = new_litem;
	else if (dst->first->elem->order > src->order) { /* insert first */
		new_litem->next = dst->first;
		dst->first = new_litem;
		new_litem->next->previous = new_litem;
	} /* insert "anywhere" */
	else {
		bool last = true;
		arcan_vobject_litem* ipoint = dst->first;
		/* scan for insertion point */
		do 
			last = (ipoint->elem->order <= src->order);
		while (last && ipoint->next && (ipoint = ipoint->next));

		if (last) {
			new_litem->previous = ipoint;
			ipoint->next = new_litem;
		}
		else {
			ipoint->previous->next = new_litem;
			new_litem->previous = ipoint->previous;
			ipoint->previous = new_litem;
			new_litem->next = ipoint;
		}
	}
}

arcan_errc arcan_video_attachobject(arcan_vobj_id id)
{
	arcan_vobject* src = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_BAD_RESOURCE;

	if (src) {
		attach_object(&current_context->stdoutp, src);
		rv = ARCAN_OK;
	}

	return rv;
}

/* run through the chain and delete all occurences at ofs */
static void swipe_chain(surface_transform* base, unsigned int ofs, unsigned size)
{
	while (base) {
		memset((void*)base + ofs, 0, size);
		base = base->next;
	}
}

/* copy a transform and at the same time, compact it into a better sized buffer */
static surface_transform* dup_chain(surface_transform* base)
{
	if (!base)
		return NULL;
	
	unsigned count = 1;
	surface_transform* res = (surface_transform*) malloc(sizeof(surface_transform));
	surface_transform* current = res;
	
	while (base)
	{
		memcpy(current, base, sizeof(surface_transform));

		if (base->next)
			current->next = (surface_transform*) malloc(sizeof(surface_transform));
		else
			current->next = NULL;

		current = current->next;
		base = base->next;
	}
	
	return res;
}

static bool detatch_fromtarget(struct rendertarget* dst, arcan_vobject* src, bool gotowner)
{
	arcan_vobject_litem* current_litem;
	if (gotowner && src->owner)
		current_litem = src->owner;
	else {
		current_litem = dst->first;
		while (current_litem && current_litem->elem && current_litem->elem != src)
			current_litem = current_litem->next;
	}

	if (!current_litem || !current_litem->elem)
		return false;
		
/* with frameset objects, we can get a detatch call even though it's not attached */
	if (gotowner && src->owner)
		src->owner = NULL;

/* double-linked */
	if (current_litem->previous)
		current_litem->previous->next = current_litem->next;
	else
		dst->first = current_litem->next; /* only first cell lacks a previous node */

	if (current_litem->next)
		current_litem->next->previous = current_litem->previous;

	memset(current_litem, 0, sizeof(arcan_vobject_litem));
	free(current_litem);
	return true;
}

arcan_errc arcan_video_detatchobject(arcan_vobj_id id)
{
	arcan_vobject* src = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	if (src)
		rv = detatch_fromtarget(&current_context->stdoutp, src, true) ? ARCAN_OK : ARCAN_ERRC_UNACCEPTED_STATE;
	
	return rv;
}

enum arcan_transform_mask arcan_video_getmask(arcan_vobj_id id)
{
	enum arcan_transform_mask mask = 0;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0)
		mask = vobj->mask;

	return mask;
}


arcan_errc arcan_video_transformmask(arcan_vobj_id id, enum arcan_transform_mask mask)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		vobj->mask = mask;
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_video_linkobjs(arcan_vobj_id srcid, arcan_vobj_id parentid, enum arcan_transform_mask mask)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* src = arcan_video_getobject(srcid);
	arcan_vobject* dst = arcan_video_getobject(parentid);

	if (srcid == parentid || parentid == 0)
		dst = &current_context->world;

	if (src && src->flags.clone)
		return ARCAN_ERRC_CLONE_NOT_PERMITTED;
	else if (src && dst) {
		arcan_vobject* current = dst;

		while (current) {
			if (current->parent == src)
				return ARCAN_ERRC_CLONE_NOT_PERMITTED;
			else
				current = current->parent;
		}
	
		src->parent = dst;

		swipe_chain(src->transform, offsetof(surface_transform, blend), sizeof(struct transf_blend));
		swipe_chain(src->transform, offsetof(surface_transform, move), sizeof(struct transf_move));
		swipe_chain(src->transform, offsetof(surface_transform, scale), sizeof(struct transf_scale));
		swipe_chain(src->transform, offsetof(surface_transform, rotate), sizeof(struct transf_rotate));
		rv = ARCAN_OK;
		src->mask = mask;
	}

	return rv;
}

static float ortho_proj[16];
static void arcan_video_gldefault()
{
/* not 100% sure which of these have been replaced by the programmable pipeline or not .. */
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_SCISSOR_TEST);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_FOG);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
	
	if (arcan_video_display.msasamples)
	    	glEnable(GL_MULTISAMPLE);

	glEnable(GL_BLEND);
	glClearColor(0.0, 0.0, 0.0, 1.0f);
	glAlphaFunc(GL_GREATER, 0);
	glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
	glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
    
	glEnable(GL_LINE_SMOOTH);
	glEnable(GL_POLYGON_SMOOTH);
	build_orthographic_matrix(ortho_proj, 0, arcan_video_display.width, arcan_video_display.height, 0, 0, 1);
	glScissor(0, 0, arcan_video_display.width, arcan_video_display.height);
	glFrontFace(GL_CW);
	glCullFace(GL_BACK);
}


const static char* defvprg =
"uniform mat4 modelview;\n"
"uniform mat4 projection;\n"

"attribute vec2 texcoord;\n"
"varying vec2 texco;\n"
"attribute vec4 vertex;\n"
"void main(){\n"
"	gl_Position = (projection * modelview) * vertex;\n"
"   texco = texcoord;\n"
"}";

const static char* deffprg =
"uniform sampler2D map_diffuse;\n"
"varying vec2 texco;\n"
"uniform float obj_opacity;\n"
"void main(){\n"
"   vec4 col = texture2D(map_diffuse, texco);\n"
"   col.a = col.a * obj_opacity;\n"
"	gl_FragColor = col;\n"
" }";

arcan_errc arcan_video_init(uint16_t width, uint16_t height, uint8_t bpp, bool fs, bool frames, bool conservative)
{
/* some GL attributes have to be set before creating the video-surface */
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_SWAP_CONTROL, arcan_video_display.vsync == true ? 1 : 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	
	if (arcan_video_display.msasamples > 0){ 
	        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, arcan_video_display.msasamples);
	}

	SDL_WM_SetCaption("Arcan", "Arcan");

	arcan_video_display.fullscreen = fs;
	arcan_video_display.sdlarg = (fs ? SDL_FULLSCREEN : 0) | SDL_OPENGL | (frames ? SDL_NOFRAME : 0);
	arcan_video_display.screen = SDL_SetVideoMode(width, height, bpp, arcan_video_display.sdlarg);

	if (arcan_video_display.msasamples && !arcan_video_display.screen){
		arcan_warning("arcan_video_init(), Couldn't open OpenGL display, attempting without MSAA\n");
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		arcan_video_display.screen = SDL_SetVideoMode(width, height, bpp, arcan_video_display.sdlarg);
	}

	if (!arcan_video_display.screen){
		arcan_warning("arcan_video_init(), SDL_SetVideoMode failed, reason: %s\n", SDL_GetError());
		return ARCAN_ERRC_BADVMODE; 
	}

/* need to be called AFTER we have a valid GL context, else we get the "No GL version" */
	int err;
	if ( (err = glewInit()) != GLEW_OK){
		arcan_fatal("Couldn't initialize GLew: %s\n", glewGetErrorString(err));
	}

	arcan_video_display.width = width;
	arcan_video_display.height = height;
	arcan_video_display.bpp = bpp;
	arcan_video_display.conservative = conservative;
	arcan_video_display.defaultshdr  = arcan_shader_build("DEFAULT", NULL, defvprg, deffprg);
		
	if (arcan_video_display.screen) {
		if (TTF_Init() == -1) {
			arcan_warning("Warning: arcan_video_init(), Couldn't initialize freetype. Text rendering disabled.\n");
			arcan_video_display.text_support = false;
		}
		else
			arcan_video_display.text_support = true;

		current_context->world.current.scale.x = 1.0;
		current_context->world.current.scale.y = 1.0;
		current_context->vitem_limit = arcan_video_display.default_vitemlim;
		current_context->vitems_pool = (arcan_vobject*) calloc(sizeof(arcan_vobject), current_context->vitem_limit);
		arcan_video_gldefault();
		arcan_3d_setdefaults();
	}

	return arcan_video_display.screen ? ARCAN_OK : ARCAN_ERRC_BADVMODE;
}

uint16_t arcan_video_screenw()
{
	return arcan_video_display.screen ? arcan_video_display.screen->w : 0;
}

uint16_t arcan_video_screenh()
{
	return arcan_video_display.screen ? arcan_video_display.screen->h : 0;
}

static inline uint16_t nexthigher(uint16_t k)
{
	k--;
	for (int i=1; i < sizeof(uint16_t) * 8; i = i * 2)
		k = k | k >> i;
	return k+1;
}

/* this is not particularly reliable either */
void arcan_video_fullscreen()
{
	SDL_WM_ToggleFullScreen(arcan_video_display.screen);
}

/* it would be interesting and useful to limit this to PNG and JPG and strip out SDL_Image altogether.
 * futhermore, check up on libpng memory access patterns, MMAP the file with MADIVSE on MADV_SEQUENTIAL and MAP_POPULATE
 * since this function is used extremely often and shuffles a lot of data, furthermore, we would like to split this into 
 * a subprocess with lesser privileges due to the .. "varying" quality of image decoding routines. */
#ifndef ASYNCH_CONCURRENT_THREADS
#define ASYNCH_CONCURRENT_THREADS 8
#endif 

/* ZLib antialiased SDL- based upscaler from SDL-gfx/rotozoom package */
#include "stretchblit.c"

/* copy RGBA src row by row with optional "flip",
 * swidth <= dwidth */
static inline void imagecopy(uint32_t* dst, uint32_t* src, int dwidth, int swidth, int height, bool flipv)
{
	if (flipv)
	{
		for (int drow = height-1, srow = 0; srow < height; srow++, drow--)
			memcpy(&dst[drow * dwidth], &src[srow * swidth], swidth * 4);
	}
	else
		for (int row = 0; row < height; row++)
			memcpy(&dst[row * dwidth], &src[row * swidth], swidth * 4);
}

static arcan_errc arcan_video_getimage(const char* fname, arcan_vobject* dst, arcan_vstorage* dstframe, bool asynchsrc)
{
	static SDL_sem* asynchsynch = NULL;
	if (!asynchsynch)
		asynchsynch = SDL_CreateSemaphore(ASYNCH_CONCURRENT_THREADS);

/* with asynchsynch, it's likely that we get a storm of requests and we'd likely suffer 
 * thrashing, so limit this. */
	SDL_SemWait(asynchsynch);
	
    arcan_errc rv = ARCAN_ERRC_BAD_RESOURCE;
	SDL_Surface* res = IMG_Load(fname);

	if (res) {
		dst->origw = res->w;
		dst->origh = res->h;

	/* the thread_loader will take care of converting the asynchsrc to an image once its completely done */
		if (!asynchsrc)
			dst->feed.state.tag = ARCAN_TAG_IMAGE;
		
		dstframe->source = strdup(fname);
		
	/* let SDL do byte-order conversion and make sure we have BGRA, ... */
		SDL_Surface* gl_image =
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		    SDL_CreateRGBSurface(SDL_SWSURFACE, res->w, res->h, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
#else
		    SDL_CreateRGBSurface(SDL_SWSURFACE, res->w, res->h, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
#endif
		SDL_SetAlpha(res, 0, SDL_ALPHA_TRANSPARENT);
		SDL_BlitSurface(res, NULL, gl_image, NULL);

		uint16_t neww, newh;
		if (dst->gl_storage.scale == ARCAN_VIMAGE_NOPOW2){
			neww = gl_image->w;
			newh = gl_image->h;
		}
		else {
			neww = nexthigher(gl_image->w);
			newh = nexthigher(gl_image->h);
		}
		
		dst->gl_storage.w = neww;
		dst->gl_storage.h = newh;

		dstframe->s_raw = neww * newh * 4;
		dstframe->raw = (uint8_t*) malloc(dstframe->s_raw);
		
		if (newh != gl_image->h || neww != gl_image->w){

/* we need a stretch blit or patch coordinates */
			if (dst->gl_storage.scale == ARCAN_VIMAGE_SCALEPOW2){
/* for stretch blit, we use the interpolated upscaler from SDL_gfx/rotozoom,
 * as gluScaleImage is not present in GLES and not thread-safe (sigh) */
				stretchblit(gl_image, (uint32_t*) dstframe->raw, neww, newh, neww * 4, dst->gl_storage.imageproc == imageproc_fliph); 
			}
			else if (dst->gl_storage.scale == ARCAN_VIMAGE_TXCOORD){
				memset(dstframe->raw, 0, dstframe->s_raw); /* black 0 alpha "border" */
/* dst is aligned with the nearest power of 2 of the dimensions of source */
				imagecopy((uint32_t*) dstframe->raw, gl_image->pixels, 
									neww, gl_image->w, gl_image->h, dst->gl_storage.imageproc == imageproc_fliph); 

/* Patch texture coordinates */
				float hx = (float)dst->origw / (float)dst->gl_storage.w;
				float hy = (float)dst->origh / (float)dst->gl_storage.h;
				generate_basic_mapping(dst->txcos, hx, hy);
			}

		}
		else{ /* src and dst match, only do line- by line copy if flip is needed */
			if (dst->gl_storage.imageproc == imageproc_fliph)
				imagecopy((uint32_t*)dstframe->raw, gl_image->pixels, neww, neww, newh, true);
			else
				memcpy(dstframe->raw, gl_image->pixels, dstframe->s_raw);
		}
		
		if (!asynchsrc)
			allocate_and_store_globj(dst, &dst->gl_storage.glid, dst->gl_storage.w, dst->gl_storage.h, dstframe->raw);

		SDL_FreeSurface(res);
		SDL_FreeSurface(gl_image);
	
		if (!asynchsrc && arcan_video_display.conservative){
#ifdef DEBUG
			memset(dst->default_frame.raw, 0x50, dst->default_frame.s_raw);
#endif
			free(dst->default_frame.raw);
			dst->default_frame.raw = 0;
		}

		rv = ARCAN_OK;
	}

	SDL_SemPost(asynchsynch);
	return rv;
}

void arcan_video_3dorder(bool first){
	if (first)
		arcan_video_display.late3d = false;
	else
		arcan_video_display.late3d = true;
}

arcan_errc arcan_video_allocframes(arcan_vobj_id id, unsigned char capacity, enum arcan_framemode mode)
{
	arcan_vobject* target = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	if (!target)
		return rv;

	if (target->flags.clone)
		return ARCAN_ERRC_CLONE_NOT_PERMITTED;
	
		capacity++; /* reserve 1 */
		if (capacity == 0)
			return ARCAN_ERRC_OUT_OF_SPACE;

		if (target->frameset)
			free(target->frameset);
		
		target->frameset = (arcan_vobject**) calloc(capacity, sizeof(arcan_vobject*) );
		target->frameset[0] = target;
		target->frameset_meta.current = 0;
		target->frameset_meta.capacity = capacity;
		target->frameset_meta.framemode = mode;

	return ARCAN_OK;
}

arcan_errc arcan_video_framecyclemode(arcan_vobj_id id, signed mode)
{
	arcan_vobject* vobj = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;

/* all the real work is done in tick/render */
	if (vobj){
		vobj->frameset_meta.mode = mode;
		vobj->frameset_meta.counter = abs(mode);
		rv = ARCAN_OK;
	}
	
	return rv;
}

arcan_vobj_id arcan_video_rawobject(uint8_t* buf, size_t bufs, img_cons constraints, float origw, float origh, unsigned short zv)
{
	arcan_vobj_id rv = 0;

	if (buf && bufs == (constraints.w * constraints.h * constraints.bpp) && constraints.bpp == 4) {
		arcan_vobject* newvobj = arcan_video_newvobject(&rv);

		if (!newvobj)
			return ARCAN_EID;

		newvobj->gl_storage.w = constraints.w;
		newvobj->gl_storage.h = constraints.h;
		newvobj->origw = origw;
		newvobj->origh = origh;

	/* allocate */
		glGenTextures(1, &newvobj->gl_storage.glid);
		
	/* tacitly assume diffuse is bound to tu0 */
		glBindTexture(GL_TEXTURE_2D, newvobj->gl_storage.glid);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

		newvobj->gl_storage.ncpt = constraints.bpp;
		newvobj->default_frame.s_raw = bufs;
		newvobj->default_frame.raw = buf;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_PIXEL_FORMAT,
			newvobj->gl_storage.w, newvobj->gl_storage.h, 0,
			GL_PIXEL_FORMAT, GL_UNSIGNED_BYTE, newvobj->default_frame.raw);
		
		glBindTexture(GL_TEXTURE_2D, 0);
		newvobj->order = 0;
		arcan_video_attachobject(rv);
	}

	return rv;
}

/* glGenFramebuffers(n, ptr_ids);
 * glDeleteFramebuffers when destroying the fbo
 * 
 * glBindFramebuffer(before rendering)
 * bind 0 for default.
 * 
 * glRenderbufferStorage(GL_RENDERBUFER, RGBA, width, height);
 * glFramebufferTexture2D(GL_FRAMEBUFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE2D,
 * 
 * verify with glCheckFrameBufferStatus(target) == GL_FRAMEBUFER_COMPLETE
 * 
 * For PBO readback,
 * glGenBuffers(1, pbo);
 * glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
 * glReadBuffer(fbnotex);
 * glReadPixels(0, 0, w, h, format, type, 0);
 * glBindBuffer ->A glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
 * 
 * to free, BindBuffer, UnmapBuffer, BindBuffer0, DeleteBuffers*/

arcan_errc arcan_video_attachtorendertarget(arcan_vobj_id did, arcan_vobj_id src)
{
	arcan_vobject* dstobj = arcan_video_getobject(did);
	arcan_vobject* srcobj = arcan_video_getobject(src);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	if (dstobj && srcobj){
/* find dstobj in rendertargets */
		rv = ARCAN_ERRC_UNACCEPTED_STATE;
		
		for (int ind = 0; ind < current_context->n_rtargets; ind++){
			if (current_context->rtargets[ind].color == dstobj){
				attach_object(&current_context->rtargets[ind], srcobj); 
				rv = ARCAN_OK;
			}
	
		}
	}
	
	return rv;
}


static bool alloc_fbo(struct rendertarget* dst)
{
	glGenFramebuffers(1, &dst->fbo);

/* need both stencil and depth buffer, but we don't need the data from them */
	glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);

	if (dst->mode > RENDERTARGET_DEPTH)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst->color->gl_storage.glid, 0);

		if (dst->mode >= RENDERTARGET_COLOR_DEPTH) {
			glGenRenderbuffers(1, &dst->depth);
/* need a depth buffer (can optimize if we knew that no 3D vids was present */
			glBindRenderbuffer(GL_RENDERBUFFER, dst->depth); 
			glRenderbufferStorage(GL_RENDERBUFFER, 
				dst->mode == RENDERTARGET_COLOR_DEPTH_STENCIL ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT, 
				dst->color->gl_storage.w, dst->color->gl_storage.h);
		}
	}
/* DEPTH buffer only (shadowmapping, ...) */
	else {
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dst->color->gl_storage.glid, 0);
	}
	
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

arcan_errc arcan_video_setuprendertarget(arcan_vobj_id did, int readback)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(did);

/* FIXME, check for duplicates */
	if (vobj)
		if (current_context->n_rtargets < RENDERTARGET_LIMIT){
			struct rendertarget* dst = &current_context->rtargets[ current_context->n_rtargets++ ];
			dst->mode = RENDERTARGET_COLOR_DEPTH_STENCIL;
			dst->readback = readback;
			dst->color = vobj;
			if (dst->color->gl_storage.w != arcan_video_display.width ||
				dst->color->gl_storage.h != arcan_video_display.height) {
			}

			alloc_fbo(dst);
			rv = ARCAN_OK;
		}
		else 
			rv = ARCAN_ERRC_OUT_OF_SPACE;
	
	return rv;
}

arcan_errc arcan_video_setactiveframe(arcan_vobj_id dst, unsigned fid)
{
	arcan_vobject* dstvobj = arcan_video_getobject(dst);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	if (dstvobj && dstvobj->frameset){
		dstvobj->frameset_meta.current = fid < dstvobj->frameset_meta.capacity && dstvobj->frameset[fid] ? fid : 0;
		dstvobj->current_frame = dstvobj->frameset[ dstvobj->frameset_meta.current ];
		rv = ARCAN_OK;		
	}
	
	return rv;
}

arcan_vobj_id arcan_video_setasframe(arcan_vobj_id dst, arcan_vobj_id src, unsigned fid, bool detatch, arcan_errc* errc)
{
	arcan_vobject* dstvobj = arcan_video_getobject(dst);
	arcan_vobject* srcvobj = arcan_video_getobject(src);
	arcan_vobj_id rv = ARCAN_EID;
	fid++; /* enforce 1 index */
	
	if (errc)
		*errc = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	if (dstvobj && srcvobj){
		if (dstvobj->frameset && fid < dstvobj->frameset_meta.capacity){
			
/* if we want to manage the frame entirely through this object, we can detatch src and stop worrying about deleting it */
			if (detatch)
				arcan_video_detatchobject(src);

/* if there already is an object in the desired slot, return the management responsibility to the user*/
			if (dstvobj->frameset[fid]){
				arcan_vobject* frame = dstvobj->frameset[fid];
				rv = frame->cellid;
				assert(rv >= 0);
			}
				
			dstvobj->frameset[fid] = srcvobj;
			if (errc) *errc = ARCAN_OK;
		}
		else 
			if (errc) *errc = ARCAN_ERRC_OUT_OF_SPACE;
	}
	
	return rv;
}

struct thread_loader_args {
/* where the results will be stored */
	arcan_vobject* dst;
	arcan_vobj_id dstid;
	char* fname;
	intptr_t tag;
};

/* if the loading failed, we'll add a small black image in its stead,
 * and emit a failed video event */
static int thread_loader(void* in)
{
	arcan_event result;
	struct thread_loader_args* localargs = (struct thread_loader_args*) in;
	arcan_vobject* dst = localargs->dst;
	
/* while this happens, the following members of the struct are not to be touched elsewhere:
 * origw / origh, default_frame->tag/source, gl_storage */
	arcan_errc rc = arcan_video_getimage(localargs->fname, dst, &dst->default_frame, true);
	result.data.video.data = localargs->tag;
	
	if (rc == ARCAN_OK){ /* emit OK event */
		result.kind = EVENT_VIDEO_ASYNCHIMAGE_LOADED;
		result.data.video.constraints.w = dst->origw;
		result.data.video.constraints.h = dst->origh;
	} else {
		dst->origw = 32;
		dst->origh = 32;
		dst->default_frame.s_raw = 32 * 32 * 4;
		dst->default_frame.raw = (uint8_t*) malloc(dst->default_frame.s_raw);
		memset(dst->default_frame.raw, 0, dst->default_frame.s_raw);
		dst->gl_storage.w = 32;
		dst->gl_storage.h = 32;
		dst->default_frame.source = strdup(localargs->fname);
		result.data.video.data = localargs->tag;
		result.data.video.constraints.w = 32;
		result.data.video.constraints.h = 32;
		result.kind = EVENT_VIDEO_ASYNCHIMAGE_LOAD_FAILED;
		/* emit FAILED event */
	}

	result.data.video.source = localargs->dstid;
	result.category = EVENT_VIDEO;

	arcan_event_enqueue(arcan_event_defaultctx(), &result);
	free(localargs->fname);
	free(localargs);
	
	return 0;
}

/* create a new vobj, fill it out with enough vals that we can treat it 
 * as any other, but while the ASYNCIMG tag is active, it will be skipped in
 * rendering (linking, instancing etc. sortof works) but any external (script)
 * using the object before receiving a LOADED event may give undefined results */
static arcan_vobj_id loadimage_asynch(const char* fname, img_cons constraints, intptr_t tag)
{
	arcan_vobj_id rv = ARCAN_EID;
	arcan_vobject* dstobj = arcan_video_newvobject(&rv);

	struct thread_loader_args* args = (struct thread_loader_args*) calloc(sizeof(struct thread_loader_args), 1);
	args->dstid = rv;
	args->dst = dstobj;
	
	if (!args->dst){
		free(args);
		return ARCAN_EID;
	}
	
	args->fname = strdup(fname);
	args->tag = tag;
	args->dst->feed.state.tag = ARCAN_TAG_ASYNCIMG;
	args->dst->feed.state.ptr = (void*) SDL_CreateThread(thread_loader, (void*) args);
	
	return rv;
}

arcan_errc arcan_video_pushasynch(arcan_vobj_id source)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(source);

	if (vobj){
		if (vobj->feed.state.tag == ARCAN_TAG_ASYNCIMG){
		/* protect us against premature invocation */
			int status;
			SDL_WaitThread((SDL_Thread*)vobj->feed.state.ptr, &status);
			allocate_and_store_globj(vobj, &vobj->gl_storage.glid, vobj->gl_storage.w, vobj->gl_storage.h, vobj->default_frame.raw);
	
			if (arcan_video_display.conservative){
#ifdef DEBUG
				memset(vobj->default_frame.raw, 0x66, vobj->default_frame.s_raw);
#endif
				free(vobj->default_frame.raw);
				vobj->default_frame.raw = 0;
				vobj->default_frame.s_raw = 0;
			}
			
			vobj->feed.state.tag = ARCAN_TAG_IMAGE;
			vobj->feed.state.ptr = NULL;
		}
		else rv = ARCAN_ERRC_UNACCEPTED_STATE;
	}
	
	return rv;
}

static arcan_vobj_id loadimage(const char* fname, img_cons constraints, arcan_errc* errcode)
{
	GLuint gtid = 0;
	arcan_vobj_id rv = 0;

	arcan_vobject* newvobj = arcan_video_newvobject(&rv);
	if (newvobj == NULL)
		return ARCAN_EID;
	
	arcan_errc rc = arcan_video_getimage(fname, newvobj, &newvobj->default_frame, false);

	if (rc == ARCAN_OK) {
		newvobj->current.position.x = 0;
		newvobj->current.position.y = 0;
		newvobj->current.rotation.quaternion = build_quat_euler(0, 0, 0);
	}
	else{
		arcan_video_deleteobject(rv);
	}
	
	if (errcode != NULL)
		*errcode = rc;

	return rv;
}

vfunc_state* arcan_video_feedstate(arcan_vobj_id id)
{
	void* rv = NULL;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		rv = &vobj->feed.state;
	}

	return rv;
}

arcan_errc arcan_video_alterfeed(arcan_vobj_id id, arcan_vfunc_cb cb, vfunc_state state)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && vobj->flags.clone)
		return ARCAN_ERRC_CLONE_NOT_PERMITTED;
	
	if (vobj && id > 0) {
		if (cb) {
			vobj->feed.state = state;
			vobj->feed.ffunc = cb;
			rv = ARCAN_OK;
			if (state.tag == ARCAN_TAG_3DOBJ){
				vobj->order = abs(vobj->order) * -1;
			} else vobj->order = abs(vobj->order);
		}
		else
			rv = ARCAN_ERRC_BAD_ARGUMENT;
	}

	return rv;
}

static int8_t empty_ffunc(enum arcan_ffunc_cmd cmd, uint8_t* buf, uint32_t s_buf, uint16_t width, uint16_t height, uint8_t bpp, unsigned mode, vfunc_state state){
	return 0;
}

arcan_vfunc_cb arcan_video_emptyffunc()
{
	return (arcan_vfunc_cb) empty_ffunc;
}

arcan_vobj_id arcan_video_setupfeed(arcan_vfunc_cb ffunc, img_cons constraints, uint8_t ntus, uint8_t ncpt)
{
	if (!ffunc)
		return 0;

	arcan_vobj_id rv = 0;
	arcan_vobject* newvobj = arcan_video_newvobject(&rv);

	if (ffunc && newvobj) {
		arcan_vstorage* vstor = &newvobj->default_frame;
/* preset */
		newvobj->origw = constraints.w;
		newvobj->origh = constraints.h;
		newvobj->gl_storage.ncpt = ncpt == 0 ? 4 : ncpt;

		if (newvobj->gl_storage.scale == ARCAN_VIMAGE_NOPOW2){
			newvobj->gl_storage.w = constraints.w;
			newvobj->gl_storage.h = constraints.h;
		}
		else {
		/* For feeds, we don't do the forced- rescale on every frame, way too expensive */
			newvobj->gl_storage.w = nexthigher(constraints.w);
			newvobj->gl_storage.h = nexthigher(constraints.h);
			float hx = (float)constraints.w / (float)newvobj->gl_storage.w;
			float hy = (float)constraints.h / (float)newvobj->gl_storage.h;
			generate_basic_mapping(newvobj->txcos, hx, hy);
		}

		/* allocate */
		vstor->s_raw = newvobj->gl_storage.w * newvobj->gl_storage.h * newvobj->gl_storage.ncpt;
		vstor->raw = (uint8_t*) calloc(vstor->s_raw, 1);
		
		newvobj->feed.ffunc = ffunc;
		allocate_and_store_globj(newvobj, &newvobj->gl_storage.glid, newvobj->gl_storage.w, newvobj->gl_storage.h, newvobj->default_frame.raw);
	}

	return rv;
}

/* some targets like to change size dynamically (thanks for that),
 * thus, drop the allocated buffers, generate new one and tweak txcos */
arcan_errc arcan_video_resizefeed(arcan_vobj_id id, img_cons constraints, bool mirror)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && (vobj->flags.clone == true ||
        vobj->feed.state.tag != ARCAN_TAG_FRAMESERV))
		return ARCAN_ERRC_CLONE_NOT_PERMITTED;
	
	if (vobj) {
		if (vobj->feed.state.tag == ARCAN_TAG_ASYNCIMG)
			arcan_video_pushasynch(id);

#ifdef DEBUG
			if (vobj->default_frame.s_raw)
				memset(vobj->default_frame.s_raw, 0xee, s_raw);
#endif

		free(vobj->default_frame.raw);
		vobj->default_frame.s_raw = 0;
		vobj->default_frame.raw = NULL;

		if (vobj->gl_storage.scale == ARCAN_VIMAGE_NOPOW2){
			vobj->origw = vobj->gl_storage.w = constraints.w;
			vobj->origh = vobj->gl_storage.h = constraints.h;
		}
		else {
			vobj->gl_storage.w = nexthigher(constraints.w);
			vobj->gl_storage.h = nexthigher(constraints.h);
			vobj->origw = constraints.w;
			vobj->origh = constraints.h;
		}
		
		vobj->default_frame.s_raw = vobj->gl_storage.w * vobj->gl_storage.h * 4;
		vobj->default_frame.raw = (uint8_t*) calloc(vobj->default_frame.s_raw,1);

		float hx = vobj->gl_storage.scale == ARCAN_VIMAGE_NOPOW2 ? 1.0 : (float)constraints.w / (float)vobj->gl_storage.w;
		float hy = vobj->gl_storage.scale == ARCAN_VIMAGE_NOPOW2 ? 1.0 : (float)constraints.h / (float)vobj->gl_storage.h;

		/* as the dimensions may be different, we need to reinitialize the gl-storage as well */
		glBindTexture(GL_TEXTURE_2D, vobj->gl_storage.glid);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_PIXEL_FORMAT, vobj->gl_storage.w, vobj->gl_storage.h, 0, GL_PIXEL_FORMAT, GL_UNSIGNED_BYTE, vobj->default_frame.raw);
		if (mirror)
			generate_mirror_mapping(vobj->txcos, hx, hy);
		else
			generate_basic_mapping(vobj->txcos, hx, hy);
			
		glBindTexture(GL_TEXTURE_2D, 0);
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_vobj_id arcan_video_loadimageasynch(const char* rloc, img_cons constraints, intptr_t tag)
{
	arcan_vobj_id rv = loadimage_asynch(rloc, constraints, tag);

	if (rv > 0) {
		arcan_vobject* vobj = arcan_video_getobject(rv);

		if (vobj){
			vobj->current.rotation.quaternion = build_quat_euler( 0, 0, 0 );
			arcan_video_attachobject(rv);
		}
	}

	return rv;
}

arcan_vobj_id arcan_video_loadimage(const char* rloc,img_cons constraints, unsigned short zv)
{
	arcan_vobj_id rv = loadimage((char*) rloc, constraints, NULL);

/* the asynch version could've been deleted in between, so we need to double check */
		if (rv > 0) {
		arcan_vobject* vobj = arcan_video_getobject(rv);
		if (vobj){
			vobj->order = zv;
			vobj->current.rotation.quaternion = build_quat_euler( 0, 0, 0 );
			arcan_video_attachobject(rv);
		}
	}

	return rv;
}

arcan_vobj_id arcan_video_addfobject(arcan_vfunc_cb feed, vfunc_state state, img_cons constraints, unsigned short zv)
{
	arcan_vobj_id rv;
	const int feed_ntus = 1;

	if ((rv = arcan_video_setupfeed(feed, constraints, feed_ntus, constraints.bpp)) > 0) {
		arcan_vobject* vobj = arcan_video_getobject(rv);
		vobj->order = zv;
		vobj->feed.state = state;

		if (state.tag == ARCAN_TAG_3DOBJ)
			vobj->order = -1 * zv;
		
		arcan_video_attachobject(rv);
	}

	return rv;
}

arcan_errc arcan_video_scaletxcos(arcan_vobj_id id, float sfs, float sft)
{
	arcan_vobject* vobj = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;

	if (vobj){
		vobj->txcos[0] *= sfs; 	vobj->txcos[2] *= sfs; 	vobj->txcos[4] *= sfs; 	vobj->txcos[6] *= sfs;
		vobj->txcos[1] *= sft; 	vobj->txcos[3] *= sft; 	vobj->txcos[5] *= sft; 	vobj->txcos[7] *= sft;
				
		rv = ARCAN_OK;
	}

	return rv;
}

/*
 * This one is a mess,
 * (a) begin by splitting the input string into a linked list of data elements.
 * each element can EITHER modfiy to current cursor position OR represent a rendered surface.

 * (b) take the linked list, sweep through it and figure out which dimensions it requires,
 * allocate a corresponding storage object.

 * (c) sweep the list yet again, render to the storage object.
*/

// TTF_FontHeight sets line-spacing.
struct rcell {

	bool surface;
	unsigned int width;
	unsigned int height;

	union {
		SDL_Surface* surf;

		struct {
			uint8_t newline;
			uint8_t tab;
			bool cr;
		} format;
	} data;

	struct rcell* next;
};

/* Simple font-cache */
static TTF_Font* grab_font(const char* fname, uint8_t size)
{
	int leasti = 0, i;
	const int nchannels_rgba = 4;

	if (!fname || !arcan_video_display.text_support)
		return NULL;

	for (i = 0; i < font_cache_size; i++) {
		/* (a) no need to look further, either empty slot or no font found. */
		if (font_cache[i].data == NULL)
			break;
		if (strcmp(fname, font_cache[i].identifier) == 0 &&
		        font_cache[i].size == size) {
			font_cache[i].usecount++;
			return font_cache[i].data;
		}

		if (font_cache[i].usecount < font_cache[leasti].usecount)
			leasti = i;
	}

	/* (b) no match, no empty slot. */
	if (font_cache[i].data != NULL) {
		free(font_cache[leasti].identifier);
		TTF_CloseFont(font_cache[leasti].data);
	}
	else
		i = leasti;

	/* load in new font */
	font_cache[i].data = TTF_OpenFont(fname, size);
	font_cache[i].identifier = strdup(fname);
	font_cache[i].usecount = 1;
	font_cache[i].size = size;

	return font_cache[i].data;
}

/*
 * starting from a suspected format character,
 * repeatedly look for format characters.
 * When there is none- left to be found,
 */
struct text_format formatend(char* base, struct text_format prev, char* orig, bool* ok) {
	struct text_format failed = {0};
	prev.newline = prev.tab = prev.cr = 0; /* don't carry caret modifiers */
	
	bool inv = false;

	while (*base) {
		if (isspace(*base)) {
			base++;
			continue;
		}

		if (*base != '\\') {
			prev.endofs = base;
			break;
		}

		else {
		retry:
			switch (*(base+1)) {
					char* fontbase = base+1;
					char* numbase;
					char cbuf[3];

/* missing; halign row, lalign row, ralign row */
				case '!':
					inv = true;
					base++;
					*base = '\\';
					goto retry;
					break;
				case 't':
					prev.tab++;
					base += 2;
					break;
				case 'n':
					prev.newline++;
					base += 2;
					break;
				case 'r':
					prev.cr = true;
					base += 2;
					break;
				case 'b':
					prev.style = (inv ? prev.style & !TTF_STYLE_BOLD : prev.style | TTF_STYLE_BOLD);
					inv = false;
					base+=2;
					break;
				case 'i':
					prev.style = (inv ? prev.style & !TTF_STYLE_ITALIC : prev.style | TTF_STYLE_ITALIC);
					inv = false;
					base+=2;
					break;
				case 'u':
					prev.style = (inv ? prev.style & TTF_STYLE_UNDERLINE : prev.style | TTF_STYLE_UNDERLINE);
					inv = false;
					base+=2;
					break;
				case '#':
					base += 2;
					for (int i = 0; i < 3; i++) {
						/* slide to cell */
						if (!isxdigit(*base) || !isxdigit(*(base+1))) {
							arcan_warning("Warning: arcan_video_renderstring(), couldn't scan font colour directive (#rrggbb, 0-9, a-f)\n");
							*ok = false;
 							return failed;
						}

						base += 2;
					}
/* now we know 6 valid chars are there, time to collect. */
					cbuf[0] = *(base - 6);
					cbuf[1] = *(base - 5);
					cbuf[2] = 0;
					prev.col.r = strtol(cbuf, 0, 16);
					cbuf[0] = *(base - 4);
					cbuf[1] = *(base - 3);
					cbuf[2] = 0;
					prev.col.g = strtol(cbuf, 0, 16);
					cbuf[0] = *(base - 2);
					cbuf[1] = *(base - 1);
					cbuf[2] = 0;
					prev.col.b = strtol(cbuf, 0, 16);

					break;

/* read 6(+2) characters, convert in pairs of two to hex, fill SDL_Color and set .alpha */
					break;
				case 'f':
					fontbase = (base += 2);
					while (*base != ',') {
						if (*base == 0) {
							arcan_warning("Warning: arcan_video_renderstring(), couldn't scan font directive '%s' (%s)\n", fontbase, orig);
							*ok = false;
							return failed;
						}
						base++;
					}

					*base = 0; /* now fontbase points to the full filename, wrap that through the resource finder */
					base++;
					numbase = base;

					while (*base != 0 && isdigit(*base))
						base++;

					if (numbase == base)
						arcan_warning("Warning: arcan_video_renderstring(), missing size argument in font specification (%s).\n", orig);
					else {
						char ch = *base;
						*base = 0;

						char* fname = arcan_find_resource(fontbase, ARCAN_RESOURCE_SHARED | ARCAN_RESOURCE_THEME);
						TTF_Font* font = NULL;

						if (!fname){
							arcan_warning("Warning: arcan_video_renderstring(), couldn't find font (%s) (%s)\n", fontbase, orig);
							*ok = false;
							return failed;
						}
						else
							if ((font =
							            grab_font(fname,
							                      strtoul(numbase, NULL, 10))) == NULL){
								arcan_warning("Warning: arcan_video_renderstring(), couldn't open font (%s) (%s)\n", fname, orig);
								free(fname);
								*ok = false;
								return failed;
							}
							else
								prev.font = font;

						free(fname);
						*base = ch;
					}

					/* scan until whitespace or ',' (filename) then if ',' scan to non-number */
					break;
				default:
					arcan_warning("Warning: arcan_video_renderstring(), unknown escape sequence: '\\%c' (%s)\n", *(base+1), orig);
					*ok = false;
					return failed;
			}
		}
	}

	if (*base == 0)
		prev.endofs = base;

//	printf("formatblock(lf:%i, tab:%i, cr:%i)\n", prev.newline, prev.tab, prev.cr);
	*ok = true;
	return prev;
}

/* a */
static int build_textchain(char* message, struct rcell* root, bool sizeonly)
{
	int rv = 0;
	struct text_format* curr_style = &current_context->curr_style;
	curr_style->col.r = curr_style->col.g = curr_style->col.b = 0xff;
	curr_style->style = 0;

	if (message) {
		struct rcell* cnode = root;
		char* current = message;
		char* base = message;
		int msglen = 0;

		/* outer loop, find first split- point */
		while (*current) {
			if (*current == '\\') {
				/* special case */
				if (*(current+1) == '\\') {
					memmove(current, current+1, strlen(current)+1);
					current += 1;
					msglen++;
				}
				else { /* split point found */
					if (msglen > 0) {
						*current = 0;
						/* render surface and slide window */
						if (!curr_style->font) {
							arcan_warning("Warning: arcan_video_renderstring(), no font specified / found.\n");
							return -1;
						}

						if (sizeonly){
							TTF_SetFontStyle(curr_style->font, curr_style->style);
							TTF_SizeUTF8(curr_style->font, base, (int*) &cnode->width, (int*) &cnode->height);
						}
						else{
							cnode->surface = true;
							TTF_SetFontStyle(curr_style->font, curr_style->style);
							cnode->data.surf = TTF_RenderUTF8_Blended(curr_style->font, base, curr_style->col);
							if (!cnode->data.surf)
								arcan_warning("Warning: arcan_video_renderstring(), couldn't render text, possible reason: %s\n", TTF_GetError());
							else
								SDL_SetAlpha(cnode->data.surf, 0, SDL_ALPHA_TRANSPARENT);
						}
						cnode = cnode->next = (struct rcell*) calloc(sizeof(struct rcell), 1);
						*current = '\\';
					}

					bool okstatus;
					*curr_style = formatend(current, *curr_style, message, &okstatus);
					if (!okstatus)
						return -1;

					/* caret modifiers need to be separately chained
					 * to avoid (three?) nasty little basecases */
					if (curr_style->newline || curr_style->tab || curr_style->cr) {
						cnode->surface = false;
						rv += curr_style->newline;
						cnode->data.format.newline = curr_style->newline;
						cnode->data.format.tab = curr_style->tab;
						cnode->data.format.cr = curr_style->cr;
						cnode = cnode->next = (void*) calloc(sizeof(struct rcell), 1);
					}

					current = base = curr_style->endofs;
					if (current == NULL)
						return -1; /* note, may this be a condition for a break rather than a return? */

					msglen = 0;
				}
			}
			else {
				msglen += 1;
				current++;
			}
		}

		/* last element .. */
		if (msglen && curr_style->font) {
			cnode->next = NULL;

			if (sizeonly){
				TTF_SetFontStyle(curr_style->font, curr_style->style);
				TTF_SizeUTF8(curr_style->font, base, (int*) &cnode->width, (int*) &cnode->height);
			}
			else{
				cnode->surface = true;
				TTF_SetFontStyle(curr_style->font, curr_style->style);
				cnode->data.surf = TTF_RenderUTF8_Blended(curr_style->font, base, curr_style->col);
				SDL_SetAlpha(cnode->data.surf, 0, SDL_ALPHA_TRANSPARENT);
			}
		}

		cnode = cnode->next = (void*) calloc(sizeof(struct rcell), 1);
		cnode->data.format.newline = 1;
		rv++;
	}

	return rv;
}

static unsigned int round_mult(unsigned num, unsigned int mult)
{
	if (num == 0 || mult == 0)
		return mult; /* intended ;-) */
	unsigned int remain = num % mult;
	return remain ? num + mult - remain : num;
}

/* tabs are messier still,
 * for each format segment, there may be 'tabc' number of tabsteps,
 * these concern only the current text block and are thus calculated from a fixed offset. */
static unsigned int get_tabofs(int offset, int tabc, int8_t tab_spacing, unsigned int* tabs)
{
	if (!tabs || *tabs == 0) /* tabc will always be >= 1 */
		return tab_spacing ? round_mult(offset, tab_spacing) + ((tabc - 1) * tab_spacing) : offset;

	unsigned int lastofs = offset;

	/* find last matching tab pos first */
	while (*tabs && *tabs < offset)
		lastofs = *tabs++;

	/* matching tab found */
	if (*tabs) {
		offset = *tabs;
		tabc--;
	}

	while (tabc--) {
		if (*tabs)
			offset = *tabs++;
		else
			offset += round_mult(offset, tab_spacing); /* out of defined tabs, pad with default spacing */

	}

	return offset;
}

static void dumptchain(struct rcell* node)
{
	int count = 0;

	while (node) {
		if (node->surface) {
			printf("[%i] image surface\n", count++);
		}
		else {
			printf("[%i] format (%i lines, %i tabs, %i cr)\n", count++, node->data.format.newline, node->data.format.tab, node->data.format.cr);
		}
		node = node->next;
	}
}

void arcan_video_stringdimensions(const char* message, int8_t line_spacing, int8_t tab_spacing, unsigned int* tabs, unsigned int* maxw, unsigned int* maxh)
{
	/* (A) */
	int chainlines;
	struct rcell root = {.surface = false};
	char* work = strdup(message);
	current_context->curr_style.newline = 0;
	current_context->curr_style.tab = 0;
	current_context->curr_style.cr = false;
	
	if ((chainlines = build_textchain(work, &root, true)) > 0) {
		struct rcell* cnode = &root;
		unsigned int linecount = 0;
		bool flushed = false;
		*maxw = 0;
		*maxh = 0;
		
		int lineh = 0;
		int curw = 0;
		int curh = 0;
		
		while (cnode) {
			if (cnode->width > 0) {
				if (cnode->height > lineh + line_spacing)
					lineh = cnode->height;

				curw += cnode->width;
			}
			else {
				if (cnode->data.format.cr)
					curw = 0;
				
				if (cnode->data.format.tab)
					curw = get_tabofs(curw, cnode->data.format.tab, tab_spacing, tabs);
				
				if (cnode->data.format.newline > 0)
					for (int i = cnode->data.format.newline; i > 0; i--) {
						*maxh += lineh + line_spacing;
						lineh = 0;
					}
			}
			
			if (curw > *maxw)
				*maxw = curw;
			
			cnode = cnode->next;
		}
	}

	struct rcell* current = root.next;
	
	while (current){
		struct rcell* prev = current;
		current = current->next;
		prev->next = (void*) 0xdeadbeef;
		free(prev);
	}

	free(work);
}
	
/* note: currently does not obey restrictions placed
 * on texturemode (i.e. everything is padded to power of two and txco hacked) */
arcan_vobj_id arcan_video_renderstring(const char* message, int8_t line_spacing, int8_t tab_spacing, unsigned int* tabs, unsigned int* n_lines, unsigned int** lineheights)
{
	arcan_vobj_id rv = ARCAN_EID;

	/* (A) */
	int chainlines;
	struct rcell* root = calloc( sizeof(struct rcell), 1);
	char* work = strdup(message);
	current_context->curr_style.newline = 0;
	current_context->curr_style.tab = 0;
	current_context->curr_style.cr = false;

	if ((chainlines = build_textchain(work, root, false)) > 0) {
		/* (B) */
		/*		dumptchain(&root); */
		struct rcell* cnode = root;
		unsigned int linecount = 0;
		bool flushed = false;
		int maxw = 0;
		int maxh = 0;
		int lineh = 0;
		int curw = 0;
		int curh = 0;
		/* note, linecount is overflow */
		unsigned int* lines = (unsigned int*) calloc(sizeof(unsigned int), chainlines + 1);
		unsigned int* curr_line = lines;

		while (cnode) {
			if (cnode->surface) {
				assert(cnode->data.surf != NULL);
				if (cnode->data.surf->h > lineh + line_spacing)
					lineh = cnode->data.surf->h;

				curw += cnode->data.surf->w;
			}
			else {
				if (cnode->data.format.cr)
					curw = 0;

				if (cnode->data.format.tab)
					curw = get_tabofs(curw, cnode->data.format.tab, tab_spacing, tabs);

				if (cnode->data.format.newline > 0)
					for (int i = cnode->data.format.newline; i > 0; i--) {
						lines[linecount++] = maxh;
						maxh += lineh + line_spacing;
						lineh = 0;
					}
			}

			if (curw > maxw)
				maxw = curw;

			cnode = cnode->next;
		}
		
		/* (C) */
		/* prepare structures */
		arcan_vobject* vobj = arcan_video_newvobject(&rv);
		if (!vobj){
			arcan_fatal("Fatal: arcan_video_renderstring(), couldn't allocate video object. Out of Memory or out of IDs in current context. There is likely a resource leak in the scripts of the current theme.\n");
		}

		int storw = nexthigher(maxw);
		int storh = nexthigher(maxh);
		vobj->gl_storage.w = storw;
		vobj->gl_storage.h = storh;
		vobj->default_frame.s_raw = storw * storh * 4;
		vobj->default_frame.raw = (uint8_t*) calloc(vobj->default_frame.s_raw, 1);
		vobj->feed.state.tag = ARCAN_TAG_TEXT;
		vobj->blendmode = blend_force;
		vobj->origw = maxw;
		vobj->origh = maxh;
		vobj->parent = &current_context->world;
		glGenTextures(1, &vobj->gl_storage.glid);
		glBindTexture(GL_TEXTURE_2D, vobj->gl_storage.glid);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

		/* find dimensions and cleanup */
		cnode = root;
		curw = 0;
		int yofs = 0;

		SDL_Surface* canvas =
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		    SDL_CreateRGBSurface(SDL_SWSURFACE, storw, storh, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
#else
		    SDL_CreateRGBSurface(SDL_SWSURFACE, storw, storh, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
#endif
		if (canvas == NULL)
			arcan_fatal("Fatal: arcan_video_renderstring(); couldn't build canvas.\n\t Input string is probably unreasonably large wide (len: %zi curw: %i)\n", strlen(message), curw);

		int line = 0;

		while (cnode) {
			if (cnode->surface) {
				SDL_Rect dstrect = {.x = curw, .y = lines[line]};
				SDL_BlitSurface(cnode->data.surf, 0, canvas, &dstrect);
				curw += cnode->data.surf->w;
			}
			else {
				if (cnode->data.format.tab > 0)
					curw = get_tabofs(curw, cnode->data.format.tab, tab_spacing, tabs);

				if (cnode->data.format.cr)
					curw = 0;

				if (cnode->data.format.newline > 0) {
					line += cnode->data.format.newline;
				}
			}
			
			cnode = cnode->next;
		}
	
	/* upload */
		memcpy(vobj->default_frame.raw, canvas->pixels, canvas->w * canvas->h * 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_PIXEL_FORMAT, canvas->w, canvas->h, 0, GL_PIXEL_FORMAT, GL_UNSIGNED_BYTE, vobj->default_frame.raw);
		glBindTexture(GL_TEXTURE_2D, 0);
	
		SDL_FreeSurface(canvas);

		float wv = (float)maxw / (float)vobj->gl_storage.w;
		float hv = (float)maxh / (float)vobj->gl_storage.h;

		generate_basic_mapping(vobj->txcos, wv, hv);
		arcan_video_attachobject(rv);

		if (n_lines)
			*n_lines = linecount;

		if (lineheights)
			*lineheights = lines;
		else
			free(lines);
	}
	
	struct rcell* current;

cleanup:
	current = root;
	while (current){
		assert(current != (void*) 0xdeadbeef);
		if (current->surface && current->data.surf)
			SDL_FreeSurface(current->data.surf);
			
		struct rcell* prev = current;
		current = current->next;
		prev->next = (void*) 0xdeadbeef;
		free(prev);
	}
	
	free(work);

	return rv;
}

arcan_errc arcan_video_forceblend(arcan_vobj_id id, bool on)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		vobj->blendmode = blend_force;

		rv = ARCAN_OK;
	}

	return rv;
}

unsigned short arcan_video_getzv(arcan_vobj_id id)
{
	unsigned short rv = 0;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj){
		rv = vobj->order;
	}
	
	return rv;
}

/* change zval (see arcan_video_addobject) for a particular object.
 * return value is an error code */
arcan_errc arcan_video_setzv(arcan_vobj_id id, unsigned short newzv)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && newzv > 0) {
		vobj->order = newzv;
		arcan_video_detatchobject(id);
		arcan_video_attachobject(id);
		rv = ARCAN_OK;
	}

	return rv;
}

/* forcibly kill videoobject after n cycles,
 * which will reset a counter that upon expiration invocates
 * arcan_video_deleteobject(arcan_vobj_id id)
 */
arcan_errc arcan_video_setlife(arcan_vobj_id id, unsigned lifetime)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		if (lifetime == 0)
			vobj->mask &= ~MASK_LIVING;
		else
			vobj->mask |= MASK_LIVING; /* forcetoggle living */
		
		vobj->lifetime = lifetime;
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_video_zaptransform(arcan_vobj_id id)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);
	if (vobj) {
		surface_transform* current = vobj->transform;
		
		while (current) {
			surface_transform* next = current->next;
			free(current);
			current = next;
		}
		vobj->transform = NULL;
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_video_instanttransform(arcan_vobj_id id){
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj) {
			surface_transform* current = vobj->transform;
			while (current){
				if (current->move.startt){
					vobj->current.position = current->move.endp;
				}
				
				if (current->blend.startt)
					vobj->current.opa = current->blend.endopa;
				
				if (current->rotate.startt)
					vobj->current.rotation = current->rotate.endo;
				
				if (current->scale.startt)
					vobj->current.scale = current->scale.endd;
				
				current = current->next;
			}
			
		arcan_video_zaptransform(id);
	}
	
	return rv;
}

arcan_errc arcan_video_transformcycle(arcan_vobj_id sid, bool flag)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* src = arcan_video_getobject(sid);
	
	if (src)
	{
		src->flags.cycletransform = flag;
		rv = ARCAN_OK;
	}
	
	return rv;
}

arcan_errc arcan_video_copytransform(arcan_vobj_id sid, arcan_vobj_id did)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	arcan_vobject* src, (* dst);

	if (sid == did) 
		rv = ARCAN_ERRC_BAD_ARGUMENT;
	
	src = arcan_video_getobject(sid);
	dst = arcan_video_getobject(did);
	
/* remove what's happening in destination, move pointers from source to dest and done. */
	if (src && dst && src != dst){

		memcpy(&dst->current, &src->current, sizeof(surface_properties)); 

		arcan_video_zaptransform(did);
		dst->transform = dup_chain(src->transform);
		dst->order = src->order;
		dst->origw = src->origw;
		dst->origh = src->origh;
			
		rv = ARCAN_OK;
	}
	
	return rv;
}

arcan_errc arcan_video_transfertransform(arcan_vobj_id sid, arcan_vobj_id did)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	arcan_vobject* src, (* dst);

	if (sid == did) 
		rv = ARCAN_ERRC_BAD_ARGUMENT;
	
	src = arcan_video_getobject(sid);
	dst = arcan_video_getobject(did);
	
/* remove what's happening in destination, move pointers from source to dest and done. */
	if (src && dst && src != dst){
		arcan_video_zaptransform(did);

		memcpy(&dst->current, &src->current, sizeof(surface_properties));
		dst->transform = src->transform;
		src->transform = NULL;
		dst->order = src->order;
		dst->origw = src->origw;
		dst->origh = src->origh;
		
		rv = ARCAN_OK;
	}
	
	return rv;
}

/* removes an object immediately,
 * one of the more complicated / dangerous functions,
 *   -- if there's a feed function, invoke that with destroy
 *   -- if there's a frameset, destroy associated frames IF NOT detatched,
 *   -- if detatched, it is assumed the underlying ffunc mechanism will clean it up.
 *   -- scan all associated objects and remove dangling references AND destroy them too if not masked */
arcan_errc arcan_video_deleteobject(arcan_vobj_id id)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);
	
/* vid 0 is reserved for world, which should never be deleted */
	if (vobj && id > 0) {
		if (vobj->flags.clone == false){
			if (vobj->feed.ffunc){
				vobj->feed.ffunc(ffunc_destroy, 0, 0, 0, 0, 0, 0, vobj->feed.state);
			}
			
			if (vobj->feed.state.tag == ARCAN_TAG_ASYNCIMG)
				SDL_WaitThread( vobj->feed.state.ptr, NULL );

/* for a frameset, index (0) is always reserved for self */
			for (unsigned i = 1; i < vobj->frameset_meta.capacity; i++)
				if (vobj->frameset[i]){
					arcan_video_deleteobject(vobj->frameset[i]->cellid);
					vobj->frameset[i] = 0;
				}

			free(vobj->frameset);
#ifdef _DEBUG
			if (vobj->default_frame.raw)
				memset(vobj->default_frame.raw, 0x10, vobj->default_frame.s_raw); 
#endif
			free(vobj->default_frame.raw);

/* don't keep any dangling referernce */
			vobj->current_frame = vobj;
			glDeleteTextures(1, &vobj->gl_storage.glid);
		}

		current_context->nalive--;
		assert(current_context->nalive >= 0);

/* no effect if already detatched */
		arcan_video_detatchobject(id);

/* FIXME: scan all rendertargets for the id as well */

/* scan the current context, look for clones and other objects that has this object as a parent */
		arcan_vobject_litem* current;
retry:  
		current = current_context->stdoutp.first;
		
		while (current && current->elem) {
			arcan_vobject* elem = current->elem;
		
			if (elem->parent == vobj) {
/* if we have a child or a linked object that isn't masked from being deleted, cascade */
				if (elem->flags.clone || (elem->mask & MASK_LIVING) == 0) {
					arcan_video_deleteobject(current->elem->cellid);
					goto retry; /* no guarantee the structure is intact */
				}
/* otherwise, just inherit this objects parent (eventually, WORLDID) */
				else 
					elem->parent = vobj->parent;
			}

			current = current->next;
		}

/* transforms are also dynamically allocated, so clean and reset */
		arcan_video_zaptransform(id);

/* lots of default values are assumed to be 0, so reset the entire object to be sure.
 * will help leak detectors as well */ 
		memset(vobj, 0, sizeof(arcan_vobject));
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_video_override_mapping(arcan_vobj_id id, float* newmapping)
{
	arcan_vobject* vobj = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;

	if (vobj && id > 0) {
		float* fv = vobj->txcos;
		memcpy(vobj->txcos, newmapping, sizeof(float) * 8);
		rv = ARCAN_OK;
	}

	return rv;
}

arcan_errc arcan_video_retrieve_mapping(arcan_vobj_id id, float* dst)
{
	arcan_vobject* vobj = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	
	if (vobj && dst && id > 0) {
		memcpy(dst, vobj->txcos, sizeof(float) * 8);
		rv = ARCAN_OK;
	}
	
	return rv;
}

arcan_vobj_id arcan_video_findparent(arcan_vobj_id id)
{
	arcan_vobj_id rv = ARCAN_EID;
	arcan_vobject* vobj = arcan_video_getobject(id);
	
		if (vobj){
			rv = id;

			if (vobj->parent && vobj->parent->owner) {
				rv = vobj->parent->cellid;
			}
		}
		
	return rv;
}

arcan_vobj_id arcan_video_findchild(arcan_vobj_id parentid, unsigned ofs)
{
	arcan_vobj_id rv = ARCAN_EID;
    arcan_vobject* vobj = arcan_video_getobject(parentid);
    
    if (!vobj)
        return rv;
    
    arcan_vobject_litem* current = current_context->stdoutp.first;
    
    while (current && current->elem) {
        arcan_vobject* elem = current->elem;
        arcan_vobject** frameset = elem->frameset;
		
		/* how to deal with those that inherit? */
        if (elem->parent == vobj) {
            if (ofs > 0) 
                ofs--;
            else{
                rv = elem->cellid;
                return rv;
            }
        }
        
        current = current->next;
    }

	return rv;
}

arcan_errc arcan_video_objectrotate(arcan_vobj_id id, float roll, float pitch, float yaw, unsigned int tv)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj) {
		rv = ARCAN_OK;
		
		/* clear chains for rotate attribute
		 * if time is set to ovverride and be immediate */
		if (tv == 0) {
			swipe_chain(vobj->transform, offsetof(surface_transform, rotate), sizeof(struct transf_rotate));
			vobj->current.rotation.roll = roll;
			vobj->current.rotation.pitch = pitch;
			vobj->current.rotation.yaw = yaw;
			vobj->current.rotation.quaternion = build_quat_euler(roll, pitch, yaw);
		}
		else { /* find endpoint to attach at */
			surface_orientation bv = vobj->current.rotation;

			surface_transform* base = vobj->transform;
			surface_transform* last = base;

			/* figure out the starting angle */
			while (base && base->rotate.startt) {
				if (!base->next)
					bv = base->rotate.endo;

				last = base;
				base = base->next;
			}

			if (!base){
				if (last)
					base = last->next = (surface_transform*) calloc(sizeof(surface_transform), 1);
				else
					base = last = (surface_transform*) calloc(sizeof(surface_transform), 1);
			}

			if (!vobj->transform)
				vobj->transform = base;
			
			base->rotate.startt = last->rotate.endt < arcan_video_display.c_ticks ? arcan_video_display.c_ticks : last->rotate.endt;
			base->rotate.endt   = base->rotate.startt + tv;
			base->rotate.starto = bv;
			base->rotate.endo.roll = roll;
			base->rotate.endo.pitch = pitch;
			base->rotate.endo.yaw = yaw;
			base->rotate.endo.quaternion = build_quat_euler(roll, pitch, yaw);
			base->rotate.interp = interpolate_linear;
		}
	}

	return rv;
}

/* alter object opacity, range 0..1 */
arcan_errc arcan_video_objectopacity(arcan_vobj_id id, float opa, unsigned int tv)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);
	opa = CLAMP(opa, 0.0, 1.0);
	
	if (vobj) {
		rv = ARCAN_OK;

		/* clear chains for rotate attribute
		 * if time is set to ovverride and be immediate */
		if (tv == 0) {
			swipe_chain(vobj->transform, offsetof(surface_transform, blend), sizeof(struct transf_blend));
			vobj->current.opa = opa;
		}
		else { /* find endpoint to attach at */
			float bv = vobj->current.opa;

			surface_transform* base = vobj->transform;
			surface_transform* last = base;

			while (base && base->blend.startt) {
				bv = base->blend.endopa;
				last = base;
				base = base->next;
			}

			if (!base){
				if (last)
					base = last->next = (surface_transform*) calloc(sizeof(surface_transform), 1);
				else
					base = last = (surface_transform*) calloc(sizeof(surface_transform), 1);
			}
			
			if (!vobj->transform)
				vobj->transform = base;

			base->blend.startt = last->blend.endt < arcan_video_display.c_ticks ? arcan_video_display.c_ticks : last->blend.endt;
			base->blend.endt = base->blend.startt + tv;
			base->blend.startopa = bv;
			base->blend.endopa = opa + 0.0000000001;
			base->blend.interp = interpolate_linear;
		}
	}

	return rv;
}

/* linear transition from current position to a new desired position,
 * if time is 0 the move will be instantaneous (and not generate an event)
 * otherwise time denotes how many ticks it should take to move the object
 * from its start position to it's final. An event will in this case be generated */
arcan_errc arcan_video_objectmove(arcan_vobj_id id, float newx, float newy, float newz, unsigned int tv)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj) {
		rv = ARCAN_OK;

		/* clear chains for rotate attribute
		 * if time is set to ovverride and be immediate */
		if (tv == 0) {
			swipe_chain(vobj->transform, offsetof(surface_transform, move), sizeof(struct transf_move));
			vobj->current.position.x = newx;
			vobj->current.position.y = newy;
			vobj->current.position.z = newz;
		}
		else { /* find endpoint to attach at */
			surface_transform* base = vobj->transform;
			surface_transform* last = base;

			/* figure out the coordinates which the transformation is chained to */
			point bwp = vobj->current.position;
			
			while (base && base->move.startt) {
				bwp = base->move.endp;
				 
				last = base;
				base = base->next;
			}

			if (!base){
				if (last)
					base = last->next = (surface_transform*) calloc(sizeof(surface_transform), 1);
				else
					base = last = (surface_transform*) calloc(sizeof(surface_transform), 1);
			}
			
			point newp = {newx, newy, newz};

			if (!vobj->transform)
				vobj->transform = base;
			
			base->move.startt = last->move.endt < arcan_video_display.c_ticks ? arcan_video_display.c_ticks : last->move.endt;
			base->move.endt   = base->move.startt + tv;
			base->move.interp = interpolate_linear;
			base->move.startp = bwp;
			base->move.endp   = newp;
		}
	}

	return rv;
}

/* scale the video object to match neww and newh, with stepx or stepy at 0 it will be instantaneous,
 * otherwise it will move at stepx % of delta-size each tick
 * return value is an errorcode, run through char* arcan_verror(int8_t) */
arcan_errc arcan_video_objectscale(arcan_vobj_id id, float wf, float hf, float df, unsigned tv)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj) {
		const int immediately = 0;
		rv = ARCAN_OK;

		if (tv == immediately) {
			swipe_chain(vobj->transform, offsetof(surface_transform, scale), sizeof(struct transf_scale));

			vobj->current.scale.x = wf;
			vobj->current.scale.y = hf;
			vobj->current.scale.z = df;
		}
		else {
			surface_transform* base = vobj->transform;
			surface_transform* last = base;

			/* figure out the coordinates which the transformation is chained to */
			scalefactor bs = vobj->current.scale;

			while (base && base->scale.startt) {
				bs = base->scale.endd;
				
				last = base;
				base = base->next;
			}

			if (!base){
				if (last)
					base = last->next = (surface_transform*) calloc(sizeof(surface_transform), 1);
				else
					base = last = (surface_transform*) calloc(sizeof(surface_transform), 1);
			}

			if (!vobj->transform)
				vobj->transform = base;
			
			base->scale.startt = last->scale.endt < arcan_video_display.c_ticks ? arcan_video_display.c_ticks : last->scale.endt;
			base->scale.endt = base->scale.startt + tv;
			base->scale.interp = interpolate_linear;
			base->scale.startd = bs;
			base->scale.endd.x = wf;
			base->scale.endd.y = hf;
			base->scale.endd.z = df;
		}
	}

	return rv;
}

/* called whenever a cell in update has a time that reaches 0 */
static void compact_transformation(arcan_vobject* base, unsigned int ofs, unsigned int count)
{
	if (!base || !base->transform) return;
	
	surface_transform* last = NULL;
	surface_transform* work = base->transform;
	/* copy the next transformation */
	
	while (work && work->next) {
		assert(work != work->next);
		memcpy((void*)(work) + ofs, (void*)(work->next) + ofs, count);
		last = work;
		work = work->next;
	}

	/* reset the last one */
	memset((void*) work + ofs, 0, count);

	/* if it is now empty, free and delink */
	if (!(work->blend.startt |
	          work->scale.startt |
	          work->move.startt |
	          work->rotate.startt )
	   )	{
		free(work);
		if (last)
			last->next = NULL;
		else
			base->transform = NULL;
	}
}

arcan_errc arcan_video_setprogram(arcan_vobj_id id, arcan_shader_id shid)
{
	arcan_vobject* vobj = arcan_video_getobject(id);
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;

	if (vobj && vobj->flags.clone == true)
		rv = ARCAN_ERRC_CLONE_NOT_PERMITTED;
	else if (vobj && id >= 0) {
		vobj->gl_storage.program = shid;
		rv = ARCAN_OK;
	}

	return rv;
}

static bool update_object(arcan_vobject* ci, unsigned int stamp)
{
	bool upd = false;
	
	if (ci->last_updated == stamp)
		return false;
	
/* update parent if this has not already been updated this cycle */
	if (ci->last_updated < stamp && 
		ci->parent && ci->parent != &current_context->world && 
		ci->parent->last_updated != stamp){
		update_object(ci->parent, stamp);
	}

	if (!ci->transform)
		return false;
	
	ci->last_updated = stamp;

	if (ci->transform->blend.startt) {
		upd = true;
		float fract = lerp_fract(ci->transform->blend.startt, ci->transform->blend.endt, stamp);
		ci->current.opa = lerp_val(ci->transform->blend.startopa, ci->transform->blend.endopa, fract);

		if (fract > 0.9999f) {
			ci->current.opa = ci->transform->blend.endopa;
			
			if (ci->flags.cycletransform){
				arcan_video_objectopacity(ci->cellid, ci->transform->blend.endopa, ci->transform->blend.endt - ci->transform->blend.startt);
			}
			
			compact_transformation(ci,
			                       offsetof(surface_transform, blend),
			                       sizeof(struct transf_blend));
			
/* only fire event if we've run out of the transform chain for the current value */
			if (!ci->transform || ci->transform->blend.startt == 0) {
				arcan_event ev = {.category = EVENT_VIDEO, .kind = EVENT_VIDEO_BLENDED};
				ev.data.video.source = ci->cellid;
				arcan_event_enqueue(arcan_event_defaultctx(), &ev);
			}
		}
	}

	if (ci->transform && ci->transform->move.startt) {
		upd = true;
		float fract = lerp_fract(ci->transform->move.startt, ci->transform->move.endt, stamp);
		ci->current.position = lerp_vector(ci->transform->move.startp, ci->transform->move.endp, fract);
		
		if (fract > 0.9999f) {
			ci->current.position = ci->transform->move.endp;
			
			if (ci->flags.cycletransform)
				arcan_video_objectmove(ci->cellid, 
					 ci->transform->move.endp.x,
					 ci->transform->move.endp.y,
					 ci->transform->move.endp.z,
					 ci->transform->move.endt - ci->transform->move.startt);
			
			compact_transformation(ci,
								   offsetof(surface_transform, move),
								   sizeof(struct transf_move));
			
			if (!ci->transform || ci->transform->move.startt == 0) {
				arcan_event ev = {.category = EVENT_VIDEO, .kind = EVENT_VIDEO_MOVED};
				ev.data.video.source = ci->cellid;
				arcan_event_enqueue(arcan_event_defaultctx(), &ev);
			}
		}
	}

	if (ci->transform && ci->transform->scale.startt) {
		upd = true;
		float fract = lerp_fract(ci->transform->scale.startt, ci->transform->scale.endt, stamp);
		ci->current.scale = lerp_vector(ci->transform->scale.startd, ci->transform->scale.endd, fract);
		if (fract > 0.9999f) {
			ci->current.scale = ci->transform->scale.endd;

			if (ci->flags.cycletransform)
				arcan_video_objectscale(ci->cellid, ci->transform->scale.endd.x,
																ci->transform->scale.endd.y,
																ci->transform->scale.endd.z,
																ci->transform->scale.endt - ci->transform->scale.startt);
			
			compact_transformation(ci,
								   offsetof(surface_transform, scale),
								   sizeof(struct transf_scale));
			if (!ci->transform || ci->transform->scale.startt == 0) {
				arcan_event ev = {.category = EVENT_VIDEO, .kind = EVENT_VIDEO_SCALED};
				ev.data.video.source = ci->cellid;
				arcan_event_enqueue(arcan_event_defaultctx(), &ev);
			}
		}
	}
	
	if (ci->transform && ci->transform->rotate.startt) {
		upd = true;
		float fract = lerp_fract(ci->transform->rotate.startt, ci->transform->rotate.endt, stamp);
		ci->current.rotation.quaternion = nlerp_quat(ci->transform->rotate.starto.quaternion, ci->transform->rotate.endo.quaternion, fract);
		
		if (fract > 0.9999f) {
			ci->current.rotation = ci->transform->rotate.endo;
			if (ci->flags.cycletransform)
				arcan_video_objectrotate(ci->cellid,
																 ci->transform->rotate.endo.roll,
																 ci->transform->rotate.endo.pitch,
																 ci->transform->rotate.endo.yaw,
																 ci->transform->rotate.endt - ci->transform->rotate.startt);
			
			compact_transformation(ci,
								   offsetof(surface_transform, rotate),
								   sizeof(struct transf_rotate));
			
			if (!ci->transform || ci->transform->rotate.startt == 0) {
				arcan_event ev = {.category = EVENT_VIDEO, .kind = EVENT_VIDEO_ROTATED};
				ev.data.video.source = ci->cellid;
				arcan_event_enqueue(arcan_event_defaultctx(), &ev);
			}
		}
	}
	
	return upd;
}

/* process a logical time-frame (which more or less means, update / rescale / redraw / flip)
 * returns msecs elapsed */
uint32_t arcan_video_tick(uint8_t steps)
{
	unsigned now = arcan_frametime();
	arcan_vobject_litem* current = current_context->stdoutp.first;

	while (steps--) {
/*		printf("(%d) alive: %ld\n", arcan_video_display.c_ticks, current_context->nalive); */
		update_object(&current_context->world, arcan_video_display.c_ticks);
		arcan_video_display.c_ticks++;
		arcan_shader_envv(TIMESTAMP_D, &arcan_video_display.c_ticks, sizeof(arcan_video_display.c_ticks));
		
		if (current)
			do {
				arcan_vobject* elem = current->elem;
			
				/* is the item to be updated? */
				update_object(elem, arcan_video_display.c_ticks);
				if (elem->feed.ffunc)
					elem->feed.ffunc(ffunc_tick, 0, 0, 0, 0, 0, 0, elem->feed.state);
				
				if ((elem->mask & MASK_LIVING) > 0) {
					if (elem->lifetime <= 0) {
						arcan_event dobjev = {
							.category = EVENT_VIDEO,
							.kind = EVENT_VIDEO_EXPIRE
						};
						arcan_vobj_id tid = elem->cellid;
						dobjev.data.video.source = tid;

						arcan_event_enqueue(arcan_event_defaultctx(), &dobjev);
						/* disable the LIVING mask, otherwise we'd fire multiple
						 * expire events when video logic is lagging behind */
						elem->mask &= ~MASK_LIVING;
					}
					else {
						elem->lifetime--;
					}
				}
			
/* mode > 0, cycle every 'n' ticks */
				if (elem->frameset_meta.mode > 0){
					elem->frameset_meta.counter--;
					if (elem->frameset_meta.counter == 0){
						elem->frameset_meta.counter = abs( elem->frameset_meta.mode );
						step_active_frame(elem);
					}
				}

/* sweep the entire list, any detatched objects won't ever get this update (frameservers, ...) */
			}
			while ((current = current->next) != NULL);

		current = current_context->stdoutp.first;
	}

	return 0;
}

arcan_errc arcan_video_setclip(arcan_vobj_id id, bool toggleon)
{
	arcan_errc rv = ARCAN_ERRC_NO_SUCH_OBJECT;
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj){
		vobj->flags.cliptoparent = toggleon;
		rv = ARCAN_OK;
	}
	
	return rv;
}

bool arcan_video_visible(arcan_vobj_id id)
{
	bool rv = false;
	arcan_vobject* vobj= arcan_video_getobject(id);

	if (vobj && id > 0)
		return vobj->current.opa > 0.001;

	return rv;
}

/* take sprops, apply them to the coordinates in vobj with proper masking (or force to ignore mask), store the results in dprops */
static void apply(arcan_vobject* vobj, surface_properties* dprops, float lerp, surface_properties* sprops, bool force)
{
	*dprops = vobj->current;
	
/* apply within own dimensions */
	if (vobj->transform){
		surface_transform* tf = vobj->transform;
		unsigned ct = arcan_video_display.c_ticks;
		
		if (tf->move.startt)
			dprops->position = lerp_vector(tf->move.startp,
				tf->move.endp,
				lerp_fract(tf->move.startt, tf->move.endt, (float)ct + lerp));
	
		if (tf->scale.startt)
			dprops->scale = lerp_vector(tf->scale.startd, tf->scale.endd, lerp_fract(tf->scale.startt, tf->scale.endt, (float)ct + lerp));

		if (tf->blend.startt)
			dprops->opa = lerp_val(tf->blend.startopa, tf->blend.endopa, lerp_fract(tf->blend.startt, tf->blend.endt, (float)ct + lerp));

		if (tf->rotate.startt)
			dprops->rotation.quaternion = nlerp_quat(tf->rotate.starto.quaternion, tf->rotate.endo.quaternion, lerp_fract(tf->rotate.startt, tf->rotate.endt, (float)ct + lerp));
	
		if (!sprops)
			return;
	}
	
/* translate to sprops */
	if (force || (vobj->mask & MASK_POSITION) > 0)
		dprops->position = add_vector(dprops->position, sprops->position);
	
	if (force || (vobj->mask & MASK_ORIENTATION) > 0){
		dprops->rotation.yaw   += dprops->rotation.yaw;
		dprops->rotation.pitch += dprops->rotation.pitch;
		dprops->rotation.roll  += dprops->rotation.roll;
		dprops->rotation.quaternion = add_quat(dprops->rotation.quaternion, sprops->rotation.quaternion);
	}
	
	if (force || (vobj->mask & MASK_OPACITY) > 0){
		dprops->opa *= sprops->opa;
	}
	
/*	if (force || (vobj->mask & MASK_SCALE) > 0){
		dprops->scale = mul_vector(dprops->scale, sprops->scale);
	} */
}

/* this is really grounds for some more elaborate caching strategy if CPU- bound.
 * using some frame- specific tag so that we don't repeatedly resolve with this complexity. */
void arcan_resolve_vidprop(arcan_vobject* vobj, float lerp, surface_properties* props)
{
	if (vobj->parent != &current_context->world){
		surface_properties dprop = {0};
		arcan_resolve_vidprop(vobj->parent, lerp, &dprop);
		apply(vobj, props, lerp, &dprop, false);
	} 
	else{
		apply(vobj, props, lerp, &current_context->world.current, true);
	}
}

static inline void draw_vobj(float x, float y, float x2, float y2, float zv, float* txcos)
{
	GLfloat verts[] = { x,y, x2,y, x2,y2, x,y2 };

	GLint attrindv = arcan_shader_vattribute_loc(ATTRIBUTE_VERTEX);
	GLint attrindt = arcan_shader_vattribute_loc(ATTRIBUTE_TEXCORD);

	if (attrindv != -1){
		glEnableVertexAttribArray(attrindv);
		glVertexAttribPointer(attrindv, 2, GL_FLOAT, GL_FALSE, 0, verts);

		if (attrindt != -1){
			glEnableVertexAttribArray(attrindt);
			glVertexAttribPointer(attrindt, 2, GL_FLOAT, GL_FALSE, 0, txcos);
		}

		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

		if (attrindt != -1)
			glDisableVertexAttribArray(attrindt);
		
		glDisableVertexAttribArray(attrindv);
	}
}

static inline void draw_surf(surface_properties prop, arcan_vobject* src, float* txcos)
{
    if (src->feed.state.tag == ARCAN_TAG_ASYNCIMG)
        return;
    
	float omatr[16], imatr[16], dmatr[16];
	prop.scale.x *= src->origw * 0.5;
	prop.scale.y *= src->origh * 0.5;

	identity_matrix(imatr);
	translate_matrix(imatr, prop.position.x + prop.scale.x, prop.position.y + prop.scale.y, 0.0);
	matr_quatf(norm_quat (prop.rotation.quaternion), omatr);
	multiply_matrix(dmatr, imatr, omatr);
	
	arcan_shader_envv(MODELVIEW_MATR, dmatr, sizeof(float) * 16);
	arcan_shader_envv(OBJ_OPACITY, &prop.opa, sizeof(float));

	draw_vobj(-prop.scale.x, -prop.scale.y, prop.scale.x, prop.scale.y, 0, txcos);
}

/* scan all 'feed objects' (possible optimization, keep these tracked in a separate list and run prior to all other rendering,
 * might gain something when other pseudo-asynchronous operations (e.g. PBO) are concerned */
void arcan_video_pollfeed()
{
	arcan_vobject_litem* current = current_context->stdoutp.first;

	while(current && current->elem){
	arcan_vobject* cframe = current->elem->current_frame;
	arcan_vobject* celem  = current->elem;

/* if there's a feed function, try and grab a new sample and upload,
 * make sure that we use the current elements "feed function", but set the target
 * to its current active frame, most of the time, they are the same */
		if ( celem->feed.ffunc &&
		celem->feed.ffunc(ffunc_poll, 0, 0, 0, 0, 0, 0, celem->feed.state) == FFUNC_RV_GOTFRAME) {

			/* cycle active frame */
			if (celem->frameset_meta.mode < 0){
				celem->frameset_meta.counter--;
				
				if (celem->frameset_meta.counter == 0){
					celem->frameset_meta.counter = abs( celem->frameset_meta.mode );
					step_active_frame(celem);
					cframe = celem->current_frame;
				}
			}
			
			enum arcan_ffunc_rv funcres = celem->feed.ffunc(ffunc_render,
			cframe->default_frame.raw, cframe->default_frame.s_raw,
			cframe->gl_storage.w, cframe->gl_storage.h, cframe->gl_storage.ncpt,
			cframe->gl_storage.glid,
			celem->feed.state);
			
/* special "hack" for situations where the ffunc can do the gl-calls without an additional memtransfer (some video/targets, particularly in no POW2 Textures) */
			if (funcres == FFUNC_RV_COPIED){
				glBindTexture(GL_TEXTURE_2D, cframe->gl_storage.glid);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cframe->gl_storage.w, cframe->gl_storage.h, GL_PIXEL_FORMAT, GL_UNSIGNED_BYTE, cframe->default_frame.raw);
			}
		}

		current = current->next;
	}
}

static void process_rendertarget(struct rendertarget* tgt, float lerp)
{
	arcan_vobject* world = &current_context->world;
	arcan_vobject_litem* current = tgt->first;
	glClear(GL_COLOR_BUFFER_BIT);

	arcan_debug_pumpglwarnings("refreshGL:pre3d");

	/* first, handle all 3d work (which may require multiple passes etc.) */
	if (!arcan_video_display.late3d && current && current->elem->order < 0){
		current = arcan_refresh_3d(0, current, lerp, 0);
	}

/* skip a possible 3d pipeline */
	while (current && current->elem->order < 0) 
		current = current->next;
	
	arcan_debug_pumpglwarnings("refreshGL:pre2d");
	
	if (current){
	/* make sure we're in a decent state for 2D */
		glClientActiveTexture(GL_TEXTURE0);
		glDisable(GL_DEPTH_TEST);

		memcpy(arcan_video_display.projmatr, ortho_proj, sizeof(float) * 16);
		arcan_shader_activate(arcan_video_display.defaultshdr);
		arcan_shader_envv(PROJECTION_MATR, arcan_video_display.projmatr, sizeof(float)*16);
		arcan_shader_envv(FRACT_TIMESTAMP_F, &lerp, sizeof(float));
		glScissor(0, 0, arcan_video_display.width, arcan_video_display.height);
		
		while (current && current->elem->order >= 0){
#ifdef _DEBUG
			char cvid[24];
			snprintf(cvid, 24, "refreshGL:2d(%d)", (unsigned) current->elem->cellid);
			if (arcan_debug_pumpglwarnings(cvid) == -1){
				arcan_warning("fatal: GL error detected, check dump.\n");
				abort();
			};
#endif
	
			arcan_vobject* elem = current->elem;
			surface_properties* csurf = &elem->current;

			assert(elem->parent != NULL);
			/* calculate coordinate system translations, 
			* world cannot be masked */
			surface_properties dprops = {0};
			arcan_resolve_vidprop(elem, lerp, &dprops);
            
/* don't waste time on objects that aren't supposed to be visible */
			if ( dprops.opa < EPSILON){
				current = current->next;
				continue;
			}

/* enable clipping if used */
			bool clipped = false;
			if (elem->flags.cliptoparent && elem->parent != &current_context->world){
/* toggle stenciling, reset into zero, draw parent bounding area to stencil only,
 * redraw parent into stencil, draw new object then disable stencil. */
				clipped = true;
				glEnable(GL_STENCIL_TEST);
				glClearStencil(0);
				glClear(GL_STENCIL_BUFFER_BIT);
				glColorMask(0, 0, 0, 0);
				glStencilFunc(GL_ALWAYS, 1, 1);
				glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

/* switch to default shader as we don't want any fancy vertex processing interfering with clipping */
				arcan_shader_activate(arcan_video_display.defaultshdr);
				arcan_vobject* celem = elem;

/* since we can have hierarchies of partially clipped, we may need to resolve all */
				while (celem->parent != &current_context->world){
					surface_properties pprops = {0};
					arcan_resolve_vidprop(celem->parent, lerp, &pprops);
					if (celem->parent->flags.cliptoparent == false)
						draw_surf(pprops, celem->parent, elem->current_frame->txcos);

					celem = celem->parent;
				}

				glColorMask(1, 1, 1, 1);
				glStencilFunc(GL_EQUAL, 1, 1);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			}

			arcan_shader_activate( elem->gl_storage.program > 0 ? elem->gl_storage.program : arcan_video_display.defaultshdr );

/* depending on frameset- mode, we may need to split the frameset up into multitexturing */
		if (elem->frameset_meta.counter > 0 && elem->frameset_meta.framemode == ARCAN_FRAMESET_MULTITEXTURE){
				unsigned j = GL_MAX_TEXTURE_UNITS < elem->frameset_meta.capacity ? GL_MAX_TEXTURE_UNITS : elem->frameset_meta.capacity;
				for(unsigned i = 0;i < j; i++){
					char unifbuf[9] = {0};
					int frameind = (elem->frameset_meta.current + i) % elem->frameset_meta.capacity;
					if (elem->frameset[frameind] == NULL){
#ifdef _DEBUG
						arcan_warning("arcan_video_refresh_GL( MULTITEXTURE ) -- unmapped cell (%d) ignored.\n", frameind);
#endif
						continue;
					}
					glActiveTexture(GL_TEXTURE0 + i);
					glEnable(GL_TEXTURE_2D);
					glBindTexture(GL_TEXTURE_2D, elem->frameset[ frameind ]->gl_storage.glid);
					snprintf(unifbuf, 8, "map_tu%d", i);
					arcan_shader_forceunif(unifbuf, shdrint, &i, false);
				}
		}
			else
				glBindTexture(GL_TEXTURE_2D, elem->current_frame->gl_storage.glid);

/* only blend if the object isn't entirely solid or if the object has specific settings */
			if (dprops.opa > 0.99999 && ( elem->blendmode != blend_force ))
				glDisable(GL_BLEND);
			else
				glEnable(GL_BLEND);

			draw_surf(dprops, elem, elem->current_frame->txcos);

			if (clipped)
				glDisable(GL_STENCIL_TEST);

			current = current->next;
		}
	}

/* reset and try the 3d part again if requested */
	current = tgt->first;
	if (arcan_video_display.late3d && current && current->elem->order < 0){
		current = arcan_refresh_3d(0, current, lerp, 0);
	}
}

/* assumes working ortographic projection matrix based on current resolution,
 * redraw the entire scene and linearly interpolate transformations */
static void arcan_debug_curfbostatus(GLenum status, enum rendertarget_mode);
void arcan_video_refresh_GL(float lerp)
{
	static bool nofbo = false;
	
/* for performance reasons, we should try and re-use FBOs whenever possible */
	if (nofbo == false){
		for (off_t ind = 0; ind < current_context->n_rtargets; ind++){
			struct rendertarget* tgt = &current_context->rtargets[ind];
		
			glBindFramebuffer(GL_FRAMEBUFFER, tgt->fbo);

			if (tgt->mode == RENDERTARGET_DEPTH){
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, tgt->depth, 0);
/* unsure if these ONLY EVER apply to the active FBO, assume that they do. */
					glDrawBuffer(GL_NONE);
					glReadBuffer(GL_NONE);
			}
			else { /* RENDERTARGET_COLOR */
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tgt->color->gl_storage.glid, 0);
				
				if (tgt->mode > RENDERTARGET_COLOR)
					glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tgt->depth);
	
				if (tgt->mode > RENDERTARGET_COLOR_DEPTH)
					glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, tgt->depth);
			}
			
			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status == GL_FRAMEBUFFER_COMPLETE){
				process_rendertarget(tgt, lerp);
			}
			else {
				nofbo = true;
				arcan_warning("Error using rendertarget(FBO), feature disabled.\n");
				arcan_debug_curfbostatus(status, tgt->mode);
			}
		}
	
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	process_rendertarget(&current_context->stdoutp, lerp);
	
/* now all PBOs should be finished, push them to their respective buffers */
}

void arcan_video_refresh(float tofs)
{
	arcan_video_refresh_GL(tofs);
	SDL_GL_SwapBuffers();
}

void arcan_video_default_scalemode(enum arcan_vimage_mode newmode)
{
	arcan_video_display.scalemode = newmode;
}

void arcan_video_default_texmode(enum arcan_vtex_mode modes, enum arcan_vtex_mode modet)
{
	arcan_video_display.deftxs = modes == ARCAN_VTEX_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
	arcan_video_display.deftxt = modet == ARCAN_VTEX_REPEAT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
}

bool arcan_video_hittest(arcan_vobj_id id, unsigned int x, unsigned int y)
{
	arcan_vobject* vobj = arcan_video_getobject(id);
	if (vobj){
/* get object properties taking inheritance etc. into account */
		surface_properties dprops = {0};
		arcan_resolve_vidprop(vobj, 0.0, &dprops);
		dprops.scale.x *= vobj->origw * 0.5;
		dprops.scale.y *= vobj->origh * 0.5;
		
/* transform and rotate the bounding coordinates into screen space */
		float omatr[16], imatr[16], dmatr[16];
		int view[4] = {0, 0, arcan_video_display.width, arcan_video_display.height};

		identity_matrix(imatr);
		matr_quatf(dprops.rotation.quaternion, omatr);
		translate_matrix(imatr, dprops.position.x + dprops.scale.x, dprops.position.y + dprops.scale.y, 0.0);
		multiply_matrix(dmatr, omatr, imatr);
		
		float p[4][3];

		/* unproject all 4 vertices, usually very costly but for 4 vertices it's manageable */
		project_matrix(-dprops.scale.x, -dprops.scale.y, 0.0, dmatr, arcan_video_display.projmatr, view, &p[0][0], &p[0][1], &p[0][2]);
		project_matrix( dprops.scale.x, -dprops.scale.y, 0.0, dmatr, arcan_video_display.projmatr, view, &p[1][0], &p[1][1], &p[1][2]);
		project_matrix( dprops.scale.x,  dprops.scale.y, 0.0, dmatr, arcan_video_display.projmatr, view, &p[2][0], &p[2][1], &p[2][2]);
		project_matrix(-dprops.scale.x,  dprops.scale.y, 0.0, dmatr, arcan_video_display.projmatr, view, &p[3][0], &p[3][1], &p[3][2]);

		float px[4], py[4];
		px[0] = p[0][0]; px[1] = p[1][0]; px[2] = p[2][0]; px[3] = p[3][0];
		py[0] = p[0][1]; py[1] = p[1][1]; py[2] = p[2][1]; py[3] = p[3][1];
			
		/* now we have a convex n-gone poly (0 -> 1 -> 2 -> 0) */
		return pinpoly(4, px, py, (float) x, (float) arcan_video_display.height - y);
	}
	
	return false;
}

unsigned int arcan_video_pick(arcan_vobj_id* dst, unsigned int count, int x, int y)
{
	if (count == 0)
		return 0;

	arcan_vobject_litem* current = current_context->stdoutp.first;
	uint32_t base = 0;

	while (current && base < count) {
		if (current->elem->cellid && !(current->elem->mask & MASK_UNPICKABLE) && current->elem->current.opa > EPSILON && arcan_video_hittest(current->elem->cellid, x, y))
			dst[base++] = current->elem->cellid;
		current = current->next;
	}

	return base;
}

/* just a wrapper for 'style' */
img_cons arcan_video_dimensions(uint16_t w, uint16_t h)
{
	img_cons res = {w, h};
	return res;
}

void arcan_video_dumppipe()
{
	arcan_vobject_litem* current = current_context->stdoutp.first;
	uint32_t count = 0;
	printf("-----------\n");
	if (current)
		do {
			printf("[%i] #(%i) - (ID:%u) (Order:%i) (Dimensions: %f, %f - %f, %f) (Opacity:%f)\n", current->elem->flags.in_use, count++, (unsigned) current->elem->cellid, current->elem->order,
			       current->elem->current.position.x, current->elem->current.position.y, current->elem->current.scale.x, current->elem->current.scale.y, current->elem->current.opa);
		}
		while ((current = current->next) != NULL);
	printf("-----------\n");
}

/* the actual storage dimensions,
 * as these might concern "% 2" texture requirement */
img_cons arcan_video_storage_properties(arcan_vobj_id id)
{
	img_cons res = {.w = 0, .h = 0, .bpp = 0};
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		res.w = vobj->gl_storage.w;
		res.h = vobj->gl_storage.h;
		res.bpp = vobj->gl_storage.ncpt;
	}

	return res;
}

/* image dimensions at load time, without
 * any transformations being applied */
surface_properties arcan_video_initial_properties(arcan_vobj_id id)
{
	surface_properties res = {0};
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		res.scale.x = vobj->origw;
		res.scale.y = vobj->origh;
	}

	return res;
}

surface_properties arcan_video_resolve_properties(arcan_vobj_id id)
{
	surface_properties res = {0};
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj && id > 0) {
		arcan_resolve_vidprop(vobj, 0.0, &res);
		res.scale.x *= vobj->origw;
		res.scale.y *= vobj->origh;
	}

	return res;
}

surface_properties arcan_video_current_properties(arcan_vobj_id id)
{
	surface_properties rv = {0};
	arcan_vobject* vobj = arcan_video_getobject(id);

	if (vobj){
		rv = vobj->current;
		rv.scale.x *= vobj->origw;
		rv.scale.y *= vobj->origh;
	}
	
	return rv;
}

surface_properties arcan_video_properties_at(arcan_vobj_id id, unsigned ticks)
{
	if (ticks == 0)
		return arcan_video_current_properties(id);
	surface_properties rv = {0};
	arcan_vobject* vobj = arcan_video_getobject(id);
	
	if (vobj){
		rv = vobj->current;
/* if there's no transform defined, then the ticks will be the same */
		if (vobj->transform){
/* translate ticks from relative to absolute */
			ticks += arcan_video_display.c_ticks;
/* check if there is a transform for each individual attribute, and find the one that
 * defines a timeslot within the range of the desired value */
			surface_transform* current = vobj->transform;
			if (current->move.startt){
				while (current->move.endt < ticks && current->next && current->next->move.startt)
					current = current->next;

				if (current->move.endt <= ticks)
					rv.position = current->move.endp;
				else if (current->move.startt == ticks)
					rv.position = current->move.startp;
				else{ /* need to interpolate */
					float fract = lerp_fract(current->move.startt, current->move.endt, ticks);
					rv.position = lerp_vector(current->move.startp, current->move.endp, fract);
				}
			}
			
			current = vobj->transform;
			if (current->scale.startt){
				while (current->scale.endt < ticks && current->next && current->next->scale.startt)
					current = current->next;

				if (current->scale.endt <= ticks)
					rv.scale = current->scale.endd;
				else if (current->scale.startt == ticks)
					rv.scale = current->scale.startd;
				else{
					float fract = lerp_fract(current->scale.startt, current->scale.endt, ticks);
					rv.scale = lerp_vector(current->scale.startd, current->scale.endd, fract);
				}
			}

			current = vobj->transform;
			if (current->blend.startt){
				while (current->blend.endt < ticks && current->next && current->next->blend.startt)
					current = current->next;

				if (current->blend.endt <= ticks)
					rv.opa = current->blend.endopa;
				else if (current->blend.startt == ticks)
					rv.opa = current->blend.startopa;
				else{
					float fract = lerp_fract(current->blend.startt, current->blend.endt, ticks);
					rv.opa = lerp_val(current->blend.startopa, current->blend.endopa, fract);
				}
			}

			current = vobj->transform;
			if (current->rotate.startt){
				while (current->rotate.endt < ticks && current->next && current->next->rotate.startt)
					current = current->next;

				if (current->rotate.endt <= ticks)
					rv.rotation = current->rotate.endo;
				else if (current->rotate.startt == ticks)
					rv.rotation = current->rotate.starto;
				else{ 
					float fract = lerp_fract(current->rotate.startt, current->rotate.endt, ticks);
					rv.rotation.quaternion = nlerp_quat(current->rotate.starto.quaternion, current->rotate.endo.quaternion, fract);
				}
			}
		}

		rv.scale.x *= vobj->origw;
		rv.scale.y *= vobj->origh;
	}
	
	return rv;
}

bool arcan_video_prepare_external()
{
	/* There seems to be no decent, portable, way to minimize + suspend and when child terminates, maximize and be 
	 * sure that OpenGL / SDL context data is restored respectively. Thus we destroy the surface,
	 * and then rebuild / reupload all textures. */
	if (-1 == arcan_video_pushcontext())
		return false;
	
	SDL_FreeSurface(arcan_video_display.screen);
	if (arcan_video_display.fullscreen)
		SDL_QuitSubSystem(SDL_INIT_VIDEO);

	/* We need to kill of large parts of SDL as it may hold locks on other resources that the external launch might need */
	arcan_event_deinit(arcan_event_defaultctx());
	arcan_shader_unload_all();

	return true;
}

unsigned arcan_video_maxorder()
{
	arcan_vobject_litem* current = current_context->stdoutp.first;
	int order = 0;
	
	while (current){
		if (current->elem && current->elem->order > order)
			order = current->elem->order;
		
		current = current->next;
	}

	return order;
}

unsigned arcan_video_contextusage(unsigned* free)
{
	if (free){
		*free = 0;
		for (unsigned i = 1; i < current_context->vitem_limit-1; i++)
			if (current_context->vitems_pool[i].flags.in_use)
				(*free)++;
	}

	return current_context->vitem_limit-1;
}

void arcan_video_contextsize(unsigned newlim)
{
	arcan_video_display.default_vitemlim = newlim;
}

void arcan_video_restore_external()
{
	if (arcan_video_display.fullscreen)
		SDL_Init(SDL_INIT_VIDEO);

	arcan_video_display.screen = SDL_SetVideoMode(arcan_video_display.width,
											arcan_video_display.height,
											arcan_video_display.bpp,
											arcan_video_display.sdlarg);
	arcan_event_init( arcan_event_defaultctx() );
	arcan_video_gldefault();
	arcan_shader_rebuild_all();
	arcan_video_popcontext();
}

void arcan_video_shutdown()
{
	arcan_vobject_litem* current = current_context->stdoutp.first;
	unsigned lastctxa, lastctxc = arcan_video_popcontext();

/*  this will effectively make sure that all external launchers, frameservers etc. gets killed off */
	while ( lastctxc != (lastctxa = arcan_video_popcontext()) )
		lastctxc = lastctxa;

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int arcan_debug_pumpglwarnings(const char* src){
	GLenum errc = glGetError();
	if (errc != GL_NO_ERROR){
		arcan_warning("GLError detected (%s) GL error, code: %d\n", src, errc);
		return -1;
	}
}

static char fbobuf[64];
static char* renderbuf_parameters(GLuint id)
{
	int w, h, format;

	glBindRenderbuffer(GL_RENDERBUFFER, id);
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &w); 
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &h); 
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format); 
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	snprintf(fbobuf, 64, "%d * %d @ %d\n", w, h, format);
	
	return fbobuf;
}

static char* texture_parameters(GLuint id)
{
	int w, h, format;
	
	glBindTexture(GL_TEXTURE_2D, id);

	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w); 
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format); 
	
	glBindTexture(GL_TEXTURE_2D, 0);

	snprintf(fbobuf, 64, "%d * %d @ %d\n", w, h, format);
	return fbobuf;
}
        
/* assume an active / bound FBO,
 * enumerate all available attachments, decode the format of each detatched object */
static void arcan_debug_curfbostatus(GLenum status, enum rendertarget_mode mode)
{
	arcan_warning("FBO status:\n----------\n");
		switch (mode){
			case RENDERTARGET_COLOR : arcan_warning("mode: color\n"); break;
			case RENDERTARGET_COLOR_DEPTH : arcan_warning("mode: color, depth\n"); break;
			case RENDERTARGET_COLOR_DEPTH_STENCIL: arcan_warning("mode: color, depth, stencil\n"); break;
			case RENDERTARGET_DEPTH : arcan_warning("mode: depth\n"); break;
		}
	
	switch (status){
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: arcan_warning("error: incomplete attachment\n"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: arcan_warning("error: incomplete / missing attachment\n"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: arcan_warning("error: incomplete draw buffer\n"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: arcan_warning("error: incomplete read buffer\n"); break;
		case GL_FRAMEBUFFER_UNSUPPORTED: arcan_warning("error: GPU FBO implementation doesn't support the requested configuration.\n"); break;
		default:
			arcan_warning("error: unknown code(%d)\n", status);
	}
	
	
	int n_colbuf = 0;
	glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &n_colbuf);
	arcan_warning("\tcolor buffer attachments: %d\n", n_colbuf);
		
	int objectType;
	int objectId;

	for(int i = 0; i < n_colbuf; ++i){
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
			GL_COLOR_ATTACHMENT0+i,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
			&objectType);

		if(objectType != GL_NONE){
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
				GL_COLOR_ATTACHMENT0+i,
				GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
				&objectId);

			arcan_warning("\tcolor attachment(%d):\n", i);
			if(objectType == GL_TEXTURE)
				arcan_warning("\t\ttexture: %s\n", texture_parameters(objectId));
			else if(objectType == GL_RENDERBUFFER)
				arcan_warning("\t\trenderbuffer: %d\n", renderbuf_parameters(objectId));
		}
	}

	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
		GL_DEPTH_ATTACHMENT,
		GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
		&objectType);
 
	if(objectType != GL_NONE){
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
			GL_DEPTH_ATTACHMENT,
			GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
			&objectId);

		arcan_warning("\tdepth attachment:\n");
		switch(objectType){
			case GL_TEXTURE: arcan_warning("\t\ttexture: %s\n", texture_parameters(objectId)); break;
			case GL_RENDERBUFFER: arcan_warning("\t\trenderbuffer: %s\n", renderbuf_parameters(objectId)); break;
    }
	}

	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
	if(objectType != GL_NONE){
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectId);

		arcan_warning("\tstencil attachment:\n");
		switch(objectType){
			case GL_TEXTURE: arcan_warning("\t\ttexture: %s\n", texture_parameters(objectId)); break;
			case GL_RENDERBUFFER: arcan_warning("\t\trenderbuffer: %s\n", renderbuf_parameters(objectId)); break;
		}
	}

}

