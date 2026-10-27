/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Input: keyboard / gamepad hot-switching, Start-only joining, drop-out recovery, join prompt text.
 */
#include "all.h"

int g_hot_switch = 1; /* P1 follows the last used device; extra gamepads join only with Start */
int g_start_button = 7; /* log frame rate once a second, first 120 s */
static int g_detect_logged;

static int g_join_logged; /* Start fast-forwards the running cutscene */
static int g_join_start_text;

#define ADDR_CONTROLLER_STATUS 0x0054AE40u  /* sub_54AE40(slot): below 2 means that slot has no controller */

#define DROP_DEBOUNCE_MS      1000   /* time, not frames: 60 frames were 60 ms at 1000 fps */

static unsigned DeviceMask(int device)
{
    unsigned mask = 0;
    int raw;

    for (raw = 0; raw < DEVICE_COUNT; raw++)
        if (((MapDeviceFn)ADDR_MAP_DEVICE)(raw, 0) == device)
            mask |= 1u << raw;
    return mask;
}

static void AssignDevice(int slot, int device)
{
    ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(slot, device);
    ((EnableDeviceFn)ADDR_ENABLE_DEVICE)(device, 1);
}

/*
 * Player 1 follows the device that was used last (keyboard or gamepad). If player 1
 * is already on a gamepad, other gamepads are left to the game so they join as player 2.
 */
static DWORD g_p1_inactive_since;   /* tick count when player 1 became inactive, 0 = active */
static int g_p1_dropped;

/*
 * If player 1 gets dropped (controller removed, remote play glitches, ...) the game keeps
 * their device assigned. Its join code only looks at devices that are not assigned to a
 * slot, so the dropped player could never press a button to come back. After a short
 * debounce the slot is unassigned so the normal join works again.
 */
static int HandleDroppedPlayer1(void)
{
    int p1 = *(int *)ADDR_PLAYER_OBJ;

    if (!p1 || *(char *)(p1 + PLAYER_ACTIVE_OFS) < 0) {
        g_p1_inactive_since = 0;
        g_p1_dropped = 0;
        return 0;
    }
    if (!g_p1_inactive_since) {
        g_p1_inactive_since = TickNonZero();
        if (g_log) {
            int status0 = ((int (__cdecl *)(int))ADDR_CONTROLLER_STATUS)(0);
            int status1 = ((int (__cdecl *)(int))ADDR_CONTROLLER_STATUS)(1);

            fprintf(g_log, "player 1 became inactive: slot 0 device %d (assigned %d), slot 1 device %d, controller status %d/%d\n",
                    *(int *)ADDR_SLOT_DEVICE, *(int *)ADDR_SLOT_ASSIGNED, *(int *)(ADDR_SLOT_DEVICE + 12), status0, status1);
            fflush(g_log);
        }
    }
    if (!g_p1_dropped && GetTickCount() - g_p1_inactive_since >= DROP_DEBOUNCE_MS) {
        g_p1_dropped = 1;
        if (*(int *)ADDR_SLOT_ASSIGNED) {
            ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(0, -1);
            Log("player 1 dropped, device unassigned so they can rejoin");
        }
    }
    return 1;
}

#define ADDR_DEVICE_RECORDS  0x00917000u  /* 132-byte record per device, first byte: device is connected */

#define ADDR_ALT_INPUT_MODE  0x0091EF2Cu   /* dword_91EF2C: records are indexed by slot instead of device */

#define DEVICE_RECORD_SIZE   132

static int DeviceConnected(int device)
{
    if (device < 0 || device >= FIRST_KEYBOARD_DEVICE)
        return 1;
    return *(char *)(ADDR_DEVICE_RECORDS + device * DEVICE_RECORD_SIZE) != 0;
}

/* Log every change of the input / player state so lost-input reports can be traced. */
static void TraceInputState(void)
{
    static int last[12], lines;
    int now[12], i, p, same = 1;

    now[0] = *(int *)ADDR_SLOT_DEVICE;
    now[1] = *(int *)ADDR_SLOT_ASSIGNED;
    now[2] = *(int *)(ADDR_SLOT_DEVICE + 12);
    now[3] = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    now[4] = DeviceConnected(now[0]);
    now[5] = DeviceConnected(now[2]);
    for (i = 0; i < 2; i++) {
        p = *(int *)(ADDR_PLAYER_OBJ + 4 * i);
        now[6 + i] = p ? (*(char *)(p + PLAYER_ACTIVE_OFS) < 0) : -1;
    }
    now[8] = *(int *)ADDR_MENU_DEPTH;
    now[9] = DeviceConnected(0);
    now[10] = DeviceConnected(1);
    now[11] = 0;
    for (i = 0; i < 12; i++)
        if (now[i] != last[i])
            same = 0;
    if (same || !g_log || lines >= 300)
        return;
    memcpy(last, now, sizeof last);
    lines++;
    fprintf(g_log, "input state: slot0 dev %d set %d conn %d | slot1 dev %d set %d conn %d | p1 active %d p2 active %d | menu depth %d | pad0 conn %d pad1 conn %d\n",
            now[0], now[1], now[4], now[2], now[3], now[5], now[6], now[7], now[8], now[9], now[10]);
    fflush(g_log);
}

/*
 * Player 2's gamepad can go away (Remote Play guest leaves, pad unplugged). The game keeps
 * the device assigned to slot 1, so nobody could join as player 2 again. Once the device has been
 * reported disconnected for a short while the slot is released.
 */
static DWORD g_p2_gone_since;   /* 0 = device present */
static int g_p2_released;

static void HandleDisconnectedPlayer2(void)
{
    int device = *(int *)(ADDR_SLOT_DEVICE + 12);

    if (!*(int *)(ADDR_SLOT_ASSIGNED + 12) || device < 0 || device >= FIRST_KEYBOARD_DEVICE ||
        *(int *)ADDR_ALT_INPUT_MODE || DeviceConnected(device)) {
        g_p2_gone_since = 0;
        g_p2_released = 0;
        return;
    }
    if (!g_p2_gone_since)
        g_p2_gone_since = TickNonZero();
    if (!g_p2_released && GetTickCount() - g_p2_gone_since >= DROP_DEBOUNCE_MS) {
        g_p2_released = 1;
        ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(1, -1);
        if (g_log) {
            fprintf(g_log, "player 2 device %d disconnected, slot 1 released\n", device);
            fflush(g_log);
        }
    }
}

#define ADDR_JOIN_DETECT_CALL 0x004280D3u  /* call sub_6D9120 in the join code sub_427EC0 */

/*
 * The game lets any gamepad button press join as another player, while a keyboard has to use
 * F1 / F2. Gamepads are held to the same standard: only Start joins. Devices that pressed
 * something else are left out of the scan so other devices can still be found.
 */
static int __cdecl JoinDetect(int mask, int kinds, int *type, int *code, int skip_taken)
{
    int local_type, local_code, dev, tries;

    if (!g_hot_switch)
        return ((DetectDeviceFn)ADDR_DETECT_DEVICE)(mask, kinds, type, code, skip_taken);
    for (tries = 0; tries < DEVICE_COUNT; tries++) {
        dev = ((DetectDeviceFn)ADDR_DETECT_DEVICE)(mask, kinds, &local_type, &local_code, skip_taken);
        if (dev < 0)
            return -1;
        if (dev >= FIRST_KEYBOARD_DEVICE || local_code == g_start_button) {
            if (g_log && g_join_logged < 30 && dev < FIRST_KEYBOARD_DEVICE) {
                fprintf(g_log, "join accepted: gamepad %d Start (button code %d)\n", dev, local_code);
                fflush(g_log);
                g_join_logged++;
            }
            if (type)
                *type = local_type;
            if (code)
                *code = local_code;
            return dev;
        }
        if (g_log && g_join_logged < 30) {
            fprintf(g_log, "join ignored: gamepad %d pressed button code %d (Start is %d)\n", dev, local_code, g_start_button);
            fflush(g_log);
            g_join_logged++;
        }
        mask |= 1 << dev;
    }
    return -1;
}

#define ADDR_PAUSE_OWNER  0x00802AF8u  /* dword_802AF8: slot whose input drives the pause menu */

#define ADDR_PAUSED_1     0x0093B328u  /* dword_93B328 / dword_93B32C: game is paused by a player */

#define ADDR_PAUSED_2     0x0093B32Cu

#define ADDR_DEVICE_REFRESH_CALL 0x006DAEABu  /* call sub_6DA970 in sub_6DAEA0 */

#define ADDR_DEVICE_REFRESH      0x006DA970u  /* thiscall sub_6DA970(this, slots): re-seats devices of the players */

#define ADDR_DROP_PLAYER         0x00427CD0u  /* sub_427CD0(player, 1, 0): the pause menu's "Drop Out" */

static int g_p2_drop_pending;

static int PlayerActive(int slot)
{
    int p = *(int *)(ADDR_PLAYER_OBJ + 4 * slot);

    return p && *(char *)(p + PLAYER_ACTIVE_OFS) < 0;
}

/*
 * When a gamepad disappears the game re-seats the devices of the players and hands the
 * orphaned player a keyboard layout, so a Remote Play guest who leaves turns into a keyboard
 * player 2. Player 2 should drop out instead: keep slot 1 unassigned and drop the player
 * the way the pause menu's "Drop Out" does.
 */
static void __attribute__((thiscall)) DeviceRefreshHook(void *self, int slots)
{
    int set_before = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    int dev_before = *(int *)(ADDR_SLOT_DEVICE + 12);

    ((DeviceRefreshFn)ADDR_DEVICE_REFRESH)(self, slots);

    if (*(int *)(ADDR_SLOT_ASSIGNED + 12) && (!set_before ||
        (dev_before >= 0 && dev_before < FIRST_KEYBOARD_DEVICE && !DeviceConnected(dev_before) &&
         *(int *)(ADDR_SLOT_DEVICE + 12) >= FIRST_KEYBOARD_DEVICE))) {
        ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(1, -1);
        if (set_before)
            g_p2_drop_pending = 1;
        Log("device refresh: player 2 not given a keyboard");
    }
}

/*
 * The pause menu reads its input from the slot that paused. When player 2 drops out (gamepad
 * lost) the game parks player 2 on a keyboard layout nobody uses and leaves them as the pause
 * owner, so the menu can no longer be driven by player 1. Release player 2's slot once their
 * pad is gone and hand the pause menu to player 1.
 */
static int g_p2_pad = -1;

static void HandleLostPlayer2Pad(void)
{
    int slot1_set = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    int device = *(int *)(ADDR_SLOT_DEVICE + 12);

    if (slot1_set && device >= 0 && device < FIRST_KEYBOARD_DEVICE && DeviceConnected(device)) {
        g_p2_pad = device;
    } else if (g_p2_pad >= 0 && !DeviceConnected(g_p2_pad)) {
        if (slot1_set)
            ((AssignDeviceFn)ADDR_ASSIGN_DEVICE)(1, -1);
        g_p2_pad = -1;
        g_p2_drop_pending = 1;
        Log("player 2 gamepad lost, slot 1 released");
    }

    if (g_p2_drop_pending) {
        g_p2_drop_pending = 0;
        if (PlayerActive(1)) {
            ((void (__cdecl *)(int, int, int, int))ADDR_DROP_PLAYER)(1, 1, 0, 0);
            Log("player 2 dropped out");
        }
    }

    if ((*(int *)ADDR_PAUSED_1 || *(int *)ADDR_PAUSED_2) && *(int *)ADDR_PAUSE_OWNER == 1 &&
        !*(int *)(ADDR_SLOT_ASSIGNED + 12) && *(int *)ADDR_SLOT_ASSIGNED) {
        *(int *)ADDR_PAUSE_OWNER = 0;
        Log("pause menu handed to player 1");
    }
}

static unsigned g_prev_pressed;

void HotSwitchInput(void)
{
    int raw, device, ctx;
    int slot0_dev = *(int *)ADDR_SLOT_DEVICE;
    int slot1_dev = *(int *)(ADDR_SLOT_DEVICE + 12);
    int slot0_set = *(int *)ADDR_SLOT_ASSIGNED;
    int slot1_set = *(int *)(ADDR_SLOT_ASSIGNED + 12);
    int player2 = *(int *)(ADDR_PLAYER_OBJ + 4);
    unsigned mask = 0, pressed = 0, fresh;

    TraceInputState();
    HandleLostPlayer2Pad();
    HandleDisconnectedPlayer2();
    if (HandleDroppedPlayer1())
        return;                                  /* let the game's join code take the next press */

    if (slot0_set && !DeviceConnected(slot0_dev))
        slot0_set = 0;                           /* player 1's gamepad is gone: any live device may take over */
    if (slot0_set)
        mask |= DeviceMask(slot0_dev);           /* only look at other devices */
    if (player2 && *(char *)(player2 + PLAYER_ACTIVE_OFS) < 0 && slot1_set)
        mask |= DeviceMask(slot1_dev);           /* player 2 is playing: leave their device alone */

    /*
     * Collect every raw device with a pressed input, then only react to the ones that were
     * not pressed on the previous frame. A key that stays down (a stuck key after a Remote
     * Play guest drops, a held key on another keyboard layout) would otherwise take player 1
     * back from the gamepad every frame.
     */
    ctx = ((InputSaveFn)ADDR_INPUT_SAVE)();
    ((InputSelectFn)ADDR_INPUT_SELECT)(0);
    for (raw = 0; raw < DEVICE_COUNT; raw++) {
        int found = ((DetectDeviceFn)ADDR_DETECT_DEVICE)((int)pressed, DETECT_KINDS | DETECT_KINDS_ANALOG, NULL, NULL, 0);

        if (found < 0)
            break;
        pressed |= 1u << found;
    }
    ((InputSelectFn)ADDR_INPUT_SELECT)(ctx);
    fresh = pressed & ~g_prev_pressed & ~mask;
    g_prev_pressed = pressed;
    if (!fresh)
        return;
    for (raw = 0; raw < DEVICE_COUNT && !(fresh & (1u << raw)); raw++)
        ;

    device = ((MapDeviceFn)ADDR_MAP_DEVICE)(raw, 0);
    if (!DeviceConnected(device))
        return;                                  /* stale input from a pad that is no longer there */
    if (g_log && g_detect_logged < 40) {
        fprintf(g_log, "input detected: raw %d device %d (slot 0 uses %d, slot 1 uses %d)\n",
                raw, device, slot0_dev, slot1_dev);
        fflush(g_log);
        g_detect_logged++;
    }

    if (slot0_set && slot0_dev < FIRST_KEYBOARD_DEVICE && device < FIRST_KEYBOARD_DEVICE) {
        /*
         * Player 1 is on a gamepad and another gamepad was pressed: that is a second
         * player. The game's own join code (sub_427EC0) picks up any pressed device
         * that is not assigned to a slot yet, assigns it and joins player 2, so
         * assigning it here would hide it from that check. Leave it alone.
         */
        return;
    }

    if (*(int *)ADDR_SLOT_ASSIGNED && slot0_dev == device)
        return;
    AssignDevice(0, device);
    Log("player 1 input device switched");
}

/*
 * The join prompts ("Press F1 / F2 or any button to start") come from the game's text data and
 * live in heap memory. While gamepads need Start to join they are rewritten in place (same
 * length or shorter) and restored when the option is turned off.
 */
#define JOIN_TEXT_COUNT 2

static const char *k_join_orig[JOIN_TEXT_COUNT] = {
    "Press F1 or any button to start",
    "Press F2 or any button to start",
};

static const char *k_join_start[JOIN_TEXT_COUNT] = {
    "F1 or START",
    "F2 or START",
};

#define JOIN_TEXT_MAX 8

static char *g_join_addr[JOIN_TEXT_MAX];

static int g_join_kind[JOIN_TEXT_MAX];

static int g_join_found;

static void ScanJoinText(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000, self = (uintptr_t)GetModuleHandleA("SagaTweaks.asi");
    size_t self_size = 0;
    int k;

    if (self) {
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);

        self_size = nt->OptionalHeader.SizeOfImage;
    }
    g_join_found = 0;
    while (addr < 0x7FFF0000u && VirtualQuery((void *)addr, &mbi, sizeof mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        size_t size = mbi.RegionSize;

        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READWRITE)) &&
            !(base >= self && base < self + self_size) && size > 64) {
            static char chunk[0x10000 + 64];
            size_t off = 0, i;
            SIZE_T got;

            while (off < size) {
                size_t want = size - off > 0x10000 ? 0x10000 : size - off;

                if (!ReadProcessMemory(GetCurrentProcess(), (void *)(base + off), chunk, want, &got) || got < 40)
                    break;
                for (i = 0; i + 40 < got; i++) {
                    if (chunk[i] != 'P' || chunk[i + 1] != 'r' || chunk[i + 5] != ' ')
                        continue;
                    for (k = 0; k < JOIN_TEXT_COUNT; k++) {
                        if ((!memcmp(chunk + i, k_join_orig[k], strlen(k_join_orig[k]) + 1) ||
                             !memcmp(chunk + i, k_join_start[k], strlen(k_join_start[k]) + 1)) &&
                            g_join_found < JOIN_TEXT_MAX) {
                            char *at = (char *)(base + off + i);
                            int d, dup = 0;

                            for (d = 0; d < g_join_found; d++)
                                dup |= g_join_addr[d] == at;
                            if (!dup) {
                                g_join_addr[g_join_found] = at;
                                g_join_kind[g_join_found++] = k;
                            }
                        }
                    }
                }
                off += got > 64 ? got - 64 : got;     /* overlap so a string on a boundary is not missed */
            }
        }
        addr = base + size;
    }
}

static void ApplyJoinText(void)
{
    int i;

    for (i = 0; i < g_join_found; i++) {
        const char *want = g_join_start_text ? k_join_start[g_join_kind[i]] : k_join_orig[g_join_kind[i]];
        const char *other = g_join_start_text ? k_join_orig[g_join_kind[i]] : k_join_start[g_join_kind[i]];
        size_t n = strlen(want) + 1, n_other = strlen(other) + 1;
        char now[40];
        SIZE_T got;

        /* Only the other variant of the text is replaced: a block the game has freed and reused is left alone. */
        if (ReadProcessMemory(GetCurrentProcess(), g_join_addr[i], now, n_other, &got) && got == n_other &&
            !memcmp(now, other, n_other))
            WriteProcessMemory(GetCurrentProcess(), g_join_addr[i], want, n, &got);
    }
}

static DWORD WINAPI JoinTextThread(LPVOID param)
{
    int tries = 0;

    (void)param;
    for (;;) {
        Sleep(g_join_found ? 1000 : 3000);
        if (!g_join_found && g_hot_switch && tries++ < 40) {     /* nothing to rewrite while Start-only joining is off */
            ScanJoinText();
            if (g_join_found && g_log) {
                fprintf(g_log, "join prompt text found (%d copies)\n", g_join_found);
                fflush(g_log);
            }
        }
        g_join_start_text = g_hot_switch;
        if (g_join_found)
            ApplyJoinText();
    }
    return 0;
}
int InputReady(void)
{
    return Matches(ADDR_JOIN_DETECT_CALL, (const uint8_t[]){ 0xE8 }, 1) &&
           Matches(ADDR_DEVICE_REFRESH_CALL, (const uint8_t[]){ 0xE8 }, 1);
}

void InputInstall(void)
{
    Log(PatchCall(ADDR_JOIN_DETECT_CALL, JoinDetect) ? "join detection hooked" : "join detection hook FAILED");
    Log(PatchCall(ADDR_DEVICE_REFRESH_CALL, DeviceRefreshHook) ? "device refresh hooked" : "device refresh hook FAILED");
    CreateThread(NULL, 0, JoinTextThread, NULL, 0, NULL);
}

void InputFrame(int game)
{
    if (g_hot_switch && game)
        HotSwitchInput();
}
