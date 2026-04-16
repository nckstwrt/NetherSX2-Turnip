# NetherSX2-Turnip

NetherSX2 with Adreno Turnip Driver support

# Download
[Releases](https://github.com/nckstwrt/NetherSX2-Turnip/releases)

## Description

NetherSX2 (formerly AetherSX2) does not support selecting a Turnip driver for Qualcomm based devices. This solution provides inbuilt support for Turnip drivers to provide better performance when using Vulkan rendering in NetherSX (Android PlayStation 2 emulation)

## Changes

This patch modifies **NetherSX2-v2.2n-4248** with these changes:

* Added 2 Turnip Drivers built in to NetherSX2-Turnip:
  
  * Mr.Purple T19 - For SD865 devices (Retroid Flip 2, Retroid Mini, etc) as it gives the best performance. This is a modifed version of Mr.Purple's T19 driver specifically for SD865 (as the original could cause the whole system to reboot)
  
  * Mr.Purple T26 - For everything else as this Mesa/Mr.Turnip's latest and unified driver that should work on everything.

* Removes the restriction on new packages names for the APKs. Normal NetherSX2 had to be installed over AetherSX2 whereas NetherSX2-Turnip lives alongside your existing NetherSX2/AetherSX2 and does not overwrite your existing AetherSX2/NetherSX2 installs or settings.

* Vulkan is now the default renderer (instead of OpenGL)

* Removed the annoying warning "should not be sold, etc notification

## How It Works

libemucore.so is modified to load the supplied libvulkad.so instead of the system's libvulkan.so. Our libvulkad.so then loads libvulkan.so but patches it in memory so that instead of loading the system's ICD driver it loads whatever Turnip driver we want (based on what system is detected)

## Results

With NetherSX2-Turnip on my Retroid SD865 devices I can run taxing games like Sly 2 - Band of Thieves at 2x resolution without slowdown. Everything runs faster and therefore higher resolutions are now available to be used than when using the stock Qualcomm drivers (or OpenGL)

## Tips

There is one setting that can make a big difference and that is Hardware Download Mode (under Graphics). Setting this from Accurate to Disable Readbacks can make quite a difference with only rare graphical effects. Worth experimenting with to get games like Nascar 2004 Thunder working at maximum speed.

Additionally, for the Need For Speed games at least, setting Blending to Minimum can make a big difference on lower powered devices.

## NetherSX2 Classic / v3668

I have made the same changes to NetherSX2 Classic - but the additions are not worth releasing as the benefits from NetherSX2 Classic come from the OpenGL renderer. But if anyone wants it I can release it too.
