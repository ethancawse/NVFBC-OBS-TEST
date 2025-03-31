/************************************************
 * nvfbc_dx9_capture.cpp
 ************************************************/

 #include <obs-module.h>
 #include <obs.h>
 #include <windows.h>
 #include <string.h>
 #include <d3d9.h>
 #include <d3d9types.h>
 #include "nvFBC.h"
 #include <NvFBCLibrary.h>
 #include <NvFBCToDx9vid.h>
 #include <chrono>
 
 // --------------------------------------------------------------------
 // Forward declarations for OBS callbacks
 // --------------------------------------------------------------------
 static const char *nvfbc_dx9_get_name(void *type_data);
 static void      *nvfbc_dx9_create(obs_data_t *settings, obs_source_t *source);
 static void       nvfbc_dx9_destroy(void *data);
 static void       nvfbc_dx9_update(void *data, obs_data_t *settings);
 static void       nvfbc_dx9_video_tick(void *data, float seconds);
 static void       nvfbc_dx9_video_render(void *data, gs_effect_t *effect);
 static uint32_t   nvfbc_dx9_get_width(void *data);
 static uint32_t   nvfbc_dx9_get_height(void *data);
 
 // --------------------------------------------------------------------
 // Forward declarations for helper functions
 // --------------------------------------------------------------------
 struct nvfbc_dx9_data; // forward declaration of our context structure
 static bool create_d3d9_device(struct nvfbc_dx9_data *ctx, int adapterIndex);
 static void release_d3d9_device(struct nvfbc_dx9_data *ctx);
 static bool init_nvfbc_dx9(struct nvfbc_dx9_data *ctx, int adapterIndex);
 static void free_nvfbc_dx9(struct nvfbc_dx9_data *ctx);
 static bool nvfbc_grab_frame(struct nvfbc_dx9_data *ctx);
 
 // --------------------------------------------------------------------
 // Function to initialize our obs_source_info structure
 // --------------------------------------------------------------------
 extern "C" void init_nvfbc_source_info();
 
 // --------------------------------------------------------------------
 // Global obs_source_info structure (uninitialized here)
 // --------------------------------------------------------------------
 extern "C" {
     struct obs_source_info nvfbc_source_info;
 }
 
 // --------------------------------------------------------------------
 // Initialize obs_source_info
 // --------------------------------------------------------------------
 extern "C" void init_nvfbc_source_info()
 {
     blog(LOG_INFO, "[nvfbc_dx9] Initializing source info");
     nvfbc_source_info.id             = "nvfbc_dx9_capture";
     nvfbc_source_info.type           = OBS_SOURCE_TYPE_INPUT;
     nvfbc_source_info.output_flags   = OBS_SOURCE_VIDEO;
 
     nvfbc_source_info.get_name       = nvfbc_dx9_get_name;
     nvfbc_source_info.create         = nvfbc_dx9_create;
     nvfbc_source_info.destroy        = nvfbc_dx9_destroy;
     nvfbc_source_info.update         = nvfbc_dx9_update;
 
     nvfbc_source_info.get_defaults   = NULL;
     nvfbc_source_info.get_properties = NULL;
     nvfbc_source_info.show           = NULL;
     nvfbc_source_info.hide           = NULL;
 
     // We now use video_tick for timed capture.
     nvfbc_source_info.video_tick     = nvfbc_dx9_video_tick;
     nvfbc_source_info.video_render   = nvfbc_dx9_video_render;
     nvfbc_source_info.get_width      = nvfbc_dx9_get_width;
     nvfbc_source_info.get_height     = nvfbc_dx9_get_height;
 
     blog(LOG_INFO, "[nvfbc_dx9] Source info initialized");
 }
 
 // --------------------------------------------------------------------
 // OBS Plugin Entry Points
 // --------------------------------------------------------------------
 OBS_DECLARE_MODULE()
 OBS_MODULE_USE_DEFAULT_LOCALE("nvfbc_dx9_plugin", "en-US")
 
 bool obs_module_load(void)
 {
     blog(LOG_INFO, "[nvfbc_dx9] obs_module_load called");
     init_nvfbc_source_info();
     obs_register_source(&nvfbc_source_info);
     blog(LOG_INFO, "[nvfbc_dx9] Plugin loaded (Dx9Vid).");
     return true;
 }
 
 void obs_module_unload(void)
 {
     blog(LOG_INFO, "[nvfbc_dx9] Plugin unloaded.");
 }
 
 // --------------------------------------------------------------------
 // Data structure for NVFBC + D3D9 context
 // --------------------------------------------------------------------
 struct nvfbc_dx9_data {
     obs_source_t      *source;
 
     NvFBCLibrary      *nvfbc_lib;
     NvFBCToDx9Vid     *fbc;
     bool               fbc_initialized;
 
     IDirect3D9Ex      *d3d_ex;
     IDirect3DDevice9Ex *d3d_device;
     IDirect3DSurface9 *capture_surface;
     IDirect3DSurface9 *sysmem_surface;
 
     int width;
     int height;
 
     gs_texture_t      *obs_texture;
 
     // Accumulator to throttle frame grabs (in seconds)
     double accumulator;
     // Instrumentation: count of successful frame grabs
     uint32_t frame_count;
     // Use std::chrono for last-log time
     std::chrono::steady_clock::time_point last_log_time;
 };
 
 // --------------------------------------------------------------------
 // Width/Height Callbacks
 // --------------------------------------------------------------------
 static uint32_t nvfbc_dx9_get_width(void *data)
 {
     auto *ctx = static_cast<nvfbc_dx9_data *>(data);
     uint32_t w = ctx ? ctx->width : 0;
     //blog(LOG_DEBUG, "[nvfbc_dx9] get_width returns %u", w);
     return w;
 }
 
 static uint32_t nvfbc_dx9_get_height(void *data)
 {
     auto *ctx = static_cast<nvfbc_dx9_data *>(data);
     uint32_t h = ctx ? ctx->height : 0;
     //blog(LOG_DEBUG, "[nvfbc_dx9] get_height returns %u", h);
     return h;
 }
 
 // Helper function that calls the NVFBC create function using SEH.
 static NvFBCToDx9Vid* CreateNVFBCInstance(void* d3dDevice, NvFBCLibrary* lib, DWORD* maxWidth, DWORD* maxHeight)
 {
     NvFBCToDx9Vid* instance = nullptr;
     __try {
         instance = (NvFBCToDx9Vid*)lib->create(NVFBC_TO_DX9_VID, maxWidth, maxHeight, 0, d3dDevice);
     }
     __except(EXCEPTION_EXECUTE_HANDLER) {
         instance = nullptr;
     }
     return instance;
 }
 
 bool pick_desktop_resolution_and_adapter(int &outWidth, int &outHeight, int &outAdapterIndex)
 {
     DISPLAY_DEVICE dd;
     ZeroMemory(&dd, sizeof(dd));
     dd.cb = sizeof(dd);
 
     int adapterIndex = 0;
     bool foundPrimary = false;
     while (EnumDisplayDevices(NULL, adapterIndex, &dd, 0)) {
         if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
             DEVMODE dm;
             ZeroMemory(&dm, sizeof(dm));
             dm.dmSize = sizeof(dm);
             if (EnumDisplaySettings(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
                 outWidth = dm.dmPelsWidth;
                 outHeight = dm.dmPelsHeight;
                 outAdapterIndex = adapterIndex;
                 foundPrimary = true;
                 break;
             }
         }
         adapterIndex++;
         ZeroMemory(&dd, sizeof(dd));
         dd.cb = sizeof(dd);
     }
 
     if (!foundPrimary) {
         outWidth = 1920;
         outHeight = 1080;
         outAdapterIndex = 0;
     }
     return foundPrimary;
 }
 
 // --------------------------------------------------------------------
 // Name Callback
 // --------------------------------------------------------------------
 static const char *nvfbc_dx9_get_name(void *)
 {
     return "NVFBC (Dx9Vid) Capture";
 }
 
 // --------------------------------------------------------------------
 // Create/Destroy/Update Callbacks
 // --------------------------------------------------------------------
 static void *nvfbc_dx9_create(obs_data_t *settings, obs_source_t *source)
 {
     blog(LOG_INFO, "[nvfbc_dx9] nvfbc_dx9_create called");
     auto *ctx = new nvfbc_dx9_data();
     ctx->source          = source;
     ctx->nvfbc_lib       = nullptr;
     ctx->fbc             = nullptr;
     ctx->fbc_initialized = false;
     ctx->d3d_ex          = nullptr;
     ctx->d3d_device      = nullptr;
     ctx->capture_surface = nullptr;
     ctx->sysmem_surface  = nullptr;
     ctx->obs_texture     = nullptr;
 
     // Initialize instrumentation
     ctx->accumulator     = 0.0;
     ctx->frame_count     = 0;
     ctx->last_log_time   = std::chrono::steady_clock::now();
 
     int adapterIndex = 0;
     int width = 0, height = 0;
 
     blog(LOG_INFO, "Picking desktop resolution");
     bool primaryFound = pick_desktop_resolution_and_adapter(width, height, adapterIndex);
     blog(LOG_INFO, "Chosen adapter index: %d, resolution: %dx%d", adapterIndex, width, height);
 
     ctx->width = width;
     ctx->height = height;
 
     blog(LOG_INFO, "[nvfbc_dx9] Creating D3D9 device on adapter %d", adapterIndex);
     if (!create_d3d9_device(ctx, adapterIndex)) {
         blog(LOG_ERROR, "[nvfbc_dx9] create_d3d9_device failed");
         nvfbc_dx9_destroy(ctx);
         return nullptr;
     }
     blog(LOG_INFO, "[nvfbc_dx9] D3D9 device created successfully");
 
     blog(LOG_INFO, "[nvfbc_dx9] Initializing NVFBC");
     if (!init_nvfbc_dx9(ctx, adapterIndex)) {
         blog(LOG_ERROR, "[nvfbc_dx9] init_nvfbc_dx9 failed");
         nvfbc_dx9_destroy(ctx);
         return nullptr;
     }
     blog(LOG_INFO, "[nvfbc_dx9] NVFBC initialized successfully");
 
     obs_enter_graphics();
     ctx->obs_texture = gs_texture_create(width, height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
     obs_leave_graphics();
 
     if (!ctx->obs_texture) {
         blog(LOG_ERROR, "[nvfbc_dx9] Failed to create OBS texture");
         nvfbc_dx9_destroy(ctx);
         return nullptr;
     }
 
     return ctx;
 }
 
 static void nvfbc_dx9_destroy(void *data)
 {
     auto *ctx = static_cast<nvfbc_dx9_data *>(data);
     if (!ctx)
         return;
     blog(LOG_INFO, "[nvfbc_dx9] Destroying source instance");
 
     free_nvfbc_dx9(ctx);
     release_d3d9_device(ctx);
 
     if (ctx->obs_texture) {
         gs_texture_destroy(ctx->obs_texture);
         ctx->obs_texture = nullptr;
     }
 
     delete ctx;
     blog(LOG_INFO, "[nvfbc_dx9] Source instance destroyed");
 }
 
 static void nvfbc_dx9_update(void *data, obs_data_t *settings)
 {
     UNUSED_PARAMETER(data);
     UNUSED_PARAMETER(settings);
     blog(LOG_DEBUG, "[nvfbc_dx9] Update called");
 }
 
 // --------------------------------------------------------------------
 // Video Tick Callback (using an accumulator to throttle frame grabs)
 // --------------------------------------------------------------------
 static void nvfbc_dx9_video_tick(void *data, float seconds)
 {
     auto *ctx = static_cast<nvfbc_dx9_data *>(data);
     if (!ctx || !ctx->fbc_initialized)
         return;
 
     // Retrieve the global OBS video settings
     video_t *video = obs_get_video();
     if (!video)
         return;
     const struct video_output_info *voi = video_output_get_info(video);
     double fps = 30.0; // fallback
     if (voi && voi->fps_den != 0)
         fps = (double)voi->fps_num / (double)voi->fps_den;
     double desired_interval = 1.0 / fps;
 
     // Accumulate elapsed time (seconds is passed into video_tick)
     ctx->accumulator += seconds;
 
     // Use std::chrono to get current time
     auto now = std::chrono::steady_clock::now();
     // If at least one second has passed since the last log, output frame count.
     auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - ctx->last_log_time).count();
     if (elapsed >= 1.0) {
         blog(LOG_INFO, "[nvfbc_dx9] Captured %u frames in the last second (target ~%.2f FPS)",
             ctx->frame_count, fps);
         ctx->frame_count = 0;
         ctx->last_log_time = now;
     }
 
     // When enough time has accumulated, grab a frame
     if (ctx->accumulator >= desired_interval) {
         if (nvfbc_grab_frame(ctx)) {
             ctx->frame_count++;
         } else {
             blog(LOG_ERROR, "[nvfbc_dx9] Frame grab failed during video tick");
         }
         ctx->accumulator -= desired_interval;
     }
 }
 
 // --------------------------------------------------------------------
 // Video Render Callback
 // --------------------------------------------------------------------
 static void nvfbc_dx9_video_render(void *data, gs_effect_t *effect)
 {
     auto *ctx = static_cast<nvfbc_dx9_data *>(data);
     if (!ctx || !ctx->obs_texture)
         return;
 
     gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
     gs_effect_set_texture(image, ctx->obs_texture);
     gs_draw_sprite(ctx->obs_texture, 0, (uint32_t)ctx->width, (uint32_t)ctx->height);
 
     blog(LOG_DEBUG, "[nvfbc_dx9] Rendered frame at %dx%d", ctx->width, ctx->height);
 }
 
 // --------------------------------------------------------------------
 // D3D9 / NVFBC Helper Functions
 // --------------------------------------------------------------------
 static bool create_d3d9_device(nvfbc_dx9_data *ctx, int adapterIndex)
 {
     HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &ctx->d3d_ex);
     if (FAILED(hr) || !ctx->d3d_ex) {
         blog(LOG_ERROR, "[nvfbc_dx9] Direct3DCreate9Ex failed: 0x%08lx", hr);
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] Direct3D9Ex created");
 
     D3DPRESENT_PARAMETERS d3dpp;
     ZeroMemory(&d3dpp, sizeof(d3dpp));
     d3dpp.Windowed             = TRUE;
     d3dpp.BackBufferFormat     = D3DFMT_UNKNOWN;
     d3dpp.BackBufferWidth      = ctx->width;
     d3dpp.BackBufferHeight     = ctx->height;
     d3dpp.BackBufferCount      = 1;
     d3dpp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
     d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
     d3dpp.hDeviceWindow        = GetDesktopWindow();
 
     hr = ctx->d3d_ex->CreateDeviceEx(
         adapterIndex,
         D3DDEVTYPE_HAL,
         d3dpp.hDeviceWindow,
         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES,
         &d3dpp,
         nullptr,
         &ctx->d3d_device
     );
     if (FAILED(hr) || !ctx->d3d_device) {
         blog(LOG_ERROR, "[nvfbc_dx9] CreateDeviceEx failed: 0x%08lx", hr);
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] D3D9 device created");
 
     hr = ctx->d3d_device->CreateRenderTarget(
         ctx->width,
         ctx->height,
         D3DFMT_A8R8G8B8,
         D3DMULTISAMPLE_NONE,
         0,
         FALSE,
         &ctx->capture_surface,
         nullptr
     );
     if (FAILED(hr) || !ctx->capture_surface) {
         blog(LOG_ERROR, "[nvfbc_dx9] CreateRenderTarget (capture) failed: 0x%08lx", hr);
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] Capture render target created");
 
     hr = ctx->d3d_device->CreateOffscreenPlainSurface(
         ctx->width,
         ctx->height,
         D3DFMT_A8R8G8B8,
         D3DPOOL_SYSTEMMEM,
         &ctx->sysmem_surface,
         nullptr
     );
     if (FAILED(hr) || !ctx->sysmem_surface) {
         blog(LOG_ERROR, "[nvfbc_dx9] CreateOffscreenPlainSurface (sysmem) failed: 0x%08lx", hr);
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] System memory surface created");
 
     return true;
 }
 
 static void release_d3d9_device(nvfbc_dx9_data *ctx)
 {
     if (ctx->sysmem_surface) {
         ctx->sysmem_surface->Release();
         ctx->sysmem_surface = nullptr;
     }
     if (ctx->capture_surface) {
         ctx->capture_surface->Release();
         ctx->capture_surface = nullptr;
     }
     if (ctx->d3d_device) {
         ctx->d3d_device->Release();
         ctx->d3d_device = nullptr;
     }
     if (ctx->d3d_ex) {
         ctx->d3d_ex->Release();
         ctx->d3d_ex = nullptr;
     }
     blog(LOG_INFO, "[nvfbc_dx9] D3D9 device released");
 }
 
 static bool init_nvfbc_dx9(nvfbc_dx9_data *ctx, int adapterIndex)
 {
     blog(LOG_INFO, "[nvfbc_dx9] Loading NVFBC library");
     ctx->nvfbc_lib = new NvFBCLibrary();
     if (!ctx->nvfbc_lib->load()) {
         blog(LOG_ERROR, "[nvfbc_dx9] Could not load NvFBC library.");
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] NVFBC library loaded");
 
     DWORD maxDisplayWidth = -1, maxDisplayHeight = -1;
     blog(LOG_INFO, "[nvfbc_dx9] Creating NVFBC instance");
     ctx->fbc = CreateNVFBCInstance((void*)ctx->d3d_device, ctx->nvfbc_lib, &maxDisplayWidth, &maxDisplayHeight);
     if (!ctx->fbc) {
         blog(LOG_ERROR, "[nvfbc_dx9] Failed to create NvFBCToDx9Vid. Possibly unsupported driver.");
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] NVFBC instance created (max width: %u, max height: %u)", maxDisplayWidth, maxDisplayHeight);
 
     NVFBC_TODX9VID_OUT_BUF outBuf;
     outBuf.pPrimary = ctx->capture_surface;
 
     NVFBC_TODX9VID_SETUP_PARAMS setupParams = {};
     setupParams.dwVersion      = NVFBC_TODX9VID_SETUP_PARAMS_V3_VER;
     setupParams.bWithHWCursor  = 1;
     setupParams.bStereoGrab    = 0;
     setupParams.bDiffMap       = 0;
     setupParams.dwNumBuffers   = 1;
     setupParams.ppBuffer       = &outBuf;
     setupParams.eMode          = NVFBC_TODX9VID_ARGB;
     setupParams.bHDRRequest    = FALSE;
 
     blog(LOG_INFO, "[nvfbc_dx9] Setting up NVFBC instance");
     NVFBCRESULT r = ctx->fbc->NvFBCToDx9VidSetUp(&setupParams);
     if (r != NVFBC_SUCCESS) {
         blog(LOG_ERROR, "[nvfbc_dx9] NvFBCToDx9VidSetUp failed: %d", r);
         return false;
     }
     blog(LOG_INFO, "[nvfbc_dx9] NVFBC instance set up successfully");
 
     ctx->fbc_initialized = true;
     return true;
 }
 
 static void free_nvfbc_dx9(nvfbc_dx9_data *ctx)
 {
     if (ctx->fbc) {
         blog(LOG_INFO, "[nvfbc_dx9] Releasing NVFBC instance");
         ctx->fbc->NvFBCToDx9VidRelease();
         ctx->fbc = nullptr;
     }
     if (ctx->nvfbc_lib) {
         ctx->nvfbc_lib->close();
         delete ctx->nvfbc_lib;
         ctx->nvfbc_lib = nullptr;
     }
     ctx->fbc_initialized = false;
     blog(LOG_INFO, "[nvfbc_dx9] NVFBC instance released");
 }
 
 static bool nvfbc_grab_frame(nvfbc_dx9_data *ctx)
 {
     if (!ctx->fbc_initialized)
         return false;
 
     blog(LOG_DEBUG, "[nvfbc_dx9] Grabbing frame");
     NvFBCFrameGrabInfo frameInfo;
     ZeroMemory(&frameInfo, sizeof(frameInfo));
 
     NVFBC_TODX9VID_GRAB_FRAME_PARAMS grabParams = {};
     grabParams.dwVersion           = NVFBC_TODX9VID_GRAB_FRAME_PARAMS_V1_VER;
     grabParams.dwFlags             = NVFBC_TODX9VID_NOWAIT;
     grabParams.eGMode              = NVFBC_TODX9VID_SOURCEMODE_SCALE;
     grabParams.dwTargetWidth       = ctx->width;
     grabParams.dwTargetHeight      = ctx->height;
     grabParams.pNvFBCFrameGrabInfo = &frameInfo;
 
     NVFBCRESULT r = ctx->fbc->NvFBCToDx9VidGrabFrame(&grabParams);
     if (r != NVFBC_SUCCESS) {
         blog(LOG_ERROR, "[nvfbc_dx9] GrabFrame failed: %d", r);
         return false;
     }
 
     obs_enter_graphics();
 
     // Potentially not needed, so commented out:
     // HRESULT presHr = ctx->d3d_device->PresentEx(NULL, NULL, NULL, NULL, D3DPRESENT_INTERVAL_IMMEDIATE);
     // if (FAILED(presHr)) {
     //     blog(LOG_ERROR, "[nvfbc_dx9] PresentEx failed: 0x%08lx", presHr);
     //     obs_leave_graphics();
     //     return false;
     // }
 
     HRESULT hr = ctx->d3d_device->GetRenderTargetData(ctx->capture_surface, ctx->sysmem_surface);
     if (FAILED(hr)) {
         blog(LOG_ERROR, "[nvfbc_dx9] GetRenderTargetData failed: 0x%08lx", hr);
         obs_leave_graphics();
         return false;
     }
 
     D3DLOCKED_RECT lr;
     hr = ctx->sysmem_surface->LockRect(&lr, NULL, D3DLOCK_READONLY);
     if (FAILED(hr)) {
         blog(LOG_ERROR, "[nvfbc_dx9] LockRect failed: 0x%08lx", hr);
         obs_leave_graphics();
         return false;
     }
 
     const uint8_t *src = (const uint8_t *)lr.pBits;
     uint32_t pitchBytes = (uint32_t)lr.Pitch;
 
     gs_texture_set_image(ctx->obs_texture, src, pitchBytes, false);
 
     ctx->sysmem_surface->UnlockRect();
     //blog(LOG_DEBUG, "[nvfbc_dx9] Frame grabbed and texture updated");
     obs_leave_graphics();
     return true;
 }
 