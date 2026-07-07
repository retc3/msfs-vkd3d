/*
 * msfs fork: native Windows shared-resource interop implementation.
 * See vkd3d_native_interop.h for the why. This is the only TU that includes
 * the real D3D11/DXGI headers; keep vkd3d headers out of here.
 */

#ifdef _WIN32

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string.h>

#include "vkd3d_native_interop.h"

/* Defined locally to avoid linking dxguid / colliding with other TUs. */
static const GUID vkd3d_native_uuid_IDXGIFactory1 =
        {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID vkd3d_native_uuid_IDXGIResource1 =
        {0x30961379, 0x4609, 0x4a41, {0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e, 0xe0, 0xc1}};

/* Negative cache: helper creation failed once, don't retry per allocation. */
#define VKD3D_NATIVE_INTEROP_UNSUPPORTED ((void *)(UINT_PTR)1)

struct vkd3d_native_interop
{
    HMODULE dxgi_module;
    HMODULE d3d11_module;
    ID3D11Device *device;
};

/* Load strictly from System32 so we get the REAL runtime, not the DXVK/vkd3d
 * DLLs sitting next to the game executable. */
static HMODULE vkd3d_native_load_system_module(const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    size_t len;
    UINT n;

    n = GetSystemDirectoryW(path, MAX_PATH);
    len = wcslen(name);
    if (!n || n + 1 + len + 1 > MAX_PATH)
        return NULL;

    path[n] = L'\\';
    memcpy(path + n + 1, name, (len + 1) * sizeof(WCHAR));
    return LoadLibraryW(path);
}

static void vkd3d_native_interop_free(struct vkd3d_native_interop *interop)
{
    if (!interop)
        return;
    if (interop->device)
        ID3D11Device_Release(interop->device);
    if (interop->d3d11_module)
        FreeLibrary(interop->d3d11_module);
    if (interop->dxgi_module)
        FreeLibrary(interop->dxgi_module);
    HeapFree(GetProcessHeap(), 0, interop);
}

static struct vkd3d_native_interop *vkd3d_native_interop_create(const void *luid)
{
    typedef HRESULT (WINAPI *PFN_vkd3d_CreateDXGIFactory1)(REFIID iid, void **factory);
    PFN_vkd3d_CreateDXGIFactory1 create_factory;
    PFN_D3D11_CREATE_DEVICE create_device;
    struct vkd3d_native_interop *interop;
    IDXGIAdapter1 *adapter = NULL;
    IDXGIFactory1 *factory = NULL;
    DXGI_ADAPTER_DESC1 desc;
    unsigned int i;
    HRESULT hr;

    if (!(interop = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*interop))))
        return NULL;

    interop->dxgi_module = vkd3d_native_load_system_module(L"dxgi.dll");
    interop->d3d11_module = vkd3d_native_load_system_module(L"d3d11.dll");
    if (!interop->dxgi_module || !interop->d3d11_module)
        goto fail;

    create_factory = (PFN_vkd3d_CreateDXGIFactory1)(void *)
            GetProcAddress(interop->dxgi_module, "CreateDXGIFactory1");
    create_device = (PFN_D3D11_CREATE_DEVICE)(void *)
            GetProcAddress(interop->d3d11_module, "D3D11CreateDevice");
    if (!create_factory || !create_device)
        goto fail;

    if (FAILED(create_factory(&vkd3d_native_uuid_IDXGIFactory1, (void **)&factory)))
        goto fail;

    /* Find the adapter that matches the Vulkan device's LUID. */
    for (i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) == S_OK; i++)
    {
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))
                && !memcmp(&desc.AdapterLuid, luid, sizeof(desc.AdapterLuid)))
            break;
        IDXGIAdapter1_Release(adapter);
        adapter = NULL;
    }

    if (!adapter)
        goto fail;

    hr = create_device((IDXGIAdapter *)adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
            NULL, 0, D3D11_SDK_VERSION, &interop->device, NULL, NULL);

    IDXGIAdapter1_Release(adapter);
    IDXGIFactory1_Release(factory);

    if (FAILED(hr) || !interop->device)
    {
        factory = NULL;
        goto fail;
    }

    return interop;

fail:
    if (factory)
        IDXGIFactory1_Release(factory);
    vkd3d_native_interop_free(interop);
    return NULL;
}

static struct vkd3d_native_interop *vkd3d_native_interop_get(void **interop_slot, const void *luid)
{
    struct vkd3d_native_interop *interop, *previous;

    interop = InterlockedCompareExchangePointer(interop_slot, NULL, NULL);
    if (interop == VKD3D_NATIVE_INTEROP_UNSUPPORTED)
        return NULL;
    if (interop)
        return interop;

    interop = vkd3d_native_interop_create(luid);

    previous = InterlockedCompareExchangePointer(interop_slot,
            interop ? (void *)interop : VKD3D_NATIVE_INTEROP_UNSUPPORTED, NULL);
    if (previous)
    {
        /* Lost the race; use the winner. */
        vkd3d_native_interop_free(interop);
        return previous == VKD3D_NATIVE_INTEROP_UNSUPPORTED ? NULL
                : (struct vkd3d_native_interop *)previous;
    }

    return interop;
}

bool vkd3d_native_interop_create_shared_texture(void **interop_slot, const void *luid,
        uint32_t width, uint32_t height, uint16_t mip_levels, uint16_t array_size,
        uint32_t dxgi_format, uint32_t sample_count, uint32_t sample_quality,
        uint32_t bind_flags, void **out_nt_handle, void **out_texture)
{
    /* NT-handle sharing without a keyed mutex first: this matches the D3D12
     * shared-heap protocol the game and external apps expect. Some runtime
     * versions insist on the legacy SHARED bit alongside it. */
    static const UINT misc_flag_attempts[] =
    {
        D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
        D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
    };

    struct vkd3d_native_interop *interop;
    ID3D11Texture2D *texture = NULL;
    IDXGIResource1 *dxgi_resource;
    D3D11_TEXTURE2D_DESC desc;
    HANDLE nt_handle = NULL;
    unsigned int i;
    HRESULT hr;

    *out_nt_handle = NULL;
    *out_texture = NULL;

    if (!(interop = vkd3d_native_interop_get(interop_slot, luid)))
        return false;

    memset(&desc, 0, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = mip_levels;
    desc.ArraySize = array_size;
    desc.Format = (DXGI_FORMAT)dxgi_format;
    desc.SampleDesc.Count = sample_count;
    desc.SampleDesc.Quality = sample_quality;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;

    for (i = 0; i < ARRAYSIZE(misc_flag_attempts); i++)
    {
        desc.MiscFlags = misc_flag_attempts[i];
        if (SUCCEEDED(hr = ID3D11Device_CreateTexture2D(interop->device, &desc, NULL, &texture)))
            break;
        texture = NULL;
    }

    if (!texture)
        return false;

    if (FAILED(hr = ID3D11Texture2D_QueryInterface(texture,
            &vkd3d_native_uuid_IDXGIResource1, (void **)&dxgi_resource)))
    {
        ID3D11Texture2D_Release(texture);
        return false;
    }

    hr = IDXGIResource1_CreateSharedHandle(dxgi_resource, NULL,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &nt_handle);
    IDXGIResource1_Release(dxgi_resource);

    if (FAILED(hr) || !nt_handle)
    {
        ID3D11Texture2D_Release(texture);
        return false;
    }

    *out_nt_handle = nt_handle;
    *out_texture = texture;
    return true;
}

void vkd3d_native_interop_release_object(void *object)
{
    if (object)
        IUnknown_Release((IUnknown *)object);
}

void vkd3d_native_interop_destroy(void *interop)
{
    if (interop && interop != VKD3D_NATIVE_INTEROP_UNSUPPORTED)
        vkd3d_native_interop_free(interop);
}

#endif  /* _WIN32 */
