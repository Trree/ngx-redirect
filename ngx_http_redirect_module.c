#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct
{
    ngx_http_status_t           status;
    ngx_str_t					redirectServer;
} ngx_http_redirect_ctx_t;

typedef struct
{
    ngx_http_upstream_conf_t upstream;
    ngx_str_t addr;
} ngx_http_redirect_conf_t;


static char *
ngx_http_redirect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_redirect_handler(ngx_http_request_t *r);
static void* ngx_http_redirect_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_redirect_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

static ngx_int_t
redirect_upstream_process_header(ngx_http_request_t *r);
static ngx_int_t
redirect_process_status_line(ngx_http_request_t *r);


static ngx_str_t  ngx_http_proxy_hide_headers[] =
{
    ngx_string("Date"),
    ngx_string("Server"),
    ngx_string("X-Pad"),
    ngx_string("X-Accel-Expires"),
    ngx_string("X-Accel-Redirect"),
    ngx_string("X-Accel-Limit-Rate"),
    ngx_string("X-Accel-Buffering"),
    ngx_string("X-Accel-Charset"),
    ngx_null_string
};


static ngx_command_t  ngx_http_redirect_commands[] =
{

    {
        ngx_string("redirect"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
        ngx_http_redirect,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_redirect_conf_t, addr),
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t  ngx_http_redirect_module_ctx =
{
    NULL,                              /* preconfiguration */
    NULL,                        	   /* postconfiguration */

    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */

    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */

    ngx_http_redirect_create_loc_conf,   /* create location configuration */
    ngx_http_redirect_merge_loc_conf     /* merge location configuration */
};

ngx_module_t  ngx_http_redirect_module =
{
    NGX_MODULE_V1,
    &ngx_http_redirect_module_ctx,           /* module context */
    ngx_http_redirect_commands,              /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void* ngx_http_redirect_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_redirect_conf_t  *mycf;

    mycf = (ngx_http_redirect_conf_t  *)ngx_pcalloc(cf->pool, sizeof(ngx_http_redirect_conf_t));
    if (mycf == NULL)
    {
        return NULL;
    }

    //���¼򵥵�Ӳ����ngx_http_upstream_conf_t�ṹ�еĸ���Ա������
    //��ʱʱ�䶼��Ϊ1���ӡ���Ҳ��http�������ģ���Ĭ��ֵ
    mycf->upstream.connect_timeout = 60000;
    mycf->upstream.send_timeout = 60000;
    mycf->upstream.read_timeout = 60000;
    mycf->upstream.store_access = 0600;
    //ʵ����buffering�Ѿ������˽��Թ̶���С���ڴ���Ϊ��������ת�����ε�
    //��Ӧ���壬���̶��������Ĵ�С����buffer_size�����bufferingΪ1
    //�ͻ�ʹ�ø�����ڴ滺���������������ε���Ӧ���������ʹ��bufs.num��
    //��������ÿ����������СΪbufs.size�����⻹��ʹ����ʱ�ļ�����ʱ�ļ���
    //��󳤶�Ϊmax_temp_file_size
    mycf->upstream.buffering = 0;
    mycf->upstream.bufs.num = 8;
    mycf->upstream.bufs.size = ngx_pagesize;
    mycf->upstream.buffer_size = ngx_pagesize;
    mycf->upstream.busy_buffers_size = 2 * ngx_pagesize;
    mycf->upstream.temp_file_write_size = 2 * ngx_pagesize;
    mycf->upstream.max_temp_file_size = 1024 * 1024 * 1024;

    //upstreamģ��Ҫ��hide_headers��Ա����Ҫ��ʼ����upstream�ڽ���
    //�����η��������صİ�ͷʱ�������
    //ngx_http_upstream_process_headers��������hide_headers��Ա��
    //��Ӧת�������ε�һЩhttpͷ�����أ������ｫ����Ϊ
    //NGX_CONF_UNSET_PTR ����Ϊ����merge�ϲ����������ʹ��
    //upstreamģ���ṩ��ngx_http_upstream_hide_headers_hash
    //������ʼ��hide_headers ��Ա
    mycf->upstream.hide_headers = NGX_CONF_UNSET_PTR;
    mycf->upstream.pass_headers = NGX_CONF_UNSET_PTR;
    
    //mycf->addr = {0, NULL};
    return mycf;
}


static char *ngx_http_redirect_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_redirect_conf_t *prev = (ngx_http_redirect_conf_t *)parent;
    ngx_http_redirect_conf_t *conf = (ngx_http_redirect_conf_t *)child;

    ngx_hash_init_t             hash;
    hash.max_size = 100;
    hash.bucket_size = 1024;
    hash.name = "proxy_headers_hash";
    if (ngx_http_upstream_hide_headers_hash(cf, &conf->upstream,
                                            &prev->upstream, ngx_http_proxy_hide_headers, &hash)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
    ngx_conf_merge_str_value(conf->addr, conf->addr, ""); 

    return NGX_CONF_OK;
}


static ngx_int_t
redirect_upstream_create_request(ngx_http_request_t *r)
{
    static ngx_str_t redirectQueryLine = ngx_string("GET / HTTP/1.1\r\nHost: www.sina.com.cn\r\nConnection: close\r\n\r\n");
    //ngx_int_t queryLineLen = redirectQueryLine.len ;
    //�������ڴ���������ڴ棬��������ô���������������ѵ�����£�������
    //��������������ʱ��������Ҫepoll��ε���send���Ͳ�����ɣ�
    //��ʱ���뱣֤����ڴ治�ᱻ�ͷţ��������ʱ������ڴ�ᱻ�Զ��ͷţ�
    //�����ڴ�й©�Ŀ���
    ngx_buf_t* b = ngx_create_temp_buf(r->pool, redirectQueryLine.len);
    if (b == NULL)
        return NGX_ERROR;

    b->last = ngx_copy(b->last, redirectQueryLine.data, redirectQueryLine.len);
    //*b->last++ = CR; *b->last++ = LF;
    //lastҪָ�������ĩβ
    //b->last = b->pos + queryLineLen;

    //�����൱��snprintf��ֻ����֧��4.4���еı�4-7�г�������ת����ʽ
    //ngx_snprintf(b->pos, queryLineLen , (char*)redirectQueryLine.data, &r->args);
    // r->upstream->request_bufs��һ��ngx_chain_t�ṹ����������Ҫ
    //���͸����η�����������

    ngx_chain_t                  *cl;
    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = b;
    r->upstream->request_bufs = cl;
    cl->next = NULL;

    b->flush = 1;
    //r->upstream->request_sent = 0;
    //r->upstream->header_sent = 0;
    // header_hash������Ϊ0
    r->header_hash = 1;
    return NGX_OK;
}

static ngx_int_t
redirect_process_status_line(ngx_http_request_t *r)
{
    size_t                 len;
    ngx_int_t              rc;
    ngx_http_upstream_t   *u;

    //�������вŻᱣ���ν���http��Ӧ�е�״̬������ȡ�������������
    ngx_http_redirect_ctx_t* ctx = ngx_http_get_module_ctx(r, ngx_http_redirect_module);
    if (ctx == NULL)
    {
        return NGX_ERROR;
    }

    u = r->upstream;

    //http����ṩ��ngx_http_parse_status_line�������Խ���http
    //��Ӧ�У�������������յ����ַ������������е�ngx_http_status_t�ṹ
    rc = ngx_http_parse_status_line(r, &u->buffer, &ctx->status);
    //����NGX_AGAIN��ʾ��û�н�����������http��Ӧ�У���Ҫ���ո����
    //�ַ�����������
    if (rc == NGX_AGAIN)
    {
        return rc;
    }
    //����NGX_ERROR��û�н��յ��Ϸ���http��Ӧ��
    if (rc == NGX_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream sent no valid HTTP/1.0 header");

        r->http_version = NGX_HTTP_VERSION_9;
        u->state->status = NGX_HTTP_OK;

        return NGX_OK;
    }

    //���±�ʾ������������http��Ӧ�У���ʱ����һЩ�򵥵ĸ�ֵ��������������
    //����Ϣ���õ�r->upstream->headers_in�ṹ���У�upstream��������
    //�еİ�ͷʱ���ͻ��headers_in�еĳ�Ա���õ���Ҫ�����η��͵�
    //r->headers_out�ṹ���У�Ҳ����˵������������headers_in�����õ�
    //��Ϣ�����ն��ᷢ�����οͻ��ˡ�Ϊʲô����ֱ������r->headers_out��Ҫ
    //�������һ���أ�������Ϊupstreamϣ���ܹ�����
    //ngx_http_upstream_conf_t���ýṹ���е�hide_headers�ȳ�Ա��
    //�������ε���Ӧͷ����ͳһ����
    if (u->state)
    {
        u->state->status = ctx->status.code;
    }

    u->headers_in.status_n = ctx->status.code;

    len = ctx->status.end - ctx->status.start;
    u->headers_in.status_line.len = len;

    u->headers_in.status_line.data = ngx_pnalloc(r->pool, len);
    if (u->headers_in.status_line.data == NULL)
    {
        return NGX_ERROR;
    }

    ngx_memcpy(u->headers_in.status_line.data, ctx->status.start, len);

    //��һ������ʼ����httpͷ��������process_header�ص�����Ϊ
    //redirect_upstream_process_header��
    //֮�����յ������ַ�������redirect_upstream_process_header����
    u->process_header = redirect_upstream_process_header;

    //��������յ����ַ�������http��Ӧ���⣬���ж�����ַ���
    //����redirect_upstream_process_header��������
    return redirect_upstream_process_header(r);
}


static ngx_int_t
redirect_upstream_process_header(ngx_http_request_t *r)
{
    ngx_int_t                       rc;
    ngx_table_elt_t                *h;
    ngx_http_upstream_header_t     *hh;
    ngx_http_upstream_main_conf_t  *umcf;

    //���ｫupstreamģ��������ngx_http_upstream_main_conf_tȡ��
    //������Ŀ��ֻ��1�����Խ�Ҫת�������οͻ��˵�http��Ӧͷ����ͳһ
    //�������ýṹ���д洢����Ҫ��ͳһ������httpͷ�����ƺͻص�����
    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);

    //ѭ���Ľ������е�httpͷ��
    for ( ;; )
    {
        // http����ṩ�˻����Ե�ngx_http_parse_header_line
        //�����������ڽ���httpͷ��
        rc = ngx_http_parse_header_line(r, &r->upstream->buffer, 1);
        //����NGX_OK��ʾ������һ��httpͷ��
        if (rc == NGX_OK)
        {
            //��headers_in.headers���ngx_list_t����������httpͷ��
            h = ngx_list_push(&r->upstream->headers_in.headers);
            if (h == NULL)
            {
                return NGX_ERROR;
            }
            //���¿�ʼ����ո����ӵ�headers�����е�httpͷ��
            h->hash = r->header_hash;

            h->key.len = r->header_name_end - r->header_name_start;
            h->value.len = r->header_end - r->header_start;
            //�������ڴ���з�����httpͷ�����ڴ�
            h->key.data = ngx_pnalloc(r->pool,
                                      h->key.len + 1 + h->value.len + 1 + h->key.len);
            if (h->key.data == NULL)
            {
                return NGX_ERROR;
            }

            h->value.data = h->key.data + h->key.len + 1;
            h->lowcase_key = h->key.data + h->key.len + 1 + h->value.len + 1;

            ngx_memcpy(h->key.data, r->header_name_start, h->key.len);
            h->key.data[h->key.len] = '\0';
            ngx_memcpy(h->value.data, r->header_start, h->value.len);
            h->value.data[h->value.len] = '\0';

            if (h->key.len == r->lowcase_index)
            {
                ngx_memcpy(h->lowcase_key, r->lowcase_header, h->key.len);
            }
            else
            {
                ngx_strlow(h->lowcase_key, h->key.data, h->key.len);
            }

            //upstreamģ����һЩhttpͷ�������⴦��
            hh = ngx_hash_find(&umcf->headers_in_hash, h->hash,
                               h->lowcase_key, h->key.len);

            if (hh && hh->handler(r, h, hh->offset) != NGX_OK)
            {
                return NGX_ERROR;
            }

            continue;
        }

        //����NGX_HTTP_PARSE_HEADER_DONE��ʾ��Ӧ�����е�httpͷ��������
        //��ϣ��������ٽ��յ��Ķ�����http����
        if (rc == NGX_HTTP_PARSE_HEADER_DONE)
        {
            //���֮ǰ����httpͷ��ʱû�з���server��dateͷ�������»�
            //����httpЭ������������ͷ��
            if (r->upstream->headers_in.server == NULL)
            {
                h = ngx_list_push(&r->upstream->headers_in.headers);
                if (h == NULL)
                {
                    return NGX_ERROR;
                }

                h->hash = ngx_hash(ngx_hash(ngx_hash(ngx_hash(
                                                         ngx_hash('s', 'e'), 'r'), 'v'), 'e'), 'r');

                ngx_str_set(&h->key, "Server");
                ngx_str_null(&h->value);
                h->lowcase_key = (u_char *) "server";
            }

            if (r->upstream->headers_in.date == NULL)
            {
                h = ngx_list_push(&r->upstream->headers_in.headers);
                if (h == NULL)
                {
                    return NGX_ERROR;
                }

                h->hash = ngx_hash(ngx_hash(ngx_hash('d', 'a'), 't'), 'e');

                ngx_str_set(&h->key, "Date");
                ngx_str_null(&h->value);
                h->lowcase_key = (u_char *) "date";
            }

            return NGX_OK;
        }

        //�������NGX_AGAIN���ʾ״̬����û�н�����������httpͷ����
        //Ҫ��upstreamģ����������µ��ַ����ٽ���process_header
        //�ص���������
        if (rc == NGX_AGAIN)
        {
            return NGX_AGAIN;
        }

        //��������ֵ���ǷǷ���
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "upstream sent invalid header");

        return NGX_HTTP_UPSTREAM_INVALID_HEADER;
    }
}

static void
redirect_upstream_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                  "redirect_upstream_finalize_request");
}


static char *
ngx_http_redirect(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_redirect_handler;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_redirect_handler(ngx_http_request_t *r)
{
    ngx_http_redirect_ctx_t* myctx = ngx_http_get_module_ctx(r, ngx_http_redirect_module);
    if (myctx == NULL)
    {
        myctx = ngx_palloc(r->pool, sizeof(ngx_http_redirect_ctx_t));
        if (myctx == NULL)
        {
            return NGX_ERROR;
        }
        //���½����������������������
        ngx_http_set_ctx(r, myctx, ngx_http_redirect_module);
    }
    //��ÿ1��Ҫʹ��upstream�����󣬱��������ֻ�ܵ���1��
    //ngx_http_upstream_create�����������ʼ��r->upstream��Ա
    if (ngx_http_upstream_create(r) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "ngx_http_upstream_create() failed");
        return NGX_ERROR;
    }

    //�õ����ýṹ��ngx_http_redirect_conf_t
    ngx_http_redirect_conf_t  *mycf = (ngx_http_redirect_conf_t  *) ngx_http_get_module_loc_conf(r, ngx_http_redirect_module);
    ngx_http_upstream_t *u = r->upstream;
    //�����������ļ��еĽṹ��������r->upstream->conf��Ա
    u->conf = &mycf->upstream;
    //����ת������ʱʹ�õĻ�����
    u->buffering = mycf->upstream.buffering;


    ngx_url_t url;
    ngx_memzero(&url, sizeof(ngx_url_t));
    url.url = mycf->addr;
    url.default_port = 80;

    if (ngx_parse_url(r->pool, &url) != NGX_OK) {
        if (url.err) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "%s in upstream \"%V\"", url.err, &url.url);
        }

        return NGX_ERROR;
    }
    
    u_char *p;
    if (url.uri.len) {
        if (url.uri.data[0] == '?') {
            p = ngx_pnalloc(r->pool, url.uri.len + 1);
            if (p == NULL) {
                return NGX_ERROR;
            }

            *p++ = '/';
            ngx_memcpy(p, url.uri.data, url.uri.len);

            url.uri.len++;
            url.uri.data = p - 1;
        }
    }

    u->resolved = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_resolved_t));
    if (u->resolved == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "ngx_pcalloc resolved error. %s.", strerror(errno));
        return NGX_ERROR;
    }

    if (url.addrs) {
        u->resolved->sockaddr = url.addrs[0].sockaddr;
        u->resolved->socklen = url.addrs[0].socklen;
        u->resolved->name = url.addrs[0].name;
        u->resolved->naddrs = 1;
    }

    u->resolved->host = url.host;
    u_short port = 80;
    u->resolved->port = (in_port_t) (url.no_port ? port : url.port);
    u->resolved->no_port = url.no_port;

    myctx->redirectServer= url.host;


    //������������ʵ�ֵĻص�������Ҳ����5.3.3����5.3.5����ʵ�ֵ�3������
    u->create_request = redirect_upstream_create_request;
    u->process_header = redirect_process_status_line;
    u->finalize_request = redirect_upstream_finalize_request;

    //������뽫count��Ա��1�����ɼ�5.1.5��
    r->main->count++;
    //����upstream
    ngx_http_upstream_init(r);
    return NGX_DONE;
}

