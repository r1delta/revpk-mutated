# revpk-mutated
It's not good

to build on CachyOS (i only have this to test lmao)
(it comes with zstd installed dw about it)
```
pacman -S base-devel cmake
git clone --recursive http://github.com/r1delta/revpk-mutated
mkdir -p revpk-mutated/build
cd revpk-mutated/build
cmake ..
make
```
based on revpk from https://github.com/mauler125/r5sdk and uses https://github.com/TinyTinni/ValveFileVDF
this is mit licensed but i dont know how r5sdk licensing works so handle this at your own risk

## ERRATA

if you put a WAV file or XMA file (if you're on an Xbox 180) into the VPK it will come out "all fucked up" and "distorted" (vitalized's words, not mine)
