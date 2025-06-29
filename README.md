# Abs-C
[Abs](https://github.com/sciboy12/Abs) ported to C

Thanks to [danil1petrov](https://github.com/danil1petrov), who is the original creator of this port.

If you encounter any problems, please ask on the [Discord](https://discord.gg/vKJfPyU) or file an issue regarding it.

## Setup and compilation
```
sudo apt install libinih-dev
cd (Wherever you saved Abs-C to)
make
sudo setcap cap_sys_nice=eip ./abs-c # used for low-latency input
```
## Configuration

Place abs-c.ini into ~/.config/

### Config options

#### Display

These need to match your current display resolution, in order for accurate scale calculations:

`width`

`height`
#### Custom Area
These values are relative to the center of the touchpad/screen and are measured in percent:

`x_offset_pct`: X Offset

`x_scale_pct`: X Scale 

`y_offset_pct`: Y Offset

`y_scale_pct`: Y Scale

`keep_ratio`: Enable/Disable Input Ratio Compensation (Similar to "Keep aspect ratio" on monitors)

#### Input options

`enable_buttons`: enable LMB on touchpads with a physical LMB

`use_pen`: Use an active stylus on a compatible touchscreen tablet, instead of the touchpad

## Usage

If playing on osu!Lazer, disable High Precision Mouse (and Fullscreen/Confine mouse cursor to window, if on Wayland) within Lazer's settings.

Add your user to the input group (reboot afterwards):

`sudo usermod –a –G input $USER`

Run with:
```
./abs-c
```
