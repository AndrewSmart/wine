/*
 * Copyright 1993      Martin Ayotte
 *           1998-2002 Eric Pouech
 *           2011 Andrew Eikum for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "mmsystem.h"
#include "mmreg.h"
#include "msacm.h"
#include "winuser.h"
#include "winnls.h"
#include "winternl.h"

#include "winemm.h"

#include "ole2.h"
#include "initguid.h"
#include "devpkey.h"
#include "mmdeviceapi.h"
#include "audioclient.h"
#include "audiopolicy.h"

#include "wine/debug.h"

/* TODO: Remove after dsound has been rewritten for mmdevapi */
#include "dsound.h"
#include "dsdriver.h"
#define DS_HW_ACCEL_FULL 0

WINE_DEFAULT_DEBUG_CHANNEL(winmm);

/* HWAVE (and HMIXER) format:
 *
 * XXXX... 1FDD DDDD IIII IIII
 * X = unused (must be 0)
 * 1 = the bit is set to 1, to avoid all-zero HWAVEs
 * F = flow direction (0 = IN, 1 = OUT)
 * D = index into g_out_mmdevices
 * I = index in the mmdevice's devices array
 *
 * Two reasons that we don't just use pointers:
 *   - HWAVEs must fit into 16 bits for compatibility with old applications.
 *   - We must be able to identify bad devices without crashing.
 */

#define MAX_DEVICES 256

typedef struct _WINMM_CBInfo {
    DWORD_PTR callback;
    DWORD_PTR user;
    DWORD flags;
    HWAVE hwave;
} WINMM_CBInfo;

struct _WINMM_MMDevice;
typedef struct _WINMM_MMDevice WINMM_MMDevice;

typedef struct _WINMM_Device {
    WINMM_CBInfo cb_info;

    HWAVE handle;

    BOOL open;

    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    IAudioCaptureClient *capture;
    IAudioClock *clock;
    IAudioStreamVolume *volume;

    HACMSTREAM acm_handle;
    ACMSTREAMHEADER acm_hdr;
    UINT32 acm_offs;

    WAVEHDR *first, *last, *playing, *loop_start;

    BOOL stopped;
    DWORD loop_counter;
    UINT32 bytes_per_frame, samples_per_sec, ofs_bytes, played_frames;

    /* stored in frames of sample rate, *not* AC::GetFrequency */
    UINT64 last_clock_pos;

    HANDLE event;
    CRITICAL_SECTION lock;

    WINMM_MMDevice *parent;
} WINMM_Device;

struct _WINMM_MMDevice {
    WAVEOUTCAPSW out_caps; /* must not be modified outside of WINMM_InitMMDevices*/
    WAVEINCAPSW in_caps; /* must not be modified outside of WINMM_InitMMDevices*/
    WCHAR *dev_id;

    GUID session;

    CRITICAL_SECTION lock;

    WINMM_Device *devices[MAX_DEVICES];
};

static WINMM_MMDevice *g_out_mmdevices;
static UINT g_outmmdevices_count;

static WINMM_MMDevice *g_in_mmdevices;
static UINT g_inmmdevices_count;

static IMMDeviceEnumerator *g_devenum;

static CRITICAL_SECTION g_devthread_lock;
static HANDLE g_devices_thread;
static HWND g_devices_hwnd;

static UINT g_devhandle_count;
static HANDLE *g_device_handles;
static WINMM_Device **g_handle_devices;

typedef struct _WINMM_OpenInfo {
    HWAVE handle;
    UINT req_device;
    WAVEFORMATEX *format;
    DWORD_PTR callback;
    DWORD_PTR cb_user;
    DWORD flags;
} WINMM_OpenInfo;

static LRESULT WOD_Open(WINMM_OpenInfo *info);
static LRESULT WOD_Close(HWAVEOUT hwave);
static LRESULT WID_Open(WINMM_OpenInfo *info);
static LRESULT WID_Close(HWAVEIN hwave);

BOOL WINMM_InitWaveform(void)
{
    InitializeCriticalSection(&g_devthread_lock);
    return TRUE;
}

static inline HWAVE WINMM_MakeHWAVE(UINT mmdevice, BOOL is_out, UINT device)
{
    return ULongToHandle((1 << 15) | ((!!is_out) << 14) |
            (mmdevice << 8) | device);
}

static inline void WINMM_DecomposeHWAVE(HWAVE hwave, UINT *mmdevice_index,
        BOOL *is_out, UINT *device_index, UINT *junk)
{
    ULONG32 l = HandleToULong(hwave);
    *device_index = l & 0xFF;
    *mmdevice_index = (l >> 8) & 0x3F;
    *is_out = (l >> 14) & 0x1;
    *junk = l >> 15;
}

static void WINMM_InitDevice(WINMM_Device *device,
        WINMM_MMDevice *parent, HWAVE hwave)
{
    InitializeCriticalSection(&device->lock);
    device->handle = hwave;
    device->parent = parent;
}

/* finds the first unused Device, marks it as "open", and returns
 * a pointer to the device
 *
 * IMPORTANT: it is the caller's responsibility to release the device's lock
 * on success
 */
static WINMM_Device *WINMM_FindUnusedDevice(BOOL is_out, UINT mmdevice_index)
{
    WINMM_MMDevice *mmdevice;
    UINT i;

    if(is_out)
        mmdevice = &g_out_mmdevices[mmdevice_index];
    else
        mmdevice = &g_in_mmdevices[mmdevice_index];

    EnterCriticalSection(&mmdevice->lock);
    for(i = 0; i < MAX_DEVICES; ++i){
        WINMM_Device *device = mmdevice->devices[i];

        if(!device){
            device = mmdevice->devices[i] = HeapAlloc(GetProcessHeap(),
                    HEAP_ZERO_MEMORY, sizeof(WINMM_Device));
            if(!device){
                LeaveCriticalSection(&mmdevice->lock);
                return NULL;
            }

            WINMM_InitDevice(device, mmdevice,
                    WINMM_MakeHWAVE(mmdevice_index, is_out, i));
            EnterCriticalSection(&device->lock);
        }else
            EnterCriticalSection(&device->lock);

        if(!device->open){
            LeaveCriticalSection(&mmdevice->lock);
            device->open = TRUE;
            TRACE("Found free device: mmdevice: %u, device id: %u\n",
                    mmdevice_index, i);
            return device;
        }

        LeaveCriticalSection(&device->lock);
    }

    LeaveCriticalSection(&mmdevice->lock);

    TRACE("All devices in use: mmdevice: %u\n", mmdevice_index);

    return NULL;
}

static inline BOOL WINMM_ValidateAndLock(WINMM_Device *device)
{
    if(!device)
        return FALSE;

    EnterCriticalSection(&device->lock);

    if(!device->open){
        LeaveCriticalSection(&device->lock);
        return FALSE;
    }

    return TRUE;
}

static WINMM_Device *WINMM_GetDeviceFromHWAVE(HWAVE hwave)
{
    WINMM_MMDevice *mmdevice;
    WINMM_Device *device;
    UINT mmdevice_index, device_index, junk;
    BOOL is_out;

    WINMM_DecomposeHWAVE(hwave, &mmdevice_index, &is_out, &device_index, &junk);

    if(junk != 0x1)
        return NULL;

    if(mmdevice_index >= (is_out ? g_outmmdevices_count : g_inmmdevices_count))
        return NULL;

    if(is_out)
        mmdevice = &g_out_mmdevices[mmdevice_index];
    else
        mmdevice = &g_in_mmdevices[mmdevice_index];

    EnterCriticalSection(&mmdevice->lock);

    device = mmdevice->devices[device_index];

    LeaveCriticalSection(&mmdevice->lock);

    return device;
}

/* Note: NotifyClient should never be called while holding the device lock
 * since the client may call wave* functions from within the callback. */
static DWORD WINMM_NotifyClient(WINMM_CBInfo *info, WORD msg, DWORD_PTR param1,
        DWORD_PTR param2)
{
    TRACE("(%p, %u, %lx, %lx)\n", info->hwave, msg, param1, param2);

    if(info->flags & DCB_NULL)
        return MMSYSERR_NOERROR;

    if(!DriverCallback(info->callback, info->flags, (HDRVR)info->hwave,
                msg, info->user, param1, param2))
        return MMSYSERR_ERROR;

    return MMSYSERR_NOERROR;
}

static HRESULT WINMM_GetFriendlyName(IMMDevice *device, WCHAR *out,
        UINT outlen)
{
    IPropertyStore *ps;
    PROPVARIANT var;
    HRESULT hr;

    hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &ps);
    if(FAILED(hr))
        return hr;

    PropVariantInit(&var);

    hr = IPropertyStore_GetValue(ps,
            (PROPERTYKEY*)&DEVPKEY_Device_FriendlyName, &var);
    if(FAILED(hr)){
        IPropertyStore_Release(ps);
        return hr;
    }

    lstrcpynW(out, var.u.pwszVal, outlen);

    PropVariantClear(&var);

    IPropertyStore_Release(ps);

    return S_OK;
}

static HRESULT WINMM_TestFormat(IAudioClient *client, DWORD rate, DWORD depth,
        WORD channels)
{
    WAVEFORMATEX fmt, *junk;
    HRESULT hr;

    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = channels;
    fmt.nSamplesPerSec = rate;
    fmt.wBitsPerSample = depth;
    fmt.nBlockAlign = (channels * depth) / 8;
    fmt.nAvgBytesPerSec = rate * fmt.nBlockAlign;
    fmt.cbSize = 0;

    hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_SHARED,
            &fmt, &junk);
    if(SUCCEEDED(hr))
        CoTaskMemFree(junk);

    return hr;
}

static struct _TestFormat {
    DWORD flag;
    DWORD rate;
    DWORD depth;
    WORD channels;
} formats_to_test[] = {
    { WAVE_FORMAT_1M08, 11025, 8, 1 },
    { WAVE_FORMAT_1M16, 11025, 16, 1 },
    { WAVE_FORMAT_1S08, 11025, 8, 2 },
    { WAVE_FORMAT_1S16, 11025, 16, 2 },
    { WAVE_FORMAT_2M08, 22050, 8, 1 },
    { WAVE_FORMAT_2M16, 22050, 16, 1 },
    { WAVE_FORMAT_2S08, 22050, 8, 2 },
    { WAVE_FORMAT_2S16, 22050, 16, 2 },
    { WAVE_FORMAT_4M08, 44100, 8, 1 },
    { WAVE_FORMAT_4M16, 44100, 16, 1 },
    { WAVE_FORMAT_4S08, 44100, 8, 2 },
    { WAVE_FORMAT_4S16, 44100, 16, 2 },
    { WAVE_FORMAT_48M08, 48000, 8, 1 },
    { WAVE_FORMAT_48M16, 48000, 16, 1 },
    { WAVE_FORMAT_48S08, 48000, 8, 2 },
    { WAVE_FORMAT_48S16, 48000, 16, 2 },
    { WAVE_FORMAT_96M08, 96000, 8, 1 },
    { WAVE_FORMAT_96M16, 96000, 16, 1 },
    { WAVE_FORMAT_96S08, 96000, 8, 2 },
    { WAVE_FORMAT_96S16, 96000, 16, 2 },
    {0}
};

static DWORD WINMM_GetSupportedFormats(IMMDevice *device)
{
    DWORD flags = 0;
    HRESULT hr;
    struct _TestFormat *fmt;
    IAudioClient *client;

    hr = IMMDevice_Activate(device, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&client);
    if(FAILED(hr))
        return 0;

    for(fmt = formats_to_test; fmt->flag; ++fmt){
        hr = WINMM_TestFormat(client, fmt->rate, fmt->depth, fmt->channels);
        if(hr == S_OK)
            flags |= fmt->flag;
    }

    IAudioClient_Release(client);

    return flags;
}

static HRESULT WINMM_InitMMDevice(EDataFlow flow, IMMDevice *device,
        WINMM_MMDevice *dev, UINT index)
{
    HRESULT hr;

    if(flow == eRender){
        dev->out_caps.wMid = 0xFF;
        dev->out_caps.wPid = 0xFF;
        dev->out_caps.vDriverVersion = 0x00010001;
        dev->out_caps.dwFormats = WINMM_GetSupportedFormats(device);
        dev->out_caps.wReserved1 = 0;
        dev->out_caps.dwSupport = WAVECAPS_LRVOLUME | WAVECAPS_VOLUME |
            WAVECAPS_SAMPLEACCURATE;
        dev->out_caps.wChannels = 2;
        dev->out_caps.szPname[0] = '\0';

        hr = WINMM_GetFriendlyName(device, dev->out_caps.szPname,
                sizeof(dev->out_caps.szPname) /
                sizeof(*dev->out_caps.szPname));
        if(FAILED(hr))
            return hr;
    }else{
        dev->in_caps.wMid = 0xFF;
        dev->in_caps.wPid = 0xFF;
        dev->in_caps.vDriverVersion = 0x00010001;
        dev->in_caps.dwFormats = WINMM_GetSupportedFormats(device);
        dev->in_caps.wReserved1 = 0;
        dev->in_caps.wChannels = 2;
        dev->in_caps.szPname[0] = '\0';

        hr = WINMM_GetFriendlyName(device, dev->in_caps.szPname,
                sizeof(dev->in_caps.szPname) /
                sizeof(*dev->in_caps.szPname));
        if(FAILED(hr))
            return hr;
    }

    hr = IMMDevice_GetId(device, &dev->dev_id);
    if(FAILED(hr))
        return hr;

    CoCreateGuid(&dev->session);

    InitializeCriticalSection(&dev->lock);

    return S_OK;
}

static HRESULT WINMM_EnumDevices(WINMM_MMDevice **devices, UINT *devcount,
        EDataFlow flow)
{
    IMMDeviceCollection *devcoll;
    HRESULT hr;

    hr = IMMDeviceEnumerator_EnumAudioEndpoints(g_devenum, flow,
            DEVICE_STATE_ACTIVE, &devcoll);
    if(FAILED(hr))
        return hr;

    hr = IMMDeviceCollection_GetCount(devcoll, devcount);
    if(FAILED(hr)){
        IMMDeviceCollection_Release(devcoll);
        return hr;
    }

    if(*devcount > 0){
        UINT n, count;
        IMMDevice *def_dev = NULL;

        *devices = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                sizeof(WINMM_MMDevice) * (*devcount));
        if(!*devices){
            IMMDeviceCollection_Release(devcoll);
            return E_OUTOFMEMORY;
        }

        count = 0;

        /* make sure that device 0 is the default device */
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_devenum,
                flow, eConsole, &def_dev);
        if(SUCCEEDED(hr)){
            WINMM_InitMMDevice(flow, def_dev, &(*devices)[0], 0);
            count = 1;
        }

        for(n = 0; n < *devcount; ++n){
            IMMDevice *device;

            hr = IMMDeviceCollection_Item(devcoll, n, &device);
            if(SUCCEEDED(hr)){
                if(device != def_dev){
                    WINMM_InitMMDevice(flow, device, &(*devices)[count], count);
                    ++count;
                }

                IMMDevice_Release(device);
            }
        }

        if(def_dev)
            IMMDevice_Release(def_dev);

        *devcount = count;
    }

    IMMDeviceCollection_Release(devcoll);

    return S_OK;
}

static HRESULT WINMM_InitMMDevices(void)
{
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL,
            CLSCTX_INPROC_SERVER, &IID_IMMDeviceEnumerator, (void**)&g_devenum);
    if(FAILED(hr))
        return hr;

    hr = WINMM_EnumDevices(&g_out_mmdevices, &g_outmmdevices_count, eRender);
    if(FAILED(hr)){
        g_outmmdevices_count = 0;
        g_inmmdevices_count = 0;
        return hr;
    }

    hr = WINMM_EnumDevices(&g_in_mmdevices, &g_inmmdevices_count, eCapture);
    if(FAILED(hr)){
        g_inmmdevices_count = 0;
        return hr;
    }

    return S_OK;
}

static inline BOOL WINMM_IsMapper(UINT device)
{
    return (device == WAVE_MAPPER || device == (UINT16)WAVE_MAPPER);
}

static MMRESULT WINMM_TryDeviceMapping(WINMM_OpenInfo *info, WORD channels,
        DWORD freq, DWORD bits_per_samp, BOOL is_out)
{
    WINMM_Device *device;
    WAVEFORMATEX target;
    MMRESULT mr;
    UINT i;

    TRACE("format: %u, channels: %u, sample rate: %u, bit depth: %u\n",
            WAVE_FORMAT_PCM, channels, freq, bits_per_samp);

    target.wFormatTag = WAVE_FORMAT_PCM;
    target.nChannels = channels;
    target.nSamplesPerSec = freq;
    target.wBitsPerSample = bits_per_samp;
    target.nBlockAlign = (target.nChannels * target.wBitsPerSample) / 8;
    target.nAvgBytesPerSec = target.nSamplesPerSec * target.nBlockAlign;
    target.cbSize = 0;

    if(is_out)
        mr = acmStreamOpen(NULL, NULL, info->format, &target, NULL, 0,
                0, ACM_STREAMOPENF_QUERY);
    else
        mr = acmStreamOpen(NULL, NULL, &target, info->format, NULL, 0,
                0, ACM_STREAMOPENF_QUERY);
    if(mr != MMSYSERR_NOERROR)
        return mr;

    /* ACM can convert from src->dst, so try to find a device
     * that supports dst */
    if(is_out){
        if(WINMM_IsMapper(info->req_device)){
            for(i = 0; i < g_outmmdevices_count; ++i){
                WINMM_OpenInfo l_info = *info;
                l_info.req_device = i;
                l_info.format = &target;
                mr = WOD_Open(&l_info);
                if(mr == MMSYSERR_NOERROR){
                    info->handle = l_info.handle;
                    break;
                }
            }
        }else{
            WINMM_OpenInfo l_info = *info;
            l_info.flags &= ~WAVE_MAPPED;
            l_info.format = &target;
            mr = WOD_Open(&l_info);
            if(mr == MMSYSERR_NOERROR)
                info->handle = l_info.handle;
        }
    }else{
        if(WINMM_IsMapper(info->req_device)){
            for(i = 0; i < g_inmmdevices_count; ++i){
                WINMM_OpenInfo l_info = *info;
                l_info.req_device = i;
                l_info.format = &target;
                mr = WID_Open(&l_info);
                if(mr == MMSYSERR_NOERROR){
                    info->handle = l_info.handle;
                    break;
                }
            }
        }else{
            WINMM_OpenInfo l_info = *info;
            l_info.flags &= ~WAVE_MAPPED;
            l_info.format = &target;
            mr = WID_Open(&l_info);
            if(mr == MMSYSERR_NOERROR)
                info->handle = l_info.handle;
        }
    }
    if(mr != MMSYSERR_NOERROR)
        return WAVERR_BADFORMAT;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)info->handle);
    if(!device)
        return MMSYSERR_INVALHANDLE;

    /* set up the ACM stream */
    if(is_out)
        mr = acmStreamOpen(&device->acm_handle, NULL, info->format, &target,
                NULL, 0, 0, 0);
    else
        mr = acmStreamOpen(&device->acm_handle, NULL, &target, info->format,
                NULL, 0, 0, 0);
    if(mr != MMSYSERR_NOERROR){
        if(is_out)
            WOD_Close((HWAVEOUT)info->handle);
        else
            WID_Close((HWAVEIN)info->handle);
        return mr;
    }

    TRACE("Success\n");
    return MMSYSERR_NOERROR;
}

static MMRESULT WINMM_MapDevice(WINMM_OpenInfo *info, BOOL is_out)
{
    UINT i;
    MMRESULT mr;
    WAVEFORMATEXTENSIBLE *fmtex = (WAVEFORMATEXTENSIBLE*)info->format;

    TRACE("(%p, %d)\n", info, is_out);

    /* try to find a direct match */
    if(is_out){
        WINMM_OpenInfo l_info = *info;
        if(WINMM_IsMapper(info->req_device)){
            for(i = 0; i < g_outmmdevices_count; ++i){
                l_info.req_device = i;
                mr = WOD_Open(&l_info);
                if(mr == MMSYSERR_NOERROR){
                    info->handle = l_info.handle;
                    return mr;
                }
            }
        }else{
            l_info.flags &= ~WAVE_MAPPED;
            mr = WOD_Open(&l_info);
            if(mr == MMSYSERR_NOERROR){
                info->handle = l_info.handle;
                return mr;
            }
        }
    }else{
        WINMM_OpenInfo l_info = *info;
        if(WINMM_IsMapper(info->req_device)){
            for(i = 0; i < g_inmmdevices_count; ++i){
                l_info.req_device = i;
                mr = WID_Open(&l_info);
                if(mr == MMSYSERR_NOERROR){
                    info->handle = l_info.handle;
                    return mr;
                }
            }
        }else{
            l_info.flags &= ~WAVE_MAPPED;
            mr = WID_Open(&l_info);
            if(mr == MMSYSERR_NOERROR){
                info->handle = l_info.handle;
                return mr;
            }
        }
    }

    /* no direct match, so set up the ACM stream */
    if(info->format->wFormatTag != WAVE_FORMAT_PCM ||
            (info->format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             !IsEqualGUID(&fmtex->SubFormat, &KSDATAFORMAT_SUBTYPE_PCM))){
        /* convert to PCM format if it's not already */
        mr = WINMM_TryDeviceMapping(info, info->format->nChannels,
                info->format->nSamplesPerSec, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;

        mr = WINMM_TryDeviceMapping(info, info->format->nChannels,
                info->format->nSamplesPerSec, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
    }else{
        WORD channels;

        /* first try just changing bit depth and channels */
        channels = info->format->nChannels;
        mr = WINMM_TryDeviceMapping(info, channels,
                info->format->nSamplesPerSec, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels,
                info->format->nSamplesPerSec, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;

        channels = (channels == 2) ? 1 : 2;
        mr = WINMM_TryDeviceMapping(info, channels,
                info->format->nSamplesPerSec, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels,
                info->format->nSamplesPerSec, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;

        /* that didn't work, so now try different sample rates */
        channels = info->format->nChannels;
        mr = WINMM_TryDeviceMapping(info, channels, 96000, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 48000, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 44100, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 22050, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 11025, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;

        channels = (channels == 2) ? 1 : 2;
        mr = WINMM_TryDeviceMapping(info, channels, 96000, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 48000, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 44100, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 22050, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 11025, 16, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;

        channels = info->format->nChannels;
        mr = WINMM_TryDeviceMapping(info, channels, 96000, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 48000, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 44100, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 22050, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 11025, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;

        channels = (channels == 2) ? 1 : 2;
        mr = WINMM_TryDeviceMapping(info, channels, 96000, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 48000, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 44100, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 22050, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
        mr = WINMM_TryDeviceMapping(info, channels, 11025, 8, is_out);
        if(mr == MMSYSERR_NOERROR)
            return mr;
    }

    WARN("Unable to find compatible device!\n");
    return WAVERR_BADFORMAT;
}

static LRESULT WINMM_OpenDevice(WINMM_Device *device, WINMM_MMDevice *mmdevice,
        WINMM_OpenInfo *info)
{
    WAVEFORMATEX *closer_fmt = NULL, *passed_fmt;
    LRESULT ret = MMSYSERR_ERROR;
    HRESULT hr;

    hr = IMMDeviceEnumerator_GetDevice(g_devenum, mmdevice->dev_id,
            &device->device);
    if(FAILED(hr)){
        ERR("Device %s (%s) unavailable: %08x\n",
                wine_dbgstr_w(mmdevice->dev_id),
                wine_dbgstr_w(mmdevice->out_caps.szPname), hr);
        goto error;
    }

    hr = IMMDevice_Activate(device->device, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&device->client);
    if(FAILED(hr)){
        ERR("Activate failed: %08x\n", hr);
        goto error;
    }

    if(info->format->wFormatTag == WAVE_FORMAT_PCM){
        /* we aren't guaranteed that the struct in lpFormat is a full
         * WAVEFORMATEX struct, which IAC::IsFormatSupported requires */
        passed_fmt = HeapAlloc(GetProcessHeap(), 0, sizeof(WAVEFORMATEX));
        memcpy(passed_fmt, info->format, sizeof(PCMWAVEFORMAT));
        passed_fmt->cbSize = 0;
    }else
        passed_fmt = info->format;

    hr = IAudioClient_IsFormatSupported(device->client,
            AUDCLNT_SHAREMODE_SHARED, passed_fmt, &closer_fmt);
    if(closer_fmt)
        CoTaskMemFree(closer_fmt);
    if(FAILED(hr) && hr != AUDCLNT_E_UNSUPPORTED_FORMAT){
        if(info->format->wFormatTag == WAVE_FORMAT_PCM)
            HeapFree(GetProcessHeap(), 0, passed_fmt);
        ERR("IsFormatSupported failed: %08x\n", hr);
        goto error;
    }
    if(hr == S_FALSE || hr == AUDCLNT_E_UNSUPPORTED_FORMAT){
        if(info->format->wFormatTag == WAVE_FORMAT_PCM)
            HeapFree(GetProcessHeap(), 0, passed_fmt);
        ret = WAVERR_BADFORMAT;
        goto error;
    }
    if(info->flags & WAVE_FORMAT_QUERY){
        if(info->format->wFormatTag == WAVE_FORMAT_PCM)
            HeapFree(GetProcessHeap(), 0, passed_fmt);
        ret = MMSYSERR_NOERROR;
        goto error;
    }

    /* buffer size = 10 * 100000 (100 ns) = 0.1 seconds */
    hr = IAudioClient_Initialize(device->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
            10 * 100000, 50000, passed_fmt, &device->parent->session);
    if(info->format->wFormatTag == WAVE_FORMAT_PCM)
        HeapFree(GetProcessHeap(), 0, passed_fmt);
    if(FAILED(hr)){
        ERR("Initialize failed: %08x\n", hr);
        goto error;
    }

    hr = IAudioClient_GetService(device->client, &IID_IAudioClock,
            (void**)&device->clock);
    if(FAILED(hr)){
        ERR("GetService failed: %08x\n", hr);
        goto error;
    }

    if(!device->event){
        device->event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if(!device->event){
            ERR("CreateEvent failed: %08x\n", hr);
            goto error;
        }

        if(g_device_handles){
            g_device_handles = HeapReAlloc(GetProcessHeap(), 0, g_device_handles,
                    sizeof(HANDLE) * (g_devhandle_count + 1));
            g_handle_devices = HeapReAlloc(GetProcessHeap(), 0, g_handle_devices,
                    sizeof(WINMM_Device *) * (g_devhandle_count + 1));
        }else{
            g_device_handles = HeapAlloc(GetProcessHeap(), 0, sizeof(HANDLE));
            g_handle_devices = HeapAlloc(GetProcessHeap(), 0,
                    sizeof(WINMM_Device *));
        }
        g_device_handles[g_devhandle_count] = device->event;
        g_handle_devices[g_devhandle_count] = device;
        ++g_devhandle_count;
    }

    hr = IAudioClient_SetEventHandle(device->client, device->event);
    if(FAILED(hr)){
        ERR("SetEventHandle failed: %08x\n", hr);
        goto error;
    }

    device->bytes_per_frame = info->format->nBlockAlign;
    device->samples_per_sec = info->format->nSamplesPerSec;

    device->played_frames = 0;
    device->last_clock_pos = 0;
    device->ofs_bytes = 0;
    device->loop_counter = 0;
    device->stopped = TRUE;
    device->first = device->last = device->playing = device->loop_start = NULL;

    device->cb_info.flags = HIWORD(info->flags & CALLBACK_TYPEMASK);
    device->cb_info.callback = info->callback;
    device->cb_info.user = info->cb_user;
    device->cb_info.hwave = (HWAVE)device->handle;

    info->handle = device->handle;

    return MMSYSERR_NOERROR;

error:
    if(device->client){
        IAudioClient_Release(device->client);
        device->client = NULL;
    }
    if(device->device){
        IMMDevice_Release(device->device);
        device->device = NULL;
    }

    return ret;
}

static LRESULT WOD_Open(WINMM_OpenInfo *info)
{
    WINMM_MMDevice *mmdevice;
    WINMM_Device *device = NULL;
    WINMM_CBInfo cb_info;
    LRESULT ret = MMSYSERR_ERROR;
    HRESULT hr;

    TRACE("(%u, %p, %08x)\n", info->req_device, info, info->flags);

    if(WINMM_IsMapper(info->req_device) || (info->flags & WAVE_MAPPED))
        return WINMM_MapDevice(info, TRUE);

    if(info->req_device >= g_outmmdevices_count)
        return MMSYSERR_BADDEVICEID;

    mmdevice = &g_out_mmdevices[info->req_device];

    if(!mmdevice->out_caps.szPname[0])
        return MMSYSERR_NOTENABLED;

    device = WINMM_FindUnusedDevice(TRUE, info->req_device);
    if(!device)
        return MMSYSERR_ALLOCATED;

    ret = WINMM_OpenDevice(device, mmdevice, info);
    if((info->flags & WAVE_FORMAT_QUERY) || ret != MMSYSERR_NOERROR)
        goto error;
    ret = MMSYSERR_ERROR;

    hr = IAudioClient_GetService(device->client, &IID_IAudioRenderClient,
            (void**)&device->render);
    if(FAILED(hr)){
        ERR("GetService failed: %08x\n", hr);
        goto error;
    }

    hr = IAudioClient_GetService(device->client, &IID_IAudioStreamVolume,
            (void**)&device->volume);
    if(FAILED(hr)){
        ERR("GetService failed: %08x\n", hr);
        goto error;
    }

    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    WINMM_NotifyClient(&cb_info, WOM_OPEN, 0, 0);

    return MMSYSERR_NOERROR;

error:
    if(device->device){
        IMMDevice_Release(device->device);
        device->device = NULL;
    }
    if(device->client){
        IAudioClient_Release(device->client);
        device->client = NULL;
    }
    if(device->render){
        IAudioRenderClient_Release(device->render);
        device->render = NULL;
    }
    if(device->volume){
        IAudioStreamVolume_Release(device->volume);
        device->volume = NULL;
    }
    if(device->clock){
        IAudioClock_Release(device->clock);
        device->clock = NULL;
    }
    device->open = FALSE;
    LeaveCriticalSection(&device->lock);
    return ret;
}

static LRESULT WID_Open(WINMM_OpenInfo *info)
{
    WINMM_MMDevice *mmdevice;
    WINMM_Device *device = NULL;
    WINMM_CBInfo cb_info;
    LRESULT ret = MMSYSERR_ERROR;
    HRESULT hr;

    TRACE("(%u, %p, %08x)\n", info->req_device, info, info->flags);

    if(WINMM_IsMapper(info->req_device) || info->flags & WAVE_MAPPED)
        return WINMM_MapDevice(info, FALSE);

    if(info->req_device >= g_inmmdevices_count)
        return MMSYSERR_BADDEVICEID;

    mmdevice = &g_in_mmdevices[info->req_device];

    if(!mmdevice->in_caps.szPname[0])
        return MMSYSERR_NOTENABLED;

    device = WINMM_FindUnusedDevice(FALSE, info->req_device);
    if(!device)
        return MMSYSERR_ALLOCATED;

    ret = WINMM_OpenDevice(device, mmdevice, info);
    if((info->flags & WAVE_FORMAT_QUERY) || ret != MMSYSERR_NOERROR)
        goto error;
    ret = MMSYSERR_ERROR;

    hr = IAudioClient_GetService(device->client, &IID_IAudioCaptureClient,
            (void**)&device->capture);
    if(FAILED(hr)){
        ERR("GetService failed: %08x\n", hr);
        goto error;
    }

    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    WINMM_NotifyClient(&cb_info, WIM_OPEN, 0, 0);

    return MMSYSERR_NOERROR;

error:
    if(device->device){
        IMMDevice_Release(device->device);
        device->device = NULL;
    }
    if(device->client){
        IAudioClient_Release(device->client);
        device->client = NULL;
    }
    if(device->capture){
        IAudioCaptureClient_Release(device->capture);
        device->capture = NULL;
    }
    if(device->clock){
        IAudioClock_Release(device->clock);
        device->clock = NULL;
    }
    device->open = FALSE;
    LeaveCriticalSection(&device->lock);
    return ret;
}

static HRESULT WINMM_CloseDevice(WINMM_Device *device)
{
    device->open = FALSE;

    if(!device->stopped){
        IAudioClient_Stop(device->client);
        device->stopped = TRUE;
    }

    IMMDevice_Release(device->device);
    device->device = NULL;

    IAudioClient_Release(device->client);
    device->client = NULL;

    IAudioClock_Release(device->clock);
    device->clock = NULL;

    return S_OK;
}

static LRESULT WOD_Close(HWAVEOUT hwave)
{
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE((HWAVE)hwave);
    WINMM_CBInfo cb_info;

    TRACE("(%p)\n", hwave);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    WINMM_CloseDevice(device);

    IAudioRenderClient_Release(device->render);
    device->render = NULL;

    IAudioStreamVolume_Release(device->volume);
    device->volume = NULL;

    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    WINMM_NotifyClient(&cb_info, WOM_CLOSE, 0, 0);

    return MMSYSERR_NOERROR;
}

static LRESULT WID_Close(HWAVEIN hwave)
{
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE((HWAVE)hwave);
    WINMM_CBInfo cb_info;

    TRACE("(%p)\n", hwave);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    WINMM_CloseDevice(device);

    IAudioCaptureClient_Release(device->capture);
    device->capture = NULL;

    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    WINMM_NotifyClient(&cb_info, WIM_CLOSE, 0, 0);

    return MMSYSERR_NOERROR;
}

static LRESULT WINMM_PrepareHeader(HWAVE hwave, WAVEHDR *header)
{
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE(hwave);

    TRACE("(%p, %p)\n", hwave, header);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    if(device->render && device->acm_handle){
        ACMSTREAMHEADER *ash;
        DWORD size;
        MMRESULT mr;

        mr = acmStreamSize(device->acm_handle, header->dwBufferLength, &size,
                ACM_STREAMSIZEF_SOURCE);
        if(mr != MMSYSERR_NOERROR){
            LeaveCriticalSection(&device->lock);
            return mr;
        }

        ash = HeapAlloc(GetProcessHeap(), 0, sizeof(ACMSTREAMHEADER) + size);
        if(!ash){
            LeaveCriticalSection(&device->lock);
            return MMSYSERR_NOMEM;
        }

        ash->cbStruct = sizeof(*ash);
        ash->fdwStatus = 0;
        ash->dwUser = (DWORD_PTR)header;
        ash->pbSrc = (BYTE*)header->lpData;
        ash->cbSrcLength = header->dwBufferLength;
        ash->dwSrcUser = header->dwUser;
        ash->pbDst = (BYTE*)ash + sizeof(ACMSTREAMHEADER);
        ash->cbDstLength = size;
        ash->dwDstUser = 0;

        mr = acmStreamPrepareHeader(device->acm_handle, ash, 0);
        if(mr != MMSYSERR_NOERROR){
            LeaveCriticalSection(&device->lock);
            return mr;
        }

        header->reserved = (DWORD_PTR)ash;
    }

    LeaveCriticalSection(&device->lock);

    header->dwFlags |= WHDR_PREPARED;
    header->dwFlags &= ~WHDR_DONE;

    return MMSYSERR_NOERROR;
}

static LRESULT WINMM_UnprepareHeader(HWAVE hwave, WAVEHDR *header)
{
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE(hwave);

    TRACE("(%p, %p)\n", hwave, header);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    if(device->render && device->acm_handle){
        ACMSTREAMHEADER *ash = (ACMSTREAMHEADER*)header->reserved;

        acmStreamUnprepareHeader(device->acm_handle, ash, 0);

        HeapFree(GetProcessHeap(), 0, ash);
    }

    LeaveCriticalSection(&device->lock);

    header->dwFlags &= ~WHDR_PREPARED;
    header->dwFlags |= WHDR_DONE;

    return MMSYSERR_NOERROR;
}

static UINT32 WINMM_HeaderLenBytes(WINMM_Device *device, WAVEHDR *header)
{
    if(device->acm_handle){
        ACMSTREAMHEADER *ash = (ACMSTREAMHEADER*)header->reserved;
        return ash->cbDstLengthUsed;
    }

    return header->dwBufferLength;
}

static UINT32 WINMM_HeaderLenFrames(WINMM_Device *device, WAVEHDR *header)
{
    return WINMM_HeaderLenBytes(device, header) / device->bytes_per_frame;
}

static WAVEHDR *WOD_MarkDoneHeaders(WINMM_Device *device)
{
    HRESULT hr;
    WAVEHDR *queue, *first = device->first;
    UINT64 clock_freq, clock_pos, clock_frames;
    UINT32 nloops, queue_frames;

    hr = IAudioClock_GetFrequency(device->clock, &clock_freq);
    if(FAILED(hr)){
        ERR("GetFrequency failed: %08x\n", hr);
        return first;
    }

    hr = IAudioClock_GetPosition(device->clock, &clock_pos, NULL);
    if(FAILED(hr)){
        ERR("GetPosition failed: %08x\n", hr);
        return first;
    }

    clock_frames = (clock_pos / (double)clock_freq) * device->samples_per_sec;

    first = queue = device->first;
    nloops = device->loop_counter;
    while(queue &&
            (queue_frames = WINMM_HeaderLenFrames(device, queue)) <=
                clock_frames - device->last_clock_pos){
        WAVEHDR *next = queue->lpNext;
        if(!nloops){
            device->first->dwFlags &= ~WHDR_INQUEUE;
            device->first->dwFlags |= WHDR_DONE;
            device->last_clock_pos += queue_frames;
            queue = device->first = next;
        }else{
            if(queue->dwFlags & WHDR_BEGINLOOP)
                queue = next;

            if(queue->dwFlags & WHDR_ENDLOOP){
                queue = device->loop_start;
                --nloops;
            }
        }
    }

    return first;
}

static void WOD_PushData(WINMM_Device *device)
{
    WINMM_CBInfo cb_info;
    HRESULT hr;
    UINT32 pad, bufsize, avail_frames, queue_frames, written, ofs;
    UINT32 queue_bytes, nloops;
    BYTE *data;
    WAVEHDR *queue, *first = NULL;

    TRACE("(%p)\n", device->handle);

    EnterCriticalSection(&device->lock);

    if(!device->device)
        goto exit;

    if(!device->first){
        device->stopped = TRUE;
        device->last_clock_pos = 0;
        IAudioClient_Stop(device->client);
        IAudioClient_Reset(device->client);
        goto exit;
    }

    hr = IAudioClient_GetBufferSize(device->client, &bufsize);
    if(FAILED(hr)){
        ERR("GetBufferSize failed: %08x\n", hr);
        goto exit;
    }

    hr = IAudioClient_GetCurrentPadding(device->client, &pad);
    if(FAILED(hr)){
        ERR("GetCurrentPadding failed: %08x\n", hr);
        goto exit;
    }

    first = WOD_MarkDoneHeaders(device);

    /* determine which is larger between the available buffer size and
     * the amount of data left in the queue */
    avail_frames = bufsize - pad;

    queue = device->playing;
    ofs = device->ofs_bytes;
    queue_frames = 0;
    nloops = 0;
    while(queue && queue_frames < avail_frames){
        queue_bytes = WINMM_HeaderLenBytes(device, queue);
        queue_frames = (queue_bytes - ofs) / device->bytes_per_frame;

        ofs = 0;

        if(queue->dwFlags & WHDR_ENDLOOP && nloops < device->loop_counter){
            queue = device->loop_start;
            ++nloops;
        }else
            queue = queue->lpNext;
    }

    if(avail_frames != 0 && queue_frames == 0){
        hr = IAudioRenderClient_GetBuffer(device->render, avail_frames, &data);
        if(FAILED(hr)){
            ERR("GetBuffer failed: %08x\n", hr);
            goto exit;
        }

        hr = IAudioRenderClient_ReleaseBuffer(device->render, avail_frames,
                AUDCLNT_BUFFERFLAGS_SILENT);
        if(FAILED(hr)){
            ERR("ReleaseBuffer failed: %08x\n", hr);
            goto exit;
        }

        goto exit;
    }

    if(queue_frames < avail_frames)
        avail_frames = queue_frames;
    if(avail_frames == 0)
        goto exit;

    hr = IAudioRenderClient_GetBuffer(device->render, avail_frames, &data);
    if(FAILED(hr)){
        ERR("GetBuffer failed: %08x\n", hr);
        goto exit;
    }

    written = 0;
    while(device->playing && written < avail_frames){
        UINT32 copy_frames, copy_bytes;
        BYTE *queue_data;

        queue = device->playing;

        if(device->acm_handle){
            ACMSTREAMHEADER *ash = (ACMSTREAMHEADER*)queue->reserved;
            queue_bytes = ash->cbDstLengthUsed;
            queue_data = ash->pbDst;
        }else{
            queue_bytes = queue->dwBufferLength;
            queue_data = (BYTE*)queue->lpData;
        }

        queue_frames = (queue_bytes - device->ofs_bytes) /
            device->bytes_per_frame;

        copy_frames = queue_frames < (avail_frames - written) ?
            queue_frames : avail_frames - written;
        copy_bytes = copy_frames * device->bytes_per_frame;

        memcpy(data, queue_data + device->ofs_bytes, copy_bytes);

        data += copy_bytes;
        written += copy_frames;
        device->ofs_bytes += copy_bytes;

        if(device->ofs_bytes >= queue_bytes){
            device->ofs_bytes = 0;

            if(!(queue->dwFlags & (WHDR_BEGINLOOP | WHDR_ENDLOOP)))
                device->playing = queue->lpNext;
            else{
                if(queue->dwFlags & WHDR_BEGINLOOP){
                    device->loop_start = device->playing;
                    device->playing = queue->lpNext;
                    device->loop_counter = queue->dwLoops;
                }
                if(queue->dwFlags & WHDR_ENDLOOP){
                    --device->loop_counter;
                    if(device->loop_counter)
                        device->playing = device->loop_start;
                    else
                        device->loop_start = device->playing = queue->lpNext;
                }
            }
        }
    }

    hr = IAudioRenderClient_ReleaseBuffer(device->render, avail_frames, 0);
    if(FAILED(hr)){
        ERR("ReleaseBuffer failed: %08x\n", hr);
        goto exit;
    }

    device->played_frames += avail_frames;

exit:
    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    while(first && (first->dwFlags & WHDR_DONE)){
        WAVEHDR *next = first->lpNext;
        WINMM_NotifyClient(&cb_info, WOM_DONE, (DWORD_PTR)first, 0);
        first = next;
    }
}

static void WID_PullACMData(WINMM_Device *device)
{
    UINT32 packet, packet_bytes;
    DWORD flags;
    BYTE *data;
    WAVEHDR *queue;
    HRESULT hr;
    MMRESULT mr;

    if(device->acm_hdr.cbDstLength == 0){
        hr = IAudioClient_GetCurrentPadding(device->client, &packet);
        if(FAILED(hr)){
            ERR("GetCurrentPadding failed: %08x\n", hr);
            return;
        }

        if(packet == 0)
            return;

        hr = IAudioCaptureClient_GetBuffer(device->capture, &data, &packet,
                &flags, NULL, NULL);
        if(FAILED(hr)){
            ERR("GetBuffer failed: %08x\n", hr);
            return;
        }

        acmStreamSize(device->acm_handle, packet * device->bytes_per_frame,
                &packet_bytes, ACM_STREAMSIZEF_SOURCE);

        device->acm_offs = 0;

        device->acm_hdr.cbStruct = sizeof(device->acm_hdr);
        device->acm_hdr.fdwStatus = 0;
        device->acm_hdr.dwUser = 0;
        device->acm_hdr.pbSrc = data;
        device->acm_hdr.cbSrcLength = packet * device->bytes_per_frame;
        device->acm_hdr.cbSrcLengthUsed = 0;
        device->acm_hdr.dwSrcUser = 0;
        device->acm_hdr.pbDst = HeapAlloc(GetProcessHeap(), 0, packet_bytes);
        device->acm_hdr.cbDstLength = packet_bytes;
        device->acm_hdr.cbDstLengthUsed = 0;
        device->acm_hdr.dwDstUser = 0;

        mr = acmStreamPrepareHeader(device->acm_handle, &device->acm_hdr, 0);
        if(mr != MMSYSERR_NOERROR){
            ERR("acmStreamPrepareHeader failed: %d\n", mr);
            return;
        }

        mr = acmStreamConvert(device->acm_handle, &device->acm_hdr, 0);
        if(mr != MMSYSERR_NOERROR){
            ERR("acmStreamConvert failed: %d\n", mr);
            return;
        }

        hr = IAudioCaptureClient_ReleaseBuffer(device->capture, packet);
        if(FAILED(hr))
            ERR("ReleaseBuffer failed: %08x\n", hr);
    }

    queue = device->first;
    while(queue){
        UINT32 to_copy_bytes;

        to_copy_bytes = min(queue->dwBufferLength - queue->dwBytesRecorded,
                device->acm_hdr.cbDstLengthUsed - device->acm_offs);

        memcpy(queue->lpData + queue->dwBytesRecorded,
                device->acm_hdr.pbDst + device->acm_offs, to_copy_bytes);

        queue->dwBytesRecorded += to_copy_bytes;
        device->acm_offs += to_copy_bytes;

        if(queue->dwBufferLength - queue->dwBytesRecorded <
                device->bytes_per_frame){
            queue->dwFlags &= ~WHDR_INQUEUE;
            queue->dwFlags |= WHDR_DONE;
            device->first = queue = queue->lpNext;
        }

        if(device->acm_offs >= device->acm_hdr.cbDstLengthUsed){
            acmStreamUnprepareHeader(device->acm_handle, &device->acm_hdr, 0);
            HeapFree(GetProcessHeap(), 0, device->acm_hdr.pbDst);
            device->acm_hdr.cbDstLength = 0;
            device->acm_hdr.cbDstLengthUsed = 0;

            /* done with this ACM Header, so try to pull more data */
            WID_PullACMData(device);
            return;
        }
    }

    /* out of WAVEHDRs to write into, so toss the rest of this packet */
    acmStreamUnprepareHeader(device->acm_handle, &device->acm_hdr, 0);
    HeapFree(GetProcessHeap(), 0, device->acm_hdr.pbDst);
    device->acm_hdr.cbDstLength = 0;
    device->acm_hdr.cbDstLengthUsed = 0;
}

static void WID_PullData(WINMM_Device *device)
{
    WINMM_CBInfo cb_info;
    WAVEHDR *queue, *first = NULL;
    HRESULT hr;

    TRACE("(%p)\n", device->handle);

    EnterCriticalSection(&device->lock);

    if(!device->device || !device->first)
        goto exit;

    first = device->first;

    if(device->acm_handle){
        WID_PullACMData(device);
        goto exit;
    }

    while(device->first){
        BYTE *data;
        UINT32 pad, packet_len, packet;
        DWORD flags;

        hr = IAudioClient_GetCurrentPadding(device->client, &pad);
        if(FAILED(hr)){
            ERR("GetCurrentPadding failed: %08x\n", hr);
            goto exit;
        }

        if(pad == 0)
            goto exit;

        hr = IAudioCaptureClient_GetBuffer(device->capture, &data, &packet,
                &flags, NULL, NULL);
        if(FAILED(hr)){
            ERR("GetBuffer failed: %08x\n", hr);
            goto exit;
        }

        packet_len = packet;
        queue = device->first;
        while(queue && packet > 0){
            UINT32 to_copy_bytes;

            to_copy_bytes = min(packet * device->bytes_per_frame,
                    queue->dwBufferLength - queue->dwBytesRecorded);

            memcpy(queue->lpData + queue->dwBytesRecorded, data,
                    to_copy_bytes);

            queue->dwBytesRecorded += to_copy_bytes;

            if(queue->dwBufferLength - queue->dwBytesRecorded <
                    device->bytes_per_frame){
                queue->dwFlags &= ~WHDR_INQUEUE;
                queue->dwFlags |= WHDR_DONE;
                device->first = queue = queue->lpNext;
            }

            packet -= to_copy_bytes / device->bytes_per_frame;
        }

        hr = IAudioCaptureClient_ReleaseBuffer(device->capture, packet_len);
        if(FAILED(hr))
            ERR("ReleaseBuffer failed: %08x\n", hr);
    }

exit:
    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    while(first && (first->dwFlags & WHDR_DONE)){
        WAVEHDR *next = first->lpNext;
        WINMM_NotifyClient(&cb_info, WIM_DATA, (DWORD_PTR)first, 0);
        first = next;
    }
}

static HRESULT WINMM_BeginPlaying(WINMM_Device *device)
{
    HRESULT hr;

    TRACE("(%p)\n", device->handle);

    EnterCriticalSection(&device->lock);

    if(device->render)
        /* prebuffer data before starting */
        WOD_PushData(device);

    if(device->stopped){
        device->stopped = FALSE;

        hr = IAudioClient_Start(device->client);
        if(FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED){
            device->stopped = TRUE;
            LeaveCriticalSection(&device->lock);
            ERR("Start failed: %08x\n", hr);
            return hr;
        }
    }

    LeaveCriticalSection(&device->lock);

    return S_OK;
}

static LRESULT WINMM_Pause(HWAVE hwave)
{
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE((HWAVE)hwave);
    HRESULT hr;

    TRACE("(%p)\n", hwave);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    hr = IAudioClient_Stop(device->client);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        ERR("Stop failed: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    device->stopped = FALSE;

    LeaveCriticalSection(&device->lock);

    return MMSYSERR_NOERROR;
}

static LRESULT WINMM_Reset(HWAVE hwave)
{
    WINMM_CBInfo cb_info;
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE((HWAVE)hwave);
    WAVEHDR *first;
    MMRESULT mr;

    TRACE("(%p)\n", hwave);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    mr = WINMM_Pause(hwave);
    if(mr != MMSYSERR_NOERROR){
        LeaveCriticalSection(&device->lock);
        return mr;
    }
    device->stopped = TRUE;

    if(device->render)
        first = WOD_MarkDoneHeaders(device);
    else
        first = device->first;
    device->first = device->last = device->playing = NULL;
    device->ofs_bytes = 0;
    device->played_frames = 0;
    device->loop_counter = 0;
    device->last_clock_pos = 0;

    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    while(first){
        WAVEHDR *next = first->lpNext;
        first->dwFlags &= ~WHDR_INQUEUE;
        first->dwFlags |= WHDR_DONE;
        if(device->render)
            WINMM_NotifyClient(&cb_info, WOM_DONE, (DWORD_PTR)first, 0);
        else
            WINMM_NotifyClient(&cb_info, WIM_DATA, (DWORD_PTR)first, 0);
        first = next;
    }

    return MMSYSERR_NOERROR;
}

static MMRESULT WINMM_FramesToMMTime(MMTIME *time, UINT32 played_frames,
        UINT32 sample_rate, UINT32 bytes_per_frame)
{
    switch(time->wType){
    case TIME_SAMPLES:
        time->u.sample = played_frames;
        return MMSYSERR_NOERROR;
    case TIME_MS:
        time->u.ms = (DWORD)((played_frames / (double)sample_rate) * 1000);
        return MMSYSERR_NOERROR;
    case TIME_SMPTE:
        time->u.smpte.fps = 30;
        if(played_frames >= sample_rate){
            time->u.smpte.sec = played_frames / (double)sample_rate;
            time->u.smpte.min = time->u.smpte.sec / 60;
            time->u.smpte.hour = time->u.smpte.min / 60;
            time->u.smpte.sec %= 60;
            time->u.smpte.min %= 60;
            played_frames %= sample_rate;
        }else{
            time->u.smpte.sec = 0;
            time->u.smpte.min = 0;
            time->u.smpte.hour = 0;
        }
        time->u.smpte.frame = (played_frames / (double)sample_rate) * 30;
        return MMSYSERR_NOERROR;
    case TIME_BYTES:
    default:
        time->wType = TIME_BYTES;
        time->u.cb = played_frames * bytes_per_frame;
        return MMSYSERR_NOERROR;
    }

    return MMSYSERR_ERROR;
}

static LRESULT WINMM_GetPosition(HWAVE hwave, MMTIME *time)
{
    WINMM_Device *device = WINMM_GetDeviceFromHWAVE((HWAVE)hwave);
    UINT32 played_frames, sample_rate, bytes_per_frame;

    TRACE("(%p, %p)\n", hwave, time);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    played_frames = device->played_frames;
    sample_rate = device->samples_per_sec;
    bytes_per_frame = device->bytes_per_frame;

    LeaveCriticalSection(&device->lock);

    return WINMM_FramesToMMTime(time, played_frames, sample_rate,
            bytes_per_frame);
}

static LRESULT CALLBACK WINMM_DevicesMsgProc(HWND hwnd, UINT msg, WPARAM wparam,
        LPARAM lparam)
{
    switch(msg){
    case WODM_OPEN:
        return WOD_Open((WINMM_OpenInfo*)wparam);
    case WODM_CLOSE:
        return WOD_Close((HWAVEOUT)wparam);
    case WIDM_OPEN:
        return WID_Open((WINMM_OpenInfo*)wparam);
    case WIDM_CLOSE:
        return WID_Close((HWAVEIN)wparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI WINMM_DevicesThreadProc(void *arg)
{
    HANDLE evt = arg;
    HRESULT hr;
    static const WCHAR messageW[] = {'M','e','s','s','a','g','e',0};

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)){
        ERR("CoInitializeEx failed: %08x\n", hr);
        return 1;
    }

    hr = WINMM_InitMMDevices();
    if(FAILED(hr)){
        CoUninitialize();
        return 1;
    }

    g_devices_hwnd = CreateWindowW(messageW, NULL, 0, 0, 0, 0, 0,
            HWND_MESSAGE, NULL, NULL, NULL);
    if(!g_devices_hwnd){
        ERR("CreateWindow failed: %d\n", GetLastError());
        CoUninitialize();
        return 1;
    }

    SetWindowLongPtrW(g_devices_hwnd, GWLP_WNDPROC,
            (LONG_PTR)WINMM_DevicesMsgProc);

    /* inform caller that the thread is ready to process messages */
    SetEvent(evt);
    evt = NULL; /* do not use after this point */

    while(1){
        DWORD wait;
        wait = MsgWaitForMultipleObjects(g_devhandle_count, g_device_handles,
                FALSE, INFINITE, QS_ALLINPUT);
        if(wait == g_devhandle_count - WAIT_OBJECT_0){
            MSG msg;
            if(PeekMessageW(&msg, g_devices_hwnd, 0, 0, PM_REMOVE))
                ERR("Unexpected message: 0x%x\n", msg.message);
        }else if(wait < g_devhandle_count){
            WINMM_Device *device = g_handle_devices[wait - WAIT_OBJECT_0];
            if(device->render)
                WOD_PushData(device);
            else
                WID_PullData(device);
        }else
            ERR("Unexpected MsgWait result 0x%x, GLE: %d\n", wait,
                    GetLastError());
    }

    DestroyWindow(g_devices_hwnd);

    CoUninitialize();

    return 0;
}

static BOOL WINMM_StartDevicesThread(void)
{
    HANDLE events[2];
    DWORD wait;

    EnterCriticalSection(&g_devthread_lock);

    if(g_devices_thread){
        DWORD wait;

        wait = WaitForSingleObject(g_devices_thread, 0);
        if(wait == WAIT_TIMEOUT){
            LeaveCriticalSection(&g_devthread_lock);
            return TRUE;
        }
        if(wait != WAIT_OBJECT_0){
            LeaveCriticalSection(&g_devthread_lock);
            return FALSE;
        }

        g_devices_thread = NULL;
        g_devices_hwnd = NULL;
    }

    TRACE("Starting up devices thread\n");

    events[0] = CreateEventW(NULL, FALSE, FALSE, NULL);

    g_devices_thread = CreateThread(NULL, 0, WINMM_DevicesThreadProc,
            events[0], 0, NULL);
    if(!g_devices_thread){
        LeaveCriticalSection(&g_devthread_lock);
        CloseHandle(events[0]);
        return FALSE;
    }

    events[1] = g_devices_thread;
    wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    CloseHandle(events[0]);
    if(wait != WAIT_OBJECT_0){
        if(wait == 1 - WAIT_OBJECT_0){
            CloseHandle(g_devices_thread);
            g_devices_thread = NULL;
            g_devices_hwnd = NULL;
        }
        LeaveCriticalSection(&g_devthread_lock);
        return FALSE;
    }

    LeaveCriticalSection(&g_devthread_lock);

    return TRUE;
}

/**************************************************************************
 * 				waveOutGetNumDevs		[WINMM.@]
 */
UINT WINAPI waveOutGetNumDevs(void)
{
    if(!WINMM_StartDevicesThread())
        return 0;

    TRACE("count: %u\n", g_outmmdevices_count);

    return g_outmmdevices_count;
}

/**************************************************************************
 * 				waveOutGetDevCapsA		[WINMM.@]
 */
UINT WINAPI waveOutGetDevCapsA(UINT_PTR uDeviceID, LPWAVEOUTCAPSA lpCaps,
			       UINT uSize)
{
    WAVEOUTCAPSW	wocW;
    UINT 		ret;

    TRACE("(%lu, %p, %u)\n", uDeviceID, lpCaps, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpCaps)
        return MMSYSERR_INVALPARAM;

    ret = waveOutGetDevCapsW(uDeviceID, &wocW, sizeof(wocW));

    if (ret == MMSYSERR_NOERROR) {
	WAVEOUTCAPSA wocA;
	wocA.wMid           = wocW.wMid;
	wocA.wPid           = wocW.wPid;
	wocA.vDriverVersion = wocW.vDriverVersion;
        WideCharToMultiByte( CP_ACP, 0, wocW.szPname, -1, wocA.szPname,
                             sizeof(wocA.szPname), NULL, NULL );
	wocA.dwFormats      = wocW.dwFormats;
	wocA.wChannels      = wocW.wChannels;
	wocA.dwSupport      = wocW.dwSupport;
	memcpy(lpCaps, &wocA, min(uSize, sizeof(wocA)));
    }
    return ret;
}

/**************************************************************************
 * 				waveOutGetDevCapsW		[WINMM.@]
 */
UINT WINAPI waveOutGetDevCapsW(UINT_PTR uDeviceID, LPWAVEOUTCAPSW lpCaps,
			       UINT uSize)
{
    WAVEOUTCAPSW mapper_caps, *caps;

    TRACE("(%lu, %p, %u)\n", uDeviceID, lpCaps, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if (lpCaps == NULL)	return MMSYSERR_INVALPARAM;

    if(WINMM_IsMapper(uDeviceID)){
        /* FIXME: Should be localized */
        static const WCHAR mapper_pnameW[] = {'W','i','n','e',' ','S','o','u',
            'n','d',' ','M','a','p','p','e','r',0};

        mapper_caps.wMid = 0xFF;
        mapper_caps.wPid = 0xFF;
        mapper_caps.vDriverVersion = 0x00010001;
        mapper_caps.dwFormats = 0xFFFFFFFF;
        mapper_caps.wReserved1 = 0;
        mapper_caps.dwSupport = WAVECAPS_LRVOLUME | WAVECAPS_VOLUME |
            WAVECAPS_SAMPLEACCURATE;
        mapper_caps.wChannels = 2;
        lstrcpyW(mapper_caps.szPname, mapper_pnameW);

        caps = &mapper_caps;
    }else{
        if(uDeviceID >= g_outmmdevices_count)
            return MMSYSERR_BADDEVICEID;

        caps = &g_out_mmdevices[uDeviceID].out_caps;
    }

    memcpy(lpCaps, caps, min(uSize, sizeof(*lpCaps)));

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutGetErrorTextA 	[WINMM.@]
 * 				waveInGetErrorTextA 	[WINMM.@]
 */
UINT WINAPI waveOutGetErrorTextA(UINT uError, LPSTR lpText, UINT uSize)
{
    UINT	ret;

    if (lpText == NULL) ret = MMSYSERR_INVALPARAM;
    else if (uSize == 0) ret = MMSYSERR_NOERROR;
    else
    {
        LPWSTR	xstr = HeapAlloc(GetProcessHeap(), 0, uSize * sizeof(WCHAR));
        if (!xstr) ret = MMSYSERR_NOMEM;
        else
        {
            ret = waveOutGetErrorTextW(uError, xstr, uSize);
            if (ret == MMSYSERR_NOERROR)
                WideCharToMultiByte(CP_ACP, 0, xstr, -1, lpText, uSize, NULL, NULL);
            HeapFree(GetProcessHeap(), 0, xstr);
        }
    }
    return ret;
}

/**************************************************************************
 * 				waveOutGetErrorTextW 	[WINMM.@]
 * 				waveInGetErrorTextW 	[WINMM.@]
 */
UINT WINAPI waveOutGetErrorTextW(UINT uError, LPWSTR lpText, UINT uSize)
{
    UINT        ret = MMSYSERR_BADERRNUM;

    if (lpText == NULL) ret = MMSYSERR_INVALPARAM;
    else if (uSize == 0) ret = MMSYSERR_NOERROR;
    else if (
	       /* test has been removed because MMSYSERR_BASE is 0, and gcc did emit
		* a warning for the test was always true */
	       (/*uError >= MMSYSERR_BASE && */ uError <= MMSYSERR_LASTERROR) ||
	       (uError >= WAVERR_BASE  && uError <= WAVERR_LASTERROR)) {
	if (LoadStringW(hWinMM32Instance,
			uError, lpText, uSize) > 0) {
	    ret = MMSYSERR_NOERROR;
	}
    }
    return ret;
}

/**************************************************************************
 *			waveOutOpen			[WINMM.@]
 * All the args/structs have the same layout as the win16 equivalents
 */
MMRESULT WINAPI waveOutOpen(LPHWAVEOUT lphWaveOut, UINT uDeviceID,
                       LPCWAVEFORMATEX lpFormat, DWORD_PTR dwCallback,
                       DWORD_PTR dwInstance, DWORD dwFlags)
{
    LRESULT res;
    HRESULT hr;
    WINMM_OpenInfo info;

    TRACE("(%p, %u, %p, %lx, %lx, %08x)\n", lphWaveOut, uDeviceID, lpFormat,
            dwCallback, dwInstance, dwFlags);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lphWaveOut && !(dwFlags & WAVE_FORMAT_QUERY))
        return MMSYSERR_INVALPARAM;

    hr = WINMM_StartDevicesThread();
    if(FAILED(hr)){
        ERR("Couldn't start the device thread: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    info.format = (WAVEFORMATEX*)lpFormat;
    info.callback = dwCallback;
    info.cb_user = dwInstance;
    info.req_device = uDeviceID;
    info.flags = dwFlags;

    res = SendMessageW(g_devices_hwnd, WODM_OPEN, (DWORD_PTR)&info, 0);
    if(res != MMSYSERR_NOERROR)
        return res;

    if(lphWaveOut)
        *lphWaveOut = (HWAVEOUT)info.handle;

    return res;
}

/**************************************************************************
 * 				waveOutClose		[WINMM.@]
 */
UINT WINAPI waveOutClose(HWAVEOUT hWaveOut)
{
    TRACE("(%p)\n", hWaveOut);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    return SendMessageW(g_devices_hwnd, WODM_CLOSE, (WPARAM)hWaveOut, 0);
}

/**************************************************************************
 * 				waveOutPrepareHeader	[WINMM.@]
 */
UINT WINAPI waveOutPrepareHeader(HWAVEOUT hWaveOut,
				 WAVEHDR* lpWaveOutHdr, UINT uSize)
{
    TRACE("(%p, %p, %u)\n", hWaveOut, lpWaveOutHdr, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpWaveOutHdr || uSize < sizeof(WAVEHDR))
        return MMSYSERR_INVALPARAM;

    if(lpWaveOutHdr->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;

    return WINMM_PrepareHeader((HWAVE)hWaveOut, lpWaveOutHdr);
}

/**************************************************************************
 * 				waveOutUnprepareHeader	[WINMM.@]
 */
UINT WINAPI waveOutUnprepareHeader(HWAVEOUT hWaveOut,
				   LPWAVEHDR lpWaveOutHdr, UINT uSize)
{
    TRACE("(%p, %p, %u)\n", hWaveOut, lpWaveOutHdr, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpWaveOutHdr || uSize < sizeof(WAVEHDR))
        return MMSYSERR_INVALPARAM;
    
    if(!(lpWaveOutHdr->dwFlags & WHDR_PREPARED))
        return MMSYSERR_NOERROR;

    if(lpWaveOutHdr->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;

    return WINMM_UnprepareHeader((HWAVE)hWaveOut, lpWaveOutHdr);
}

/**************************************************************************
 * 				waveOutWrite		[WINMM.@]
 */
UINT WINAPI waveOutWrite(HWAVEOUT hWaveOut, WAVEHDR *header, UINT uSize)
{
    WINMM_Device *device;
    HRESULT hr;

    TRACE("(%p, %p, %u)\n", hWaveOut, header, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveOut);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    if(!header->lpData || !(header->dwFlags & WHDR_PREPARED)){
        LeaveCriticalSection(&device->lock);
        return WAVERR_UNPREPARED;
    }

    if(header->dwFlags & WHDR_INQUEUE){
        LeaveCriticalSection(&device->lock);
        return WAVERR_STILLPLAYING;
    }

    if(device->acm_handle){
        ACMSTREAMHEADER *ash = (ACMSTREAMHEADER*)header->reserved;
        MMRESULT mr;

        ash->cbSrcLength = header->dwBufferLength;
        mr = acmStreamConvert(device->acm_handle, ash, 0);
        if(mr != MMSYSERR_NOERROR){
            LeaveCriticalSection(&device->lock);
            return mr;
        }
    }

    if(device->first){
        device->last->lpNext = header;
        device->last = header;
        if(!device->playing)
            device->playing = header;
    }else
        device->playing = device->first = device->last = header;

    header->lpNext = NULL;
    header->dwFlags &= ~WHDR_DONE;
    header->dwFlags |= WHDR_INQUEUE;

    hr = WINMM_BeginPlaying(device);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        return MMSYSERR_ERROR;
    }

    LeaveCriticalSection(&device->lock);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutBreakLoop	[WINMM.@]
 */
UINT WINAPI waveOutBreakLoop(HWAVEOUT hWaveOut)
{
    WINMM_Device *device;

    TRACE("(%p)\n", hWaveOut);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveOut);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    device->loop_counter = 0;

    LeaveCriticalSection(&device->lock);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutPause		[WINMM.@]
 */
UINT WINAPI waveOutPause(HWAVEOUT hWaveOut)
{
    TRACE("(%p)\n", hWaveOut);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    return WINMM_Pause((HWAVE)hWaveOut);
}

/**************************************************************************
 * 				waveOutReset		[WINMM.@]
 */
UINT WINAPI waveOutReset(HWAVEOUT hWaveOut)
{
    TRACE("(%p)\n", hWaveOut);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    return WINMM_Reset((HWAVE)hWaveOut);
}

/**************************************************************************
 * 				waveOutRestart		[WINMM.@]
 */
UINT WINAPI waveOutRestart(HWAVEOUT hWaveOut)
{
    WINMM_Device *device;
    HRESULT hr;

    TRACE("(%p)\n", hWaveOut);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveOut);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    device->stopped = TRUE;

    hr = WINMM_BeginPlaying(device);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        return MMSYSERR_ERROR;
    }

    LeaveCriticalSection(&device->lock);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutGetPosition	[WINMM.@]
 */
UINT WINAPI waveOutGetPosition(HWAVEOUT hWaveOut, LPMMTIME lpTime,
			       UINT uSize)
{
    TRACE("(%p, %p, %u)\n", hWaveOut, lpTime, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!uSize || !lpTime || uSize != sizeof(MMTIME))
        return MMSYSERR_INVALPARAM;

    return WINMM_GetPosition((HWAVE)hWaveOut, lpTime);
}

/**************************************************************************
 * 				waveOutGetPitch		[WINMM.@]
 */
UINT WINAPI waveOutGetPitch(HWAVEOUT hWaveOut, LPDWORD lpdw)
{
    TRACE("(%p, %p)\n", hWaveOut, lpdw);
    return MMSYSERR_NOTSUPPORTED;
}

/**************************************************************************
 * 				waveOutSetPitch		[WINMM.@]
 */
UINT WINAPI waveOutSetPitch(HWAVEOUT hWaveOut, DWORD dw)
{
    TRACE("(%p, %08x)\n", hWaveOut, dw);

    return MMSYSERR_NOTSUPPORTED;
}

/**************************************************************************
 * 				waveOutGetPlaybackRate	[WINMM.@]
 */
UINT WINAPI waveOutGetPlaybackRate(HWAVEOUT hWaveOut, LPDWORD lpdw)
{
    TRACE("(%p, %p)\n", hWaveOut, lpdw);

    return MMSYSERR_NOTSUPPORTED;
}

/**************************************************************************
 * 				waveOutSetPlaybackRate	[WINMM.@]
 */
UINT WINAPI waveOutSetPlaybackRate(HWAVEOUT hWaveOut, DWORD dw)
{
    TRACE("(%p, %08x)\n", hWaveOut, dw);

    return MMSYSERR_NOTSUPPORTED;
}

/**************************************************************************
 * 				waveOutGetVolume	[WINMM.@]
 */
UINT WINAPI waveOutGetVolume(HWAVEOUT hWaveOut, DWORD *out)
{
    WINMM_Device *device;
    UINT32 channels;
    float *vols;
    HRESULT hr;

    TRACE("(%p, %p)\n", hWaveOut, out);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!out)
        return MMSYSERR_INVALPARAM;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveOut);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    hr = IAudioStreamVolume_GetChannelCount(device->volume, &channels);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        ERR("GetChannelCount failed: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    vols = HeapAlloc(GetProcessHeap(), 0, sizeof(float) * channels);
    if(!vols){
        LeaveCriticalSection(&device->lock);
        return MMSYSERR_NOMEM;
    }

    hr = IAudioStreamVolume_GetAllVolumes(device->volume, channels, vols);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        HeapFree(GetProcessHeap(), 0, vols);
        ERR("GetAllVolumes failed: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    LeaveCriticalSection(&device->lock);

    *out = ((UINT16)(vols[0] * (DWORD)0xFFFF));
    if(channels > 1)
        *out |= ((UINT16)(vols[1] * (DWORD)0xFFFF)) << 16;

    HeapFree(GetProcessHeap(), 0, vols);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutSetVolume	[WINMM.@]
 */
UINT WINAPI waveOutSetVolume(HWAVEOUT hWaveOut, DWORD in)
{
    WINMM_Device *device;
    UINT32 channels;
    float *vols;
    HRESULT hr;

    TRACE("(%p, %08x)\n", hWaveOut, in);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveOut);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    hr = IAudioStreamVolume_GetChannelCount(device->volume, &channels);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        ERR("GetChannelCount failed: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    vols = HeapAlloc(GetProcessHeap(), 0, sizeof(float) * channels);
    if(!vols){
        LeaveCriticalSection(&device->lock);
        return MMSYSERR_NOMEM;
    }

    hr = IAudioStreamVolume_GetAllVolumes(device->volume, channels, vols);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        HeapFree(GetProcessHeap(), 0, vols);
        ERR("GetAllVolumes failed: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    vols[0] = (float)((DWORD)(in & 0xFFFF) / (float)0xFFFF);
    if(channels > 1)
        vols[1] = (float)((DWORD)(in >> 16) / (float)0xFFFF);

    hr = IAudioStreamVolume_SetAllVolumes(device->volume, channels, vols);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        HeapFree(GetProcessHeap(), 0, vols);
        ERR("SetAllVolumes failed: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    LeaveCriticalSection(&device->lock);

    HeapFree(GetProcessHeap(), 0, vols);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutGetID		[WINMM.@]
 */
UINT WINAPI waveOutGetID(HWAVEOUT hWaveOut, UINT* lpuDeviceID)
{
    WINMM_Device *device;
    UINT dev, junk;
    BOOL is_out;

    TRACE("(%p, %p)\n", hWaveOut, lpuDeviceID);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpuDeviceID)
        return MMSYSERR_INVALPARAM;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveOut);
    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    LeaveCriticalSection(&device->lock);

    WINMM_DecomposeHWAVE((HWAVE)hWaveOut, lpuDeviceID, &is_out, &dev, &junk);

    return MMSYSERR_NOERROR;
}

static UINT WINMM_QueryInstanceIDSize(UINT device, DWORD_PTR *len, BOOL is_out)
{
    UINT count;
    WINMM_MMDevice *devices;

    TRACE("(%u, %p, %d)\n", device, len, is_out);

    if(is_out){
        count = g_outmmdevices_count;
        devices = g_out_mmdevices;
    }else{
        count = g_inmmdevices_count;
        devices = g_in_mmdevices;
    }

    if(device >= count)
        return MMSYSERR_INVALHANDLE;

    *len = (lstrlenW(devices[device].dev_id) + 1) * sizeof(WCHAR);

    return MMSYSERR_NOERROR;
}

static UINT WINMM_QueryInstanceID(UINT device, WCHAR *str, DWORD_PTR len,
        BOOL is_out)
{
    UINT count, id_len;
    WINMM_MMDevice *devices;

    TRACE("(%u, %p, %d)\n", device, str, is_out);

    if(is_out){
        count = g_outmmdevices_count;
        devices = g_out_mmdevices;
    }else{
        count = g_inmmdevices_count;
        devices = g_in_mmdevices;
    }

    if(device >= count)
        return MMSYSERR_INVALHANDLE;

    id_len = (lstrlenW(devices[device].dev_id) + 1) * sizeof(WCHAR);
    if(len < id_len)
        return MMSYSERR_ERROR;

    memcpy(str, devices[device].dev_id, id_len);

    return MMSYSERR_NOERROR;
}

static UINT WINMM_DRVMessage(UINT dev, UINT message, DWORD_PTR param1,
        DWORD_PTR param2, BOOL is_out)
{
    WINE_MLD *wmld;
    UINT type = is_out ? MMDRV_WAVEOUT : MMDRV_WAVEIN;

    TRACE("(%u, %u, %ld, %ld, %d)\n", dev, message, param1, param2, is_out);

    if((wmld = MMDRV_Get(ULongToHandle(dev), type, FALSE)) == NULL){
        if((wmld = MMDRV_Get(ULongToHandle(dev), type, TRUE)) != NULL)
            return MMDRV_PhysicalFeatures(wmld, message, param1, param2);
        return MMSYSERR_INVALHANDLE;
    }

    if(message < DRVM_IOCTL ||
            (message >= DRVM_IOCTL_LAST && message < DRVM_MAPPER))
        return MMSYSERR_INVALPARAM;

    return MMDRV_Message(wmld, message, param1, param2);
}

static UINT WINMM_FillDSDriverDesc(UINT dev, DSDRIVERDESC *desc, BOOL is_out)
{
    WCHAR *name;

    if(is_out){
        if(dev >= g_outmmdevices_count)
            return MMSYSERR_INVALHANDLE;
        name = g_out_mmdevices[dev].out_caps.szPname;
    }else{
        if(dev >= g_inmmdevices_count)
            return MMSYSERR_INVALHANDLE;
        name = g_in_mmdevices[dev].in_caps.szPname;
    }

    memset(desc, 0, sizeof(*desc));

    strcpy(desc->szDesc, "WinMM: ");
    WideCharToMultiByte(CP_ACP, 0, name, -1,
            desc->szDesc + strlen(desc->szDesc),
            sizeof(desc->szDesc) - strlen(desc->szDesc), NULL, NULL);

    strcpy(desc->szDrvname, "winmm.dll");

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveOutMessage 		[WINMM.@]
 */
UINT WINAPI waveOutMessage(HWAVEOUT hWaveOut, UINT uMessage,
                           DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    TRACE("(%p, %u, %lx, %lx)\n", hWaveOut, uMessage, dwParam1, dwParam2);

    switch(uMessage){
    case DRV_QUERYFUNCTIONINSTANCEIDSIZE:
        return WINMM_QueryInstanceIDSize(HandleToULong(hWaveOut),
                (DWORD_PTR*)dwParam1, TRUE);
    case DRV_QUERYFUNCTIONINSTANCEID:
        return WINMM_QueryInstanceID(HandleToULong(hWaveOut),
                (WCHAR*)dwParam1, (DWORD_PTR)dwParam2, TRUE);
    /* TODO: Remove after dsound has been rewritten for mmdevapi */
    case DRV_QUERYDSOUNDDESC:
    case DRV_QUERYDSOUNDIFACE:
        if(dwParam2 == DS_HW_ACCEL_FULL)
            return WINMM_DRVMessage(HandleToULong(hWaveOut), uMessage,
                    dwParam1, 0, TRUE);
        return WINMM_FillDSDriverDesc(HandleToULong(hWaveOut),
                (DSDRIVERDESC*)dwParam1, TRUE);
    }

    return MMSYSERR_NOTSUPPORTED;
}

/**************************************************************************
 * 				waveInGetNumDevs 		[WINMM.@]
 */
UINT WINAPI waveInGetNumDevs(void)
{
    if(!WINMM_StartDevicesThread())
        return 0;

    TRACE("count: %u\n", g_inmmdevices_count);

    return g_inmmdevices_count;
}

/**************************************************************************
 * 				waveInGetDevCapsW 		[WINMM.@]
 */
UINT WINAPI waveInGetDevCapsW(UINT_PTR uDeviceID, LPWAVEINCAPSW lpCaps, UINT uSize)
{
    WAVEINCAPSW mapper_caps, *caps;

    TRACE("(%lu, %p, %u)\n", uDeviceID, lpCaps, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpCaps)
        return MMSYSERR_INVALPARAM;

    if(WINMM_IsMapper(uDeviceID)){
        /* FIXME: Should be localized */
        static const WCHAR mapper_pnameW[] = {'W','i','n','e',' ','S','o','u',
            'n','d',' ','M','a','p','p','e','r',0};

        mapper_caps.wMid = 0xFF;
        mapper_caps.wPid = 0xFF;
        mapper_caps.vDriverVersion = 0x00010001;
        mapper_caps.dwFormats = 0xFFFFFFFF;
        mapper_caps.wReserved1 = 0;
        mapper_caps.wChannels = 2;
        lstrcpyW(mapper_caps.szPname, mapper_pnameW);

        caps = &mapper_caps;
    }else{
        if(uDeviceID >= g_inmmdevices_count)
            return MMSYSERR_BADDEVICEID;

        caps = &g_in_mmdevices[uDeviceID].in_caps;
    }

    memcpy(lpCaps, caps, min(uSize, sizeof(*lpCaps)));

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveInGetDevCapsA 		[WINMM.@]
 */
UINT WINAPI waveInGetDevCapsA(UINT_PTR uDeviceID, LPWAVEINCAPSA lpCaps, UINT uSize)
{
    UINT ret;
    WAVEINCAPSW wicW;

    TRACE("(%lu, %p, %u)\n", uDeviceID, lpCaps, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpCaps)
        return MMSYSERR_INVALPARAM;

    ret = waveInGetDevCapsW(uDeviceID, &wicW, sizeof(wicW));

    if (ret == MMSYSERR_NOERROR) {
	WAVEINCAPSA wicA;
	wicA.wMid           = wicW.wMid;
	wicA.wPid           = wicW.wPid;
	wicA.vDriverVersion = wicW.vDriverVersion;
        WideCharToMultiByte( CP_ACP, 0, wicW.szPname, -1, wicA.szPname,
                             sizeof(wicA.szPname), NULL, NULL );
	wicA.dwFormats      = wicW.dwFormats;
	wicA.wChannels      = wicW.wChannels;
	memcpy(lpCaps, &wicA, min(uSize, sizeof(wicA)));
    }
    return ret;
}

/**************************************************************************
 * 				waveInOpen			[WINMM.@]
 */
MMRESULT WINAPI waveInOpen(HWAVEIN* lphWaveIn, UINT uDeviceID,
                           LPCWAVEFORMATEX lpFormat, DWORD_PTR dwCallback,
                           DWORD_PTR dwInstance, DWORD dwFlags)
{
    LRESULT res;
    HRESULT hr;
    WINMM_OpenInfo info;

    TRACE("(%p, %x, %p, %lx, %lx, %08x)\n", lphWaveIn, uDeviceID, lpFormat,
            dwCallback, dwInstance, dwFlags);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lphWaveIn && !(dwFlags & WAVE_FORMAT_QUERY))
        return MMSYSERR_INVALPARAM;

    hr = WINMM_StartDevicesThread();
    if(FAILED(hr)){
        ERR("Couldn't start the device thread: %08x\n", hr);
        return MMSYSERR_ERROR;
    }

    info.format = (WAVEFORMATEX*)lpFormat;
    info.callback = dwCallback;
    info.cb_user = dwInstance;
    info.req_device = uDeviceID;
    info.flags = dwFlags;

    res = SendMessageW(g_devices_hwnd, WIDM_OPEN, (DWORD_PTR)&info, 0);
    if(res != MMSYSERR_NOERROR)
        return res;

    if(lphWaveIn)
        *lphWaveIn = (HWAVEIN)info.handle;

    return res;
}

/**************************************************************************
 * 				waveInClose			[WINMM.@]
 */
UINT WINAPI waveInClose(HWAVEIN hWaveIn)
{
    TRACE("(%p)\n", hWaveIn);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    return SendMessageW(g_devices_hwnd, WIDM_CLOSE, (WPARAM)hWaveIn, 0);
}

/**************************************************************************
 * 				waveInPrepareHeader		[WINMM.@]
 */
UINT WINAPI waveInPrepareHeader(HWAVEIN hWaveIn, WAVEHDR* lpWaveInHdr,
				UINT uSize)
{
    TRACE("(%p, %p, %u)\n", hWaveIn, lpWaveInHdr, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpWaveInHdr || uSize < sizeof(WAVEHDR))
        return MMSYSERR_INVALPARAM;

    if(lpWaveInHdr->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;

    return WINMM_PrepareHeader((HWAVE)hWaveIn, lpWaveInHdr);
}

/**************************************************************************
 * 				waveInUnprepareHeader	[WINMM.@]
 */
UINT WINAPI waveInUnprepareHeader(HWAVEIN hWaveIn, WAVEHDR* lpWaveInHdr,
				  UINT uSize)
{
    TRACE("(%p, %p, %u)\n", hWaveIn, lpWaveInHdr, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpWaveInHdr || uSize < sizeof(WAVEHDR))
        return MMSYSERR_INVALPARAM;

    if(!(lpWaveInHdr->dwFlags & WHDR_PREPARED))
        return MMSYSERR_NOERROR;

    if(lpWaveInHdr->dwFlags & WHDR_INQUEUE)
        return WAVERR_STILLPLAYING;

    return WINMM_UnprepareHeader((HWAVE)hWaveIn, lpWaveInHdr);
}

/**************************************************************************
 * 				waveInAddBuffer		[WINMM.@]
 */
UINT WINAPI waveInAddBuffer(HWAVEIN hWaveIn, WAVEHDR *header, UINT uSize)
{
    WINMM_Device *device;

    TRACE("(%p, %p, %u)\n", hWaveIn, header, uSize);

    if(!header || uSize < sizeof(WAVEHDR))
        return MMSYSERR_INVALPARAM;

    if(!(header->dwFlags & WHDR_PREPARED))
        return WAVERR_UNPREPARED;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveIn);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    if(!device->first)
        device->first = device->last = header;
    else{
        device->last->lpNext = header;
        device->last = header;
    }

    header->dwBytesRecorded = 0;
    header->lpNext = NULL;
    header->dwFlags &= ~WHDR_DONE;
    header->dwFlags |= WHDR_INQUEUE;

    LeaveCriticalSection(&device->lock);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveInReset		[WINMM.@]
 */
UINT WINAPI waveInReset(HWAVEIN hWaveIn)
{
    TRACE("(%p)\n", hWaveIn);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    return WINMM_Reset((HWAVE)hWaveIn);
}

/**************************************************************************
 * 				waveInStart		[WINMM.@]
 */
UINT WINAPI waveInStart(HWAVEIN hWaveIn)
{
    WINMM_Device *device;
    HRESULT hr;

    TRACE("(%p)\n", hWaveIn);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveIn);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    hr = WINMM_BeginPlaying(device);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        return MMSYSERR_ERROR;
    }

    LeaveCriticalSection(&device->lock);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveInStop		[WINMM.@]
 */
UINT WINAPI waveInStop(HWAVEIN hWaveIn)
{
    WINMM_CBInfo cb_info;
    WINMM_Device *device;
    WAVEHDR *buf;
    HRESULT hr;

    TRACE("(%p)\n", hWaveIn);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveIn);

    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    hr = WINMM_Pause((HWAVE)hWaveIn);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        return MMSYSERR_ERROR;
    }
    device->stopped = TRUE;

    buf = device->first;
    if(buf && buf->dwBytesRecorded > 0){
        device->first = buf->lpNext;
        buf->dwFlags &= ~WHDR_INQUEUE;
        buf->dwFlags |= WHDR_DONE;
    }else
        buf = NULL;

    memcpy(&cb_info, &device->cb_info, sizeof(cb_info));

    LeaveCriticalSection(&device->lock);

    if(buf)
        WINMM_NotifyClient(&cb_info, WIM_DATA, (DWORD_PTR)buf, 0);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveInGetPosition	[WINMM.@]
 */
UINT WINAPI waveInGetPosition(HWAVEIN hWaveIn, LPMMTIME lpTime,
			      UINT uSize)
{
    TRACE("(%p, %p, %u)\n", hWaveIn, lpTime, uSize);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!uSize || !lpTime || uSize != sizeof(MMTIME))
        return MMSYSERR_INVALPARAM;

    return WINMM_GetPosition((HWAVE)hWaveIn, lpTime);
}

/**************************************************************************
 * 				waveInGetID			[WINMM.@]
 */
UINT WINAPI waveInGetID(HWAVEIN hWaveIn, UINT* lpuDeviceID)
{
    UINT dev, junk;
    BOOL is_out;
    WINMM_Device *device;

    TRACE("(%p, %p)\n", hWaveIn, lpuDeviceID);

    if(!WINMM_StartDevicesThread())
        return MMSYSERR_ERROR;

    if(!lpuDeviceID)
        return MMSYSERR_INVALPARAM;

    device = WINMM_GetDeviceFromHWAVE((HWAVE)hWaveIn);
    if(!WINMM_ValidateAndLock(device))
        return MMSYSERR_INVALHANDLE;

    LeaveCriticalSection(&device->lock);

    WINMM_DecomposeHWAVE((HWAVE)hWaveIn, lpuDeviceID, &is_out, &dev, &junk);

    return MMSYSERR_NOERROR;
}

/**************************************************************************
 * 				waveInMessage 		[WINMM.@]
 */
UINT WINAPI waveInMessage(HWAVEIN hWaveIn, UINT uMessage,
                          DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    TRACE("(%p, %u, %ld, %ld)\n", hWaveIn, uMessage, dwParam1, dwParam2);

    switch(uMessage){
    case DRV_QUERYFUNCTIONINSTANCEIDSIZE:
        return WINMM_QueryInstanceIDSize(HandleToULong(hWaveIn),
                (DWORD_PTR*)dwParam1, FALSE);
    case DRV_QUERYFUNCTIONINSTANCEID:
        return WINMM_QueryInstanceID(HandleToULong(hWaveIn),
                (WCHAR*)dwParam1, (DWORD_PTR)dwParam2, FALSE);
    /* TODO: Remove after dsound has been rewritten for mmdevapi */
    case DRV_QUERYDSOUNDDESC:
    case DRV_QUERYDSOUNDIFACE:
        if(dwParam2 == DS_HW_ACCEL_FULL)
            return WINMM_DRVMessage(HandleToULong(hWaveIn), uMessage,
                    dwParam1, 0, FALSE);
        return WINMM_FillDSDriverDesc(HandleToULong(hWaveIn),
                (DSDRIVERDESC*)dwParam1, FALSE);
    }

    return MMSYSERR_NOTSUPPORTED;
}

UINT WINAPI mixerGetNumDevs(void)
{
    return 0;
}

/**************************************************************************
 * 				mixerGetDevCapsA		[WINMM.@]
 */
UINT WINAPI mixerGetDevCapsA(UINT_PTR uDeviceID, LPMIXERCAPSA lpCaps, UINT uSize)
{
    MIXERCAPSW micW;
    UINT       ret;

    if (lpCaps == NULL) return MMSYSERR_INVALPARAM;

    ret = mixerGetDevCapsW(uDeviceID, &micW, sizeof(micW));

    if (ret == MMSYSERR_NOERROR) {
        MIXERCAPSA micA;
        micA.wMid           = micW.wMid;
        micA.wPid           = micW.wPid;
        micA.vDriverVersion = micW.vDriverVersion;
        WideCharToMultiByte( CP_ACP, 0, micW.szPname, -1, micA.szPname,
                             sizeof(micA.szPname), NULL, NULL );
        micA.fdwSupport     = micW.fdwSupport;
        micA.cDestinations  = micW.cDestinations;
        memcpy(lpCaps, &micA, min(uSize, sizeof(micA)));
    }
    return ret;
}

/**************************************************************************
 * 				mixerGetDevCapsW		[WINMM.@]
 */
UINT WINAPI mixerGetDevCapsW(UINT_PTR uDeviceID, LPMIXERCAPSW lpCaps, UINT uSize)
{
    if(!lpCaps)
        return MMSYSERR_INVALPARAM;

    return MMSYSERR_BADDEVICEID;
}

/**************************************************************************
 * 				mixerOpen			[WINMM.@]
 */
UINT WINAPI mixerOpen(LPHMIXER lphMix, UINT uDeviceID, DWORD_PTR dwCallback,
                      DWORD_PTR dwInstance, DWORD fdwOpen)
{
    DWORD dwRet;

    TRACE("(%p, %d, %08lx, %08lx, %08x)\n",
	  lphMix, uDeviceID, dwCallback, dwInstance, fdwOpen);

    dwRet = WINMM_CheckCallback(dwCallback, fdwOpen, TRUE);
    if (dwRet != MMSYSERR_NOERROR)
        return dwRet;

    return MMSYSERR_BADDEVICEID;
}

/**************************************************************************
 * 				mixerClose			[WINMM.@]
 */
UINT WINAPI mixerClose(HMIXER hMix)
{
    TRACE("(%p)\n", hMix);
    return MMSYSERR_INVALHANDLE;
}

/**************************************************************************
 * 				mixerGetID			[WINMM.@]
 */
UINT WINAPI mixerGetID(HMIXEROBJ hmix, LPUINT lpid, DWORD fdwID)
{
    TRACE("(%p, %p, %08x)\n", hmix, lpid, fdwID);
    return MMSYSERR_INVALHANDLE;
}

/**************************************************************************
 * 				mixerGetControlDetailsW		[WINMM.@]
 */
UINT WINAPI mixerGetControlDetailsW(HMIXEROBJ hmix, LPMIXERCONTROLDETAILS lpmcdW,
				    DWORD fdwDetails)
{
    TRACE("(%p, %p, %08x)\n", hmix, lpmcdW, fdwDetails);

    if(!lpmcdW || lpmcdW->cbStruct != sizeof(*lpmcdW))
        return MMSYSERR_INVALPARAM;

    return MMSYSERR_INVALHANDLE;
}

/**************************************************************************
 * 				mixerGetControlDetailsA	[WINMM.@]
 */
UINT WINAPI mixerGetControlDetailsA(HMIXEROBJ hmix, LPMIXERCONTROLDETAILS lpmcdA,
                                    DWORD fdwDetails)
{
    DWORD			ret = MMSYSERR_NOTENABLED;

    TRACE("(%p, %p, %08x)\n", hmix, lpmcdA, fdwDetails);

    if (lpmcdA == NULL || lpmcdA->cbStruct != sizeof(*lpmcdA))
	return MMSYSERR_INVALPARAM;

    switch (fdwDetails & MIXER_GETCONTROLDETAILSF_QUERYMASK) {
    case MIXER_GETCONTROLDETAILSF_VALUE:
	/* can safely use A structure as it is, no string inside */
	ret = mixerGetControlDetailsW(hmix, lpmcdA, fdwDetails);
	break;
    case MIXER_GETCONTROLDETAILSF_LISTTEXT:
	{
            MIXERCONTROLDETAILS_LISTTEXTA *pDetailsA = lpmcdA->paDetails;
            MIXERCONTROLDETAILS_LISTTEXTW *pDetailsW;
	    int size = max(1, lpmcdA->cChannels) * sizeof(MIXERCONTROLDETAILS_LISTTEXTW);
            unsigned int i;

	    if (lpmcdA->u.cMultipleItems != 0) {
		size *= lpmcdA->u.cMultipleItems;
	    }
	    pDetailsW = HeapAlloc(GetProcessHeap(), 0, size);
            lpmcdA->paDetails = pDetailsW;
            lpmcdA->cbDetails = sizeof(MIXERCONTROLDETAILS_LISTTEXTW);
	    /* set up lpmcd->paDetails */
	    ret = mixerGetControlDetailsW(hmix, lpmcdA, fdwDetails);
	    /* copy from lpmcd->paDetails back to paDetailsW; */
            if (ret == MMSYSERR_NOERROR) {
                for (i = 0; i < lpmcdA->u.cMultipleItems * lpmcdA->cChannels; i++) {
                    pDetailsA->dwParam1 = pDetailsW->dwParam1;
                    pDetailsA->dwParam2 = pDetailsW->dwParam2;
                    WideCharToMultiByte( CP_ACP, 0, pDetailsW->szName, -1,
                                         pDetailsA->szName,
                                         sizeof(pDetailsA->szName), NULL, NULL );
                    pDetailsA++;
                    pDetailsW++;
                }
                pDetailsA -= lpmcdA->u.cMultipleItems * lpmcdA->cChannels;
                pDetailsW -= lpmcdA->u.cMultipleItems * lpmcdA->cChannels;
            }
	    HeapFree(GetProcessHeap(), 0, pDetailsW);
	    lpmcdA->paDetails = pDetailsA;
            lpmcdA->cbDetails = sizeof(MIXERCONTROLDETAILS_LISTTEXTA);
	}
	break;
    default:
	ERR("Unsupported fdwDetails=0x%08x\n", fdwDetails);
    }

    return ret;
}

/**************************************************************************
 * 				mixerGetLineControlsA	[WINMM.@]
 */
UINT WINAPI mixerGetLineControlsA(HMIXEROBJ hmix, LPMIXERLINECONTROLSA lpmlcA,
				  DWORD fdwControls)
{
    MIXERLINECONTROLSW	mlcW;
    DWORD		ret;
    unsigned int	i;

    TRACE("(%p, %p, %08x)\n", hmix, lpmlcA, fdwControls);

    if (lpmlcA == NULL || lpmlcA->cbStruct != sizeof(*lpmlcA) ||
	lpmlcA->cbmxctrl != sizeof(MIXERCONTROLA))
	return MMSYSERR_INVALPARAM;

    mlcW.cbStruct = sizeof(mlcW);
    mlcW.dwLineID = lpmlcA->dwLineID;
    mlcW.u.dwControlID = lpmlcA->u.dwControlID;
    mlcW.u.dwControlType = lpmlcA->u.dwControlType;

    /* Debugging on Windows shows for MIXER_GETLINECONTROLSF_ONEBYTYPE only,
       the control count is assumed to be 1 - This is relied upon by a game,
       "Dynomite Deluze"                                                    */
    if (MIXER_GETLINECONTROLSF_ONEBYTYPE == (fdwControls & MIXER_GETLINECONTROLSF_QUERYMASK)) {
        mlcW.cControls = 1;
    } else {
        mlcW.cControls = lpmlcA->cControls;
    }
    mlcW.cbmxctrl = sizeof(MIXERCONTROLW);
    mlcW.pamxctrl = HeapAlloc(GetProcessHeap(), 0,
			      mlcW.cControls * mlcW.cbmxctrl);

    ret = mixerGetLineControlsW(hmix, &mlcW, fdwControls);

    if (ret == MMSYSERR_NOERROR) {
	lpmlcA->dwLineID = mlcW.dwLineID;
	lpmlcA->u.dwControlID = mlcW.u.dwControlID;
	lpmlcA->u.dwControlType = mlcW.u.dwControlType;

	for (i = 0; i < mlcW.cControls; i++) {
	    lpmlcA->pamxctrl[i].cbStruct = sizeof(MIXERCONTROLA);
	    lpmlcA->pamxctrl[i].dwControlID = mlcW.pamxctrl[i].dwControlID;
	    lpmlcA->pamxctrl[i].dwControlType = mlcW.pamxctrl[i].dwControlType;
	    lpmlcA->pamxctrl[i].fdwControl = mlcW.pamxctrl[i].fdwControl;
	    lpmlcA->pamxctrl[i].cMultipleItems = mlcW.pamxctrl[i].cMultipleItems;
            WideCharToMultiByte( CP_ACP, 0, mlcW.pamxctrl[i].szShortName, -1,
                                 lpmlcA->pamxctrl[i].szShortName,
                                 sizeof(lpmlcA->pamxctrl[i].szShortName), NULL, NULL );
            WideCharToMultiByte( CP_ACP, 0, mlcW.pamxctrl[i].szName, -1,
                                 lpmlcA->pamxctrl[i].szName,
                                 sizeof(lpmlcA->pamxctrl[i].szName), NULL, NULL );
	    /* sizeof(lpmlcA->pamxctrl[i].Bounds) ==
	     * sizeof(mlcW.pamxctrl[i].Bounds) */
	    memcpy(&lpmlcA->pamxctrl[i].Bounds, &mlcW.pamxctrl[i].Bounds,
		   sizeof(mlcW.pamxctrl[i].Bounds));
	    /* sizeof(lpmlcA->pamxctrl[i].Metrics) ==
	     * sizeof(mlcW.pamxctrl[i].Metrics) */
	    memcpy(&lpmlcA->pamxctrl[i].Metrics, &mlcW.pamxctrl[i].Metrics,
		   sizeof(mlcW.pamxctrl[i].Metrics));
	}
    }

    HeapFree(GetProcessHeap(), 0, mlcW.pamxctrl);

    return ret;
}

/**************************************************************************
 * 				mixerGetLineControlsW		[WINMM.@]
 */
UINT WINAPI mixerGetLineControlsW(HMIXEROBJ hmix, LPMIXERLINECONTROLSW lpmlcW,
				  DWORD fdwControls)
{
    TRACE("(%p, %p, %08x)\n", hmix, lpmlcW, fdwControls);

    if(!lpmlcW || lpmlcW->cbStruct != sizeof(*lpmlcW))
        return MMSYSERR_INVALPARAM;

    return MMSYSERR_INVALHANDLE;
}

/**************************************************************************
 * 				mixerGetLineInfoW		[WINMM.@]
 */
UINT WINAPI mixerGetLineInfoW(HMIXEROBJ hmix, LPMIXERLINEW lpmliW, DWORD fdwInfo)
{
    TRACE("(%p, %p, %08x)\n", hmix, lpmliW, fdwInfo);

    return MMSYSERR_INVALHANDLE;
}

/**************************************************************************
 * 				mixerGetLineInfoA		[WINMM.@]
 */
UINT WINAPI mixerGetLineInfoA(HMIXEROBJ hmix, LPMIXERLINEA lpmliA,
			      DWORD fdwInfo)
{
    MIXERLINEW		mliW;
    UINT		ret;

    TRACE("(%p, %p, %08x)\n", hmix, lpmliA, fdwInfo);

    if (lpmliA == NULL || lpmliA->cbStruct != sizeof(*lpmliA))
	return MMSYSERR_INVALPARAM;

    mliW.cbStruct = sizeof(mliW);
    switch (fdwInfo & MIXER_GETLINEINFOF_QUERYMASK) {
    case MIXER_GETLINEINFOF_COMPONENTTYPE:
	mliW.dwComponentType = lpmliA->dwComponentType;
	break;
    case MIXER_GETLINEINFOF_DESTINATION:
	mliW.dwDestination = lpmliA->dwDestination;
	break;
    case MIXER_GETLINEINFOF_LINEID:
	mliW.dwLineID = lpmliA->dwLineID;
	break;
    case MIXER_GETLINEINFOF_SOURCE:
	mliW.dwDestination = lpmliA->dwDestination;
	mliW.dwSource = lpmliA->dwSource;
	break;
    case MIXER_GETLINEINFOF_TARGETTYPE:
	mliW.Target.dwType = lpmliA->Target.dwType;
	mliW.Target.wMid = lpmliA->Target.wMid;
	mliW.Target.wPid = lpmliA->Target.wPid;
	mliW.Target.vDriverVersion = lpmliA->Target.vDriverVersion;
        MultiByteToWideChar( CP_ACP, 0, lpmliA->Target.szPname, -1, mliW.Target.szPname, sizeof(mliW.Target.szPname)/sizeof(WCHAR));
	break;
    default:
	WARN("Unsupported fdwControls=0x%08x\n", fdwInfo);
        return MMSYSERR_INVALFLAG;
    }

    ret = mixerGetLineInfoW(hmix, &mliW, fdwInfo);

    if(ret == MMSYSERR_NOERROR)
    {
        lpmliA->dwDestination = mliW.dwDestination;
        lpmliA->dwSource = mliW.dwSource;
        lpmliA->dwLineID = mliW.dwLineID;
        lpmliA->fdwLine = mliW.fdwLine;
        lpmliA->dwUser = mliW.dwUser;
        lpmliA->dwComponentType = mliW.dwComponentType;
        lpmliA->cChannels = mliW.cChannels;
        lpmliA->cConnections = mliW.cConnections;
        lpmliA->cControls = mliW.cControls;
        WideCharToMultiByte( CP_ACP, 0, mliW.szShortName, -1, lpmliA->szShortName,
                             sizeof(lpmliA->szShortName), NULL, NULL);
        WideCharToMultiByte( CP_ACP, 0, mliW.szName, -1, lpmliA->szName,
                             sizeof(lpmliA->szName), NULL, NULL );
        lpmliA->Target.dwType = mliW.Target.dwType;
        lpmliA->Target.dwDeviceID = mliW.Target.dwDeviceID;
        lpmliA->Target.wMid = mliW.Target.wMid;
        lpmliA->Target.wPid = mliW.Target.wPid;
        lpmliA->Target.vDriverVersion = mliW.Target.vDriverVersion;
        WideCharToMultiByte( CP_ACP, 0, mliW.Target.szPname, -1, lpmliA->Target.szPname,
                             sizeof(lpmliA->Target.szPname), NULL, NULL );
    }
    return ret;
}

/**************************************************************************
 * 				mixerSetControlDetails	[WINMM.@]
 */
UINT WINAPI mixerSetControlDetails(HMIXEROBJ hmix, LPMIXERCONTROLDETAILS lpmcd,
				   DWORD fdwDetails)
{
    TRACE("(%p, %p, %08x)\n", hmix, lpmcd, fdwDetails);

    return MMSYSERR_INVALHANDLE;
}

/**************************************************************************
 * 				mixerMessage		[WINMM.@]
 */
DWORD WINAPI mixerMessage(HMIXER hmix, UINT uMsg, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    TRACE("(%p, %d, %08lx, %08lx)\n", hmix, uMsg, dwParam1, dwParam2);

    return MMSYSERR_INVALHANDLE;
}
