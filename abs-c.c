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

// Flag to control the main loop
volatile sig_atomic_t stop = 0;
int tab_fd;

void quit() {
    printf("\nExiting.\n");
    stop = 1;

    ioctl(tab_fd, UI_DEV_DESTROY);
    exit(0);
    }

typedef struct
{
    int display_width;
    int display_height;
    float x_scale_pct_min;
    float x_scale_pct_max;
    float y_scale_pct_min;
    float y_scale_pct_max;
    int keep_ratio;
    int use_pen;

} configuration;

static int handler(void* user, const char* section, const char* name,
                   const char* value)
{
    configuration* pconfig = (configuration*)user;

    #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
    if (MATCH("display", "width")) {
        pconfig->display_width = atoi(value);
    } else if (MATCH("display", "height")) {
        pconfig->display_height = atoi(value);
    } else if (MATCH("area", "x_scale_pct_min")) {
        pconfig->x_scale_pct_min = atof(value);
    } else if (MATCH("area", "x_scale_pct_max")) {
        pconfig->x_scale_pct_max = atof(value);
    } else if (MATCH("area", "y_scale_pct_min")) {
        pconfig->y_scale_pct_min = atof(value);
    } else if (MATCH("area", "y_scale_pct_max")) {
        pconfig->y_scale_pct_max = atof(value);
    } else if (MATCH("area", "keep_ratio")) {
        pconfig->keep_ratio = atoi(value);
    } else if (MATCH("area", "use_pen")) {
        pconfig->use_pen = atoi(value);
    } else {
        return 0;  /* unknown section/name, error */
    }
    return 1;
}

// Creates the virtual tablet and returns it's file descriptor
int init_uinput(int tmin_x, int tmax_x, int tmin_y, int tmax_y) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("Failed to open /dev/uinput");
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
    uidev.absfuzz[ABS_X] = 0;
    uidev.absflat[ABS_X] = 0;

    uidev.absmin[ABS_Y] = tmin_y;
    uidev.absmax[ABS_Y] = tmax_y;
    uidev.absfuzz[ABS_Y] = 0;
    uidev.absflat[ABS_Y] = 0;

    write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);

    return fd;
}


int main() {
    signal(SIGINT, quit);
    signal(SIGTERM, quit);

    int display_width;
    int display_height;
    float x_scale_pct_min;
    float x_scale_pct_max;
    float y_scale_pct_min;
    float y_scale_pct_max;
    int keep_ratio;
    int use_pen;

    configuration config;

    // Get user config dir
    char config_path[50];
    strcpy(config_path, getenv("HOME"));
    strcat(config_path, "/.config/abs-c.ini");
    printf("Loading config from %s\n", config_path);

    // Try to load config from $HOME/.config/
    if (ini_parse(config_path, handler, &config) < 0) {
        // Use defaults if above fails
        display_width = 1366;
        display_height = 768;
        x_scale_pct_min = 100; // Left edge
        x_scale_pct_max = 100; // Right edge
        y_scale_pct_min = 100; // Top edge
        y_scale_pct_max = 100; // Bottom edge
        keep_ratio = 1;
        use_pen = 0;
    }

    // Set variables to config file contents if parsing succeeds
    else {
        display_width = config.display_width;
        display_height = config.display_height;
        x_scale_pct_min = config.x_scale_pct_min;
        x_scale_pct_max = config.x_scale_pct_max;
        y_scale_pct_min = config.y_scale_pct_min;
        y_scale_pct_max = config.y_scale_pct_max;
        keep_ratio = config.keep_ratio;
        use_pen = config.use_pen;
    }

    
    // Try to detect touchpad using preset keywords
    struct dirent **namelist;
    int i, ndevs, fd, found;
    char path[32],name[256];
    char *pen;
    char *tpad1;
    char *tpad2;
    char *tpad3;
    char *tpad4;
    bool is_mac;

    if (use_pen == 1) {
        pen = "Stylus";
    }
    else {
        tpad1 = "Touchpad";
        tpad2 = "TouchPad";
        tpad3 = "Synaptics";
        tpad4 = "bcm5974";
    }

    ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);
    bool is_mouse;
    for (i = 0; i < ndevs; i++) {
        snprintf(path, sizeof path, "%s%s", "/dev/input/", namelist[i]->d_name);
        fd = open(path, O_RDONLY);
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        if(strstr(name, "Mouse") != NULL) {
            is_mouse = true;
        }

        if (is_mouse != true) {
            if (use_pen == 1) {
                if (strstr(name,pen) != NULL){found = 1; break;}
            }

            // Check for keyword match
            else if (found != 1 &
                strstr(name,tpad1) != NULL |
                strstr(name,tpad2) != NULL |
                strstr(name,tpad3) != NULL |
                strstr(name,tpad4) != NULL){found = 1; break;}
        }
        is_mouse = false;
    }
        // Ensure touchpad mode is active
        if (use_pen != 1) {
            // Check if the touchpad uses the bcm5974 chip (Apple)
            if (strstr(name,tpad4) != NULL) {
                is_mac = true;
            }
            else {
                is_mac = false;
            }
        }
    // Exit if no touchpad is detected
    if (found != 1){printf("Device not found, exiting...\n"); exit(0);};

    // Get min and max touchpad values
    int abs[1];
    ioctl(fd, EVIOCGABS(ABS_X), abs);
    int tmin_x = abs[1];
    int tmax_x = abs[2];
    ioctl(fd, EVIOCGABS(ABS_Y), abs);
    int tmin_y = abs[1];
    int tmax_y = abs[2];

    // Fix area if using a Mac Touchpad (bcm5974)
    if (is_mac == true) {
    tmin_y = tmin_y +  1350;
    }

    // Calculate screen and touchpad aspect ratios
    double sr, tr, xo, yo;
    sr = (float)display_width / (float)display_height;
    tr = (float)tmax_x / (float)tmax_y;

    // Compensate for screen/touchpad ratio difference and center the active area
    // This is to ensure there is no stretching of the area
    // Equivalent to "Keep aspect ratio" setting on monitors
    if (keep_ratio == 1) {
        if (sr < tr) {

            // Calculate X offset
            xo = tmax_x * sr / 16;

            // Apply Offset
            tmin_x = tmin_x + xo;
            tmax_x = tmax_x - xo;
        }

        else {
            //Calculate Y offset
            yo = tmax_y * sr / 16;

            //Apply Offset
            tmin_y = tmin_y + yo;
            tmax_y = tmax_y - yo;
        }
    }

    // Apply custom area
    float x_scale_min = x_scale_pct_min * 0.01;
    float x_scale_max = x_scale_pct_max * 0.01;
    float y_scale_min = y_scale_pct_min * 0.01;
    float y_scale_max = y_scale_pct_max * 0.01;

    int new_tmin_x = tmax_x - (x_scale_min * (tmax_x - tmin_x));
    int new_tmax_x = tmin_x + (x_scale_max * (tmax_x - tmin_x));
    int new_tmin_y = tmax_y - (y_scale_min * (tmax_y - tmin_y));
    int new_tmax_y = tmin_y + (y_scale_max * (tmax_y - tmin_y));

    // Grab device
    // Comment this out to enable your touchpad's gestures/buttons
    ioctl(fd, EVIOCGRAB, 1);

    // Init update indicator
    bool update = false;

    // Init virtual tablet    
    tab_fd = init_uinput(new_tmin_x, new_tmax_x, new_tmin_y, new_tmax_y);
    struct input_event tab_ev[2];
    memset(tab_ev, 0, sizeof(tab_ev));

    // After setting up everything and before entering the loop:
    struct sched_param param = { .sched_priority = 20 };
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        perror("Failed to set real-time priority");
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct input_event ev;
    struct input_event out_ev[3];

    int x = 0, y = 0, x_old = -1, y_old = -1;

    printf("Press Ctrl-C to quit\n");

    while (!stop) {
        if (poll(&pfd, 1, -1) <= 0) continue;
        if (read(fd, &ev, sizeof(ev)) <= 0) continue;

        int n = 0;
        while (read(fd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_X) x = ev.value;
                if (ev.code == ABS_Y) y = ev.value;
            }
            if (ev.type == EV_SYN && ev.code == SYN_REPORT) break;
        }

        if ((x != x_old || y != y_old) && x > 0 && y > 0) {
            out_ev[0] = (struct input_event){ .type = EV_ABS, .code = ABS_X, .value = x };
            out_ev[1] = (struct input_event){ .type = EV_ABS, .code = ABS_Y, .value = y };
            out_ev[2] = (struct input_event){ .type = EV_SYN, .code = SYN_REPORT, .value = 0 };

            write(tab_fd, out_ev, sizeof(struct input_event) * 3);

            x_old = x;
            y_old = y;
        }
    }
}
