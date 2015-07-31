# mpdjoy

## About
mpdjoy controls your MPD-host by any linux driven joystick/pad.

It's just a small etude to tinker around libmpd and to improve
my daemon skel, but you might find it usefull anyway.
e.g. how to evaluate joysticks the more native way

You'll need a loaded playlist for now. Use any mpdclient
like the good mpc for that.

## Future improvements
Handling of playlists (next/prev)
Handling of "repeat"-flags
Dynamic joystik device polling

## Install
mpdjoy should compile smoothly by doing a

make

additionally needed libraries: libconfig and libmpdclient

## Compiling issues
If the compiler complains about int/long int within config_setting_lookup_int,
change the 3 marked lines to what your system libconfig treats.

## Why
I just wanted to have buttons for my fingers to handle mpd - music
because my SoC-Board doesn't have video output at all.
Of course I've a IR-remote solution, too, but sometimes a local
hid is cushier :)

## What it does
mpdjoy will read its configuration file which is
named mpdjoy.cfg by default.
You may give it another one by passing it with 
-c /path/to/config.mpd.cfg
If you plan to use more than one pad/joystick
you just need another instance with a different
config, containing another /dev/input/jsX

First you want to take a look at the sample
configuration, just edit it to fit your needs.
Well, take care on its non-trivial structure,
but hey, it's self-explaining.

Starting mpdjoy without any parameter (as root), will
evaluate mpdjoy.cfg and run with a single-lined very simple 
mpd-client "frontend" output in terminal.
Watch terminal output for any BUTTON and AXIS Number 
to be used in your personal configuration. That means,
probe the buttons/axis to see which number
is assigned to. There are no real standards on this level afaik.
But it's possible that the sample config already
fits your needs...

If the configuration fits your needs, you may start it
with -d(aemon) so it runs thrifty in background.

At the moment you'll probably have to be root to start it,
otherwise there might be problems accessing the input device
on several platforms.
Running passing -d(aemon) will let it fork to background with 
given userid/groupid rights. Running in background without
given userid will not work at all. As we all now, never
run it as root.

## Hint
If you want to start it from userspace, you would need
to ensure correct file permissions on given input device /dev/input/jsX, 
otherwise there'll be *no* error, BUT also no pad/stick event handling.
This is the only reason to start as root - there a vary ways to set
right permissions, often adding the user to a "joystick"-group is the way, 
but that's not part of this.
I just want to avoid a "doesn't work" - which is caused by file permissions

Another issue might be the hid device at all.
You may check the joystick/pad with "jstest", which
is part of a package mostly named "joystick"
BTW, jstest will give you the button and axis number, too.

h4v3 fun