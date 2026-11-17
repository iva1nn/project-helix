/* Recomp Achievements: helix-native event fires matching achievements/fe1.json and fe2.json.
 *
 * Conditions come from this port's own RAM map (options/stats next to gameopt, character records, overlay
 * ident, HUD text, actor HP). Not RetroAchievements MemAddr strings.
 *
 * Verified against keep-state RAM (playtest I_stats / I_virus / Ibad / A_tapeB / d4_choice):
 *   FE1/FE2 in-game time = options block + 8 (FE1 0x8007780C, FE2 0x8007B91C), 30 Hz ticks
 *     (I_stats 16743 ticks = 9:18 at 30 Hz, matching the on-screen Game Time).
 *   FE2 phone-save count = 0x8007B924 (A_tapeB 17, B_party 2, I_virus 0 then I_stats 1 after the clear save).
 *   FE2 combat totals in the same block: 0x8007B928 kills, 0x8007B92C sneak kills.
 *   FE2 good route = 0x80082B04 (1 after the DNA Virus on Mist); bad = 0x8007B93C.
 *   FE2 live items = Code Master GameShark + 0x438 (Hana 0x8008C015; Metal Hook 0x8008C025).
 *   FE2 weapons: character record 0x8008BF50 + 0x100*who, owned at +0x18+8k (Uzi is k=2).
 *   FE1 items: Hana record 0x8008DF00 + 0x64.
 *   FE2 big-head / Helium = cheat word 0x8007B4F8 bit 7 (the game's own panel bit). */
#include "hw.h"
#include "gameopt.h"
#include "rl_achievements.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

extern int cheat_health, cheat_ammo;
int cheats_any_on(void);
int controls_player_info(uint32_t *actor, int32_t pos[3]);
int fonts_dialogue_char(int c);

#ifndef FE_BUILD_ID
#define FE_BUILD_ID "dev"
#endif

#define FE1 (strcmp(FE_GAME, "fe1") == 0)
#define FE1_MODULE 0x80082D6Cu
#define FE1_WANTED 0x80082BE8u
#define FE2_WANTED 0x80082E94u
#define FE1_AREA   0x8008C740u
#define FE2_AREA   0x8008A828u
#define ACTOR_HP   0x68u
#define FE1_TIME   0x8007780Cu
#define FE2_TIME   0x8007B91Cu
#define FE2_SAVES  0x8007B924u
#define FE2_KILLS  0x8007B928u
#define FE2_SNEAK  0x8007B92Cu
#define FE2_BAD    0x8007B93Cu
#define FE2_GOOD   0x80082B04u
#define FE2_CHEAT  0x8007B4F8u
#define FE1_ITEMS  0x8008DF64u
#define FE2_ITEMS  0x8008C015u
#define FE2_REC    0x8008BF50u
#define TICK_MIN   1800u /* 30 Hz ticks per minute (I_stats Game Time) */

static uint32_t ach_read(uint32_t address, uint8_t *buf, uint32_t n, void *ud) {
    uint32_t got = 0;
    (void)ud;
    while (got < n) {
        uint32_t a = address + got, chunk;
        if (a < PSX_RAM_SIZE) {
            chunk = n - got;
            if (a + chunk > PSX_RAM_SIZE) chunk = PSX_RAM_SIZE - a;
            memcpy(buf + got, psx_ram + a, chunk);
            got += chunk;
        } else if (a >= 0x200000u && a < 0x200400u) {
            uint32_t off = a - 0x200000u;
            chunk = n - got;
            if (off + chunk > 0x400u) chunk = 0x400u - off;
            memcpy(buf + got, psx_scratch + off, chunk);
            got += chunk;
        } else {
            break;
        }
    }
    return got;
}

static int casual_cheats, dbg;
static char fired[160][80];
static int nfired;
static char last_ov[16], play_ov[16];
static int last_swap, last_gallery, last_hit, last_hp = -1, last_disc = -1;
static uint32_t loc_frames, disc_t0, sneak0, kills0;
static int catch_up;
static uint8_t last_good, last_bad, last_head;
static uint8_t last_it[64], last_wp[4][17];

static int already(const char *e) {
    for (int i = 0; i < nfired; i++) if (!strcmp(fired[i], e)) return 1;
    return 0;
}

static void fire(const char *e) {
    if (!e || !e[0] || already(e)) return;
    if (nfired < (int)(sizeof fired / sizeof fired[0]))
        snprintf(fired[nfired++], sizeof fired[0], "%s", e);
    LOG("achievements: event %s (frame %llu)\n", e, (unsigned long long)frame_count);
    rl_ach_event(e);
}

static void fire_hard(const char *e, const char *hard) {
    fire(e);
    if (hard && gopt_get(GO_DIFFICULTY) == 1) fire(hard);
}

static void fire_ok(const char *e, const char *perfect) {
    fire(e);
    if (perfect && !last_hit) fire(perfect);
}

static uint32_t game_time(void) { return mem_r32(FE1 ? FE1_TIME : FE2_TIME); }
static uint32_t fe2_saves(void) { return FE1 ? ~0u : mem_r32(FE2_SAVES); }
static uint32_t fe2_kills(void) { return FE1 ? 0 : mem_r32(FE2_KILLS); }
static uint32_t fe2_sneak(void) { return FE1 ? 0 : mem_r32(FE2_SNEAK); }

static const char *overlay_now(void) {
    uint32_t base = 0, w;
    for (int i = 0; i < recomp_image_count; i++)
        if (recomp_images[i].is_overlay) { base = recomp_images[i].base; break; }
    if (!base) return "";
    memcpy(&w, &psx_ram[base & 0x1FFFFC], 4);
    for (int i = 0; i < recomp_image_count; i++) {
        const ImageEntry *im = &recomp_images[i];
        if (im->is_overlay && im->ident == w) return im->name;
    }
    return "";
}

static int swapping(const char *ov) {
    if (FE1) return mem_r32(FE1_MODULE) == 9u;
    return ov[0] && !strcmp(ov, "SWAP");
}

static int wanted_disc(void) { return (int)(int8_t)mem_r8(FE1 ? FE1_WANTED : FE2_WANTED); }

static int room_now(void) {
    uint32_t area = FE1 ? FE1_AREA : FE2_AREA, p = mem_r32(area + 0x5Cu);
    if ((p & 0xFFE00000u) != 0x80000000u) return -1;
    return (int)mem_r32(area + 0x80u);
}

static int player_hp(void) {
    uint32_t actor;
    int32_t pos[3];
    if (controls_player_info(&actor, pos) < 0 || !actor) return -1;
    return (int)mem_r8(actor + ACTOR_HP);
}

static int loc_overlay(const char *ov) {
    return ov[0] == 'L' && ov[1] == 'O' && ov[2] == 'C' && ov[3] && !ov[4];
}

static int ov_is(const char *ov, const char *want) { return want && ov[0] && !strcmp(ov, want); }

static int detected(void) {
    if (FE1) return (mem_r16(0x80082C42u) & 0x200u) != 0; /* red target-icon bit (GameShark) */
    return 0;
}

/* same-disc chapter changes (LOC* -> LOC* / SWAP / CREDITS). Disc-end SWAP also fires the bundle below. */
static void overlay_edge(const char *from, const char *to) {
    uint32_t dk, ds;
    if (!from[0] || !to[0] || !strcmp(from, to)) return;
    dk = fe2_kills() - kills0;
    ds = fe2_sneak() - sneak0;
    if (FE1) {
        if (disc_current == 3 && !strcmp(to, "GSHELL") &&
            (!strcmp(from, "LOCF") || !strcmp(from, "LOCG"))) {
            fire("fear_effect_cd4");
            if (gopt_get(GO_DIFFICULTY) == 1) fire("a_hot_night");
            else if (!already("paper_doll")) fire("now_i_have_ruined_everything");
            if (game_time() - disc_t0 <= 25u * TICK_MIN) {
                fire("fear_effect_fast_cd4_hana");
                fire("fear_effect_fast_cd4_glas");
            }
        }
        if (disc_current == 0 && !strcmp(from, "LOCA") && !last_hit)
            fire("joining_the_party");
        if (disc_current == 1 && !strcmp(from, "LOCD") && !last_hit)
            fire("walking_dead");
        if (disc_current == 1 && !strcmp(from, "LOCH") && !last_hit)
            fire("left_4_dead");
        if (disc_current == 2 && !strcmp(from, "LOCE") && !detected())
            fire("femme_fatale_ii");
        return;
    }
    if (!strcmp(from, "LOCA") && dk >= 5 && !last_hit) fire("mighty_foot_engaged");
    if (!strcmp(from, "LOCB") && ds >= 10) fire("metal_gear_rain");
    if (!strcmp(from, "LOCB") && ds >= 1 && !detected()) fire("that_even_beats_my_last_shareholders_meeting");
    if (!strcmp(from, "LOCE") && dk >= 20) fire("say_hello_to_my_little_friends");
    if (!strcmp(from, "LOCD") && dk >= 10 && !last_hit) fire("s_mart_s_top_of_the_line");
    if (!strcmp(from, "LOCB") && (!strcmp(to, "LOCH") || !strcmp(to, "SWAP")))
        fire_hard("hell_of_a_diversion", "party_hard");
    if (!strcmp(from, "LOCH") && !strcmp(to, "SWAP"))
        fire_hard("mission_accomplished_right", "heist_of_the_century");
    if (!strcmp(from, "LOCE") && (!strcmp(to, "LOCI") || !strcmp(to, "SWAP"))) {
        fire("we_re_not_using_the_z_word");
        fire_hard("can_t_we_all_just_get_along", "oh_you_re_an_immortal_then");
    }
    if (!strcmp(from, "LOCI") && !strcmp(to, "SWAP"))
        fire_hard("dead_men_tell_no_tales", "you_are_indeed_a_worthy_opponent");
    if (!strcmp(from, "LOCD") && (!strcmp(to, "LOCF") || !strcmp(to, "SWAP") || !strcmp(to, "CREDITS")))
        fire_hard("war_war_never_changes", "time_is_a_flat_circle");
    if (!strcmp(from, "LOCF") && (!strcmp(to, "CREDITS") || !strcmp(to, "SWAP"))) {
        fire_hard("the_time_for_change_is_upon_us", "ascension_to_enlightenment");
        if (!already("violence_begets_violence")) fire("mercy_begets_mercy");
    }
}

static void maybe_speed_and_saves(void) {
    if (FE1) return;
    if (game_time() <= 150u * TICK_MIN)
        fire("cutscenes_where_we_re_going_we_don_t_need_cutscenes");
    if (fe2_saves() <= 8u) fire("telephobia");
}

/* finishing a disc: SWAP (wanted != current) or FE2 CREDITS. Rising-edge only, so a disc-4 save does not grant CD1. */
static void disc_done(int done) {
    uint32_t dt;
    if (done < 0 || done > 3) return;
    dt = game_time() - disc_t0;
    if (FE1) {
        if (done == 0) {
            fire("fear_effect_cd1");
            if (dt <= 25u * TICK_MIN) fire("fear_effect_fast_cd1");
        }
        if (done == 1) {
            fire("fear_effect_cd2");
            if (dt <= 25u * TICK_MIN) fire("fear_effect_fast_cd2");
        }
        if (done == 2) {
            fire("fear_effect_cd3");
            if (dt <= 35u * TICK_MIN) fire("fear_effect_fast_cd3");
        }
        return;
    }
    if (done == 0) fire_hard("so_are_we_going_to_talk_about_that_machine", "tough_kitties");
    if (done == 1) {
        fire_hard("hell_of_a_diversion", "party_hard");
        fire_hard("mission_accomplished_right", "heist_of_the_century");
    }
    if (done == 2) {
        fire_hard("can_t_we_all_just_get_along", "oh_you_re_an_immortal_then");
        fire_hard("dead_men_tell_no_tales", "you_are_indeed_a_worthy_opponent");
    }
    if (done == 3) {
        fire_hard("war_war_never_changes", "time_is_a_flat_circle");
        fire_hard("the_time_for_change_is_upon_us", "ascension_to_enlightenment");
        maybe_speed_and_saves();
    }
}

static int has_ci(const char *s, const char *n) {
    size_t ns = strlen(s), nn = strlen(n);
    if (nn == 0 || nn > ns) return 0;
    for (size_t i = 0; i + nn <= ns; i++) {
        size_t k = 0;
        while (k < nn && toupper((unsigned char)s[i + k]) == toupper((unsigned char)n[k])) k++;
        if (k == nn) return 1;
    }
    return 0;
}

static void match_text(const char *t) {
    int n = (int)strlen(t);
    const char *ov = overlay_now();
    if (n < 4 || n > 48) return;
    if (dbg) {
        static char seen[48][64];
        static int nseen;
        int i;
        for (i = 0; i < nseen; i++) if (!strcmp(seen[i], t)) break;
        if (i == nseen && nseen < 48) {
            snprintf(seen[nseen++], sizeof seen[0], "%s", t);
            LOG("achievements: text \"%s\" ov=%s room=%d\n", t, ov, room_now());
        }
    }
    if (FE1) {
        if (has_ci(t, "PO MON")) fire_ok("undressed_to_kill", "undressed_to_kill_perfect");
        if (has_ci(t, "STONE SWORD")) fire("stone_sword");
        if (has_ci(t, "WHITE GUARD")) fire("the_white_guard_of_impermanence");
        if (has_ci(t, "HOT OIL")) fire("master_chef");
        if (n <= 24 && has_ci(t, "HELL") && has_ci(t, "GATE") && loc_overlay(ov)) fire("hell_s_gate");
        if (has_ci(t, "WIRE CUT")) fire("i_thought_i_was_a_dead_man");
        if (has_ci(t, "SIGN KEY")) fire("triad_mobster");
        if (has_ci(t, "WET TOWEL") || has_ci(t, "HOT TOWEL")) {
            fire("more_stunning_than_a_flash_grenade");
            if (!last_hit) fire("they_are_too_distracted_to_hit_me");
        }
        if (has_ci(t, "TRAIN KEY")) fire("all_aboard");
        if (has_ci(t, "LADDER") && has_ci(t, "CARD")) fire("electrician");
        if (has_ci(t, "MUSIC BOX") || (has_ci(t, "PUPPET") && loc_overlay(ov))) fire("puppet_show");
        if (has_ci(t, "WHITE VASE") || has_ci(t, "BLACK VASE")) {
            fire("florist");
            if (!detected()) fire("femme_fatale_iii");
        }
        if (has_ci(t, "PAPER DOLL")) fire("paper_doll");
        if (has_ci(t, "PAPER MONEY") || has_ci(t, "PAPER MADAM")) fire("madam_chen_perfect");
        if ((has_ci(t, "STONE TREE") || has_ci(t, "STONE WILLOW")) && !last_hit) fire("fight_in_hell");
        if (has_ci(t, "STONE SCROLL")) fire("from_womb_to_tomb");
        if (has_ci(t, "GATE KEY") && disc_current == 3) fire("beyond_the_hell_s_gate");
        if (has_ci(t, "NEW DESIRE") || (has_ci(t, "DRESS") && disc_current == 2)) {
            fire_ok("make_yourself_comfortable", "fashionista");
            if (!detected()) fire("femme_fatale_i");
        }
        if (has_ci(t, "MR. LAM") || has_ci(t, "MR LAM") || has_ci(t, "MISTER LAM"))
            fire_ok("the_great_escape", NULL);
        if (has_ci(t, "HELICOPTER")) fire_ok("helicopter_shooter", "helicopter_shooter_perfect");
        if (has_ci(t, "MACHINE GUN")) fire_ok("the_train_mercenary", "the_train_mercenary_perfect");
        if (has_ci(t, "DEMON")) fire_ok("demons_on_the_ceiling", "demons_on_the_ceiling_perfect");
        if (has_ci(t, "GHOST")) fire_ok("ghost", "ghost_perfect");
        if (has_ci(t, "DRAGON")) fire("drakologist");
        if (has_ci(t, "BED")) fire_ok("the_mercenary_under_the_bed", "the_mercenary_under_the_bed_perfect");
        if (has_ci(t, "WET FLOOR")) fire("caution_wet_floor");
        if (has_ci(t, "ZOMBIE") && !last_hit) fire("left_4_dead");
        return;
    }
    if (has_ci(t, "VIDEOTAPE")) fire("i_have_to_return_some_videotapes");
    if (has_ci(t, "LILY COIN")) fire("seven_days");
    if (has_ci(t, "SACRED SEAL")) fire("reduced_to_rubble");
    if (has_ci(t, "EMP") && ov_is(ov, "LOCA")) fire("blackout");
    if (has_ci(t, "METAL HOOK")) fire("no_need_for_a_plumber");
    if (has_ci(t, "BLASTING")) fire("i_m_a_professional_i_swear");
    if (has_ci(t, "FLASH DISK") || has_ci(t, "HACKING")) {
        if (game_time() - disc_t0 <= 3u * TICK_MIN) fire("the_fastest_hacker_in_the_east");
    }
    if (has_ci(t, "DNA DISK") || has_ci(t, "FINGERPRINT")) fire("piece_of_cake");
    if (has_ci(t, "JIN") && has_ci(t, "PACK")) fire("this_jin_fellow_certainly_seems_trustworthy");
    if (has_ci(t, "BIOLAB")) fire("rain_s_kennedy");
    if (has_ci(t, "MOTION") && has_ci(t, "SENSOR")) fire("it_s_all_very_technical");
    if (has_ci(t, "BLOOD SAMPLE") || has_ci(t, "DNA VIRUS")) fire("the_future_is_yours");
    if (has_ci(t, "SS-2000") || has_ci(t, "SS 2000")) fire("the_roof_is_on_fire");
    if (has_ci(t, "CHUNG")) fire("violence_begets_violence");
    if (has_ci(t, "SONIC BOOM")) {
        if (ov_is(ov, "LOCE") && !last_hit) fire("my_hero");
        fire("boom_goes_the_dynamite");
    }
    if (has_ci(t, "STEAM") && !last_hit) fire("no_sweat");
    if (has_ci(t, "PLANNER") && !last_hit) fire("the_best_laid_plans");
    if (has_ci(t, "CIRCUIT")) fire("overclocked");
    if (has_ci(t, "BANNER")) fire("if_only_hana_were_here");
    if (has_ci(t, "TASER") || has_ci(t, "ARC ")) fire("chain_lightning");
    if (has_ci(t, "ELEVATOR")) fire("i_ll_have_what_he_s_having");
    if (has_ci(t, "GREEN DOT")) fire("pale_green_dot");
    if (has_ci(t, "BIO BOSS") && !last_hit) fire("employee_of_the_month");
    if (has_ci(t, "SHOTGUN")) fire("loading_up_the_boomstick");
    if (has_ci(t, "RL-480") || has_ci(t, "RL 480")) fire("nemesis_wannabes");
    if (has_ci(t, "BULL") && has_ci(t, "SKULL")) fire("wheel_of_fortune");
    if (has_ci(t, "GOLEM")) fire("expedited_demolition");
    if (has_ci(t, "DOPPEL")) fire("post_traumatic_speedrun_disorder");
    if (has_ci(t, "M.A.W") || has_ci(t, "MAW")) fire("adding_insult_to_injury");
    if (has_ci(t, "GENERAL") || has_ci(t, "SOLDIER UNIT") || has_ci(t, "CAPTURE THE FLAG")) {
        static const char *chess[4] = {
            "be_where_your_enemy_is_not",
            "great_results_can_be_achieved_with_small_forces",
            "patience_is_the_sharpest_blade",
            "he_who_wishes_to_fight_must_first_count_the_cost"};
        static int nchess;
        if (nchess < 4) fire(chess[nchess++]);
    }
}

void ach_on_text(uint32_t s) {
    char t[64];
    int n = 0, c;
    if (!s) return;
    for (; n < 63 && (c = (int)mem_r8(s + (uint32_t)n)) != 0; n++) {
        int d = fonts_dialogue_char(c);
        t[n] = (d < 32 || d > 126) ? '?' : (char)d;
    }
    while (n && t[n - 1] == ' ') n--;
    t[n] = 0;
    match_text(t);
}

void ach_feed_text(const char *s) {
    char t[64];
    int n = 0;
    if (!s) return;
    for (; n < 63 && s[n]; n++) t[n] = s[n];
    while (n && t[n - 1] == ' ') n--;
    t[n] = 0;
    match_text(t);
}

void ach_fire_named(const char *name) { fire(name); }

void ach_reset_fired(void) { nfired = 0; memset(fired, 0, sizeof fired); }

int ach_fired_n(void) { return nfired; }

const char *ach_fired_at(int i) { return (i >= 0 && i < nfired) ? fired[i] : ""; }

typedef struct {
    int disc;
    const char *ov;
    uint32_t addr;
    const char *ev, *perfect;
} ItemRise;

/* Live item bytes: FE1 Hana +0x64; FE2 Hana 0x8008C015 (GS+0x438). disc -1 = any. */
static const ItemRise ITEMS[] = {
    /* FE1 disc 1 */
    {0, "LOCA", 0x8008DF65u, "blowing_off_the_steam", NULL},
    {0, "LOCA", 0x8008DF6Au, "electric", NULL},
    {0, "LOCA", 0x8008DF6Cu, "i_thought_i_was_a_dead_man", NULL},
    {0, "LOCA", 0x8008DF66u, "triad_mobster", "triad_mobster_perfect"},
    {0, "LOCA", 0x8008DF6Du, "starting_with_a_bang", NULL},
    {0, "LOCA", 0x8008DF6Bu, "aircraft", "aircraft_perfect"},
    /* FE1 disc 2 */
    {1, NULL, 0x8008DF71u, "undressed_to_kill", "undressed_to_kill_perfect"},
    {1, NULL, 0x8008DF6Eu, "all_aboard", NULL},
    {1, NULL, 0x8008DF73u, "electrician", NULL},
    {1, NULL, 0x8008DF6Cu, "more_stunning_than_a_flash_grenade", NULL},
    /* FE1 disc 3 */
    {2, NULL, 0x8008DF70u, "make_yourself_comfortable", "fashionista"},
    {2, NULL, 0x8008DF71u, "shadows_in_the_basement", NULL},
    {2, NULL, 0x8008DF73u, "florist", NULL},
    {2, NULL, 0x8008DF76u, "madam_chen_perfect", NULL},
    {2, NULL, 0x8008DF77u, "just_two_guys_patrolling_an_oval_bedroom", NULL},
    /* FE1 disc 4 */
    {3, NULL, 0x8008DF5Au, "from_womb_to_tomb", NULL},
    {3, NULL, 0x8008DF5Bu, "a_favor_for_madam_chen", NULL},
    {3, NULL, 0x8008DF5Cu, "mystic_symbols", NULL},
    {3, NULL, 0x8008DF65u, "beyond_the_hell_s_gate", NULL},
    /* FE2 aqueduct / any */
    {-1, "LOCA", 0x8008C025u, "no_need_for_a_plumber", NULL},
    {-1, "LOCA", 0x8008C016u, "i_m_a_professional_i_swear", NULL},
    {-1, "LOCA", 0x8008C024u, "the_fastest_hacker_in_the_east", NULL},
    {-1, "LOCA", 0x8008C02Cu, "i_have_to_return_some_videotapes", NULL},
    {-1, "LOCA", 0x8008C02Du, "shut_up_and_take_my_money", NULL},
    {-1, "LOCA", 0x8008C017u, "curiosity_killed_the_cat", NULL},
    {-1, "LOCB", 0x8008C018u, "piece_of_cake", NULL},
    {-1, "LOCB", 0x8008C019u, "this_jin_fellow_certainly_seems_trustworthy", NULL},
    {-1, "LOCH", 0x8008C01Au, "out_with_the_old_in_with_the_nucleus", NULL},
    {-1, "LOCH", 0x8008C01Bu, "the_blueprint_for_life", NULL},
    {-1, "LOCE", 0x8008C01Cu, "we_re_all_friends_here", NULL},
    {-1, "LOCI", 0x8008C026u, "seven_days", NULL},
    {-1, "LOCI", 0x8008C027u, "reduced_to_rubble", NULL},
    {-1, "LOCD", 0x8008C01Du, "a_diamond_in_the_rough", NULL},
    {-1, "LOCF", 0x8008C01Eu, "now_you_re_thinking_with_portals", NULL},
    {-1, "LOCF", 0x8008C01Fu, "the_ties_that_bind", NULL},
    {-1, "LOCF", 0x8008C020u, "the_future_is_yours", NULL},
};

static void snap_items(void) {
    size_t i;
    for (i = 0; i < sizeof ITEMS / sizeof ITEMS[0]; i++) last_it[i] = mem_r8(ITEMS[i].addr);
    for (i = 0; i < 4; i++) {
        int k;
        for (k = 0; k < 17; k++)
            last_wp[i][k] = mem_r8(FE2_REC + 0x100u * (uint32_t)i + 0x18u + 8u * (uint32_t)k);
    }
}

static int item_applies(const ItemRise *it, const char *ov) {
    if (FE1 && it->addr < 0x8008DF00u) return 0;
    if (!FE1 && it->addr < 0x8008C000u) return 0;
    if (it->disc >= 0 && it->disc != disc_current) return 0;
    if (it->ov && !ov_is(ov, it->ov)) return 0;
    return 1;
}

static void watch_items(const char *ov) {
    size_t i;
    for (i = 0; i < sizeof ITEMS / sizeof ITEMS[0]; i++) {
        uint8_t now = mem_r8(ITEMS[i].addr);
        if (item_applies(&ITEMS[i], ov) && last_it[i] == 0 && now != 0) {
            if (!strcmp(ITEMS[i].ev, "the_fastest_hacker_in_the_east") &&
                game_time() - disc_t0 > 3u * TICK_MIN) {
                last_it[i] = now;
                continue;
            }
            fire_ok(ITEMS[i].ev, ITEMS[i].perfect);
            if (!strcmp(ITEMS[i].ev, "electric") && !detected()) fire("silent_aproach");
            if (!strcmp(ITEMS[i].ev, "triad_mobster") && !last_hit) fire("dodging_bullets");
            if (!strcmp(ITEMS[i].ev, "florist") && !detected()) fire("femme_fatale_iii");
        }
        last_it[i] = now;
    }
    if (!FE1) {
        uint8_t uzi_h = mem_r8(FE2_REC + 0x18u + 8u * 2u);
        uint8_t uzi_r = mem_r8(FE2_REC + 0x300u + 0x18u + 8u * 2u);
        if (last_wp[0][2] == 0 && uzi_h) fire("and_one_for_you");
        if (last_wp[3][2] == 0 && uzi_r) fire("one_for_me");
        /* k=6 showed as a found special on A_tapeB; RL-480 / SS-2000 style pickups */
        if (ov_is(ov, "LOCE") && last_wp[2][6] == 0 && mem_r8(FE2_REC + 0x200u + 0x18u + 8u * 6u))
            fire("pocket_rocket");
        if (ov_is(ov, "LOCD") && last_wp[1][6] == 0 && mem_r8(FE2_REC + 0x100u + 0x18u + 8u * 6u))
            fire("the_roof_is_on_fire");
        if (ov_is(ov, "LOCH") && last_wp[0][4] == 0 && mem_r8(FE2_REC + 0x18u + 8u * 4u))
            fire("let_s_blow_up_this_joint");
        for (i = 0; i < 4; i++) {
            int k;
            for (k = 0; k < 17; k++)
                last_wp[i][k] = mem_r8(FE2_REC + 0x100u * (uint32_t)i + 0x18u + 8u * (uint32_t)k);
        }
    }
}

static void watch_endings(const char *ov) {
    uint8_t good, bad, head;
    if (FE1) return;
    good = (uint8_t)(mem_r32(FE2_GOOD) != 0);
    bad = (uint8_t)(mem_r32(FE2_BAD) != 0);
    head = (uint8_t)(mem_r8(FE2_CHEAT) & 0x80u);
    if (good && !last_good) {
        fire("time_for_dim_sum_and_a_haircut");
        if (!last_hit) fire("southpaw_supremacy");
        if (gopt_get(GO_DIFFICULTY) == 1)
            fire("i_m_sure_this_little_runaway_job_will_be_much_easier");
        maybe_speed_and_saves();
    }
    if (bad && !last_bad) {
        fire("time_paradox");
        fire("boom_goes_the_dynamite");
        maybe_speed_and_saves();
    }
    if (!good && !bad && ov_is(ov, "CREDITS")) fire("dead_bodies_everywhere");
    if (head && !last_head) fire("helium_effect_2");
    last_good = good;
    last_bad = bad;
    last_head = head;
}

static void watch_hp(int hp) {
    if (hp >= 0 && last_hp >= 0 && hp < last_hp) last_hit = 1;
    if (hp > last_hp && last_hp >= 0) last_hit = 0; /* rush-moment refill */
    last_hp = hp;
}

static void presence(const char *ov, int hp, int room) {
    char buf[128];
    const char *diff = gopt_get(GO_DIFFICULTY) == 1 ? "Hard" : "Normal";
    if (ov[0])
        snprintf(buf, sizeof buf, "Disc %d · %s · room %d · %s", disc_current + 1, ov, room, diff);
    else
        snprintf(buf, sizeof buf, "Disc %d · %s", disc_current + 1, diff);
    if (hp >= 0) {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof buf - n, " · HP %d", hp);
    }
    rl_ach_presence(buf);
}

static void watch(void) {
    char ov[16];
    int swap, gallery, hp, room, wanted;
    snprintf(ov, sizeof ov, "%s", overlay_now());
    swap = swapping(ov);
    gallery = !FE1 && gopt_extra_running();
    hp = player_hp();
    room = room_now();
    wanted = wanted_disc();
    watch_hp(hp);

    if (disc_current != last_disc) {
        disc_t0 = game_time();
        last_disc = disc_current;
    }

    if (catch_up) {
        snprintf(last_ov, sizeof last_ov, "%s", ov);
        if (loc_overlay(ov)) snprintf(play_ov, sizeof play_ov, "%s", ov);
        last_swap = swap;
        last_gallery = gallery;
        loc_frames = 0;
        last_hit = 0;
        last_hp = hp;
        disc_t0 = game_time();
        last_disc = disc_current;
        sneak0 = fe2_sneak();
        kills0 = fe2_kills();
        last_good = (uint8_t)(mem_r32(FE2_GOOD) != 0);
        last_bad = (uint8_t)(mem_r32(FE2_BAD) != 0);
        last_head = (uint8_t)(mem_r8(FE2_CHEAT) & 0x80u);
        snap_items();
        catch_up = 0;
        presence(ov, hp, room);
        return;
    }

    watch_items(ov);
    watch_endings(ov);

    if (swap && !last_swap && wanted >= 0 && wanted <= 3 && wanted != disc_current) {
        overlay_edge(play_ov[0] ? play_ov : last_ov, "SWAP");
        disc_done(disc_current);
    } else if (!swap && ov[0] && strcmp(ov, last_ov)) {
        overlay_edge(play_ov[0] ? play_ov : last_ov, ov);
        if (!strcmp(ov, "CREDITS")) disc_done(3);
        last_hit = 0;
        sneak0 = fe2_sneak();
        kills0 = fe2_kills();
    }

    if (gallery && !last_gallery) {
        static const char *art[4] = {
            "purveyor_of_fine_art_i_beginnings_and_endings",
            "purveyor_of_fine_art_ii_tomb_raider_i_hardly_know_her",
            "purveyor_of_fine_art_iii_the_future_is_a_foreign_land",
            "purveyor_of_fine_art_iv_mission_complete_time_for_a_massage"};
        int d = disc_current;
        if (d >= 0 && d < 4) fire(art[d]);
    }

    /* bomb vest leftover: overlay word at the GameShark countdown site while LOCA is up */
    if (FE1 && ov_is(ov, "LOCA") && already("i_thought_i_was_a_dead_man")) {
        uint32_t t = mem_r32(0x800AFA2Cu);
        if (t >= 340u && t < 3600u) fire("fast_fingers"); /* 5:40 left if the word is seconds */
    }

    if (dbg && ((frame_count & 63) == 0 || swap != last_swap || strcmp(ov, last_ov)))
        LOG("achievements: disc %d ov=%s swap=%d wanted=%d room=%d hp=%d hard=%d hit=%d t=%u saves=%u\n",
            disc_current + 1, ov, swap, wanted, room, hp, gopt_get(GO_DIFFICULTY), last_hit,
            game_time(), fe2_saves());

    if (loc_overlay(ov) && !strcmp(ov, last_ov)) loc_frames++;
    else loc_frames = loc_overlay(ov) ? 1 : 0;
    if (loc_overlay(ov)) snprintf(play_ov, sizeof play_ov, "%s", ov);
    snprintf(last_ov, sizeof last_ov, "%s", ov);
    last_swap = swap;
    last_gallery = gallery;
    presence(ov, hp, room);
}

void ach_cheats_used(void) {
    if (casual_cheats) return;
    casual_cheats = 1;
    rl_ach_casual("cheats");
}

void ach_save_state_loaded(void) {
    rl_ach_casual("save_state");
    catch_up = 1;
}

void ach_memcard_loaded(void) {
    rl_ach_save_loaded();
    catch_up = 1;
}

void ach_frame(void) {
    rl_ach_frame();
    watch();
}

static void on_unlock(uint32_t id, void *ud) {
    (void)ud;
    LOG("achievements: unlocked id %u\n", id);
}

void ach_init(void) {
    const char *id = FE1 ? "project-helix--fe1" : "project-helix--fe2";
    dbg = getenv("FE_DBG_ACH") != 0;
    rl_ach_set_info(id, FE_BUILD_ID);
    rl_ach_on_unlock(on_unlock, NULL);
    LOG("achievements: %s (%s)\n", rl_ach_init("psx", ach_read, NULL) ? "connected" : "no launcher", id);
    if (cheat_health || cheat_ammo || cheats_any_on()) ach_cheats_used();
    catch_up = 1;
}
