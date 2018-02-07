
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_router.h>
#include <nxt_http.h>


static void nxt_h1p_read_header(nxt_task_t *task, void *obj, void *data);
static void nxt_h1p_header_parse(nxt_task_t *task, void *obj, void *data);
static nxt_int_t nxt_h1p_connection(void *ctx, nxt_http_field_t *field,
    uintptr_t data);
static nxt_int_t nxt_h1p_transfer_encoding(void *ctx, nxt_http_field_t *field,
    uintptr_t data);
static void nxt_h1p_request_body_read(nxt_task_t *task, nxt_http_request_t *r);
static void nxt_h1p_body_read(nxt_task_t *task, void *obj, void *data);
static void nxt_h1p_request_local_addr(nxt_task_t *task, nxt_http_request_t *r);
static void nxt_h1p_request_header_send(nxt_task_t *task,
    nxt_http_request_t *r);
static void nxt_h1p_request_send(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *out);
static nxt_buf_t *nxt_h1p_chunk_create(nxt_task_t *task, nxt_http_request_t *r,
    nxt_buf_t *out);
static void nxt_h1p_sent(nxt_task_t *task, void *obj, void *data);
static void nxt_h1p_request_close(nxt_task_t *task, nxt_http_proto_t proto);
static void nxt_h1p_keepalive(nxt_task_t *task, nxt_h1proto_t *h1p,
    nxt_conn_t *c);
static void nxt_h1p_close(nxt_task_t *task, nxt_conn_t *c);
static void nxt_h1p_conn_close(nxt_task_t *task, void *obj, void *data);
static void nxt_h1p_conn_error(nxt_task_t *task, void *obj, void *data);
static void nxt_h1p_conn_timeout(nxt_task_t *task, void *obj, void *data);
static nxt_msec_t nxt_h1p_timeout_value(nxt_conn_t *c, uintptr_t data);


static const nxt_conn_state_t  nxt_h1p_idle_state;
static const nxt_conn_state_t  nxt_h1p_read_header_state;
static const nxt_conn_state_t  nxt_h1p_read_body_state;
static const nxt_conn_state_t  nxt_h1p_send_state;


const nxt_http_proto_body_read_t  nxt_http_proto_body_read[3] = {
    nxt_h1p_request_body_read,
    NULL,
    NULL,
};


const nxt_http_proto_local_addr_t  nxt_http_proto_local_addr[3] = {
    nxt_h1p_request_local_addr,
    NULL,
    NULL,
};


const nxt_http_proto_header_send_t  nxt_http_proto_header_send[3] = {
    nxt_h1p_request_header_send,
    NULL,
    NULL,
};


const nxt_http_proto_send_t  nxt_http_proto_send[3] = {
    nxt_h1p_request_send,
    NULL,
    NULL,
};


const nxt_http_proto_close_t  nxt_http_proto_close[3] = {
    nxt_h1p_request_close,
    NULL,
    NULL,
};


static nxt_lvlhsh_t            nxt_h1p_fields_hash;

static nxt_http_field_proc_t   nxt_h1p_fields[] = {
    { nxt_string("Connection"),        &nxt_h1p_connection, 0 },
    { nxt_string("Transfer-Encoding"), &nxt_h1p_transfer_encoding, 0 },

    { nxt_string("Host"),              &nxt_http_request_host, 0 },
    { nxt_string("Cookie"),            &nxt_http_request_field,
        offsetof(nxt_http_request_t, cookie) },
    { nxt_string("Content-Type"),      &nxt_http_request_field,
        offsetof(nxt_http_request_t, content_type) },
    { nxt_string("Content-Length"),    &nxt_http_request_content_length, 0 },
};


nxt_int_t
nxt_h1p_init(nxt_task_t *task, nxt_runtime_t *rt)
{
    return nxt_http_fields_hash(&nxt_h1p_fields_hash, rt->mem_pool,
                                nxt_h1p_fields, nxt_nitems(nxt_h1p_fields));
}


void
nxt_http_conn_init(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t               *c;
    nxt_socket_conf_t        *skcf;
    nxt_event_engine_t       *engine;
    nxt_socket_conf_joint_t  *joint;

    c = obj;
    joint = data;

    nxt_debug(task, "http conn init");

    c->joint = joint;
    joint->count++;

    skcf = joint->socket_conf;
    c->local = skcf->sockaddr;
    c->socket.data = NULL;

    engine = task->thread->engine;
    c->read_work_queue = &engine->fast_work_queue;
    c->write_work_queue = &engine->fast_work_queue;

    c->read_state = &nxt_h1p_idle_state;

    nxt_conn_wait(c);
}


static const nxt_conn_state_t  nxt_h1p_idle_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_h1p_read_header,
    .close_handler = nxt_h1p_conn_close,
    .error_handler = nxt_h1p_conn_error,

    .timer_handler = nxt_h1p_conn_timeout,
    .timer_value = nxt_h1p_timeout_value,
    .timer_data = offsetof(nxt_socket_conf_t, idle_timeout),
};


static void
nxt_h1p_read_header(nxt_task_t *task, void *obj, void *data)
{
    size_t                   size;
    nxt_conn_t               *c;
    nxt_socket_conf_joint_t  *joint;

    c = obj;

    nxt_debug(task, "h1p read header");

    if (c->read == NULL) {
        joint = c->joint;
        size = joint->socket_conf->header_buffer_size;

        c->read = nxt_buf_mem_alloc(c->mem_pool, size, 0);
        if (nxt_slow_path(c->read == NULL)) {
            nxt_h1p_conn_error(task, c, c->socket.data);
            return;
        }
    }

    c->read_state = &nxt_h1p_read_header_state;

    nxt_conn_read(task->thread->engine, c);
}


static const nxt_conn_state_t  nxt_h1p_read_header_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_h1p_header_parse,
    .close_handler = nxt_h1p_conn_close,
    .error_handler = nxt_h1p_conn_error,

    .timer_handler = nxt_h1p_conn_timeout,
    .timer_value = nxt_h1p_timeout_value,
    .timer_data = offsetof(nxt_socket_conf_t, header_read_timeout),
};


static void
nxt_h1p_header_parse(nxt_task_t *task, void *obj, void *data)
{
    size_t                   size;
    nxt_int_t                ret;
    nxt_buf_t                *in, *b;
    nxt_conn_t               *c;
    nxt_h1proto_t            *h1p;
    nxt_http_status_t        status;
    nxt_http_request_t       *r;
    nxt_socket_conf_joint_t  *joint;

    c = obj;
    h1p = data;

    nxt_debug(task, "h1p header parse");

    if (h1p == NULL) {
        h1p = nxt_mp_zget(c->mem_pool, sizeof(nxt_h1proto_t));
        if (nxt_slow_path(h1p == NULL)) {
            goto fail;
        }

        c->socket.data = h1p;
        h1p->conn = c;
    }

    r = h1p->request;

    if (r == NULL) {
        r = nxt_http_request_create(task);
        if (nxt_slow_path(r == NULL)) {
            goto fail;
        }

        h1p->request = r;
        r->proto.h1 = h1p;
        joint = c->joint;
        r->socket_conf = joint->socket_conf;

        r->remote = c->remote;

        ret = nxt_http_parse_request_init(&h1p->parser, r->mem_pool);
        if (nxt_slow_path(ret != NXT_OK)) {
            /*
             * The request is very uncomplete here,
             * so "internal server error" useless here.
             */
            nxt_mp_release(r->mem_pool);
            h1p->request = NULL;
            goto fail;
        }
    }

    ret = nxt_http_parse_request(&h1p->parser, &c->read->mem);

    if (nxt_fast_path(ret == NXT_DONE)) {
        r->target.start = h1p->parser.target_start;
        r->target.length = h1p->parser.target_end - h1p->parser.target_start;

        r->version.start = h1p->parser.version.str;
        r->version.length = sizeof(h1p->parser.version.str);

        r->method = &h1p->parser.method;
        r->path = &h1p->parser.path;
        r->args = &h1p->parser.args;

        /*
         * By default the keepalive mode is disabled in HTTP/1.0 and
         * enabled in HTTP/1.1.  The mode can be overridden later by
         * the "Connection" field processed in nxt_h1p_connection().
         */
        h1p->keepalive = (h1p->parser.version.s.minor != '0');

        r->fields = h1p->parser.fields;

        ret = nxt_http_fields_process(r->fields, &nxt_h1p_fields_hash, r);

        if (nxt_fast_path(ret == NXT_OK)) {
            r->state->ready_handler(task, r, NULL);
            return;
        }

        /* ret == NXT_ERROR */

        nxt_http_request_error(task, r, NXT_HTTP_BAD_REQUEST);
        return;
    }

    if (ret == NXT_AGAIN) {
        in = c->read;

        if (nxt_buf_mem_free_size(&in->mem) == 0) {
            size = r->socket_conf->large_header_buffer_size;

            if (size <= (size_t) nxt_buf_mem_used_size(&in->mem)
                || h1p->nbuffers >= r->socket_conf->large_header_buffers)
            {
                nxt_http_request_error(task, r,
                                      NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE);
                return;
            }

            b = nxt_buf_mem_alloc(c->mem_pool, size, 0);
            if (nxt_slow_path(b == NULL)) {
                nxt_http_request_error(task, r, NXT_HTTP_INTERNAL_SERVER_ERROR);
                return;
            }

            h1p->nbuffers++;

            size = nxt_buf_mem_used_size(&in->mem);
            b->mem.free = nxt_cpymem(b->mem.pos, in->mem.pos, size);

            in->next = h1p->buffers;
            h1p->buffers = in;
            c->read = b;
        }

        nxt_conn_read(task->thread->engine, c);
        return;
    }

    switch (ret) {

    case NXT_HTTP_PARSE_INVALID:
        status = NXT_HTTP_BAD_REQUEST;
        break;

    case NXT_HTTP_PARSE_UNSUPPORTED_VERSION:
        status = NXT_HTTP_VERSION_NOT_SUPPORTED;
        break;

    case NXT_HTTP_PARSE_TOO_LARGE_FIELD:
        status = NXT_HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE;
        break;

    default:
    case NXT_ERROR:
        status = NXT_HTTP_INTERNAL_SERVER_ERROR;
        break;
    }

    nxt_http_request_error(task, r, status);
    return;

fail:

    nxt_h1p_conn_close(task, c, h1p);
}


static nxt_int_t
nxt_h1p_connection(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    nxt_http_request_t  *r;

    r = ctx;

    if (field->value_length == 5 && nxt_memcmp(field->value, "close", 5) == 0) {
        r->proto.h1->keepalive = 0;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_h1p_transfer_encoding(void *ctx, nxt_http_field_t *field, uintptr_t data)
{
    nxt_http_te_t       te;
    nxt_http_request_t  *r;

    r = ctx;

    if (field->value_length == 7
        && nxt_memcmp(field->value, "chunked", 7) == 0)
    {
        te = NXT_HTTP_TE_CHUNKED;

    } else {
        te = NXT_HTTP_TE_UNSUPPORTED;
    }

    r->proto.h1->transfer_encoding = te;

    return NXT_OK;
}


static void
nxt_h1p_request_body_read(nxt_task_t *task, nxt_http_request_t *r)
{
    size_t             size, body_length;
    nxt_buf_t          *in, *b;
    nxt_conn_t         *c;
    nxt_h1proto_t      *h1p;
    nxt_http_status_t  status;

    h1p = r->proto.h1;

    nxt_debug(task, "h1p body read %O te:%d",
              r->content_length_n, h1p->transfer_encoding);

    switch (h1p->transfer_encoding) {

    case NXT_HTTP_TE_CHUNKED:
        status = NXT_HTTP_LENGTH_REQUIRED;
        goto error;

    case NXT_HTTP_TE_UNSUPPORTED:
        status = NXT_HTTP_NOT_IMPLEMENTED;
        goto error;

    default:
    case NXT_HTTP_TE_NONE:
        break;
    }

    if (r->content_length_n == -1 || r->content_length_n == 0) {
        goto ready;
    }

    if (r->content_length_n > (nxt_off_t) r->socket_conf->max_body_size) {
        status = NXT_HTTP_PAYLOAD_TOO_LARGE;
        goto error;
    }

    body_length = (size_t) r->content_length_n;

    b = r->body;

    if (b == NULL) {
        b = nxt_buf_mem_alloc(r->mem_pool, body_length, 0);
        if (nxt_slow_path(b == NULL)) {
            status = NXT_HTTP_INTERNAL_SERVER_ERROR;
            goto error;
        }

        r->body = b;
    }

    in = h1p->conn->read;

    size = nxt_buf_mem_used_size(&in->mem);

    if (size != 0) {
        if (size > body_length) {
            size = body_length;
        }

        b->mem.free = nxt_cpymem(b->mem.free, in->mem.pos, size);
        in->mem.pos += size;
    }

    size = nxt_buf_mem_free_size(&b->mem);

    nxt_debug(task, "h1p body rest: %uz", size);

    if (size != 0) {
        in->next = h1p->buffers;
        h1p->buffers = in;

        c = h1p->conn;
        c->read = b;
        c->read_state = &nxt_h1p_read_body_state;

        nxt_conn_read(task->thread->engine, c);
        return;
    }

ready:

    nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                       r->state->ready_handler, task, r, NULL);

    return;

error:

    h1p->keepalive = 0;

    nxt_http_request_error(task, r, status);
}


static const nxt_conn_state_t  nxt_h1p_read_body_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_h1p_body_read,
    .close_handler = nxt_h1p_conn_close,
    .error_handler = nxt_h1p_conn_error,

    .timer_handler = nxt_h1p_conn_timeout,
    .timer_value = nxt_h1p_timeout_value,
    .timer_data = offsetof(nxt_socket_conf_t, body_read_timeout),
    .timer_autoreset = 1,
};


static void
nxt_h1p_body_read(nxt_task_t *task, void *obj, void *data)
{
    size_t              size;
    nxt_conn_t          *c;
    nxt_h1proto_t       *h1p;
    nxt_http_request_t  *r;

    c = obj;
    h1p = data;

    nxt_debug(task, "h1p body read");

    size = nxt_buf_mem_free_size(&c->read->mem);

    nxt_debug(task, "h1p body rest: %uz", size);

    if (size != 0) {
        nxt_conn_read(task->thread->engine, c);

    } else {
        r = h1p->request;
        c->read = NULL;
        nxt_work_queue_add(&task->thread->engine->fast_work_queue,
                           r->state->ready_handler, task, r, NULL);
    }
}


static void
nxt_h1p_request_local_addr(nxt_task_t *task, nxt_http_request_t *r)
{
    r->local = nxt_conn_local_addr(task, r->proto.h1->conn);
}


#define NXT_HTTP_LAST_SUCCESS                                                 \
    (NXT_HTTP_OK + nxt_nitems(nxt_http_success) - 1)

static const nxt_str_t  nxt_http_success[] = {
    nxt_string("HTTP/1.1 200 OK\r\n"),
    nxt_string("HTTP/1.1 201 Created\r\n"),
    nxt_string("HTTP/1.1 202 Accepted\r\n"),
    nxt_string("HTTP/1.1 203 Non-Authoritative Information\r\n"),
    nxt_string("HTTP/1.1 204 No Content\r\n"),
    nxt_string("HTTP/1.1 205 Reset Content\r\n"),
    nxt_string("HTTP/1.1 206 Partial Content\r\n"),
};


#define NXT_HTTP_LAST_REDIRECTION                                             \
    (NXT_HTTP_MULTIPLE_CHOICES + nxt_nitems(nxt_http_redirection) - 1)

static const nxt_str_t  nxt_http_redirection[] = {
    nxt_string("HTTP/1.1 300 Multiple Choices\r\n"),
    nxt_string("HTTP/1.1 301 Moved Permanently\r\n"),
    nxt_string("HTTP/1.1 302 Found\r\n"),
    nxt_string("HTTP/1.1 303 See Other\r\n"),
    nxt_string("HTTP/1.1 304 Not Modified\r\n"),
};


#define NXT_HTTP_LAST_CLIENT_ERROR                                            \
    (NXT_HTTP_BAD_REQUEST + nxt_nitems(nxt_http_client_error) - 1)

static const nxt_str_t  nxt_http_client_error[] = {
    nxt_string("HTTP/1.1 400 Bad Request\r\n"),
    nxt_string("HTTP/1.1 401 Unauthorized\r\n"),
    nxt_string("HTTP/1.1 402 Payment Required\r\n"),
    nxt_string("HTTP/1.1 403 Forbidden\r\n"),
    nxt_string("HTTP/1.1 404 Not Found\r\n"),
    nxt_string("HTTP/1.1 405 Method Not Allowed\r\n"),
    nxt_string("HTTP/1.1 406 Not Acceptable\r\n"),
    nxt_string("HTTP/1.1 407 Proxy Authentication Required\r\n"),
    nxt_string("HTTP/1.1 408 Request Timeout\r\n"),
    nxt_string("HTTP/1.1 409 Conflict\r\n"),
    nxt_string("HTTP/1.1 410 Gone\r\n"),
    nxt_string("HTTP/1.1 411 Length Required\r\n"),
    nxt_string("HTTP/1.1 412 Precondition Failed\r\n"),
    nxt_string("HTTP/1.1 413 Payload Too Large\r\n"),
    nxt_string("HTTP/1.1 414 URI Too Long\r\n"),
    nxt_string("HTTP/1.1 415 Unsupported Media Type\r\n"),
    nxt_string("HTTP/1.1 416 Range Not Satisfiable\r\n"),
    nxt_string("HTTP/1.1 417 Expectation Failed\r\n"),
    nxt_string("HTTP/1.1 418\r\n"),
    nxt_string("HTTP/1.1 419\r\n"),
    nxt_string("HTTP/1.1 420\r\n"),
    nxt_string("HTTP/1.1 421\r\n"),
    nxt_string("HTTP/1.1 422\r\n"),
    nxt_string("HTTP/1.1 423\r\n"),
    nxt_string("HTTP/1.1 424\r\n"),
    nxt_string("HTTP/1.1 425\r\n"),
    nxt_string("HTTP/1.1 426\r\n"),
    nxt_string("HTTP/1.1 427\r\n"),
    nxt_string("HTTP/1.1 428\r\n"),
    nxt_string("HTTP/1.1 429\r\n"),
    nxt_string("HTTP/1.1 430\r\n"),
    nxt_string("HTTP/1.1 431 Request Header Fields Too Large\r\n"),
};


#define NXT_HTTP_LAST_SERVER_ERROR                                            \
    (NXT_HTTP_INTERNAL_SERVER_ERROR + nxt_nitems(nxt_http_server_error) - 1)

static const nxt_str_t  nxt_http_server_error[] = {
    nxt_string("HTTP/1.1 500 Internal Server Error\r\n"),
    nxt_string("HTTP/1.1 501 Not Implemented\r\n"),
    nxt_string("HTTP/1.1 502 Bad Gateway\r\n"),
    nxt_string("HTTP/1.1 503 Service Unavailable\r\n"),
    nxt_string("HTTP/1.1 504 Gateway Timeout\r\n"),
    nxt_string("HTTP/1.1 505 HTTP Version Not Supported\r\n"),
};


#define UNKNOWN_STATUS_LENGTH  (sizeof("HTTP/1.1 65536\r\n") - 1)

static void
nxt_h1p_request_header_send(nxt_task_t *task, nxt_http_request_t *r)
{
    u_char              *p;
    size_t              size;
    nxt_buf_t           *header;
    nxt_str_t           unknown_status;
    nxt_int_t           conn;
    nxt_uint_t          n;
    nxt_bool_t          http11;
    nxt_conn_t          *c;
    nxt_h1proto_t       *h1p;
    const nxt_str_t     *status;
    nxt_http_field_t    *field;
    nxt_event_engine_t  *engine;
    u_char              buf[UNKNOWN_STATUS_LENGTH];

    static const char   chunked[] = "Transfer-Encoding: chunked\r\n";

    static const nxt_str_t  connection[2] = {
        nxt_string("Connection: close\r\n"),
        nxt_string("Connection: keep-alive\r\n"),
    };

    nxt_debug(task, "h1p request header send");

    r->header_sent = 1;
    h1p = r->proto.h1;
    n = r->status;

    if (n >= NXT_HTTP_OK && n <= NXT_HTTP_LAST_SUCCESS) {
        status = &nxt_http_success[n - NXT_HTTP_OK];

    } else if (n >= NXT_HTTP_MULTIPLE_CHOICES
               && n <= NXT_HTTP_LAST_REDIRECTION)
    {
        status = &nxt_http_redirection[n - NXT_HTTP_MULTIPLE_CHOICES];

    } else if (n >= NXT_HTTP_BAD_REQUEST && n <= NXT_HTTP_LAST_CLIENT_ERROR) {
        status = &nxt_http_client_error[n - NXT_HTTP_BAD_REQUEST];

    } else if (n >= NXT_HTTP_INTERNAL_SERVER_ERROR
               && n <= NXT_HTTP_LAST_SERVER_ERROR)
    {
        status = &nxt_http_server_error[n - NXT_HTTP_INTERNAL_SERVER_ERROR];

    } else {
        p = nxt_sprintf(buf, buf + UNKNOWN_STATUS_LENGTH,
                        "HTTP/1.1 %03d\r\n", n);

        unknown_status.length = p - buf;
        unknown_status.start = buf;
        status = &unknown_status;
    }

    size = status->length;
    /* Trailing CRLF at the end of header. */
    size += sizeof("\r\n") - 1;

    http11 = (h1p->parser.version.s.minor != '0');

    if (r->resp.content_length == NULL || r->resp.content_length->skip) {
        if (http11) {
            h1p->chunked = 1;
            size += sizeof(chunked) - 1;
            /* Trailing CRLF will be added by the first chunk header. */
            size -= sizeof("\r\n") - 1;

        } else {
            h1p->keepalive = 0;
        }
    }

    conn = -1;

    if (http11 ^ h1p->keepalive) {
        conn = h1p->keepalive;
        size += connection[conn].length;
    }

    nxt_list_each(field, r->resp.fields) {

        if (!field->skip) {
            size += field->name_length + field->value_length;
            size += sizeof(": \r\n") - 1;
        }

    } nxt_list_loop;

    header = nxt_buf_mem_alloc(r->mem_pool, size, 0);
    if (nxt_slow_path(header == NULL)) {
        /* The internal server error is set just for logging. */
        r->status = NXT_HTTP_INTERNAL_SERVER_ERROR;
        nxt_h1p_conn_close(task, h1p->conn, h1p);
        return;
    }

    p = header->mem.free;

    p = nxt_cpymem(p, status->start, status->length);

    nxt_list_each(field, r->resp.fields) {

        if (!field->skip) {
            p = nxt_cpymem(p, field->name, field->name_length);
            *p++ = ':'; *p++ = ' ';
            p = nxt_cpymem(p, field->value, field->value_length);
            *p++ = '\r'; *p++ = '\n';
        }

    } nxt_list_loop;

    if (conn >= 0) {
        p = nxt_cpymem(p, connection[conn].start, connection[conn].length);
    }

    if (h1p->chunked) {
        p = nxt_cpymem(p, chunked, sizeof(chunked) - 1);
        /* Trailing CRLF will be added by the first chunk header. */

    } else {
        *p++ = '\r'; *p++ = '\n';
    }

    header->mem.free = p;

    c = h1p->conn;

    c->write = header;
    c->write_state = &nxt_h1p_send_state;

    engine = task->thread->engine;

    nxt_work_queue_add(&engine->fast_work_queue, r->state->ready_handler,
                       task, r, NULL);

    nxt_conn_write(engine, c);
}


static const nxt_conn_state_t  nxt_h1p_send_state
    nxt_aligned(64) =
{
    .ready_handler = nxt_h1p_sent,
    .close_handler = nxt_h1p_conn_close,
    .error_handler = nxt_h1p_conn_error,

    .timer_handler = nxt_h1p_conn_timeout,
    .timer_value = nxt_h1p_timeout_value,
    .timer_data = offsetof(nxt_socket_conf_t, send_timeout),
    .timer_autoreset = 1,
};


static void
nxt_h1p_request_send(nxt_task_t *task, nxt_http_request_t *r, nxt_buf_t *out)
{
    nxt_conn_t  *c;

    nxt_debug(task, "h1p request send");

    c = r->proto.h1->conn;

    if (r->proto.h1->chunked) {
        out = nxt_h1p_chunk_create(task, r, out);
        if (nxt_slow_path(out == NULL)) {
            nxt_h1p_conn_error(task, c, c->socket.data);
            return;
        }
    }

    if (c->write == NULL) {
        c->write = out;
        c->write_state = &nxt_h1p_send_state;

        nxt_conn_write(task->thread->engine, c);

    } else {
        nxt_buf_chain_add(&c->write, out);
    }
}


static nxt_buf_t *
nxt_h1p_chunk_create(nxt_task_t *task, nxt_http_request_t *r, nxt_buf_t *out)
{
    nxt_off_t          size;
    nxt_buf_t          *b, **prev, *header, *tail;

    const size_t       chunk_size = 2 * (sizeof("\r\n") - 1) + NXT_OFF_T_HEXLEN;
    static const char  tail_chunk[] = "\r\n0\r\n\r\n";

    size = 0;
    prev = &out;

    for (b = out; b != NULL; b = b->next) {

        if (nxt_buf_is_last(b)) {
            tail = nxt_buf_mem_alloc(r->mem_pool, chunk_size, 0);
            if (nxt_slow_path(tail == NULL)) {
                return NULL;
            }

            *prev = tail;
            tail->next = b;
            /*
             * The tail_chunk size with trailing zero is 8 bytes, so
             * memcpy may be inlined with just single 8 byte move operation.
             */
            nxt_memcpy(tail->mem.free, tail_chunk, sizeof(tail_chunk));
            tail->mem.free += sizeof(tail_chunk) - 1;

            break;
        }

        size += nxt_buf_used_size(b);
        prev = &b->next;
    }

    if (size == 0) {
        return out;
    }

    header = nxt_buf_mem_alloc(r->mem_pool, chunk_size, 0);
    if (nxt_slow_path(header == NULL)) {
        return NULL;
    }

    header->next = out;
    header->mem.free = nxt_sprintf(header->mem.free, header->mem.end,
                                   "\r\n%xO\r\n", size);
    return header;
}


static void
nxt_h1p_sent(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t          *c;
    nxt_event_engine_t  *engine;

    c = obj;

    nxt_debug(task, "h1p sent");

    engine = task->thread->engine;

    c->write = nxt_sendbuf_completion0(task, &engine->fast_work_queue,
                                       c->write);
    if (c->write != NULL) {
        nxt_conn_write(engine, c);
    }
}


static void
nxt_h1p_request_close(nxt_task_t *task, nxt_http_proto_t proto)
{
    nxt_conn_t     *c;
    nxt_h1proto_t  *h1p;

    nxt_debug(task, "h1p request close");

    h1p = proto.h1;
    h1p->request = NULL;

    c = h1p->conn;

    if (h1p->keepalive) {
        nxt_h1p_keepalive(task, h1p, c);

    } else {
        nxt_h1p_close(task, c);
    }
}


static void
nxt_h1p_keepalive(nxt_task_t *task, nxt_h1proto_t *h1p, nxt_conn_t *c)
{
    size_t     size;
    nxt_buf_t  *in, *b, *next;

    nxt_debug(task, "h1p keepalive");

    if (!c->tcp_nodelay) {
        nxt_conn_tcp_nodelay_on(task, c);
    }

    b = h1p->buffers;

    nxt_memzero(h1p, offsetof(nxt_h1proto_t, conn));

    in = c->read;

    if (in == NULL) {
        /* A request with large body. */
        in = b;
        c->read = in;

        b = in->next;
        in->next = NULL;
    }

    while (b != NULL) {
        next = b->next;
        nxt_mp_free(c->mem_pool, b);
        b = next;
    }

    size = nxt_buf_mem_used_size(&in->mem);

    if (size == 0) {
        in->mem.pos = in->mem.start;
        in->mem.free = in->mem.start;

        if (c->socket.read_ready) {
            c->read_state = &nxt_h1p_read_header_state;
            nxt_conn_read(task->thread->engine, c);

        } else {
            c->read_state = &nxt_h1p_idle_state;
            nxt_conn_wait(c);
        }

    } else {
        nxt_debug(task, "h1p pipelining");

        nxt_memmove(in->mem.start, in->mem.pos, size);

        in->mem.pos = in->mem.start;
        in->mem.free = in->mem.start + size;

        nxt_h1p_header_parse(task, c, c->socket.data);
    }
}


static void
nxt_h1p_close(nxt_task_t *task, nxt_conn_t *c)
{
    nxt_debug(task, "h1p close");

    c->socket.data = NULL;

    if (c->socket.fd != -1) {
        c->write_state = &nxt_router_conn_close_state;

        nxt_conn_close(task->thread->engine, c);
    }
}


static void
nxt_h1p_conn_close(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t          *c;
    nxt_h1proto_t       *h1p;
    nxt_http_request_t  *r;

    c = obj;
    h1p = data;

    nxt_debug(task, "h1p conn close");

    if (h1p != NULL) {
        r = h1p->request;

        if (r != NULL) {
            r->state->error_handler(task, r, r->proto.h1);
            return;
        }
    }

    nxt_h1p_close(task, c);
}


static void
nxt_h1p_conn_error(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t     *c;
    nxt_h1proto_t  *h1p;

    c = obj;
    h1p = data;

    nxt_debug(task, "h1p conn error");

    nxt_h1p_conn_close(task, c, h1p);
}


static void
nxt_h1p_conn_timeout(nxt_task_t *task, void *obj, void *data)
{
    nxt_conn_t   *c;
    nxt_timer_t  *timer;

    timer = obj;

    nxt_debug(task, "h1p conn timeout");

    c = nxt_read_timer_conn(timer);

    nxt_h1p_conn_close(task, c, c->socket.data);
}


static nxt_msec_t
nxt_h1p_timeout_value(nxt_conn_t *c, uintptr_t data)
{
    nxt_socket_conf_joint_t  *joint;

    joint = c->joint;

    return nxt_value_at(nxt_msec_t, joint->socket_conf, data);
}
