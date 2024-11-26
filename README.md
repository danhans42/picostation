# Picostation

Forked from https://github.com/paulocode/picostation

I've taken some pretty drastic design choices with my fork of this project, such as restructuring most of the code and converting it to C++. Keeping the repo as a fork no longer made sense to me; the original repo is archived and inactive, so there would be no point in further syncing changes between the repos. I also found some of the limitations of a forked repo on github a bit annoying.

## __In developement__ _Raspberry Pi Pico based ODE_ for the original Playstation
<a href="https://twitter.com/paulo7x8/status/1602007862733312000"><img src="https://i.ibb.co/9hT2GQc/pico-tweet.jpg" alt="original tweet" height="400"/></a>

### Supported models:
- PU-8  (SCPH100X)
- PU-18 (SCPH55XX)

### Compatibility
<b>NOTE: rename your cue-sheet to UNIROM.cue</b><br>
- Game compatibility and reliability is greatly improved from the original Picostation repo, but there will still be games that don't work at all, and some that may freeze randomly or run poorly.
- ~~Some games may load (see <a href="https://github.com/paulocode/picostation/wiki/Game-Compatibility-List">Game Compatibility List</a> wiki page)~~

### How-to
- see <a href="https://github.com/paulocode/picostation/wiki/How-to">How-to</a> wiki page

### Notes


### To-do (see <a href="https://github.com/paulocode/picostation/issues">issues</a>)
- ~~Stabilize image loading~~
- Make an interface for image choice/loading
- Make it possible to update the pico via SD card

### Links
- Original repo this fork is based on: https://github.com/paulocode/picostation
- PCB: https://github.com/paulocode/picostation_pcb
- FAQ: https://github.com/paulocode/picostation_faq
- Slow Solder Board (SSB) solder points / checking connection: https://mmmonkey.co.uk/xstation-sony-playstation-install-notes-and-pinout/
- How to compile (Windows): https://shawnhymel.com/2096/how-to-set-up-raspberry-pi-pico-c-c-toolchain-on-windows-with-vs-code/
- PCB pinout: <a href="https://i.ibb.co/RvjvDyp/pinout.png"><img src="https://i.ibb.co/mDNDc8C/pinout.png" alt="pinout" border="0"></a>
- 3D Printable mount (550X) by <a href="https://twitter.com/SadSnifit">@Sadsnifit</a> : https://www.printables.com/fr/model/407224-picostation-mount-for-scph-5502
