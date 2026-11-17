"""Headless end-to-end: every catalog event fires from the running port, plus overlay / swap / gallery / HUD watchers.

    python tools/ach_e2e.py

Uses fe_port discs/data and the built fe1port/fe2port (override with FE1_EXE / FE2_EXE). One process per game,
--headless --agent --fast --no-remaster, under the machine's headless lock.
"""
from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]  # helix-github
FE_PORT = Path(r"F:\Fear Effect - Project Helix\fe_port")
JSON = {
    "fe1": REPO / "achievements" / "fe1.json",
    "fe2": REPO / "achievements" / "fe2.json",
}
GAMES = {
    "fe1": dict(
        exe=FE_PORT / "build_fe1" / "fe1port.exe",
        cue=FE_PORT / "discs" / "fe1" / "Fear Effect (USA) (Disc 1).cue",
        module=0x80082D6C,
        wanted=0x80082BE8,
        ov_base=0x800AF0A0,
        swap_ident=None,  # FE1 swap is module 9
        gallery=None,
        diff=0x80077805,
    ),
    "fe2": dict(
        exe=FE_PORT / "build" / "fe2port.exe",
        cue=FE_PORT / "discs" / "fe2" / "Fear Effect 2 - Retro Helix (USA) (Disc 1) (v1.1).cue",
        module=None,
        wanted=0x80082E94,
        ov_base=0x800AF0C8,
        swap_ident=0x0000000C,
        credits_ident=0x0000000B,
        loca_ident=0x0000000D,
        locb_ident=0x0000000E,
        loch_ident=0x00000014,
        gshell_ident=0x00000009,
        gallery=("shell",),
        diff=0x8007B915,
    ),
}
for g, d in GAMES.items():
    env_exe = os.environ.get(g.upper() + "_EXE")
    if env_exe:
        d["exe"] = Path(env_exe)


class Dead(Exception):
    pass


class Port:
    def __init__(self, game: str, work: Path, timeout=90):
        G = GAMES[game]
        self.game, self.timeout = game, timeout
        self.log = work / "run.log"
        cmd = [
            str(G["exe"]), str(G["cue"]),
            "--data", str(FE_PORT / "data"),
            "--headless", "--fast", "--agent", "--mute", "--no-remaster",
            "--renderer", "sw",
            "--log", str(self.log),
            "--memcard", str(work / "card.mcd"),
        ]
        env = dict(os.environ)
        env["FE_DBG_ACH"] = "1"
        self.p = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            cwd=str(work), text=True, bufsize=1, env=env,
        )
        self.q: queue.Queue[str | None] = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        assert self.p.stdout
        for line in self.p.stdout:
            if line.startswith(("ok", "err")):
                self.q.put(line.rstrip("\n"))
        self.q.put(None)

    def _fail(self, why: str) -> Dead:
        fatal = ""
        try:
            for l in self.log.read_text(errors="replace").splitlines():
                if l.startswith("FATAL"):
                    fatal = l
                    break
        except OSError:
            pass
        rc = self.p.poll()
        return Dead("%s: fatal=%r rc=%s log=%s" % (why, fatal, rc, self.log))

    def cmd(self, line: str, timeout=None) -> str:
        try:
            assert self.p.stdin
            self.p.stdin.write(line + "\n")
            self.p.stdin.flush()
        except OSError:
            raise self._fail("stdin closed on %r" % line)
        try:
            r = self.q.get(timeout=timeout or self.timeout)
        except queue.Empty:
            raise self._fail("timeout on %r" % line)
        if r is None:
            raise self._fail("eof on %r" % line)
        if r.startswith("err"):
            raise Dead("agent %s for %r" % (r, line))
        return r

    def step(self, n=2):
        self.cmd("step %d 0 128 128" % n, timeout=self.timeout + n / 10)

    def poke32(self, addr: int, val: int):
        self.cmd("poke %x %s" % (addr, val.to_bytes(4, "little").hex()))

    def poke8(self, addr: int, val: int):
        self.cmd("poke %x %02x" % (addr, val & 0xFF))

    def hold32(self, addr: int, val: int):
        self.cmd("hold %x %s" % (addr, val.to_bytes(4, "little").hex()))

    def hold_clear(self):
        self.cmd("hold_clear")

    def disc(self, n: int):
        self.cmd("disc %d" % n)

    def text(self, s: str):
        self.cmd("ach_text " + s)

    def events(self) -> list[str]:
        r = self.cmd("ach")
        parts = r.split(" ", 1)
        if len(parts) < 2 or not parts[1].strip():
            return []
        return [x for x in parts[1].split(",") if x]

    def close(self):
        try:
            assert self.p.stdin
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
            self.p.wait(timeout=8)
        except Exception:
            self.p.kill()


def catalog(game: str) -> list[dict]:
    data = json.loads(JSON[game].read_text(encoding="utf-8"))
    return data["achievements"]


def run_game(game: str, work: Path) -> list[str]:
    fails: list[str] = []
    achs = catalog(game)
    slugs = [a["event"] for a in achs]
    G = GAMES[game]
    print("%s: booting %s (%d events)" % (game, G["exe"].name, len(slugs)))
    p = Port(game, work)
    ingame: set[str] = set()
    try:
        p.cmd("ach")  # first vblank: enter the agent loop
        p.step(3)  # ach_init catch_up snapshots boot overlay before we poke SWAP/LOC
        p.cmd("ach_reset")
        for a in achs:
            slug = a["event"]
            p.cmd("ach_fire " + slug)
            got = p.events()
            if slug not in got:
                fails.append("%s catalog fire missing %s (have %s)" % (game, slug, ",".join(got[-5:])))
        got = p.events()
        missing = [s for s in slugs if s not in got]
        if missing:
            fails.append("%s catalog missing %d events: %s" % (game, len(missing), ",".join(missing[:8])))
        if len(got) != len(slugs):
            fails.append("%s catalog fired %d unique, expected %d" % (game, len(got), len(slugs)))
        p.cmd("ach_reset")
        got = p.events()
        if got:
            fails.append("%s ach_reset left %s" % (game, got))

        if game == "fe1":
            ingame_fe1(p, G, fails, ingame)
        else:
            ingame_fe2(p, G, fails, ingame)

        missing_in = [s for s in slugs if s not in ingame]
        if missing_in:
            fails.append("%s in-game miss %d: %s" % (game, len(missing_in), ",".join(missing_in[:12])))
    finally:
        p.close()
    log = work.joinpath("run.log").read_text(errors="replace") if work.joinpath("run.log").exists() else ""
    if "achievements:" not in log:
        fails.append("%s log has no achievements: line" % game)
    return fails


def saw(p: Port, fails: list[str], ingame: set[str], game: str, *names: str) -> set[str]:
    got = set(p.events())
    ingame |= got
    for n in names:
        if n not in got:
            fails.append("%s in-game miss %s (tail %s)" % (game, n, ",".join(sorted(got)[-8:])))
    return got


def hold_ov(p: Port, G: dict, ident: int):
    p.hold_clear()
    p.hold32(G["ov_base"], ident)


def rise8(p: Port, addr: int):
    p.poke8(addr, 0)
    p.step(2)
    p.poke8(addr, 1)
    p.step(3)


def ingame_fe1(p: Port, G: dict, fails: list[str], ingame: set[str]):
    # disc 1 SWAP (module 9) + fast clock from boot
    p.poke32(G["module"], 9)
    p.poke8(G["wanted"], 1)
    p.step(3)
    saw(p, fails, ingame, "fe1", "fear_effect_cd1", "fear_effect_fast_cd1")
    p.cmd("ach_reset")
    p.poke32(G["module"], 0)

    hold_ov(p, G, 0x0A)  # LOCA
    p.step(2)
    p.text("HELL GATE")
    saw(p, fails, ingame, "fe1", "hell_s_gate")

    for addr, slug in (
        (0x8008DF65, "blowing_off_the_steam"),
        (0x8008DF6A, "electric"),
        (0x8008DF6C, "i_thought_i_was_a_dead_man"),
        (0x8008DF66, "triad_mobster"),
        (0x8008DF6D, "starting_with_a_bang"),
        (0x8008DF6B, "aircraft"),
    ):
        rise8(p, addr)
        extra = []
        if slug == "electric":
            extra = ["silent_aproach"]
        if slug == "triad_mobster":
            extra = ["triad_mobster_perfect", "dodging_bullets"]
        if slug == "aircraft":
            extra = ["aircraft_perfect"]
        saw(p, fails, ingame, "fe1", slug, *extra)

    p.poke32(0x800AFA2C, 400)
    p.step(3)
    saw(p, fails, ingame, "fe1", "fast_fingers")

    p.hold_clear()
    p.cmd("ach_reset")
    p.text("PO MON KEY")
    p.text("STONE SWORD")
    p.text("WHITE GUARD")
    p.text("HOT OIL")
    p.text("WET TOWEL")
    p.text("TRAIN KEY")
    p.text("LADDER CARD")
    p.text("MUSIC BOX")
    p.text("WHITE VASE")
    p.text("PAPER DOLL")
    p.text("PAPER MONEY")
    p.text("STONE TREE")
    p.text("STONE SCROLL")
    p.text("NEW DESIRE")
    p.text("MR. LAM")
    p.text("HELICOPTER")
    p.text("MACHINE GUN")
    p.text("DEMON")
    p.text("GHOST")
    p.text("DRAGON")
    p.text("UNDER THE BED")
    p.text("WET FLOOR")
    p.text("ZOMBIE")
    p.text("WIRE CUTTERS")
    p.text("SIGN KEY")
    saw(p, fails, ingame, "fe1",
        "undressed_to_kill", "undressed_to_kill_perfect", "stone_sword",
        "the_white_guard_of_impermanence", "master_chef",
        "more_stunning_than_a_flash_grenade", "they_are_too_distracted_to_hit_me",
        "all_aboard", "electrician", "puppet_show", "florist", "femme_fatale_iii",
        "paper_doll", "madam_chen_perfect", "fight_in_hell", "from_womb_to_tomb",
        "make_yourself_comfortable", "fashionista", "femme_fatale_i",
        "the_great_escape", "helicopter_shooter", "helicopter_shooter_perfect",
        "the_train_mercenary", "the_train_mercenary_perfect",
        "demons_on_the_ceiling", "demons_on_the_ceiling_perfect",
        "ghost", "ghost_perfect", "drakologist",
        "the_mercenary_under_the_bed", "the_mercenary_under_the_bed_perfect",
        "caution_wet_floor", "left_4_dead")

    p.disc(2)
    p.poke32(G["module"], 9)
    p.poke8(G["wanted"], 2)
    p.step(3)
    saw(p, fails, ingame, "fe1", "fear_effect_cd2", "fear_effect_fast_cd2")
    p.poke32(G["module"], 0)
    p.cmd("ach_reset")
    hold_ov(p, G, 0x0D)  # LOCD
    p.step(2)
    hold_ov(p, G, 0x09)
    p.step(3)
    saw(p, fails, ingame, "fe1", "walking_dead")

    hold_ov(p, G, 0x11)  # LOCH
    p.step(2)
    hold_ov(p, G, 0x09)
    p.step(3)
    saw(p, fails, ingame, "fe1", "left_4_dead")

    p.disc(3)
    p.poke32(G["module"], 9)
    p.poke8(G["wanted"], 3)
    p.step(3)
    saw(p, fails, ingame, "fe1", "fear_effect_cd3", "fear_effect_fast_cd3")
    p.poke32(G["module"], 0)
    p.cmd("ach_reset")
    hold_ov(p, G, 0x0E)  # LOCE
    p.step(2)
    hold_ov(p, G, 0x09)
    p.step(3)
    saw(p, fails, ingame, "fe1", "femme_fatale_ii")

    p.disc(4)
    p.text("GATE KEY")
    saw(p, fails, ingame, "fe1", "beyond_the_hell_s_gate")
    hold_ov(p, G, 0x0A)
    rise8(p, 0x8008DF5A)
    saw(p, fails, ingame, "fe1", "from_womb_to_tomb")
    rise8(p, 0x8008DF5B)
    saw(p, fails, ingame, "fe1", "a_favor_for_madam_chen")
    rise8(p, 0x8008DF5C)
    saw(p, fails, ingame, "fe1", "mystic_symbols")
    p.hold_clear()
    p.cmd("ach_reset")  # paper_doll HUD must not block Hana's exclusive ending
    hold_ov(p, G, 0x0F)  # LOCF
    p.step(2)
    hold_ov(p, G, 0x09)  # GSHELL
    p.step(3)
    saw(p, fails, ingame, "fe1", "fear_effect_cd4", "now_i_have_ruined_everything",
        "fear_effect_fast_cd4_hana", "fear_effect_fast_cd4_glas")

    p.poke8(G["diff"], 1)
    p.cmd("ach_reset")
    hold_ov(p, G, 0x0F)
    p.step(2)
    hold_ov(p, G, 0x09)
    p.step(3)
    saw(p, fails, ingame, "fe1", "a_hot_night")

    # joining_the_party: disc 1 LOCA leave damageless
    p.disc(1)
    p.cmd("ach_reset")
    hold_ov(p, G, 0x0A)
    p.step(2)
    hold_ov(p, G, 0x09)
    p.step(3)
    saw(p, fails, ingame, "fe1", "joining_the_party")

    # disc 2 item rises
    p.disc(2)
    p.hold_clear()
    rise8(p, 0x8008DF71)
    saw(p, fails, ingame, "fe1", "undressed_to_kill")
    rise8(p, 0x8008DF6E)
    saw(p, fails, ingame, "fe1", "all_aboard")
    rise8(p, 0x8008DF73)
    saw(p, fails, ingame, "fe1", "electrician")
    rise8(p, 0x8008DF6C)
    saw(p, fails, ingame, "fe1", "more_stunning_than_a_flash_grenade")
    p.disc(3)
    rise8(p, 0x8008DF70)
    saw(p, fails, ingame, "fe1", "make_yourself_comfortable")
    rise8(p, 0x8008DF71)
    saw(p, fails, ingame, "fe1", "shadows_in_the_basement")
    rise8(p, 0x8008DF77)
    saw(p, fails, ingame, "fe1", "just_two_guys_patrolling_an_oval_bedroom")


def ingame_fe2(p: Port, G: dict, fails: list[str], ingame: set[str]):
    p.hold_clear()
    p.hold32(G["ov_base"], G["swap_ident"])
    p.poke8(G["wanted"], 1)
    p.step(3)
    saw(p, fails, ingame, "fe2", "so_are_we_going_to_talk_about_that_machine")
    p.hold_clear()
    p.cmd("ach_reset")

    hold_ov(p, G, G["loca_ident"])
    p.step(2)
    p.text("VIDEOTAPE A")
    p.text("LILY COIN")
    p.text("SACRED SEAL")
    p.text("EMP GRENADE")
    p.text("METAL HOOK")
    p.text("BLASTING CAP")
    p.text("HACKING")
    p.text("FINGERPRINT")
    p.text("JIN PACK")
    p.text("BIOLAB SUIT")
    p.text("MOTION SENSOR")
    p.text("DNA VIRUS")
    p.text("SS-2000")
    p.text("CHUNG LI")
    p.text("STEAM ROOM")
    p.text("PLANNER")
    p.text("CIRCUIT")
    p.text("BANNER")
    p.text("ARC TASER")
    p.text("ELEVATOR KEY")
    p.text("GREEN DOT")
    p.text("BIO BOSS")
    p.text("SHOTGUN SHELL")
    p.text("RL-480")
    p.text("BULL SKULL")
    p.text("GOLEM")
    p.text("DOPPELGANGER")
    p.text("M.A.W.")
    p.text("GENERAL")
    p.text("GENERAL")
    p.text("GENERAL")
    p.text("GENERAL")
    saw(p, fails, ingame, "fe2",
        "i_have_to_return_some_videotapes", "seven_days", "reduced_to_rubble", "blackout",
        "no_need_for_a_plumber", "i_m_a_professional_i_swear", "the_fastest_hacker_in_the_east",
        "piece_of_cake", "this_jin_fellow_certainly_seems_trustworthy", "rain_s_kennedy",
        "it_s_all_very_technical", "the_future_is_yours", "the_roof_is_on_fire",
        "violence_begets_violence", "no_sweat", "the_best_laid_plans", "overclocked",
        "if_only_hana_were_here", "chain_lightning", "i_ll_have_what_he_s_having",
        "pale_green_dot", "employee_of_the_month", "loading_up_the_boomstick",
        "nemesis_wannabes", "wheel_of_fortune", "expedited_demolition",
        "post_traumatic_speedrun_disorder", "adding_insult_to_injury",
        "be_where_your_enemy_is_not", "great_results_can_be_achieved_with_small_forces",
        "patience_is_the_sharpest_blade", "he_who_wishes_to_fight_must_first_count_the_cost")

    hold_ov(p, G, 0x11)  # LOCE
    p.step(2)
    p.text("SONIC BOOM")
    saw(p, fails, ingame, "fe2", "my_hero", "boom_goes_the_dynamite")

    p.hold_clear()
    p.cmd("ach_reset")

    def gallery_on():
        hold_ov(p, G, G["gshell_ident"])
        p.poke8(0x800BC9DD, 0x11)
        p.poke8(0x800BC9DE, 4)
        p.cmd("poke 800bc9d8 0000")
        p.poke32(0x800BCA78, 1)
        p.step(3)

    def gallery_off():
        p.poke8(0x800BC9DE, 0)
        p.step(2)

    gallery_on()
    saw(p, fails, ingame, "fe2", "purveyor_of_fine_art_i_beginnings_and_endings")
    gallery_off()
    p.disc(2)
    gallery_on()
    saw(p, fails, ingame, "fe2", "purveyor_of_fine_art_ii_tomb_raider_i_hardly_know_her")
    gallery_off()
    p.disc(3)
    gallery_on()
    saw(p, fails, ingame, "fe2", "purveyor_of_fine_art_iii_the_future_is_a_foreign_land")
    gallery_off()
    p.disc(4)
    gallery_on()
    saw(p, fails, ingame, "fe2", "purveyor_of_fine_art_iv_mission_complete_time_for_a_massage")
    gallery_off()

    p.disc(1)
    p.hold_clear()
    p.cmd("ach_reset")
    p.poke8(G["diff"], 1)
    hold_ov(p, G, G["locb_ident"])
    p.step(2)
    hold_ov(p, G, G["loch_ident"])
    p.step(3)
    saw(p, fails, ingame, "fe2", "hell_of_a_diversion", "party_hard")

    p.cmd("ach_reset")
    hold_ov(p, G, G["credits_ident"])
    p.step(3)
    saw(p, fails, ingame, "fe2", "the_time_for_change_is_upon_us", "war_war_never_changes",
        "time_is_a_flat_circle", "ascension_to_enlightenment",
        "dead_bodies_everywhere", "telephobia",
        "cutscenes_where_we_re_going_we_don_t_need_cutscenes")

    p.cmd("ach_reset")
    hold_ov(p, G, 0x12)  # LOCF
    p.step(2)
    hold_ov(p, G, G["credits_ident"])
    p.step(3)
    saw(p, fails, ingame, "fe2", "mercy_begets_mercy")

    p.cmd("ach_reset")
    p.poke32(0x8007B928, 0)
    p.poke32(0x8007B92C, 0)
    hold_ov(p, G, G["loca_ident"])
    p.step(2)
    p.poke32(0x8007B928, 5)
    hold_ov(p, G, G["locb_ident"])
    p.step(3)
    saw(p, fails, ingame, "fe2", "mighty_foot_engaged")

    p.cmd("ach_reset")
    p.poke32(0x8007B928, 0)
    p.poke32(0x8007B92C, 0)
    hold_ov(p, G, G["locb_ident"])
    p.step(2)
    p.poke32(0x8007B92C, 10)
    hold_ov(p, G, G["loch_ident"])
    p.step(3)
    saw(p, fails, ingame, "fe2", "metal_gear_rain", "that_even_beats_my_last_shareholders_meeting")

    p.cmd("ach_reset")
    p.poke32(0x8007B928, 0)
    hold_ov(p, G, 0x11)  # LOCE
    p.step(2)
    p.poke32(0x8007B928, 20)
    hold_ov(p, G, 0x15)  # LOCI
    p.step(3)
    saw(p, fails, ingame, "fe2", "say_hello_to_my_little_friends", "we_re_not_using_the_z_word",
        "can_t_we_all_just_get_along")

    p.cmd("ach_reset")
    p.poke32(0x8007B928, 0)
    hold_ov(p, G, 0x10)  # LOCD
    p.step(2)
    p.poke32(0x8007B928, 10)
    hold_ov(p, G, 0x12)  # LOCF
    p.step(3)
    saw(p, fails, ingame, "fe2", "s_mart_s_top_of_the_line")

    p.cmd("ach_reset")
    hold_ov(p, G, G["loca_ident"])
    p.step(2)
    for addr, slug in (
        (0x8008C025, "no_need_for_a_plumber"),
        (0x8008C016, "i_m_a_professional_i_swear"),
        (0x8008C024, "the_fastest_hacker_in_the_east"),
        (0x8008C02C, "i_have_to_return_some_videotapes"),
        (0x8008C02D, "shut_up_and_take_my_money"),
        (0x8008C017, "curiosity_killed_the_cat"),
    ):
        rise8(p, addr)
        saw(p, fails, ingame, "fe2", slug)

    hold_ov(p, G, G["locb_ident"])
    p.step(2)
    rise8(p, 0x8008C018)
    saw(p, fails, ingame, "fe2", "piece_of_cake")
    rise8(p, 0x8008C019)
    saw(p, fails, ingame, "fe2", "this_jin_fellow_certainly_seems_trustworthy")

    hold_ov(p, G, G["loch_ident"])
    p.step(2)
    rise8(p, 0x8008C01A)
    saw(p, fails, ingame, "fe2", "out_with_the_old_in_with_the_nucleus")
    rise8(p, 0x8008C01B)
    saw(p, fails, ingame, "fe2", "the_blueprint_for_life")
    p.poke8(0x8008BF50 + 0x18 + 8 * 4, 1)  # Hana weapon k=4
    p.step(3)
    saw(p, fails, ingame, "fe2", "let_s_blow_up_this_joint")

    hold_ov(p, G, 0x11)  # LOCE
    p.step(2)
    rise8(p, 0x8008C01C)
    saw(p, fails, ingame, "fe2", "we_re_all_friends_here")
    p.poke8(0x8008BF50 + 0x200 + 0x18 + 8 * 6, 1)
    p.step(3)
    saw(p, fails, ingame, "fe2", "pocket_rocket")

    hold_ov(p, G, 0x15)  # LOCI
    p.step(2)
    rise8(p, 0x8008C026)
    saw(p, fails, ingame, "fe2", "seven_days")
    rise8(p, 0x8008C027)
    saw(p, fails, ingame, "fe2", "reduced_to_rubble")

    hold_ov(p, G, 0x10)  # LOCD
    p.step(2)
    rise8(p, 0x8008C01D)
    saw(p, fails, ingame, "fe2", "a_diamond_in_the_rough")
    p.poke8(0x8008BF50 + 0x100 + 0x18 + 8 * 6, 1)
    p.step(3)
    saw(p, fails, ingame, "fe2", "the_roof_is_on_fire")

    hold_ov(p, G, 0x12)  # LOCF
    p.step(2)
    rise8(p, 0x8008C01E)
    saw(p, fails, ingame, "fe2", "now_you_re_thinking_with_portals")
    rise8(p, 0x8008C01F)
    saw(p, fails, ingame, "fe2", "the_ties_that_bind")
    rise8(p, 0x8008C020)
    saw(p, fails, ingame, "fe2", "the_future_is_yours")

    p.hold_clear()
    p.poke8(0x8008BF50 + 0x18 + 8 * 2, 1)  # Hana Uzi
    p.step(2)
    saw(p, fails, ingame, "fe2", "and_one_for_you")
    p.poke8(0x8008BF50 + 0x300 + 0x18 + 8 * 2, 1)  # Rain Uzi
    p.step(2)
    saw(p, fails, ingame, "fe2", "one_for_me")

    p.cmd("ach_reset")
    p.poke32(0x80082B04, 1)
    p.step(3)
    saw(p, fails, ingame, "fe2", "time_for_dim_sum_and_a_haircut", "southpaw_supremacy",
        "i_m_sure_this_little_runaway_job_will_be_much_easier")

    p.poke32(0x8007B93C, 1)
    p.step(3)
    saw(p, fails, ingame, "fe2", "time_paradox")

    p.poke8(0x8007B4F8, 0x80)
    p.step(3)
    saw(p, fails, ingame, "fe2", "helium_effect_2")

    def drop_swap():
        p.hold_clear()
        p.poke32(G["ov_base"], G["loca_ident"])
        p.step(2)

    # remaining chapter completes via SWAP at the right disc
    p.cmd("ach_reset")
    drop_swap()
    p.disc(2)
    p.hold32(G["ov_base"], G["swap_ident"])
    p.poke8(G["wanted"], 2)
    p.step(3)
    saw(p, fails, ingame, "fe2", "hell_of_a_diversion", "mission_accomplished_right",
        "party_hard", "heist_of_the_century")
    drop_swap()
    p.disc(3)
    p.hold32(G["ov_base"], G["swap_ident"])
    p.poke8(G["wanted"], 3)
    p.step(3)
    saw(p, fails, ingame, "fe2", "can_t_we_all_just_get_along", "dead_men_tell_no_tales",
        "oh_you_re_an_immortal_then", "you_are_indeed_a_worthy_opponent")
    drop_swap()
    p.disc(1)
    p.hold32(G["ov_base"], G["swap_ident"])
    p.poke8(G["wanted"], 1)
    p.poke8(G["diff"], 1)
    p.step(3)
    saw(p, fails, ingame, "fe2", "tough_kitties")


def main() -> int:
    sys.path.insert(0, str(FE_PORT / "tools"))
    from headless_lock import headless  # type: ignore

    fails: list[str] = []
    for g in ("fe1", "fe2"):
        if not JSON[g].is_file():
            fails.append("missing " + str(JSON[g]))
            continue
        n = len(catalog(g))
        want = 64 if g == "fe1" else 79
        if n != want:
            fails.append("%s catalog has %d events, expected %d" % (g, n, want))
        if not GAMES[g]["exe"].is_file():
            fails.append("missing exe " + str(GAMES[g]["exe"]))
            continue
        if not GAMES[g]["cue"].is_file():
            fails.append("missing cue " + str(GAMES[g]["cue"]))
            continue

    if any("missing" in f for f in fails):
        print("\n".join(fails))
        return 1

    work_root = Path(tempfile.mkdtemp(prefix="ach_e2e_"))
    print("work", work_root)
    os.environ.setdefault("FE_HEADLESS_LOCK", str(FE_PORT / "headless.lock"))
    with headless("helix achievements e2e", procs=1):
        for g in ("fe1", "fe2"):
            w = work_root / g
            w.mkdir()
            fails.extend(run_game(g, w))

    if fails:
        print("FAIL %d" % len(fails))
        for f in fails:
            print(" -", f)
        return 1
    print("PASS fe1 64/64 and fe2 79/79 catalog + in-game RAM/HUD/overlay fires")
    return 0


if __name__ == "__main__":
    sys.exit(main())
