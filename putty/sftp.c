/*
 * sftp.c: SFTP generic client code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "misc.h"
#include "int64.h"
#include "tree234.h"
#include "sftp.h"

struct sftp_packet {
    char *data;
    unsigned length, maxlen;
    unsigned savedpos;
    int type;
    BinarySink_IMPLEMENTATION;
    BinarySource_IMPLEMENTATION;
};

static const char *fxp_error_message;
static int fxp_errtype;

static void fxp_internal_error(const char *msg);

/* ----------------------------------------------------------------------
 * SFTP packet construction functions.
 */
static void sftp_pkt_BinarySink_write(
    BinarySink *bs, const void *data, size_t length)
{
    struct sftp_packet *pkt = BinarySink_DOWNCAST(bs, struct sftp_packet);
    unsigned newlen;

    assert(length <= 0xFFFFFFFFU - pkt->length);

    newlen = pkt->length + length;
    if (pkt->maxlen < newlen) {
	pkt->maxlen = newlen * 5 / 4 + 256;
	pkt->data = sresize(pkt->data, pkt->maxlen, char);
    }

    memcpy(pkt->data + pkt->length, data, length);
    pkt->length = newlen;
}
static struct sftp_packet *sftp_pkt_init(int pkt_type)
{
    struct sftp_packet *pkt;
    pkt = snew(struct sftp_packet);
    pkt->data = NULL;
    pkt->savedpos = -1;
    pkt->length = 0;
    pkt->maxlen = 0;
    BinarySink_INIT(pkt, sftp_pkt_BinarySink_write);
    put_uint32(pkt, 0); /* length field will be filled in later */
    put_byte(pkt, pkt_type);
    return pkt;
}

static void BinarySink_put_fxp_attrs(BinarySink *bs, struct fxp_attrs attrs)
{
    put_uint32(bs, attrs.flags);
    if (attrs.flags & SSH_FILEXFER_ATTR_SIZE)
	put_uint64(bs, attrs.size);
    if (attrs.flags & SSH_FILEXFER_ATTR_UIDGID) {
	put_uint32(bs, attrs.uid);
	put_uint32(bs, attrs.gid);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
	put_uint32(bs, attrs.permissions);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_ACMODTIME) {
	put_uint32(bs, attrs.atime);
	put_uint32(bs, attrs.mtime);
    }
    if (attrs.flags & SSH_FILEXFER_ATTR_EXTENDED) {
	/*
	 * We currently don't support sending any extended
	 * attributes.
	 */
    }
}

#define put_fxp_attrs(bs, attrs) \
    BinarySink_put_fxp_attrs(BinarySink_UPCAST(bs), attrs)

/* ----------------------------------------------------------------------
 * SFTP packet decode functions.
 */

static int BinarySource_get_fxp_attrs(BinarySource *src,
                                      struct fxp_attrs *attrs)
{
    attrs->flags = get_uint32(src);
    if (attrs->flags & SSH_FILEXFER_ATTR_SIZE)
        attrs->size = get_uint64(src);
    if (attrs->flags & SSH_FILEXFER_ATTR_UIDGID) {
	attrs->uid = get_uint32(src);
	attrs->gid = get_uint32(src);
    }
    if (attrs->flags & SSH_FILEXFER_ATTR_PERMISSIONS)
	attrs->permissions = get_uint32(src);
    if (attrs->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
	attrs->atime = get_uint32(src);
	attrs->mtime = get_uint32(src);
    }
    if (attrs->flags & SSH_FILEXFER_ATTR_EXTENDED) {
	unsigned long count = get_uint32(src);
	while (count--) {
	    /*
	     * We should try to analyse these, if we ever find one
	     * we recognise.
	     */
	    get_string(src);
	    get_string(src);
	}
    }
    return 1;
}

#define get_fxp_attrs(bs, attrs) \
    BinarySource_get_fxp_attrs(BinarySource_UPCAST(bs), attrs)

static void sftp_pkt_free(struct sftp_packet *pkt)
{
    if (pkt->data)
	sfree(pkt->data);
    sfree(pkt);
}

/* ----------------------------------------------------------------------
 * Send and receive packet functions.
 */
int sftp_send(struct sftp_packet *pkt)
{
    int ret;
    PUT_32BIT(pkt->data, pkt->length - 4);
    ret = sftp_senddata(pkt->data, pkt->length);
    sftp_pkt_free(pkt);
    return ret;
}
struct sftp_packet *sftp_recv(void)
{
    struct sftp_packet *pkt;
    char x[4];

    if (!sftp_recvdata(x, 4))
	return NULL;

    pkt = snew(struct sftp_packet);
    pkt->savedpos = 0;
    pkt->length = pkt->maxlen = GET_32BIT(x);
    pkt->data = snewn(pkt->length, char);

    if (!sftp_recvdata(pkt->data, pkt->length)) {
	sftp_pkt_free(pkt);
	return NULL;
    }

    BinarySource_INIT(pkt, pkt->data, pkt->length);
    pkt->type = get_byte(pkt);

    if (get_err(pkt)) {
	sftp_pkt_free(pkt);
	return NULL;
    }

    return pkt;
}

/* ----------------------------------------------------------------------
 * Request ID allocation and temporary dispatch routines.
 */

#define REQUEST_ID_OFFSET 256

struct sftp_request {
    unsigned id;
    int registered;
    void *userdata;
};

static int sftp_reqcmp(void *av, void *bv)
{
    struct sftp_request *a = (struct sftp_request *)av;
    struct sftp_request *b = (struct sftp_request *)bv;
    if (a->id < b->id)
	return -1;
    if (a->id > b->id)
	return +1;
    return 0;
}
static int sftp_reqfind(void *av, void *bv)
{
    unsigned *a = (unsigned *) av;
    struct sftp_request *b = (struct sftp_request *)bv;
    if (*a < b->id)
	return -1;
    if (*a > b->id)
	return +1;
    return 0;
}

static tree234 *sftp_requests;

static struct sftp_request *sftp_alloc_request(void)
{
    unsigned low, high, mid;
    int tsize;
    struct sftp_request *r;

    if (sftp_requests == NULL)
	sftp_requests = newtree234(sftp_reqcmp);

    /*
     * First-fit allocation of request IDs: always pick the lowest
     * unused one. To do this, binary-search using the counted
     * B-tree to find the largest ID which is in a contiguous
     * sequence from the beginning. (Precisely everything in that
     * sequence must have ID equal to its tree index plus
     * REQUEST_ID_OFFSET.)
     */
    tsize = count234(sftp_requests);

    low = -1;
    high = tsize;
    while (high - low > 1) {
	mid = (high + low) / 2;
	r = (struct sftp_request *)index234(sftp_requests, mid);
	if (r->id == mid + REQUEST_ID_OFFSET)
	    low = mid;		       /* this one is fine */
	else
	    high = mid;		       /* this one is past it */
    }
    /*
     * Now low points to either -1, or the tree index of the
     * largest ID in the initial sequence.
     */
    {
	unsigned i = low + 1 + REQUEST_ID_OFFSET;
	assert(NULL == find234(sftp_requests, &i, sftp_reqfind));
    }

    /*
     * So the request ID we need to create is
     * low + 1 + REQUEST_ID_OFFSET.
     */
    r = snew(struct sftp_request);
    r->id = low + 1 + REQUEST_ID_OFFSET;
    r->registered = 0;
    r->userdata = NULL;
    add234(sftp_requests, r);
    return r;
}

void sftp_cleanup_request(void)
{
    if (sftp_requests != NULL) {
	freetree234(sftp_requests);
	sftp_requests = NULL;
    }
}

void sftp_register(struct sftp_request *req)
{
    req->registered = 1;
}

struct sftp_request *sftp_find_request(struct sftp_packet *pktin)
{
    unsigned id;
    struct sftp_request *req;

    if (!pktin) {
	fxp_internal_error("did not receive a valid SFTP packet\n");
	return NULL;
    }

    id = get_uint32(pktin);
    if (get_err(pktin)) {
	fxp_internal_error("did not receive a valid SFTP packet\n");
	return NULL;
    }

    req = (struct sftp_request *)find234(sftp_requests, &id, sftp_reqfind);
    if (!req || !req->registered) {
	fxp_internal_error("request ID mismatch\n");
	return NULL;
    }

    del234(sftp_requests, req);

    return req;
}

/* ----------------------------------------------------------------------
 * SFTP primitives.
 */

/*
 * Deal with (and free) an FXP_STATUS packet. Return 1 if
 * SSH_FX_OK, 0 if SSH_FX_EOF, and -1 for anything else (error).
 * Also place the status into fxp_errtype.
 */
static int fxp_got_status(struct sftp_packet *pktin)
{
    static const char *const messages[] = {
	/* SSH_FX_OK. The only time we will display a _message_ for this
	 * is if we were expecting something other than FXP_STATUS on
	 * success, so this is actually an error message! */
	"unexpected OK response",
	"end of file",
	"no such file or directory",
	"permission denied",
	"failure",
	"bad message",
	"no connection",
	"connection lost",
	"operation unsupported",
    };

    if (pktin->type != SSH_FXP_STATUS) {
	fxp_error_message = "expected FXP_STATUS packet";
	fxp_errtype = -1;
    } else {
	fxp_errtype = get_uint32(pktin);
	if (get_err(pktin)) {
	    fxp_error_message = "malformed FXP_STATUS packet";
	    fxp_errtype = -1;
	} else {
	    if (fxp_errtype < 0 ||
		fxp_errtype >= sizeof(messages) / sizeof(*messages))
		fxp_error_message = "unknown error code";
	    else
		fxp_error_message = messages[fxp_errtype];
	}
    }

    if (fxp_errtype == SSH_FX_OK)
	return 1;
    else if (fxp_errtype == SSH_FX_EOF)
	return 0;
    else
	return -1;
}

static void fxp_internal_error(const char *msg)
{
    fxp_error_message = msg;
    fxp_errtype = -1;
}

const char *fxp_error(void)
{
    return fxp_error_message;
}

int fxp_error_type(void)
{
    return fxp_errtype;
}

/*
 * Perform exchange of init/version packets. Return 0 on failure.
 */
int fxp_init(void)
{
    struct sftp_packet *pktout, *pktin;
    unsigned long remotever;

    pktout = sftp_pkt_init(SSH_FXP_INIT);
    put_uint32(pktout, SFTP_PROTO_VERSION);
    sftp_send(pktout);

    pktin = sftp_recv();
    if (!pktin) {
	fxp_internal_error("could not connect");
	return 0;
    }
    if (pktin->type != SSH_FXP_VERSION) {
	fxp_internal_error("did not receive FXP_VERSION");
        sftp_pkt_free(pktin);
	return 0;
    }
    remotever = get_uint32(pktin);
    if (get_err(pktin)) {
	fxp_internal_error("malformed FXP_VERSION packet");
        sftp_pkt_free(pktin);
	return 0;
    }
    if (remotever > SFTP_PROTO_VERSION) {
	fxp_internal_error
	    ("remote protocol is more advanced than we support");
        sftp_pkt_free(pktin);
	return 0;
    }
    /*
     * In principle, this packet might also contain extension-
     * string pairs. We should work through them and look for any
     * we recognise. In practice we don't currently do so because
     * we know we don't recognise _any_.
     */
    sftp_pkt_free(pktin);

    return 1;
}

/*
 * Canonify a pathname.
 */
struct sftp_request *fxp_realpath_send(const char *path)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_REALPATH);
    put_uint32(pktout, req->id);
    put_stringz(pktout, path);
    sftp_send(pktout);

    return req;
}

char *fxp_realpath_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    sfree(req);

    if (pktin->type == SSH_FXP_NAME) {
	unsigned long count;
        char *path;
        ptrlen name;

        count = get_uint32(pktin);
	if (get_err(pktin) || count != 1) {
	    fxp_internal_error("REALPATH did not return name count of 1\n");
            sftp_pkt_free(pktin);
	    return NULL;
	}
        name = get_string(pktin);
	if (get_err(pktin)) {
	    fxp_internal_error("REALPATH returned malformed FXP_NAME\n");
            sftp_pkt_free(pktin);
	    return NULL;
	}
        path = mkstr(name);
	sftp_pkt_free(pktin);
        return path;
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return NULL;
    }
}

/*
 * Open a file.
 */
struct sftp_request *fxp_open_send(const char *path, int type,
                                   struct fxp_attrs *attrs)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_OPEN);
    put_uint32(pktout, req->id);
    put_stringz(pktout, path);
    put_uint32(pktout, type);
    if (attrs)
        put_fxp_attrs(pktout, *attrs);
    else
        put_uint32(pktout, 0); /* empty ATTRS structure */
    sftp_send(pktout);

    return req;
}

static struct fxp_handle *fxp_got_handle(struct sftp_packet *pktin)
{
    ptrlen id;
    struct fxp_handle *handle;

    id = get_string(pktin);
    if (get_err(pktin)) {
        fxp_internal_error("received malformed FXP_HANDLE");
        sftp_pkt_free(pktin);
        return NULL;
    }
    handle = snew(struct fxp_handle);
    handle->hstring = mkstr(id);
    handle->hlen = id.len;
    sftp_pkt_free(pktin);
    return handle;
}

struct fxp_handle *fxp_open_recv(struct sftp_packet *pktin,
				 struct sftp_request *req)
{
    sfree(req);

    if (pktin->type == SSH_FXP_HANDLE) {
        return fxp_got_handle(pktin);
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return NULL;
    }
}

/*
 * Open a directory.
 */
struct sftp_request *fxp_opendir_send(const char *path)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_OPENDIR);
    put_uint32(pktout, req->id);
    put_stringz(pktout, path);
    sftp_send(pktout);

    return req;
}

struct fxp_handle *fxp_opendir_recv(struct sftp_packet *pktin,
				    struct sftp_request *req)
{
    sfree(req);
    if (pktin->type == SSH_FXP_HANDLE) {
        return fxp_got_handle(pktin);
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return NULL;
    }
}

/*
 * Close a file/dir.
 */
struct sftp_request *fxp_close_send(struct fxp_handle *handle)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_CLOSE);
    put_uint32(pktout, req->id);
    put_string(pktout, handle->hstring, handle->hlen);
    sftp_send(pktout);

    sfree(handle->hstring);
    sfree(handle);

    return req;
}

int fxp_close_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    sfree(req);
    fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    return fxp_errtype == SSH_FX_OK;
}

struct sftp_request *fxp_mkdir_send(const char *path)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_MKDIR);
    put_uint32(pktout, req->id);
    put_stringz(pktout, path);
    put_uint32(pktout, 0);     /* (FIXME) empty ATTRS structure */
    sftp_send(pktout);

    return req;
}

int fxp_mkdir_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_rmdir_send(const char *path)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_RMDIR);
    put_uint32(pktout, req->id);
    put_stringz(pktout, path);
    sftp_send(pktout);

    return req;
}

int fxp_rmdir_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_remove_send(const char *fname)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_REMOVE);
    put_uint32(pktout, req->id);
    put_stringz(pktout, fname);
    sftp_send(pktout);

    return req;
}

int fxp_remove_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_rename_send(const char *srcfname,
                                     const char *dstfname)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_RENAME);
    put_uint32(pktout, req->id);
    put_stringz(pktout, srcfname);
    put_stringz(pktout, dstfname);
    sftp_send(pktout);

    return req;
}

int fxp_rename_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

/*
 * Retrieve the attributes of a file. We have fxp_stat which works
 * on filenames, and fxp_fstat which works on open file handles.
 */
struct sftp_request *fxp_stat_send(const char *fname)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_STAT);
    put_uint32(pktout, req->id);
    put_stringz(pktout, fname);
    sftp_send(pktout);

    return req;
}

static int fxp_got_attrs(struct sftp_packet *pktin, struct fxp_attrs *attrs)
{
    get_fxp_attrs(pktin, attrs);
    if (get_err(pktin)) {
        fxp_internal_error("malformed SSH_FXP_ATTRS packet");
        sftp_pkt_free(pktin);
        return 0;
    }
    sftp_pkt_free(pktin);
    return 1;
}

int fxp_stat_recv(struct sftp_packet *pktin, struct sftp_request *req,
		  struct fxp_attrs *attrs)
{
    sfree(req);
    if (pktin->type == SSH_FXP_ATTRS) {
        return fxp_got_attrs(pktin, attrs);
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return 0;
    }
}

struct sftp_request *fxp_fstat_send(struct fxp_handle *handle)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_FSTAT);
    put_uint32(pktout, req->id);
    put_string(pktout, handle->hstring, handle->hlen);
    sftp_send(pktout);

    return req;
}

int fxp_fstat_recv(struct sftp_packet *pktin, struct sftp_request *req,
		   struct fxp_attrs *attrs)
{
    sfree(req);
    if (pktin->type == SSH_FXP_ATTRS) {
        return fxp_got_attrs(pktin, attrs);
	sftp_pkt_free(pktin);
	return 1;
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return 0;
    }
}

/*
 * Set the attributes of a file.
 */
struct sftp_request *fxp_setstat_send(const char *fname,
                                      struct fxp_attrs attrs)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_SETSTAT);
    put_uint32(pktout, req->id);
    put_stringz(pktout, fname);
    put_fxp_attrs(pktout, attrs);
    sftp_send(pktout);

    return req;
}

int fxp_setstat_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

struct sftp_request *fxp_fsetstat_send(struct fxp_handle *handle,
				       struct fxp_attrs attrs)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_FSETSTAT);
    put_uint32(pktout, req->id);
    put_string(pktout, handle->hstring, handle->hlen);
    put_fxp_attrs(pktout, attrs);
    sftp_send(pktout);

    return req;
}

int fxp_fsetstat_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    int id;
    sfree(req);
    id = fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    if (id != 1) {
    	return 0;
    }
    return 1;
}

/*
 * Read from a file. Returns the number of bytes read, or -1 on an
 * error, or possibly 0 if EOF. (I'm not entirely sure whether it
 * will return 0 on EOF, or return -1 and store SSH_FX_EOF in the
 * error indicator. It might even depend on the SFTP server.)
 */
struct sftp_request *fxp_read_send(struct fxp_handle *handle,
				   uint64 offset, int len)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_READ);
    put_uint32(pktout, req->id);
    put_string(pktout, handle->hstring, handle->hlen);
    put_uint64(pktout, offset);
    put_uint32(pktout, len);
    sftp_send(pktout);

    return req;
}

int fxp_read_recv(struct sftp_packet *pktin, struct sftp_request *req,
		  char *buffer, int len)
{
    sfree(req);
    if (pktin->type == SSH_FXP_DATA) {
        ptrlen data;

        data = get_string(pktin);
	if (get_err(pktin)) {
	    fxp_internal_error("READ returned malformed SSH_FXP_DATA packet");
            sftp_pkt_free(pktin);
	    return -1;
	}

	if (data.len > len) {
	    fxp_internal_error("READ returned more bytes than requested");
            sftp_pkt_free(pktin);
	    return -1;
	}

	memcpy(buffer, data.ptr, data.len);
        sftp_pkt_free(pktin);
	return data.len;
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return -1;
    }
}

/*
 * Read from a directory.
 */
struct sftp_request *fxp_readdir_send(struct fxp_handle *handle)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_READDIR);
    put_uint32(pktout, req->id);
    put_string(pktout, handle->hstring, handle->hlen);
    sftp_send(pktout);

    return req;
}

struct fxp_names *fxp_readdir_recv(struct sftp_packet *pktin,
				   struct sftp_request *req)
{
    sfree(req);
    if (pktin->type == SSH_FXP_NAME) {
	struct fxp_names *ret;
	unsigned long i;

        i = get_uint32(pktin);

	/*
	 * Sanity-check the number of names. Minimum is obviously
	 * zero. Maximum is the remaining space in the packet
	 * divided by the very minimum length of a name, which is
	 * 12 bytes (4 for an empty filename, 4 for an empty
	 * longname, 4 for a set of attribute flags indicating that
	 * no other attributes are supplied).
	 */
	if (get_err(pktin) || i > get_avail(pktin) / 12) {
	    fxp_internal_error("malformed FXP_NAME packet");
	    sftp_pkt_free(pktin);
	    return NULL;
	}

	/*
	 * Ensure the implicit multiplication in the snewn() call
	 * doesn't suffer integer overflow and cause us to malloc
	 * too little space.
	 */
	if (i > INT_MAX / sizeof(struct fxp_name)) {
	    fxp_internal_error("unreasonably large FXP_NAME packet");
	    sftp_pkt_free(pktin);
	    return NULL;
	}

	ret = snew(struct fxp_names);
	ret->nnames = i;
	ret->names = snewn(ret->nnames, struct fxp_name);
	for (i = 0; i < (unsigned long)ret->nnames; i++) {
	    ret->names[i].filename = mkstr(get_string(pktin));
	    ret->names[i].longname = mkstr(get_string(pktin));
            get_fxp_attrs(pktin, &ret->names[i].attrs);
        }

	if (get_err(pktin)) {
            fxp_internal_error("malformed FXP_NAME packet");
            for (i = 0; i < (unsigned long)ret->nnames; i++) {
                sfree(ret->names[i].filename);
                sfree(ret->names[i].longname);
            }
            sfree(ret->names);
            sfree(ret);
            sfree(pktin);
            return NULL;
	}
        sftp_pkt_free(pktin);
	return ret;
    } else {
	fxp_got_status(pktin);
        sftp_pkt_free(pktin);
	return NULL;
    }
}

/*
 * Write to a file. Returns 0 on error, 1 on OK.
 */
struct sftp_request *fxp_write_send(struct fxp_handle *handle,
				    void *buffer, uint64 offset, int len)
{
    struct sftp_request *req = sftp_alloc_request();
    struct sftp_packet *pktout;

    pktout = sftp_pkt_init(SSH_FXP_WRITE);
    put_uint32(pktout, req->id);
    put_string(pktout, handle->hstring, handle->hlen);
    put_uint64(pktout, offset);
    put_string(pktout, buffer, len);
    sftp_send(pktout);

    return req;
}

int fxp_write_recv(struct sftp_packet *pktin, struct sftp_request *req)
{
    sfree(req);
    fxp_got_status(pktin);
    sftp_pkt_free(pktin);
    return fxp_errtype == SSH_FX_OK;
}

/*
 * Free up an fxp_names structure.
 */
void fxp_free_names(struct fxp_names *names)
{
    int i;

    for (i = 0; i < names->nnames; i++) {
	sfree(names->names[i].filename);
	sfree(names->names[i].longname);
    }
    sfree(names->names);
    sfree(names);
}

/*
 * Duplicate an fxp_name structure.
 */
struct fxp_name *fxp_dup_name(struct fxp_name *name)
{
    struct fxp_name *ret;
    ret = snew(struct fxp_name);
    ret->filename = dupstr(name->filename);
    ret->longname = dupstr(name->longname);
    ret->attrs = name->attrs;	       /* structure copy */
    return ret;
}

/*
 * Free up an fxp_name structure.
 */
void fxp_free_name(struct fxp_name *name)
{
    sfree(name->filename);
    sfree(name->longname);
    sfree(name);
}

/*
 * Store user data in an sftp_request structure.
 */
void *fxp_get_userdata(struct sftp_request *req)
{
    return req->userdata;
}

void fxp_set_userdata(struct sftp_request *req, void *data)
{
    req->userdata = data;
}

/*
 * A wrapper to go round fxp_read_* and fxp_write_*, which manages
 * the queueing of multiple read/write requests.
 */

struct req {
    char *buffer;
    int len, retlen, complete;
    uint64 offset;
    struct req *next, *prev;
};

struct fxp_xfer {
    uint64 offset, furthestdata, filesize;
    int req_totalsize, req_maxsize, eof, err;
    struct fxp_handle *fh;
    struct req *head, *tail;
};

static struct fxp_xfer *xfer_init(struct fxp_handle *fh, uint64 offset)
{
    struct fxp_xfer *xfer = snew(struct fxp_xfer);

    xfer->fh = fh;
    xfer->offset = offset;
    xfer->head = xfer->tail = NULL;
    xfer->req_totalsize = 0;
    xfer->req_maxsize = 1048576;
    xfer->err = 0;
    xfer->filesize = uint64_make(ULONG_MAX, ULONG_MAX);
    xfer->furthestdata = uint64_make(0, 0);

    return xfer;
}

int xfer_done(struct fxp_xfer *xfer)
{
    /*
     * We're finished if we've seen EOF _and_ there are no
     * outstanding requests.
     */
    return (xfer->eof || xfer->err) && !xfer->head;
}

void xfer_download_queue(struct fxp_xfer *xfer)
{
    while (xfer->req_totalsize < xfer->req_maxsize &&
	   !xfer->eof && !xfer->err) {
	/*
	 * Queue a new read request.
	 */
	struct req *rr;
	struct sftp_request *req;

	rr = snew(struct req);
	rr->offset = xfer->offset;
	rr->complete = 0;
	if (xfer->tail) {
	    xfer->tail->next = rr;
	    rr->prev = xfer->tail;
	} else {
	    xfer->head = rr;
	    rr->prev = NULL;
	}
	xfer->tail = rr;
	rr->next = NULL;

	rr->len = 32768;
	rr->buffer = snewn(rr->len, char);
	sftp_register(req = fxp_read_send(xfer->fh, rr->offset, rr->len));
	fxp_set_userdata(req, rr);

	xfer->offset = uint64_add32(xfer->offset, rr->len);
	xfer->req_totalsize += rr->len;

#ifdef DEBUG_DOWNLOAD
	{ char buf[40]; uint64_decimal(rr->offset, buf); printf("queueing read request %p at %s\n", rr, buf); }
#endif
    }
}

struct fxp_xfer *xfer_download_init(struct fxp_handle *fh, uint64 offset)
{
    struct fxp_xfer *xfer = xfer_init(fh, offset);

    xfer->eof = FALSE;
    xfer_download_queue(xfer);

    return xfer;
}

/*
 * Returns INT_MIN to indicate that it didn't even get as far as
 * fxp_read_recv and hence has not freed pktin.
 */
int xfer_download_gotpkt(struct fxp_xfer *xfer, struct sftp_packet *pktin)
{
    struct sftp_request *rreq;
    struct req *rr;

    rreq = sftp_find_request(pktin);
    if (!rreq)
        return INT_MIN;            /* this packet doesn't even make sense */
    rr = (struct req *)fxp_get_userdata(rreq);
    if (!rr) {
        fxp_internal_error("request ID is not part of the current download");
	return INT_MIN;		       /* this packet isn't ours */
    }
    rr->retlen = fxp_read_recv(pktin, rreq, rr->buffer, rr->len);
#ifdef DEBUG_DOWNLOAD
    printf("read request %p has returned [%d]\n", rr, rr->retlen);
#endif

    if ((rr->retlen < 0 && fxp_error_type()==SSH_FX_EOF) || rr->retlen == 0) {
	xfer->eof = TRUE;
        rr->retlen = 0;
	rr->complete = -1;
#ifdef DEBUG_DOWNLOAD
	printf("setting eof\n");
#endif
    } else if (rr->retlen < 0) {
	/* some error other than EOF; signal it back to caller */
	xfer_set_error(xfer);
	rr->complete = -1;
	return -1;
    }

    rr->complete = 1;

    /*
     * Special case: if we have received fewer bytes than we
     * actually read, we should do something. For the moment I'll
     * just throw an ersatz FXP error to signal this; the SFTP
     * draft I've got says that it can't happen except on special
     * files, in which case seeking probably has very little
     * meaning and so queueing an additional read request to fill
     * up the gap sounds like the wrong answer. I'm not sure what I
     * should be doing here - if it _was_ a special file, I suspect
     * I simply shouldn't have been queueing multiple requests in
     * the first place...
     */
    if (rr->retlen > 0 && uint64_compare(xfer->furthestdata, rr->offset) < 0) {
	xfer->furthestdata = rr->offset;
#ifdef DEBUG_DOWNLOAD
	{ char buf[40];
	uint64_decimal(xfer->furthestdata, buf);
	printf("setting furthestdata = %s\n", buf); }
#endif
    }

    if (rr->retlen < rr->len) {
	uint64 filesize = uint64_add32(rr->offset,
				       (rr->retlen < 0 ? 0 : rr->retlen));
#ifdef DEBUG_DOWNLOAD
	{ char buf[40];
	uint64_decimal(filesize, buf);
	printf("short block! trying filesize = %s\n", buf); }
#endif
	if (uint64_compare(xfer->filesize, filesize) > 0) {
	    xfer->filesize = filesize;
#ifdef DEBUG_DOWNLOAD
	    printf("actually changing filesize\n");
#endif	    
	}
    }

    if (uint64_compare(xfer->furthestdata, xfer->filesize) > 0) {
	fxp_error_message = "received a short buffer from FXP_READ, but not"
	    " at EOF";
	fxp_errtype = -1;
	xfer_set_error(xfer);
	return -1;
    }

    return 1;
}

void xfer_set_error(struct fxp_xfer *xfer)
{
    xfer->err = 1;
}

int xfer_download_data(struct fxp_xfer *xfer, void **buf, int *len)
{
    void *retbuf = NULL;
    int retlen = 0;

    /*
     * Discard anything at the head of the rr queue with complete <
     * 0; return the first thing with complete > 0.
     */
    while (xfer->head && xfer->head->complete && !retbuf) {
	struct req *rr = xfer->head;

	if (rr->complete > 0) {
	    retbuf = rr->buffer;
	    retlen = rr->retlen;
#ifdef DEBUG_DOWNLOAD
	    printf("handing back data from read request %p\n", rr);
#endif
	}
#ifdef DEBUG_DOWNLOAD
	else
	    printf("skipping failed read request %p\n", rr);
#endif

	xfer->head = xfer->head->next;
	if (xfer->head)
	    xfer->head->prev = NULL;
	else
	    xfer->tail = NULL;
	xfer->req_totalsize -= rr->len;
	sfree(rr);
    }

    if (retbuf) {
	*buf = retbuf;
	*len = retlen;
	return 1;
    } else
	return 0;
}

struct fxp_xfer *xfer_upload_init(struct fxp_handle *fh, uint64 offset)
{
    struct fxp_xfer *xfer = xfer_init(fh, offset);

    /*
     * We set `eof' to 1 because this will cause xfer_done() to
     * return true iff there are no outstanding requests. During an
     * upload, our caller will be responsible for working out
     * whether all the data has been sent, so all it needs to know
     * from us is whether the outstanding requests have been
     * handled once that's done.
     */
    xfer->eof = 1;

    return xfer;
}

int xfer_upload_ready(struct fxp_xfer *xfer)
{
    if (sftp_sendbuffer() == 0)
	return 1;
    else
	return 0;
}

void xfer_upload_data(struct fxp_xfer *xfer, char *buffer, int len)
{
    struct req *rr;
    struct sftp_request *req;

    rr = snew(struct req);
    rr->offset = xfer->offset;
    rr->complete = 0;
    if (xfer->tail) {
	xfer->tail->next = rr;
	rr->prev = xfer->tail;
    } else {
	xfer->head = rr;
	rr->prev = NULL;
    }
    xfer->tail = rr;
    rr->next = NULL;

    rr->len = len;
    rr->buffer = NULL;
    sftp_register(req = fxp_write_send(xfer->fh, buffer, rr->offset, len));
    fxp_set_userdata(req, rr);

    xfer->offset = uint64_add32(xfer->offset, rr->len);
    xfer->req_totalsize += rr->len;

#ifdef DEBUG_UPLOAD
    { char buf[40]; uint64_decimal(rr->offset, buf); printf("queueing write request %p at %s [len %d]\n", rr, buf, len); }
#endif
}

/*
 * Returns INT_MIN to indicate that it didn't even get as far as
 * fxp_write_recv and hence has not freed pktin.
 */
int xfer_upload_gotpkt(struct fxp_xfer *xfer, struct sftp_packet *pktin)
{
    struct sftp_request *rreq;
    struct req *rr, *prev, *next;
    int ret;

    rreq = sftp_find_request(pktin);
    if (!rreq)
        return INT_MIN;            /* this packet doesn't even make sense */
    rr = (struct req *)fxp_get_userdata(rreq);
    if (!rr) {
        fxp_internal_error("request ID is not part of the current upload");
	return INT_MIN;		       /* this packet isn't ours */
    }
    ret = fxp_write_recv(pktin, rreq);
#ifdef DEBUG_UPLOAD
    printf("write request %p has returned [%d]\n", rr, ret);
#endif

    /*
     * Remove this one from the queue.
     */
    prev = rr->prev;
    next = rr->next;
    if (prev)
	prev->next = next;
    else
	xfer->head = next;
    if (next)
	next->prev = prev;
    else
	xfer->tail = prev;
    xfer->req_totalsize -= rr->len;
    sfree(rr);

    if (!ret)
	return -1;

    return 1;
}

void xfer_cleanup(struct fxp_xfer *xfer)
{
    struct req *rr;
    while (xfer->head) {
	rr = xfer->head;
	xfer->head = xfer->head->next;
	sfree(rr->buffer);
	sfree(rr);
    }
    sfree(xfer);
}
