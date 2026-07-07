/*
 * msfs fork: native Windows shared-resource interop.
 *
 * vkd3d's stock shared-resource support targets Wine: it exports Vulkan
 * opaque handles and stamps metadata through Wine-only D3DKMT escapes.
 * Real Windows processes (Fenix displays, capture tools, ...) cannot open
 * those handles. This helper allocates shared textures through the REAL
 * D3D11 runtime instead, so the NT share handle vkd3d hands out is a
 * genuine runtime handle any process can open, while the memory itself is
 * imported into Vulkan via VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
 *
 * This header deliberately uses only plain C types: native_interop.c is the
 * single TU that includes the real <d3d11.h>/<dxgi1_2.h>, which would clash
 * with vkd3d's own D3D11 typedef subset in vkd3d_private.h.
 */

#ifndef __VKD3D_NATIVE_INTEROP_H
#define __VKD3D_NATIVE_INTEROP_H

#include <stdbool.h>
#include <stdint.h>

/* Lazily initializes a helper device on the adapter matching `luid` (8 bytes)
 * inside *interop_slot (thread-safe, may store a negative-cache sentinel).
 * On success returns a process-local NT share handle in *out_nt_handle and
 * the keep-alive texture object (IUnknown) in *out_texture.
 * bind_flags are D3D11_BIND_* values, dxgi_format/sample values verbatim. */
bool vkd3d_native_interop_create_shared_texture(void **interop_slot, const void *luid,
        uint32_t width, uint32_t height, uint16_t mip_levels, uint16_t array_size,
        uint32_t dxgi_format, uint32_t sample_count, uint32_t sample_quality,
        uint32_t bind_flags, void **out_nt_handle, void **out_texture);

/* IUnknown_Release on an object returned by this module. */
void vkd3d_native_interop_release_object(void *object);

/* Destroys the helper device stored in a device's interop slot. */
void vkd3d_native_interop_destroy(void *interop);

#endif  /* __VKD3D_NATIVE_INTEROP_H */
