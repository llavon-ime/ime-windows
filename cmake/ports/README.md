# ggml overlay

This is the registry's ggml 0.11.1 port plus the application's Windows Vulkan
pipeline cache. Keep the upstream port patches when updating this overlay.

`ggml/persistent-vulkan-cache.diff` wires a cache into compute-pipeline creation
and adds an optional `ggml_backend_vk_save_pipeline_cache` registry function.
`ggml/llavon-vulkan-cache.hpp` owns the cache, its validation and atomic disk writes.
The backend uses C++23 and links Windows CNG (`bcrypt`) for SHA-256. Other
platforms retain their existing compute-pipeline creation path.

The default path is `%LOCALAPPDATA%\Llavon IME\vulkan-cache`. The filename and
validated envelope identify the backend cache schema, GPU vendor/device,
driver version, pointer width and Vulkan pipeline-cache UUID. The Vulkan
header and payload checksum are validated before data reaches the driver.
Files larger than 128 MiB are ignored. Failed reads, writes or cache creation
do not prevent inference. Writers use unique temporary files and replace the
destination atomically; concurrent processes can replace one another's cache
contents, but never expose a partial file.

The core explicitly saves after model preparation. The device also saves
remaining changes on destruction. Prediction does not perform disk writes.
Unchanged payloads are not rewritten. Driver updates use a new filename;
obsolete files may be removed without affecting correctness.

Diagnostics can override `GGML_VK_PIPELINE_CACHE_DIR`, or set
`GGML_VK_PIPELINE_CACHE_DISABLE` to disable caching for a comparison. These
are process environment settings and never modify the registry.

See [Vulkan pipeline-cache creation](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html),
[serialization](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html),
and [the compatibility header](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheHeaderVersionOne.html).

Manual hardware validation is in
`service/tests/vulkan_pipeline_cache_hardware.ps1`. It runs fresh processes
against `ime-core-latency-check` and checks reuse, corrupt data, incompatible
identities, repair and an unwritable directory. Test paths stay in the supplied
artifact directory; no existing cache is deleted.
