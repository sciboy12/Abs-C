#include <stdio.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

int main(int argc, char *argv[])
{
	
	
	char devname[] = "/dev/input/event5";
	int device = open(devname, O_RDONLY);
	struct input_event ev;
	int x,y;
	int x_old,y_old;
	float x0=1266,y0=0,x1=5676,y1=1366,xp,yp;
	float ax0=1162,ay0=0,ax1=4690,ay1=768,axp,ayp;

	float xt=x0 - x1;
	float ax0t = ax0 - ax1;
	float r = ax0t / xt;
	//printf("%s \n",r);
	float ax0s = ax0 * (r * 2);

	Display *disp;
	Screen *scr;
    Window root_window;
    disp = XOpenDisplay(0);
    root_window = XRootWindow(disp, 0);
    XSelectInput(disp, root_window, KeyReleaseMask);
    
    scr = ScreenOfDisplay(disp, 0);
    printf("%d %d\n", scr->width, scr->height);
      
	while(1)
	{
		read(device,&ev, sizeof(ev));
		if(ev.code == 53){
			x = ev.value;
			}
		if(ev.code == 54){
			y = ev.value;
			}
		if(x != x_old | y != y_old){
			yp = y0 + ((y1-y0)/(x1-x0)) * (x - x0);
			ayp = ay0 + ((ay1-ay0)/(ax1-ax0s)) * (y - ax0s);
			XWarpPointer(disp, None, root_window, 0, 0, 0, 0, yp, ayp);
			XFlush(disp);
		}
		if(ev.code == 53){
			x_old = ev.value;
			}
		if(ev.code == 54){
			y_old = ev.value;
			}
	}
}
