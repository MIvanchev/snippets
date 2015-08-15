/*
 * Пикселс - a quick & dirty Win32 setup for experiments involving direct
 * pixel access.
 *
 * Just implement the function updateScreen (invoked every frame) and you're all
 * set. By default it fills the screen with random pixels. The frame buffer is
 * accessible through the global variable buf. The visible portion of the buffer
 * has the dimensions WIDTH x HEIGHT, but the real width of each scanline is
 * stored in the global variable bufPitch. Each scanline of the buffer is
 * aligned on a 16-byte boundary thus allowing efficient SIMD (e.g. SSE)
 * operations.
 *
 * To compile with MinGW or MinGW-w64 type:
 *
 * gcc pixels.c -o pixels.exe -lgdi32
 *
 * To compile with SSE support, you can add one of the flags -msse, -msse2,
 * -msse3 etc. The compilation with Visual Studio is untested, but should be
 * straightforward. Contributions are always welcome :)
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Mihail Ivanchev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define UNICODE
#define _UNICODE
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <tchar.h>

#define TITLE _T("Пикселс")
#define WIDTH 800
#define HEIGHT 600
#define SCREEN_UPDATE_TIMER_ID 1
#define FPS_UPDATE_INTERVAL 500

#define ErrorDlg(msg) MessageBox(NULL, msg, _T("Error"), MB_ICONERROR | MB_OK)

static int exitCode;
static HWND window;
static HDC bufDc;
static HGDIOBJ bufDcPrevObj;
static HBITMAP bufBmp;
static DWORD *buf;
static DWORD bufPitch;
static DWORD bufOffset;
static int numFrames;
static DWORD lastTime;
static DWORD elapsedTime;

static void reportSysError(LPCTSTR);
static int initFrameBuffer();
static void clearFrameBuffer();
static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM) __attribute__((force_align_arg_pointer));
static void updateFps();
static void deinitialize();
static int createAppWindow(HINSTANCE, int);
static void updateScreen();

void reportSysError(LPCTSTR prefix)
{
    TCHAR sysmsg[256], errmsg[256];
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  GetLastError(),
                  MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                  sysmsg,
                  256,
                  NULL);
    wcsncpy(errmsg, prefix, 256);
    wcsncat(errmsg, _T(" "), 256);
    wcsncat(errmsg, sysmsg, 256);
    ErrorDlg(errmsg);
}

LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HDC dstDc;
    PAINTSTRUCT paintInfo;
    BOOL result;

    switch (msg)
    {
    case WM_CREATE:
        if (initFrameBuffer())
            return -1;

        if (!SetTimer(window, SCREEN_UPDATE_TIMER_ID, USER_TIMER_MINIMUM, NULL))
        {
            reportSysError(_T("Failed to create the display update timer."));
            return -1;
        }

        break;
    case WM_DESTROY:
        deinitialize();
        PostQuitMessage(exitCode);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(window);
        break;
    case WM_TIMER:
        InvalidateRect(window, NULL, FALSE);
        break;
    case WM_PAINT:
        dstDc = BeginPaint(window, &paintInfo);
        updateScreen();
        BitBlt(dstDc, - (int) bufOffset, 0, WIDTH, HEIGHT, bufDc, 0, 0, SRCCOPY);
        EndPaint(window, &paintInfo);
        updateFps();

        break;
    default:
        return DefWindowProc(window, msg, wParam, lParam);
    }

    return 0;
}

void updateFps()
{
    DWORD currTime;
    int fps;
    TCHAR title[256];

    numFrames++;

    currTime = GetTickCount();
    if (currTime < lastTime)
        elapsedTime += 0xFFFFFFFF - lastTime + currTime;
    else
        elapsedTime += currTime - lastTime;

    if (elapsedTime >= FPS_UPDATE_INTERVAL)
    {
        fps = (1000 * numFrames) / elapsedTime;
        swprintf(title, 256, _T("%s (FPS: %d)"), TITLE, fps);
        SetWindowText(window, title);
        elapsedTime = 0;
        numFrames = 0;
    }

    lastTime = currTime;
}

void deinitialize()
{
    if (window)
        KillTimer(window, SCREEN_UPDATE_TIMER_ID);

    if (bufDcPrevObj)
        SelectObject(bufDc, bufDcPrevObj);

    if (bufBmp)
        DeleteObject(bufBmp);

    if (bufDc)
        DeleteDC(bufDc);
}

int initFrameBuffer()
{
    BITMAPINFO bmpInfo;
    uintptr_t defaultAddr;
    uintptr_t alignedAddr;
    unsigned int alignmentError;

    /*
     * The Windows DIBs are guaranteed to be aligned on a 4-byte boundary so 3
     * pixels (12 bytes) are prepended to the scanlines in order to realign the
     * buffer to a 16-byte boundary whenever necessary. The buffer pitch is then
     * further increased to make sure that each scanline is aligned on a 16-byte
     * boundary.
     */

    bufPitch = WIDTH + 3;
    alignmentError = (bufPitch * 4) % 16;
    if (alignmentError)
        bufPitch += (16 - alignmentError) / 4;

    memset(&bmpInfo, 0, sizeof(BITMAPINFO));

    BITMAPINFOHEADER *infoHdr = &bmpInfo.bmiHeader;
    infoHdr->biSize = sizeof(BITMAPINFOHEADER);
    infoHdr->biWidth = bufPitch;
    infoHdr->biHeight = HEIGHT;
    infoHdr->biPlanes = 1;
    infoHdr->biBitCount = 32;
    infoHdr->biCompression = BI_RGB;

    bufDc = CreateCompatibleDC(NULL);
    if (!bufDc)
    {
        reportSysError(_T("Failed to create a device context for the frame buffer."));
        return 1;
    }

    bufBmp = CreateDIBSection(bufDc,
                              &bmpInfo,
                              DIB_RGB_COLORS,
                              (LPVOID *) &buf,
                              NULL,
                              0);
    if (!bufBmp)
    {
        reportSysError(
            _T("Failed to create a device independent bitmap (DIB) for the frame buffer."));
        return 1;
    }

    bufDcPrevObj = SelectObject(bufDc, bufBmp);
    if (!bufDcPrevObj)
    {
        reportSysError(
            _T("Failed to associate the frame buffer with its device context."));
        return 1;
    }

    /*
     * Realign the buffer to a 16-byte boundary, but remember how many pixels to
     * offset in the negative X direction when blitting the buffer.
     */

    defaultAddr = (uintptr_t) (void*) buf;
    alignedAddr = (defaultAddr + 15) & ~ (uintptr_t) 0xF;

    buf = (DWORD*) (void*) alignedAddr;
    bufOffset = (alignedAddr - defaultAddr) / 4;

    return 0;
}

void clearFrameBuffer(DWORD color)
{
#ifdef __SSE2__
    __asm__ ("cmpl      $0, %[cnt];                     \n\t"
             "je        2f;                             \n\t"
             "pshufd    $0, %[clr], %[clr];             \n\t"
             "xorl      %%ebx, %%ebx;                   \n\t"
             "1:                                        \n\t"
             "movdqa    %[clr], (%[buf], %%ebx, 4);     \n\t"
             "addl      $4, %%ebx;                      \n\t"
             "subl      $4, %[cnt];                     \n\t"
             "jnz       1b;                             \n\t"
             "2:                                        \n\t"
             :
             : [buf] "r"(buf), [cnt] "r"(HEIGHT * bufPitch), [clr] "x"(color)
             : "%ebx", "cc", "memory");
#else
    int i, j;
    for (j = 0; j < HEIGHT; j++)
    {
        for (i = 0; i < bufPitch; i++)
            buf[j * bufPitch + i] = color;
    }
#endif
}

int createAppWindow(HINSTANCE instance, int cmdShow)
{
    WNDCLASS wc;
    DWORD style;
    RECT clientRect;

    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = 0;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = _T("appclass");
    if (!RegisterClass(&wc))
    {
        reportSysError(_T("Failed to register the window class."));
        return 1;
    }

    clientRect.top = 0;
    clientRect.left = 0;
    clientRect.bottom = HEIGHT - 1;
    clientRect.right = WIDTH - 1;
    style = WS_OVERLAPPEDWINDOW & ~WS_SIZEBOX;
    if (!AdjustWindowRect(&clientRect, style, FALSE))
    {
        reportSysError(_T("Failed to calculate the window size."));
        return 1;
    }

    window = CreateWindow(wc.lpszClassName,
                          TITLE,
                          WS_OVERLAPPEDWINDOW & ~(WS_SIZEBOX | WS_MAXIMIZEBOX),
                          CW_USEDEFAULT,
                          CW_USEDEFAULT,
                          (clientRect.right - clientRect.left) + 1,
                          (clientRect.bottom - clientRect.top) + 1,
                          NULL,
                          NULL,
                          instance,
                          NULL);
    if (!window)
    {
        reportSysError(_T("Failed to create the application window."));
        return 1;
    }

    ShowWindow(window, cmdShow);
    UpdateWindow(window);

    return 0;
}

void updateScreen()
{
    int i, j, r, g, b;

    clearFrameBuffer(0x000000);

    for (j = 0; j < HEIGHT; j++)
    {
        for (i = 0; i < WIDTH; i++)
        {
            r = rand() % 256;
            g = rand() % 256;
            b = rand() % 256;

            buf[j * bufPitch + i] = (r << 18) + (g << 8) + b;
        }
    }
}

__attribute__((force_align_arg_pointer))
int WINAPI WinMain(HINSTANCE currInst,
                   HINSTANCE prevInst,
                   LPSTR cmdLine,
                   int cmdShow)
{
    MSG msg;

    /*
     * According to MSDN 0 should always be returned if the message loop hasn't
     * been entered yet.
     */

    if (createAppWindow(currInst, cmdShow))
        return 0;

    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return msg.wParam;
}
