#define _GNU_SOURCE
#include <autodo.h>
#include <mongoose.h>
#include <string.h>

/**
 * @brief Get array size.
 * @param[in] x The array
 * @return      The size.
 */
#define ARRAY_SIZE(x)   (sizeof(x) / sizeof(x[0]))

typedef struct http_server_router
{
    auto_map_node_t         node;
    struct
    {
        int                 ref_cb;         /**< Reference for callback function. */
        char*               prefix;         /**< Route prefix */
        char*               raw;            /**< Route string. */
        auto_regex_code_t*  url_pattern;
        size_t              group_cnt;
        size_t*             groups;
    } data;
} http_server_router_t;

typedef struct http_server_msg_helper
{
    int                     ret;
    struct mg_connection*   c;
    struct mg_http_message* hm;
    http_server_router_t*   router;
} http_server_msg_helper_t;

typedef struct http_server_s
{
    struct mg_mgr   mgr;

    int             looping;        /**< looping flag */
    auto_thread_t*  thread;
    auto_async_t*   async;

    auto_map_t      routers;        /**< #http_server_router_t */

    struct
    {
        char*           name;
        char*           listen_url;
        char*           serve_dir;
        char*           ssi_pattern;
    } options;
} http_server_t;

static const auto_api_t* api;

static void _http_server_destroy_route(struct lua_State* L, http_server_router_t* router)
{
    if (router->data.ref_cb != AUTO_LUA_NOREF)
    {
        api->lua->L_unref(L, AUTO_LUA_REGISTRYINDEX, router->data.ref_cb);
        router->data.ref_cb = AUTO_LUA_NOREF;
    }

    if (router->data.prefix != NULL)
    {
        free(router->data.prefix);
        router->data.prefix = NULL;
    }

    if (router->data.raw != NULL)
    {
        free(router->data.raw);
        router->data.raw = NULL;
    }

    if (router->data.url_pattern != NULL)
    {
        api->regex->destroy(router->data.url_pattern);
        router->data.url_pattern = NULL;
    }

    if (router->data.groups != NULL)
    {
        free(router->data.groups);
        router->data.groups = NULL;
    }

    free(router);
}

static void _http_server_cleanup_routers(struct lua_State* L, http_server_t* server)
{
    auto_map_node_t* node;
    while ((node = api->map->begin(&server->routers)) != NULL)
    {
        http_server_router_t* router = container_of(node, http_server_router_t, node);
        api->map->erase(&server->routers, node);

        _http_server_destroy_route(L, router);
    }
}

static int _http_server_gc(struct lua_State* L)
{
    http_server_t* server = api->lua->touserdata(L, 1);

    server->looping = 0;
    if (server->thread != NULL)
    {
        api->thread->join(server->thread);
        server->thread = NULL;
    }

    if (server->async != NULL)
    {
        api->async->destroy(server->async);
        server->async = NULL;
    }

    _http_server_cleanup_routers(L, server);

    mg_mgr_free(&server->mgr);

    if (server->options.name != NULL)
    {
        free(server->options.name);
        server->options.name = NULL;
    }
    if (server->options.listen_url != NULL)
    {
        free(server->options.listen_url);
        server->options.listen_url = NULL;
    }
    if (server->options.serve_dir != NULL)
    {
        free(server->options.serve_dir);
        server->options.serve_dir = NULL;
    }

    return 0;
}

static void _http_server_body(void* arg)
{
    http_server_t* server = arg;

    while (server->looping)
    {
        mg_mgr_poll(&server->mgr, 100);
    }
}

static int _http_server_handle_msg_lua_after(struct lua_State* L, int status, void* ctx)
{
    (void)L; (void)status; (void)ctx;
    return 0;
}

static void _http_server_handle_msg_lua(struct lua_State* L, void* arg)
{
    int i;
    http_server_msg_helper_t* helper = arg;
    http_server_router_t* router = helper->router;

    api->lua->rawgeti(L, AUTO_LUA_REGISTRYINDEX, helper->router->data.ref_cb);

    for (i = 0; i < helper->ret; i++)
    {
        size_t len = router->data.groups[2 * i + 1] - router->data.groups[2 * i];
        api->lua->pushlstring(L, helper->hm->uri.ptr + router->data.groups[2 * i], len);
    }

    api->lua->A_callk(L, helper->ret, 0, NULL, _http_server_handle_msg_lua_after);
}

static void _http_server_handle_msg(http_server_t* server, struct mg_connection* c, struct mg_http_message* hm)
{
    auto_map_node_t* it;
    http_server_msg_helper_t helper = { 0, c, hm, NULL };

    /* Check if url match router */
    for (it = api->map->begin(&server->routers); it != NULL; it = api->map->next(it))
    {
        helper.router = container_of(it, http_server_router_t, node);
        helper.ret = api->regex->match(helper.router->data.url_pattern, hm->uri.ptr, hm->uri.len,
            helper.router->data.groups, helper.router->data.group_cnt);

        if (helper.ret >= 0)
        {
            api->async->call_in_lua(server->async, _http_server_handle_msg_lua, &helper);
            return;
        }
    }

    if (server->options.serve_dir != NULL)
    {
        struct mg_http_serve_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.root_dir = server->options.serve_dir;
        opts.ssi_pattern = server->options.ssi_pattern;
        mg_http_serve_dir(c, hm, &opts);
    }
}

static void _http_server_work(struct mg_connection* c, int ev, void* ev_data, void* fn_data)
{
    http_server_t* server = fn_data;

    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message* hm = (struct mg_http_message*) ev_data;
        _http_server_handle_msg(server, c, hm);
    }
}

static char* string_replace_2(const char* orig, const char* match, const char* replace, int* replaced)
{
    const char* pos = strstr(orig, match);
    if (pos == NULL)
    {
        *replaced = 0;
        return strdup(orig);
    }

    *replaced = 1;
    size_t match_len = strlen(match);

    char* dst;
    asprintf(&dst, "%.*s%s%s", (int)(pos - orig), orig, replace, pos + match_len);

    return dst;
}

static char* string_replace(const char* orig, const char* match, const char* replace)
{
    char* dst;
    char* last_ret = (char*)orig;

    for (;;)
    {
        int replaced;
        dst = string_replace_2(last_ret, match, replace, &replaced);

        if (!replaced)
        {
            break;
        }

        if (last_ret != orig)
        {
            free(last_ret);
        }
        last_ret = dst;
        dst = NULL;
    }

    return dst;
}

static int _http_server_route_fill_prefix(http_server_router_t* route, const char* raw_route)
{
    static struct {
        const char* match;
        const char* pattern;
    } s_pattern_list[] = {
        { "<string>",   "([^/\\s]+)" },
        { "<int>",      "(\\d+)" },
        { "<float>",    "([+-]?[0-9]+\\.?[0-9+])" },
        { "<path>",     "([^\\s]+)" },
        { "<uuid>",     "([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})" },
    };

    size_t i;
    char* last_str = (char*)raw_route;

    for (i = 0; i < ARRAY_SIZE(s_pattern_list); i++)
    {
        route->data.prefix = string_replace(last_str, s_pattern_list[i].match, s_pattern_list[i].pattern);
        if (last_str != raw_route)
        {
            free(last_str);
        }
        last_str = route->data.prefix;
    }

    route->data.url_pattern = api->regex->create(route->data.prefix, strlen(route->data.prefix));
    if (route->data.url_pattern == NULL)
    {
        return 0;
    }

    route->data.group_cnt = api->regex->get_group_count(route->data.url_pattern);
    if (route->data.group_cnt != 0)
    {
        route->data.groups = malloc(sizeof(size_t) * route->data.group_cnt * 2);
    }

    return route->data.url_pattern != NULL;
}

static int _http_server_route(struct lua_State* L)
{
    http_server_t* server = api->lua->touserdata(L, 1);
    const char* raw_route = api->lua->L_checkstring(L, 2);
    api->lua->L_checktype(L, 3, AUTO_LUA_TFUNCTION);

    /* Only 3 arguments needed. */
    api->lua->settop(L, 3);

    http_server_router_t* route = malloc(sizeof(http_server_router_t));
    memset(route, 0, sizeof(*route));
    route->data.ref_cb = AUTO_LUA_NOREF;
    route->data.raw = strdup(raw_route);
    route->data.ref_cb = api->lua->L_ref(L, AUTO_LUA_REGISTRYINDEX);

    if (!_http_server_route_fill_prefix(route, raw_route))
    {
        goto failure;
    }

    if (api->map->insert(&server->routers, &route->node) != NULL)
    {
        goto failure;
    }

    api->lua->pushboolean(L, 1);
    return 1;

failure:
    _http_server_destroy_route(L, route);
    api->lua->pushboolean(L, 0);
    return 1;
}

static int _http_server_run(struct lua_State* L)
{
    http_server_t* server = api->lua->touserdata(L, 1);

    /* Start listen */
    struct mg_connection* c = mg_http_listen(&server->mgr,
        server->options.listen_url, _http_server_work, server);
    if (c == NULL)
    {
        api->lua->pushboolean(L, 0);
        return 1;
    }

    /* Create background thread for serving */
    server->thread = api->thread->create(_http_server_body, server);

    api->lua->pushboolean(L, 1);
    return 1;
}

static int _http_server_cmp_route(const auto_map_node_t* key1,
    const auto_map_node_t* key2, void* arg)
{
    (void)arg;
    http_server_router_t* r1 = container_of(key1, http_server_router_t, node);
    http_server_router_t* r2 = container_of(key2, http_server_router_t, node);
    return strcmp(r1->data.raw, r2->data.raw);
}

static void _http_server_set_metatable(struct lua_State* L)
{
    static const auto_luaL_Reg s_http_server_meta[] = {
        { "__gc",       _http_server_gc },
        { NULL,         NULL },
    };
    static const auto_luaL_Reg s_http_server_method[] = {
        { "route",      _http_server_route },
        { "run",        _http_server_run },
        { NULL,         NULL },
    };
    if (api->lua->L_newmetatable(L, "__auto_http_server") != 0)
    {
        api->lua->L_setfuncs(L, s_http_server_meta, 0);
        api->lua->L_newlib(L, s_http_server_method);
        api->lua->setfield(L, -2, "__index");
    }
    api->lua->setmetatable(L, -2);
}

static void _http_server_parse_options(struct lua_State* L, int idx, http_server_t* server)
{
    /* listen_url */
    if (api->lua->getfield(L, idx, "listen_url") == AUTO_LUA_TSTRING)
    {
        server->options.listen_url = strdup(api->lua->tostring(L, -1));
    }
    else
    {
        server->options.listen_url = strdup("http://127.0.0.1:5000");
    }
    api->lua->pop(L, 1);

    /* name */
    if (api->lua->getfield(L, idx, "name") == AUTO_LUA_TSTRING)
    {
        server->options.name = strdup(api->lua->tostring(L, -1));
    }
    else
    {
        server->options.name = strdup("autodo-mongoose");
    }
    api->lua->pop(L, 1);

    /* serve_dir */
    if (api->lua->getfield(L, idx, "serve_dir") == AUTO_LUA_TSTRING)
    {
        server->options.serve_dir = strdup(api->lua->tostring(L, -1));
    }
    api->lua->pop(L, 1);

    /* ssi_pattern */
    if (api->lua->getfield(L, idx, "ssi_pattern") == AUTO_LUA_TSTRING)
    {
        server->options.ssi_pattern = strdup(api->lua->tostring(L, -1));
    }
    api->lua->pop(L, 1);
}

static int _http_server(struct lua_State* L)
{
    http_server_t* server = api->lua->newuserdatauv(L, sizeof(http_server_t), 0);
    memset(server, 0, sizeof(*server));

    _http_server_set_metatable(L);
    _http_server_parse_options(L, 1, server);

    mg_mgr_init(&server->mgr);
    server->looping = 1;
    api->map->init(&server->routers, _http_server_cmp_route, NULL);

    server->async = api->async->create(api->lua->newthread(L));
    api->lua->pop(L, 1);

    return 1;
}

AUTO_EXPORT int luaopen_mongoose(struct lua_State* L)
{
    api = auto_api();

    static const auto_luaL_Reg s_http_method[] = {
        { "http_server",    _http_server },
        { NULL,             NULL },
    };
    api->lua->L_newlib(L, s_http_method);

    return 1;
}
