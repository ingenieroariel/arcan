#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <sqlite3.h>
#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include <lualib.h>
#include <lauxlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>

#include "../a12.h"
#include "../../engine/arcan_mem.h"
#include "../../engine/arcan_db.h"
#include "nbio.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>

#include "anet_helper.h"
#include "directory.h"

static lua_State* L;
static struct arcan_dbh* DB;
static struct global_cfg* CFG;
static bool INITIALIZED;

struct runner;

static struct runner {
	uint16_t identifier;
	struct shmifsrv_client* C;
	struct runner* next;
} RUNNERS;

static void dump_stack(lua_State* L, FILE* dst)
{
	int top = lua_gettop(L);
	fprintf(dst, "-- stack dump (%d)--\n", top);

	for (size_t i = 1; i <= top; i++){
		int t = lua_type(L, i);

		switch (t){
		case LUA_TBOOLEAN:
			fprintf(dst, lua_toboolean(L, i) ? "true" : "false");
		break;
		case LUA_TSTRING:
			fprintf(dst, "%d\t'%s'\n", i, lua_tostring(L, i));
			break;
		case LUA_TNUMBER:
			fprintf(dst, "%d\t%g\n", i, lua_tonumber(L, i));
			break;
		default:
			fprintf(dst, "%d\t%s\n", i, lua_typename(L, t));
			break;
		}
	}

	fprintf(dst, "\n");
}

void anet_directory_lua_exit()
{
	lua_close(L);
	alt_nbio_release();
	L = NULL;
}

static int db_get_key(lua_State* L)
{
	const char* key = luaL_checkstring(L, 1);
	char* val = arcan_db_appl_val(DB, "a12", key);
	if (val){
		lua_pushstring(L, val);
	}
	else
		lua_pushnil(L);

	return 1;
}

static bool lookup_entrypoint(lua_State* L, const char* ep, size_t len)
{
	lua_getglobal(L, ep);

	if (!lua_isfunction(L, -1)){
		lua_pop(L, 1);
		return false;
	}

	return true;
}

static void push_dircl(lua_State* L, struct dircl* C)
{
/* should probably bind C to userdata, retain the tag in dircl and
 * use that to recall the same reference until it's gone */
	lua_newtable(L);
	luaL_getmetatable(L, "dircl");
	lua_setmetatable(L, -2);
}

extern int a12_trace_targets;
static int cfg_index(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	if (strcmp(key, "secret") == 0){
		lua_pushstring(L, CFG->meta.opts->secret);
		return 1;
	}

	if (strcmp(key, "allow_tunnel") == 0){
		lua_pushboolean(L, CFG->dirsrv.allow_tunnel);
		return 1;
	}

	if (strcmp(key, "log_level") == 0){
		lua_pushnumber(L, a12_trace_targets);
		return 1;
	}

	luaL_error(L, "unknown key: %s, allowed: secret, allow_tunnel, log_level\n", key);
	return 0;
}

static const char* trace_groups[] = {
	"video",
	"audio",
	"system",
	"event",
	"transfer",
	"debug",
	"missing",
	"alloc",
	"crypto",
	"vdetail",
	"binary",
	"security",
	"directory"
};

static int cfg_newindex(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	if (strcmp(key, "secret") == 0){
		const char* val = luaL_checkstring(L, 3);

		if (strlen(val) > 31 || !*val)
			luaL_error(L, "secret = (0 < string < 32)");
		snprintf(CFG->meta.opts->secret, 32, "%s", val);
	}
	else if (strcmp(key, "allow_tunnel") == 0){
		if (lua_type(L, 3) != LUA_TBOOLEAN)
			luaL_error(L, "allow_tunnel = [true | false]");
		CFG->dirsrv.allow_tunnel = !!lua_tonumber(L, 3);
	}
	else if (strcmp(key, "log_level") == 0){
		if (lua_type(L, 3) == LUA_TTABLE){
			a12_trace_targets = 0;
			for (size_t i = 0; i <= COUNT_OF(trace_groups); i++){
				lua_getfield(L, 3, trace_groups[i]);
				if (lua_toboolean(L, -1)){
					a12_trace_targets |= 1 << i;
				}
				lua_pop(L, 1);
			}
		}
		else if (lua_type(L, 3) == LUA_TNUMBER){
			a12_trace_targets = lua_tonumber(L, 3);
		}
	}
	else if (strcmp(key, "log_target") == 0){
		const char* path = lua_tostring(L, 3);
		FILE* fpek = fopen(path, "w");
		if (!fpek)
			luaL_error(L, "couldn't open (w+): config.log_target = %s\n", path);
		a12_set_trace_level(a12_trace_targets, fpek);
	}
	else
		luaL_error(L, "unknown key: config.%s, allowed: secret, allow_tunnel\n", key);

	return 0;
}

static int cfgperm_index(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);
	printf("perm-index: %s\n", key);

	return 0;
}

static int cfgperm_newindex(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	if (strcmp(key, "source") == 0){
		if (CFG->dirsrv.allow_src)
			free(CFG->dirsrv.allow_src);
		CFG->dirsrv.allow_src = strdup(luaL_checkstring(L, 3));
	}
	else if (strcmp(key, "dir") == 0){
		if (CFG->dirsrv.allow_dir)
			free(CFG->dirsrv.allow_dir);
		CFG->dirsrv.allow_dir = strdup(luaL_checkstring(L, 3));
	}
	else if (strcmp(key, "appl") == 0){
		if (CFG->dirsrv.allow_appl)
			free(CFG->dirsrv.allow_appl);
		CFG->dirsrv.allow_appl = strdup(luaL_checkstring(L, 3));
	}
	else if (strcmp(key, "resources") == 0){
		if (CFG->dirsrv.allow_ares)
			free(CFG->dirsrv.allow_ares);
		CFG->dirsrv.allow_ares = strdup(luaL_checkstring(L, 3));
	}
	return 0;
}

static int cfgpath_index(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

	if (strcmp(key, "database") == 0){
		lua_pushstring(L, CFG->db_file);
		return 1;
	}

	if (strcmp(key, "appl") == 0){
		if (!getenv("ARCAN_APPLBASEPATH"))
			lua_pushnil(L);
		else
			lua_pushstring(L, getenv("ARCAN_APPLBASEPATH"));
		return 1;
	}

	if (strcmp(key, "keystore") == 0){
		if (INITIALIZED){
			luaL_error(L, "config.keystore read-only after init()");
			return 0;
		}
		if (!getenv("ARCAN_STATEPATH"))
			lua_pushnil(L);
		else
			lua_pushstring(L, getenv("ARCAN_STATEPATH"));
		return 1;
	}

	luaL_error(L, "unknown path: config.paths.%s, "
		"accepted: database, appl, appl_server, keystore, resources", key);

	return 0;
}

static int cfgpath_newindex(lua_State* L)
{
	const char* key = luaL_checkstring(L, 2);

/* both state, appl and keystore are polluting env. from the legacy/history of
 * being passed via shmif through handover execution and we lack portable
 * primitives for anything better, so re-use that */
	if (strcmp(key, "appl") == 0){
		const char* val = luaL_checkstring(L, 3);
		if (-1 != CFG->directory){
		close(CFG->directory);
		}
		CFG->directory = open(val, O_RDONLY | O_DIRECTORY);
		if (-1 == CFG->directory){
			luaL_error(L, "config.paths.appl = %s, can't open as directory", val);
		}
		CFG->flag_rescan = 1;
		setenv("ARCAN_APPLBASEPATH", val, 1);
		return 0;
	}
	else if (strcmp(key, "appl_server") == 0){
		const char* val = luaL_checkstring(L, 3);

		if (CFG->dirsrv.appl_server_path){
			free(CFG->dirsrv.appl_server_path);
			close(CFG->dirsrv.appl_server_dfd);
		}

		int dirfd = open(val, O_RDONLY | O_DIRECTORY);
		if (-1 == dirfd)
			luaL_error(L, "config.paths.appl_server = %s, can't open as directory", val);

		CFG->dirsrv.appl_server_path = strdup(val);
		CFG->dirsrv.appl_server_dfd = dirfd;
		return 0;
	}
	else if (strcmp(key, "resources") == 0){
		const char* val = luaL_checkstring(L, 3);

		if (CFG->dirsrv.resource_path){
			free(CFG->dirsrv.resource_path);
			close(CFG->dirsrv.resource_dfd);
		}

		int dirfd = open(val, O_RDONLY | O_DIRECTORY);
		if (-1 == dirfd)
			luaL_error(L, "config.paths.appl_server = %s, can't open as directory", val);

		CFG->dirsrv.resource_path = strdup(val);
		CFG->dirsrv.resource_dfd = dirfd;
		return 0;
	}

/* remaining keys are read-only after init */
	if (INITIALIZED)
		luaL_error(L, "config.paths.%s, read/only after init()", key);

	if (strcmp(key, "database") == 0){
		const char* val = luaL_checkstring(L, 3);
		if (CFG->db_file)
			free(CFG->db_file);
		CFG->db_file = strdup(val);
	}
	else if (strcmp(key, "keystore") == 0){
		const char* val = luaL_checkstring(L, 3);
		int dirfd = open(val, O_RDONLY | O_DIRECTORY);
		if (-1 == dirfd)
			luaL_error(L, "config.paths.keystore = %s, can't open as directory", val);

		if (CFG->meta.keystore.directory.dirfd > 0)
			close(CFG->meta.keystore.directory.dirfd);

		CFG->meta.keystore.directory.dirfd = dirfd;
	}
	else {
		luaL_error(L,
			"unknown path key (%s), accepted:\n\t\n", key,
			"database, appl, appl_server, keystore, resources");
	}

	return 0;
}

static int dir_index(lua_State* L)
{
	return 0;
}

static int dir_newindex(lua_State* L)
{
	return 0;
}

bool anet_directory_lua_init(struct global_cfg* cfg)
{
	L = luaL_newstate();
	CFG = cfg;

	if (!L){
		fprintf(stderr, "luaL_newstate() - failed\n");
		return false;
	}

	luaL_openlibs(L);

	luaL_newmetatable(L, "dircl");
	lua_pushcfunction(L, dir_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, dir_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_pop(L, 1);

	luaL_newmetatable(L, "cfgtbl");
	lua_pushcfunction(L, cfg_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, cfg_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_pop(L, 1);

	luaL_newmetatable(L, "cfgpermtbl");
	lua_pushcfunction(L, cfgperm_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, cfgperm_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_pop(L, 1);

	luaL_newmetatable(L, "cfgpathtbl");
	lua_pushcfunction(L, cfgpath_index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, cfgpath_newindex);
	lua_setfield(L, -2, "__newindex");
	lua_pop(L, 1);

/*
 * build "config" which will contain the current config.
 *
 * then call 'init' which lets the script in 'fn' mutate 'config',
 * open 'DB' and then expose the database accessor functions.
 */
	lua_newtable(L); /* TABLE */
	luaL_getmetatable(L, "cfgtbl");
	lua_setmetatable(L, -2);

/* add permissions table to config */
	lua_pushstring(L, "permissions"); /* TABLE(cfgtbl), STRING */
	lua_createtable(L, 0, 10); /* TABLE (cfgtbl), "permissions", TABLE */
	luaL_getmetatable(L, "cfgpermtbl");
	lua_setmetatable(L, -2);
	lua_rawset(L, -3); /* TABLE(cfgperm) */

/* add paths table to config */
	lua_pushstring(L, "paths"); /* TABLE(cfgtbl), STRING */
	lua_createtable(L, 0, 10); /* TABLE(cfgtbl), "paths", TABLE */
	luaL_getmetatable(L, "cfgpathtbl");
	lua_setmetatable(L, -2); /* TABLE(cfgtbl), "paths", TABLE(cfgpath) */
	lua_rawset(L, -3); /* TABLE(cfgtbl) */

	lua_setglobal(L, "config"); /* nil */

	if (cfg->config_file){
		int status = luaL_dofile(L, cfg->config_file);
		if (0 != status){
			const char* msg = lua_tostring(L, -1);
			fprintf(stderr, "%s: failed, %s\n", cfg->config_file, msg);
			return false;
		}

		if (lookup_entrypoint(L, "init", 4)){
			if (0 != lua_pcall(L, 0, 0, 0)){
				fprintf(stderr, "%s: failed, %s\n", cfg->config_file, lua_tostring(L, -1));
				return false;
			}
		}
	}

/*
 * we want the database open regardless to deal with keystore scripts */
	INITIALIZED = true;
	DB = arcan_db_open(cfg->db_file, NULL);
	if (!DB){
		fprintf(stderr,
			"Couldn't open database, check config.paths.database and permissions\n");
		return false;
	}

	return true;
}

int anet_directory_lua_filter_source(struct dircl* C, arcan_event* ev)
{
/* look for the entrypoint */
	lua_getglobal(L, "new_source");
	if (!lua_isfunction(L, -1)){
		lua_pop(L, 1);
		return 0;
	}

/* call event + type into it */
	push_dircl(L, C);
	lua_pushstring(L, ev->ext.netstate.name); /* caller guarantees termination */
	if (ev->ext.netstate.type == ROLE_DIR)
		lua_pushstring(L, "directory");
	else if (ev->ext.netstate.type == ROLE_SOURCE)
		lua_pushstring(L, "source");
	else if (ev->ext.netstate.type == ROLE_SINK)
		lua_pushstring(L, "sink");

/* script errors should perhaps be treated more leniently in the config,
 * copy patterns from arcan_lua.c if so, but for the time being be strict */
	lua_call(L, 3, 0);

	return 1;
}

struct pk_response
	anet_directory_lua_register_unknown(struct dircl* C, struct pk_response base)
{
	lua_getglobal(L, "register_unknown");
	if (!lua_isfunction(L, -1)){
		lua_pop(L, 1);
		return base;
	}

	push_dircl(L, C);
	lua_call(L, 1, 1);
	if (lua_type(L, -1) == LUA_TBOOLEAN){
		base.authentic = lua_toboolean(L, -1);
	}

	lua_pop(L, 1);
	return base;
}

void anet_directory_lua_join(struct dircl* C, struct appl_meta* appl)
{
/* Check if there already is a runner for the appl. */
	struct runner* cur = &RUNNERS;
	struct runner** tgt;

	char* argv[] = {CFG->path_self, "dirappl", NULL, NULL};

	struct shmifsrv_envp env = {
		.path = CFG->path_self,
		.envv = NULL,
		.argv = argv,
		.detach = 2 | 4 | 8
	};

	int clsock;
	struct shmifsrv_client* cl = shmifsrv_spawn_client(env, &clsock, NULL, 0);

	struct dircl* newent = malloc(sizeof(struct dircl));
	*newent = (struct dircl){
		.C = cl,
		.in_appl = -1,
		.endpoint = {
			.category = EVENT_EXTERNAL,
			.ext.kind = EVENT_EXTERNAL_NETSTATE
		}
	};
}

void anet_directory_lua_register(struct dircl* C)
{
	lua_getglobal(L, "register");
	if (!lua_isfunction(L, -1)){
		lua_pop(L, 1);
		return;
	}

	push_dircl(L, C);
	lua_call(L, 1, 0);
	return;
}
