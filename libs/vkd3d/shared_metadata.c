/*
 * Copyright 2021 Derek Lesho for Codeweavers
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

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"

#include "winioctl.h"

#define IOCTL_SHARED_GPU_RESOURCE_SET_METADATA           CTL_CODE(FILE_DEVICE_VIDEO, 4, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_SHARED_GPU_RESOURCE_GET_METADATA           CTL_CODE(FILE_DEVICE_VIDEO, 5, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_SHARED_GPU_RESOURCE_OPEN                   CTL_CODE(FILE_DEVICE_VIDEO, 1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* native-windows fallback transport for the dxvk<->vkd3d shared-texture
 * metadata. the \\.\SharedGpuResource device above only exists under wine, so
 * on native windows the ioctls fail and the decoded-video texture metadata
 * never crosses from dxvk to vkd3d (audio-only video in msfs). we stash it in a
 * temp file keyed by the raw shared-handle value, which is stable for global
 * kmt handles and same-process nt handles. dxvk uses the identical scheme. */
static void vkd3d_shared_metadata_native_path(HANDLE handle, char *path, size_t size)
{
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);

    if (!n || n >= sizeof(tmp))
    {
        path[0] = '\0';
        return;
    }
    snprintf(path, size, "%svkd3d-dxvk-shared-%llx.bin", tmp, (unsigned long long)(ULONG_PTR)handle);
}

static bool vkd3d_set_shared_metadata_native(HANDLE handle, void *buf, uint32_t buf_size)
{
    char path[MAX_PATH];
    DWORD written = 0;
    HANDLE file;
    bool ret;

    vkd3d_shared_metadata_native_path(handle, path, sizeof(path));
    if (!path[0])
        return false;

    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    ret = WriteFile(file, buf, buf_size, &written, NULL) && written == buf_size;
    CloseHandle(file);
    return ret;
}

static bool vkd3d_get_shared_metadata_native(HANDLE handle, void *buf, uint32_t buf_size, uint32_t *metadata_size)
{
    char path[MAX_PATH];
    DWORD read_bytes = 0;
    HANDLE file;
    bool ret;

    vkd3d_shared_metadata_native_path(handle, path, sizeof(path));
    if (!path[0])
        return false;

    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    ret = ReadFile(file, buf, buf_size, &read_bytes, NULL) && read_bytes > 0;
    CloseHandle(file);
    if (metadata_size)
        *metadata_size = read_bytes;
    return ret;
}

bool vkd3d_set_shared_metadata(HANDLE handle, void *buf, uint32_t buf_size)
{
    DWORD ret_size;

    if (DeviceIoControl(handle, IOCTL_SHARED_GPU_RESOURCE_SET_METADATA, buf, buf_size, NULL, 0, &ret_size, NULL))
        return true;

    /* wine device absent -> native windows fallback */
    return vkd3d_set_shared_metadata_native(handle, buf, buf_size);
}

bool vkd3d_get_shared_metadata(HANDLE handle, void *buf, uint32_t buf_size, uint32_t *metadata_size)
{
    DWORD ret_size;

    bool ret = DeviceIoControl(handle, IOCTL_SHARED_GPU_RESOURCE_GET_METADATA, NULL, 0, buf, buf_size, &ret_size, NULL);

    if (ret)
    {
        if (metadata_size)
            *metadata_size = ret_size;
        return true;
    }

    /* wine device absent -> native windows fallback */
    return vkd3d_get_shared_metadata_native(handle, buf, buf_size, metadata_size);
}

HANDLE vkd3d_open_kmt_handle(HANDLE kmt_handle)
{
    struct
    {
        unsigned int kmt_handle;
        /* the following parameter represents a larger sized string for a dynamically allocated struct for use when opening an object by name */
        WCHAR name[1];
    } shared_resource_open;

    HANDLE nt_handle = CreateFileA("\\\\.\\SharedGpuResource", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (nt_handle == INVALID_HANDLE_VALUE)
        return nt_handle;

    shared_resource_open.kmt_handle = (ULONG_PTR)kmt_handle;
    shared_resource_open.name[0] = 0;
    if (!DeviceIoControl(nt_handle, IOCTL_SHARED_GPU_RESOURCE_OPEN, &shared_resource_open, sizeof(shared_resource_open), NULL, 0, NULL, NULL))
    {
        CloseHandle(nt_handle);
        return INVALID_HANDLE_VALUE;
    }
    return nt_handle;
}
