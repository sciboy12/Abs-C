#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "tosuhandler.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ini.h>
#include <limits.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <math.h>


volatile sig_atomic_t stop = 0;
int tab_fd = -1;
int fd = -1;
bool have_x = false;
bool have_y = false;

static void quit(int sig)
{
    (void)sig;
    stop = 1;
}

static inline bool emit_events(const struct input_event *ev, int n)
{
    size_t total = (size_t)n * sizeof(struct input_event);
    const unsigned char *p = (const unsigned char *)ev;
    size_t done = 0;

    while (done < total)
    {
        ssize_t w = write(tab_fd, p + done, total - done);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            perror("write input_event batch");
            return false;
        }
        if (w == 0)
        {
            errno = EIO;
            perror("write input_event batch");
            return false;
        }
        done += (size_t)w;
    }

    return true;
}

static inline bool emit_frame(bool tool_pen,
                            bool touch,
                            int x,
                            int y,
                            int pressure,
                            int distance)
{
    struct input_event ev[8];
    int n = 0;

    ev[n++] = (struct input_event){
        .type = EV_KEY,
        .code = BTN_TOOL_PEN,
        .value = tool_pen};

    ev[n++] = (struct input_event){
        .type = EV_KEY,
        .code = BTN_TOUCH,
        .value = touch};

    ev[n++] = (struct input_event){
        .type = EV_ABS,
        .code = ABS_X,
        .value = x};

    ev[n++] = (struct input_event){
        .type = EV_ABS,
        .code = ABS_Y,
        .value = y};

    ev[n++] = (struct input_event){
        .type = EV_ABS,
        .code = ABS_PRESSURE,
        .value = pressure};

    ev[n++] = (struct input_event){
        .type = EV_ABS,
        .code = ABS_DISTANCE,
        .value = distance};

    ev[n++] = (struct input_event){
        .type = EV_SYN,
        .code = SYN_REPORT,
        .value = 0};

    return emit_events(ev, n);
}

static inline void set_grab(int fd, bool *grabbed, bool want)
{
    if (*grabbed == want)
        return;
    if (ioctl(fd, EVIOCGRAB, want) == 0)
    {
        *grabbed = want;
    }
    else
    {
        perror("ioctl EVIOCGRAB");
    }
}

// Internal helper: returns malloc'd home directory for the real user.
static char *get_real_user_home(void)
{
    struct passwd *pw = NULL;
    const char *sudo_user = getenv("SUDO_USER");

    // If sudo was used, trust the env var — it's the only sane option.
    if (sudo_user && sudo_user[0] != '\0')
    {
        pw = getpwnam(sudo_user);
        if (pw && pw->pw_dir && pw->pw_dir[0] != '\0')
            return strdup(pw->pw_dir);
    }

    // Fallback: real UID (normal execution)
    uid_t ruid = getuid();
    pw = getpwuid(ruid);
    if (pw && pw->pw_dir && pw->pw_dir[0] != '\0')
        return strdup(pw->pw_dir);

    return NULL;
}

// Public function:
// Returns a malloc'd string with the full ~/.config/abs-c.ini path.
// Caller must free() it. Returns NULL on failure.
char *get_abs_c_config_path(void)
{
    char *home = get_real_user_home();
    if (!home)
        return NULL;

    const char *rel = "/.config/abs-c.ini";
    size_t len = strlen(home) + strlen(rel) + 1;

    char *path = malloc(len);
    if (!path)
    {
        free(home);
        return NULL;
    }

    snprintf(path, len, "%s%s", home, rel);
    free(home);
    return path;
}

void check_caps(const char *binary_name)
{
    char fullpath[PATH_MAX];
    if (!realpath(binary_name, fullpath))
        strncpy(fullpath, binary_name, PATH_MAX);

    cap_t caps = cap_get_proc();
    if (!caps)
        return;

    cap_flag_value_t cap_flag;
    if (cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &cap_flag) == 0 &&
        cap_flag != CAP_SET)
    {
        fprintf(stderr, "[!] Warning: CAP_SYS_NICE not set. Real-time priority "
                        "might fail.\n");
        fprintf(stderr, "    Run: sudo setcap cap_sys_nice=eip %s\n", fullpath);
    }
    cap_free(caps);
}

typedef struct
{
    int display_width;
    int display_height;
    float x_offset_pct;
    float x_scale_pct;
    float y_offset_pct;
    float y_scale_pct;
    int keep_ratio;
    bool enable_tosu;
} configuration;

static int handler(void *user, const char *section, const char *name,
                const char *value)
{
    configuration *cfg = (configuration *)user;
#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("display", "width"))
        cfg->display_width = atoi(value);
    else if (MATCH("display", "height"))
        cfg->display_height = atoi(value);
    else if (MATCH("area", "x_offset_pct"))
        cfg->x_offset_pct = atof(value);
    else if (MATCH("area", "x_scale_pct"))
        cfg->x_scale_pct = atof(value);
    else if (MATCH("area", "y_offset_pct"))
        cfg->y_offset_pct = atof(value);
    else if (MATCH("area", "y_scale_pct"))
        cfg->y_scale_pct = atof(value);
    else if (MATCH("area", "keep_ratio"))
        cfg->keep_ratio = atoi(value);
    else if (MATCH("input", "enable_tosu"))
        cfg->enable_tosu = atoi(value);
    else
        return 0;
    return 1;
}

int init_uinput(int tmin_x, int tmax_x, int tmin_y, int tmax_y)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
    {
        perror("open /dev/uinput");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0)
    {
        perror("UI_SET_EVBIT");
        close(fd);
        return -1;
    }

    /* Pen buttons */
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS2);

    /* Absolute axes */
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE);

    /* Required-ish for modern tablet recognition */
    ioctl(fd, UI_SET_ABSBIT, ABS_DISTANCE);

    /* Optional but highly recommended */

    struct uinput_abs_setup abs;

    /* ABS_X */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_X;
    abs.absinfo.minimum = tmin_x;
    abs.absinfo.maximum = tmax_x;
    abs.absinfo.resolution = 1000;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        perror("UI_ABS_SETUP ABS_X");

    /* ABS_Y */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_Y;
    abs.absinfo.minimum = tmin_y;
    abs.absinfo.maximum = tmax_y;
    abs.absinfo.resolution = 1000;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        perror("UI_ABS_SETUP ABS_Y");

    /* ABS_PRESSURE */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_PRESSURE;
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = 1024;
    abs.absinfo.resolution = 1;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        perror("UI_ABS_SETUP PRESSURE");

    /* ABS_DISTANCE */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_DISTANCE;
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = 1;
    abs.absinfo.resolution = 1;

    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        perror("UI_ABS_SETUP DISTANCE");

    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_user_dev uidev = {0};

    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Abs-C Virtual Tablet");

    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x1234;
    uidev.id.product = 0xfedc;
    uidev.id.version = 1;

    /* ABS X */
    uidev.absmin[ABS_X] = tmin_x;
    uidev.absmax[ABS_X] = tmax_x;
    uidev.absfuzz[ABS_X] = 0;
    uidev.absflat[ABS_X] = 0;

    /* ABS Y */
    uidev.absmin[ABS_Y] = tmin_y;
    uidev.absmax[ABS_Y] = tmax_y;
    uidev.absfuzz[ABS_Y] = 0;
    uidev.absflat[ABS_Y] = 0;

    /* PRESSURE */
    uidev.absmin[ABS_PRESSURE] = 0;
    uidev.absmax[ABS_PRESSURE] = 1024;
    uidev.absfuzz[ABS_PRESSURE] = 0;
    uidev.absflat[ABS_PRESSURE] = 0;

    /* DISTANCE */
    uidev.absmin[ABS_DISTANCE] = 0;
    uidev.absmax[ABS_DISTANCE] = 1;

    if (write(fd, &uidev, sizeof(uidev)) < 0)
    {
        perror("write uinput_user_dev");
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0)
    {
        perror("UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    /* Allow device enumeration to settle */
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 100000000L
    };

    nanosleep(&ts, NULL);

    return fd;
}

static inline int test_bit(int bit, const unsigned long *array)
{
    return (array[bit / (8 * sizeof(unsigned long))] >>
            (bit % (8 * sizeof(unsigned long)))) &
        1;
}


void print_help(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -h, --help            Show this help message\n");
    printf("  -l, --list            List input devices with EV_ABS support\n");
    printf(
        "  -d, --device <arg>    Specify device by path or name substring\n");
}

void list_devices()
{
    struct dirent **namelist;
    int ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);
    if (ndevs < 0)
    {
        perror("scandir");
        return;
    }

    char path[256], name[256];
    struct stat st;

    for (int i = 0; i < ndevs; i++)
    {
        if (strcmp(namelist[i]->d_name, ".") == 0 ||
            strcmp(namelist[i]->d_name, "..") == 0)
        {
            free(namelist[i]);
            continue;
        }

        snprintf(path, sizeof(path), "/dev/input/%s", namelist[i]->d_name);

        if (stat(path, &st) < 0)
        {
            free(namelist[i]);
            continue;
        }
        if (!S_ISCHR(st.st_mode))
        {
            free(namelist[i]);
            continue;
        } // Only character devices

        int devfd = open(path, O_RDONLY);
        if (devfd < 0)
        {
            free(namelist[i]);
            continue;
        }

        ioctl(devfd, EVIOCGNAME(sizeof(name)), name);

        unsigned long evbits[(EV_MAX + (sizeof(unsigned long) * 8) - 1) /
                            (sizeof(unsigned long) * 8)] = {0};
        ioctl(devfd, EVIOCGBIT(0, sizeof(evbits)), evbits);

        if (test_bit(EV_ABS, evbits))
        {
            unsigned long absbits[(ABS_MAX + (sizeof(unsigned long) * 8) - 1) /
                                (sizeof(unsigned long) * 8)] = {0};

            ioctl(devfd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

            bool has_x = test_bit(ABS_X, absbits);
            bool has_y = test_bit(ABS_Y, absbits);

            if (has_x && has_y)
            {
                printf("%s: %s\n", path, name);
            }
        }
        close(devfd);
        free(namelist[i]);
    }

    free(namelist);
}

int main(int argc, char *argv[])
{
    int exit_code = EXIT_FAILURE;
    bool grabbed = false;
    bool tosu_started = false;

    struct sigaction sa = {0};
    sa.sa_handler = quit;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
    {
        perror("sigaction");
        goto cleanup;
    }

    check_caps(argv[0]);

    configuration config = {.display_width = 1366,
                            .display_height = 768,
                            .x_offset_pct = 0,
                            .x_scale_pct = 100,
                            .y_offset_pct = 0,
                            .y_scale_pct = 100,
                            .keep_ratio = 1,
                            .enable_tosu = 0};

    char *config_path = get_abs_c_config_path();
    if (!config_path)
    {
        fprintf(
            stderr,
            "Couldn't find the config file path. Using default settings.\n");
    }
    else
    {
        printf("Loading config from %s\n", config_path);
        ini_parse(config_path, handler, &config);
        free(config_path);
    }

    const char *dev_override = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            print_help(argv[0]);
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        }
        else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list"))
        {
            list_devices();
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        }
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) &&
                i + 1 < argc)
            dev_override = argv[++i];
    }

    struct dirent **namelist;
    int ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);
    if (ndevs < 0)
    {
        perror("scandir");
        goto cleanup;
    }

    char path[256], name[256];
    struct stat st;
    bool found = false, usable = false;

    for (int i = 0; i < ndevs && !found; i++)
    {
        if (strcmp(namelist[i]->d_name, ".") == 0 ||
            strcmp(namelist[i]->d_name, "..") == 0)
        {
            continue;
        }

        if (strncmp(namelist[i]->d_name, "event", 5) != 0)
        {
            continue;
        }

        snprintf(path, sizeof(path), "/dev/input/%s", namelist[i]->d_name);

        if (stat(path, &st) < 0)
        {
            continue;
        }
        if (!S_ISCHR(st.st_mode))
        {
            continue;
        }

        int devfd = open(path, O_RDONLY);
        if (devfd < 0)
            continue;

        if (ioctl(devfd, EVIOCGNAME(sizeof(name)), name) < 0)
        {
            close(devfd);
            continue;
        }
        name[sizeof(name) - 1] = '\0';

        if (dev_override)
        {
            if (dev_override[0] == '/' && strcmp(path, dev_override) != 0)
            {
                close(devfd);
                continue;
            }
            else if (dev_override[0] != '/' && !strstr(name, dev_override))
            {
                close(devfd);
                continue;
            }
        }

        unsigned long evbits[(EV_MAX + (sizeof(unsigned long) * 8) - 1) /
                            (sizeof(unsigned long) * 8)] = {0};
        if (ioctl(devfd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
        {
            perror("ioctl EVIOCGBIT");
            close(devfd);
            continue;
        }

        if (test_bit(EV_ABS, evbits))
        {
            unsigned long absbits[(ABS_MAX + (sizeof(unsigned long) * 8) - 1) /
                                (sizeof(unsigned long) * 8)] = {0};

            if (ioctl(devfd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) < 0)
            {
                perror("ioctl EVIOCGBIT(EV_ABS)");
                close(devfd);
                continue;
            }

            bool has_x = test_bit(ABS_X, absbits);
            bool has_y = test_bit(ABS_Y, absbits);

            if (has_x && has_y)
            {
                fd = devfd;
                found = usable = true;
                printf("Using device %s (%s)\n", path, name);
                devfd = -1;
            }
        }

        else if (dev_override)
        {
            found = true;
            usable = false;
        }

        if (devfd >= 0)
            close(devfd);
    }

    for (int i = 0; i < ndevs; i++)
        free(namelist[i]);
    free(namelist);

    if (!found)
    {
        fprintf(stderr,
                dev_override ? "No device matching '%s'\n"
                            : "No suitable input device found.\n",
                dev_override);
        goto cleanup;
    }
    if (!usable)
    {
        fprintf(stderr, "Device '%s' is not usable (requires EV_ABS support)\n",
                dev_override);
        goto cleanup;
    }

    struct input_absinfo absinfo;
    if (ioctl(fd, EVIOCGABS(ABS_X), &absinfo) < 0)
    {
        perror("ioctl EVIOCGABS(ABS_X)");
        goto cleanup;
    }
    int tmin_x = absinfo.minimum, tmax_x = absinfo.maximum;
    if (ioctl(fd, EVIOCGABS(ABS_Y), &absinfo) < 0)
    {
        perror("ioctl EVIOCGABS(ABS_Y)");
        goto cleanup;
    }
    int tmin_y = absinfo.minimum, tmax_y = absinfo.maximum;

    double sr = (double)config.display_width / config.display_height;
    float x_center = (tmin_x + tmax_x) / 2.0f +
                    config.x_offset_pct * 0.01f * (tmax_x - tmin_x) / 2.0f;
    float y_center = (tmin_y + tmax_y) / 2.0f +
                    config.y_offset_pct * 0.01f * (tmax_y - tmin_y) / 2.0f;

    float desired_width = (tmax_x - tmin_x) * config.x_scale_pct * 0.01f;
    float desired_height = (tmax_y - tmin_y) * config.y_scale_pct * 0.01f;
    float desired_ratio = desired_width / desired_height;

    if (config.keep_ratio)
    {
        if (desired_ratio > sr)
            desired_height = desired_width / sr;
        else
            desired_width = desired_height * sr;
    }

    float x_half_range = desired_width / 2.0f;
    float y_half_range = desired_height / 2.0f;

    int new_tmin_x = (int)(x_center - x_half_range);
    int new_tmax_x = (int)(x_center + x_half_range);
    int new_tmin_y = (int)(y_center - y_half_range);
    int new_tmax_y = (int)(y_center + y_half_range);

    tab_fd = init_uinput(new_tmin_x, new_tmax_x, new_tmin_y, new_tmax_y);
    if (tab_fd < 0)
        goto cleanup;

    struct sched_param param = {.sched_priority = 20};
    if (sched_setscheduler(0, SCHED_FIFO, &param) < 0)
    {
        perror("sched_setscheduler");
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
    {
        perror("mlockall");
    }

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    struct input_event ev_buf[64];

    int x = 0, y = 0;
    int pending_x = 0;
    int pending_y = 0;

    bool frame_has_x = false;
    bool frame_has_y = false;
    int pressure = 0;
    int distance = 1;
    bool active = true;
    bool last_active = active;
    bool pen_down = false;
    bool pen_hover = false;
    bool dirty = false;
    bool prev_active = active;

    if (config.enable_tosu)
    {
        tosu_init();
        tosu_started = true;
    }

    /* Give Tosu IPC/shared state a moment to initialize */
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 50000000L /* 50ms */
    };

    nanosleep(&ts, NULL);

    /* Initial sync */
    if (config.enable_tosu)
    {
        active = tosu_get_absolute_state();
        last_active = active;
        prev_active = active;
    }

    printf("Press Ctrl-C to quit\n");

    while (!stop)
    {
        if (config.enable_tosu)
        {
            active = tosu_get_absolute_state();
            if (active && !prev_active)
            {
                dirty = true;
            }

            prev_active = active;
        }


        if (active != last_active)
        {
            set_grab(fd, &grabbed, active);
            last_active = active;
        }

        int poll_ret = poll(&pfd, 1, -1);

        if (poll_ret < 0)
        {
            if (errno == EINTR)
                continue;

            perror("poll");
            break;
        }

        if (poll_ret == 0)
        {
            continue;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "poll error on input device\n");
            break;
        }

        ssize_t rd = read(fd, ev_buf, sizeof(ev_buf));
        if (rd < 0)
        {
            if (errno == EINTR && stop)
                break;
            perror("read input_event");
            continue;
        }

        if (rd % sizeof(struct input_event) != 0)
        {
            fprintf(stderr, "partial input_event read\n");
            continue;
        }

        int nevents = rd / sizeof(struct input_event);

        for (int i = 0; i < nevents; i++)
        {
            struct input_event *ev = &ev_buf[i];


            if (ev->type == EV_KEY)
            {
                /*
                    BTN_TOUCH from source device:
                    means hover/proximity exists
                */
                if (ev->code == BTN_TOUCH)
                {
                    if (ev->value && !pen_hover)
                    {
                        pen_hover = true;
                        distance = pen_down ? 0 : 1;
                        pressure = pen_down ? 1024 : 0;
                        dirty = true;
                    }
                    else if (!ev->value && pen_hover)
                    {
                        pen_hover = false;
                        pen_down = false;
                        pressure = 0;
                        distance = 1;
                        dirty = true;
                    }
                }

                /*
                    BTN_LEFT from source device:
                    means actual pen contact
                */
                else if (ev->code == BTN_LEFT)
                {
                    if (ev->value && !pen_down)
                    {
                        /* PEN DOWN */
                        pen_hover = true;
                        pen_down = true;
                        pressure = 1024;
                        distance = 0;
                        dirty = true;
                    }
                    else if (!ev->value && pen_down)
                    {
                        /* PEN UP */
                        pen_down = false;
                        pressure = 0;
                        distance = 1;
                        dirty = true;
                    }
                }
            }
            else if (ev->type == EV_ABS)
            {
                switch (ev->code)
                {
                case ABS_X:
                    pending_x = ev->value;
                    frame_has_x = true;
                    have_x = true;
                    dirty = true;
                    break;

                case ABS_Y:
                    pending_y = ev->value;
                    frame_has_y = true;
                    have_y = true;
                    dirty = true;
                    break;

                default:
                    break;
                }
            }
            else if (ev->type == EV_SYN && ev->code == SYN_REPORT)
            {
                if (ev->code == SYN_DROPPED)
                {
                    fprintf(stderr, "SYN_DROPPED received\n");

                    frame_has_x = false;
                    frame_has_y = false;
                    dirty = false;

                    continue;
                }
                if (dirty && have_x && have_y)
                {
                    if (frame_has_x)
                        x = pending_x;

                    if (frame_has_y)
                        y = pending_y;

                    bool tool_pen = pen_hover || pen_down;
                    bool touch = pen_down;

                    /*
                        IMPORTANT:
                        Always process and commit internal state,
                        even while inactive.

                        Only suppress OUTPUT emission.
                    */
                    if (active)
                    {
                        if (!emit_frame(tool_pen,
                                        touch,
                                        x,
                                        y,
                                        pressure,
                                        distance))
                        {
                            goto cleanup;
                        }
                    }

                    dirty = false;
                    frame_has_x = false;
                    frame_has_y = false;
                }
            }
        }
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    if (grabbed && fd >= 0 && ioctl(fd, EVIOCGRAB, 0) < 0)
    {
        perror("ioctl EVIOCGRAB release");
    }
    if (tosu_started)
    {
        tosu_shutdown();
    }
    if (tab_fd >= 0)
    {
        if (ioctl(tab_fd, UI_DEV_DESTROY) < 0)
        {
            perror("ioctl UI_DEV_DESTROY");
        }
        close(tab_fd);
        tab_fd = -1;
    }
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
    fprintf(stderr, "Exiting with status %d\n", exit_code);
    return exit_code;
}
