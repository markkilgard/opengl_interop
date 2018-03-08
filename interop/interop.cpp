
// interop.cpp - demonstrate sandboxed async renderer process generating sharing frames for master process via OpenGL interop

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif
#include <math.h>
#include <GL/glew.h>
#ifdef __APPLE__
# include <GLUT/glut.h>
#else
# include <GL/glut.h>
# ifdef _WIN32
#  include <windows.h>
#  include <GL/wglew.h>
# else
#  include <GL/glx.h>
# endif
#endif

#include <D3D11_1.h>
#include <wrl/client.h>  // for Microsoft::WRL::ComPtr template

#include "showfps.h"        // show frames per second performance
#include "request_vsync.h"  // control vertical refresh synchronization for buffer swaps
#include "sRGB_math.h"

static int window_width = 500, window_height = 500;
static int fbo_width = 500, fbo_height = 500;
static bool use_sRGB = false;
static bool set_dx_device_debug_flag = false;

const char *program_name = "interop";

// Modes, controlled with command line options
bool logging = false;       // -log enables
int swap_interval = 1;      // -novsync sets to zero
bool mipmap_sharetex = 1;   // -bitmap_text sets false
bool use_nvpr = 1;          // -bitmap sets false
UINT32 object_to_draw = 0;
bool timer_updates_renderer_window = true;

// Initially the master; spawning renderer reverses these.
bool i_am_master = true;
bool i_am_renderer = false;

bool D3D11RuntimeLoaded = false;
HMODULE hD3D11Lib = NULL;

Microsoft::WRL::ComPtr<ID3D11Device> d3d_device;
Microsoft::WRL::ComPtr<ID3D11Device1> d3d_device1;
Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
PFN_D3D11_CREATE_DEVICE D3D11CreateDevicePtr = NULL;

HANDLE wgl_d3d_device;
HANDLE job;

FPScontext fps_ctx;  // context for reporting frames/second performance

// Helper macros
#define arraysize(a) (sizeof(a) / sizeof((a)[0]))

struct SharedTexture {
    ID3D11Texture2D *sharetex_d3d;
    HANDLE sharetex_handle;
    GLuint sharetex_gl;
    HANDLE sharetex_wgl_lock_handle;
    GLuint fbo;  // only for renderer
};

// Master-only state
UINT32 current_sharetex_index = -1;

// Renderrer only state
GLuint stencil_tex;

static void fatalError(const char *message)
{
    fprintf(stderr, "%s: %s", program_name, message);
    exit(1);
}

#define MAX_RENDER_BUFFER 4

// Shared state, const copy of sharedData->render_buffer_count
int render_buffer_count = 4;

SharedTexture sharetex[MAX_RENDER_BUFFER];

struct SharedData {
    int render_interval;
    bool use_nvpr;
    UINT32 object_to_draw;
    LONG timer_updates_renderer_window;

    UINT width;
    UINT height;

    bool mipmap_sharetex;
    bool logging;
    bool use_sRGB;

    UINT render_buffer_count;
    HANDLE sharedHandle[MAX_RENDER_BUFFER];

    UINT32 produceCount;
    UINT32 consumeCount;

    bool renderer_should_terminate;
    bool master_should_terminate;
};

SharedData* sharedData;

int logf(const char *fmt, ...)
{
    int rv = 0;
    va_list args;
    va_start(args, fmt);
    bool print_message = logging;
    if (sharedData) {
        print_message = sharedData->logging;
    }
    if (print_message) {
        const char *whoami = i_am_master ? "MASTER: " : "RENDERER: ";
        fwrite(whoami, strlen(whoami), sizeof(char), stdout);
        rv = vprintf(fmt, args);
        fputc('\n', stdout);
        fflush(stdout);
    }
    va_end(args);
    return rv;
}

int reportf(const char *fmt, ...)
{
    int rv = 0;
    va_list args;
    va_start(args, fmt);
    const char *whoami = i_am_master ? "MASTER: " : "RENDERER: ";
    fwrite(whoami, strlen(whoami), sizeof(char), stdout);
    rv = vprintf(fmt, args);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(args);
    return rv;
}

void reshape(int w, int h)
{
    reshapeFPScontext(&fps_ctx, w, h);
    window_width = w;
    window_height = h;
    glViewport(0, 0, w, h);
}

// Load Direct3D library and get D3D11CreateDevice entry point.
void LoadDirect3D()
{
    if (!D3D11RuntimeLoaded) {
        D3D11RuntimeLoaded = true;

        hD3D11Lib = LoadLibraryA("d3d11.dll");
        if (hD3D11Lib == NULL) {
            reportf("Unable load d3d11.dll");
            exit(0);
        }
        if (hD3D11Lib) {
            D3D11CreateDevicePtr = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11Lib, "D3D11CreateDevice");
        }
        if (D3D11CreateDevicePtr == NULL) {
            reportf("could not GetProcAddress of D3D11CreateDevice");
            exit(0);
        }
    }
}

void InitiallizeDirect3D()
{
    logf("Enter InitiallizeDirect3D");
    const D3D_FEATURE_LEVEL fLevel[] = { D3D_FEATURE_LEVEL_11_1 };
    const UINT devflags = D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | // no separate D3D11 worker thread
        (set_dx_device_debug_flag ? D3D11_CREATE_DEVICE_DEBUG : 0) | // useful for diagnosing DX failures
        D3D11_CREATE_DEVICE_SINGLETHREADED;

    HRESULT hr = D3D11CreateDevicePtr(NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        devflags,
        fLevel,
        arraysize(fLevel),
        D3D11_SDK_VERSION,
        &d3d_device,
        NULL,
        &context);
    assert(hr == S_OK);
    // Get a second interface for OpenSharedResource1 method.
    if (SUCCEEDED(d3d_device.As(&d3d_device1))) {
        (void)context.As(&context1);
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS opts;
    d3d_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof(opts));
    logf("ExtendedResourceSharing = %d", opts.ExtendedResourceSharing);

    logf("Leave: InitiallizeDirect3D");
}

void InteropWithDirect3D()
{
    wgl_d3d_device = wglDXOpenDeviceNV(d3d_device.Get());
    if (wgl_d3d_device == NULL) {
        reportf("wglDXOpenDeviceNV failed");
        exit(1);
    }
}

static unsigned int ilog2(unsigned int val) {
    assert(val != 0);
    if (val == 1) return 0;
    unsigned int ret = 0;
    while (val > 1) {
        val >>= 1;
        ret++;
    }
    return ret;
}

// Called by master to initialized shared texture
void createTexture2D(SharedTexture& tex, int width, int height, bool sRGB)
{
    const DXGI_FORMAT format = sRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT samples = 1;  // aliased

    D3D11_TEXTURE2D_DESC dcolor;
    dcolor.Width = width;
    dcolor.Height = height;
    if (mipmap_sharetex) {
        UINT num_mips = ilog2(max(width,height));
        dcolor.MipLevels = num_mips;
    } else {
        dcolor.MipLevels = 1;
    }
    dcolor.ArraySize = 1;
    dcolor.Format = format;
    dcolor.SampleDesc.Count = samples;
    dcolor.SampleDesc.Quality = 0;
    dcolor.Usage = D3D11_USAGE_DEFAULT;  // A resource that requires read and write access by the GPU
    dcolor.BindFlags = D3D11_BIND_RENDER_TARGET;
    dcolor.CPUAccessFlags = 0;
    dcolor.MiscFlags = 0;
    dcolor.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    const D3D11_SUBRESOURCE_DATA* no_initial_image = nullptr;
    HRESULT hr;
    hr = d3d_device->CreateTexture2D(&dcolor, no_initial_image, &tex.sharetex_d3d);
    assert(hr == S_OK);
    IDXGIResource1* pResource = NULL;
    hr = tex.sharetex_d3d->QueryInterface(__uuidof(IDXGIResource1), (void**)&pResource);
    assert(hr == S_OK);
    hr = pResource->CreateSharedHandle(/*SECURITY_ATTRIBUTES*/NULL,
        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        /*name*/NULL, &tex.sharetex_handle);
    assert(hr == S_OK);
    pResource->Release();

    tex.sharetex_gl = 0;
    glGenTextures(1, &tex.sharetex_gl);
    assert(tex.sharetex_gl);
    BOOL ok = wglDXSetResourceShareHandleNV(tex.sharetex_d3d, tex.sharetex_handle);
    assert(ok);
    GLenum texture_target = GL_TEXTURE_2D;
    tex.sharetex_wgl_lock_handle = wglDXRegisterObjectNV(wgl_d3d_device, tex.sharetex_d3d,
        tex.sharetex_gl, texture_target, WGL_ACCESS_READ_WRITE_NV);

    if (tex.sharetex_wgl_lock_handle == NULL) {
        reportf("wglDXRegisterObjectNV failed");
        exit(1);
    }

    if (mipmap_sharetex) {
        glTextureParameteri(tex.sharetex_gl, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(tex.sharetex_gl, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    } else {
        glTextureParameteri(tex.sharetex_gl, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex.sharetex_gl, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }

    tex.fbo = 0;  // unused by renderer
}

ULONG_PTR GetParentProcessId()
{
    LONG (WINAPI *NtQueryInformationProcess)(HANDLE ProcessHandle, ULONG ProcessInformationClass,
        PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength);
    *(FARPROC *)&NtQueryInformationProcess =
        GetProcAddress(LoadLibraryA("NTDLL.DLL"), "NtQueryInformationProcess");
    if (NtQueryInformationProcess) {
        ULONG_PTR pbi[6];
        ULONG ulSize = 0;
        if (NtQueryInformationProcess(GetCurrentProcess(), 0,
            &pbi, sizeof(pbi), &ulSize) >= 0 && ulSize == sizeof(pbi))
            return pbi[5];
    }
    return (ULONG_PTR)-1;
}

void formatMessage(DWORD dw)
{
    char* message_buffer = NULL;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        HRESULT_CODE(dw),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&message_buffer,
        0, NULL);
    reportf("error: %s (0x%x), HRESULT=%d", message_buffer, dw, HRESULT_CODE(dw));

    LocalFree(message_buffer);
}

#define SPAWN_RENDERER_FLAG "-renderer"

// Run by the master to spawn the renderer.
void spawnRendererProcess(const char *program)
{
    // Create nameless memory mapping of sharedData for sharing data with renderer process.
    SECURITY_ATTRIBUTES attributes;
    attributes.bInheritHandle = true;
    attributes.lpSecurityDescriptor = NULL;
    attributes.nLength = 0;
    const HANDLE map_file_handle = CreateFileMapping(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, sizeof(SharedData), /*nameless*/NULL);
    logf("map_file_handle=%p", map_file_handle);
    sharedData = (SharedData*)MapViewOfFile(map_file_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData));
    logf("sharedData=0x%p", sharedData);

    ZeroMemory(sharedData, sizeof(sharedData));
    sharedData->render_interval = 1000;
    sharedData->use_nvpr = use_nvpr;
    sharedData->object_to_draw = object_to_draw;
    sharedData->timer_updates_renderer_window = timer_updates_renderer_window;
    sharedData->width = fbo_width;
    sharedData->height = fbo_height;
    sharedData->render_buffer_count = render_buffer_count;
    sharedData->mipmap_sharetex = mipmap_sharetex;
    sharedData->logging = logging;
    sharedData->use_sRGB = use_sRGB;
    for (int i=0; i<MAX_RENDER_BUFFER; i++) {
        sharedData->sharedHandle[i] = sharetex[i].sharetex_handle;
    }
    logf("sharedData->sharedHandle[0] = %p", sharedData->sharedHandle[0]);

    sharedData->renderer_should_terminate = false;
    sharedData->master_should_terminate = false;

    job = CreateJobObject(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = { 0 };
        // Configure all child processes associated with the job to terminate when the
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        BOOL ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        if (!ok) {
            reportf("SetInformationJobObject failed!");
            DWORD err = GetLastError();
            formatMessage(err);
        }
    } else {
        reportf("CreateJobObject failed!");
        DWORD err = GetLastError();
        formatMessage(err);
    }

    logf("spawnRendererProcess");
    STARTUPINFOA si;
    PROCESS_INFORMATION process_info;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&process_info, sizeof(process_info));
    char cmdLine[256];
    // Pass inherited handle name via command line.
    sprintf_s(cmdLine, "%s " SPAWN_RENDERER_FLAG " %llu", program, (UINT64)map_file_handle);
    BOOL ok = CreateProcessA(NULL,   // No module name (use command line)
        cmdLine,        // Command line
        NULL,           // Process handle not inheritable
        NULL,           // Thread handle not inheritable
        TRUE,           // Allow map_file_handle to be inherited by renderer process.
        0,              // No creation flags
        NULL,           // Use parent's environment block
        NULL,           // Use parent's starting directory 
        &si,            // Pointer to STARTUPINFO structure
        &process_info);           // Pointer to PROCESS_INFORMATION structure    
    if (!ok) {
        reportf("CreateProcessA of renderer process failed");
        DWORD err = GetLastError();
        formatMessage(err);
        exit(1);
    }
    logf("spawnRendererProcess done, ok=%d", ok);

    // Add the renderer process to the job so when the master dies, the renderer dies.
    ok = AssignProcessToJobObject(job, process_info.hProcess);
    if (!ok) {
        reportf("AssignProcessToJobObject failed");
        DWORD err = GetLastError();
        formatMessage(err);
    }

    reportf("spawned renderer...");
}

void drawBitmapString(const char *s)
{
    while (*s) {
        glutBitmapCharacter(GLUT_BITMAP_TIMES_ROMAN_24, *s);
        s++;
    }
}

// assume sRGB-encoded float inputs
void setColor3f(float r, float g, float b)
{
    if (use_sRGB) {
        float linear_r = convertSRGBColorComponentToLinearf(r);
        float linear_g = convertSRGBColorComponentToLinearf(g);
        float linear_b = convertSRGBColorComponentToLinearf(b);
        glColor3f(linear_r, linear_g, linear_b);
    } else {
        glColor3f(r, g, b);
    }
}

void drawPathString(const char *s)
{
    const int emScale = 2048;

    static bool font_initialized = false;
    static GLuint glyphBase;
    static GLfloat horizontalAdvance[256];

    static GLfloat yMin, yMax, underline_position, underline_thickness;

    if (!font_initialized) {
        const int numChars = 256;  /* ISO/IEC 8859-1 8-bit character range */
        GLfloat horizontalAdvance[256];
        glyphBase = glGenPathsNV(1 + numChars);

        // Use the path object at the end of the range as a template.
        GLuint pathTemplate = glyphBase + numChars;
        glPathCommandsNV(pathTemplate, 0, NULL, 0, GL_FLOAT, NULL);
        // Stroke width is 5% of the em scale.
        glPathParameteriNV(pathTemplate, GL_PATH_STROKE_WIDTH_NV, 0.25 * emScale);
        glPathParameteriNV(pathTemplate, GL_PATH_JOIN_STYLE_NV, GL_ROUND_NV);

        glPathGlyphRangeNV(glyphBase,
            GL_STANDARD_FONT_NAME_NV, "Sans", GL_BOLD_BIT_NV,
            0, numChars,
            GL_SKIP_MISSING_GLYPH_NV, pathTemplate, emScale);
        // Query font and glyph metrics.
        float font_data[4];
        glGetPathMetricRangeNV(GL_FONT_Y_MIN_BOUNDS_BIT_NV | GL_FONT_Y_MAX_BOUNDS_BIT_NV |
            GL_FONT_UNDERLINE_POSITION_BIT_NV | GL_FONT_UNDERLINE_THICKNESS_BIT_NV,
            glyphBase + ' ', /*count*/1,
            4 * sizeof(GLfloat),
            font_data);
        yMin = font_data[0];
        yMax = font_data[1];
        underline_position = font_data[2];
        underline_thickness = font_data[3];

        glGetPathMetricRangeNV(GL_GLYPH_HORIZONTAL_BEARING_ADVANCE_BIT_NV,
            glyphBase, numChars,
            0, /* stride of zero means sizeof(GLfloat) since 1 bit in mask */
            horizontalAdvance);

        font_initialized = true;
    }

    size_t len = strlen(s);
    if (len < 1) {
        return;   // just ignore zero length strings
    }

    GLfloat *xtranslate = (GLfloat*)alloca(sizeof(GLfloat)*len);
    if (!xtranslate) {
        fprintf(stderr, "%s: malloc of xtranslate failed\n", program_name);
        exit(1);
    }
    xtranslate[0] = 0.0;  /* Initial xtranslate is zero. */
    {
        // Use 100% spacing; use 0.9 for both for 90% spacing.
        GLfloat advanceScale = 1.0,
            kerningScale = 1.0; // Set this to zero to ignore kerning.
        glGetPathSpacingNV(GL_ACCUM_ADJACENT_PAIRS_NV,
            (GLsizei)len, GL_UNSIGNED_BYTE, s,
            glyphBase,
            advanceScale, kerningScale,
            GL_TRANSLATE_X_NV,
            &xtranslate[1]);  /* messageLen-1 accumulated translates are written here. */
    }

    // Total advance is accumulated spacing plus horizontal advance of
    // the last glyph
    assert(len >= 1);  // already bailed for zero length above
    GLfloat totalAdvance = xtranslate[len - 1] +
        horizontalAdvance[s[len - 1]];

    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_NOTEQUAL, 0, ~0);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
    glStencilMask(~0);

    glMatrixPushEXT(GL_MODELVIEW); {
        GLfloat scale = 1.0f/(12*emScale);
        glMatrixTranslatefEXT(GL_MODELVIEW, -0.97f, -0.75f, 0);
        glMatrixScalefEXT(GL_MODELVIEW, scale, scale, 1);

        // Dark under stroked glyphs for offsetting
        setColor3f(0.2, 0.2, 0.2);
        glStencilThenCoverStrokePathInstancedNV((GLsizei)len,
            GL_UNSIGNED_BYTE, s, glyphBase,
            1, ~0,  /* Use all stencil bits */
            GL_BOUNDING_BOX_OF_BOUNDING_BOXES_NV,
            GL_TRANSLATE_X_NV, xtranslate);
        // Filled white glyphs
        setColor3f(1, 1, 1);
        glStencilThenCoverFillPathInstancedNV((GLsizei)len,
            GL_UNSIGNED_BYTE, s, glyphBase,
            GL_PATH_FILL_MODE_NV, ~0,  /* Use all stencil bits */
            GL_BOUNDING_BOX_OF_BOUNDING_BOXES_NV,
            GL_TRANSLATE_X_NV, xtranslate);

    } glMatrixPopEXT(GL_MODELVIEW);

    glDisable(GL_STENCIL_TEST);

    // no need to free xtranslate since used alloca
}

void drawString(const char * s)
{
    if (use_nvpr) {
        drawPathString(s);
    } else {
        drawBitmapString(s);
    }
}

// assume sRGB-encoded float inputs
void setClearColor(float r, float g, float b, float a)
{
    if (use_sRGB) {
        float linear_r = convertSRGBColorComponentToLinearf(r);
        float linear_g = convertSRGBColorComponentToLinearf(g);
        float linear_b = convertSRGBColorComponentToLinearf(b);
        glClearColor(linear_r, linear_g, linear_b, a);
    } else {
        glClearColor(r, g, b, a);
    }
}

// Renderer's scene drawing
void renderScene()
{
    static int rotation = 0;
    setClearColor(0.5, 0.5, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glPushMatrix(); {
        logf("rotation = %d", rotation);
        glRotatef(rotation, 1, 1, 0);
        rotation = (rotation + 1) % 360;
        setColor3f(1, 1, 1);
        switch (sharedData->object_to_draw % 3) {
        case 0:
            glutWireSphere(0.5, 10, 10);
            break;
        case 1:
            glutWireCube(0.5);
            break;
        case 2:
            glutSolidTeapot(0.5);
            break;
        default:
            assert(!"bogus object");
        }
    } glPopMatrix();

    setClearColor(1, 0, 0, 1);

    int time = glutGet(GLUT_ELAPSED_TIME);
    glWindowPos2f(40, 50);
    char buffer[200];
    sprintf(buffer, "Drawn in renderer after %0.2f seconds", time/1000.f);
    drawString(buffer);
}

void displayRenderer()
{
    assert(i_am_renderer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    glViewport(0, 0, window_width, window_height);
    renderScene();
    glutSwapBuffers();
}

void handleFailedLock()
{
    DWORD err = GetLastError();
    formatMessage(err);
    switch (HRESULT_CODE(err)) {
    case ERROR_BUSY:
        logf("object already locked!");
        break;
    case ERROR_INVALID_DATA:
        logf("object to lock does not belong to device");
        break;
    case ERROR_LOCK_FAILED:
        logf("lock failed");
        break;
    default:
        assert(!"unexpected wglDXLockObjectsNV error");
        break;
    }
}

void handleFailedUnlock()
{
    DWORD err = GetLastError();
    formatMessage(err);
    switch (HRESULT_CODE(err)) {
    case ERROR_BUSY:
        logf("object not locked!");
        break;
    case ERROR_INVALID_DATA:
        logf("object to unlock does not belong to device");
        break;
    case ERROR_LOCK_FAILED:
        logf("unlock failed");
        break;
    default:
        assert(!"unexpected wglDXUnlockObjectsNV error");
        break;
    }
}

bool renderLockedSharedTexture(SharedTexture& tex, void(*renderFunc)(SharedTexture& tex))
{
    BOOL lock_ok = wglDXLockObjectsNV(wgl_d3d_device, 1, &tex.sharetex_wgl_lock_handle);
    if (lock_ok) {
        renderFunc(tex);

        BOOL unlock_ok = wglDXUnlockObjectsNV(wgl_d3d_device, 1, &tex.sharetex_wgl_lock_handle);
        if (unlock_ok) {
            logf("successful lock/render/unlock");
            return true;
        } else {
            handleFailedUnlock();
            return false;
        }
    } else {
        handleFailedLock();
        return false;
    }
}

void grabAllLocks()
{
    for (int i = 0; i<render_buffer_count; i++) {
        BOOL lock_ok = wglDXLockObjectsNV(wgl_d3d_device, 1, &sharetex[i].sharetex_wgl_lock_handle);
        logf("wglDXLockObjectsNV = %d");
    }
}

void renderSharedTexture(SharedTexture& tex)
{
    const int time = glutGet(GLUT_ELAPSED_TIME);
    const int mask255 = 0x3ff;
    const int timeMod255 = time & mask255;
    const float percent = timeMod255 / float(mask255 + 1);
    const float radians = percent*3.14159 * 2;
    logf("time = %d ms, x = %g, sin = %g", time, percent, sin(radians));

    // Oscillate rotation of -10 to 10 degrees.
    glMatrixLoadIdentityEXT(GL_MODELVIEW);
    glMatrixRotatefEXT(GL_MODELVIEW, sin(radians) * 10, 0, 0, 1);

    // Rotate shared texture image to be clear its a texture.
    glMatrixLoadIdentityEXT(GL_TEXTURE);
    glMatrixRotatefEXT(GL_TEXTURE, 15, 0, 0, 1);

    setColor3f(1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, tex.sharetex_gl);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUAD_STRIP); {
        glTexCoord2f(0, 0);
        glVertex2f(-0.8, -0.8);
        glTexCoord2f(2, 0);
        glVertex2f(0.8, -0.8);
        glTexCoord2f(0, 2);
        glVertex2f(-0.8, 0.8);
        glTexCoord2f(2, 2);
        glVertex2f(0.8, 0.8);
    } glEnd();

    glMatrixLoadIdentityEXT(GL_MODELVIEW);
}

void displayMaster()
{
    assert(i_am_master);

    logf("displaying index = %d", current_sharetex_index);

    if (sharedData->produceCount - sharedData->consumeCount < 1) {
        logf("empty FIFO in displayMaster");
        setClearColor(0, 0.1f, 0, 1);  // dark green
        glClear(GL_COLOR_BUFFER_BIT);
        setColor3f(1,1,1);
        glWindowPos2i(20, 250);
        drawString("Waiting for renderer process to start...");
        glutSwapBuffers();
        return;
    }

    UINT32 ndx = sharedData->consumeCount % render_buffer_count;
    logf("produce to index %d", ndx);

    current_sharetex_index = ndx;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    setClearColor(0, 1, 0, 1);  // dark green
    glClear(GL_COLOR_BUFFER_BIT);

    renderLockedSharedTexture(sharetex[current_sharetex_index], renderSharedTexture);

    glMatrixLoadIdentityEXT(GL_TEXTURE);
    glDisable(GL_TEXTURE_2D);
    handleFPS(&fps_ctx);

    glutSwapBuffers();

    while (sharedData->produceCount - sharedData->consumeCount > 1) {
        UINT32 ndx = sharedData->consumeCount % render_buffer_count;
        logf("skipping index = %d", ndx);
        InterlockedIncrement(&sharedData->consumeCount);
    }
}

// Run by the renderer process
void startRendererProcess(HANDLE map_file_handle)
{
#if 0  // enable to hit a break point when renderer process starts
    __debugbreak();
#endif
    i_am_renderer = true;
    i_am_master = false;
    reportf("pid = %d, parent = %d", GetCurrentProcessId(), GetParentProcessId());

    logf("startRendererProcess: map_file_handle = %p", map_file_handle);
    sharedData = (SharedData*)MapViewOfFile(map_file_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedData));
    logf("sharedData=0x%p", sharedData);
    logf("width = %d", sharedData->width);
    logf("height = %d", sharedData->height);
    logf("render_buffer_count = %d", sharedData->render_buffer_count);
    for (UINT i = 0; i < sharedData->render_buffer_count; i++) {
        logf("sharedHandle[%d] = %p", i, sharedData->sharedHandle[i]);
    }
    logf("renderer_should_terminate = %d", sharedData->renderer_should_terminate);
    logf("master_should_terminate = %d", sharedData->master_should_terminate);

    HANDLE remoteProcess = OpenProcess(PROCESS_ALL_ACCESS, TRUE, GetParentProcessId());

    for (UINT i = 0; i < sharedData->render_buffer_count; i++) {
        HANDLE hRemoteSharedHandle = sharedData->sharedHandle[i];
        sharedData->sharedHandle[i] = 0;
        BOOL ok = DuplicateHandle(remoteProcess, hRemoteSharedHandle,
            GetCurrentProcess(), &sharedData->sharedHandle[i],
            0, TRUE, DUPLICATE_SAME_ACCESS);
        logf("DuplicateHandle = %d (handle=%p)", ok, sharedData->sharedHandle[i]);
        if (!ok) {
            DWORD err = GetLastError();
            logf("err = %d", err);
        }
        assert(sharedData->sharedHandle[i]);
        logf("sharedHandle[%d] = %p", i, sharedData->sharedHandle[i]);
    }
}

void renderSceneToTexture(SharedTexture& tex)
{
    glBindFramebuffer(GL_FRAMEBUFFER, tex.fbo);
    glViewport(0,0, sharedData->width, sharedData->height);

    logf("render scene");
    renderScene();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (mipmap_sharetex) {
        logf("generate mipmaps");
        glGenerateTextureMipmap(tex.sharetex_gl);
    }
}

// Master's atexit callback
static void masterExitCalled()
{
    sharedData->renderer_should_terminate = true;
}

// Renderer's atexit callback
static void rendererExitCalled()
{
    sharedData->master_should_terminate = true;
}

void generateNewFrame()
{
    if (sharedData->renderer_should_terminate) {
        logf("master says renderer should terminate");
        exit(0);
    }

    if (sharedData->produceCount - sharedData->consumeCount == render_buffer_count) {
        logf("FIFO backed up!");
        return;
    }

    UINT32 ndx = sharedData->produceCount % render_buffer_count;
    logf("produce to index %d", ndx);

    renderLockedSharedTexture(sharetex[ndx], renderSceneToTexture);

    InterlockedIncrement(&sharedData->produceCount);
}

void delayGenerateNewFrame(int value)
{
    generateNewFrame();
    glutTimerFunc(sharedData->render_interval, delayGenerateNewFrame, 0);
    if (sharedData->timer_updates_renderer_window) {
        glutPostRedisplay();
    }
}

void idleMaster()
{
    if (sharedData->master_should_terminate) {
        logf("renderer says master should terminate");
        exit(0);
    }

    if (sharedData->produceCount - sharedData->consumeCount == 0) {
        return;
    }

    UINT32 ndx = sharedData->consumeCount % render_buffer_count;
    logf("consume from index %d", ndx);

    current_sharetex_index = ndx;

    glutPostRedisplay();
}

void keyboard(unsigned char c, int x, int y)
{
    switch (c) {
    case 27:  // Escape, quits
        exit(0);
        break;
    case 't':
        InterlockedXor(&sharedData->timer_updates_renderer_window, 0x1);
        break;
    case 'o':
        InterlockedIncrement(&sharedData->object_to_draw);
        break;
    case ' ':
        if (i_am_renderer) {
            generateNewFrame();
        }
        return;
    case '+':
    case '=':
        // XXX Allow this from either master or renderer process though not properly locked.
        if (sharedData->render_interval < 100) {
            sharedData->render_interval += 10;
        } else {
            sharedData->render_interval += 100;
        }
        reportf("render_interval = %d", sharedData->render_interval);
        return;
    case '-':
    case '_':
        // XXX Allow this from either master or renderer process though not properly locked.
        if (sharedData->render_interval <= 100) {
            sharedData->render_interval = max(10, sharedData->render_interval - 10);
        } else {
            sharedData->render_interval = sharedData->render_interval - 100;
        }
        reportf("render_interval = %d", sharedData->render_interval);
        return;
    case 'l':
        sharedData->logging = !sharedData->logging;
        break;
    case 'H':
        if (i_am_renderer) {
            reportf("induce hang...");
            // Induce hang.
            grabAllLocks();
            Sleep(10*1000);
            reportf("done sleeping.");
        }
        break;
    case 'm':
        toggleFPSunits();
        return;
    case 'v':
        swap_interval = !swap_interval;
        reportf("swap_interval = %d", swap_interval);
        requestSynchornizedSwapBuffers(swap_interval);
        break;
    case 13:  // Enter, forces redraw
        break;
    default:
        return;  // skips posting redisplay for unrecognized key
    }
    glutPostRedisplay();
}

void menu(int item)
{
    keyboard(item, 0, 0);
}

void createSharedTextures()
{
    assert(i_am_master);
    reportf("make shared textures");
    for (int i = 0; i < render_buffer_count; i++) {
        createTexture2D(sharetex[i], fbo_width, fbo_height, use_sRGB);
        logf("%d: sharetex_gl = %d", sharetex[i].sharetex_gl);
        logf("%d: sharetex_d3d = %p", sharetex[i].sharetex_d3d);
        logf("%d: sharetex_handle = %p", sharetex[i].sharetex_handle);
        logf("%d: sharetex_wgl_lock_handle = %p", sharetex[i].sharetex_wgl_lock_handle);
    }
}

void initMaster(const char *program_name)
{
    assert(i_am_master);
    if (use_sRGB) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    setClearColor(0, 1, 0, 1);  // green
    logf("me = %d, parent = %d", GetCurrentProcessId(), GetParentProcessId());

    createSharedTextures();

    spawnRendererProcess(program_name);
    glutIdleFunc(idleMaster);
}

void establishRendererSharedTexture(SharedTexture& tex, HANDLE sharetex_handle)
{
    // Get HANDLE to shared texture from shared memory.
    tex.sharetex_handle = sharetex_handle;
    assert(tex.sharetex_handle);

    // Get ID3D11Texture2D pointer for shared texture from its HANDLE.
    d3d_device1->OpenSharedResource1(
        tex.sharetex_handle,
        __uuidof(ID3D11Texture2D),
        (void**)&tex.sharetex_d3d);
    logf("sharetex_d3d = %p", tex.sharetex_d3d);
    D3D11_RESOURCE_DIMENSION dim;
    tex.sharetex_d3d->GetType(&dim);
    assert(dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D);

    // Allocate an OpenGL texture name.
    tex.sharetex_gl = 0;
    glGenTextures(1, &tex.sharetex_gl);
    logf("sharetex_gl=%d", tex.sharetex_gl);
    assert(tex.sharetex_gl);
    BOOL ok = wglDXSetResourceShareHandleNV(tex.sharetex_d3d, tex.sharetex_handle);
    assert(ok);
    logf("wglDXSetResourceShareHandleNV = %d", ok);

    GLenum texture_target = GL_TEXTURE_2D;
    tex.sharetex_wgl_lock_handle = wglDXRegisterObjectNV(wgl_d3d_device, tex.sharetex_d3d,
        tex.sharetex_gl, texture_target, WGL_ACCESS_READ_WRITE_NV);
    assert(tex.sharetex_wgl_lock_handle);

    if (tex.sharetex_wgl_lock_handle == NULL) {
        reportf("wglDXRegisterObjectNV failed");
        exit(1);
    }

    DWORD parent_pid = (DWORD)GetParentProcessId();
    logf("me = %d, parent = %d", GetCurrentProcessId(), parent_pid);

    glGenFramebuffers(1, &tex.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, tex.fbo);
    logf("%d: fbo = %d", tex.fbo);
    const GLint base_level = 0;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex.sharetex_gl, base_level);
    glGenTextures(1, &stencil_tex);
    const GLsizei one_level = 1;  // No mipmaps
    glTextureStorage2DEXT(stencil_tex, GL_TEXTURE_2D, one_level, GL_STENCIL_INDEX8, fbo_width, fbo_height);
    //glTextureImage2DEXT(stencil_tex, GL_TEXTURE_2D, base_level, GL_STENCIL_INDEX8, fbo_width, fbo_height, 0, GL_INTENSITY, GL_FLOAT, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, stencil_tex, base_level);
}

void establishRendererSharedTextures()
{
    assert(i_am_renderer);
    for (int i = 0; i < render_buffer_count; i++) {
        logf("establishRendererSharedTexture: %d", i);
        establishRendererSharedTexture(sharetex[i], sharedData->sharedHandle[i]);
    }
}

void initRenderer()
{
    assert(i_am_renderer);

    // Copy shared variable values from shared memory to gloabls.
    render_buffer_count = sharedData->render_buffer_count;
    mipmap_sharetex = sharedData->mipmap_sharetex;
    logging = sharedData->logging;
    use_nvpr = sharedData->use_nvpr;
    use_sRGB = sharedData->use_sRGB;

    if (use_sRGB) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    establishRendererSharedTextures();

    generateNewFrame();
    glutTimerFunc(sharedData->render_interval, delayGenerateNewFrame, 0);
}

static int findExtension(const char *extension, const char *extensions)
{
    const char *start;
    const char *where, *terminator;

    start = extensions;
    for (;;) {
        /* If your application crashes in the strstr routine below,
        you are probably calling ExtensionSupported without
        having a current window.  Calling glGetString without
        a current OpenGL context has unpredictable results.
        Please fix your program. */
        where = strstr(start, extension);
        if (!where)
            break;
        terminator = where + strlen(extension);
        if (where == start || *(where - 1) == ' ') {
            if (*terminator == ' ' || *terminator == '\0') {
                return 1;
            }
        }
        start = terminator;
    }

    return 0;
}

int WGLExtensionSupported(const char *extension)
{
    static const char *wgl_extensions_str = NULL;
    if (!wgl_extensions_str) {
        wgl_extensions_str = wglGetExtensionsStringARB(wglGetCurrentDC());
    }
    return findExtension(extension, wgl_extensions_str);
}

#ifdef _WIN32
# if !defined(strnicmp) && !defined(__CYGWIN__)
#  define strnicmp _strnicmp  // avoid deprecation warning
# endif
# if !defined(stricmp) && !defined(__CYGWIN__)
#  define stricmp _stricmp  // avoid deprecation warning
# endif
# if !defined(snprintf) && !defined(__CYGWIN__)
#  define snprintf _snprintf
# endif
#else
# include <strings.h>
# define strnicmp strncasecmp
#endif

int main(int argc, char *argv[])
{
    glutInit(&argc, argv);
    // Search for flag to spawn renderer process.
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i], SPAWN_RENDERER_FLAG) && argv[i+1] != NULL) {
            HANDLE map_file_handle = (HANDLE)atoll(argv[i+1]);
            logf("main: map_file_handle = %p", map_file_handle);
            startRendererProcess(map_file_handle);
            i++;
            break;
        }
        if (!strcmp(argv[i], "-nomipmap")) {
            mipmap_sharetex = false;
            continue;
        }
        if (!stricmp(argv[i], "-sRGB")) {
            use_sRGB = true;
            printf("enable sRGB framebuffer\n");
            continue;
        }
        if (!stricmp(argv[i], "-dxDebug")) {  // flag to set D3D11_CREATE_DEVICE_DEBUG
            set_dx_device_debug_flag = true;
            printf("enable D3D11_CREATE_DEVICE_DEBUG flag\n");
            continue;
        }
        if (!strcmp(argv[i], "-bitmap_text")) {
            use_nvpr = false;
            continue;
        }
        if (!strcmp(argv[i], "-novsync")) {
            swap_interval = 0;
            continue;
        }
        if (!strcmp(argv[i], "-log")) {
            logging = true;
            continue;
        }
        if (!strcmp(argv[i], "-buffers") && argv[i + 1] != NULL) {
            render_buffer_count = atoi(argv[i+1]);
            render_buffer_count = max(2, min(render_buffer_count, 4));
            printf("render_buffer_count = %d\n", render_buffer_count);
            i++;
            continue;
        }
        if (!strcmp(argv[i], "-size") && argv[i + 1] != NULL) {
            int size = atoi(argv[i + 1]);
            int clamped_size = max(32, min(size, 4096));
            fbo_width = clamped_size;
            fbo_height = clamped_size;
            printf("buffer size = %dx%d\n", fbo_width, fbo_height);
            i++;
            continue;
        }
        printf("usage: %s [-novsync] [-log] [-buffers 2/3/4] [-nomipmap]\n", program_name);
        exit(1);
    }
    glutInitWindowSize(window_width, window_height);
    if (i_am_renderer) {
        // Put renderer window to right of master window.
        glutInitWindowPosition(10 + window_width + 50, 10);
    } else {
        glutInitWindowPosition(10, 10);
    }
    glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH | GLUT_STENCIL);
    const char *window_title = i_am_master ? "interop master (app)" : "interop renderer (OpenGL sandbox)";
    glutCreateWindow(window_title);
    GLenum result = glewInit();
    if (result != GLEW_OK) {
        fatalError("OpenGL Extension Wrangler (GLEW) failed to initialize");
    }
    bool lacks_NV_DX_interop2 = !WGLExtensionSupported("WGL_NV_DX_interop2");
    if (lacks_NV_DX_interop2) {
        fatalError("%s: requires WGL_NV_DX_interop2 OpenGL extension to WGL");
    }
    bool lacks_EXT_direct_state_acces = !glutExtensionSupported("GL_EXT_direct_state_access");
    if (lacks_EXT_direct_state_acces) {
        fatalError("%s: requires GL_EXT_direct_state_access OpenGL extension");
    }
    if (i_am_master) {
        glutDisplayFunc(displayMaster);
        atexit(masterExitCalled);
    } else {
        glutDisplayFunc(displayRenderer);
        atexit(rendererExitCalled);
    }
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutCreateMenu(menu);
    glutAddMenuEntry("[m] Toggle fps vs. milliseconds", 'm');
    glutAddMenuEntry("[v] Toggle frame synchronization", 'v');
    glutAddMenuEntry("[o] Cycle objects", 'o');
    glutAddMenuEntry("[t] Toggle timer updates renderer window", 't');
    glutAddMenuEntry("[+] Increase render interval", '+');
    glutAddMenuEntry("[-] Decrease render interval", '-');
    glutAddMenuEntry("[Esc] Quit", 27);
    glutAttachMenu(GLUT_RIGHT_BUTTON);

    // Both master & renderer need to initalize Direct3D for interop.
    LoadDirect3D();
    InitiallizeDirect3D();
    InteropWithDirect3D();

    if (i_am_master) {
        initMaster(argv[0]);
        requestSynchornizedSwapBuffers(swap_interval);
    } else {
        initRenderer();
        // Renderer should run unsynchronized.
        requestSynchornizedSwapBuffers(0);
    }

    initFPScontext(&fps_ctx, FPS_USAGE_TEXTURE);
    if (i_am_master) {
        enableFPS();
    }

    logf("start glutMainLoop");
    glutMainLoop();
    return 0;
}

