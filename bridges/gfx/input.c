#include "gfx_internal.h"
#include <stdlib.h>
#include <string.h>

/* Set-1 scancodes agree with Linux evdev for the ordinary PC keyboard block. */
static const uint32_t base[248] = {
    [1]=0xff1b, [2]='1', [3]='2', [4]='3', [5]='4', [6]='5', [7]='6', [8]='7', [9]='8', [10]='9', [11]='0',
    [12]='-', [13]='=', [14]=0xff08, [15]=0xff09, [16]='q', [17]='w', [18]='e', [19]='r', [20]='t',
    [21]='y', [22]='u', [23]='i', [24]='o', [25]='p', [26]='[', [27]=']', [28]=0xff0d, [29]=0xffe3,
    [30]='a', [31]='s', [32]='d', [33]='f', [34]='g', [35]='h', [36]='j', [37]='k', [38]='l', [39]=';',
    [40]='\'', [41]='`', [42]=0xffe1, [43]='\\', [44]='z', [45]='x', [46]='c', [47]='v', [48]='b',
    [49]='n', [50]='m', [51]=',', [52]='.', [53]='/', [54]=0xffe2, [55]=0xffaa, [56]=0xffe9, [57]=' ',
    [58]=0xffe5, [59]=0xffbe, [60]=0xffbf, [61]=0xffc0, [62]=0xffc1, [63]=0xffc2, [64]=0xffc3,
    [65]=0xffc4, [66]=0xffc5, [67]=0xffc6, [68]=0xffc7, [69]=0xff7f, [70]=0xff14,
    [71]=0xffb7, [72]=0xffb8, [73]=0xffb9, [74]=0xffad, [75]=0xffb4, [76]=0xffb5, [77]=0xffb6,
    [78]=0xffab, [79]=0xffb1, [80]=0xffb2, [81]=0xffb3, [82]=0xffb0, [83]=0xffae, [86]='<',
    [87]=0xffc8, [88]=0xffc9, [96]=0xff8d, [97]=0xffe4, [98]=0xffaf, [99]=0xff61, [100]=0xffea,
    [102]=0xff50, [103]=0xff52, [104]=0xff55, [105]=0xff51, [106]=0xff53, [107]=0xff57,
    [108]=0xff54, [109]=0xff56, [110]=0xff63, [111]=0xffff, [119]=0xff13,
    [125]=0xffeb, [126]=0xffec, [127]=0xff67,
    [183]=0xffca, [184]=0xffcb, [185]=0xffcc, [186]=0xffcd, [187]=0xffce, [188]=0xffcf,
    [189]=0xffd0, [190]=0xffd1, [191]=0xffd2, [192]=0xffd3, [193]=0xffd4, [194]=0xffd5
};
static const uint32_t shifted[248] = {
    [2]='!', [3]='@', [4]='#', [5]='$', [6]='%', [7]='^', [8]='&', [9]='*', [10]='(', [11]=')',
    [12]='_', [13]='+', [26]='{', [27]='}', [39]=':', [40]='"', [41]='~', [43]='|', [51]='<', [52]='>', [53]='?', [86]='>'
};
static unsigned char keymap[32];
static const struct { const char *name; uint32_t sym; } names[] = {
    {"BackSpace",0xff08}, {"Tab",0xff09}, {"Linefeed",0xff0a}, {"Clear",0xff0b}, {"Return",0xff0d},
    {"Pause",0xff13}, {"Scroll_Lock",0xff14}, {"Sys_Req",0xff15}, {"Escape",0xff1b}, {"Delete",0xffff},
    {"Home",0xff50}, {"Left",0xff51}, {"Up",0xff52}, {"Right",0xff53}, {"Down",0xff54},
    {"Prior",0xff55}, {"Page_Up",0xff55}, {"Next",0xff56}, {"Page_Down",0xff56}, {"End",0xff57},
    {"Begin",0xff58}, {"Print",0xff61}, {"Insert",0xff63}, {"Menu",0xff67}, {"Help",0xff6a},
    {"Num_Lock",0xff7f}, {"KP_Space",0xff80}, {"KP_Tab",0xff89}, {"KP_Enter",0xff8d},
    {"KP_Home",0xff95}, {"KP_Left",0xff96}, {"KP_Up",0xff97}, {"KP_Right",0xff98}, {"KP_Down",0xff99},
    {"KP_Prior",0xff9a}, {"KP_Page_Up",0xff9a}, {"KP_Next",0xff9b}, {"KP_Page_Down",0xff9b},
    {"KP_End",0xff9c}, {"KP_Begin",0xff9d}, {"KP_Insert",0xff9e}, {"KP_Delete",0xff9f},
    {"KP_Multiply",0xffaa}, {"KP_Add",0xffab}, {"KP_Separator",0xffac}, {"KP_Subtract",0xffad},
    {"KP_Decimal",0xffae}, {"KP_Divide",0xffaf}, {"KP_Equal",0xffbd},
    {"Shift_L",0xffe1}, {"Shift_R",0xffe2}, {"Control_L",0xffe3}, {"Control_R",0xffe4},
    {"Caps_Lock",0xffe5}, {"Shift_Lock",0xffe6}, {"Meta_L",0xffe7}, {"Meta_R",0xffe8},
    {"Alt_L",0xffe9}, {"Alt_R",0xffea}, {"Super_L",0xffeb}, {"Super_R",0xffec}, {"ISO_Left_Tab",0xfe20},
    {"space",' '}, {"exclam",'!'}, {"quotedbl",'"'}, {"numbersign",'#'}, {"dollar",'$'}, {"percent",'%'},
    {"ampersand",'&'}, {"apostrophe",'\''}, {"quoteright",'\''}, {"parenleft",'('}, {"parenright",')'},
    {"asterisk",'*'}, {"plus",'+'}, {"comma",','}, {"minus",'-'}, {"period",'.'}, {"slash",'/'},
    {"colon",':'}, {"semicolon",';'}, {"less",'<'}, {"equal",'='}, {"greater",'>'}, {"question",'?'},
    {"at",'@'}, {"bracketleft",'['}, {"backslash",'\\'}, {"bracketright",']'}, {"asciicircum",'^'},
    {"underscore",'_'}, {"grave",'`'}, {"quoteleft",'`'}, {"braceleft",'{'}, {"bar",'|'},
    {"braceright",'}'}, {"asciitilde",'~'}
};

unsigned gfx_evdev(WPARAM vk, LPARAM scan_bits)
{
    unsigned scan = ((uint32_t)scan_bits >> 16) & 255;
    if (vk == VK_PAUSE) return 119;
    if (vk == VK_SNAPSHOT) return 99;
    if (vk == VK_NUMLOCK) return 69;
    if (vk >= VK_F13 && vk <= VK_F24) return 183 + (unsigned)(vk - VK_F13);
    if ((uint32_t)scan_bits & (1u << 24)) {
        switch (scan) {
        case 0x1c: return 96; case 0x1d: return 97; case 0x35: return 98; case 0x37: return 99;
        case 0x38: return 100; case 0x47: return 102; case 0x48: return 103; case 0x49: return 104;
        case 0x4b: return 105; case 0x4d: return 106; case 0x4f: return 107; case 0x50: return 108;
        case 0x51: return 109; case 0x52: return 110; case 0x53: return 111;
        case 0x5b: return 125; case 0x5c: return 126; case 0x5d: return 127;
        default: return 0;
        }
    }
    return scan < 89 ? scan : 0;
}
int gfx_key_down(unsigned evdev)
{
    unsigned code = evdev + 8;
    return evdev && code < 256 && (keymap[code >> 3] & (1u << (code & 7))) != 0;
}
void gfx_set_key(unsigned evdev, int pressed)
{
    unsigned code = evdev + 8;
    if (!evdev || code >= 256) return;
    if (pressed) keymap[code >> 3] |= (unsigned char)(1u << (code & 7));
    else keymap[code >> 3] &= (unsigned char)~(1u << (code & 7));
}
uint32_t gfx_modifiers(void)
{
    uint32_t state = 0;
    if (gfx_key_down(42) || gfx_key_down(54)) state |= 1;
    if (GetKeyState(VK_CAPITAL) & 1) state |= 2;
    if (gfx_key_down(29) || gfx_key_down(97)) state |= 4;
    if (gfx_key_down(56) || gfx_key_down(100)) state |= 8;
    if (GetKeyState(VK_NUMLOCK) & 1) state |= 16;
    if (gfx_key_down(125) || gfx_key_down(126)) state |= 64;
    return state;
}
uint32_t lx_XStringToKeysym(const char *name)
{
    if (!name || !*name) return 0;
    if (!name[1]) return (unsigned char)*name;
    for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
        if (!strcmp(name, names[i].name)) return names[i].sym;
    char *end;
    if (name[0] == 'F') {
        unsigned long n = strtoul(name + 1, &end, 10);
        if (!*end && n >= 1 && n <= 35) return 0xffbd + n;
    }
    if (!strncmp(name, "KP_", 3) && name[3] >= '0' && name[3] <= '9' && !name[4]) return 0xffb0 + name[3] - '0';
    if (name[0] == '0' && name[1] == 'x') {
        unsigned long n = strtoul(name + 2, &end, 16);
        if (end != name + 2 && !*end) return n;
    }
    if (name[0] == 'U') {
        unsigned long n = strtoul(name + 1, &end, 16);
        if (end != name + 1 && !*end && n <= 0x10ffff && !(n >= 0xd800 && n <= 0xdfff)) return 0x01000000 | n;
    }
    return 0;
}
unsigned lx_XKeysymToKeycode(LXDisplay *d, uint32_t sym)
{
    (void)d;
    if (!sym) return 0;
    if (sym >= 'A' && sym <= 'Z') sym += 'a' - 'A';
    if (sym == 0xfe20) sym = 0xff09;
    if (sym == 0xffe7) sym = 0xffeb;
    if (sym == 0xffe8) sym = 0xffec;
    static const uint32_t kp_nav[] = {0xff9e,0xff9c,0xff99,0xff9b,0xff96,0xff9d,0xff98,0xff95,0xff97,0xff9a};
    for (unsigned i = 0; i < 10; ++i) if (sym == kp_nav[i]) sym = 0xffb0 + i;
    if (sym == 0xff9f) sym = 0xffae;
    for (unsigned i = 1; i < 248; ++i)
        if (base[i] == sym || shifted[i] == sym) return i + 8;
    return 0;
}
int lx_XQueryKeymap(LXDisplay *d, char bitmap[32])
{
    if (!d || !bitmap) return 0;
    gfx_pump();
    memcpy(bitmap, keymap, sizeof(keymap));
    return 1;
}
int lx_XLookupString(const LXKeyEvent *event, char *buffer, int capacity, uint32_t *keysym, void *compose)
{
    (void)compose;
    uint32_t sym = 0;
    if (event && event->keycode >= 8 && event->keycode < 256) {
        unsigned ev = event->keycode - 8;
        sym = base[ev];
        if (sym >= 'a' && sym <= 'z') {
            if (!!(event->state & 1) != !!(event->state & 2)) sym -= 'a' - 'A';
        } else if ((event->state & 1) && shifted[ev]) sym = shifted[ev];
        if (!(event->state & 16)) {
            static const uint32_t nav[] = {0xff9e,0xff9c,0xff99,0xff9b,0xff96,0xff9d,0xff98,0xff95,0xff97,0xff9a};
            if (sym >= 0xffb0 && sym <= 0xffb9) sym = nav[sym - 0xffb0];
            else if (sym == 0xffae) sym = 0xff9f;
        }
    }
    if (keysym) *keysym = sym;
    int c = -1;
    if (sym && sym < 256) c = (int)sym;
    else if (sym >= 0xffb0 && sym <= 0xffb9) c = '0' + (int)(sym - 0xffb0);
    else if (sym == 0xff08) c = '\b'; else if (sym == 0xff09) c = '\t';
    else if (sym == 0xff0d || sym == 0xff8d) c = '\r'; else if (sym == 0xff1b) c = 27;
    else if (sym == 0xffff) c = 127; else if (sym == 0xffae) c = '.';
    else if (sym == 0xffaa) c = '*'; else if (sym == 0xffab) c = '+';
    else if (sym == 0xffad) c = '-'; else if (sym == 0xffaf) c = '/';
    if (event && (event->state & 4) && c >= '@' && c <= 127) c &= 31;
    if (buffer && capacity > 0 && c >= 0) { buffer[0] = (char)c; return 1; }
    return 0;
}
