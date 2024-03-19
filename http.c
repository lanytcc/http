
#include "quickjs-libc.h"
#include "util.h"

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#define export_fn __declspec(dllexport)
#else
#define export_fn __attribute__((visibility("default")))
#endif

#include <curl/curl.h>
#include <cutils.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <quickjs.h>

enum {
    HTTP_REQ_METHOD,
    HTTP_REQ_URI,
    HTTP_REQ_BODY,
    HTTP_REQ_PARAMS,
    HTTP_REQ_HEADERS,
    HTTP_REQ_COUNT,
};

// http request object
typedef struct {
    JSContext *ctx;
    char *str_fields[HTTP_REQ_PARAMS];
    JSValue js_fields[HTTP_REQ_COUNT - HTTP_REQ_PARAMS];
} http_req;

static const char *http_req_fields[] = {
    "method", "uri", "body", "params", "headers",
};

// 只在载入时修改一次
static JSClassID http_req_class_id = 0;

static void http_req_finalizer(JSRuntime *rt, JSValue val) {
    http_req *req = JS_GetOpaque(val, http_req_class_id);
    if (req) {
        for (size_t i = 0; i < HTTP_REQ_PARAMS; ++i) {
            js_free(req->ctx, req->str_fields[i]);
        }
        for (size_t i = 0; i < HTTP_REQ_COUNT - HTTP_REQ_PARAMS; ++i) {
            JS_FreeValue(req->ctx, req->js_fields[i]);
        }
        js_free(req->ctx, req);
    }
}

static JSClassDef http_req_class = {
    .class_name = "request",
    .finalizer = http_req_finalizer,
};

static JSValue http_req_set(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv);
static JSValue http_req_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                             JSValueConst *argv) {
    JSValue obj = JS_UNDEFINED;
    http_req *req = NULL;
    JSValue proto;
    JSClassID class_id;

    class_id = http_req_class_id;
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        goto fail;
    if (JS_IsObject(proto)) {
        obj = JS_NewObjectProtoClass(ctx, proto, class_id);
        if (JS_IsException(obj))
            goto fail;
    } else {
        JS_ThrowTypeError(ctx, "prototype property is not an object");
        goto fail;
    }
    req = js_mallocz(ctx, sizeof(*req));
    if (!req) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    req->ctx = ctx;
    for (int i = 0; i < HTTP_REQ_COUNT - HTTP_REQ_PARAMS; ++i) {
        req->js_fields[i] = JS_UNDEFINED;
    }

    JS_SetOpaque(obj, req);
    if (argc > 0) {
        if (JS_IsObject(argv[0])) {
            if (JS_IsException(http_req_set(ctx, obj, argc, argv))) {
                goto fail;
            }
        } else {
            JS_ThrowTypeError(ctx, "request([val]), val must be object");
            goto fail;
        }
    }
    JS_FreeValue(ctx, proto);
    return obj;
fail:
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, obj);
    js_free(ctx, req);
    return JS_EXCEPTION;
}

// return json object
static JSValue http_req_get(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    http_req *req = JS_GetOpaque2(ctx, this_val, http_req_class_id);
    JSValue obj;
    if (!req)
        return JS_EXCEPTION;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (size_t i = 0; i < HTTP_REQ_PARAMS; ++i) {
        if (req->str_fields[i]) {
            JS_DefinePropertyValueStr(ctx, obj, http_req_fields[i],
                                      JS_NewString(ctx, req->str_fields[i]),
                                      JS_PROP_C_W_E);
        }
    }
    for (size_t i = 0; i < HTTP_REQ_COUNT - HTTP_REQ_PARAMS; ++i) {
        if (!JS_IsUndefined(req->js_fields[i])) {
            JS_DefinePropertyValueStr(ctx, obj,
                                      http_req_fields[i + HTTP_REQ_PARAMS],
                                      req->js_fields[i], JS_PROP_C_W_E);
        }
    }
    return obj;
}

static JSValue http_req_set(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    http_req *req = JS_GetOpaque2(ctx, this_val, http_req_class_id);
    JSValue v, val;
    if (!req)
        return JS_EXCEPTION;
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "set([val]), val must be object");
        return JS_EXCEPTION;
    }
    val = argv[0];
    if (!JS_IsObject(val)) {
        JS_ThrowTypeError(ctx, "set([val]), val must be object");
        return JS_EXCEPTION;
    }

    for (int i = 0; i < HTTP_REQ_PARAMS; ++i) {
        v = JS_GetPropertyStr(ctx, val, http_req_fields[i]);
        if (JS_IsUndefined(v))
            continue;
        if (!JS_IsString(v)) {
            JS_ThrowTypeError(ctx,
                              "request([val]), val's fields must be string");
            return JS_EXCEPTION;
        }
        req->str_fields[i] = (char *)JS_ToCString(ctx, v);
    }
    for (int i = 0; i < HTTP_REQ_COUNT - HTTP_REQ_PARAMS; ++i) {
        v = JS_GetPropertyStr(ctx, val, http_req_fields[i + HTTP_REQ_PARAMS]);
        if (JS_IsUndefined(v))
            continue;
        if (!JS_IsObject(v)) {
            JS_ThrowTypeError(
                ctx, "request([val]), val's params, headers must be object");
            return JS_EXCEPTION;
        }
        req->js_fields[i] = JS_DupValue(ctx, v);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry http_req_proto_funcs[] = {
    JS_CFUNC_DEF("get", 0, http_req_get),
    JS_CFUNC_DEF("set", 1, http_req_set),
};

// http response object
typedef struct {
    JSContext *ctx;
    int status;
    char *reason;
    char *body;
    JSValue headers;
} http_res;

static JSClassID http_res_class_id = 0;

static void http_res_finalizer(JSRuntime *rt, JSValue val) {
    http_res *res = JS_GetOpaque(val, http_res_class_id);
    if (res) {
        js_free(res->ctx, res->reason);
        js_free(res->ctx, res->body);
        JS_FreeValue(res->ctx, res->headers);
        js_free(res->ctx, res);
    }
}

static JSClassDef http_res_class = {
    .class_name = "response",
    .finalizer = http_res_finalizer,
};
static JSValue http_res_set(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv);
static JSValue http_res_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                             JSValueConst *argv) {
    JSValue obj = JS_UNDEFINED;
    http_res *res = NULL;
    JSValue proto;
    JSClassID class_id;

    class_id = http_res_class_id;
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        goto fail;
    if (JS_IsObject(proto)) {
        obj = JS_NewObjectProtoClass(ctx, proto, class_id);
        if (JS_IsException(obj))
            goto fail;
    } else {
        JS_ThrowTypeError(ctx, "prototype property is not an object");
        goto fail;
    }
    res = js_mallocz(ctx, sizeof(*res));
    if (!res) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
    res->ctx = ctx;
    res->status = 200;
    res->headers = JS_UNDEFINED;

    JS_SetOpaque(obj, res);
    if (argc > 0) {
        if (JS_IsObject(argv[0])) {
            if (JS_IsException(http_res_set(ctx, obj, argc, argv))) {
                goto fail;
            }
        } else {
            JS_ThrowTypeError(ctx, "response([val]), val must be object");
            goto fail;
        }
    }
    JS_FreeValue(ctx, proto);
    return obj;
fail:
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, obj);
    js_free(ctx, res);
    return JS_EXCEPTION;
}

static JSValue http_res_get(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    http_res *res = JS_GetOpaque2(ctx, this_val, http_res_class_id);
    JSValue obj;
    if (!res)
        return JS_EXCEPTION;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    JS_DefinePropertyValueStr(ctx, obj, "status", JS_NewInt32(ctx, res->status),
                              JS_PROP_C_W_E);
    if (res->reason)
        JS_DefinePropertyValueStr(
            ctx, obj, "reason", JS_NewString(ctx, res->reason), JS_PROP_C_W_E);
    if (res->body)
        JS_DefinePropertyValueStr(ctx, obj, "body",
                                  JS_NewString(ctx, res->body), JS_PROP_C_W_E);

    if (!JS_IsUndefined(res->headers))
        JS_DefinePropertyValueStr(ctx, obj, "headers", res->headers,
                                  JS_PROP_C_W_E);

    return obj;
}

static JSValue http_res_set(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    http_res *res = JS_GetOpaque2(ctx, this_val, http_res_class_id);
    JSValue v, val;
    if (!res)
        return JS_EXCEPTION;
    if (argc < 1) {
        JS_ThrowTypeError(ctx, "set([val]), val must be object");
        return JS_EXCEPTION;
    }
    val = argv[0];
    if (!JS_IsObject(val)) {
        JS_ThrowTypeError(ctx, "set([val]), val must be object");
        return JS_EXCEPTION;
    }

    v = JS_GetPropertyStr(ctx, val, "status");
    if (!JS_IsUndefined(v)) {
        if (JS_IsNumber(v))
            JS_ToInt32(ctx, &res->status, v);
        else {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                              "response([val]), val.status must be number");
            return JS_EXCEPTION;
        }
    }

    v = JS_GetPropertyStr(ctx, val, "reason");
    if (!JS_IsUndefined(v)) {
        if (JS_IsString(v)) {
            js_free(ctx, res->reason);
            res->reason = (char *)JS_ToCString(ctx, v);
        } else {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                              "response([val]), val.reason must be string");
            return JS_EXCEPTION;
        }
    }

    v = JS_GetPropertyStr(ctx, val, "body");
    if (!JS_IsUndefined(v)) {
        if (JS_IsString(v)) {
            js_free(ctx, res->body);
            res->body = (char *)JS_ToCString(ctx, v);
        } else {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx, "response([val]), val.body must be string");
            return JS_EXCEPTION;
        }
    }

    v = JS_GetPropertyStr(ctx, val, "headers");
    if (!JS_IsUndefined(v)) {
        if (JS_IsObject(v)) {
            JS_FreeValue(ctx, res->headers);
            res->headers = JS_DupValue(ctx, v);
        } else {
            JS_FreeValue(ctx, v);
            JS_ThrowTypeError(ctx,
                              "response([val]), val.headers must be object");
            return JS_EXCEPTION;
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry http_res_proto_funcs[] = {
    JS_CFUNC_DEF("get", 0, http_res_get),
    JS_CFUNC_DEF("set", 1, http_res_set),
};

// to str, like "key1=value1&key2=value2"
static JSValue params_helper(JSContext *ctx, http_req *req, char **params_str) {
    JSValue val;
    JSPropertyEnum *tab;
    uint32_t len;
    uint64_t cnt = 0, cnt_buf1, cnt_buf2;
    char *str_buf1 = NULL, *str_buf2 = NULL, *key, *value;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, req->js_fields[0],
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return JS_EXCEPTION;
    }
    for (uint32_t i = 0; i < len; ++i) {
        val = JS_GetProperty(ctx, req->js_fields[0], tab[i].atom);
        if (!JS_IsString(val)) {
            JS_ThrowTypeError(ctx, "params's value must be string");
            goto fail1;
        }

        key = (char *)JS_AtomToCString(ctx, tab[i].atom);
        value = (char *)JS_ToCString(ctx, val);
        if (!key || !value) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }

        cnt_buf1 = calculate_encoded_size(key) + 1;
        str_buf1 = js_malloc(ctx, cnt_buf1);
        if (!str_buf1)
            goto fail2;
        urlencode(key, str_buf1 + cnt, cnt_buf1);
        cnt += cnt_buf1;

        cnt_buf2 = calculate_encoded_size(value) + 1;
        str_buf2 = js_malloc(ctx, cnt_buf2);
        if (!str_buf2) {
            js_free(ctx, str_buf1);
            goto fail2;
        }
        urlencode(value, str_buf2, cnt_buf2);
        cnt += cnt_buf2;

        if (*params_str) {
            cnt += 1;
            *params_str = js_realloc(ctx, *params_str, cnt);
            if (!*params_str) {
                js_free(ctx, str_buf1);
                js_free(ctx, str_buf2);
                goto fail2;
            }
            strcat(*params_str, "&");
        } else {
            *params_str = js_mallocz(ctx, cnt);
            if (!*params_str) {
                js_free(ctx, str_buf1);
                js_free(ctx, str_buf2);
            fail2:
                js_free(ctx, key);
                js_free(ctx, value);
                goto fail;
            }
        }
        strcat(*params_str, str_buf1);
        strcat(*params_str, "=");
        strcat(*params_str, str_buf2);

        js_free(ctx, str_buf1);
        js_free(ctx, str_buf2);
        js_free(ctx, key);
        js_free(ctx, value);

        JS_FreeValue(ctx, val);
    }

    for (uint32_t i = 0; i < len; ++i) {
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, val);
fail1:
    for (uint32_t i = 0; i < len; ++i) {
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    return JS_EXCEPTION;
}

static JSValue params_to_obj(JSContext *ctx, const char *params_str) {
    JSValue obj = JS_UNDEFINED;
    char *p, *q, *name, *value;
    if (!params_str)
        return JS_UNDEFINED;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    p = (char *)params_str;
    while (*p) {
        q = strchr(p, '=');
        if (!q)
            break;
        name = js_mallocz(ctx, q - p + 1);
        if (!name)
            goto fail;
        memcpy(name, p, q - p);
        p = q + 1;
        q = strchr(p, '&');
        if (!q) {
            value = js_mallocz(ctx, strlen(p) + 1);
            if (!value) {
                js_free(ctx, name);
                goto fail;
            }
            strcpy(value, p);
            p += strlen(p);
        } else {
            value = js_mallocz(ctx, q - p + 1);
            if (!value) {
                js_free(ctx, name);
                goto fail;
            }
            memcpy(value, p, q - p);
            p = q + 1;
        }
        JS_DefinePropertyValueStr(ctx, obj, name, JS_NewString(ctx, value),
                                  JS_PROP_C_W_E);
        js_free(ctx, name);
        js_free(ctx, value);
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue ev_params_to_obj(JSContext *ctx, struct evkeyvalq *params) {
    JSValue obj = JS_UNDEFINED;
    if (!params)
        return JS_UNDEFINED;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (struct evkeyval *p = params->tqh_first; p; p = p->next.tqe_next) {
        JS_DefinePropertyValueStr(ctx, obj, p->key, JS_NewString(ctx, p->value),
                                  JS_PROP_C_W_E);
    }
    return obj;
}

// to str, like "Header1: Value1\r\nHeader2: Value2\r\n"
static JSValue headers_helper(JSContext *ctx, http_req *req,
                              char **headers_str) {
    JSValue val;
    JSPropertyEnum *tab;
    uint32_t len;
    uint64_t cnt = 0;
    char *header_name, *header_value;
    if (JS_GetOwnPropertyNames(
            ctx, &tab, &len, req->js_fields[HTTP_REQ_HEADERS - HTTP_REQ_PARAMS],
            JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return JS_EXCEPTION;
    }
    for (uint32_t i = 0; i < len; ++i) {
        val = JS_GetProperty(ctx,
                             req->js_fields[HTTP_REQ_HEADERS - HTTP_REQ_PARAMS],
                             tab[i].atom);
        if (!JS_IsString(val)) {
            JS_ThrowTypeError(ctx, "Header's value must be a string");
            goto fail1;
        }

        header_name = (char *)JS_AtomToCString(ctx, tab[i].atom);
        header_value = (char *)JS_ToCString(ctx, val);
        if (!header_name || !header_value) {
            JS_ThrowOutOfMemory(ctx);
            goto fail;
        }
        // 2 for ": " and 2 for "\r\n"
        cnt += strlen(header_name) + strlen(header_value) + 5;
        if (*headers_str) {
            *headers_str = js_realloc(ctx, *headers_str, cnt);
            if (!*headers_str)
                goto fail2;
        } else {
            *headers_str = js_mallocz(ctx, cnt);
            if (!*headers_str) {
            fail2:
                js_free(ctx, header_name);
                js_free(ctx, header_value);
                goto fail;
            }
        }
        strcat(*headers_str, header_name);
        strcat(*headers_str, ": ");
        strcat(*headers_str, header_value);
        strcat(*headers_str, "\r\n");

        js_free(ctx, header_name);
        js_free(ctx, header_value);

        JS_FreeValue(ctx, val);
    }

    for (uint32_t i = 0; i < len; ++i) {
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, val);
fail1:
    for (uint32_t i = 0; i < len; ++i) {
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
    return JS_EXCEPTION;
}

static JSValue headers_to_obj(JSContext *ctx, const char *headers_str) {
    JSValue obj = JS_UNDEFINED;
    char *p, *q, *name, *value;
    if (!headers_str)
        return JS_UNDEFINED;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    p = (char *)headers_str;
    while (*p) {
        q = strchr(p, ':');
        if (!q)
            break;
        name = js_mallocz(ctx, q - p + 1);
        if (!name)
            goto fail;
        memcpy(name, p, q - p);
        p = q + 1;
        while (*p == ' ')
            p++;
        q = strchr(p, '\r');
        if (!q) {
            js_free(ctx, name);
            break;
        }
        value = js_mallocz(ctx, q - p + 1);
        if (!value) {
            js_free(ctx, name);
            goto fail;
        }
        memcpy(value, p, q - p);
        p = q + 2;
        JS_DefinePropertyValueStr(ctx, obj, name, JS_NewString(ctx, value),
                                  JS_PROP_C_W_E);
        js_free(ctx, name);
        js_free(ctx, value);
    }
    return obj;
fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue ev_headers_to_obj(JSContext *ctx, struct evkeyvalq *headers) {
    JSValue obj = JS_UNDEFINED;
    if (!headers)
        return JS_UNDEFINED;
    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    for (struct evkeyval *p = headers->tqh_first; p; p = p->next.tqe_next) {
        JS_DefinePropertyValueStr(ctx, obj, p->key, JS_NewString(ctx, p->value),
                                  JS_PROP_C_W_E);
    }
    return obj;
}

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *data) {
    http_res *res = data;
    size_t realsize = size * nmemb;
    res->body = js_realloc(res->ctx, res->body, realsize + 1);
    if (!res->body)
        return 0;
    memcpy(res->body, ptr, realsize);
    res->body[realsize] = 0;
    return realsize;
}

static size_t header_callback(void *ptr, size_t size, size_t nmemb,
                              void *data) {
    char *res = data;
    size_t realsize = size * nmemb;
    res = realloc(res, realsize + 1);
    if (!res)
        return 0;
    memcpy(res, ptr, realsize);
    res[realsize] = 0;
    return realsize;
}

static JSValue http_fetch(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    http_req *req;
    http_res *res;
    CURL *curl;
    CURLcode ret;
    struct curl_slist *headers = NULL;
    char *headers_str, *params_str, *back_headers_str = NULL;
    JSValue obj;
    JSValueConst args[1];

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fetch([req]), req must be object");
    req = JS_GetOpaque2(ctx, argv[0], http_req_class_id);
    if (!req)
        return JS_ThrowTypeError(ctx, "fetch([req]), req must be object");

    curl = curl_easy_init();
    if (!curl)
        return JS_ThrowTypeError(ctx, "curl_easy_init failed");

    if (req->str_fields[HTTP_REQ_URI]) {
        curl_easy_setopt(curl, CURLOPT_URL, req->str_fields[HTTP_REQ_URI]);
    } else {
        curl_easy_cleanup(curl);
        return JS_ThrowTypeError(ctx, "fetch([req]), req.uri must be string");
    }

    if (req->str_fields[HTTP_REQ_METHOD])
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,
                         req->str_fields[HTTP_REQ_METHOD]);
    if (req->str_fields[HTTP_REQ_BODY])
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                         req->str_fields[HTTP_REQ_BODY]);

    if (!JS_IsUndefined(req->js_fields[HTTP_REQ_PARAMS - HTTP_REQ_PARAMS])) {
        if (JS_IsException(params_helper(ctx, req, &params_str))) {
            curl_easy_cleanup(curl);
            return JS_EXCEPTION;
        }
        if (params_str) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params_str);
        }
    }
    if (!JS_IsUndefined(req->js_fields[HTTP_REQ_HEADERS - HTTP_REQ_PARAMS])) {
        if (JS_IsException(headers_helper(ctx, req, &headers_str))) {
            js_free(ctx, params_str);
            curl_easy_cleanup(curl);
            return JS_EXCEPTION;
        }
        if (headers_str) {
            headers = curl_slist_append(headers, headers_str);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
    }

    res = js_mallocz(ctx, sizeof(*res));
    if (!res) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    res->ctx = ctx;
    res->body = NULL;
    res->headers = JS_UNDEFINED;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, res);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, back_headers_str);

    ret = curl_easy_perform(curl);
    if (ret != CURLE_OK) {
        JS_ThrowTypeError(ctx, "curl_easy_perform failed: %s",
                          curl_easy_strerror(ret));
        js_free(ctx, params_str);
        js_free(ctx, headers_str);
        goto fail;
    }

    js_free(ctx, params_str);
    js_free(ctx, headers_str);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res->status);
    if (back_headers_str) {
        res->headers = headers_to_obj(ctx, back_headers_str);
        free(back_headers_str);
    }

    curl_easy_cleanup(curl);

    obj = JS_NewObjectClass(ctx, http_res_class_id);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, res);
    args[0] = obj;

    curl_slist_free_all(headers);

    return obj;
fail:
    js_free(ctx, res->body);
    JS_FreeValue(ctx, res->headers);
    js_free(ctx, res);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return JS_EXCEPTION;
}

typedef struct http_server_cb http_server_cb;
typedef struct {
    JSContext *ctx;
    struct event_base *base;
    struct evhttp *http;
    JSValue *callbacks;
    size_t callbacks_len;
    http_server_cb *cbs;
    size_t cbs_len;
#ifdef _WIN32
    WSADATA wsaData;
#endif
} http_server;

struct http_server_cb {
    JSContext *ctx;
    http_server *server;
    JSValue server_this;
    size_t callback_index;
};

static JSClassID http_server_class_id = 0;

static void http_server_finalizer(JSRuntime *rt, JSValue val) {
    http_server *server = JS_GetOpaque(val, http_server_class_id);
    if (server) {
        evhttp_free(server->http);
        event_base_free(server->base);
        for (size_t i = 0; i < server->callbacks_len; ++i) {
            JS_FreeValue(server->ctx, server->callbacks[i]);
        }
        js_free(server->ctx, server->callbacks);
        js_free(server->ctx, server->cbs);
        js_free(server->ctx, server);
#ifdef _WIN32
        WSACleanup();
#endif
    }
}

static JSClassDef http_server_class = {
    .class_name = "server",
    .finalizer = http_server_finalizer,
};

static JSValue http_server_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv) {
    JSValue obj = JS_UNDEFINED;
    http_server *server = NULL;
    JSValue proto;
    JSClassID class_id;

    class_id = http_server_class_id;
    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        goto fail;
    if (JS_IsObject(proto)) {
        obj = JS_NewObjectProtoClass(ctx, proto, class_id);
        if (JS_IsException(obj))
            goto fail;
    } else {
        JS_ThrowTypeError(ctx, "prototype property is not an object");
        goto fail;
    }
    server = js_mallocz(ctx, sizeof(*server));
    if (!server) {
        JS_ThrowOutOfMemory(ctx);
        goto fail;
    }
#ifdef _WIN32
    WSAStartup(MAKEWORD(2, 2), &server->wsaData);
#endif
    server->ctx = ctx;
    server->base = event_base_new();
    if (!server->base) {
        JS_ThrowInternalError(ctx, "event_base_new failed");
        goto fail;
    }
    server->http = evhttp_new(server->base);
    if (!server->http) {
        JS_ThrowInternalError(ctx, "evhttp_new failed");
        goto fail;
    }
    JS_SetOpaque(obj, server);
    JS_FreeValue(ctx, proto);
    return obj;
fail:
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, obj);
    js_free(ctx, server);
    return JS_EXCEPTION;
}

static JSValue http_server_listen(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    http_server *server = JS_GetOpaque2(ctx, this_val, http_server_class_id);
    char *address;
    int port;
    if (!server)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "listen([address, port]), address and "
                                      "port must be string and number");
    if (JS_ToCString(ctx, argv[0]) && JS_ToInt32(ctx, &port, argv[1])) {
        return JS_ThrowTypeError(ctx, "listen([address, port]), address and "
                                      "port must be string and number");
    }
    address = (char *)JS_ToCString(ctx, argv[0]);
    if (!address)
        return JS_EXCEPTION;
    if (evhttp_bind_socket(server->http, address, port) < 0) {
        js_free(ctx, address);
        return JS_ThrowInternalError(ctx, "Failed to bind to port: %d", port);
    }
    js_free(ctx, address);
    return JS_UNDEFINED;
}

static const char *evhttp_cmd_str[] = {
    "GET", "POST", "HEAD", "PUT", "DELETE", "OPTIONS", "TRACE", "CONNECT",
};

static const char *evhttp_cmd_type_to_str(enum evhttp_cmd_type type) {
    size_t i = 0;
    if (type < EVHTTP_REQ_GET || type > EVHTTP_REQ_PATCH)
        return NULL;
    for (i = 0; i < countof(evhttp_cmd_str); ++i) {
        if (type & (1 << i))
            break;
    }
    return evhttp_cmd_str[type];
}

static void callback_helper(struct evhttp_request *req, void *arg) {
    http_server_cb *cb = arg;
    JSAtom atom;
    JSValue argv[1], ret, key, value;
    http_req *req_obj;
    http_res *res_obj;
    const char *uri_str, *method_str;
    struct evkeyvalq *headers, uri_params;
    struct evbuffer *buf;
    size_t len, idx = 0;

    uri_str = evhttp_request_get_uri(req);
    enum evhttp_cmd_type method = evhttp_request_get_command(req);
    method_str = evhttp_cmd_type_to_str(method);
    if (!method_str || !uri_str) {
        JS_ThrowInternalError(cb->ctx, "method_str or uri_str is NULL");
        js_std_dump_error(cb->ctx);
        return;
    }
    evhttp_parse_query_str(uri_str, &uri_params);
    headers = evhttp_request_get_input_headers(req);

    req_obj = js_mallocz(cb->ctx, sizeof(*req_obj));
    if (!req_obj) {
        js_std_dump_error(cb->ctx);
        return;
    }
    req_obj->ctx = cb->ctx;
    req_obj->str_fields[HTTP_REQ_URI] = js_strdup(cb->ctx, uri_str);
    req_obj->str_fields[HTTP_REQ_METHOD] = js_strdup(cb->ctx, method_str);
    buf = evhttp_request_get_input_buffer(req);
    len = evbuffer_get_length(buf);
    if (len > 0 && (method == EVHTTP_REQ_POST || method == EVHTTP_REQ_PUT ||
                    method == EVHTTP_REQ_PATCH)) {
        req_obj->str_fields[HTTP_REQ_BODY] = js_malloc(cb->ctx, len + 1);
        if (!req_obj->str_fields[HTTP_REQ_BODY]) {
            js_std_dump_error(cb->ctx);
            return;
        }
        evbuffer_copyout(buf, req_obj->str_fields[HTTP_REQ_BODY], len);
        req_obj->str_fields[HTTP_REQ_BODY][len] = '\0';
    }
    req_obj->js_fields[HTTP_REQ_PARAMS - HTTP_REQ_PARAMS] =
        ev_params_to_obj(cb->ctx, &uri_params);
    req_obj->js_fields[HTTP_REQ_HEADERS - HTTP_REQ_PARAMS] =
        ev_headers_to_obj(cb->ctx, headers);

    argv[0] = JS_NewObjectClass(cb->ctx, http_req_class_id);
    if (JS_IsException(argv[0])) {
        for (size_t i = 0; i < HTTP_REQ_PARAMS; ++i) {
            js_free(req_obj->ctx, req_obj->str_fields[i]);
        }
        js_free(cb->ctx, req_obj);
        js_std_dump_error(cb->ctx);
        return;
    }
    JS_SetOpaque(argv[0], req_obj);

    ret = JS_Call(cb->ctx, cb->server->callbacks[cb->callback_index],
                  cb->server_this, 1, argv);

    if (!JS_IsObject(ret)) {
        js_std_dump_error(cb->ctx);
        return;
    }
    res_obj = JS_GetOpaque(ret, http_res_class_id);
    if (!res_obj) {
        JS_ThrowInternalError(cb->ctx, "callback must return response object");
        js_std_dump_error(cb->ctx);
        return;
    }
    buf = evbuffer_new();
    if (!buf) {
        JS_ThrowOutOfMemory(cb->ctx);
        js_std_dump_error(cb->ctx);
        return;
    }
    // if (JS_GetPropertyLength(cb->ctx, (int64_t *)&len, res_obj->headers) ==
    //     -1) {
    //     js_std_dump_error(cb->ctx);
    //     return;
    // }
    while (!JS_IsUndefined(res_obj->headers)) {
        key = JS_GetPropertyUint32(cb->ctx, res_obj->headers, idx++);
        if (JS_IsUndefined(key))
            break;
        if (JS_IsException(key)) {
            js_std_dump_error(cb->ctx);
            return;
        }
        atom = JS_ValueToAtom(cb->ctx, key);
        if (unlikely(atom == JS_ATOM_NULL)) {
            js_std_dump_error(cb->ctx);
            return;
        }
        value = JS_GetProperty(cb->ctx, res_obj->headers, atom);
        JS_FreeAtom(cb->ctx, atom);
        if (JS_IsException(value)) {
            js_std_dump_error(cb->ctx);
            return;
        }
        evhttp_add_header(evhttp_request_get_output_headers(req),
                          JS_ToCString(cb->ctx, key),
                          JS_ToCString(cb->ctx, value));
        JS_FreeValue(cb->ctx, key);
        JS_FreeValue(cb->ctx, value);
    }
    if (res_obj->body)
        evbuffer_add(buf, res_obj->body, strlen(res_obj->body));
    evhttp_send_reply(req, res_obj->status, res_obj->reason, buf);
    evbuffer_free(buf);
}

static JSValue http_server_on(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
    http_server *server = JS_GetOpaque2(ctx, this_val, http_server_class_id);
    char *path;
    JSValue call_back;

    if (!server)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "on([path, handler]), path and handler "
                                      "must be string and function");
    if (!JS_ToCString(ctx, argv[0]) || !JS_IsFunction(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "on([path, handler]), path and handler "
                                      "must be string and function");

    path = (char *)JS_ToCString(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;

    call_back = JS_DupValue(ctx, argv[1]);
    server->callbacks = js_realloc(
        ctx, server->callbacks, (server->callbacks_len + 1) * sizeof(JSValue));
    if (!server->callbacks) {
        js_free(ctx, path);
        JS_FreeValue(ctx, call_back);
        return JS_ThrowOutOfMemory(ctx);
    }
    server->callbacks[server->callbacks_len] = call_back;
    server->callbacks_len++;

    server->cbs = js_realloc(ctx, server->cbs,
                             (server->cbs_len + 1) * sizeof(http_server_cb));
    if (!server->cbs) {
        js_free(ctx, path);
        JS_FreeValue(ctx, call_back);
        return JS_ThrowOutOfMemory(ctx);
    }
    server->cbs[server->cbs_len].ctx = ctx;
    server->cbs[server->cbs_len].server = server;
    server->cbs[server->cbs_len].server_this = this_val;
    server->cbs[server->cbs_len].callback_index = server->callbacks_len - 1;
    server->cbs_len++;

    if (evhttp_set_cb(server->http, path, callback_helper,
                      &server->cbs[server->cbs_len - 1]) < 0) {
        js_free(ctx, path);
        JS_FreeValue(ctx, call_back);
        return JS_ThrowInternalError(ctx, "Failed to set callback for path: %s",
                                     path);
    }
    js_free(ctx, path);
    return JS_UNDEFINED;
}

static JSValue http_server_dispatch(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
    http_server *server = JS_GetOpaque2(ctx, this_val, http_server_class_id);
    if (!server)
        return JS_EXCEPTION;
    if (event_base_dispatch(server->base) < 0)
        return JS_ThrowInternalError(ctx, "dispatch failed");
    return JS_UNDEFINED;
}

static JSValue http_server_break(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    http_server *server = JS_GetOpaque2(ctx, this_val, http_server_class_id);
    if (!server)
        return JS_EXCEPTION;
    if (event_base_loopbreak(server->base) < 0)
        return JS_ThrowInternalError(ctx, "break failed");
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry http_server_proto_funcs[] = {
    JS_CFUNC_DEF("listen", 2, http_server_listen),
    JS_CFUNC_DEF("on", 2, http_server_on),
    JS_CFUNC_DEF("dispatch", 0, http_server_dispatch),
    JS_CFUNC_DEF("break", 0, http_server_break),
};

static int http_init(JSContext *ctx, JSModuleDef *m) {

    JSValue req_proto, req_obj, res_proto, res_obj, server_proto, server_obj;

    req_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, req_proto, http_req_proto_funcs,
                               countof(http_req_proto_funcs));
    JS_SetClassProto(ctx, http_req_class_id, req_proto);

    req_obj = JS_NewCFunction2(ctx, http_req_ctor, "request", 0,
                               JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, req_obj, req_proto);
    JS_SetModuleExport(ctx, m, "request", req_obj);

    res_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, res_proto, http_res_proto_funcs,
                               countof(http_res_proto_funcs));
    JS_SetClassProto(ctx, http_res_class_id, res_proto);

    res_obj = JS_NewCFunction2(ctx, http_res_ctor, "response", 0,
                               JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, res_obj, res_proto);
    JS_SetModuleExport(ctx, m, "response", res_obj);

    JS_SetModuleExport(ctx, m, "fetch",
                       JS_NewCFunction(ctx, http_fetch, "fetch", 1));

    server_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, server_proto, http_server_proto_funcs,
                               countof(http_server_proto_funcs));
    JS_SetClassProto(ctx, http_server_class_id, server_proto);

    server_obj = JS_NewCFunction2(ctx, http_server_ctor, "server", 0,
                                  JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, server_obj, server_proto);
    JS_SetModuleExport(ctx, m, "server", server_obj);

    return 0;
}

export_fn JSModuleDef *js_init_module(JSContext *ctx, const char *module_name) {
    JSModuleDef *m;
    JSClassID id;

    m = JS_NewCModule(ctx, module_name, http_init);
    if (!m)
        return NULL;

    if (http_req_class_id == 0) {
        int ret;
        id = JS_NewClassID(&http_req_class_id);
        ret = JS_NewClass(JS_GetRuntime(ctx), id, &http_req_class);
        if (ret < 0)
            return NULL;
        http_req_class_id = id;
    }
    if (http_res_class_id == 0) {
        int ret;
        id = JS_NewClassID(&http_res_class_id);
        ret = JS_NewClass(JS_GetRuntime(ctx), id, &http_res_class);
        if (ret < 0)
            return NULL;
        http_res_class_id = id;
    }

    JS_AddModuleExport(ctx, m, "request");
    JS_AddModuleExport(ctx, m, "response");
    JS_AddModuleExport(ctx, m, "fetch");
    JS_AddModuleExport(ctx, m, "server");

    return m;
}