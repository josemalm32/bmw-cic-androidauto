/* nbt_carplay: the iDrive controller and the steering wheel keys to CarPlay.
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Valentin Radu
 *
 * nbt_knob writes one line per event into input.fifo, as for Android Auto: "R 65536 <delta>" (turn) and
 * "K <Android key code> <1 down | 0 up>". Here they become HID reports over AirPlay's event connection:
 *   turn -> knob wheel (one report per step, then a rest report); NBT_KNOB_INVERT / --knob-invert turns it around
 *   tilt up / down / left / right (19-22) -> knob y -1 / +1, x -1 / +1 (a nudge: report, then rest)
 *   push (23) -> knob select held while pressed; BACK (4) -> knob back; HOME (3) -> knob home
 *   media keys: PLAY_PAUSE 85, PLAY 126, PAUSE 127, NEXT 87, PREVIOUS 88 -> the media HID (1 play, 2 pause,
 *   3 play/pause, 4 next, 5 previous; then 0)
 *   voice (84, the long press of the steering wheel button) -> Siri
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cp.h"

static int g_select, g_back, g_home;

static void knob(int x, int y, int wheel)
{
    unsigned char r[4];
    r[0] = (unsigned char)((g_select ? 1 : 0) | (g_home ? 2 : 0) | (g_back ? 4 : 0));
    r[1] = (unsigned char)(signed char)x;
    r[2] = (unsigned char)(signed char)y;
    r[3] = (unsigned char)(signed char)wheel;
    airplay_hid(HID_KNOB, r, 4);
}

static void media(unsigned code)
{
    unsigned char r = (unsigned char)code;
    airplay_hid(HID_MEDIA, &r, 1);
    r = 0;
    airplay_hid(HID_MEDIA, &r, 1);
}

static void line(const char* l)
{
    unsigned code;
    int value;
    if (sscanf(l, "R %u %d", &code, &value) == 2) {
        if (g_opt.knob_invert)
            value = -value;
        while (value) {
            const int step = value > 127 ? 127 : value < -127 ? -127 : value;
            knob(0, 0, step);
            value -= step;
        }
        knob(0, 0, 0);
        return;
    }
    if (sscanf(l, "K %u %d", &code, &value) != 2)
        return;
    switch (code) {
    case 19:
    case 20:
    case 21:
    case 22:
        if (value) {
            knob(code == 21 ? -1 : code == 22 ? 1 : 0, code == 19 ? -1 : code == 20 ? 1 : 0, 0);
            knob(0, 0, 0);
        }
        break;
    case 23:
        g_select = value != 0;
        knob(0, 0, 0);
        break;
    case 4:
        g_back = value != 0;
        knob(0, 0, 0);
        break;
    case 3:
        g_home = value != 0;
        knob(0, 0, 0);
        break;
    case 85:
        if (value)
            media(3);
        break;
    case 126:
        if (value)
            media(1);
        break;
    case 127:
        if (value)
            media(2);
        break;
    case 87:
        if (value)
            media(4);
        break;
    case 88:
        if (value)
            media(5);
        break;
    case 84:
        if (value)
            airplay_siri();
        break;
    default:
        break;
    }
}

static void* input_thread(void* arg)
{
    char buf[512];
    unsigned n = 0;
    int fd;
    (void)arg;
    /* read and write: the FIFO never reports end of file while nbt_knob is restarted */
    fd = open(g_opt.input, O_RDWR);
    if (fd < 0) {
        say("input: %s: %s (no knob)", g_opt.input, strerror(errno));
        return 0;
    }
    say("input: key lines from %s", g_opt.input);
    while (!g_stop) {
        ssize_t r;
        if (n >= sizeof(buf) - 1)   /* a line longer than the buffer: dropped */
            n = 0;
        r = read(fd, buf + n, sizeof(buf) - 1 - n);
        char *p, *e;
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (unsigned)r;
        buf[n] = 0;
        p = buf;
        while ((e = strchr(p, '\n')) != 0) {
            *e = 0;
            if (airplay_connected())
                line(p);
            p = e + 1;
        }
        n = (unsigned)(buf + n - p);
        memmove(buf, p, n);
    }
    close(fd);
    return 0;
}

void input_start(void)
{
    pthread_t t;
    if (!g_opt.input || !g_opt.input[0])
        return;
    if (!pthread_create(&t, 0, input_thread, 0))
        pthread_detach(t);
}
