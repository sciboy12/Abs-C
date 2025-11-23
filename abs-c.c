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
#include <sys/stat.h>
#include <pwd.h>

volatile sig_atomic_t stop = 0;
int tab_fd;
int fd = -1;

void quit(int sig) {
    (void)sig;
    printf("\nExiting.\n");
    stop = 1;
    if (fd > 0) ioctl(fd, EVIOCGRAB, 0);
    if (tab_fd) ioctl(tab_fd, UI_DEV_DESTROY);
    exit(0);
}

// Internal helper: returns malloc'd home directory for the real user.
static char *get_real_user_home(void) {
    struct passwd *pw = NULL;
    const char *sudo_user = getenv("SUDO_USER");

    // If sudo was used, trust the env var — it's the only sane option.
    if (sudo_user && sudo_user[0] != '\0') {
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
char *get_abs_c_config_path(void) {
    char *home = get_real_user_home();
    if (!home) return NULL;

    const char *rel = "/.config/abs-c.ini";
    size_t len = strlen(home) + strlen(rel) + 1;

    char *path = malloc(len);
    if (!path) {
        free(home);
        return NULL;
    }

    snprintf(path, len, "%s%s", home, rel);
    free(home);
    return path;
}

void check_caps(const char *binary_name) {
    char fullpath[PATH_MAX];
    if (!realpath(binary_name, fullpath)) strncpy(fullpath, binary_name, PATH_MAX);

    cap_t caps = cap_get_proc();
    if (!caps) return;

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
    float x_offset_pct;
    float x_scale_pct;
    float y_offset_pct;
    float y_scale_pct;
    int keep_ratio;
    bool enable_buttons;
} configuration;

static int handler(void* user, const char* section, const char* name, const char* value) {
    configuration* cfg = (configuration*)user;
    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("display", "width")) cfg->display_width = atoi(value);
    else if (MATCH("display", "height")) cfg->display_height = atoi(value);
    else if (MATCH("area", "x_offset_pct")) cfg->x_offset_pct = atof(value);
    else if (MATCH("area", "x_scale_pct")) cfg->x_scale_pct = atof(value);
    else if (MATCH("area", "y_offset_pct")) cfg->y_offset_pct = atof(value);
    else if (MATCH("area", "y_scale_pct")) cfg->y_scale_pct = atof(value);
    else if (MATCH("area", "keep_ratio")) cfg->keep_ratio = atoi(value);
    else if (MATCH("input", "enable_buttons")) cfg->enable_buttons = atoi(value);
    else return 0;
    return 1;
}

int init_uinput(int tmin_x, int tmax_x, int tmin_y, int tmax_y) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); exit(EXIT_FAILURE); }

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

static inline int test_bit(int bit, const unsigned long *array) {
    return (array[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1;
}

void print_help(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("  -h, --help            Show this help message\n");
    printf("  -l, --list            List input devices with EV_ABS support\n");
    printf("  -d, --device <arg>    Specify device by path or name substring\n");
}

void list_devices() {
    struct dirent **namelist;
    int ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);
    if (ndevs < 0) { perror("scandir"); return; }

    char path[256], name[256];
    struct stat st;

    for (int i = 0; i < ndevs; i++) {
        if (strcmp(namelist[i]->d_name, ".") == 0 || strcmp(namelist[i]->d_name, "..") == 0) {
            free(namelist[i]);
            continue;
        }

        snprintf(path, sizeof(path), "/dev/input/%s", namelist[i]->d_name);

        if (stat(path, &st) < 0) { free(namelist[i]); continue; }
        if (!S_ISCHR(st.st_mode)) { free(namelist[i]); continue; } // Only character devices

        int devfd = open(path, O_RDONLY);
        if (devfd < 0) { free(namelist[i]); continue; }

        ioctl(devfd, EVIOCGNAME(sizeof(name)), name);

        unsigned long evbits[(EV_MAX+7)/8] = {0};
        ioctl(devfd, EVIOCGBIT(0, sizeof(evbits)), evbits);
        bool has_abs = test_bit(EV_ABS, evbits);

        if (has_abs) printf("%s: %s \n", path, name);

        close(devfd);
        free(namelist[i]);
    }

    free(namelist);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, quit);
    signal(SIGTERM, quit);

    check_caps(argv[0]);

    configuration config = {
        .display_width = 1366,
        .display_height = 768,
        .x_offset_pct = 0,
        .x_scale_pct = 100,
        .y_offset_pct = 0,
        .y_scale_pct = 100,
        .keep_ratio = 1,
        .enable_buttons = 1
    };

    char *config_path = get_abs_c_config_path();
    if (!config_path) {
        fprintf(stderr, "Couldn't find the config file path. Using default settings.\n");
    }
    else {
        printf("Loading config from %s\n", config_path);
        ini_parse(config_path, handler, &config);
        free(config_path);
    }


    const char *dev_override = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { print_help(argv[0]); return 0; }
        else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) { list_devices(); return 0; }
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) && i+1<argc) dev_override = argv[++i];
    }

    struct dirent **namelist;
    int ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);
    if (ndevs < 0) { perror("scandir"); exit(EXIT_FAILURE); }

    char path[256], name[256];
    bool found = false, usable = false;

    for (int i = 0; i < ndevs && !found; i++) {
        snprintf(path, sizeof(path), "/dev/input/%s", namelist[i]->d_name);
        int devfd = open(path, O_RDONLY);
        if (devfd < 0) continue;

        ioctl(devfd, EVIOCGNAME(sizeof(name)), name);

        if (dev_override) {
            if (dev_override[0] == '/' && strcmp(path, dev_override) != 0) { close(devfd); continue; }
            else if (dev_override[0] != '/' && !strstr(name, dev_override)) { close(devfd); continue; }
        }

        unsigned long evbits[(EV_MAX+7)/8] = {0};
        ioctl(devfd, EVIOCGBIT(0, sizeof(evbits)), evbits);

        if (test_bit(EV_ABS, evbits)) { fd = devfd; found = usable = true; }
        else if (dev_override) { found = true; usable = false; close(devfd); }
        else close(devfd);
    }

    for (int i = 0; i < ndevs; i++) free(namelist[i]);
    free(namelist);

    if (!found) { fprintf(stderr, dev_override ? "No device matching '%s'\n" : "No suitable input device found.\n", dev_override); exit(EXIT_FAILURE); }
    if (!usable) { fprintf(stderr, "Device '%s' is not usable (requires EV_ABS support)\n", dev_override);  exit(EXIT_FAILURE); }

    struct input_absinfo absinfo;
    ioctl(fd, EVIOCGABS(ABS_X), &absinfo);
    int tmin_x = absinfo.minimum, tmax_x = absinfo.maximum;
    ioctl(fd, EVIOCGABS(ABS_Y), &absinfo);
    int tmin_y = absinfo.minimum, tmax_y = absinfo.maximum;

    double sr = (double)config.display_width / config.display_height;
    float x_center = (tmin_x + tmax_x)/2.0f + config.x_offset_pct*0.01f*(tmax_x-tmin_x)/2.0f;
    float y_center = (tmin_y + tmax_y)/2.0f + config.y_offset_pct*0.01f*(tmax_y-tmin_y)/2.0f;

    float desired_width = (tmax_x-tmin_x)*config.x_scale_pct*0.01f;
    float desired_height = (tmax_y-tmin_y)*config.y_scale_pct*0.01f;
    float desired_ratio = desired_width / desired_height;

    if (config.keep_ratio) {
        if (desired_ratio > sr) desired_height = desired_width / sr;
        else desired_width = desired_height * sr;
    }

    float x_half_range = desired_width / 2.0f;
    float y_half_range = desired_height / 2.0f;

    int new_tmin_x = (int)(x_center - x_half_range);
    int new_tmax_x = (int)(x_center + x_half_range);
    int new_tmin_y = (int)(y_center - y_half_range);
    int new_tmax_y = (int)(y_center + y_half_range);

    ioctl(fd, EVIOCGRAB, 1);
    tab_fd = init_uinput(new_tmin_x, new_tmax_x, new_tmin_y, new_tmax_y);

    struct sched_param param = {.sched_priority=20};
    sched_setscheduler(0, SCHED_FIFO, &param);
    mlockall(MCL_CURRENT | MCL_FUTURE);

    struct pollfd pfd = {.fd=fd, .events=POLLIN};
    struct input_event ev_buf[64];
    int x=0, y=0, x_old=-1, y_old=-1;

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
            if (config.enable_buttons && ev->type == EV_KEY && ev->code == BTN_LEFT) {
                struct input_event btn_ev[2] = {
                    {.type=EV_KEY, .code=BTN_LEFT, .value=ev->value},
                    {.type=EV_SYN, .code=SYN_REPORT, .value=0}
                };
                write(tab_fd, btn_ev, sizeof(btn_ev));
            }
            if (ev->type==EV_SYN && ev->code==SYN_REPORT) {
                if (x!=x_old || y!=y_old) {
                    struct input_event out_ev[3] = {
                        {.type=EV_ABS, .code=ABS_X, .value=x},
                        {.type=EV_ABS, .code=ABS_Y, .value=y},
                        {.type=EV_SYN, .code=SYN_REPORT, .value=0}
                    };
                    write(tab_fd, out_ev, sizeof(out_ev));
                    x_old = x; y_old = y;
                }
            }
        }
    }

    return 0;
}
