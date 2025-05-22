#include <stdio.h>
#include <linux/uinput.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <ini.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/capability.h>
#include <limits.h>
#include <libgen.h>

volatile sig_atomic_t stop = 0;
int tab_fd;

void quit(int sig) {
    (void)sig;
    printf("\nExiting.\n");
    stop = 1;
    if (tab_fd) ioctl(tab_fd, UI_DEV_DESTROY);
    exit(0);
}

void check_caps(const char *binary_name) {
    char fullpath[PATH_MAX];
    if (!realpath(binary_name, fullpath)) {
        strncpy(fullpath, binary_name, PATH_MAX);
    }

    cap_t caps = cap_get_proc();
    if (!caps) {
        perror("cap_get_proc failed");
        return;
    }

    cap_flag_value_t cap_flag;
    if (cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &cap_flag) == 0 && cap_flag != CAP_SET) {
        fprintf(stderr, "[!] Warning: CAP_SYS_NICE not set. Real-time priority might fail.\n");
        fprintf(stderr, "    Run: sudo setcap cap_sys_nice=eip %s\n", fullpath);
    }

    cap_free(caps);
}

typedef struct {
    int display_width;
    int display_height;
    float x_scale_pct_min;
    float x_scale_pct_max;
    float y_scale_pct_min;
    float y_scale_pct_max;
    int keep_ratio;
    int use_pen;
} configuration;

static int handler(void* user, const char* section, const char* name, const char* value) {
    configuration* cfg = (configuration*)user;
    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("display", "width")) cfg->display_width = atoi(value);
    else if (MATCH("display", "height")) cfg->display_height = atoi(value);
    else if (MATCH("area", "x_scale_pct_min")) cfg->x_scale_pct_min = atof(value);
    else if (MATCH("area", "x_scale_pct_max")) cfg->x_scale_pct_max = atof(value);
    else if (MATCH("area", "y_scale_pct_min")) cfg->y_scale_pct_min = atof(value);
    else if (MATCH("area", "y_scale_pct_max")) cfg->y_scale_pct_max = atof(value);
    else if (MATCH("area", "keep_ratio")) cfg->keep_ratio = atoi(value);
    else if (MATCH("area", "use_pen")) cfg->use_pen = atoi(value);
    else return 0;
    return 1;
}

int init_uinput(int tmin_x, int tmax_x, int tmin_y, int tmax_y) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open /dev/uinput");
        exit(EXIT_FAILURE);
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    struct uinput_user_dev uidev = {0};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Abs-C Virtual Tablet");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0xfedc;
    uidev.id.version = 1;

    uidev.absmin[ABS_X] = tmin_x;
    uidev.absmax[ABS_X] = tmax_x;
    uidev.absmin[ABS_Y] = tmin_y;
    uidev.absmax[ABS_Y] = tmax_y;

    write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);

    return fd;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, quit);
    signal(SIGTERM, quit);

    check_caps(argv[0]);

    configuration config = {
        .display_width = 1366,
        .display_height = 768,
        .x_scale_pct_min = 100,
        .x_scale_pct_max = 100,
        .y_scale_pct_min = 100,
        .y_scale_pct_max = 100,
        .keep_ratio = 1,
        .use_pen = 0
    };

    char config_path[256];
    snprintf(config_path, sizeof(config_path), "%s/.config/abs-c.ini", getenv("HOME"));
    printf("Loading config from %s\n", config_path);
    ini_parse(config_path, handler, &config);

    struct dirent **namelist;
    int ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);
    if (ndevs < 0) {
        perror("scandir");
        exit(EXIT_FAILURE);
    }

    int fd = -1;
    char path[256], name[256];
    bool found = false, is_mac = false;
    const char *targets[] = {"Touchpad", "TouchPad", "Synaptics", "bcm5974"};

    for (int i = 0; i < ndevs && !found; i++) {
        snprintf(path, sizeof(path), "/dev/input/%s", namelist[i]->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strstr(name, "Mouse")) {
            close(fd);
            continue;
        }

        if (config.use_pen && strstr(name, "Stylus")) {
            found = true;
        } else {
            for (int j = 0; j < 4; j++) {
                if (strstr(name, targets[j])) {
                    found = true;
                    if (!config.use_pen && strstr(name, "bcm5974")) is_mac = true;
                    break;
                }
            }
        }
        if (!found) close(fd);
    }

    if (!found) {
        printf("Device not found, exiting...\n");
        exit(EXIT_FAILURE);
    }

    struct input_absinfo absinfo;
    ioctl(fd, EVIOCGABS(ABS_X), &absinfo);
    int tmin_x = absinfo.minimum, tmax_x = absinfo.maximum;
    ioctl(fd, EVIOCGABS(ABS_Y), &absinfo);
    int tmin_y = absinfo.minimum, tmax_y = absinfo.maximum;
    if (is_mac) tmin_y += 1350;

    double sr = (double)config.display_width / config.display_height;
    double tr = (double)tmax_x / tmax_y;
    if (config.keep_ratio) {
        if (sr < tr) {
            int xo = tmax_x * sr / 16;
            tmin_x += xo;
            tmax_x -= xo;
        } else {
            int yo = tmax_y * sr / 16;
            tmin_y += yo;
            tmax_y -= yo;
        }
    }

    float x_scale_min = config.x_scale_pct_min * 0.01;
    float x_scale_max = config.x_scale_pct_max * 0.01;
    float y_scale_min = config.y_scale_pct_min * 0.01;
    float y_scale_max = config.y_scale_pct_max * 0.01;

    int new_tmin_x = tmax_x - (x_scale_min * (tmax_x - tmin_x));
    int new_tmax_x = tmin_x + (x_scale_max * (tmax_x - tmin_x));
    int new_tmin_y = tmax_y - (y_scale_min * (tmax_y - tmin_y));
    int new_tmax_y = tmin_y + (y_scale_max * (tmax_y - tmin_y));

    ioctl(fd, EVIOCGRAB, 1);
    tab_fd = init_uinput(new_tmin_x, new_tmax_x, new_tmin_y, new_tmax_y);

    struct sched_param param = { .sched_priority = 20 };
    sched_setscheduler(0, SCHED_FIFO, &param);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct input_event ev_buf[64];
    int x = 0, y = 0, x_old = -1, y_old = -1;

    printf("Press Ctrl-C to quit\n");

    while (!stop) {
        if (poll(&pfd, 1, -1) <= 0) continue;

        int len = read(fd, ev_buf, sizeof(ev_buf));
        if (len <= 0) continue;
        int nevents = len / sizeof(struct input_event);

        for (int i = 0; i < nevents; i++) {
            struct input_event *ev = &ev_buf[i];
            if (ev->type == EV_ABS) {
                if (ev->code == ABS_X) x = ev->value;
                else if (ev->code == ABS_Y) y = ev->value;
            }
            if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
                if ((x != x_old || y != y_old) && x > 0 && y > 0) {
                    struct input_event out_ev[3] = {
                        { .type = EV_ABS, .code = ABS_X, .value = x },
                        { .type = EV_ABS, .code = ABS_Y, .value = y },
                        { .type = EV_SYN, .code = SYN_REPORT, .value = 0 }
                    };
                    write(tab_fd, out_ev, sizeof(out_ev));
                    x_old = x;
                    y_old = y;
                }
            }
        }
    }

    return 0;
}
