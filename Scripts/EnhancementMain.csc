#include scripts\core_common\system_shared.csc;
#include scripts\core_common\clientfield_shared.csc;
#include scripts\core_common\util_shared.csc;
#include scripts\zm_common\zm_perks.csc;
#include scripts\zm_common\zm_customgame.csc;
#include scripts\zm_common\zm_loadout.csc;
#include scripts\core_common\beam_shared.csc;
#include scripts\zm_common\zm_utility.csc;

#namespace T8EnhancementMod;

autoexec InitSystem() 
{
    if (util::is_frontend_map()) return; // frontend, i dont want to fucking loop again please

    system::register("T8EnhancementMod", &Init, &PostInit, undefined);
}

Init()
{
    if (BO4GetMap() == "Blood")
    {
        level thread [[ @zm_weap_chakram<scripts\zm\weapons\zm_weap_chakram.csc>::__init__ ]]();
        level thread [[ @zm_weap_hammer<scripts\zm\weapons\zm_weap_hammer.csc>::__init__ ]]();
        level thread [[ @zm_weap_sword_pistol<scripts\zm\weapons\zm_weap_sword_pistol.csc>::__init__ ]]();
        level thread [[ @zm_weap_scepter<scripts\zm\weapons\zm_weap_scepter.csc>::__init__ ]]();

        level thread [[ @zm_weap_homunculus<scripts\zm\weapons\zm_weap_homunculus.csc>::__init__ ]]();
    }

    //ShieldLog("^1T8 Enhancement Mod Loaded! (CSC)");

    clientfield::register("toplayer", "bo3port_power", 1, 1, "int", &Bo3PortPower, 0, 0);
    clientfield::register("toplayer", "bo3port_drink", 1, 3, "int", &Bo3PortDrink, 0, 0);

    thread ShieldPublicPauseScript();
    thread HardcoreBossesScript();
    thread T8WeaponsDrops();

    // !! - later
}

Bo3PortPower(localclientnum, oldval, newval, bnewent, binitialsnap, fieldname, bwastimejump)
{
    SetDvar(#"bo3port_power", newval);
}

Bo3PortDrink(localclientnum, oldval, newval, bnewent, binitialsnap, fieldname, bwastimejump)
{
    SetDvar(#"bo3port_drink", newval);
}

PostInit() 
{ 
    if (BO4GetMap() == "Blood")
        level thread [[ @zm_weap_homunculus<scripts\zm\weapons\zm_weap_homunculus.csc>::__main__ ]]();

    thread Setup();
}

Setup()
{
    //thread ConfigLogicCSC();
    //thread ZombieCounterT8(); // csc <- unused, for now

    // Debug
    //thread Eye_Change_Camera();
}