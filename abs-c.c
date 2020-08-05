#include <stdio.h>
#include <stdint.h>
#include <linux/input.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>


//using namespace std; 

int main() {
// Detect touchpad
struct dirent **namelist;
int i, ndevs, fd, found;
char path[64],name[64];
char *tpad = "Touchpad", *tpad1 = "TouchPad", *tpad2 = "Synaptics";
ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);

for (i = 0; i < ndevs; i++) {
    snprintf(path, sizeof path, "%s%s", "/dev/input/", namelist[i]->d_name);
    fd = open(path, O_RDONLY);
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    if (strstr(name,tpad) != NULL | strstr(name,tpad1) != NULL | strstr(name,tpad2) != NULL){found = 1; break;}
    }
    if (found != 1){printf("Touchpad not found\n"); exit(0);};

struct input_event ev;
int tmin_x,tmax_x,tmin_y,tmax_y,x,y,x_old,y_old,int_y,int_x;

// Get min and max touchpad values
int abs[1];
ioctl(fd, EVIOCGABS(ABS_X), abs);
tmin_x = abs[1];
tmax_x = abs[2];
ioctl(fd, EVIOCGABS(ABS_Y), abs);
tmin_y = abs[1];
tmax_y = abs[2];

// Display resolution
Display *disp;
Screen *scr;
Window root_window;
disp = XOpenDisplay(0);
root_window = XRootWindow(disp, 0);
scr = ScreenOfDisplay(disp, 0);
double sr, tr, xo, yo;
int sx, sy, newty, newtx;

// Get screen resolution
sx=scr->width;
sy=scr->height;

// Calculate screen ratio
sr = (float)sx / (float)sy;

// Calculate touchpad ratio
tr = (float)tmax_x / (float)tmax_y;

// Compensate for display/touchpad ratio difference and center the active area
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

// Grab device
// Comment this out to enable your touchpad's gestures/buttons
ioctl(fd, EVIOCGRAB);

printf("Press Ctrl-C to quit\n");
while(1) {
    read(fd,&ev, sizeof(ev));
    if( ev.code == 0 & ev.value != 0 ) { x = ev.value; }
    if( ev.code == 1 ) { y = ev.value; }
    if(x != x_old | y != y_old){
    int_x = ( x - tmin_x ) * scr->width / ( tmax_x - tmin_x );
    int_y = ( y - tmin_y ) * scr->height / ( tmax_y - tmin_y );
    XWarpPointer(disp, None, root_window, 0, 0, 0, 0, int_x, int_y);
    XFlush(disp);
    }
    if( ev.code == 0 & ev.value != 0 ) { x_old = ev.value; }
    if( ev.code == 1 ) { y_old = ev.value; }
}

}
