# rsp_tcp

(c)2018 F4FHH Nicolas (f4fhh@ducor.fr). Licensed under the GNU GPL V3

(c)2020 ON5HB Bas Heijermans, Forked and adjusted for websdr.org - V2.0 driver!

## An rtl_tcp compatible IQ server for the RSP range of SDRPlay SDR but DOES support the RSPdx (untested)
## The best SDRplay to buy and use for websdr.org is the RSP1A, it has the best filters and notches!!!!!!!

rsp_tcp is a direct port of [rtl_tcp](https://github.com/osmocom/rtl-sdr) for the RSP range of [SDRPlay SDR](https://www.sdrplay.com/).

As the rtl_tcp protocol is only 8 bits IQ it still uses the 15bit range, but you can reduce it:

1. It will work with any rtl_tcp capable frontend (probably), see usage below
2. As it's opensource, you could compile it on any Linux server

## OPTIONS
Usage:

	-a listen address
	
	-p listen port (default: 1234)
	
	-d RSP device to use (default: 1, first found)
	
	-P Antenna Port select* (0/1/2, default: 0, Port A)
	
	-r Gain reduction (default: 34 / values 0 upto 59)
	
	-L Low Noise Amplifier (default: 2 / values 0-9)
	
	-T Bias-T enable* (default: disabled)
	
	-D DAB Notch disable* (default: enabled)
	
	-B Broadcast Notch disable* (default: enabled)
	
	-R Refclk output enable* (default: disabled)
	
	-f frequency to tune to (Hz) - If freq set center/prog freq is ignored from client!!
	
	-s samplerate in [Hz] - If sample rate is set it will be ignored from client!!
	
	-W wideband disable* (default: enabled)
			
	-A Auto Gain Control (default: -34 / values 0 to -60)
	
	-G Auto Gain Control Loop-bandwidth in Hz (default: 50 / values 0/5/50/100)
	
	-n max number of linked list buffers to keep (default: 512)
	
	-b Bit conversion to 8bit (default: 14.5 / values 12/12.5/13/13.5/14/14.5/15.5/16)
	
	-o Use decimate can give high CPU load (default: minimal-programmed / values 2/4/8/16/32 / 1 = auto-best)
	
	-v Verbose output (debug) enable (default: disabled)

## USAGE
 - This software is optimised for usage with websdr.org software. 
 - Use !rtl_sdr adress/port/ppm like you normally would with an RTL-dongle
 - RTL RF gain is set automaticly, not much change needed unless signals are too low or high
 - RTL sample rates tested are those in the websdr.org documentation but lower even down to 62500 has been tested.
 - Install the HW/API 2.13 driver from RSPplay for the RSP1A first!!

## BUILDING
```
  mkdir build
  cd build
  cmake ..
  make
  sudo make install
```
## NOTES
 - a RSP API version 3.07 (no other tested!) must be installed on the linux server, see [sdrplay linux downloads](https://www.sdrplay.com/downloads/)
 - It does compile and run on Raspbian (Raspberry Pi2 tested but isn't fast enough)
 - It should compile on windows as the initial code from rtl_tcp does
 - The goal of this software is ONLY to work well with websdr.org, no other websdr servers or RTL-compatible software but it may work.
 - This is a first attempt, not all parameters work!!
## TODO
 - Find a way to send 16bit samples, so far no luck.
 - BiasT doesn't work no matter what I try, tested 2 RSP1A no BiasT
 
## HISTORY (later versions are forked by Bas ON5HB from original of F4FHH)
 - Version 0.1.0: Initial build
 - Version 0.1.4: Added extra options Bas.
 - Version 0.4.5: Added more samplerates that work 64/96/128/192/384/512/768/1024/1536/2048/2880K
 - Version 1.1.5: Removed clicks on overloads.
 - Version 1.1.6: -w removed and combined with -W to avoid ghost-signals appear. Many new bit conversions and default to 15.5bit.
 - Version 1.1.7: 14.5bit as default and fixed box reporting, now correct and box-name, only tested on RSP1A.
 - Version 2.0.0: New driver that works with API 3.07 and supports all current boxes. First working version.
 - Version 2.0.1: Loads of fixes, Wideband, Decimate for websdr.org etc.

## CREDITS
 - [Open Source Mobile Communications (OSMOCOM)](https://github.com/osmocom/rtl-sdr.git) team for the original rtl_tcp code
 - [Thierry Leconte](https://github.com/TLeconte/airspy_tcp.git) for many ideas that I found in his Airspy port of rtl_tcp
 - [Tony Hoyle](https://github.com/TonyHoyle/sdrplay.git) for the initial idea
 - [Pothosware](https://github.com/pothosware) for the cmake build examples
 - [Nicolas F4FHH](https://github.com/f4hh) for creating the original code to work with rtl_tcp compatible software

