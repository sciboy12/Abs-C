# Abs-C
[Abs](https://github.com/sciboy12/Abs) ported to C

Thanks to [danil1petrov](https://github.com/danil1petrov), who is the original creator of this port.

If you encounter any problems, please ask on the [Discord](https://discord.gg/vKJfPyU) or file an issue regarding it.

## Setup and compilation
```
sudo apt install libx11-dev
cd (Wherever you saved Abs-C to)
make
```
## Configuration

Place abs-c.ini into ~/.config/

### Config options

#### Custom Area
These values are relative to the center of the touchpad/screen and are measured in percent:

`x_scale_pct_min`: Left edge

`x_scale_pct_max`: Right Edge

`y_scale_pct_min`: Top edge

`y_scale_pct_max`: Bottom edge


#### Other options
`keep_ratio`: Enable/Disable Ratio Compensation (Similar to "Keep aspect ratio" on monitors)

`use_pen`: Use an active stylus on a compatible touchscreen tablet, instead of the touchpad

## Usage

If playing on osu!Lazer, disable High Precision Mouse and Fullscreen within Lazer's settings.

Run with:
```
sudo ./abs-c
```
