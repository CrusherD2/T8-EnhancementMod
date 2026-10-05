#include scripts\core_common\system_shared;
#include scripts\core_common\flag_shared;
#include scripts\core_common\util_shared;
#include scripts\core_common\array_shared;
#include scripts\core_common\trigger_shared;
#include scripts\core_common\scene_shared;
#include scripts\core_common\struct;
#include scripts\core_common\clientfield_shared;
#include scripts\zm_common\zm_score.gsc;
#include scripts\zm_common\zm_utility;
#include scripts\zm_common\zm_round_logic.gsc;
#include scripts\zm_common\zm_game_module.gsc;
#include scripts\zm_common\zm_customgame.gsc;
#include scripts\zm_common\zm_pack_a_punch.gsc;
#include scripts\zm_common\zm_zonemgr;
#include scripts\zm_common\zm_power;
#include scripts\zm_common\zm_sq.gsc;
#include scripts\zm_common\zm_unitrigger.gsc;
#include scripts\zm_common\zm_ui_inventory.gsc;
#include scripts\zm_common\zm_audio.gsc;

#namespace EnhancementLocalTest;

autoexec InitSystem()
{
    system::register("EnhancementLocalTest", &Init, undefined, undefined);
}

Init()
{
    // Enhancement's BO3 AAT detour already honors this for always-proc
    SetDvar(#"shield_enh_LocalTest", 1);
    ShieldLog("^2LocalTest Mod Loaded (AAT always proc on)");
    thread LocalTest_OpenMap();
}

LocalTest_BO4GetMap()
{
    if (level.script == "zm_towers") return "IX";
    else if (level.script == "zm_escape") return "Blood";
    else if (level.script == "zm_red") return "AE";
    else if (level.script == "zm_white") return "AO";
    else if (level.script == "zm_mansion") return "Dead";
    else if (level.script == "zm_orange") return "Tag";
    else if (level.script == "zm_office") return "Classified";
    else if (level.script == "zm_zodt8") return "Voyage";
}

LocalTest_GodMode()
{
    self endon(#"death");
    self iPrintLnBold("godmode ^2on");

    while (isDefined(self.localtest_godmode))
    {
        self enableInvulnerability();
        wait 0.15;
    }

    self iPrintLnBold("godmode ^1off");
    self disableInvulnerability();
}

LocalTest_Noclip()
{
    self endon(#"spawned_player", #"disconnect", #"bled_out");
    level endon(#"end_game", #"game_ended");
    self notify(#"stop_player_out_of_playable_area_monitor");
    self unlink();
    if (isdefined(self.originObj)) self.originObj delete();
    ts = 0;
    self.localtest_noclip = false;

    while (true)
    {
        if (self.localtest_noclip)
        {
            self.originObj = spawn("script_origin", self.origin, 1);
            self.originObj.angles = self.angles;
            self PlayerLinkTo(self.originObj, undefined);
            self enableweapons();

            while (true)
            {
                if (!self.localtest_noclip)
                {
                    self iPrintLnBold("^6Fly mode ^1disabled");
                    break;
                }

                nts = GetTime();
                if (nts > ts)
                {
                    ts = nts + 25000;
                    self iPrintLnBold("^6Fly mode ^2enabled");
                    self iPrintLnBold("^5" + "[{+sprint}]" + "^6: fly, ^5" + "[{+gostand}]" + "^6: up, ^5" + "[{+stance}]" + "^6: down");
                }

                if (self sprintbuttonpressed())
                    fly_speed = 60;
                else
                    fly_speed = 20;

                player_angles = self getPlayerAngles();
                front_vector = AnglesToForward(player_angles);
                left_vector = AnglesToForward(player_angles - (0, 90, 0));
                top_vector = AnglesToForward(player_angles - (90, 0, 0));
                v_movement = self getNormalizedMovement();

                if (self jumpbuttonpressed())
                    z_movement = 1;
                else if (self stancebuttonpressed())
                    z_movement = -1;
                else
                    z_movement = 0;

                move_vector = z_movement * top_vector + front_vector * v_movement[0] + (left_vector[0], left_vector[1], 0) * v_movement[1];
                self.originObj.origin = self.origin + vectorScale(move_vector, fly_speed);
                waitframe(1);
            }

            self unlink();
            if (isdefined(self.originObj)) self.originObj delete();
            waitframe(1);
        }
        waitframe(1);
    }
}

LocalTest_WatchNoclip()
{
    self endon(#"disconnect", #"bled_out");
    level endon(#"end_game", #"game_ended");

    while (true)
    {
        if (ShieldGetKey(84)) // T
        {
            if (isdefined(self.localtest_noclip) && self.localtest_noclip)
                self.localtest_noclip = false;
            else
                self.localtest_noclip = true;
            wait 0.25;
        }
        waitframe(1);
    }
}

LocalTest_WatchRound()
{
    self endon(#"disconnect");
    level endon(#"end_game", #"game_ended");

    while (true)
    {
        if (ShieldGetKey(89)) // Y
        {
            next = zm_round_logic::get_round_number() + 1;
            level thread zm_utility::zombie_goto_round(next);
            level thread zm_game_module::zombie_goto_round(next);
            self iprintlnbold("Round " + next);
            wait 0.25;
        }
        waitframe(1);
    }
}

LocalTest_KeyBinds()
{
    level endon(#"end_game", #"game_ended");
    level flag::wait_till("all_players_spawned");
    level flag::wait_till("initial_blackscreen_passed");

    player = getplayers()[0];
    if (!isdefined(player))
        return;

    if (!isdefined(player.localtest_noclip))
        player thread LocalTest_Noclip();
    player thread LocalTest_WatchNoclip();
    player thread LocalTest_WatchRound();
}

LocalTest_FreeSkipClean(b_skipped, ended_early)
{
    level notify(#"free_skip_clean");
    return;
}

LocalTest_OverrideQuest(quest_name, step_name, setup_func, cleanup_func = undefined)
{
    while (!IsDefined(level._ee))
        waitframe(1);
    while (!isdefined(level._ee[quest_name]))
        waitframe(1);

    ee = level._ee[quest_name];
    foreach (step in ee.steps)
    {
        if (step.name == step_name)
        {
            ee_step = step;
            break;
        }
    }

    if (!IsDefined(ee_step))
        return;

    ee_step.setup_func = setup_func;
    if (isdefined(cleanup_func))
        ee_step.cleanup_func = cleanup_func;
}

LocalTest_BOTDPaP(power_on)
{
    level flag::wait_till("start_zombie_round_logic");
    switch (zm_custom::function_901b751c(#"zmpapenabled"))
    {
        case 1:
            self zm_pack_a_punch::set_state_hidden();
            if (self.script_string == "roof")
            {
                level flag::wait_till("power_on1");
                var_a8d69fbd = getent("pap_shock_box", "script_string");
                var_a8d69fbd playsound(#"hash_3a18ced95ae72103");
                var_a8d69fbd playloopsound(#"hash_3a1bb2d95ae92746");
                var_a8d69fbd notify(#"hash_7f8e7011812dff48");
                wait 2;
                e_player = zm_utility::get_closest_player(var_a8d69fbd.origin);
                e_player thread zm_audio::create_and_play_dialog(#"pap", #"build", undefined, 1);
                scene::play(#"aib_vign_zm_mob_pap_ghosts");
                self zm_pack_a_punch::function_bb629351(1);
                self thread [[ @pap_quest<scripts\zm\zm_escape_pap_quest.gsc>::function_c0bc0375 ]]();
                level zm_ui_inventory::function_7df6bb60(#"zm_escape_paschal", 1);
                level flag::set(#"pap_quest_completed");
                link = @pap_quest<scripts\zm\zm_escape_pap_quest.gsc>::function_3357bedc;
                util::delay(30, "game_over", link);
            }
            break;
    }
}

LocalTest_ActivatePAP()
{
    SetGametypeSetting(#"zmpowerstate", 2);
    ShieldLog("^2LocalTest Activating PAP");

    switch (LocalTest_BO4GetMap())
    {
        case "IX":
            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");
            wait 3;
            level thread [[ @zm_towers_pap_quest<scripts\zm\zm_towers_pap_quest.gsc>::function_a7faeaaf ]]();
            break;

        case "Blood":
            SetGametypeSetting(#"zmpowerstate", 1);
            while (!isDefined(level.pack_a_punch.custom_power_think))
                waitFrame(1);
            level.pack_a_punch.custom_power_think = &LocalTest_BOTDPaP;

            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");

            zm_zonemgr::enable_zone("zone_cellblock_jail_1");
            zm_zonemgr::enable_zone("zone_cellblock_jail_2");
            zm_zonemgr::enable_zone("zone_cellblock_jail_3");
            zm_zonemgr::enable_zone("zone_cellblock_jail_4");
            zm_zonemgr::enable_zone("zone_cellblock_west_barber");
            zm_zonemgr::enable_zone("zone_broadway_floor_2");
            zm_zonemgr::enable_zone("zone_cellblock_west");
            zm_zonemgr::enable_zone("zone_start");
            zm_zonemgr::enable_zone("zone_library");

            level flag::set("pap_machine_active");
            level flag::set(#"hash_3e80d503318a5674");
            level flag::set(#"hash_537cc10c9deca9da");
            level flag::set("power_on");
            level flag::set("power_on1");
            level flag::set("power_on2");
            level flag::set("power_on3");
            level flag::set("pap_power_ready");
            level flag::set(#"pap_quest_completed");
            level flag::set("fasttravel_enabled");
            level flag::set(#"mq_computer_activated");
            level flag::set(#"catwalk_event_completed");
            level flag::set("activate_catwalk");
            level notify(#"hash_7a04a7fb98fa4e4d");

            wait 5;

            var_40762d8a = getent("t_catwalk_door_open", "targetname");
            t_catwalk_door = getent("door_model_west_side_exterior_to_catwalk", "target");
            if (isdefined(var_40762d8a))
            {
                var_40762d8a sethintstring(#"");
                var_40762d8a setinvisibletoall();
            }
            if (isdefined(t_catwalk_door))
            {
                t_catwalk_door sethintstring(#"");
                t_catwalk_door setinvisibletoall();
            }
            if (isdefined(level.var_2ea46461))
                level.var_2ea46461 delete();

            foreach (trig_elec_switch in getentarray("use_elec_switch", "targetname"))
                trig_elec_switch trigger::use();
            break;

        case "AE":
            SetGametypeSetting(#"zmpapenabled", 2);
            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");
            wait 1;
            level flag::set("pap_machine_active");
            level flag::set(#"hash_3e80d503318a5674");
            level flag::set(#"hash_537cc10c9deca9da");
            level flag::set("power_on");
            level flag::set("power_on1");
            level flag::set("power_on2");
            level flag::set("power_on3");
            level flag::set("pap_power_ready");
            level flag::set(#"pap_quest_completed");
            level flag::set("fasttravel_enabled");
            level flag::set(#"mq_computer_activated");
            level flag::set(#"zm_red_fasttravel_open");
            level flag::set(#"hash_3764b0cb106568ec");
            level flag::set(#"hash_3dba794053dea40e");
            level flag::set(#"hash_32ff7a456732ef09");
            level flag::set(#"hash_4083e9da0ba41dec");
            level flag::set(#"cage_dropped");
            level flag::set(#"hash_67695ee69c57c0b2");
            level flag::set(#"hash_61de3b8fe6f6a35");
            level flag::set(#"hash_7943879f3be8ccc6");
            level flag::set(#"eagle_attack");
            level flag::set(#"egg_free");
            level flag::set(#"fl_oracle_unlocked");
            level flag::set(#"hash_1b6616e730b1235b");
            break;

        case "AO":
            SetGametypeSetting(#"zmpapenabled", 2);
            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");
            wait 3;
            zm_sq::start(#"zm_white_main_quest");
            break;

        case "Dead":
            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");
            wait 3;
            s_scene = struct::get(#"p8_fxanim_zm_man_ooze_clump_bundle", "scriptbundlename");
            if (isdefined(s_scene))
            {
                s_scene thread scene::play(#"p8_fxanim_zm_man_ooze_clump_bundle", "clump01_rise");
                s_scene thread scene::play(#"p8_fxanim_zm_man_ooze_clump_bundle", "clump02_rise");
                s_scene thread scene::play(#"p8_fxanim_zm_man_ooze_clump_bundle", "clump03_rise");
            }
            wait 5;
            level flag::set("crystal_main_hall");
            level flag::set("crystal_library");
            level flag::set("crystal_greenhouse");
            level flag::set("crystal_main_hall_key");
            level flag::set("crystal_library_key");
            level flag::set("crystal_greenhouse_key");
            level flag::set("power_on666");
            level flag::set("unlock_pap_gate");
            level flag::set("open_pap");
            zm_power::turn_power_on_and_open_doors(666);
            break;

        case "Tag":
            level thread LocalTest_OverrideQuest(#"pap_rock", #"step_1", &LocalTest_FreeSkipClean);
            level thread LocalTest_OverrideQuest(#"pap_rock", #"step_2", &LocalTest_FreeSkipClean);
            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");
            wait 3;
            level flag::set(#"hash_3310bb35ce396e49");
            level flag::set(#"hash_5a3d0402a5557739");
            level flag::set(#"hash_3028604821838259");
            level flag::set(#"hash_78cf83ad057b4f1f");
            break;

        case "Classified":
            SetGametypeSetting(#"zmpapenabled", 2);
            level flag::wait_till("all_players_spawned");
            level flag::set("pap_machine_active");
            level flag::set(#"hash_3e80d503318a5674");
            level flag::set(#"hash_537cc10c9deca9da");
            level flag::set("power_on");
            level flag::set("power_on1");
            level flag::set("power_on2");
            level flag::set("power_on3");
            level flag::set("pap_power_ready");
            level flag::set(#"pap_quest_completed");
            level flag::set("fasttravel_enabled");
            level flag::set(#"mq_computer_activated");
            level flag::wait_till("initial_blackscreen_passed");
            level notify(#"modifier_acquired");
            wait 3;
            if (isdefined(level.var_2de08508))
                level.var_2de08508 notify(#"trigger", {#activator:getplayers()[0]});
            wait 5;
            think = @zm_office_teleporters<scripts\zm\zm_office_teleporters.gsc>::portal_think;
            if (isdefined(level.s_cage_portal))
                level.s_cage_portal zm_unitrigger::create("", 32, think, 0, 0);
            [[ @zm_office_teleporters<scripts\zm\zm_office_teleporters.gsc>::function_60abbae4 ]](1);
            if (isdefined(level.var_a23b5c5))
            {
                level.var_a23b5c5 playsound(#"hash_123af2d6dc30025a");
                level.var_a23b5c5 movez(150, 1);
            }
            break;

        case "Voyage":
            level flag::wait_till("all_players_spawned");
            level flag::wait_till("initial_blackscreen_passed");
            wait 5;
            function_5c299a0f = @zodt8_pap_quest<scripts\zm\zm_zodt8_pap_quest.gsc>::function_5c299a0f;
            if (isdefined(level.s_pap_quest) && isdefined(level.s_pap_quest.a_s_locations))
            {
                foreach (s_loc in level.s_pap_quest.a_s_locations)
                {
                    s_loc.unitrigger_stub thread [[ function_5c299a0f ]]();
                    wait 0.5;
                }
            }
            break;
    }
}

LocalTest_OpenMap()
{
    level thread LocalTest_ActivatePAP();
    level thread LocalTest_KeyBinds();

    level flag::wait_till("all_players_spawned");
    level flag::wait_till("initial_blackscreen_passed");
    wait 4;

    player = getplayers()[0];
    doors = getentarray("zombie_door", "targetname");
    doors = arraycombine(doors, getentarray("zombie_debris", "targetname"), 0, 0);
    doors = arraycombine(doors, getentarray("zombie_airlock_buy", "targetname"), 0, 0);
    foreach (door in doors)
        door notify(#"trigger", {#activator:player, #is_forced:1});

    level flag::set("power_on");
    level flag::set("power_on1");
    level flag::set("power_on2");
    level flag::set("power_on3");
    level flag::set("pap_machine_active");
    level flag::set("pap_power_ready");
    level flag::set(#"pap_quest_completed");
    level flag::set(#"zm_towers_pap_quest_completed");

    foreach (guy in getplayers())
    {
        guy zm_score::add_to_player_score(50000);
        guy.localtest_godmode = true;
        guy thread LocalTest_GodMode();
    }

    ShieldLog("^2LocalTest: doors/power/PAP/god/50k ready — T noclip, Y round skip");
}
