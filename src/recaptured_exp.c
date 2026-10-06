#include "global.h"
#include "assertf.h"
#include "battle.h"
#include "battle_controllers.h"
#include "battle_scripts.h"
#include "caps.h"
#include "constants/battle.h"
#include "constants/battle_string_ids.h"
#include "constants/item_effects.h"
#include "gba/isagbprint.h"
#include "pokemon.h"
#include "gba/types.h"
#define CMD_ARGS(...) const struct __attribute__((packed)) { u8 opcode; RECURSIVELY(R_FOR_EACH(APPEND_SEMICOLON, __VA_ARGS__)) const u8 nextInstr[0]; } *const cmd UNUSED = (const void *)gBattlescriptCurrInstr
#define NATIVE_ARGS(...) CMD_ARGS(void (*func)(void), ##__VA_ARGS__)

#define Swap(a, b) \
    do { \
        __auto_type tmp = a; \
        a = b; \
        b = tmp; \
    } while (0);

typedef s32 (*ExpHandlerFunc)(u8*);

static bool32 RunExpHandler(ExpHandlerFunc func, u8* expMonId);

static s32 HandleExpInit(u8 *expMonId);
static s32 HandleSetAndPrint(u8* expMonId);
static s32 HandleExpUpdate(u8* expMonId);
static s32 HandleLevelUp(u8* expMonId);
static s32 HandleExpLoop(u8* expMonId);
static s32 HandleExpFinish(u8* expMonId);

static void ApplyExperienceMultipliers(s32 *expAmount, u8 expGetterMonId, u8 faintedBattler);
static u32 ComputeExpRewardForBattler(u32 expMonId, enum BattlerId fainterBattler, u32 totalExp);
static enum BattlerId GetBattlerFromPlayerPartyId(u32 partyId);
static enum HoldEffect GetMonHoldEffect(struct Pokemon *mon);


static inline u8 GetHighestMonLevel(void);
static inline bool32 WasPartyMonSentOut(enum BattleTrainer trainer, u32 partyIndex);

static const u8 sCatchUpExpFactors[] =
{
    100, 110, 125, 140, 160, 175, 190, 200, 210, 220,
    225, 230, 235, 240, 245, 250,
};

static const s32 sExperienceScalingFactors[] = {
    0,      0,      1,      3,      8,      13,     22,     32,     45,     60,
    79,     100,    124,    152,    183,    217,    256,    297,    343,    393,
    447,    505,    567,    634,    705,    781,    861,    946,    1037,   1132,
    1232,   1337,   1448,   1563,   1685,   1811,   1944,   2081,   2225,   2374,
    2529,   2690,   2858,   3031,   3210,   3396,   3587,   3786,   3990,   4201,
    4419,   4643,   4874,   5112,   5357,   5608,   5866,   6132,   6404,   6684,
    6971,   7265,   7566,   7875,   8192,   8515,   8847,   9186,   9532,   9886,
    10249,  10619,  10996,  11382,  11776,  12178,  12588,  13006,  13433,  13867,
    14310,  14762,  15222,  15690,  16167,  16652,  17146,  17649,  18161,  18681,
    19210,  19748,  20295,  20851,  21417,  21991,  22574,  23166,  23768,  24379,
    25000,  25629,  26268,  26917,  27575,  28243,  28920,  29607,  30303,  31010,
    31726,  32452,  33188,  33934,  34689,  35455,  36231,  37017,  37813,  38619,
    39436,  40262,  41099,  41947,  42804,  43673,  44551,  45441,  46340,  47251,
    48172,  49104,  50046,  50999,  51963,  52938,  53924,  54921,  55929,  56947,
    57977,  59018,  60070,  61133,  62208,  63293,  64390,  65498,  66618,  67749,
    68891,  70045,  71211,  72388,  73576,  74777,  75989,  77212,  78448,  79695,
    80954,  82225,  83507,  84802,  86109,  87427,  88758,  90101,  91456,  92823,
    94202,  95593,  96997,  98413,  99841,  101282, 102735, 104201, 105679, 107169,
    108672, 110188, 111716, 113257, 114811, 116377, 117956, 119548, 121153, 122770,
    124401, 126044, 127700, 129369, 131052, 132747, 134456, 136177, 137912, 139660,
    141421, 143195, 144983, 146784, 148598, 150426, 152267, 154122, 155990, 157872,
    159767,
};

static u32 GetNumSentOutMons()
{
    u32 numSentOut = 0;
    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        if (gBattleStruct->partyState[B_SIDE_PLAYER][i].sentOut)
        {
            numSentOut++;
        }
    }
    return numSentOut;
}

enum Rec_XpState {
    XP_DONE = -2,
    XP_WAIT = -1,
    XP_INIT = 0,
    XP_SET_AND_PRINT,
    XP_UPDATE,
    XP_LVL_UP,
    XP_LOOP,
    XP_FINISH,
};

static bool32 RunExpHandler(ExpHandlerFunc func, u8* expMonId)
{
    enum Rec_XpState state = func(expMonId);

    if (state == XP_DONE)
        return TRUE;

    if (state > XP_WAIT)
        gBattleScripting.getexpState = state;

    return FALSE;
}

static const ExpHandlerFunc ExpHandlerFuncs[] =
{
    [XP_INIT]          = HandleExpInit,
    [XP_SET_AND_PRINT] = HandleSetAndPrint,
    [XP_UPDATE]        = HandleExpUpdate,
    [XP_LVL_UP]        = HandleLevelUp,
    [XP_LOOP]          = HandleExpLoop,
    [XP_FINISH]        = HandleExpFinish,
};

void BS_givepartyexp(void)
{
    NATIVE_ARGS();

    if (RunExpHandler(ExpHandlerFuncs[gBattleScripting.getexpState], &gBattleStruct->expGetterMonId))
    {
        gBattlescriptCurrInstr = cmd->nextInstr;
        return;
    }
}

// Exp Handler Functions

static s32 HandleExpInit(u8 *expMonId)
{
    *expMonId = gBattlerPartyIndexes[GetBattlerAtPosition(B_POSITION_PLAYER_LEFT)];
    u32 currentBattler = *expMonId;
    gBattleStruct->expGettersOrder[0] = currentBattler;

    u32 expOrder = 1;
    for (u32 i = 0; i < PARTY_SIZE; i++)
    {
        if (i == currentBattler)
            continue;

        gBattleStruct->expGettersOrder[expOrder++] = i;
    }
    return XP_SET_AND_PRINT;
}

static s32 HandleSetAndPrint(u8* expMonId)
{
    if (gBattleControllerExecFlags != 0)
        return XP_WAIT;

    struct Pokemon *expGetter = &gParties[B_TRAINER_PLAYER][*expMonId];

    bool32 isEggOrMaxLevel = GetMonData(expGetter, MON_DATA_SPECIES_OR_EGG) == SPECIES_EGG
                          || GetMonData(expGetter, MON_DATA_LEVEL) == MAX_LEVEL;

    if (isEggOrMaxLevel)
    {
        gBattleStruct->battlerExpReward = 0;
        return XP_LOOP;
    }

    if (!IsValidForBattle(expGetter))
        return XP_UPDATE;

    gBattleStruct->battlerExpReward = ComputeExpRewardForBattler(*expMonId, gBattlerFainted, gBattleStruct->expBuffer);

    s32 stringId = IsTradedMon(expGetter) ? STRINGID_ABOOSTED : STRINGID_EMPTYSTRING4;

    PREPARE_MON_NICK_WITH_PREFIX_BUFFER(gBattleTextBuff1, 0, *expMonId);
    PREPARE_STRING_BUFFER(gBattleTextBuff2, stringId);
    PREPARE_WORD_NUMBER_BUFFER(gBattleTextBuff3, 6, gBattleStruct->battlerExpReward);

    if (gBattlerPartyIndexes[GetBattlerAtPosition(B_POSITION_PLAYER_LEFT)] == *expMonId)
    {
        PrepareStringBattle(STRINGID_PKMNGAINEDEXP, 0);
    }
    else if (!gBattleStruct->teamGotExpMsgPrinted)
    {
        PrepareStringBattle(STRINGID_TEAMGAINEDEXP, 0);
        gBattleStruct->teamGotExpMsgPrinted = TRUE;
    }

    return XP_UPDATE;
}

static s32 HandleExpUpdate(u8* expMonId)
{
    if (gBattleControllerExecFlags != 0)
        return XP_WAIT;

    struct Pokemon* expGetter = &gParties[B_TRAINER_PLAYER][*expMonId];

    gBattleResources->bufferB[0][0] = 0;
    u32 currLvl = GetMonData(expGetter, MON_DATA_LEVEL);

    if (!GetMonData(expGetter, MON_DATA_HP) || currLvl == MAX_LEVEL)
        return XP_LVL_UP;

    gBattleResources->beforeLvlUp->stats[STAT_HP]    = GetMonData(expGetter, MON_DATA_MAX_HP);
    gBattleResources->beforeLvlUp->stats[STAT_ATK]   = GetMonData(expGetter, MON_DATA_ATK);
    gBattleResources->beforeLvlUp->stats[STAT_DEF]   = GetMonData(expGetter, MON_DATA_DEF);
    gBattleResources->beforeLvlUp->stats[STAT_SPEED] = GetMonData(expGetter, MON_DATA_SPEED);
    gBattleResources->beforeLvlUp->stats[STAT_SPATK] = GetMonData(expGetter, MON_DATA_SPATK);
    gBattleResources->beforeLvlUp->stats[STAT_SPDEF] = GetMonData(expGetter, MON_DATA_SPDEF);
    gBattleResources->beforeLvlUp->level = currLvl;
    gBattleResources->beforeLvlUp->learnMultipleMoves = FALSE;

    BtlController_EmitExpUpdate(0, B_COMM_TO_CONTROLLER, *expMonId, gBattleStruct->battlerExpReward);
    MarkBattlerForControllerExec(0);
    return XP_LVL_UP;
}

static s32 HandleLevelUp(u8* expMonId)
{
    if (gBattleControllerExecFlags != 0)
        return XP_WAIT;

    if (gBattleResources->bufferB[0][0] != CONTROLLER_TWORETURNVALUES || gBattleResources->bufferB[0][1] != RET_VALUE_LEVELED_UP)
    {
        gBattleStruct->battlerExpReward = 0;
        return XP_LOOP;
    }

    enum BattlerId battler = GetBattlerFromPlayerPartyId(*expMonId);

    struct Pokemon* expGetter = &gParties[B_TRAINER_PLAYER][*expMonId];

    if (gBattleTypeFlags & BATTLE_TYPE_TRAINER && battler != MAX_BATTLERS_COUNT)
        HandleLowHpMusicChange(GetBattlerMon(battler), battler);

    PREPARE_MON_NICK_WITH_PREFIX_BUFFER(gBattleTextBuff1, 0, *expMonId);
    PREPARE_BYTE_NUMBER_BUFFER(gBattleTextBuff2, 3, GetMonData(expGetter, MON_DATA_LEVEL));

    gLeveledUpInBattle |= 1 << *expMonId;
    BattleScriptCall(BattleScript_LevelUp);
    gBattleStruct->battlerExpReward = T1_READ_32(&gBattleResources->bufferB[0][2]);
    AdjustFriendship(&gParties[B_TRAINER_PLAYER][*expMonId], FRIENDSHIP_EVENT_GROW_LEVEL);

    // update battle mon structure after level up
    if (battler != MAX_BATTLERS_COUNT)
    {
        if (gBattleMons[battler].volatiles.transformed)
        {
            gBattleMons[battler].level = GetMonData(&gParties[B_TRAINER_PLAYER][*expMonId], MON_DATA_LEVEL);
            gBattleMons[battler].hp = GetMonData(&gParties[B_TRAINER_PLAYER][*expMonId], MON_DATA_HP);
        }
        else
        {
            CopyMonLevelAndBaseStatsToBattleMon(battler, &gParties[B_TRAINER_PLAYER][*expMonId], TRUE);
        }
        if (gBattleMons[battler].volatiles.powerTrick)
            Swap(gBattleMons[battler].attack, gBattleMons[battler].defense);
    }

    return XP_LOOP;
}

static s32 HandleExpLoop(u8* expMonId)
{
    if (gBattleStruct->battlerExpReward)
    {
        return XP_UPDATE;
    }

    if ((++gBattleStruct->expOrderId) < PARTY_SIZE)
    {
        *expMonId = gBattleStruct->expGettersOrder[gBattleStruct->expOrderId];
        if (*expMonId < PARTY_SIZE)
        {
            return XP_SET_AND_PRINT;
        }
    }
    return XP_FINISH;
}

static s32 HandleExpFinish(u8* expMonId)
{
    if (gBattleControllerExecFlags != 0)
        return XP_WAIT;

    gBattleStruct->expOrderId = 0;
    gBattleStruct->teamGotExpMsgPrinted = FALSE;
    return XP_DONE;
}

// Helper Functions

static void ApplyExperienceMultipliers(s32 *expAmount, u8 expGetterMonId, u8 faintedBattler)
{
    enum HoldEffect holdEffect = GetMonHoldEffect(&gParties[B_TRAINER_PLAYER][expGetterMonId]);

    if (IsTradedMon(&gParties[B_TRAINER_PLAYER][expGetterMonId]))
        *expAmount = (*expAmount * 150) / 100;
    if (holdEffect == HOLD_EFFECT_LUCKY_EGG)
        *expAmount = (*expAmount * 150) / 100;
    if (GetConfig(B_UNEVOLVED_EXP_MULTIPLIER) >= GEN_6 && IsMonPastEvolutionLevel(&gParties[B_TRAINER_PLAYER][expGetterMonId]))
        *expAmount = (*expAmount * 4915) / 4096;
    if (GetConfig(B_AFFECTION_MECHANICS) == TRUE && GetMonAffectionHearts(&gParties[B_TRAINER_PLAYER][expGetterMonId]) >= AFFECTION_FOUR_HEARTS)
        *expAmount = (*expAmount * 4915) / 4096;
    if (CheckBagHasItem(ITEM_EXP_CHARM, 1)) //is also for other exp boosting Powers if/when implemented
        *expAmount = (*expAmount * 150) / 100;

    //Catchup EXP
    {
        s8 levelDiff = GetHighestMonLevel() - GetMonData(&gParties[B_TRAINER_PLAYER][expGetterMonId], MON_DATA_LEVEL);

        if (levelDiff < 0)
            levelDiff = 0;

        if (levelDiff > 15)
            levelDiff = 15;

        *expAmount = (*expAmount * sCatchUpExpFactors[levelDiff]) / 100;
    }
    if (GetConfig(B_SCALED_EXP) >= GEN_5 && GetConfig(B_SCALED_EXP) != GEN_6)
    {
        // Note: There is an edge case where if a Pokémon receives a large amount of exp, it wouldn't be properly calculated
        //       because of multiplying by scaling factor(the value would simply be larger than an u32 can hold). Hence u64 is needed.
        u64 value = *expAmount;
        u8 faintedLevel = gBattleMons[faintedBattler].level;
        u8 expGetterLevel = GetMonData(&gParties[B_TRAINER_PLAYER][expGetterMonId], MON_DATA_LEVEL);

        value *= sExperienceScalingFactors[(faintedLevel * 2) + 10];
        value /= sExperienceScalingFactors[faintedLevel + expGetterLevel + 10];

        *expAmount = value + 1;
    }
}

static enum BattlerId GetBattlerFromPlayerPartyId(u32 partyId)
{
    if (gBattlerPartyIndexes[B_BATTLER_0] == partyId && !(gAbsentBattlerFlags & 1))
        return B_BATTLER_0;
    if (!IsDoubleBattle())
        return MAX_BATTLERS_COUNT;
    if (!BattlerIsPlayer(B_BATTLER_2))
        return MAX_BATTLERS_COUNT;
    if (gBattlerPartyIndexes[B_BATTLER_2] == partyId && !(gAbsentBattlerFlags & 4))
        return B_BATTLER_2;
    return MAX_BATTLERS_COUNT;
}

static enum HoldEffect GetMonHoldEffect(struct Pokemon *mon)
{
    enum Item item = GetMonData(mon, MON_DATA_HELD_ITEM);
    return GetItemHoldEffect(item);
}

static inline u8 GetHighestMonLevel(void)
{
    u32 i, level;
    u8 highestLevel = 0;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_SPECIES) != SPECIES_NONE)
        {
            level = GetMonData(&gParties[B_TRAINER_PLAYER][i], MON_DATA_LEVEL);
            if(level > highestLevel)
                highestLevel = level;
        }
    }

    return highestLevel;
}

static u32 ComputeExpRewardForBattler(u32 expMonId, enum BattlerId fainterBattler, u32 totalExp)
{
    s32 reward = 0;
    u32 numSentOut = GetNumSentOutMons();
    u32 baseExp = totalExp / numSentOut;

    struct Pokemon* expGetter = &gParties[B_TRAINER_PLAYER][expMonId];

    if (gBattleStruct->partyState[B_TRAINER_PLAYER][expMonId].sentOut)
        reward = GetSoftLevelCapExpValue(expGetter->level, baseExp);
    else
        reward = 0;

    if (WasPartyMonSentOut(B_TRAINER_PLAYER, expMonId))
        reward = GetSoftLevelCapExpValue(expGetter->level, baseExp);

    ApplyExperienceMultipliers(&reward, expMonId, fainterBattler);

    if (B_EXP_CAP_TYPE == EXP_CAP_HARD && reward != 0)
        return reward;

    enum GrowthRate growthRate = gSpeciesInfo[GetMonData(expGetter, MON_DATA_SPECIES)].growthRate;
    u32 currentExp = GetMonData(expGetter, MON_DATA_EXP);
    u32 levelCap = GetCurrentLevelCap();

    if (GetMonData(expGetter, MON_DATA_LEVEL) >= levelCap)
        reward = 0;
    else if (gExperienceTables[growthRate][levelCap] < currentExp + reward)
        reward = gExperienceTables[growthRate][levelCap] - currentExp;

    return reward;
}

static inline bool32 WasPartyMonSentOut(enum BattleTrainer trainer, u32 partyIndex)
{
    return gBattleStruct->partyState[trainer][partyIndex].sentOut;
}
