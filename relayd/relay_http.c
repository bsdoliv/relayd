/*	$OpenBSD: relay_http.c,v 1.18 2014/04/20 16:18:32 reyk Exp $	*/

/*
 * Copyright (c) 2006 - 2012 Reyk Floeter <reyk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/tree.h>
#include <sys/hash.h>

#include <net/if.h>
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <err.h>
#include <pwd.h>
#include <event.h>
#include <fnmatch.h>

#include <openssl/ssl.h>

#include "relayd.h"
#include "http.h"

static int	_relay_lookup_url(struct ctl_relay_event *, char *, char *,
		    char *, struct kv *);
int		 relay_lookup_url(struct ctl_relay_event *,
		    const char *, struct kv *);
int		 relay_lookup_query(struct ctl_relay_event *, struct kv *);
int		 relay_lookup_cookie(struct ctl_relay_event *, const char *,
		    struct kv *);
void		 relay_read_httpcontent(struct bufferevent *, void *);
void		 relay_read_httpchunks(struct bufferevent *, void *);
char		*relay_expand_http(struct ctl_relay_event *, char *,
		    char *, size_t);
int		 relay_writeheader_http(struct ctl_relay_event *,
		    struct ctl_relay_event *);
int		 relay_writerequest_http(struct ctl_relay_event *,
		    struct ctl_relay_event *);
int		 relay_writeresponse_http(struct ctl_relay_event *,
		    struct ctl_relay_event *);
void		 relay_reset_http(struct ctl_relay_event *);
static int	 relay_httpmethod_cmp(const void *, const void *);
static int	 relay_httpheader_cmp(const void *, const void *);
int		 relay_httpquery_test(struct ctl_relay_event *,
		    struct relay_rule *, struct kvlist *);
int		 relay_httpheader_test(struct ctl_relay_event *,
		    struct relay_rule *, struct kvlist *);
int		 relay_httppath_test(struct ctl_relay_event *,
		    struct relay_rule *, struct kvlist *);
int		 relay_httpurl_test(struct ctl_relay_event *,
		    struct relay_rule *, struct kvlist *);
int		 relay_httpcookie_test(struct ctl_relay_event *,
		    struct relay_rule *, struct kvlist *);
int		 relay_action(struct ctl_relay_event *, struct relay_rule *,
		    struct kvlist *);
void		 relay_httpdesc_free(struct http_descriptor *);

static struct relayd	*env = NULL;

static struct http_method	 http_methods[] = HTTP_METHODS;
static struct http_header	 http_headers[] = HTTP_HEADERS;

void
relay_http(struct relayd *x_env)
{
	if (x_env != NULL)
		env = x_env;

	DPRINTF("%s: sorting lookup tables, pid %d", __func__, getpid());

	/* Sort the HTTP lookup arrays */
	qsort(http_methods, sizeof(http_methods) /
	    sizeof(http_methods[0]) - 1,
	    sizeof(http_methods[0]), relay_httpmethod_cmp);
	qsort(http_headers, sizeof(http_headers) /
	    sizeof(http_headers[0]) - 1,
	    sizeof(http_headers[0]), relay_httpheader_cmp);
}

void
relay_http_init(struct relay *rlay)
{
	rlay->rl_proto->close = relay_close_http;

	relay_http(NULL);

	/* Calculate skip step for the filter rules (may take a while) */
	relay_calc_skip_steps(&rlay->rl_proto->rules);
}

int
relay_httpdesc_init(struct ctl_relay_event *cre)
{
	struct http_descriptor	*desc;

	if ((desc = calloc(1, sizeof(*desc))) == NULL)
		return (-1);
	TAILQ_INIT(&desc->http_headers);
	cre->desc = desc;

	return (0);
}

void
relay_httpdesc_free(struct http_descriptor *desc)
{
	int	i;

	if (desc->http_path != NULL) {
		free(desc->http_path);
		desc->http_path = NULL;
	}
	if (desc->http_query != NULL) {
		free(desc->http_query);
		desc->http_query = NULL;
	}
	if (desc->http_version != NULL) {
		free(desc->http_version);
		desc->http_version = NULL;
	}
	if (desc->query_key != NULL) {
		free(desc->query_key);
		desc->query_key = NULL;
	}
	if (desc->query_val != NULL) {
		free(desc->query_val);
		desc->query_val = NULL;
	}
	for (i = HTTP_HEADER_NONE; i < HTTP_HEADER_MAX; i++)
		desc->http_header[i] = NULL;
	kv_purge(&desc->http_headers);
}

void
relay_read_http(struct bufferevent *bev, void *arg)
{
	struct ctl_relay_event	*cre = arg;
	struct http_descriptor	*desc = cre->desc;
	struct rsession		*con = cre->con;
	struct relay		*rlay = con->se_relay;
	struct protocol		*proto = rlay->rl_proto;
	struct evbuffer		*src = EVBUFFER_INPUT(bev);
	char			*line = NULL, *key, *value;
	int			 header = 0, action;
	const char		*errstr;
	size_t			 size;
	struct kv		*hdr = NULL;
	enum httpheader		 hdrid;

	getmonotime(&con->se_tv_last);

	size = EVBUFFER_LENGTH(src);
	DPRINTF("%s: session %d: size %lu, to read %lld",
	    __func__, con->se_id, size, cre->toread);
	if (!size) {
		if (cre->dir == RELAY_DIR_RESPONSE)
			return;
		cre->toread = TOREAD_HTTP_HEADER;
		goto done;
	}

	while (!cre->done && (line = evbuffer_readline(src)) != NULL) {
		/*
		 * An empty line indicates the end of the request.
		 * libevent already stripped the \r\n for us.
		 */
		if (!strlen(line)) {
			cre->done = 1;
			free(line);
			break;
		}
		key = line;

		/*
		 * The first line is the GET/POST/PUT/... request,
		 * subsequent lines are HTTP headers.
		 */
		if (++cre->line == 1)
			value = strchr(key, ' ');
		else if (*key == ' ' || *key == '\t')
			/* Multiline headers wrap with a space or tab */
			value = NULL;
		else
			value = strchr(key, ':');
		if (value == NULL) {
			if (cre->line == 1) {
				free(line);
				relay_abort_http(con, 400, "malformed", 0);
				return;
			}

			/* Append line to the last header, if present */
			if (kv_extend(&desc->http_headers, line) == NULL) {
				free(line);
				goto fail;
			}

			free(line);
			continue;
		}
		if (*value == ':') {
			*value++ = '\0';
			value += strspn(value, " \t\r\n");
			header = 1;
		} else {
			*value++ = '\0';
			header = 0;
		}

		DPRINTF("%s: session %d: header '%s: %s'", __func__,
		    con->se_id, key, value);

		/*
		 * Identify and handle specific HTTP request methods
		 */
		if (cre->line == 1 && cre->dir == RELAY_DIR_RESPONSE) {
			desc->http_method = HTTP_METHOD_RESPONSE;
			/*
			 * Decode response path and query
			 */
			desc->http_version = strdup(line);
			if (desc->http_version == NULL) {
				free(line);
				goto fail;
			}
			desc->http_rescode = strdup(value);
			if (desc->http_rescode == NULL) {
				free(line);
				goto fail;
			}
			desc->http_resmesg = strchr(desc->http_rescode, ' ');
			if (desc->http_resmesg == NULL) {
				free(line);
				goto fail;
			}
			*desc->http_resmesg++ = '\0';
			if ((desc->http_resmesg = strdup(desc->http_resmesg))
			    == NULL) {
				free(line);
				goto fail;
			}
			DPRINTF("http_version %s http_rescode %s "
			    "http_resmesg %s", desc->http_version,
			    desc->http_rescode, desc->http_resmesg);
			goto lookup;
		} else if (cre->line == 1 && cre->dir == RELAY_DIR_REQUEST) {
			if ((desc->http_method = relay_httpmethod_byname(key))
			    == HTTP_METHOD_NONE)
				goto fail;
			/*
			 * Decode request path and query
			 */
			desc->http_path = strdup(value);
			if (desc->http_path == NULL) {
				free(line);
				goto fail;
			}
			desc->http_version = strchr(desc->http_path, ' ');
			if (desc->http_version != NULL)
				*desc->http_version++ = '\0';
			desc->http_query = strchr(desc->http_path, '?');
			if (desc->http_query != NULL)
				*desc->http_query++ = '\0';

			/*
			 * Have to allocate the strings because they could
			 * be changed independetly by the filters later.
			 */
			if (desc->http_version != NULL &&
			    (desc->http_version =
			    strdup(desc->http_version)) == NULL) {
				free(line);
				goto fail;
			}
			if (desc->http_query != NULL &&
			    (desc->http_query =
			    strdup(desc->http_query)) == NULL) {
				free(line);
				goto fail;
			}
		} else if (desc->http_method != HTTP_METHOD_NONE &&
		    strcasecmp("Content-Length", key) == 0) {
			if (desc->http_method == HTTP_METHOD_TRACE ||
			    desc->http_method == HTTP_METHOD_CONNECT) {
				/*
				 * These method should not have a body
				 * and thus no Content-Length header.
				 */
				relay_abort_http(con, 400, "malformed", 0);
				goto abort;
			}

			/*
			 * Need to read data from the client after the
			 * HTTP header.
			 * XXX What about non-standard clients not using
			 * the carriage return? And some browsers seem to
			 * include the line length in the content-length.
			 */
			cre->toread = strtonum(value, 0, LLONG_MAX,
			    &errstr);
			if (errstr) {
				relay_abort_http(con, 500, errstr, 0);
				goto abort;
			}
		}
 lookup:
		if (strcasecmp("Transfer-Encoding", key) == 0 &&
		    strcasecmp("chunked", value) == 0)
			desc->http_chunked = 1;

		if (cre->line != 1 &&
		    (hdr = kv_add(&desc->http_headers, key, value)) == NULL) {
			free(line);
			goto fail;
		}

		/*
		 * Remember the header if it is known to us for a quicker
		 * lookup later.
		 */
		if ((hdrid = relay_httpheader_byname(key)) !=
		    HTTP_HEADER_OTHER) {
			desc->http_header[hdrid] = hdr;
		}

		free(line);
	}
	if (cre->done) {
		if (desc->http_method == HTTP_METHOD_NONE) {
			relay_abort_http(con, 406, "no method", 0);
			return;
		}

		action = relay_test(proto, cre);
		if (action == RES_FAIL) {
			relay_close(con, "filter rule failed");
			return;
		} else if (action != RES_PASS) {
			relay_abort_http(con, 403, "Forbidden", con->se_label);
			return;
		}

		switch (desc->http_method) {
		case HTTP_METHOD_CONNECT:
			/* Data stream */
			cre->toread = TOREAD_UNLIMITED;
			bev->readcb = relay_read;
			break;
		case HTTP_METHOD_DELETE:
		case HTTP_METHOD_GET:
		case HTTP_METHOD_HEAD:
		case HTTP_METHOD_OPTIONS:
			cre->toread = 0;
			/* FALLTHROUGH */
		case HTTP_METHOD_POST:
		case HTTP_METHOD_PUT:
		case HTTP_METHOD_RESPONSE:
			/* HTTP request payload */
			if (cre->toread > 0)
				bev->readcb = relay_read_httpcontent;

			/* Single-pass HTTP body */
			if (cre->toread < 0) {
				cre->toread = TOREAD_UNLIMITED;
				bev->readcb = relay_read;
			}
			break;
		default:
			/* HTTP handler */
			cre->toread = TOREAD_HTTP_HEADER;
			bev->readcb = relay_read_http;
			break;
		}
		if (desc->http_chunked) {
			/* Chunked transfer encoding */
			cre->toread = TOREAD_HTTP_CHUNK_LENGTH;
			bev->readcb = relay_read_httpchunks;
		}

		if (cre->dir == RELAY_DIR_REQUEST) {
			if (relay_writerequest_http(cre->dst, cre) == -1)
			    goto fail;
		} else {
			if (relay_writeresponse_http(cre->dst, cre) == -1)
			    goto fail;
		}
		if (relay_bufferevent_print(cre->dst, "\r\n") == -1 ||
		    relay_writeheader_http(cre->dst, cre) == -1 ||
		    relay_bufferevent_print(cre->dst, "\r\n") == -1)
			goto fail;

		relay_reset_http(cre);
 done:
		if (cre->dir == RELAY_DIR_REQUEST && cre->toread <= 0 &&
		    cre->dst->bev == NULL) {
			if (rlay->rl_conf.fwdmode == FWD_TRANS) {
				relay_bindanyreq(con, 0, IPPROTO_TCP);
				return;
			}
			if (relay_connect(con) == -1)
				relay_abort_http(con, 502, "session failed", 0);
			return;
		}
	}
	if (con->se_done) {
		relay_close(con, "last http read (done)");
		return;
	}
	if (EVBUFFER_LENGTH(src) && bev->readcb != relay_read_http)
		bev->readcb(bev, arg);
	bufferevent_enable(bev, EV_READ);
	if (relay_splice(cre) == -1)
		relay_close(con, strerror(errno));
	return;
 fail:
	relay_abort_http(con, 500, strerror(errno), 0);
	return;
 abort:
	free(line);
}

void
relay_read_httpcontent(struct bufferevent *bev, void *arg)
{
	struct ctl_relay_event	*cre = arg;
	struct rsession		*con = cre->con;
	struct evbuffer		*src = EVBUFFER_INPUT(bev);
	size_t			 size;

	getmonotime(&con->se_tv_last);

	size = EVBUFFER_LENGTH(src);
	DPRINTF("%s: session %d: size %lu, to read %lld", __func__,
	    con->se_id, size, cre->toread);
	if (!size)
		return;
	if (relay_spliceadjust(cre) == -1)
		goto fail;

	if (cre->toread > 0) {
		/* Read content data */
		if ((off_t)size > cre->toread) {
			size = cre->toread;
			if (relay_bufferevent_write_chunk(cre->dst, src, size)
			    == -1)
				goto fail;
			cre->toread = 0;
		} else {
			if (relay_bufferevent_write_buffer(cre->dst, src) == -1)
				goto fail;
			cre->toread -= size;
		}
		DPRINTF("%s: done, size %lu, to read %lld", __func__,
		    size, cre->toread);
	}
	if (cre->toread == 0) {
		cre->toread = TOREAD_HTTP_HEADER;
		bev->readcb = relay_read_http;
	}
	if (con->se_done)
		goto done;
	if (bev->readcb != relay_read_httpcontent)
		bev->readcb(bev, arg);
	bufferevent_enable(bev, EV_READ);
	return;
 done:
	relay_close(con, "last http content read");
	return;
 fail:
	relay_close(con, strerror(errno));
}

void
relay_read_httpchunks(struct bufferevent *bev, void *arg)
{
	struct ctl_relay_event	*cre = arg;
	struct rsession		*con = cre->con;
	struct evbuffer		*src = EVBUFFER_INPUT(bev);
	char			*line;
	long long		 llval;
	size_t			 size;

	getmonotime(&con->se_tv_last);

	size = EVBUFFER_LENGTH(src);
	DPRINTF("%s: session %d: size %lu, to read %lld", __func__,
	    con->se_id, size, cre->toread);
	if (!size)
		return;
	if (relay_spliceadjust(cre) == -1)
		goto fail;

	if (cre->toread > 0) {
		/* Read chunk data */
		if ((off_t)size > cre->toread) {
			size = cre->toread;
			if (relay_bufferevent_write_chunk(cre->dst, src, size)
			    == -1)
				goto fail;
			cre->toread = 0;
		} else {
			if (relay_bufferevent_write_buffer(cre->dst, src) == -1)
				goto fail;
			cre->toread -= size;
		}
		DPRINTF("%s: done, size %lu, to read %lld", __func__,
		    size, cre->toread);
	}
	switch (cre->toread) {
	case TOREAD_HTTP_CHUNK_LENGTH:
		line = evbuffer_readline(src);
		if (line == NULL) {
			/* Ignore empty line, continue */
			bufferevent_enable(bev, EV_READ);
			return;
		}
		if (strlen(line) == 0) {
			free(line);
			goto next;
		}

		/*
		 * Read prepended chunk size in hex, ignore the trailer.
		 * The returned signed value must not be negative.
		 */
		if (sscanf(line, "%llx", &llval) != 1 || llval < 0) {
			free(line);
			relay_close(con, "invalid chunk size");
			return;
		}

		if (relay_bufferevent_print(cre->dst, line) == -1 ||
		    relay_bufferevent_print(cre->dst, "\r\n") == -1) {
			free(line);
			goto fail;
		}
		free(line);

		if ((cre->toread = llval) == 0) {
			DPRINTF("%s: last chunk", __func__);
			cre->toread = TOREAD_HTTP_CHUNK_TRAILER;
		}
		break;
	case TOREAD_HTTP_CHUNK_TRAILER:
		/* Last chunk is 0 bytes followed by trailer and empty line */
		line = evbuffer_readline(src);
		if (line == NULL) {
			/* Ignore empty line, continue */
			bufferevent_enable(bev, EV_READ);
			return;
		}
		if (relay_bufferevent_print(cre->dst, line) == -1 ||
		    relay_bufferevent_print(cre->dst, "\r\n") == -1) {
			free(line);
			goto fail;
		}
		if (strlen(line) == 0) {
			/* Switch to HTTP header mode */
			cre->toread = TOREAD_HTTP_HEADER;
			bev->readcb = relay_read_http;
		}
		free(line);
		break;
	case 0:
		/* Chunk is terminated by an empty newline */
		line = evbuffer_readline(src);
		if (line != NULL)
			free(line);
		if (relay_bufferevent_print(cre->dst, "\r\n") == -1)
			goto fail;
		cre->toread = TOREAD_HTTP_CHUNK_LENGTH;
		break;
	}

 next:
	if (con->se_done)
		goto done;
	if (EVBUFFER_LENGTH(src))
		bev->readcb(bev, arg);
	bufferevent_enable(bev, EV_READ);
	return;

 done:
	relay_close(con, "last http chunk read (done)");
	return;
 fail:
	relay_close(con, strerror(errno));
}

void
relay_reset_http(struct ctl_relay_event *cre)
{
	struct http_descriptor	*desc = cre->desc;

	relay_httpdesc_free(desc);
	if (cre->buf != NULL) {
		free(cre->buf);
		cre->buf = NULL;
		cre->buflen = 0;
	}
	desc->http_method = 0;
	desc->http_chunked = 0;
	cre->line = 0;
	cre->done = 0;
}

static int
_relay_lookup_url(struct ctl_relay_event *cre, char *host, char *path,
    char *query, struct kv *kv)
{
	struct rsession		*con = cre->con;
	char			*val, *md = NULL;
	int			 ret = RES_FAIL;
	const char		*str = NULL;

	if (asprintf(&val, "%s%s%s%s",
	    host, path,
	    query == NULL ? "" : "?",
	    query == NULL ? "" : query) == -1) {
		relay_abort_http(con, 500, "failed to allocate URL", 0);
		return (RES_FAIL);
	}

	switch (kv->kv_digest) {
	case DIGEST_SHA1:
	case DIGEST_MD5:
		if ((md = digeststr(kv->kv_digest,
		    val, strlen(val), NULL)) == NULL) {
			relay_abort_http(con, 500,
			    "failed to allocate digest", 0);
			goto fail;
		}
		str = md;
		break;
	case DIGEST_NONE:
		str = val;
		break;
	}

	DPRINTF("%s: session %d: %s, %s: %d", __func__, con->se_id,
	    str, kv->kv_key, strcasecmp(kv->kv_key, str));

	if (strcasecmp(kv->kv_key, str) == 0) {
		ret = RES_DROP;
		goto fail;
	}

	ret = RES_PASS;
 fail:
	if (md != NULL)
		free(md);
	free(val);
	return (ret);
}

int
relay_lookup_url(struct ctl_relay_event *cre, const char *host, struct kv *kv)
{
	struct rsession		*con = cre->con;
	struct http_descriptor	*desc = (struct http_descriptor *)cre->desc;
	int			 i, j, dots;
	char			*hi[RELAY_MAXLOOKUPLEVELS], *p, *pp, *c, ch;
	char			 ph[MAXHOSTNAMELEN];
	int			 ret;

	if (desc->http_path == NULL)
		return (RES_PASS);

	/*
	 * This is an URL lookup algorithm inspired by
	 * http://code.google.com/apis/safebrowsing/
	 *     developers_guide.html#PerformingLookups
	 */

	DPRINTF("%s: session %d: host '%s', path '%s', query '%s'",
	    __func__, con->se_id, host, desc->http_path,
	    desc->http_query == NULL ? "" : desc->http_query);

	if (canonicalize_host(host, ph, sizeof(ph)) == NULL) {
		relay_abort_http(con, 400, "invalid host name", 0);
		return (RES_FAIL);
	}

	bzero(hi, sizeof(hi));
	for (dots = -1, i = strlen(ph) - 1; i > 0; i--) {
		if (ph[i] == '.' && ++dots)
			hi[dots - 1] = &ph[i + 1];
		if (dots > (RELAY_MAXLOOKUPLEVELS - 2))
			break;
	}
	if (dots == -1)
		dots = 0;
	hi[dots] = ph;

	if ((pp = strdup(desc->http_path)) == NULL) {
		relay_abort_http(con, 500, "failed to allocate path", 0);
		return (RES_FAIL);
	}
	for (i = (RELAY_MAXLOOKUPLEVELS - 1); i >= 0; i--) {
		if (hi[i] == NULL)
			continue;

		/* 1. complete path with query */
		if (desc->http_query != NULL)
			if ((ret = _relay_lookup_url(cre, hi[i],
			    pp, desc->http_query, kv)) != RES_PASS)
				goto done;

		/* 2. complete path without query */
		if ((ret = _relay_lookup_url(cre, hi[i],
		    pp, NULL, kv)) != RES_PASS)
			goto done;

		/* 3. traverse path */
		for (j = 0, p = strchr(pp, '/');
		    p != NULL; p = strchr(p, '/'), j++) {
			if (j > (RELAY_MAXLOOKUPLEVELS - 2) || ++p == '\0')
				break;
			c = &pp[p - pp];
			ch = *c;
			*c = '\0';
			if ((ret = _relay_lookup_url(cre, hi[i],
			    pp, NULL, kv)) != RES_PASS)
				goto done;
			*c = ch;
		}
	}

	ret = RES_PASS;
 done:
	free(pp);
	return (ret);
}

int
relay_lookup_cookie(struct ctl_relay_event *cre, const char *str,
    struct kv *kv)
{
	struct rsession		*con = cre->con;
	char			*val, *ptr, *key, *value;
	int			 ret;

	if ((val = strdup(str)) == NULL) {
		relay_abort_http(con, 500, "failed to allocate cookie", 0);
		return (RES_FAIL);
	}

	for (ptr = val; ptr != NULL && strlen(ptr);) {
		if (*ptr == ' ')
			*ptr++ = '\0';
		key = ptr;
		if ((ptr = strchr(ptr, ';')) != NULL)
			*ptr++ = '\0';
		/*
		 * XXX We do not handle attributes
		 * ($Path, $Domain, or $Port)
		 */
		if (*key == '$')
			continue;

		if ((value =
		    strchr(key, '=')) == NULL ||
		    strlen(value) < 1)
			continue;
		*value++ = '\0';
		if (*value == '"')
			*value++ = '\0';
		if (value[strlen(value) - 1] == '"')
			value[strlen(value) - 1] = '\0';

		DPRINTF("%s: session %d: %s = %s, %s = %s : %d",
		    __func__, con->se_id,
		    key, value, kv->kv_key, kv->kv_value,
		    strcasecmp(kv->kv_key, key));

		if (strcasecmp(kv->kv_key, key) == 0 &&
		    ((kv->kv_value == NULL) ||
		    (fnmatch(kv->kv_value, value,
		    FNM_CASEFOLD) != FNM_NOMATCH))) {
			ret = RES_DROP;
			goto done;
		}
	}

	ret = RES_PASS;

 done:
	free(val);
	return (ret);
}

int
relay_lookup_query(struct ctl_relay_event *cre, struct kv *kv)
{
	struct http_descriptor	*desc = cre->desc;
	struct kv		*match = &desc->http_matchquery;
	char			*val, *ptr, *tmpkey = NULL, *tmpval = NULL;
	int			 ret = -1;

	if ((val = strdup(desc->http_query)) == NULL) {
		relay_abort_http(cre->con, 500, "failed to allocate query", 0);
		return (-1);
	}

	ptr = val;
	while (ptr != NULL && strlen(ptr)) {
		tmpkey = ptr;
		if ((ptr = strchr(ptr, '&')) != NULL)
			*ptr++ = '\0';
		if ((tmpval = strchr(tmpkey, '=')) == NULL || strlen(tmpval)
		    < 1)
			continue;
		*tmpval++ = '\0';

		if (fnmatch(kv->kv_key, tmpkey, 0) != FNM_NOMATCH &&
		    (kv->kv_value == NULL || fnmatch(kv->kv_value, tmpval, 0)
		     != FNM_NOMATCH))
			break;
		else
			tmpkey = NULL;
	}

	if (tmpkey == NULL || tmpval == NULL)
		goto done;

	match->kv_key = strdup(tmpkey);
	if (match->kv_key == NULL)
		goto done;
	match->kv_value = strdup(tmpval);
	if (match->kv_key == NULL)
		goto done;
	ret = 0;

done:
	free(val);
	return (ret);
}


void
relay_abort_http(struct rsession *con, u_int code, const char *msg,
    u_int16_t labelid)
{
	struct relay		*rlay = con->se_relay;
	struct bufferevent	*bev = con->se_in.bev;
	const char		*httperr = print_httperror(code), *text = "";
	char			*httpmsg;
	time_t			 t;
	struct tm		*lt;
	char			 tmbuf[32], hbuf[128];
	const char		*style, *label = NULL;

	if (labelid != 0)
		label = label_id2name(labelid);

	/* In some cases this function may be called from generic places */
	if (rlay->rl_proto->type != RELAY_PROTO_HTTP ||
	    (rlay->rl_proto->flags & F_RETURN) == 0) {
		relay_close(con, msg);
		return;
	}

	if (bev == NULL)
		goto done;

	/* Some system information */
	if (print_host(&rlay->rl_conf.ss, hbuf, sizeof(hbuf)) == NULL)
		goto done;

	/* RFC 2616 "tolerates" asctime() */
	time(&t);
	lt = localtime(&t);
	tmbuf[0] = '\0';
	if (asctime_r(lt, tmbuf) != NULL)
		tmbuf[strlen(tmbuf) - 1] = '\0';	/* skip final '\n' */

	/* Do not send details of the Internal Server Error */
	if (code != 500)
		text = msg;

	/* A CSS stylesheet allows minimal customization by the user */
	if ((style = rlay->rl_proto->style) == NULL)
		style = "body { background-color: #a00000; color: white; }";

	/* Generate simple HTTP+HTML error document */
	if (asprintf(&httpmsg,
	    "HTTP/1.0 %03d %s\r\n"
	    "Date: %s\r\n"
	    "Server: %s\r\n"
	    "Connection: close\r\n"
	    "Content-Type: text/html\r\n"
	    "\r\n"
	    "<!DOCTYPE HTML PUBLIC "
	    "\"-//W3C//DTD HTML 4.01 Transitional//EN\">\n"
	    "<html>\n"
	    "<head>\n"
	    "<title>%03d %s</title>\n"
	    "<style type=\"text/css\"><!--\n%s\n--></style>\n"
	    "</head>\n"
	    "<body>\n"
	    "<h1>%s</h1>\n"
	    "<div id='m'>%s</div>\n"
	    "<div id='l'>%s</div>\n"
	    "<hr><address>%s at %s port %d</address>\n"
	    "</body>\n"
	    "</html>\n",
	    code, httperr, tmbuf, RELAYD_SERVERNAME,
	    code, httperr, style, httperr, text,
	    label == NULL ? "" : label,
	    RELAYD_SERVERNAME, hbuf, ntohs(rlay->rl_conf.port)) == -1)
		goto done;

	/* Dump the message without checking for success */
	relay_dump(&con->se_in, httpmsg, strlen(httpmsg));
	free(httpmsg);

 done:
	if (asprintf(&httpmsg, "%s (%03d %s)", msg, code, httperr) == -1)
		relay_close(con, msg);
	else {
		relay_close(con, httpmsg);
		free(httpmsg);
	}
}

void
relay_close_http(struct rsession *con)
{
	struct http_descriptor	*desc[2] = {
		con->se_in.desc, con->se_out.desc
	};
	int			 i;

	for (i = 0; i < 2; i++) {
		if (desc[i] == NULL)
			continue;
		relay_httpdesc_free(desc[i]);
		free(desc[i]);
	}
}

char *
relay_expand_http(struct ctl_relay_event *cre, char *val, char *buf, size_t len)
{
	struct rsession	*con = cre->con;
	struct relay	*rlay = con->se_relay;
	char		 ibuf[128];

	if (strlcpy(buf, val, len) >= len)
		return (NULL);

	if (strstr(val, "$REMOTE_") != NULL) {
		if (strstr(val, "$REMOTE_ADDR") != NULL) {
			if (print_host(&cre->ss, ibuf, sizeof(ibuf)) == NULL)
				return (NULL);
			if (expand_string(buf, len,
			    "$REMOTE_ADDR", ibuf) != 0)
				return (NULL);
		}
		if (strstr(val, "$REMOTE_PORT") != NULL) {
			snprintf(ibuf, sizeof(ibuf), "%u", ntohs(cre->port));
			if (expand_string(buf, len,
			    "$REMOTE_PORT", ibuf) != 0)
				return (NULL);
		}
	}
	if (strstr(val, "$SERVER_") != NULL) {
		if (strstr(val, "$SERVER_ADDR") != NULL) {
			if (print_host(&rlay->rl_conf.ss,
			    ibuf, sizeof(ibuf)) == NULL)
				return (NULL);
			if (expand_string(buf, len,
			    "$SERVER_ADDR", ibuf) != 0)
				return (NULL);
		}
		if (strstr(val, "$SERVER_PORT") != NULL) {
			snprintf(ibuf, sizeof(ibuf), "%u",
			    ntohs(rlay->rl_conf.port));
			if (expand_string(buf, len,
			    "$SERVER_PORT", ibuf) != 0)
				return (NULL);
		}
		if (strstr(val, "$SERVER_NAME") != NULL) {
			if (expand_string(buf, len,
			    "$SERVER_NAME", RELAYD_SERVERNAME) != 0)
				return (NULL);
		}
	}
	if (strstr(val, "$TIMEOUT") != NULL) {
		snprintf(ibuf, sizeof(ibuf), "%lld",
		    (long long)rlay->rl_conf.timeout.tv_sec);
		if (expand_string(buf, len, "$TIMEOUT", ibuf) != 0)
			return (NULL);
	}

	return (buf);
}

int
relay_writerequest_http(struct ctl_relay_event *dst,
    struct ctl_relay_event *cre)
{
	struct http_descriptor	*desc = (struct http_descriptor *)cre->desc;
	const char		*name = NULL;

	if ((name = relay_httpmethod_byid(desc->http_method)) == NULL)
		return (-1);

	if (relay_bufferevent_print(dst, name) == -1 ||
	    relay_bufferevent_print(dst, " ") == -1 ||
	    relay_bufferevent_print(dst, desc->http_path) == -1 ||
	    (desc->http_query != NULL &&
	    (relay_bufferevent_print(dst, "?") == -1 ||
	    relay_bufferevent_print(dst, desc->http_query) == -1)) ||
	    relay_bufferevent_print(dst, " ") == -1 ||
	    relay_bufferevent_print(dst, desc->http_version) == -1)
		return (-1);

	return (0);
}

int
relay_writeresponse_http(struct ctl_relay_event *dst,
    struct ctl_relay_event *cre)
{
	struct http_descriptor	*desc = (struct http_descriptor *)cre->desc;

	DPRINTF("version: %s rescode: %s resmsg: %s", desc->http_version,
	    desc->http_rescode, desc->http_resmesg);

	if (relay_bufferevent_print(dst, desc->http_version) == -1 ||
	    relay_bufferevent_print(dst, " ") == -1 ||
	    relay_bufferevent_print(dst, desc->http_rescode) == -1 ||
	    relay_bufferevent_print(dst, " ") == -1 ||
	    relay_bufferevent_print(dst, desc->http_resmesg) == -1)
		return (-1);

	return (0);
}

int
relay_writeheader_http(struct ctl_relay_event *dst, struct ctl_relay_event *cre)
{
	struct kv		*hdr;
	char			 buf[IBUF_READ_SIZE], *ptr;
	struct http_descriptor	*desc = (struct http_descriptor *)cre->desc;

	TAILQ_FOREACH(hdr, &desc->http_headers, kv_entry) {
		ptr = hdr->kv_value;
		if (hdr->kv_action == KEY_ACTION_APPEND ||
		    hdr->kv_action == KEY_ACTION_SET)
			if ((ptr = relay_expand_http(cre, hdr->kv_value, buf,
			    sizeof(buf))) == NULL)
			    return (-1);

		DPRINTF("%s: ptr %s", __func__, ptr);
		if (relay_bufferevent_print(dst, hdr->kv_key) == -1 ||
		    (ptr != NULL &&
		    (relay_bufferevent_print(dst, ": ") == -1 ||
		    relay_bufferevent_print(dst, ptr) == -1 ||
		    relay_bufferevent_print(dst, "\r\n") == -1)))
			return (-1);
		DPRINTF("%s: %s: %s", __func__, hdr->kv_key,
			hdr->kv_value == NULL ? "" : hdr->kv_value);
	}

	return (0);
}

enum httpmethod
relay_httpmethod_byname(const char *name)
{
	enum httpmethod		 id = HTTP_METHOD_NONE;
	struct http_method	 method, *res = NULL;

	/* Set up key */
	method.method_name = name;

	/*
	 * RFC 2616 section 5.1.1 says that the method is case
	 * sensitive so we don't do a strcasecmp here.
	 */
	if ((res = bsearch(&method, http_methods,
	    sizeof(http_methods) / sizeof(http_methods[0]) - 1,
	    sizeof(http_methods[0]), relay_httpmethod_cmp)) != NULL)
		id = res->method_id;

	return (id);
}

const char *
relay_httpmethod_byid(u_int id)
{
	const char	*name = NULL;
	int		 i;

	for (i = 0; http_methods[i].method_name != NULL; i++) {
		if (http_methods[i].method_id == id) {
			name = http_methods[i].method_name;
			break;
		}
	}

	return (name);
}

u_int
relay_httpheader_byname(const char *name)
{
	enum httpheader		 id = HTTP_HEADER_OTHER;
	struct http_header	 header, *res = NULL;

	/* Set up key */
	header.header_name = name;

	/*
	 * In contrast to HTTP methods, HTTP header names are
	 * case-insensitive (see RFC 2616 section 2.4).
	 */
	if ((res = bsearch(&header, http_headers,
	    sizeof(http_headers) / sizeof(http_headers[0]) - 1,
	    sizeof(http_headers[0]), relay_httpheader_cmp)) != NULL)
		id = res->header_id;

	return (id);
}

static int
relay_httpmethod_cmp(const void *a, const void *b)
{
	const struct http_method *ma = a;
	const struct http_method *mb = b;
	return (strcmp(ma->method_name, mb->method_name));
}

static int
relay_httpheader_cmp(const void *a, const void *b)
{
	const struct http_header *ha = a;
	const struct http_header *hb = b;
	return (strcasecmp(ha->header_name, hb->header_name));
}

int
relay_httpquery_test(struct ctl_relay_event *cre, struct relay_rule *rule,
    struct kvlist *actions)
{
	struct http_descriptor	*desc = cre->desc;
	struct kv		*match = &desc->http_matchquery;
	struct kv		*kv = &rule->rule_kv[KEY_TYPE_QUERY];

	if (cre->dir == RELAY_DIR_RESPONSE || kv->kv_type != KEY_TYPE_QUERY)
		return (0);
	else if (kv->kv_key == NULL)
		return (0);
	else if (relay_lookup_query(cre, kv))
		return (-1);

	relay_match(actions, kv, match, NULL);

	return (0);
}

int
relay_httpheader_test(struct ctl_relay_event *cre, struct relay_rule *rule,
    struct kvlist *actions)
{
	struct http_descriptor	*desc = cre->desc;
	struct kv		*kv = &rule->rule_kv[KEY_TYPE_HEADER];
	struct kv		*match;
	u_int			 id = kv->kv_header_id;
	const char		*value;

	kv->kv_matchptr = NULL;

	if (id == HTTP_HEADER_NONE || kv->kv_type != KEY_TYPE_HEADER)
		return (0);
	else if (id < HTTP_HEADER_MAX) {
		match = desc->http_header[id];
		DPRINTF("%s: standard header %s: %p", __func__,
		    kv->kv_key, match);
		if (match != NULL)
			kv->kv_matchptr = &desc->http_header[id];
	} else {
		DPRINTF("%s: other header %s", __func__, kv->kv_key);
		TAILQ_FOREACH_REVERSE(match, &desc->http_headers,
		    kvlist, kv_entry) {
			if (strcasecmp(match->kv_key, kv->kv_key) == 0)
				goto done;
		}
		match = NULL;
	}

 done:
	if (kv->kv_action == KEY_ACTION_APPEND ||
	    kv->kv_action == KEY_ACTION_SET) {
		/* header can be NULL and will be added later */
	} else if (match == NULL) {
		/* Fail if header doesn't exist */
		return (-1);
	} else if (kv->kv_value != NULL) {
		/* Test header value using shell globbing rules */
		value = match->kv_value == NULL ? "" : match->kv_value;
		if (fnmatch(kv->kv_value, value, FNM_CASEFOLD) == FNM_NOMATCH)
			return (-1);
	}

	relay_match(actions, kv, match, &desc->http_headers);

	return (0);
}

int
relay_httppath_test(struct ctl_relay_event *cre, struct relay_rule *rule,
    struct kvlist *actions)
{
	struct http_descriptor	*desc = cre->desc;
	struct kv		*kv = &rule->rule_kv[KEY_TYPE_PATH];
	struct kv		*match = &desc->http_pathquery;
	const char		*query;

	if (cre->dir == RELAY_DIR_RESPONSE || kv->kv_type != KEY_TYPE_PATH)
		return (0);
	else if (kv->kv_key == NULL)
		return (0);
	else if (fnmatch(kv->kv_key, desc->http_path, 0) == FNM_NOMATCH)
		return (-1);
	else if (kv->kv_value != NULL && kv->kv_action == KEY_ACTION_NONE) {
		query = desc->http_query == NULL ? "" : desc->http_query;
		if (fnmatch(kv->kv_value, query, FNM_CASEFOLD) == FNM_NOMATCH)
			return (-1);
	}

	relay_match(actions, kv, match, NULL);

	return (0);
}

int
relay_httpurl_test(struct ctl_relay_event *cre, struct relay_rule *rule,
    struct kvlist *actions)
{
	struct http_descriptor	*desc = cre->desc;
	struct kv		*host = desc->http_header[HTTP_HEADER_HOST];
	struct kv		*kv = &rule->rule_kv[KEY_TYPE_URL];
	struct kv		*match = &desc->http_pathquery;

	if (cre->dir == RELAY_DIR_RESPONSE || kv->kv_type != KEY_TYPE_URL)
		return (0);
	else if (kv->kv_key == NULL || host == NULL || host->kv_value == NULL)
		return (0);
	else if (relay_lookup_url(cre, host->kv_value, kv) != 0)
		return (-1);

	relay_match(actions, kv, match, NULL);

	return (0);
}

int
relay_httpcookie_test(struct ctl_relay_event *cre, struct relay_rule *rule,
    struct kvlist *actions)
{
	struct http_descriptor	*desc = cre->desc;
	struct kv		*kv = &rule->rule_kv[KEY_TYPE_COOKIE];
	struct kv		*cookie = NULL;
	struct kv		*match = NULL;

	if (kv->kv_type != KEY_TYPE_COOKIE)
		return (0);

	switch (cre->dir) {
	case RELAY_DIR_REQUEST:
		cookie = desc->http_header[HTTP_HEADER_COOKIE];
		break;
	case RELAY_DIR_RESPONSE:
		cookie = desc->http_header[HTTP_HEADER_SET_COOKIE];
		break;
	case RELAY_DIR_ANY:
		return (0);
		/* NOTREACHED */
	default:
		break;
	}

	match = cookie;
	if (kv->kv_key == NULL || cookie == NULL ||
	    cookie->kv_value == NULL)
		return (0);
	else if (relay_lookup_cookie(cre, cookie->kv_value, kv) != 0)
		return (-1);

	relay_match(actions, kv, match, NULL);

	return (0);
}

int
relay_action(struct ctl_relay_event *cre, struct relay_rule *rule,
    struct kvlist *actions)
{
	struct rsession		*con = cre->con;
	struct http_descriptor	*desc = cre->desc;
	struct relay		*rlay = (struct relay *)con->se_relay;
	struct relay_table	*rlt = NULL;
	char			 pname[TABLE_NAME_SIZE];
	const char		*value;
	struct kv		*kv, *match;
	int			 httpindex, addkv;
	enum httpheader		 hdrid;

	if (rule->rule_flags & RULE_FLAG_FORWARD_TABLE) {
		con->se_table = NULL;
		TAILQ_FOREACH(rlt, &rlay->rl_tables, rlt_entry) {
			/*
			 * XXX This should be replaced by a faster lookup
			 * method.
			 */
			if (strlcpy(pname,
			    rlt->rlt_table->conf.name,
			    sizeof(pname)) >= sizeof(pname))
				continue;
			pname[strcspn(pname, ":")] = '\0';
			if (strcmp(pname,
			    rule->rule_tablename) == 0) {
				DPRINTF("%s: session %d:"
				    " selected table %s",
				    __func__, con->se_id,
				    rlt->rlt_table->conf.name);
				con->se_table = rlt;
				break;
			}
		}
	}

	if (rule->rule_tag)
		con->se_tag = rule->rule_tag;

	if (rule->rule_label)
		con->se_label = rule->rule_label;

	TAILQ_FOREACH(kv, actions, kv_entry) {
		match = kv->kv_match;
		httpindex = addkv = 0;

		switch (kv->kv_action) {
		case KEY_ACTION_APPEND:
			switch (kv->kv_type) {
			case KEY_TYPE_PATH:
				if (kv_setkey(match, "%s%s", match->kv_key,
				    kv->kv_key) == -1)
					return (-1);
				break;
			default:
				if (match == NULL)
					addkv = 1;
				else if (match->kv_value == NULL &&
				    kv_set(match, "%s", kv->kv_value) == -1)
					return (-1);
				else if (kv_set(match, "%s,%s",
				    match->kv_value, kv->kv_value) == -1)
					return (-1);
				httpindex = 1;
				break;
			}
			break;
		case KEY_ACTION_SET:
			switch (kv->kv_type) {
			case KEY_TYPE_COOKIE:
				if (kv_setkey(match, "%s", kv->kv_key) == -1 ||
				    kv_set(match, "%s", kv->kv_value) == -1)
					return (-1);
				break;
			case KEY_TYPE_PATH:
				if (kv_setkey(match, "%s", kv->kv_value) == -1)
					return (-1);
				break;
			default:
				if (match == NULL)
					addkv = 1;
				else if (kv_set(match, "%s",
				    kv->kv_value) == -1)
					return (-1);
				httpindex = 1;
				break;
			}
			break;
		case KEY_ACTION_REMOVE:
			switch (kv->kv_type) {
			case KEY_TYPE_PATH:
				if (kv_setkey(match, "/") == -1)
					return (-1);
				break;
			default:
				if (kv->kv_matchlist != NULL)
					kv_delete(kv->kv_matchlist, match);
				else {
					free(match->kv_value);
					match->kv_value = NULL;
				}
				if (kv->kv_matchptr != NULL)
					*kv->kv_matchptr = NULL;
				break;
			}
			break;
		case KEY_ACTION_HASH:
			switch (kv->kv_type) {
			case KEY_TYPE_PATH:
				value = match->kv_key;
				break;
			default:
				value = match->kv_value;
				break;
			}
			if (!con->se_hashkeyset)
				con->se_hashkey = HASHINIT;
			con->se_hashkey = hash32_str(value, con->se_hashkey);
			con->se_hashkeyset = 1;
			log_debug("%s: hashkey 0x%04x", __func__,
			    con->se_hashkey);
			break;
		case KEY_ACTION_LOG:
			switch (kv->kv_type) {
			case KEY_TYPE_URL:
				if (kv_setkey(match, "%s", kv->kv_key) == -1)
					return (-1);
				break;
			default:
				break;
			}
			kv_log(con->se_log, match, rule->rule_label);
			break;
		default:
			fatalx("relay_action: invalid action");
			/* NOTREACHED */
		}

		if (addkv && kv->kv_matchlist != NULL) {
			/* Add new entry to the list (eg. new HTTP header) */
			if ((match = kv_add(kv->kv_matchlist,
			    kv->kv_key, kv->kv_value)) == NULL)
				return (-1);
			match->kv_action = kv->kv_action;
			match->kv_type = kv->kv_type;
			kv->kv_match = match;
		}

		if (httpindex && kv->kv_matchptr != NULL) {
			/* Re-index the fast lookup method */
			if ((hdrid = relay_httpheader_byname(kv->kv_key)) ==
			    HTTP_HEADER_OTHER) {
				match->kv_action = kv->kv_action;
				match->kv_type = kv->kv_type;
				desc->http_header[hdrid] = match;
				kv->kv_matchptr = &desc->http_header[hdrid];
			} else
				kv->kv_matchptr = NULL;
		}
	}

	return (0);
}

#define	RELAY_GET_SKIP_STEP(i)						\
	do {								\
		r = r->rule_skip[i];					\
		DPRINTF("%s:%d: skip %d rules", __func__, __LINE__, i);	\
	} while (0)

#define	RELAY_GET_NEXT_STEP						\
	do {								\
		DPRINTF("%s:%d: next rule", __func__, __LINE__);	\
		goto nextrule;						\
	} while (0)

int
relay_test(struct protocol *proto, struct ctl_relay_event *cre)
{
	struct rsession		*con;
	struct http_descriptor	*desc = cre->desc;
	struct relay_rule	*r = NULL, *rule = NULL;
	u_int			 cnt = 0;
	u_int			 action = RES_PASS;
	struct kvlist		 actions;

	con = cre->con;

	DPRINTF("%s: session %d: start", __func__, con->se_id);

	r = TAILQ_FIRST(&proto->rules);
	while (r != NULL) {
		cnt++;
		TAILQ_INIT(&actions);
		if (r->rule_dir && r->rule_dir != cre->dir)
			RELAY_GET_SKIP_STEP(RULE_SKIP_DIR);
		else if (proto->type != r->rule_proto)
			RELAY_GET_SKIP_STEP(RULE_SKIP_PROTO);
		else if (r->rule_af != AF_UNSPEC &&
		    cre->ss.ss_family != r->rule_af)
			RELAY_GET_SKIP_STEP(RULE_SKIP_AF);
		else if (RELAY_ADDR_CMP(&r->rule_src, &cre->ss) != 0)
			RELAY_GET_SKIP_STEP(RULE_SKIP_SRC);
		else if (RELAY_ADDR_CMP(&r->rule_dst, &cre->ss) != 0)
			RELAY_GET_SKIP_STEP(RULE_SKIP_DST);
		else if (r->rule_method != HTTP_METHOD_NONE &&
		    (cre->dir != RELAY_DIR_REQUEST ||
		    desc->http_method != r->rule_method))
			RELAY_GET_SKIP_STEP(RULE_SKIP_METHOD);
		else if (r->rule_tagged && con->se_tag != r->rule_tagged)
			RELAY_GET_NEXT_STEP;
		else if (relay_httpheader_test(cre, r, &actions) != 0)
			RELAY_GET_NEXT_STEP;
		else if (relay_httpquery_test(cre, r, &actions) != 0)
			RELAY_GET_NEXT_STEP;
		else if (relay_httppath_test(cre, r, &actions) != 0)
			RELAY_GET_NEXT_STEP;
		else if (relay_httpurl_test(cre, r, &actions) != 0)
			RELAY_GET_NEXT_STEP;
		else if (relay_httpcookie_test(cre, r, &actions) != 0)
			RELAY_GET_NEXT_STEP;
		else {
			/* Rule matched */
			rule = r;

			DPRINTF("%s: session %d: matched rule %d",
			    __func__, con->se_id, rule->rule_id);

			if (relay_action(cre, r, &actions) != 0) {
				/* Something bad happened, drop connection */
				action = RES_DROP;
				break;
			}

			if (r->rule_type == RULE_TYPE_BLOCK)
				action = RES_DROP;
			else if (r->rule_type == RULE_TYPE_PASS)
				action = RES_PASS;

			if (rule->rule_flags & RULE_FLAG_QUICK)
				break;

nextrule:
			/* Continue to find last matching policy */
			r = TAILQ_NEXT(r, rule_entry);
		}
	}

	DPRINTF("%s: session %d: action %d", __func__,
	    con->se_id, action);

	return (action);
}

#define	RELAY_SET_SKIP_STEPS(i)						\
	do {								\
		while (head[i] != cur) {				\
			head[i]->rule_skip[i] = cur;			\
			head[i] = TAILQ_NEXT(head[i], rule_entry);	\
		}							\
	} while (0)

/* This code is derived from pf_calc_skip_steps() from pf.c */
void
relay_calc_skip_steps(struct relay_rules *rules)
{
	struct relay_rule	*head[RULE_SKIP_COUNT], *cur, *prev;
	int			 i;

	cur = TAILQ_FIRST(rules);
	prev = cur;
	for (i = 0; i < RULE_SKIP_COUNT; ++i)
		head[i] = cur;
	while (cur != NULL) {
		if (cur->rule_dir != prev->rule_dir)
			RELAY_SET_SKIP_STEPS(RULE_SKIP_DIR);
		else if (cur->rule_proto != prev->rule_proto)
			RELAY_SET_SKIP_STEPS(RULE_SKIP_PROTO);
		else if (cur->rule_af != prev->rule_af)
			RELAY_SET_SKIP_STEPS(RULE_SKIP_AF);
		else if (RELAY_ADDR_NEQ(&cur->rule_src, &prev->rule_src))
			RELAY_SET_SKIP_STEPS(RULE_SKIP_SRC);
		else if (RELAY_ADDR_NEQ(&cur->rule_dst, &prev->rule_dst))
			RELAY_SET_SKIP_STEPS(RULE_SKIP_DST);
		else if (cur->rule_method != prev->rule_method)
			RELAY_SET_SKIP_STEPS(RULE_SKIP_METHOD);

		prev = cur;
		cur = TAILQ_NEXT(cur, rule_entry);
	}
	for (i = 0; i < RULE_SKIP_COUNT; ++i)
		RELAY_SET_SKIP_STEPS(i);
}

void
relay_match(struct kvlist *actions, struct kv *kv, struct kv *match,
    struct kvlist *matchlist)
{
	if (kv->kv_action != KEY_ACTION_NONE) {
		kv->kv_match = match;
		kv->kv_matchlist = matchlist;
		TAILQ_INSERT_TAIL(actions, kv, kv_entry);
	}
}
