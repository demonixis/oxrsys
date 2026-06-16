# Licensing

OXRSys source code is licensed under [MPL-2.0](../LICENSE), unless a file states otherwise.

Third-party dependencies, SDKs, tools, platform runtimes, and generated assets keep their own licenses and terms. Review upstream license files before redistributing binaries that include or depend on them.

## FFmpeg

Windows vcpkg builds statically link the FFmpeg libraries used by the runtime (`libavcodec`,
`libavutil`, and `libswscale`) into `oxrsys-runtime.dll`. Keep the vcpkg manifest's FFmpeg
dependency on `default-features: false` and do not enable GPL or nonfree FFmpeg features for
redistributable builds. Release archives that include statically linked FFmpeg should preserve the
FFmpeg license notices and corresponding source/build information so recipients can rebuild or
relink the LGPL-covered FFmpeg components.

## Trademark Notice

OXRSys is an unofficial OpenXR runtime project. It is independent software and is not affiliated with, endorsed by, sponsored by, or approved by The Khronos Group, Meta, Apple, LunarG, Unity Technologies, the Godot project, or any other owner of the platforms, SDKs, runtimes, tools, and trademarks referenced here.

OpenXR and Vulkan are trademarks or registered trademarks of The Khronos Group Inc. Apple, macOS, visionOS, Metal, and related marks are trademarks of Apple Inc. Meta, Quest, and related marks are trademarks of Meta Platforms, Inc. Pico and related marks are trademarks of their respective owners. Unity and Godot names and logos remain the property of their respective owners.
