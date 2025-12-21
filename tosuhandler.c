#define _POSIX_C_SOURCE 200809L

#include "tosuhandler.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <cjson/cJSON.h>
#include <libwebsockets.h>

static pthread_mutex_t tosu_mutex = PTHREAD_MUTEX_INITIALIZER;
static char *tosu_json = NULL;
static size_t tosu_json_len = 0;
static bool tosu_json_dirty = false;
static bool running_ws_thread = false;
static bool last_abs_state = false;

static struct lws_context *ws_context = NULL;
static struct lws *ws_client = NULL;

static const char *tosu_url = "127.0.0.1";
static int tosu_port = 24050;
static const char *tosu_path = "/websocket/v2";

// ---- JSON parsing helper ----
static bool should_enable_absolute(const cJSON *root) {
    if (!root) return false;

    const cJSON *state = cJSON_GetObjectItem(root, "state");
    if (!state) return false;

    const cJSON *state_name = cJSON_GetObjectItem(state, "name");
    if (!cJSON_IsString(state_name)) return false;
    if (strcmp(state_name->valuestring, "play") != 0) return false;

    const cJSON *beatmap = cJSON_GetObjectItem(root, "beatmap");
    if (!beatmap) return false;

    const cJSON *mode = cJSON_GetObjectItem(beatmap, "mode");
    if (!mode) return false;

    const cJSON *mode_number = cJSON_GetObjectItem(mode, "number");
    if (!cJSON_IsNumber(mode_number)) return false;

    return (mode_number->valueint == 0);
}

// ---- LWS callbacks ----
static int ws_cb(struct lws *wsi, enum lws_callback_reasons reason,
                 void *user, void *in, size_t len) {

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("[tosu] WS connected\n");
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            // copy incoming JSON to shared buffer
            char *msg = malloc(len + 1);
            if (!msg) break;
            memcpy(msg, in, len);
            msg[len] = '\0';

            pthread_mutex_lock(&tosu_mutex);
            free(tosu_json);
            tosu_json = msg;
            tosu_json_len = len;
            tosu_json_dirty = true;
            pthread_mutex_unlock(&tosu_mutex);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            printf("[tosu] WS connection error\n");
            break;

        case LWS_CALLBACK_CLOSED:
            printf("[tosu] WS closed\n");
            break;

        default:
            break;
    }

    return 0;
}

static struct lws_protocols protocols[] = {
    {"tosu.v2", ws_cb, 0, 65536},
    {NULL, NULL, 0, 0}
};

// ---- WebSocket thread ----
static void *ws_thread_func(void *arg) {
    (void)arg;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    ws_context = lws_create_context(&info);
    if (!ws_context) {
        fprintf(stderr, "[tosu] failed to create LWS context\n");
        return NULL;
    }

    struct lws_client_connect_info ccinfo = {0};
    ccinfo.context = ws_context;
    ccinfo.address = tosu_url;
    ccinfo.port = tosu_port;
    ccinfo.path = tosu_path;
    ccinfo.host = tosu_url;
    ccinfo.origin = tosu_url;
    ccinfo.protocol = protocols[0].name; // important: must match v2
    ccinfo.ssl_connection = 0;

    ws_client = lws_client_connect_via_info(&ccinfo);
    if (!ws_client) {
        fprintf(stderr, "[tosu] failed to connect WS client\n");
    }

    while (running_ws_thread && ws_context) {
        lws_service(ws_context, 100); // poll 100ms
    }

    if (ws_context) {
        lws_context_destroy(ws_context);
        ws_context = NULL;
    }

    return NULL;
}

static pthread_t ws_thread;

// ---- Public API ----
void tosu_init(void) {
    if (running_ws_thread) return;
    running_ws_thread = true;

    pthread_mutex_lock(&tosu_mutex);
    free(tosu_json);
    tosu_json = NULL;
    tosu_json_len = 0;
    tosu_json_dirty = false;
    last_abs_state = false;
    pthread_mutex_unlock(&tosu_mutex);

    if (pthread_create(&ws_thread, NULL, ws_thread_func, NULL) != 0) {
        perror("[tosu] ws thread creation failed");
        running_ws_thread = false;
    }
}

void tosu_shutdown(void) {
    if (!running_ws_thread) return;

    running_ws_thread = false;
    lws_cancel_service(ws_context);
    pthread_join(ws_thread, NULL);

    pthread_mutex_lock(&tosu_mutex);
    free(tosu_json);
    tosu_json = NULL;
    tosu_json_len = 0;
    tosu_json_dirty = false;
    pthread_mutex_unlock(&tosu_mutex);
}

bool tosu_get_absolute_state(void) {
    char *local_json = NULL;

    pthread_mutex_lock(&tosu_mutex);
    if (tosu_json_dirty && tosu_json) {
        local_json = strdup(tosu_json);
        tosu_json_dirty = false;
    }
    pthread_mutex_unlock(&tosu_mutex);

    if (!local_json) return last_abs_state;

    cJSON *root = cJSON_Parse(local_json);
    free(local_json);
    if (!root) return last_abs_state;

    bool current = should_enable_absolute(root);
    cJSON_Delete(root);

    last_abs_state = current;
    return current;
}
