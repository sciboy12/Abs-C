#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
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

static _Atomic bool running = false;
static bool thread_started = false;
static bool last_abs_state = true;

/* Track previous menu state and whether we're currently treating the session as a replay.
 * Protected by state_mutex. */
static int last_menu_state = -1;
static bool replay_session = false;

/* callback the caller may register to be notified on absolute-state changes */
static void (*state_change_cb)(bool) = NULL;

// ---------------- JSON LOGIC ----------------
static bool should_enable_absolute(const cJSON *root) {
    if (!root) return false;

    const cJSON *menu = cJSON_GetObjectItem(root, "menu");
    if (!cJSON_IsObject(menu)) return false;

    const cJSON *state = cJSON_GetObjectItem(menu, "state");
    const cJSON *mode  = cJSON_GetObjectItem(menu, "gameMode");
    const cJSON *mods  = cJSON_GetObjectItem(menu, "mods");

    if (!cJSON_IsNumber(state) || !cJSON_IsNumber(mode))
        return false;

    int current_state = state->valueint;
    int current_mode  = mode->valueint;


    bool autoplay_mod_active = false;

    if (cJSON_IsObject(mods)) {
        const cJSON *mods_str = cJSON_GetObjectItem(mods, "str");
        if (cJSON_IsString(mods_str) && mods_str->valuestring) {
            const char *s = mods_str->valuestring;

            /* Defensive parsing:
            Accept both concatenated pairs ("HDDTAP")
            and comma-separated ("HD,DT,AP") formats. */

            size_t len = strlen(s);

            for (size_t i = 0; i < len; ) {

                /* Skip commas or whitespace */
                if (s[i] == ',' || s[i] == ' ') {
                    i++;
                    continue;
                }

                /* Ensure at least two chars remain */
                if (i + 1 >= len)
                    break;

                char a = s[i];
                char b = s[i + 1];

                if ((a == 'A' && b == 'T') ||
                    ((a == 'C' && b == 'N') ||
                    (a == 'A' && b == 'P'))) {
                    autoplay_mod_active = true;
                    break;
                }

                /* Advance by 2 if contiguous, else 1 if unknown */
                if (i + 2 < len && s[i + 2] != ',' && s[i + 2] != ' ')
                    i += 2;
                else
                    i += 3;  /* skip comma as well */
            }
        }
    }

    pthread_mutex_lock(&state_mutex);

    int prev_state = last_menu_state;

    if (prev_state == 7 && current_state == 2) {
        if (!replay_session) {
            replay_session = true;
        }
    }

    if (current_state != 2 && replay_session) {
        replay_session = false;
    }

    last_menu_state = current_state;

    bool enable = (current_state == 2 &&
                   current_mode == 0 &&
                   !replay_session &&
                   !autoplay_mod_active);

    pthread_mutex_unlock(&state_mutex);

    return enable;
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
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
        return NULL;
    }

    if (curl_easy_setopt(easy, CURLOPT_URL, TOSU_URL) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, 500L) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http") != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http") != CURLE_OK ||
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curl_write_cb) != CURLE_OK) {
        fprintf(stderr, "[tosu] curl_easy_setopt failed\n");
        curl_easy_cleanup(easy);
        curl_multi_cleanup(multi);
        return NULL;
    }

    while (atomic_load_explicit(&running, memory_order_acquire)) {
        struct curl_buf buf = {0};

        if (curl_easy_setopt(easy, CURLOPT_WRITEDATA, &buf) != CURLE_OK) {
            fprintf(stderr, "[tosu] failed to set WRITEDATA\n");
            break;
        }
        CURLMcode mrc = curl_multi_add_handle(multi, easy);
        if (mrc != CURLM_OK) {
            fprintf(stderr, "[tosu] curl_multi_add_handle failed: %s\n", curl_multi_strerror(mrc));
            break;
        }

        int still_running = 0;
        mrc = curl_multi_perform(multi, &still_running);
        if (mrc != CURLM_OK) {
            fprintf(stderr, "[tosu] curl_multi_perform failed: %s\n", curl_multi_strerror(mrc));
            curl_multi_remove_handle(multi, easy);
            break;
        }

        while (still_running && atomic_load_explicit(&running, memory_order_acquire)) {
            mrc = curl_multi_poll(multi, NULL, 0, 100, NULL);
            if (mrc != CURLM_OK) {
                fprintf(stderr, "[tosu] curl_multi_poll failed: %s\n", curl_multi_strerror(mrc));
                break;
            }
            mrc = curl_multi_perform(multi, &still_running);
            if (mrc != CURLM_OK) {
                fprintf(stderr, "[tosu] curl_multi_perform failed: %s\n", curl_multi_strerror(mrc));
                break;
            }
        }

        CURLcode result = CURLE_FAILED_INIT;
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(multi, &msgs_left))) {
            if (msg->msg == CURLMSG_DONE && msg->easy_handle == easy) {
                result = msg->data.result;
                break;
            }
        }

        mrc = curl_multi_remove_handle(multi, easy);
        if (mrc != CURLM_OK) {
            fprintf(stderr, "[tosu] curl_multi_remove_handle failed: %s\n", curl_multi_strerror(mrc));
            break;
        }

        if (result == CURLE_OK && buf.data) {
            cJSON *root = cJSON_Parse(buf.data);
            if (root) {
                bool current = should_enable_absolute(root);

                /* Update shared state and detect transitions. */
                pthread_mutex_lock(&state_mutex);
                bool prev = last_abs_state;
                if (prev != current) {
                    last_abs_state = current;
                }
                /* copy callback pointer under lock so it won't change under us */
                void (*cb_copy)(bool) = state_change_cb;
                pthread_mutex_unlock(&state_mutex);

                if (prev != current) {
                    /* call the callback (outside lock) if registered */
                    if (cb_copy) cb_copy(current);
                }

                cJSON_Delete(root);
            }
        } else if (result != CURLE_OK) {
            fprintf(stderr, "[tosu] request failed: %s; keeping last absolute state\n",
                    curl_easy_strerror(result));
        }

        free(buf.data);
            struct timespec ts = {
                .tv_sec = POLL_INTERVAL_MS / 1000,
                .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L
            };
            if (nanosleep(&ts, NULL) < 0 && errno != EINTR) {
                perror("[tosu] nanosleep");
                break;
            }

    }
    atomic_store_explicit(&running, false, memory_order_release);

    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    return NULL;
}

/* Register a callback that will be called whenever the absolute state changes.
 * The callback is called from the poll thread (not the main thread). */
void tosu_set_state_change_callback(void (*cb)(bool)) {
    pthread_mutex_lock(&state_mutex);
    state_change_cb = cb;
    pthread_mutex_unlock(&state_mutex);
}

// ---------------- PUBLIC API ----------------
void tosu_init(void) {
    pthread_mutex_lock(&state_mutex);
    bool already_running = atomic_load_explicit(&running, memory_order_acquire);
    if (already_running || thread_started) {
        pthread_mutex_unlock(&state_mutex);
        return;
    }
    last_abs_state = true;
    last_menu_state = -1;
    replay_session = false;
    pthread_mutex_unlock(&state_mutex);

    CURLcode gc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (gc != CURLE_OK) {
        fprintf(stderr, "[tosu] curl_global_init failed: %s\n", curl_easy_strerror(gc));
        return;
    }

    atomic_store_explicit(&running, true, memory_order_release);
    int rc = pthread_create(&poll_thread, NULL, poll_thread_func, NULL);
    if (rc != 0) {
        fprintf(stderr, "[tosu] pthread_create failed: %d\n", rc);
        atomic_store_explicit(&running, false, memory_order_release);
        curl_global_cleanup();
        return;
    }

    pthread_mutex_lock(&state_mutex);
    thread_started = true;
    pthread_mutex_unlock(&state_mutex);
}

void tosu_shutdown(void) {
    pthread_mutex_lock(&state_mutex);
    bool join_needed = thread_started;
    thread_started = false;
    pthread_mutex_unlock(&state_mutex);

    if (!join_needed) return;

    atomic_store_explicit(&running, false, memory_order_release);
    int rc = pthread_join(poll_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "[tosu] pthread_join failed: %d\n", rc);
    }

    curl_global_cleanup();
}

bool tosu_get_absolute_state(void) {
    pthread_mutex_lock(&state_mutex);
    bool state = last_abs_state;
    pthread_mutex_unlock(&state_mutex);
    return state;
}
