#include "tosuhandler.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ini.h>
#include <limits.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <poll.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>

#define INACTIVE_SLEEP_MS 100 // long sleep to save CPU
#define MAX_KEYBOARDS 64

volatile sig_atomic_t stop = 0;
int tab_fd = -1;
int fd = -1;
static bool verbose_logging = false;

typedef struct
{
    int fd;
    bool hotkey_down;
} keyboard_device;

static void verbose_perror(const char *msg)
{
    if (verbose_logging)
        perror(msg);
}

static void verbose_fprintf(FILE *stream, const char *fmt, ...)
{
    if (!verbose_logging)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);
}

static void quit(int sig)
{
    (void)sig;
    stop = 1;
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
        verbose_perror("ioctl EVIOCGRAB");
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
        verbose_fprintf(stderr, "[!] Warning: CAP_SYS_NICE not set. Real-time priority "
                                "might fail.\n");
        verbose_fprintf(stderr, "    Run: sudo setcap cap_sys_nice=eip %s\n", fullpath);
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
    bool enable_hotkey;
    int hotkey_key;
} configuration;

static int parse_key_code(const char *value);

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
    else if (MATCH("input", "enable_hotkey"))
        cfg->enable_hotkey = atoi(value);
    else if (MATCH("input", "hotkey_key"))
    {
        int key_code = parse_key_code(value);

        if (key_code >= 0)
            cfg->hotkey_key = key_code;
    }
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

    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);

    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE);

    struct uinput_abs_setup abs;

    /* ABS_X */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_X;
    abs.absinfo.minimum = tmin_x;
    abs.absinfo.maximum = tmax_x;
    abs.absinfo.resolution = 1000;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        verbose_perror("UI_ABS_SETUP ABS_X");

    /* ABS_Y */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_Y;
    abs.absinfo.minimum = tmin_y;
    abs.absinfo.maximum = tmax_y;
    abs.absinfo.resolution = 1000;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        verbose_perror("UI_ABS_SETUP ABS_Y");

    /* ABS_PRESSURE */
    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_PRESSURE;
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = 1024;
    abs.absinfo.resolution = 1;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0)
        verbose_perror("UI_ABS_SETUP PRESSURE");

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
    struct input_event ev = {0};

    ev.type = EV_KEY;
    ev.code = BTN_TOOL_PEN;
    ev.value = 1;
    write(fd, &ev, sizeof(ev));

    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(fd, &ev, sizeof(ev));

    return fd;
}

static inline int test_bit(int bit, const unsigned long *array)
{
    return (array[bit / (8 * sizeof(unsigned long))] >>
            (bit % (8 * sizeof(unsigned long)))) &
           1;
}

static void close_keyboard_devices(keyboard_device *keyboards, int *count)
{
    for (int i = 0; i < *count; i++)
    {
        if (keyboards[i].fd >= 0)
            close(keyboards[i].fd);
    }

    *count = 0;
}

static bool is_keyboard_device(int devfd, int hotkey_key, bool *hotkey_down)
{
    if (hotkey_key < 0 || hotkey_key > KEY_MAX)
        return false;

    unsigned long evbits[(EV_MAX + (sizeof(unsigned long) * 8) - 1) /
                          (sizeof(unsigned long) * 8)] = {0};

    if (ioctl(devfd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0)
        return false;

    if (!test_bit(EV_KEY, evbits))
        return false;

    unsigned long keybits[(KEY_MAX + (sizeof(unsigned long) * 8) - 1) /
                          (sizeof(unsigned long) * 8)] = {0};

    if (ioctl(devfd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0)
        return false;

    if (!test_bit(hotkey_key, keybits))
        return false;

    *hotkey_down = false;

    if (ioctl(devfd, EVIOCGKEY(sizeof(keybits)), keybits) >= 0)
        *hotkey_down = test_bit(hotkey_key, keybits);

    return true;
}

static void scan_keyboard_devices(keyboard_device *keyboards,
                                  int *count,
                                  int hotkey_key)
{
    struct dirent **namelist;
    int ndevs = scandir("/dev/input/", &namelist, NULL, alphasort);

    if (ndevs < 0)
    {
        verbose_perror("scandir /dev/input");
        return;
    }

    close_keyboard_devices(keyboards, count);

    for (int i = 0; i < ndevs && *count < MAX_KEYBOARDS; i++)
    {
        if (strcmp(namelist[i]->d_name, ".") == 0 ||
            strcmp(namelist[i]->d_name, "..") == 0)
        {
            free(namelist[i]);
            continue;
        }

        if (strncmp(namelist[i]->d_name, "event", 5) != 0)
        {
            free(namelist[i]);
            continue;
        }

        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", namelist[i]->d_name);

        struct stat st;
        if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode))
        {
            free(namelist[i]);
            continue;
        }

        int devfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (devfd < 0)
        {
            free(namelist[i]);
            continue;
        }

        bool hotkey_down = false;

        if (is_keyboard_device(devfd, hotkey_key, &hotkey_down))
        {
            keyboards[*count].fd = devfd;
            keyboards[*count].hotkey_down = hotkey_down;
            (*count)++;
            devfd = -1;
        }

        if (devfd >= 0)
            close(devfd);

        free(namelist[i]);
    }

    free(namelist);
}

static bool any_keyboard_hotkey_down(const keyboard_device *keyboards,
                                     int count)
{
    for (int i = 0; i < count; i++)
    {
        if (keyboards[i].hotkey_down)
            return true;
    }

    return false;
}

static int parse_key_code(const char *value)
{
    if (!value || !*value)
        return -1;

#define KEY_NAME(name) \
    if (!strcmp(value, #name)) \
        return name;

    KEY_NAME(KEY_RESERVED)
    KEY_NAME(KEY_ESC)
    KEY_NAME(KEY_1)
    KEY_NAME(KEY_2)
    KEY_NAME(KEY_3)
    KEY_NAME(KEY_4)
    KEY_NAME(KEY_5)
    KEY_NAME(KEY_6)
    KEY_NAME(KEY_7)
    KEY_NAME(KEY_8)
    KEY_NAME(KEY_9)
    KEY_NAME(KEY_0)
    KEY_NAME(KEY_MINUS)
    KEY_NAME(KEY_EQUAL)
    KEY_NAME(KEY_BACKSPACE)
    KEY_NAME(KEY_TAB)
    KEY_NAME(KEY_Q)
    KEY_NAME(KEY_W)
    KEY_NAME(KEY_E)
    KEY_NAME(KEY_R)
    KEY_NAME(KEY_T)
    KEY_NAME(KEY_Y)
    KEY_NAME(KEY_U)
    KEY_NAME(KEY_I)
    KEY_NAME(KEY_O)
    KEY_NAME(KEY_P)
    KEY_NAME(KEY_LEFTBRACE)
    KEY_NAME(KEY_RIGHTBRACE)
    KEY_NAME(KEY_ENTER)
    KEY_NAME(KEY_LEFTCTRL)
    KEY_NAME(KEY_A)
    KEY_NAME(KEY_S)
    KEY_NAME(KEY_D)
    KEY_NAME(KEY_F)
    KEY_NAME(KEY_G)
    KEY_NAME(KEY_H)
    KEY_NAME(KEY_J)
    KEY_NAME(KEY_K)
    KEY_NAME(KEY_L)
    KEY_NAME(KEY_SEMICOLON)
    KEY_NAME(KEY_APOSTROPHE)
    KEY_NAME(KEY_GRAVE)
    KEY_NAME(KEY_LEFTSHIFT)
    KEY_NAME(KEY_BACKSLASH)
    KEY_NAME(KEY_Z)
    KEY_NAME(KEY_X)
    KEY_NAME(KEY_C)
    KEY_NAME(KEY_V)
    KEY_NAME(KEY_B)
    KEY_NAME(KEY_N)
    KEY_NAME(KEY_M)
    KEY_NAME(KEY_COMMA)
    KEY_NAME(KEY_DOT)
    KEY_NAME(KEY_SLASH)
    KEY_NAME(KEY_RIGHTSHIFT)
    KEY_NAME(KEY_KPASTERISK)
    KEY_NAME(KEY_LEFTALT)
    KEY_NAME(KEY_SPACE)
    KEY_NAME(KEY_CAPSLOCK)
    KEY_NAME(KEY_F1)
    KEY_NAME(KEY_F2)
    KEY_NAME(KEY_F3)
    KEY_NAME(KEY_F4)
    KEY_NAME(KEY_F5)
    KEY_NAME(KEY_F6)
    KEY_NAME(KEY_F7)
    KEY_NAME(KEY_F8)
    KEY_NAME(KEY_F9)
    KEY_NAME(KEY_F10)
    KEY_NAME(KEY_NUMLOCK)
    KEY_NAME(KEY_SCROLLLOCK)
    KEY_NAME(KEY_KP7)
    KEY_NAME(KEY_KP8)
    KEY_NAME(KEY_KP9)
    KEY_NAME(KEY_KPMINUS)
    KEY_NAME(KEY_KP4)
    KEY_NAME(KEY_KP5)
    KEY_NAME(KEY_KP6)
    KEY_NAME(KEY_KPPLUS)
    KEY_NAME(KEY_KP1)
    KEY_NAME(KEY_KP2)
    KEY_NAME(KEY_KP3)
    KEY_NAME(KEY_KP0)
    KEY_NAME(KEY_KPDOT)
    KEY_NAME(KEY_ZENKAKUHANKAKU)
    KEY_NAME(KEY_102ND)
    KEY_NAME(KEY_F11)
    KEY_NAME(KEY_F12)
    KEY_NAME(KEY_RO)
    KEY_NAME(KEY_KATAKANA)
    KEY_NAME(KEY_HIRAGANA)
    KEY_NAME(KEY_HENKAN)
    KEY_NAME(KEY_KATAKANAHIRAGANA)
    KEY_NAME(KEY_MUHENKAN)
    KEY_NAME(KEY_KPJPCOMMA)
    KEY_NAME(KEY_KPENTER)
    KEY_NAME(KEY_RIGHTCTRL)
    KEY_NAME(KEY_KPSLASH)
    KEY_NAME(KEY_SYSRQ)
    KEY_NAME(KEY_RIGHTALT)
    KEY_NAME(KEY_LINEFEED)
    KEY_NAME(KEY_HOME)
    KEY_NAME(KEY_UP)
    KEY_NAME(KEY_PAGEUP)
    KEY_NAME(KEY_LEFT)
    KEY_NAME(KEY_RIGHT)
    KEY_NAME(KEY_END)
    KEY_NAME(KEY_DOWN)
    KEY_NAME(KEY_PAGEDOWN)
    KEY_NAME(KEY_INSERT)
    KEY_NAME(KEY_DELETE)
    KEY_NAME(KEY_MACRO)
    KEY_NAME(KEY_MUTE)
    KEY_NAME(KEY_VOLUMEDOWN)
    KEY_NAME(KEY_VOLUMEUP)
    KEY_NAME(KEY_POWER)
    KEY_NAME(KEY_KPEQUAL)
    KEY_NAME(KEY_KPPLUSMINUS)
    KEY_NAME(KEY_PAUSE)
    KEY_NAME(KEY_SCALE)
    KEY_NAME(KEY_KPCOMMA)
    KEY_NAME(KEY_HANGEUL)
    KEY_NAME(KEY_HANGUEL)
    KEY_NAME(KEY_HANJA)
    KEY_NAME(KEY_YEN)
    KEY_NAME(KEY_LEFTMETA)
    KEY_NAME(KEY_RIGHTMETA)
    KEY_NAME(KEY_COMPOSE)
    KEY_NAME(KEY_STOP)
    KEY_NAME(KEY_AGAIN)
    KEY_NAME(KEY_PROPS)
    KEY_NAME(KEY_UNDO)
    KEY_NAME(KEY_FRONT)
    KEY_NAME(KEY_COPY)
    KEY_NAME(KEY_OPEN)
    KEY_NAME(KEY_PASTE)
    KEY_NAME(KEY_FIND)
    KEY_NAME(KEY_CUT)
    KEY_NAME(KEY_HELP)
    KEY_NAME(KEY_MENU)
    KEY_NAME(KEY_CALC)
    KEY_NAME(KEY_SETUP)
    KEY_NAME(KEY_SLEEP)
    KEY_NAME(KEY_WAKEUP)
    KEY_NAME(KEY_FILE)
    KEY_NAME(KEY_SENDFILE)
    KEY_NAME(KEY_DELETEFILE)
    KEY_NAME(KEY_XFER)
    KEY_NAME(KEY_PROG1)
    KEY_NAME(KEY_PROG2)
    KEY_NAME(KEY_WWW)
    KEY_NAME(KEY_MSDOS)
    KEY_NAME(KEY_COFFEE)
    KEY_NAME(KEY_SCREENLOCK)
    KEY_NAME(KEY_ROTATE_DISPLAY)
    KEY_NAME(KEY_DIRECTION)
    KEY_NAME(KEY_CYCLEWINDOWS)
    KEY_NAME(KEY_MAIL)
    KEY_NAME(KEY_BOOKMARKS)
    KEY_NAME(KEY_COMPUTER)
    KEY_NAME(KEY_BACK)
    KEY_NAME(KEY_FORWARD)
    KEY_NAME(KEY_CLOSECD)
    KEY_NAME(KEY_EJECTCD)
    KEY_NAME(KEY_EJECTCLOSECD)
    KEY_NAME(KEY_NEXTSONG)
    KEY_NAME(KEY_PLAYPAUSE)
    KEY_NAME(KEY_PREVIOUSSONG)
    KEY_NAME(KEY_STOPCD)
    KEY_NAME(KEY_RECORD)
    KEY_NAME(KEY_REWIND)
    KEY_NAME(KEY_PHONE)
    KEY_NAME(KEY_ISO)
    KEY_NAME(KEY_CONFIG)
    KEY_NAME(KEY_HOMEPAGE)
    KEY_NAME(KEY_REFRESH)
    KEY_NAME(KEY_EXIT)
    KEY_NAME(KEY_MOVE)
    KEY_NAME(KEY_EDIT)
    KEY_NAME(KEY_SCROLLUP)
    KEY_NAME(KEY_SCROLLDOWN)
    KEY_NAME(KEY_KPLEFTPAREN)
    KEY_NAME(KEY_KPRIGHTPAREN)
    KEY_NAME(KEY_NEW)
    KEY_NAME(KEY_REDO)
    KEY_NAME(KEY_F13)
    KEY_NAME(KEY_F14)
    KEY_NAME(KEY_F15)
    KEY_NAME(KEY_F16)
    KEY_NAME(KEY_F17)
    KEY_NAME(KEY_F18)
    KEY_NAME(KEY_F19)
    KEY_NAME(KEY_F20)
    KEY_NAME(KEY_F21)
    KEY_NAME(KEY_F22)
    KEY_NAME(KEY_F23)
    KEY_NAME(KEY_F24)
    KEY_NAME(KEY_PLAYCD)
    KEY_NAME(KEY_PAUSECD)
    KEY_NAME(KEY_PROG3)
    KEY_NAME(KEY_PROG4)
    KEY_NAME(KEY_ALL_APPLICATIONS)
    KEY_NAME(KEY_DASHBOARD)
    KEY_NAME(KEY_SUSPEND)
    KEY_NAME(KEY_CLOSE)
    KEY_NAME(KEY_PLAY)
    KEY_NAME(KEY_FASTFORWARD)
    KEY_NAME(KEY_BASSBOOST)
    KEY_NAME(KEY_PRINT)
    KEY_NAME(KEY_HP)
    KEY_NAME(KEY_CAMERA)
    KEY_NAME(KEY_SOUND)
    KEY_NAME(KEY_QUESTION)
    KEY_NAME(KEY_EMAIL)
    KEY_NAME(KEY_CHAT)
    KEY_NAME(KEY_SEARCH)
    KEY_NAME(KEY_CONNECT)
    KEY_NAME(KEY_FINANCE)
    KEY_NAME(KEY_SPORT)
    KEY_NAME(KEY_SHOP)
    KEY_NAME(KEY_ALTERASE)
    KEY_NAME(KEY_CANCEL)
    KEY_NAME(KEY_BRIGHTNESSDOWN)
    KEY_NAME(KEY_BRIGHTNESSUP)
    KEY_NAME(KEY_MEDIA)
    KEY_NAME(KEY_SWITCHVIDEOMODE)
    KEY_NAME(KEY_KBDILLUMTOGGLE)
    KEY_NAME(KEY_KBDILLUMDOWN)
    KEY_NAME(KEY_KBDILLUMUP)
    KEY_NAME(KEY_SEND)
    KEY_NAME(KEY_REPLY)
    KEY_NAME(KEY_FORWARDMAIL)
    KEY_NAME(KEY_SAVE)
    KEY_NAME(KEY_DOCUMENTS)
    KEY_NAME(KEY_BATTERY)
    KEY_NAME(KEY_BLUETOOTH)
    KEY_NAME(KEY_WLAN)
    KEY_NAME(KEY_UWB)
    KEY_NAME(KEY_UNKNOWN)
    KEY_NAME(KEY_VIDEO_NEXT)
    KEY_NAME(KEY_VIDEO_PREV)
    KEY_NAME(KEY_BRIGHTNESS_CYCLE)
    KEY_NAME(KEY_BRIGHTNESS_AUTO)
    KEY_NAME(KEY_BRIGHTNESS_ZERO)
    KEY_NAME(KEY_DISPLAY_OFF)
    KEY_NAME(KEY_WWAN)
    KEY_NAME(KEY_WIMAX)
    KEY_NAME(KEY_RFKILL)
    KEY_NAME(KEY_MICMUTE)
    KEY_NAME(KEY_OK)
    KEY_NAME(KEY_SELECT)
    KEY_NAME(KEY_GOTO)
    KEY_NAME(KEY_CLEAR)
    KEY_NAME(KEY_POWER2)
    KEY_NAME(KEY_OPTION)
    KEY_NAME(KEY_INFO)
    KEY_NAME(KEY_TIME)
    KEY_NAME(KEY_VENDOR)
    KEY_NAME(KEY_ARCHIVE)
    KEY_NAME(KEY_PROGRAM)
    KEY_NAME(KEY_CHANNEL)
    KEY_NAME(KEY_FAVORITES)
    KEY_NAME(KEY_EPG)
    KEY_NAME(KEY_PVR)
    KEY_NAME(KEY_MHP)
    KEY_NAME(KEY_LANGUAGE)
    KEY_NAME(KEY_TITLE)
    KEY_NAME(KEY_SUBTITLE)
    KEY_NAME(KEY_ANGLE)
    KEY_NAME(KEY_FULL_SCREEN)
    KEY_NAME(KEY_ZOOM)
    KEY_NAME(KEY_MODE)
    KEY_NAME(KEY_KEYBOARD)
    KEY_NAME(KEY_ASPECT_RATIO)
    KEY_NAME(KEY_SCREEN)
    KEY_NAME(KEY_PC)
    KEY_NAME(KEY_TV)
    KEY_NAME(KEY_TV2)
    KEY_NAME(KEY_VCR)
    KEY_NAME(KEY_VCR2)
    KEY_NAME(KEY_SAT)
    KEY_NAME(KEY_SAT2)
    KEY_NAME(KEY_CD)
    KEY_NAME(KEY_TAPE)
    KEY_NAME(KEY_RADIO)
    KEY_NAME(KEY_TUNER)
    KEY_NAME(KEY_PLAYER)
    KEY_NAME(KEY_TEXT)
    KEY_NAME(KEY_DVD)
    KEY_NAME(KEY_AUX)
    KEY_NAME(KEY_MP3)
    KEY_NAME(KEY_AUDIO)
    KEY_NAME(KEY_VIDEO)
    KEY_NAME(KEY_DIRECTORY)
    KEY_NAME(KEY_LIST)
    KEY_NAME(KEY_MEMO)
    KEY_NAME(KEY_CALENDAR)
    KEY_NAME(KEY_RED)
    KEY_NAME(KEY_GREEN)
    KEY_NAME(KEY_YELLOW)
    KEY_NAME(KEY_BLUE)
    KEY_NAME(KEY_CHANNELUP)
    KEY_NAME(KEY_CHANNELDOWN)
    KEY_NAME(KEY_FIRST)
    KEY_NAME(KEY_LAST)
    KEY_NAME(KEY_AB)
    KEY_NAME(KEY_NEXT)
    KEY_NAME(KEY_RESTART)
    KEY_NAME(KEY_SLOW)
    KEY_NAME(KEY_SHUFFLE)
    KEY_NAME(KEY_BREAK)
    KEY_NAME(KEY_PREVIOUS)
    KEY_NAME(KEY_DIGITS)
    KEY_NAME(KEY_TEEN)
    KEY_NAME(KEY_TWEN)
    KEY_NAME(KEY_VIDEOPHONE)
    KEY_NAME(KEY_GAMES)
    KEY_NAME(KEY_ZOOMIN)
    KEY_NAME(KEY_ZOOMOUT)
    KEY_NAME(KEY_ZOOMRESET)
    KEY_NAME(KEY_WORDPROCESSOR)
    KEY_NAME(KEY_EDITOR)
    KEY_NAME(KEY_SPREADSHEET)
    KEY_NAME(KEY_GRAPHICSEDITOR)
    KEY_NAME(KEY_PRESENTATION)
    KEY_NAME(KEY_DATABASE)
    KEY_NAME(KEY_NEWS)
    KEY_NAME(KEY_VOICEMAIL)
    KEY_NAME(KEY_ADDRESSBOOK)
    KEY_NAME(KEY_MESSENGER)
    KEY_NAME(KEY_DISPLAYTOGGLE)
    KEY_NAME(KEY_BRIGHTNESS_TOGGLE)
    KEY_NAME(KEY_SPELLCHECK)
    KEY_NAME(KEY_LOGOFF)
    KEY_NAME(KEY_DOLLAR)
    KEY_NAME(KEY_EURO)
    KEY_NAME(KEY_FRAMEBACK)
    KEY_NAME(KEY_FRAMEFORWARD)
    KEY_NAME(KEY_CONTEXT_MENU)
    KEY_NAME(KEY_MEDIA_REPEAT)
    KEY_NAME(KEY_10CHANNELSUP)
    KEY_NAME(KEY_10CHANNELSDOWN)
    KEY_NAME(KEY_IMAGES)
    KEY_NAME(KEY_NOTIFICATION_CENTER)
    KEY_NAME(KEY_PICKUP_PHONE)
    KEY_NAME(KEY_HANGUP_PHONE)
    KEY_NAME(KEY_LINK_PHONE)
    KEY_NAME(KEY_DEL_EOL)
    KEY_NAME(KEY_DEL_EOS)
    KEY_NAME(KEY_INS_LINE)
    KEY_NAME(KEY_DEL_LINE)
    KEY_NAME(KEY_FN)
    KEY_NAME(KEY_FN_ESC)
    KEY_NAME(KEY_FN_F1)
    KEY_NAME(KEY_FN_F2)
    KEY_NAME(KEY_FN_F3)
    KEY_NAME(KEY_FN_F4)
    KEY_NAME(KEY_FN_F5)
    KEY_NAME(KEY_FN_F6)
    KEY_NAME(KEY_FN_F7)
    KEY_NAME(KEY_FN_F8)
    KEY_NAME(KEY_FN_F9)
    KEY_NAME(KEY_FN_F10)
    KEY_NAME(KEY_FN_F11)
    KEY_NAME(KEY_FN_F12)
    KEY_NAME(KEY_FN_1)
    KEY_NAME(KEY_FN_2)
    KEY_NAME(KEY_FN_D)
    KEY_NAME(KEY_FN_E)
    KEY_NAME(KEY_FN_F)
    KEY_NAME(KEY_FN_S)
    KEY_NAME(KEY_FN_B)
    KEY_NAME(KEY_FN_RIGHT_SHIFT)
    KEY_NAME(KEY_BRL_DOT1)
    KEY_NAME(KEY_BRL_DOT2)
    KEY_NAME(KEY_BRL_DOT3)
    KEY_NAME(KEY_BRL_DOT4)
    KEY_NAME(KEY_BRL_DOT5)
    KEY_NAME(KEY_BRL_DOT6)
    KEY_NAME(KEY_BRL_DOT7)
    KEY_NAME(KEY_BRL_DOT8)
    KEY_NAME(KEY_BRL_DOT9)
    KEY_NAME(KEY_BRL_DOT10)
    KEY_NAME(KEY_NUMERIC_0)
    KEY_NAME(KEY_NUMERIC_1)
    KEY_NAME(KEY_NUMERIC_2)
    KEY_NAME(KEY_NUMERIC_3)
    KEY_NAME(KEY_NUMERIC_4)
    KEY_NAME(KEY_NUMERIC_5)
    KEY_NAME(KEY_NUMERIC_6)
    KEY_NAME(KEY_NUMERIC_7)
    KEY_NAME(KEY_NUMERIC_8)
    KEY_NAME(KEY_NUMERIC_9)
    KEY_NAME(KEY_NUMERIC_STAR)
    KEY_NAME(KEY_NUMERIC_POUND)
    KEY_NAME(KEY_NUMERIC_A)
    KEY_NAME(KEY_NUMERIC_B)
    KEY_NAME(KEY_NUMERIC_C)
    KEY_NAME(KEY_NUMERIC_D)
    KEY_NAME(KEY_CAMERA_FOCUS)
    KEY_NAME(KEY_WPS_BUTTON)
    KEY_NAME(KEY_TOUCHPAD_TOGGLE)
    KEY_NAME(KEY_TOUCHPAD_ON)
    KEY_NAME(KEY_TOUCHPAD_OFF)
    KEY_NAME(KEY_CAMERA_ZOOMIN)
    KEY_NAME(KEY_CAMERA_ZOOMOUT)
    KEY_NAME(KEY_CAMERA_UP)
    KEY_NAME(KEY_CAMERA_DOWN)
    KEY_NAME(KEY_CAMERA_LEFT)
    KEY_NAME(KEY_CAMERA_RIGHT)
    KEY_NAME(KEY_ATTENDANT_ON)
    KEY_NAME(KEY_ATTENDANT_OFF)
    KEY_NAME(KEY_ATTENDANT_TOGGLE)
    KEY_NAME(KEY_LIGHTS_TOGGLE)
    KEY_NAME(KEY_ALS_TOGGLE)
    KEY_NAME(KEY_ROTATE_LOCK_TOGGLE)
    KEY_NAME(KEY_REFRESH_RATE_TOGGLE)
    KEY_NAME(KEY_BUTTONCONFIG)
    KEY_NAME(KEY_TASKMANAGER)
    KEY_NAME(KEY_JOURNAL)
    KEY_NAME(KEY_CONTROLPANEL)
    KEY_NAME(KEY_APPSELECT)
    KEY_NAME(KEY_SCREENSAVER)
    KEY_NAME(KEY_VOICECOMMAND)
    KEY_NAME(KEY_ASSISTANT)
    KEY_NAME(KEY_KBD_LAYOUT_NEXT)
    KEY_NAME(KEY_EMOJI_PICKER)
    KEY_NAME(KEY_DICTATE)
    KEY_NAME(KEY_CAMERA_ACCESS_ENABLE)
    KEY_NAME(KEY_CAMERA_ACCESS_DISABLE)
    KEY_NAME(KEY_CAMERA_ACCESS_TOGGLE)
    KEY_NAME(KEY_ACCESSIBILITY)
    KEY_NAME(KEY_DO_NOT_DISTURB)
    KEY_NAME(KEY_BRIGHTNESS_MIN)
    KEY_NAME(KEY_BRIGHTNESS_MAX)
    KEY_NAME(KEY_KBDINPUTASSIST_PREV)
    KEY_NAME(KEY_KBDINPUTASSIST_NEXT)
    KEY_NAME(KEY_KBDINPUTASSIST_PREVGROUP)
    KEY_NAME(KEY_KBDINPUTASSIST_NEXTGROUP)
    KEY_NAME(KEY_KBDINPUTASSIST_ACCEPT)
    KEY_NAME(KEY_KBDINPUTASSIST_CANCEL)
    KEY_NAME(KEY_RIGHT_UP)
    KEY_NAME(KEY_RIGHT_DOWN)
    KEY_NAME(KEY_LEFT_UP)
    KEY_NAME(KEY_LEFT_DOWN)
    KEY_NAME(KEY_ROOT_MENU)
    KEY_NAME(KEY_MEDIA_TOP_MENU)
    KEY_NAME(KEY_NUMERIC_11)
    KEY_NAME(KEY_NUMERIC_12)
    KEY_NAME(KEY_AUDIO_DESC)
    KEY_NAME(KEY_3D_MODE)
    KEY_NAME(KEY_NEXT_FAVORITE)
    KEY_NAME(KEY_STOP_RECORD)
    KEY_NAME(KEY_PAUSE_RECORD)
    KEY_NAME(KEY_VOD)
    KEY_NAME(KEY_UNMUTE)
    KEY_NAME(KEY_FASTREVERSE)
    KEY_NAME(KEY_SLOWREVERSE)
    KEY_NAME(KEY_DATA)
    KEY_NAME(KEY_ONSCREEN_KEYBOARD)
    KEY_NAME(KEY_PRIVACY_SCREEN_TOGGLE)
    KEY_NAME(KEY_SELECTIVE_SCREENSHOT)
    KEY_NAME(KEY_NEXT_ELEMENT)
    KEY_NAME(KEY_PREVIOUS_ELEMENT)
    KEY_NAME(KEY_AUTOPILOT_ENGAGE_TOGGLE)
    KEY_NAME(KEY_MARK_WAYPOINT)
    KEY_NAME(KEY_SOS)
    KEY_NAME(KEY_NAV_CHART)
    KEY_NAME(KEY_FISHING_CHART)
    KEY_NAME(KEY_SINGLE_RANGE_RADAR)
    KEY_NAME(KEY_DUAL_RANGE_RADAR)
    KEY_NAME(KEY_RADAR_OVERLAY)
    KEY_NAME(KEY_TRADITIONAL_SONAR)
    KEY_NAME(KEY_CLEARVU_SONAR)
    KEY_NAME(KEY_SIDEVU_SONAR)
    KEY_NAME(KEY_NAV_INFO)
    KEY_NAME(KEY_BRIGHTNESS_MENU)
    KEY_NAME(KEY_MACRO1)
    KEY_NAME(KEY_MACRO2)
    KEY_NAME(KEY_MACRO3)
    KEY_NAME(KEY_MACRO4)
    KEY_NAME(KEY_MACRO5)
    KEY_NAME(KEY_MACRO6)
    KEY_NAME(KEY_MACRO7)
    KEY_NAME(KEY_MACRO8)
    KEY_NAME(KEY_MACRO9)
    KEY_NAME(KEY_MACRO10)
    KEY_NAME(KEY_MACRO11)
    KEY_NAME(KEY_MACRO12)
    KEY_NAME(KEY_MACRO13)
    KEY_NAME(KEY_MACRO14)
    KEY_NAME(KEY_MACRO15)
    KEY_NAME(KEY_MACRO16)
    KEY_NAME(KEY_MACRO17)
    KEY_NAME(KEY_MACRO18)
    KEY_NAME(KEY_MACRO19)
    KEY_NAME(KEY_MACRO20)
    KEY_NAME(KEY_MACRO21)
    KEY_NAME(KEY_MACRO22)
    KEY_NAME(KEY_MACRO23)
    KEY_NAME(KEY_MACRO24)
    KEY_NAME(KEY_MACRO25)
    KEY_NAME(KEY_MACRO26)
    KEY_NAME(KEY_MACRO27)
    KEY_NAME(KEY_MACRO28)
    KEY_NAME(KEY_MACRO29)
    KEY_NAME(KEY_MACRO30)
    KEY_NAME(KEY_MACRO_RECORD_START)
    KEY_NAME(KEY_MACRO_RECORD_STOP)
    KEY_NAME(KEY_MACRO_PRESET_CYCLE)
    KEY_NAME(KEY_MACRO_PRESET1)
    KEY_NAME(KEY_MACRO_PRESET2)
    KEY_NAME(KEY_MACRO_PRESET3)
    KEY_NAME(KEY_KBD_LCD_MENU1)
    KEY_NAME(KEY_KBD_LCD_MENU2)
    KEY_NAME(KEY_KBD_LCD_MENU3)
    KEY_NAME(KEY_KBD_LCD_MENU4)
    KEY_NAME(KEY_KBD_LCD_MENU5)
    KEY_NAME(KEY_MIN_INTERESTING)
    KEY_NAME(KEY_MAX)
    KEY_NAME(KEY_CNT)

    char *end;
    long code = strtol(value, &end, 10);

    if (*value != '\0' && *end == '\0' &&
        code >= 0 && code <= KEY_MAX)
    {
        return (int)code;
    }

    return -1;

#undef KEY_NAME
}
static bool is_recognized_option(const char *arg)
{
    return !strcmp(arg, "-v") || !strcmp(arg, "--verbose") ||
           !strcmp(arg, "-h") || !strcmp(arg, "--help") ||
           !strcmp(arg, "-l") || !strcmp(arg, "--list");
}

static char *normalize_device_name(const char *device)
{
    size_t len = strlen(device);
    bool quoted = len >= 2 &&
                  ((device[0] == '"' && device[len - 1] == '"') ||
                   (device[0] == '\'' && device[len - 1] == '\''));
    size_t start = quoted ? 1 : 0;
    size_t end = quoted ? len - 1 : len;

    char *normalized = malloc(end - start + 1);
    if (!normalized)
        return NULL;

    size_t out = 0;
    for (size_t in = start; in < end; in++)
    {
        if (device[in] == '\\' && in + 1 < end && device[in + 1] == ' ')
        {
            normalized[out++] = ' ';
            in++;
        }
        else
        {
            normalized[out++] = device[in];
        }
    }
    normalized[out] = '\0';
    return normalized;
}

static char *parse_device_arg(int argc, char **argv, int *index)
{
    int start = *index + 1;
    if (start >= argc || is_recognized_option(argv[start]))
        return NULL;

    if (!strncmp(argv[start], "/dev/input/", strlen("/dev/input/")))
    {
        *index = start;
        return strdup(argv[start]);
    }

    char quote = (argv[start][0] == '"' || argv[start][0] == '\'')
                     ? argv[start][0]
                     : '\0';
    int end = start + 1;
    size_t len = strlen(argv[start]);

    if (quote)
    {
        while (end < argc)
        {
            if (is_recognized_option(argv[end]))
                break;

            len += strlen(argv[end]) + 1;

            size_t token_len = strlen(argv[end]);
            if (token_len > 0 && argv[end][token_len - 1] == quote)
            {
                end++;
                break;
            }

            end++;
        }
    }

    char *joined = malloc(len + 1);
    if (!joined)
        return NULL;

    joined[0] = '\0';
    for (int i = start; i < end; i++)
    {
        if (i > start)
            strcat(joined, " ");
        strcat(joined, argv[i]);
    }

    char *device = normalize_device_name(joined);
    free(joined);
    if (device)
        *index = end - 1;
    return device;
}

void print_help(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -h, --help            Show this help message\n");
    printf("  -v, --verbose         Show non-critical diagnostic logging\n");
    printf("  -l, --list            List input devices with EV_ABS support\n");
    printf(
        "  -d, --device <arg>    Specify device by path or exact name\n");
    printf("                         Examples: %s -d \"Device Name With Spaces\"\n", prog);
    printf("                                   %s -d Device\\ Name\\ With\\ Spaces\n", prog);
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

static inline bool emit_abs_delta(int x, int y, bool x_dirty, bool y_dirty)
{
    struct input_event ev[3];
    int n = 0;

    if (x_dirty)
    {
        ev[n++] =
            (struct input_event){.type = EV_ABS, .code = ABS_X, .value = x};
    }

    if (y_dirty)
    {
        ev[n++] =
            (struct input_event){.type = EV_ABS, .code = ABS_Y, .value = y};
    }

    // Always terminate with SYN
    ev[n++] =
        (struct input_event){.type = EV_SYN, .code = SYN_REPORT, .value = 0};

    ssize_t wrote = write(tab_fd, ev, n * sizeof(struct input_event));
    if (wrote != (ssize_t)(n * sizeof(struct input_event)))
    {
        perror("write EV_ABS");
        return false;
    }
    return true;
}

int main(int argc, char *argv[])
{
    int exit_code = EXIT_FAILURE;
    bool grabbed = false;
    bool tosu_started = false;

    keyboard_device keyboards[MAX_KEYBOARDS];
    int keyboard_count = 0;
    int keyboard_inotify_fd = -1;
    int keyboard_inotify_watch = -1;

    char *dev_override = NULL;
    bool list_requested = false;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
        {
            verbose_logging = true;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            print_help(argv[0]);
            exit_code = EXIT_SUCCESS;
            goto cleanup;
        }
        else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list"))
        {
            list_requested = true;
        }
        else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device"))
        {
            free(dev_override);
            dev_override = parse_device_arg(argc, argv, &i);
        }
    }

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
                            .enable_tosu = 0,
                            .enable_hotkey = 0,
                            .hotkey_key = KEY_LEFTALT};

    char *config_path = get_abs_c_config_path();
    if (!config_path)
    {
        fprintf(stderr, "Could not load abs-c.ini; using default config.\n");
    }
    else
    {
        printf("Loading config from %s\n", config_path);
        if (ini_parse(config_path, handler, &config) < 0)
        {
            fprintf(stderr, "Could not load abs-c.ini; using default config.\n");
        }
        free(config_path);
    }

    if (config.enable_hotkey)
    {
        scan_keyboard_devices(keyboards, &keyboard_count, config.hotkey_key);

        keyboard_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (keyboard_inotify_fd < 0)
        {
            verbose_perror("inotify_init1");
        }
        else
        {
            keyboard_inotify_watch =
                inotify_add_watch(keyboard_inotify_fd,
                                  "/dev/input",
                                  IN_CREATE | IN_DELETE |
                                  IN_MOVED_TO | IN_MOVED_FROM);

            if (keyboard_inotify_watch < 0)
            {
                verbose_perror("inotify_add_watch /dev/input");
                close(keyboard_inotify_fd);
                keyboard_inotify_fd = -1;
            }
        }
    }

    if (list_requested)
    {
        list_devices();
        exit_code = EXIT_SUCCESS;
        goto cleanup;
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

    for (int i = 0; i < ndevs && !usable; i++)
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
            else if (dev_override[0] != '/' && strcmp(name, dev_override) != 0)
            {
                close(devfd);
                continue;
            }
            found = true;
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
        verbose_perror("sched_setscheduler");
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
    {
        verbose_perror("mlockall");
    }

    struct pollfd pfd[1 + 1 + MAX_KEYBOARDS];
    struct input_event ev_buf[64];

    int x = 0, y = 0;
    bool x_updated = false;
    bool y_updated = false;
    static int last_emitted_x = -1;
    static int last_emitted_y = -1;
    bool active = true;
    bool tosu_active = true;
    static bool pen_down = false;

    if (config.enable_tosu)
    {
        tosu_set_verbose(verbose_logging);
        tosu_init();
        tosu_started = true;
    }

    struct timespec ts_last;
    clock_gettime(CLOCK_MONOTONIC, &ts_last);

    printf("Press Ctrl-C to quit\n");

    while (!stop)
    {

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);

        long dt_ms = (ts_now.tv_sec - ts_last.tv_sec) * 1000 +
                     (ts_now.tv_nsec - ts_last.tv_nsec) / 1000000;

        if (config.enable_tosu && dt_ms >= 16)
        {
            tosu_active = tosu_get_absolute_state();
            ts_last = ts_now;
        }

        /* Tosu provides the base state. The hotkey override is applied
         * after keyboard events for this poll cycle have been processed. */
        active = tosu_active;

        int poll_count = 0;
        int inotify_index = -1;

        pfd[poll_count++] = (struct pollfd){
            .fd = fd,
            .events = POLLIN};

        if (config.enable_hotkey)
        {
            if (keyboard_inotify_fd >= 0)
            {
                inotify_index = poll_count;
                pfd[poll_count++] = (struct pollfd){
                    .fd = keyboard_inotify_fd,
                    .events = POLLIN};
            }

            for (int i = 0; i < keyboard_count; i++)
            {
                pfd[poll_count++] = (struct pollfd){
                    .fd = keyboards[i].fd,
                    .events = POLLIN};
            }
        }

        int poll_ret = poll(pfd, poll_count, INACTIVE_SLEEP_MS);
        if (poll_ret <= 0)
        {
            if (!active)
            {
                struct timespec ts = {.tv_sec = 0,
                                      .tv_nsec = INACTIVE_SLEEP_MS * 1000000L};
                nanosleep(&ts, NULL);
            }
            continue;
        }

        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            fprintf(stderr, "poll error on input device\n");
            break;
        }

        bool keyboard_rescan = false;

        if (config.enable_hotkey)
        {
            if (inotify_index >= 0 &&
                (pfd[inotify_index].revents & POLLIN))
            {
                char inotify_buf[4096];

                while (read(keyboard_inotify_fd,
                            inotify_buf,
                            sizeof(inotify_buf)) > 0)
                {
                    keyboard_rescan = true;
                }
            }

            for (int i = 0; i < keyboard_count; i++)
            {
                int pfd_index = (keyboard_inotify_fd >= 0 ? 2 : 1) + i;

                if (pfd_index >= poll_count)
                    break;

                if (pfd[pfd_index].revents &
                    (POLLERR | POLLHUP | POLLNVAL))
                {
                    keyboard_rescan = true;
                    continue;
                }

                if (!(pfd[pfd_index].revents & POLLIN))
                    continue;

                ssize_t keyboard_rd =
                    read(keyboards[i].fd, ev_buf, sizeof(ev_buf));

                if (keyboard_rd < 0)
                {
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
                        keyboard_rescan = true;

                    continue;
                }

                int keyboard_events =
                    keyboard_rd / sizeof(struct input_event);

                for (int j = 0; j < keyboard_events; j++)
                {
                    struct input_event *keyboard_ev = &ev_buf[j];

                    if (keyboard_ev->type != EV_KEY ||
                        keyboard_ev->code != config.hotkey_key)
                    {
                        continue;
                    }

                    if (keyboard_ev->value == 1 ||
                        keyboard_ev->value == 2)
                    {
                        keyboards[i].hotkey_down = true;
                    }
                    else if (keyboard_ev->value == 0)
                    {
                        keyboards[i].hotkey_down = false;
                    }
                }
            }

            if (keyboard_rescan)
            {
                scan_keyboard_devices(keyboards,
                                      &keyboard_count,
                                      config.hotkey_key);
            }

            active = tosu_active;

            if (any_keyboard_hotkey_down(keyboards, keyboard_count))
                active = false;
        }

        /*
         * Grab/ungrab only the selected absolute input device.
         * Keyboard devices are intentionally never passed to set_grab().
         */
        set_grab(fd, &grabbed, active);
        ssize_t rd = read(fd, ev_buf, sizeof(ev_buf));
        if (rd < 0)
        {
            if (errno == EINTR && stop)
                break;
            perror("read input_event");
            continue;
        }

        int nevents = rd / sizeof(struct input_event);

        for (int i = 0; i < nevents; i++)
        {

            struct input_event *ev = &ev_buf[i];

            /* HARD DISABLE GATE:
               If inactive, we still consume events but DO NOT update state */
            if (!active)
            {
                continue;
            }

            if (ev->type == EV_KEY)
            {

                if (ev->code == BTN_TOUCH)
                {
                    if (ev->value && !pen_down)
                    {

                        struct input_event out = {0};

                        out.type = EV_KEY;
                        out.code = BTN_TOOL_PEN;
                        out.value = 1;
                        write(tab_fd, &out, sizeof(out));

                        out.type = EV_KEY;
                        out.code = BTN_TOUCH;
                        out.value = 1;
                        write(tab_fd, &out, sizeof(out));

                        out.type = EV_SYN;
                        out.code = SYN_REPORT;
                        out.value = 0;
                        write(tab_fd, &out, sizeof(out));

                        pen_down = true;
                    }

                    else if (!ev->value && pen_down)
                    {

                        struct input_event out = {0};

                        out.type = EV_KEY;
                        out.code = BTN_TOUCH;
                        out.value = 0;
                        write(tab_fd, &out, sizeof(out));

                        out.type = EV_KEY;
                        out.code = BTN_TOOL_PEN;
                        out.value = 0;
                        write(tab_fd, &out, sizeof(out));

                        out.type = EV_SYN;
                        out.code = SYN_REPORT;
                        out.value = 0;
                        write(tab_fd, &out, sizeof(out));

                        pen_down = false;
                    }
                }
            }

            else if (ev->type == EV_ABS)
            {
                switch (ev->code)
                {
                case ABS_X:
                    x = ev->value;
                    x_updated = true;
                    break;

                case ABS_Y:
                    y = ev->value;
                    y_updated = true;
                    break;

                default:
                    break;
                }
            }

            else if (ev->type == EV_SYN && ev->code == SYN_REPORT)
            {

                if (!pen_down)
                    continue;

                if (x_updated || y_updated)
                {

                    if (x != last_emitted_x || y != last_emitted_y)
                    {
                        emit_abs_delta(x, y, true, true);
                        last_emitted_x = x;
                        last_emitted_y = y;
                    }

                    x_updated = false;
                    y_updated = false;
                }
            }
        }

        /* HARD RESET WHEN INACTIVE:
           kills ghost state + prevents late emissions */
        if (!active)
        {
            pen_down = false;
            x_updated = false;
            y_updated = false;
        }
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    close_keyboard_devices(keyboards, &keyboard_count);

    if (keyboard_inotify_watch >= 0 && keyboard_inotify_fd >= 0)
    {
        if (inotify_rm_watch(keyboard_inotify_fd,
                             keyboard_inotify_watch) < 0)
        {
            verbose_perror("inotify_rm_watch");
        }
        keyboard_inotify_watch = -1;
    }

    if (keyboard_inotify_fd >= 0)
    {
        close(keyboard_inotify_fd);
        keyboard_inotify_fd = -1;
    }

    if (grabbed && fd >= 0 && ioctl(fd, EVIOCGRAB, 0) < 0)
    {
        verbose_perror("ioctl EVIOCGRAB release");
    }
    if (tosu_started)
    {
        tosu_shutdown();
    }
    if (tab_fd >= 0)
    {
        if (ioctl(tab_fd, UI_DEV_DESTROY) < 0)
        {
            verbose_perror("ioctl UI_DEV_DESTROY");
        }
        close(tab_fd);
        tab_fd = -1;
    }
    if (fd >= 0)
    {
        close(fd);
        fd = -1;
    }
    free(dev_override);
    fprintf(stderr, "Exiting with status %d\n", exit_code);
    return exit_code;
}
