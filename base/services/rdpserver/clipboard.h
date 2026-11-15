/*
 * PROJECT:     LiberNT RDP console server
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Clipboard redirection interface of the RDP console server
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193@gmail.com>
 */

#pragma once
#include <windows.h>
#include <freerdp/freerdp.h>

typedef struct RDP_CLIPBOARD RDP_CLIPBOARD;
typedef void (*CLIPBOARD_LOG)(const char *, ...);
RDP_CLIPBOARD *ClipboardStart(HANDLE vcm, rdpContext *rdp, CLIPBOARD_LOG log);
void ClipboardStop(RDP_CLIPBOARD *clipboard);
