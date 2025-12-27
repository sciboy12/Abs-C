#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

// ---------------- CONFIG ----------------
#define TOSU_URL "http://localhost:24050/json"
#define POLL_INTERVAL_MS 500
// ----------------------------------------

struct curl_buf {
    char *data;
    size_t len;
};

static pthread_t poll_thread;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool running = false;
static bool last_abs_state = false;

// ---------------- JSON LOGIC ----------------
static bool should_enable_absolute(const cJSON *root) {
    if (!root) return false;

    const cJSON *menu = cJSON_GetObjectItem(root, "menu");
    if (!cJSON_IsObject(menu)) return false;

    const cJSON *state = cJSON_GetObjectItem(menu, "state");
    const cJSON *mode  = cJSON_GetObjectItem(menu, "gameMode");

    if (!cJSON_IsNumber(state) || !cJSON_IsNumber(mode))
        return false;

    return state->valueint == 2 && mode->valueint == 0;
}

// ---------------- CURL CALLBACK ----------------
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct curl_buf *buf = userdata;

    char *newp = realloc(buf->data, buf->len + total + 1);
    if (!newp) return 0;

    buf->data = newp;
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';

    return total;
}

// ---------------- POLL THREAD ----------------
static void *poll_thread_func(void *arg) {
    (void)arg;

    CURLM *multi = curl_multi_init();
    CURL *easy = curl_easy_init();

    if (!multi || !easy) {
        fprintf(stderr, "[tosu] curl init failed\n");
        return NULL;
    }

    curl_easy_setopt(easy, CURLOPT_URL, TOSU_URL);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 500);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb);

    while (running) {
        struct curl_buf buf = {0};

        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buf);
        curl_multi_add_handle(multi, easy);

        int still_running = 0;
        curl_multi_perform(multi, &still_running);

        while (still_running && running) {
            curl_multi_poll(multi, NULL, 0, 100, NULL);
            curl_multi_perform(multi, &still_running);
        }

        curl_multi_remove_handle(multi, easy);

        if (buf.data) {
            cJSON *root = cJSON_Parse(buf.data);
            if (root) {
                bool current = should_enable_absolute(root);

                pthread_mutex_lock(&state_mutex);
                last_abs_state = current;
                pthread_mutex_unlock(&state_mutex);

                cJSON_Delete(root);
            }
            free(buf.data);
        }
            struct timespec ts = {
                .tv_sec = POLL_INTERVAL_MS / 1000,
                .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L
            };
            nanosleep(&ts, NULL);

    }

    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    return NULL;
}

// ---------------- PUBLIC API ----------------
void tosu_init(void) {
    if (running) return;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    pthread_mutex_lock(&state_mutex);
    last_abs_state = false;
    pthread_mutex_unlock(&state_mutex);

    running = true;
    pthread_create(&poll_thread, NULL, poll_thread_func, NULL);
}

void tosu_shutdown(void) {
    if (!running) return;

    running = false;
    pthread_join(poll_thread, NULL);

    curl_global_cleanup();
}

bool tosu_get_absolute_state(void) {
    pthread_mutex_lock(&state_mutex);
    bool state = last_abs_state;
    pthread_mutex_unlock(&state_mutex);
    return state;
}
