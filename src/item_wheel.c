#include "global.h"
#include "assertf.h"
#include "battle_pyramid.h"
#include "bg.h"
#include "constants/items.h"
#include "constants/rgb.h"
#include "constants/weather.h"
#include "decompress.h"
#include "dma3.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "field_weather.h"
#include "fieldmap.h"
#include "gba/defines.h"
#include "gba/io_reg.h"
#include "gba/macro.h"
#include "gba/types.h"
#include "global.fieldmap.h"
#include "gpu_regs.h"
#include "io_reg.h"
#include "item.h"
#include "item_icon.h"
#include "item_menu.h"
#include "main.h"
#include "malloc.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "script.h"
#include "sprite.h"
#include "task.h"
#include "util.h"
#include <limits.h>
#include <stdint.h>

// Graphics
static const u32 sItmWhlBgTiles[] = INCGFX_U32("graphics/item_wheel/ring.png", ".4bpp.smol");
static const u16 sItmWhlBgPalette[] = INCGFX_U16("graphics/item_wheel/14.pal", ".gbapal");
static const u16 sItmWhlBgTilemap[] = INCBIN_U16("graphics/item_wheel/ring.bin");

#define SLOT_PLTT_IDX(_slot) (_slot + 2)

enum ItmWhl_Slot {
    SLOT_N,
    SLOT_NE,
    SLOT_E,
    SLOT_SE,
    SLOT_S,
    SLOT_SW,
    SLOT_W,
    SLOT_NW,
    SLOT_COUNT,
};

#define PALTAG_ITEM  (0x1000 | BLEND_IMMUNE_FLAG)
#define TILETAG_ITEM  0x1000

// Structs
struct SavedReg {
    u16 dispCnt;
    u16 winIn;
    u16 winOut;
    u16 bldCnt;
    u16 bldAlpha;
};

struct ItmWhl_Mem {
    u8 mainTaskId;
    u8 previousSlot;
    u8 selectedSlot;
    u8 cardinalDelay;
    u16 prevKeys;
    u16 prevHeldKeys;
    u8 spriteIds[SLOT_COUNT];
    struct SavedReg reg;
};

// EWRAM_DATA
static EWRAM_DATA struct ItmWhl_Mem* sItmWhlMem;

// Static Functions
static void ItmWhl_LoadBgGfx(void);
static void ItmWhl_CreateItemSprites(void);
static void ItmWhl_LockPlayer(void);
static void ItmWhl_HandleDPadInput(void);
static u32 ItmWhl_DPadToSlot(void);
static void ItmWhl_CleanupAndExit(void);
static bool32 ItmWhl_HandleFlash(void);
static void ItmWhl_SetRegisters(void);
static void ItmWhl_SaveRegisters(void);
static void ItmWhl_RestoreRegisters(void);
static inline bool32 ItmWhl_IsCardinalSlot(enum ItmWhl_Slot slot);

// Tasks
static void Task_ItmWhlMain(u8 taskId);

static const u16 *sItmWhlTilemaps[] = {
    [SLOT_N]  = sItmWhlBgTilemap + (SLOT_N  + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_NE] = sItmWhlBgTilemap + (SLOT_NE + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_E]  = sItmWhlBgTilemap + (SLOT_E  + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_SE] = sItmWhlBgTilemap + (SLOT_SE + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_S]  = sItmWhlBgTilemap + (SLOT_S  + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_SW] = sItmWhlBgTilemap + (SLOT_SW + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_W]  = sItmWhlBgTilemap + (SLOT_W  + 1) * BG_SCREEN_SIZE / 2,
    [SLOT_NW] = sItmWhlBgTilemap + (SLOT_NW + 1) * BG_SCREEN_SIZE / 2,
};

static const enum Item ItmWhl_Registered[SLOT_COUNT] = {
    ITEM_ACRO_BIKE,        ITEM_MACH_BIKE, ITEM_TOWN_MAP,  ITEM_VS_SEEKER,
    ITEM_POKEMON_BOX_LINK, ITEM_GOOD_ROD,  ITEM_EXP_SHARE, ITEM_SUPER_ROD,
};

static void ItmWhl_LoadBgGfx(void)
{
    DecompressAndLoadBgGfxUsingHeap(0, sItmWhlBgTiles, 0, 0, 0);
    LoadPalette(gStandardMenuPalette, BG_PLTT_ID(15), PLTT_SIZE_4BPP);
    LoadPalette(sItmWhlBgPalette, BG_PLTT_ID(14), PLTT_SIZE_4BPP);
    CopyToBgTilemapBuffer(0, sItmWhlBgTilemap, BG_SCREEN_SIZE, 0);
    CopyBgTilemapBufferToVram(0);
    ScheduleBgCopyTilemapToVram(0);
}

#define SLOT_X 40
#define SLOT_Y 53

static const struct Coords8 sItemSlotPos[] = {
    {0, -SLOT_Y}, {SLOT_X, -SLOT_X}, {SLOT_Y, 0},  {SLOT_X, SLOT_X},
    {0, SLOT_Y},  {-SLOT_X, SLOT_X}, {-SLOT_Y, 0}, {-SLOT_X, -SLOT_X},
};

#undef SLOT_X
#undef SLOT_Y

static void ItmWhl_CreateItemSprites(void)
{
    for (enum ItmWhl_Slot i = 0; i < SLOT_COUNT; i++) {
        u8 id = AddItemIconSprite(TILETAG_ITEM + i, PALTAG_ITEM + i, ItmWhl_Registered[i]);
        struct Sprite* icon = &gSprites[id];
        icon->oam.priority = 0;
        icon->oam.objMode = ST_OAM_OBJ_NORMAL;
        icon->coordOffsetEnabled = FALSE;
        icon->x = DISPLAY_WIDTH/2 + 4 + sItemSlotPos[i].x;
        icon->y = DISPLAY_HEIGHT/2 + 4 + sItemSlotPos[i].y;
        icon->copyToObjWin = TRUE;
        sItmWhlMem->spriteIds[i] = id;
    }
}

void ItmWhl_InitItemWheel(void)
{
    sItmWhlMem = AllocZeroed(sizeof(struct ItmWhl_Mem));

    if (sItmWhlMem == NULL) {
        errorf("ItmWhl_Mem allocation failed");
        SetMainCallback2(CB2_ReturnToField);
        return;
    }

    ItmWhl_LockPlayer();
    ItmWhl_LoadBgGfx();
    ItmWhl_SaveRegisters();
    ItmWhl_SetRegisters();
    ItmWhl_CreateItemSprites();
    sItmWhlMem->previousSlot = sItmWhlMem->selectedSlot = UINT8_MAX;
    sItmWhlMem->mainTaskId = CreateTask(Task_ItmWhlMain, 1);
}

u16* map1;
u16* map2;

static void Task_ItmWhlMain(u8 taskId)
{
    TASK_DATA(state, counter);
    tData->counter++;
    switch (tData->state) {
    case 0: {

        sItmWhlMem->prevKeys = gMain.newKeys;
        sItmWhlMem->prevHeldKeys = gMain.heldKeys;
        tData->state++;
        break;
    }
    case 1: {

        if (JOY_NEW(B_BUTTON)) {
            ItmWhl_CleanupAndExit();
            UnlockPlayerFieldControls();
            UnfreezeObjectEvents();
        }
        else if (JOY_NEW(A_BUTTON)) {
            u32 slot = sItmWhlMem->selectedSlot;
            gSpecialVar_ItemId = ItmWhl_Registered[slot];
            u32 itemTaskId = CreateTask(GetItemFieldFunc(ItmWhl_Registered[slot]), 8);
            gTasks[itemTaskId].data[3] = TRUE;
            ItmWhl_CleanupAndExit();
        }
        else {
            ItmWhl_HandleDPadInput();
            if (sItmWhlMem->selectedSlot == UINT8_MAX)
                break;
            if (sItmWhlMem->selectedSlot == sItmWhlMem->previousSlot)
                break;

            CopyToBgTilemapBuffer(0, sItmWhlTilemaps[sItmWhlMem->selectedSlot], BG_SCREEN_SIZE, 0);

            if (!JOY_HELD(DPAD_ANY) && sItmWhlMem->selectedSlot != UINT8_MAX)
                tData->state++;

            ScheduleBgCopyTilemapToVram(0);
        }
        sItmWhlMem->prevHeldKeys = gMain.heldKeys;
        break;
    }
    case 2: {
        u32 slot = sItmWhlMem->selectedSlot;
        gSpecialVar_ItemId = ItmWhl_Registered[slot];
        u32 itemTaskId = CreateTask(GetItemFieldFunc(ItmWhl_Registered[slot]), 8);
        gTasks[itemTaskId].data[3] = TRUE;
        ItmWhl_CleanupAndExit();
        DestroyTask(taskId);
        break;
    }
    }
}

static void ItmWhl_HandleDPadInput(void)
{
    u32 slot = ItmWhl_DPadToSlot();

    if (slot == UINT8_MAX)
        return;
    if (slot == sItmWhlMem->selectedSlot) {
        sItmWhlMem->cardinalDelay = 0;
        return;
    }

    bool32 isDiagToCardinal =
        !ItmWhl_IsCardinalSlot(sItmWhlMem->selectedSlot) &&
        ItmWhl_IsCardinalSlot(slot);

    if (isDiagToCardinal && sItmWhlMem->cardinalDelay++ <= 4)
        return;

    sItmWhlMem->cardinalDelay = 0;
    sItmWhlMem->previousSlot = sItmWhlMem->selectedSlot;
    sItmWhlMem->selectedSlot = slot;
}

static u32 ItmWhl_DPadToSlot(void)
{
    u16 keys = gMain.newKeys | gMain.heldKeys;

    if ((keys & DPAD_UP) && (keys & DPAD_RIGHT))
        return SLOT_NE;
    if ((keys & DPAD_DOWN) && (keys & DPAD_RIGHT))
        return SLOT_SE;
    if ((keys & DPAD_DOWN) && (keys & DPAD_LEFT))
        return SLOT_SW;
    if ((keys & DPAD_UP) && (keys & DPAD_LEFT))
        return SLOT_NW;

    if (keys & DPAD_UP)
        return SLOT_N;
    if (keys & DPAD_RIGHT)
        return SLOT_E;
    if (keys & DPAD_DOWN)
        return SLOT_S;
    if (keys & DPAD_LEFT)
        return SLOT_W;

    return UINT8_MAX;
}

static void ItmWhl_LockPlayer(void)
{
    if (!IsOverworldLinkActive()) {
        FreezeObjectEvents();
        PlayerFreeze();
        StopPlayerAvatar();
    }
    LockPlayerFieldControls();
}

static void ItmWhl_CleanupAndExit(void)
{
    u8 taskId = sItmWhlMem->mainTaskId;

    for (int i = 0; i < SLOT_COUNT; i++) {
        DestroySprite(&gSprites[sItmWhlMem->spriteIds[i]]);
    }

    ItmWhl_RestoreRegisters();
    TRY_FREE_AND_SET_NULL(sItmWhlMem);
    CpuFastFill(0, GetBgTilemapBuffer(0), BG_SCREEN_SIZE);
    CopyBgTilemapBufferToVram(0);
    CpuFastFill(0, (void *)BG_CHAR_ADDR(2), BG_CHAR_SIZE);
    ClearScheduledBgCopiesToVram();
    DestroyTask(taskId);
}

static bool32 ItmWhl_HandleFlash(void)
{
    return InBattlePyramid() || GetFlashLevel();
}

static void ItmWhl_SetRegisters()
{
    bool32 flash = ItmWhl_HandleFlash();

    if (flash) {
        SetGpuRegBits(REG_OFFSET_DISPCNT, DISPCNT_OBJWIN_ON);
        SetGpuRegBits(REG_OFFSET_WINOUT, WINOUT_WINOBJ_OBJ);
        SetGpuRegBits(REG_OFFSET_WININ, WININ_WIN0_CLR);
        SetGpuRegBits(REG_OFFSET_WININ, WININ_WIN1_CLR);
        SetGpuRegBits(REG_OFFSET_WINOUT, WINOUT_WIN01_CLR);
    }
    else {

        SetGpuRegBits(REG_OFFSET_WININ, WININ_WIN0_CLR);
    }
}

static void ItmWhl_SaveRegisters(void)
{
    struct SavedReg* reg = &sItmWhlMem->reg;
    reg->dispCnt = GetGpuReg(REG_OFFSET_DISPCNT);
    reg->winIn = GetGpuReg(REG_OFFSET_WININ);
    reg->winOut = GetGpuReg(REG_OFFSET_WINOUT);
    reg->bldCnt = GetGpuReg(REG_OFFSET_BLDCNT);
    reg->bldAlpha = GetGpuReg(REG_OFFSET_BLDALPHA);
}

static void ItmWhl_RestoreRegisters(void)
{
    struct SavedReg* reg = &sItmWhlMem->reg;
    SetGpuReg(REG_OFFSET_DISPCNT, reg->dispCnt);
    SetGpuReg(REG_OFFSET_WININ, reg->winIn);
    SetGpuReg(REG_OFFSET_WINOUT, reg->winOut);
    SetGpuReg(REG_OFFSET_BLDCNT, reg->bldCnt);
    SetGpuReg(REG_OFFSET_BLDALPHA, reg->bldAlpha);
}

static inline bool32 ItmWhl_IsCardinalSlot(enum ItmWhl_Slot slot)
{
    return (slot == SLOT_N || slot == SLOT_E ||
            slot == SLOT_S || slot == SLOT_W);
}
