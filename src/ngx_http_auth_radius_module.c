#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>
#include <ctype.h>
#include "radius_client.h"

typedef struct ngx_http_auth_radius_ctx_t {
    u_char                   digest[32];
    uint8_t                  attempts;
    radius_req_queue_node_t *rqn;
    uint8_t                  done:1;
    uint8_t                  accepted:1;
    uint8_t                  timedout:1;
} ngx_http_auth_radius_ctx_t;

typedef struct {
    ngx_array_t             *servers;
    ngx_str_t                realm;
    ngx_msec_t               timeout;
    ngx_uint_t               attempts;
    radius_str_t             secret;
} ngx_http_auth_radius_main_conf_t;

static void *
ngx_http_auth_radius_create_main_conf(ngx_conf_t *cf);

static void *
ngx_http_auth_radius_create_loc_conf(ngx_conf_t *cf);

static char *
ngx_http_auth_radius_merge_loc_conf(ngx_conf_t *cf,
                                    void *parent,
                                    void *child);

static char *
ngx_http_auth_radius_set_auth_radius(ngx_conf_t *cf,
                                     ngx_command_t *cmd,
                                     void *conf);

static char *
ngx_http_auth_radius_set_radius_server(ngx_conf_t *cf,
                                       ngx_command_t *cmd,
                                       void *conf);
static char*
ngx_http_auth_radius_set_radius_timeout(ngx_conf_t *cf,
                                        ngx_command_t *cmd,
                                        void *conf);

static char*
ngx_http_auth_radius_set_radius_attempts(ngx_conf_t *cf,
                                         ngx_command_t *cmd,
                                         void *conf);

static ngx_int_t
ngx_http_auth_radius_init(ngx_conf_t *cf);

static ngx_int_t
ngx_http_auth_radius_init_servers(ngx_cycle_t *cycle);

static void
ngx_http_auth_radius_destroy_servers(ngx_cycle_t *cycle);

static ngx_int_t
ngx_http_auth_radius_send_radius_request(ngx_http_request_t *r,
                                         radius_req_queue_node_t *prev_req);

static ngx_int_t
ngx_http_auth_radius_set_realm(ngx_http_request_t *r,
                               const ngx_str_t *realm);

static ngx_command_t ngx_http_auth_radius_commands[] = {

    { ngx_string("radius_server"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_CONF_TAKE23,
      ngx_http_auth_radius_set_radius_server,
      0,
      0,
      NULL },

    { ngx_string("radius_timeout"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_CONF_TAKE1,
      ngx_http_auth_radius_set_radius_timeout,
      0,
      0,
      NULL },

    { ngx_string("radius_attempts"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_CONF_TAKE1,
      ngx_http_auth_radius_set_radius_attempts,
      0,
      0,
      NULL },

    { ngx_string("auth_radius"),
      NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
      ngx_http_auth_radius_set_auth_radius,
      0,
      0,
      NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_http_auth_radius_module_ctx = {
    NULL,                                               /* preconfiguration */
    ngx_http_auth_radius_init,                          /* postconfiguration */
    ngx_http_auth_radius_create_main_conf,              /* create main configuration */
    NULL,                                               /* init main configuration */
    NULL,                                               /* create server configuration */
    NULL,                                               /* merge server configuration */
    ngx_http_auth_radius_create_loc_conf,               /* create location configuration */
    ngx_http_auth_radius_merge_loc_conf,                /* merge location configuration */
};

ngx_module_t ngx_http_auth_radius_module = {
    NGX_MODULE_V1,
    &ngx_http_auth_radius_module_ctx,       /* module context */
    ngx_http_auth_radius_commands,          /* module directives */
    NGX_HTTP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    ngx_http_auth_radius_init_servers,      /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    ngx_http_auth_radius_destroy_servers,   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};

#define RADIUS_STR_FROM_NGX_STR_INITIALIZER(ns) .len = ns.len, .s = ns.data

static void
radius_logger(void *log, const char *fmt)
{
    ngx_uint_t level = 0;
    ngx_err_t err = 0;
    ngx_log_error_core(level, log, err, fmt, NULL);
}

static void
radius_read_handler(ngx_event_t *rev)
{
    ngx_connection_t *c = rev->data;
    ngx_log_t *log = rev->log;
    radius_req_queue_node_t *rqn = c->data;
    ngx_http_request_t *r = rqn->data;

    ngx_http_auth_radius_ctx_t *ctx;
    ctx = ngx_http_get_module_ctx(r, ngx_http_auth_radius_module);
    if (ctx == NULL || ctx->rqn != rqn) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "radius_read_handler: GONE 1");
        return;
    }

    if (rev->timedout) {
        rev->timedout = 0;
        ctx->attempts--;
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "http_req_read_handler: timeout 0x%xd, attempt: %d", r, ctx->attempts);

        if (!ctx->attempts) {
            ctx->done = 1;
            ctx->accepted = 0;
            ctx->timedout = 1;
            ngx_post_event(r->connection->write, &ngx_posted_events);
            return;
        }

        // Re-send RADIUS Auth event
        ngx_http_auth_radius_send_radius_request(r, rqn);
        return;
    }

    if (rev->timer_set) {
        rev->timer_set = 0;
        ngx_del_timer(rev);
    }

    radius_server_t *rs = get_server_by_req(rqn);
    radius_req_queue_node_t *rqn2 = radius_recv_request(rs);
    if (rqn2 == NULL) {
        // not found TODO
        ngx_log_error(NGX_LOG_ERR, log, 0, "radius_read_handler: request not found");
        return;
    }

    if (rqn != rqn2) {
        // not found TODO
        ngx_log_error(NGX_LOG_ERR, log, 0, "radius_read_handler: request not found 2");
        return;
    }

    ngx_log_error(NGX_LOG_ERR, log, 0, "radius_read_handler: rs: %d, 0x%xl, 0x%xl, id: %d, acc: %d", rs->id, r, rqn, rqn->ident, rqn->accepted);

    ctx->done = 1;
    ctx->accepted = rqn->accepted;

    // Post RADIUS Auth done event
    ngx_post_event(r->connection->write, &ngx_posted_events);

    release_req_queue_node(rqn);
}

static ngx_int_t
ngx_http_auth_radius_send_radius_request(ngx_http_request_t *r,
                                         radius_req_queue_node_t *prev_req)
{
    ngx_log_t *log = r->connection->log;

    ngx_log_error(NGX_LOG_ERR, log, 0, "ngx_http_auth_send_radius_request 0x%xl", r);

    ngx_http_auth_radius_main_conf_t *mcf;
    mcf = ngx_http_get_module_main_conf(r, ngx_http_auth_radius_module);

    ngx_http_auth_radius_ctx_t *ctx;
    ctx = ngx_http_get_module_ctx(r, ngx_http_auth_radius_module);

    if (ctx == NULL) {
        // TODO: contex must be set
        // LOG
        return NGX_ERROR;
    }

    radius_str_t user = {
        RADIUS_STR_FROM_NGX_STR_INITIALIZER(r->headers_in.user)
    };
    radius_str_t passwd = {
        RADIUS_STR_FROM_NGX_STR_INITIALIZER(r->headers_in.passwd)
    };

    // Send RADIUS Auth request
    radius_req_queue_node_t *rqn;
    rqn = radius_send_request(mcf->servers,
                              prev_req,
                              &user, &passwd,
                              log);
    if (rqn == NULL) {
        // TODO: log
        return NGX_ERROR;
    }

    // DKL: what's going on here
    radius_server_t *rs;
    rs = get_server_by_req(rqn);
    ngx_log_error(NGX_LOG_ERR, log, 0,
                  "ngx_http_auth_send_radius_request rs: %d, "
                  "assign 0x%xl to 0x%xl, id: %d",
                  rs->id, r, rqn, rqn->ident);

    ngx_connection_t *c = rs->data;
    if (c == NULL) {
        // Get connection around socket
        c = ngx_get_connection(rs->sockfd, log);
        if (c == NULL) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "ngx_http_auth_send_radius_request: ngx_get_connection");
            return NGX_ERROR;
        }

        if (ngx_nonblocking(rs->sockfd) == -1) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "ngx_http_auth_send_radius_request: ngx_nonblocking");

            ngx_free_connection(c);
            return NGX_ERROR;
        }

        // DKL: can rs be changed?
        rs->data = c;

        // Subscribe to read data event
        if (ngx_add_event(c->read, NGX_READ_EVENT, NGX_LEVEL_EVENT) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, log, 0,
                          "ngx_http_auth_send_radius_request: ngx_add_event");
            return NGX_ERROR;
        }

        c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);
    }

    c->data = rqn;
    ctx->rqn = rqn;
    rqn->data = r;

    ngx_event_t *rev = c->read;
    rev->handler = radius_read_handler;
    rev->log = log; // NB: changing to mcf->log
    rs->log = log;  // segfaults the worker

    // Subscribe to read timeout event
    ngx_add_timer(rev, mcf->timeout);

    return NGX_AGAIN;
}

static ngx_int_t
ngx_http_auth_radius_set_realm(ngx_http_request_t *r,
                               const ngx_str_t *realm)
{
    r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers);
    if (r->headers_out.www_authenticate == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->headers_out.www_authenticate->hash = 1;
    r->headers_out.www_authenticate->key.len = sizeof("WWW-Authenticate") - 1;
    r->headers_out.www_authenticate->key.data = (u_char *) "WWW-Authenticate";
    r->headers_out.www_authenticate->value = *realm;

    return NGX_HTTP_UNAUTHORIZED;
}

static ngx_int_t
ngx_http_auth_radius_handler(ngx_http_request_t *r)
{
    ngx_log_t *log = r->connection->log;

    ngx_http_auth_radius_main_conf_t *mcf;
    mcf = ngx_http_get_module_main_conf(r, ngx_http_auth_radius_module);

    ngx_http_auth_radius_main_conf_t *lcf;
    lcf = ngx_http_get_module_loc_conf(r, ngx_http_auth_radius_module);

    if (lcf->realm.data == NULL || lcf->realm.len == 0) {
        // No RADIUS realm defined
        return NGX_DECLINED;
    }

    ngx_http_auth_radius_ctx_t *ctx;
    ctx = ngx_http_get_module_ctx(r, ngx_http_auth_radius_module);

    if (ctx == NULL) {
        // No RADIUS Auth request sent yet

        // Parse credentials
        ngx_int_t rc = ngx_http_auth_basic_user(r);

        if (rc == NGX_ERROR)
            return NGX_HTTP_INTERNAL_SERVER_ERROR;

        if (rc == NGX_DECLINED) {
            return ngx_http_auth_radius_set_realm(r, &mcf->realm);
        }

        // TODO: move to _send_radius_request?

        ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
        if (ctx == NULL) {
            // TODO log
            return NGX_ERROR;
        }

        ctx->attempts = mcf->attempts;
        ctx->done = 0;
        ctx->accepted = 0;
        ctx->timedout = 0;
        ngx_http_set_ctx(r, ctx, ngx_http_auth_radius_module);

        return ngx_http_auth_radius_send_radius_request(r, NULL);
    }

    if (!ctx->done) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "Not done r: 0x%xl", r);
        return NGX_AGAIN;
    }

    if (!ctx->accepted) {
        if (ctx->timedout) {
            ngx_log_error(NGX_LOG_ERR, log, 0, "Timedout r: 0x%xl", r);
            return NGX_HTTP_SERVICE_UNAVAILABLE;
        } else {
            ngx_log_error(NGX_LOG_ERR, log, 0, "Rejected r: 0x%xl", r);
            return ngx_http_auth_radius_set_realm(r, &mcf->realm);
        }
    }

    ngx_log_error(NGX_LOG_ERR, log, 0, "Accepted r: 0x%xl", r);
    return NGX_OK;
}

static ngx_int_t
ngx_http_auth_radius_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt       *h;
    ngx_http_core_main_conf_t *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_auth_radius_handler;

    return NGX_OK;
}

static ngx_int_t
ngx_http_auth_radius_init_servers(ngx_cycle_t *cycle)
{
    ngx_http_auth_radius_main_conf_t *mcf;
    mcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_auth_radius_module);

    if (mcf == NULL) {
        return NGX_ERROR;
    }

    if (radius_init_servers(mcf->servers) == -1) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void
ngx_http_auth_radius_destroy_servers(ngx_cycle_t *cycle)
{
    ngx_http_auth_radius_main_conf_t *mcf;
    mcf = ngx_http_cycle_get_module_main_conf(cycle, ngx_http_auth_radius_module);

    if (mcf == NULL) {
        return;
    }

    radius_destroy_servers(mcf->servers);
}

static char*
ngx_http_auth_radius_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_auth_radius_main_conf_t *prev = parent;
    ngx_http_auth_radius_main_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->realm, prev->realm, "");

    ngx_conf_merge_msec_value(conf->timeout, prev->timeout, 60000);
    ngx_conf_merge_uint_value(conf->attempts, prev->attempts, 3);

    return NGX_CONF_OK;
}

static void *
ngx_http_auth_radius_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_auth_radius_main_conf_t *mcf;
    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_radius_main_conf_t));
    if (mcf == NULL) {
        return NGX_CONF_ERROR;
    }

    mcf->servers = NULL;
    mcf->realm.data = NULL;
    mcf->attempts = NGX_CONF_UNSET;
    mcf->timeout = NGX_CONF_UNSET_MSEC;

    return mcf;
}

static void *
ngx_http_auth_radius_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_auth_radius_main_conf_t *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_radius_main_conf_t));
    if (mcf == NULL) {
        return NGX_CONF_ERROR;
    }

    mcf->servers = ngx_array_create(cf->pool, 5, sizeof(radius_server_t));
    if (mcf->servers == NULL) {
        return NGX_CONF_ERROR;
    }

    mcf->realm.data = NULL;
    mcf->attempts = NGX_CONF_UNSET;
    mcf->timeout = NGX_CONF_UNSET_MSEC;

    return mcf;
}

static char *
ngx_http_auth_radius_set_radius_server(ngx_conf_t *cf,
                                       ngx_command_t *cmd,
                                       void *conf)
{
    ngx_str_t *value = cf->args->elts;

    if (cf->args->nelts != 3 && cf->args->nelts != 4) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid \"%V\" config",
                           &value[0]);
        return NGX_CONF_ERROR;
    }

    ngx_http_auth_radius_main_conf_t *mcf =
        ngx_http_conf_get_module_main_conf(cf, ngx_http_auth_radius_module);

    ngx_url_t u;
    ngx_memzero(&u, sizeof(ngx_url_t));
    u.url = value[1];
    u.uri_part = 1;
    u.default_port = RADIUS_DEFAULT_PORT;
    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid \"%V\" \"url\" value: \"%V\"",
                               &value[0], &value[1]);
        }
        return NGX_CONF_ERROR;
    }

    radius_str_t secret;
    secret.s = value[2].data;
    secret.len = value[2].len;

    mcf->secret.s = secret.s;
    mcf->secret.len = secret.len;

    radius_str_t nas_id;
    nas_id.s = NULL;
    nas_id.len = 0;

    if (cf->args->nelts == 4) {
        nas_id.s = value[3].data;
        nas_id.len = value[3].len;
    }

    radius_server_t *rs = ngx_array_push(mcf->servers);
    if (rs == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" no enough memory",
                           &value[0]);
        return NGX_CONF_ERROR;
    }

    int rs_id = mcf->servers->nelts;
    radius_add_server(rs, rs_id,
                      u.addrs[0].sockaddr, u.addrs[0].socklen,
                      &secret, &nas_id);
    rs->logger = radius_logger;

    return NGX_CONF_OK;
}

static char*
ngx_http_auth_radius_set_radius_timeout(ngx_conf_t *cf,
                                        ngx_command_t *cmd,
                                        void *conf)
{
    ngx_str_t* value = cf->args->elts;

    ngx_http_auth_radius_main_conf_t* mcf =
        ngx_http_conf_get_module_main_conf(cf, ngx_http_auth_radius_module);

    ngx_int_t timeout = ngx_parse_time(&value[1], 0);
    if (timeout == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid \"radius_timeout\" value: \"%V\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }
    mcf->timeout = timeout;

    return NGX_CONF_OK;
}

static char*
ngx_http_auth_radius_set_radius_attempts(ngx_conf_t *cf,
                                         ngx_command_t *cmd,
                                         void *conf)
{
    ngx_str_t* value = cf->args->elts;

    ngx_http_auth_radius_main_conf_t* mcf =
        ngx_http_conf_get_module_main_conf(cf, ngx_http_auth_radius_module);

    ngx_int_t attempts = ngx_atoi(value[1].data, value[1].len);
    if (attempts == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid \"radius_attempts\" value: \"%V\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }
    mcf->attempts = attempts;

    return NGX_CONF_OK;
}

static char *
ngx_http_auth_radius_set_auth_radius(ngx_conf_t *cf,
                                     ngx_command_t *cmd,
                                     void *conf)
{
    ngx_str_t *value = cf->args->elts;

    if (ngx_strncmp(value[1].data, "off", 3) == 0) {
        return NGX_CONF_OK;
    }

    ngx_http_auth_radius_main_conf_t *mcf =
        ngx_http_conf_get_module_loc_conf(cf, ngx_http_auth_radius_module);

    mcf->realm = value[1];

    return NGX_CONF_OK;
}
