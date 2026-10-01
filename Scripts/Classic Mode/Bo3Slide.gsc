#include scripts\core_common\callbacks_shared;
#include scripts\core_common\system_shared;

#namespace bo3_slide;

autoexec __init__system__()
{
	system::register("bo3_slide", &__init__, undefined, undefined);
}

__init__()
{
	callback::on_start_gametype(&bo3_slide_start);
}

bo3_slide_start()
{
	level thread bo3_slide_dvar();
	callback::on_spawned(&bo3_slide_player);
}

bo3_slide_dvar()
{
	level endon(#"game_ended");

	while (true)
	{
		if (GetDvarInt(#"shield_enh_BO3Slide", 1))
		{
			SetDvar(#"slide_subsequentslidescale", 0);
			SetDvar(#"hash_4b70f9308a25eb2c", 0);
		}
		else
		{
			SetDvar(#"slide_subsequentslidescale", 0.18);
			SetDvar(#"hash_4b70f9308a25eb2c", 1);
		}

		wait 0.25;
	}
}

bo3_slide_player()
{
	self endon(#"death", #"disconnect");

	fullSpeed = 0;
	wasSliding = false;
	lastSlide = 0;

	while (true)
	{
		if (!GetDvarInt(#"shield_enh_BO3Slide", 1))
		{
			fullSpeed = 0;
			wasSliding = false;
			wait 0.2;
			continue;
		}

		sliding = self isonslide();
		now = gettime();

		if (!sliding && wasSliding)
			lastSlide = now;

		if (!sliding && fullSpeed > 0 && (now - lastSlide) > 1500)
			fullSpeed = 0;

		if (sliding && !wasSliding)
		{
			vel = self GetVelocity();
			speed = sqrt((vel[0] * vel[0]) + (vel[1] * vel[1]));

			if (speed > 10)
			{
				if (fullSpeed <= 0 || speed >= fullSpeed)
				{
					fullSpeed = speed;
				}
				else
				{
					scale = fullSpeed / speed;
					self SetVelocity((vel[0] * scale, vel[1] * scale, vel[2]));
				}
			}
		}

		wasSliding = sliding;
		waitframe(1);
	}
}
