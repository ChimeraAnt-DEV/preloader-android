#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file GameCosmetics.h
 * @brief The native cosmetics override surface: skin/cape, texture and render-tick seams.
 *
 * This is the native half of the cosmetics renderer. It owns the *registry* — which player id
 * wears which cape pixels, which texture id is replaced by which pixels, and the geometry blob the
 * render hook should draw — and the fail-closed hooks a real build would substitute through.
 *
 * **What is wired and what is not.** The registry and the JNI surface are real: the launcher fills
 * them and they are read on the game thread. The substitution points (the Skin cape retrieval and
 * the texture binder) are resolved by RTTI name exactly like the local-player and player-render
 * feeds, and the detours are **pure passthroughs** — they forward every argument unchanged and
 * only record a call count, because the game strips its symbols so the real ABI of these functions
 * is not recoverable. A build whose slot does not resolve keeps the resource-pack path. Nothing
 * here dereferences an unknown argument, so a wrong slot cannot fault the render thread.
 *
 * The registry is the durable part: when a substitution is implemented it reads these tables,
 * and the launcher side does not change.
 */
namespace pl::runtime {

/** One override image: the pixels and the id they belong to. */
struct CosmeticImage {
  /** The id the launcher keyed this override on (a UUID or texture-name hash). */
  std::uint64_t key = 0;
  /** Raw RGBA rows, top-left origin, width*height*4 bytes. Owned by the registry copy. */
  const std::uint8_t *rgba = nullptr;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

/**
 * @brief Replaces (or adds) the cape override for a player key.
 *
 * Copies the pixels, so the caller's buffer may be freed immediately. A zero-size image clears the
 * entry.
 */
void SetCapeOverride(std::uint64_t playerKey, const std::uint8_t *rgba,
                     std::uint32_t width, std::uint32_t height);

/** Clears every cape override. */
void ClearCapeOverrides();

/** The override for a player key, or nullptr when none. */
const CosmeticImage *FindCapeOverride(std::uint64_t playerKey);

/** Number of cape overrides currently registered. */
std::size_t CapeOverrideCount();

/** Replaces the pixels bound to a texture id (the in-memory texture swap). */
void SetTextureOverride(std::uint64_t textureId, const std::uint8_t *rgba,
                        std::uint32_t width, std::uint32_t height);

/** Clears every texture override. */
void ClearTextureOverrides();

/** The override pixels for a texture id, or nullptr when none. */
const CosmeticImage *FindTextureOverride(std::uint64_t textureId);

/** Number of texture overrides currently registered. */
std::size_t TextureOverrideCount();

/** Publishes the geometry blob the render hook should draw for the local player. */
void SetRenderGeometry(const std::uint8_t *data, std::size_t size);

/** The current render geometry size in bytes, or 0 when none is set. */
std::size_t RenderGeometrySize();

/** A cheap hash of the current render geometry, so a frame can detect a change. */
std::uint64_t RenderGeometryHash();

/**
 * @brief Installs the skin/cape and texture hooks.
 *
 * All fail-closed: an unresolved RTTI name or a failed hook install leaves that seam unavailable
 * and the launcher keeps the pack path. Safe to call once, from ``InitGameHooks``.
 */
void InitCosmeticsHooks(std::size_t skinCapeVtableIndex, std::size_t textureBindVtableIndex,
                        std::size_t packetReadVtableIndex);

/**
 * @brief True once the player-join packet hook has run at least once this session.
 *
 * The join packet (PlayerListPacket / AddPlayerPacket) is where a remote player's serialized skin
 * arrives; the hook is the seam where a Chimera cosmetic would be substituted before the engine
 * uploads it. Like the other seams it is a fail-closed passthrough.
 */
bool IsPacketHookLive();

/**
 * @brief The verified `SerializedSkinRef` layout, from the shipped 1.26.60.28 binary.
 *
 * Recovered from the accessor bodies: `getImageData()` returns `this + 0x78`,
 * `getCapeImageData()` returns `this + 0xa8`, and nothing sits between them — exactly one
 * `mce::Image`, so the type is 0x30 bytes. The offsets are exact; the internal field layout of
 * `mce::Image` is not recoverable from the stripped binary, which is why the swap copies a whole
 * struct rather than poking a buffer pointer.
 */
namespace skinlayout {
constexpr std::size_t kImageSize = 0x30;
constexpr std::size_t kImageData = 0x78;
constexpr std::size_t kCapeImageData = 0xa8;
constexpr std::size_t kAnimatedImageData = 0xd8;
} // namespace skinlayout

/**
 * @brief Replaces the cape image inside a `SerializedSkinRef` with a caller-supplied struct.
 *
 * **This is the real pixel substitution.** The cape the renderer samples lives in the `mce::Image`
 * at `ref + 0xa8`; writing that member is what changes the cape. Because the binary does not expose
 * `mce::Image`'s internal fields, the whole 0x30-byte struct is copied rather than a pointer poked
 * — the caller supplies a fully-formed image struct (built by the engine's own image loader, so its
 * internals are valid), and no assumption is made about where the buffer pointer sits inside it.
 *
 * Safety:
 *  - `ref == nullptr` is a no-op (a player with no skin yet).
 *  - `image == nullptr` is a no-op (nothing to swap in).
 *  - The destination is `ref + 0xa8` and exactly `kImageSize` bytes are written, so a struct that
 *    is larger on a future build cannot overrun into the next member.
 *
 * @return true when the struct was copied.
 */
bool SwapCapeImage(void *skinRef, const void *image);

/** True once the skin/cape hook has run at least once this session. */
bool IsSkinCapeHookLive();

/** True once the texture-bind hook has run at least once this session. */
bool IsTextureHookLive();

/**
 * @brief Reads the cosmetics hook call counts.
 *
 * @param out Receives {skinCapeCalls, textureCalls, capeOverrides, textureOverrides}.
 * @return true when at least one cosmetics hook has run.
 */
bool ReadCosmeticsStats(std::uint32_t out[4]);

} // namespace pl::runtime
