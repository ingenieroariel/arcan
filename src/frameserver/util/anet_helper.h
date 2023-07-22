#ifndef HAVE_ARCAN_NET_HELPER
#define HAVE_ARCAN_NET_HELPER

/*
 * keystore provider types and constraints
 */
enum a12helper_providers {
/* naive single-file per key approach, does not handle concurrent write access
 * outside basic posix file locking semantics */
	A12HELPER_PROVIDER_BASEDIR = 0
};

struct keystore_provider {
	union {
	struct {
		int dirfd;
	} directory;
	};

	int type;
};

struct anet_options {
/* remote connection point to route through (if permitted) */
	const char* cp;
	const char* host;
	const char* port;

/* keyfile to use when picking an outgoing host, this will override any
 * specified 'host' or 'port' */
	const char* key;

/* tag from keystore to use for authentication (server reply) or if 'key' is
 * not set and [host,port]+[host_tag] is used - if host_tag is not set,
 * 'default' will be used */
	const char* host_tag;

/* pre-inherited socket to use */
	int sockfd;

/* determine if we go multithread, multiprocess or single */
	int mt_mode;

/* client or server */
	int mode;

/* n- psk hello packets with unknown public keys will be added to the keystore */
	int allow_n_keys;

/* in the event of a _EXIT message, instead send the client to migrate */
	const char* redirect_exit;

/* similarly, remember any local connection point and use that */

	const char* devicehint_cp;

/* allow connection retries, -1 infinite, 0 no retry */
	ssize_t retry_count;

/* construction arguments for the keystore */
	struct keystore_provider keystore;
	struct a12_context_options* opts;
};

/*
 * [blocking]
 * configure, connect and authenticate a client connection.
 *
 * The destination is taken from the keystore, unless not provided or
 * if an override is provided through host and port.
 *
 * If a preshared secret is to be used, provide that in opts->secret
 *
 * returns an authenticated client context or NULL, with any error
 * message dynamically allocated in *errmsg (if provided and available).
 */
struct anet_cl_connection {
	int fd;
	struct a12_state* state;
	char* errmsg;
};

struct anet_cl_connection anet_cl_setup(struct anet_options* opts);

/* setup the keystore using the specified provider,
 *
 * returns false if the provider is missing/broken or there already is a
 * keystore open in the current process.
 *
 * takes ownership of any resources referenced in the provider
 */
bool a12helper_keystore_open(struct keystore_provider*);

/* release resources tied to the keystore */
bool a12helper_keystore_release();

uint8_t* a12helper_tob64(const uint8_t* data, size_t inl, size_t* outl);

/* retrieve key and connect properties for a user-defined tag,
 * increment index to fetch the next possible host.
 *
 * returns false when there are no more keys on the store */
bool a12helper_keystore_hostkey(const char* petname, size_t index,
	uint8_t privk[static 32], char** outhost, uint16_t* outport);

/* list all the known outbound tags, terminates with a NULL petname */
bool a12helper_keystore_tags(bool (*cb)(const char* petname, void*), void* tag);

/* Append or crete a new tag with the specified host, this will also
 * create a new private key if needed. Returns the public key in outk */
bool a12helper_keystore_register(
	const char* petname, const char* host, uint16_t port, uint8_t pubk[static 32]);

/*
 * Check if the public key is known and accepted for the specified trust domain
 * (not to be confused with host/domain names as in DNS).
 */
bool a12helper_keystore_accepted(
	const uint8_t pubk[static 32], const char* connp);

/*
 * add the supplied public key to the accepted keystore.
 *
 * if [connp] is NULL, the domain will default to 'outbound'.
 *
 * otherwise connp is a comma separated list of local names (similar to
 * connection points) or a wildcard '*'.
 */
bool a12helper_keystore_accept(const uint8_t pubk[static 32], const char* connp);

/*
 * From a prefilled addrinfo structure, enumerate all interfaces and try
 * to connect, return the connected socket or -1 if it failed
 */
int anet_clfd(struct addrinfo* addr);

/*
 * Blocking read/write cycle that feeds the state machine until authentication
 * either goes through(=true) or fails(=false). The context is alive regardless
 * and it is the caller that is responsible for cleaning up.
 */
bool anet_authenticate(struct a12_state* S, int fdin, int fdout, char** err);

/*
 * Open or allocate (sz > 0) a name for assigning custom state data to a public
 * key and return a FILE* abstraction for reading or writing based on 'mode'.
 */
int a12helper_keystore_statestore(
	const uint8_t pubk[static 32], const char* name, size_t sz, const char* mode);

/*
 * Used for the BASEDIR keystore method, using environment variables or config
 * files to figure out where to store the keys.
 */
int a12helper_keystore_dirfd(const char** err);

/*
 * Using the configuration structure in anet_options, build a listening
 * socket and invoke dispatch with the heap allocated state object and
 * descriptor. This function will only return on failure, with [errmsg]
 * set to a heap allocated human readable string.
 */
bool anet_listen(struct anet_options* args, char** errmsg,
	void (*dispatch)(struct a12_state* S, int fd, void* tag), void* tag);
#endif
