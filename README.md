# Abs-C
Abs ported to C

Thanks to danil1petrov, who is the original creator of this code.

## Setup and compilation
```
sudo apt install libx11-dev evtest
sudo evtest
```
At this point, select your touchpad, then press Ctrl-C and scroll up.

Look for the Min and Max values of ABS_X and ABS_Y, then open 1.c and look for:
```
float x0=1266,y0=0,x1=5676,y1=1366,xp,yp;
float ax0=1162,ay0=0,ax1=4690,ay1=768,axp,ayp;
```
Replace the following values with the ones from `evtest` as follows:

x0=Your_ABS_X_Min
x1=Your_ABS_X_Max
ax0=Your_ABS_Y_Min
ax1=Your_ABS_Y_Max


Then compile with:
```
gcc 1.c -lX11
```
Run the compiled file with:
```
sudo a.out
```
