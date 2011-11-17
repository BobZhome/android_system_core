/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#ifdef USE_SCREENCAP
#include <errno.h>
#include <hardware/hardware.h>

#include "sysdeps.h"
#endif
#include "fdevent.h"
#include "adb.h"

#ifndef USE_SCREENCAP
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

/* TODO:
** - sync with vsync to avoid tearing
*/
/* This version number defines the format of the fbinfo struct.
   It must match versioning in ddms where this data is consumed. */
#define DDMS_RAWIMAGE_VERSION 1
struct fbinfo {
    unsigned int version;
    unsigned int bpp;
    unsigned int size;
    unsigned int width;
    unsigned int height;
    unsigned int red_offset;
    unsigned int red_length;
    unsigned int blue_offset;
    unsigned int blue_length;
    unsigned int green_offset;
    unsigned int green_length;
    unsigned int alpha_offset;
    unsigned int alpha_length;
} __attribute__((packed));

#ifdef USE_SCREENCAP
static int fill_format(struct fbinfo* info, int format)
{
    // bpp, red, green, blue, alpha

    static const int format_map[][9] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0},   // INVALID
        {32, 0, 8, 8, 8, 16, 8, 24, 8}, // HAL_PIXEL_FORMAT_RGBA_8888
        {32, 0, 8, 8, 8, 16, 8, 0, 0}, // HAL_PIXEL_FORMAT_RGBX_8888
        {24, 16, 8, 8, 8, 0, 8, 0, 0},  // HAL_PIXEL_FORMAT_RGB_888
        {16, 11, 5, 5, 6, 0, 5, 0, 0},  // HAL_PIXEL_FORMAT_RGB_565
        {32, 16, 8, 8, 8, 0, 8, 24, 8}, // HAL_PIXEL_FORMAT_BGRA_8888
        {16, 11, 5, 6, 5, 1, 5, 0, 1},  // HAL_PIXEL_FORMAT_RGBA_5551
        {16, 12, 4, 8, 4, 4, 4, 0, 4}   // HAL_PIXEL_FORMAT_RGBA_4444
    };
    const int *p;

    if (format == 0 || format > HAL_PIXEL_FORMAT_RGBA_4444)
        return -ENOTSUP;

    p = format_map[format];

    info->bpp = *(p++);
    info->red_offset = *(p++);
    info->red_length = *(p++);
    info->green_offset = *(p++);
    info->green_length = *(p++);
    info->blue_offset = *(p++);
    info->blue_length = *(p++);
    info->alpha_offset = *(p++);
    info->alpha_length = *(p++);

    return 0;
}

void framebuffer_service(int fd, void *cookie)
{
    char x[512];

    struct fbinfo fbinfo;
    int w, h, f;
    int s[2];
    int fb;
    unsigned int i;
    int rv;

    if(adb_socketpair(s)) {
        printf("cannot create service socket pair\n");
        goto done;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("- fork failed: %s -\n", strerror(errno));
        goto done;
    }

    if (pid == 0) {
        dup2(s[1], STDOUT_FILENO);
        const char* cmd_path = "/system/bin/screencap";
        const char* cmd = "screencap";
        execl(cmd_path, cmd, NULL);
        exit(1);
    }

    fb = s[0];

    /* read w, h, f */
    if (readx(fb, &w, 4)) goto done;
    if (readx(fb, &h, 4)) goto done;
    if (readx(fb, &f, 4)) goto done;

    fbinfo.version = DDMS_RAWIMAGE_VERSION;
    fbinfo.size = w * h * 4;
    fbinfo.width = w;
    fbinfo.height = h;
    rv = fill_format(&fbinfo, f);
    if (rv != 0) goto done;

    if(writex(fd, &fbinfo, sizeof(fbinfo))) goto done;

    for(i = 0; i < fbinfo.size; i += sizeof(x)) {
        if(readx(fb, &x, sizeof(x))) goto done;
        if(writex(fd, &x, sizeof(x))) goto done;
    }

    if(readx(fb, &x, fbinfo.size % sizeof(x))) goto done;
    if(writex(fd, &x, fbinfo.size % sizeof(x))) goto done;

done:
    adb_close(s[0]);
    adb_close(s[1]);
    adb_close(fd);
}
#else
void framebuffer_service(int fd, void *cookie)
{
    struct fbinfo fbinfo;
    unsigned int i;
    char buf[640];
    int fd_screencap;
    int w, h, f;
    int fds[2];

    if (pipe(fds) < 0) goto done;

    pid_t pid = fork();
    if (pid < 0) goto done;

    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        const char* command = "screencap";
        const char *args[2] = {command, NULL};
        execvp(command, (char**)args);
        exit(1);
    }

    fd_screencap = fds[0];

    /* read w, h & format */
    if(readx(fd_screencap, &w, 4)) goto done;
    if(readx(fd_screencap, &h, 4)) goto done;
    if(readx(fd_screencap, &f, 4)) goto done;

    fbinfo.version = DDMS_RAWIMAGE_VERSION;
    /* see hardware/hardware.h */
    switch (f) {
        case 1: /* RGBA_8888 */
            fbinfo.bpp = 32;
            fbinfo.size = w * h * 4;
            fbinfo.width = w;
            fbinfo.height = h;
            fbinfo.red_offset = 0;
            fbinfo.red_length = 8;
            fbinfo.green_offset = 8;
            fbinfo.green_length = 8;
            fbinfo.blue_offset = 16;
            fbinfo.blue_length = 8;
            fbinfo.alpha_offset = 24;
            fbinfo.alpha_length = 8;
            break;
        case 2: /* RGBX_8888 */
            fbinfo.bpp = 32;
            fbinfo.size = w * h * 4;
            fbinfo.width = w;
            fbinfo.height = h;
            fbinfo.red_offset = 0;
            fbinfo.red_length = 8;
            fbinfo.green_offset = 8;
            fbinfo.green_length = 8;
            fbinfo.blue_offset = 16;
            fbinfo.blue_length = 8;
            fbinfo.alpha_offset = 24;
            fbinfo.alpha_length = 0;
            break;
        case 3: /* RGB_888 */
            fbinfo.bpp = 24;
            fbinfo.size = w * h * 3;
            fbinfo.width = w;
            fbinfo.height = h;
            fbinfo.red_offset = 0;
            fbinfo.red_length = 8;
            fbinfo.green_offset = 8;
            fbinfo.green_length = 8;
            fbinfo.blue_offset = 16;
            fbinfo.blue_length = 8;
            fbinfo.alpha_offset = 24;
            fbinfo.alpha_length = 0;
            break;
        case 4: /* RGB_565 */
            fbinfo.bpp = 16;
            fbinfo.size = w * h * 2;
            fbinfo.width = w;
            fbinfo.height = h;
            fbinfo.red_offset = 11;
            fbinfo.red_length = 5;
            fbinfo.green_offset = 5;
            fbinfo.green_length = 6;
            fbinfo.blue_offset = 0;
            fbinfo.blue_length = 5;
            fbinfo.alpha_offset = 0;
            fbinfo.alpha_length = 0;
            break;
        case 5: /* BGRA_8888 */
            fbinfo.bpp = 32;
            fbinfo.size = w * h * 4;
            fbinfo.width = w;
            fbinfo.height = h;
            fbinfo.red_offset = 16;
            fbinfo.red_length = 8;
            fbinfo.green_offset = 8;
            fbinfo.green_length = 8;
            fbinfo.blue_offset = 0;
            fbinfo.blue_length = 8;
            fbinfo.alpha_offset = 24;
            fbinfo.alpha_length = 8;
           break;
        default:
            goto done;
    }

    /* write header */
    if(writex(fd, &fbinfo, sizeof(fbinfo))) goto done;

    /* write data */
    for(i = 0; i < fbinfo.size; i += sizeof(buf)) {
      if(readx(fd_screencap, buf, sizeof(buf))) goto done;
      if(writex(fd, buf, sizeof(buf))) goto done;
    }
    if(readx(fd_screencap, buf, fbinfo.size % sizeof(buf))) goto done;
    if(writex(fd, buf, fbinfo.size % sizeof(buf))) goto done;

done:
    close(fds[0]);
    close(fds[1]);
    close(fd);
}
#endif
