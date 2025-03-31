# NVFBC-OBS-TEST
A plugin that utilizes NVIDIAs NVFBC functionality to be used with OBS

Works by reading the front framebuffer directly from a nvidia GPU, and writing the contents to a D3D9 surface, which is piped into OBS for screen capturing.

Why? Allows for a hookless experience for capturing video. In theory, all encoding can be passed off to the GPU (Not supported yet). Also avoids other CPU implementations such as DXGI.

Currently in a buggy state, consumes too much GPU and CPU due to inefficient implementation and partial OBS limitations.

Prerequisites:
- Windows
- A GPU capable of utilizing NVIDIAs NVFBC technology natively or with patched drivers
- Need patched nvidia drivers to work for usage on a RTX or GTX consumer grade GPU (drivers not posted here)
- Latest version of OBS, v31
- The compiled plugin installed in the correct OBS directory

Improvements/Need to dos:
-  Instead of rendering the contents of the framebuffer to a D3D9 surface, pipe raw pixel data directly into OBS to free up some overhead
-  Add functionality for selecting different resolutions/fps values for capture in OBS
