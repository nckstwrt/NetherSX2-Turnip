# NetherSX2-Turnip

NetherSX2 with Adreno Turnip Driver support

# Download
[Releases](https://github.com/nckstwrt/NetherSX2-Turnip/releases)

## Description

NetherSX2 (formerly AetherSX2) does not support selecting a Turnip driver for Qualcomm based devices. This solution provides inbuilt support for Turnip drivers to provide better performance when using Vulkan rendering in NetherSX (Android PlayStation 2 emulation)

## Changes

This patch modifies **NetherSX2-v2.2n-4248** with these changes:

* Added 3 Turnip Drivers built in to NetherSX2-Turnip with Adreno detection:
  * 24.1.0_R18.a6xx - For **SD865** devices (Retroid Flip 2, Retroid Mini, etc) as it gives the best performance. This is a modifed version of the 24.1.0_R18.a6xx driver specifically for SD865 (as the original could cause the whole system to reboot)
  * StevenMXZ's libvulkan_freedreno_Gen8_v28 for **Snapdragon Elite/8 Gen 5**
  * StevenMXZ's libvulkan_freedreno_v26.2.0_R1 - For **everything else** as this is the latest Mesa/Turnip driver that should work for everything else.

* Removes the restriction on new packages names for the APKs. Normal NetherSX2 had to be installed over AetherSX2 whereas NetherSX2-Turnip lives alongside your existing NetherSX2/AetherSX2 and does not overwrite your existing AetherSX2/NetherSX2 installs or settings.

* Vulkan is now the default renderer (instead of OpenGL)

* Removed the annoying warning "should not be sold, etc" notification

* VulkanShim now logs to /sdcard/Android/data/xyz.aethersx2.tturnip/files/vulkan_shim.log if you want to debug any issues

## How It Works

libemucore.so is modified to load the supplied libvulkad.so instead of the system's libvulkan.so. Our libvulkad.so then loads libvulkan.so but patches it in memory so that instead of loading the system's ICD driver it loads whatever Turnip driver we want (based on what system is detected)

## Results

With NetherSX2-Turnip on my Retroid SD865 devices I can run taxing games like Sly 2 - Band of Thieves at 2x resolution without slowdown. Everything runs faster and therefore higher resolutions are now available to be used than when using the stock Qualcomm drivers (or OpenGL)

## Tips

There is one setting that can make a big difference and that is Hardware Download Mode (under Graphics). Setting this from Accurate to Disable Readbacks can make quite a difference with only rare graphical effects. Worth experimenting with to get games like **Nascar 2004 Thunder** working at maximum speed.

Additionally, for the **Need For Speed** games at least, setting Blending Accuracy to Minimum can make a big difference on lower powered devices.

For **The Getaway** set Blending Accuracy to High (gets rid of the white fog), Hardware Download Mode to Disable Readbacks and EE Cycle Rate to 60% (-2)

## NetherSX2 Classic / v3668

I have made the same changes to NetherSX2 Classic -  the additions may not be worth as much as people expect as the benefits from NetherSX2 Classic come from the OpenGL renderer as much as anything. But have released it now and will continue to do so alongside the regular 4248 version.
