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

// WIP
int quit() {
    //extern int tab_fd;
    //ioctl(tab_fd, UI_DEV_DESTROY);
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

int init_uinput(int tmin_x, int tmax_x, int tmin_y, int tmax_y) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if(fd < 0) {
        printf("Failed to access /dev/uinput\n");
        exit(EXIT_FAILURE);
    }

    #define EV_KEY          0x01
    #define EV_REL          0x02
    #define EV_ABS          0x03
    #define UI_SET_PROPBIT    _IOW(UINPUT_IOCTL_BASE, 110, int)

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    //ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    //ioctl(fd, UI_SET_KEYBIT, ABS_PRESSURE);
    //ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    #define UINPUT_MAX_NAME_SIZE    80

    struct uinput_user_dev {
        char name[UINPUT_MAX_NAME_SIZE];
        struct input_id id;
            int ff_effects_max;
            int absmax[ABS_MAX + 1];
            int absmin[ABS_MAX + 1];
            int absfuzz[ABS_MAX + 1];
            int absflat[ABS_MAX + 1];
    };

    struct uinput_user_dev uidev;

    memset(&uidev, 0, sizeof(uidev));

    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "Abs-C Virtual Tablet");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1234;
    uidev.id.product = 0xfedc;
    uidev.id.version = 1;
    uidev.absmin[ABS_X] = tmin_x;
    uidev.absmax[ABS_X] = tmax_x + 1;
    uidev.absmin[ABS_Y] = tmin_y;
    uidev.absmax[ABS_Y] = tmax_y;

    write(fd, &uidev, sizeof(uidev));
    ioctl(fd, UI_DEV_CREATE);

    return fd;
}


int main() {
    // WIP
    //signal(SIGTERM, quit);

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

    // Set variables to config file contents
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

    
    // Detect touchpad
    struct dirent **namelist;
    int i, ndevs, fd, found;
    char path[64],name[64];
    char *pen;
    char *tpad1;
    char *tpad2;
    char *tpad3;
    char *tpad4;
    bool is_mac;

    if (use_pen == 1) {
        pen = "Pen";
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

            // Check for fuzzy match
            else if (found != 1 &
                strstr(name,tpad1) != NULL |
                strstr(name,tpad2) != NULL |
                strstr(name,tpad3) != NULL |
                strstr(name,tpad4) != NULL){found = 1; break;}
        }
        is_mouse = false;
    }

        // Check if device is uses the bcm5974 chip (Apple)
        if (strstr(name,tpad4) != NULL) {
            is_mac = true;
        }
        else {
            is_mac = false;
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

    // Calculate screen ratio
    double sr, tr, xo, yo;
    sr = (float)display_width / (float)display_height;
    // Calculate touchpad ratio
    tr = (float)tmax_x / (float)tmax_y;

    // Compensate for screen/touchpad ratio difference and center the active area
    // This is to ensure there is no stretching of the area
    // Equivalent to "Keep aspect ratio" on monitors
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

    printf("Press Ctrl-C to quit\n");

    // Init virtual tablet    
    int tab_fd = init_uinput(new_tmin_x, new_tmax_x, new_tmin_y, new_tmax_y);
    struct input_event tab_ev[2];
    memset(tab_ev, 0, sizeof(tab_ev));

    // Init values and start main loop
    int x,y,x_old,y_old;
    struct input_event ev;
    while(1) {
        // Read event from touchpad
        read(fd,&ev, sizeof(ev));        

        // Set values according to event codes
        if(ev.code == ABS_X) {x = ev.value;}
        if(ev.code == ABS_Y) {y = ev.value;}

        // Only update position if value has changed
        if(x != y_old && y != 0) {
            //memset(tab_ev, 0, sizeof(tab_ev));
            tab_ev[0].type = EV_ABS;
            tab_ev[0].code = ABS_Y;
            tab_ev[0].value = y;
            write(tab_fd, tab_ev, sizeof(tab_ev));

            update = true;
        }

        // Only update position if value has changed
        // "x > 0" is used to prevent cursor from flickering to left edge
        if(y != x_old && x != 0) {
            //memset(tab_ev, 0, sizeof(tab_ev));
            tab_ev[1].type = EV_ABS;
            tab_ev[1].code = ABS_X;
            tab_ev[1].value = x;
            write(tab_fd, tab_ev, sizeof(tab_ev));

            update = true;
        }

        // Only sync if needed
        if (update == true) {
            //memset(tab_ev, 0, sizeof(tab_ev));
            tab_ev[0].type = EV_SYN;
            tab_ev[0].code = SYN_REPORT;
            tab_ev[0].value = 0;
            //write(tab_fd, tab_ev, sizeof(tab_ev));

            //memset(tab_ev, 0, sizeof(tab_ev));
            tab_ev[1].type = EV_SYN;
            tab_ev[1].code = SYN_REPORT;
            tab_ev[1].value = 0;
            //write(tab_fd, tab_ev, sizeof(tab_ev));

            // Reset update indicator
            update = false;
        }
            
        // Set old vars (for next iteration of the loop)
        x_old = x;
        y_old = y;
    }
}
