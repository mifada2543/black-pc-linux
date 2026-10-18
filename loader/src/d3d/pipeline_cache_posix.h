// Pipeline cache: turns (vertex format + shaders + render state) -> VkPipeline.
// This is the A4 bridge: the game sets FVF/vertex shader/pixel shader + render state,
// and a draw call needs the right graphics pipeline. The cache keys on everything
// that affects the pipeline and reuses identical pipelines.
//
// FVF (fixed-function vertex format) convention: handle bit 0 == 0.
// Vertex shader convention: handle bit 0 == 1 (from vertex_program_create).
#pragma once

#include "d3d/vertex_posix.h"
#include "gfx/gfx_vulkan.h"
#include "shader/nv2a_psh_posix.h"
#include "shader/nv2a_vsh_posix.h"

#include <cstdint>
#include <vector>

namespace xd3d {

// Pipeline key: everything that makes a pipeline unique.
// Two draws with the same key can share the exact same VkPipeline.
struct PipelineKey {
    // Vertex side
    uint32_t fvf_code = 0;              // FVF code (when fixed-function, bit 0 of handle == 0)
    uint32_t vertex_shader_handle = 0;  // vertex_program handle (when shader, bit 0 == 1)
    VertexLayout vertex_layout = {};    // computed from declaration + stream strides
    uint32_t vertex_input_mask = 0;     // which vertex registers are fed

    // Pixel side
    uint32_t pixel_shader_handle = 0;   // pixel shader handle from CreatePixelShader
    uint32_t tex_cube_mask = 0;         // which stages sample cube maps

    // Render state that reaches the pipeline (not the shader)
    uint32_t pipeline_flags = 0;        // blend, depth, stencil, cull, etc.
    uint32_t color_format = 0;          // gfx::VertexFormatCode
    uint32_t depth_format = 0;          // gfx::VertexFormatCode
    uint32_t sample_count = 1;

    bool operator==(const PipelineKey &other) const {
        return fvf_code == other.fvf_code &&
               vertex_shader_handle == other.vertex_shader_handle &&
               vertex_layout.attribute_count == other.vertex_layout.attribute_count &&
               vertex_layout.binding_count == other.vertex_layout.binding_count &&
               vertex_input_mask == other.vertex_input_mask &&
               pixel_shader_handle == other.pixel_shader_handle &&
               tex_cube_mask == other.tex_cube_mask &&
               pipeline_flags == other.pipeline_flags &&
               color_format == other.color_format &&
               depth_format == other.depth_format &&
               sample_count == other.sample_count;
    }
};

// Hash for unordered_map
struct PipelineKeyHash {
    size_t operator()(const PipelineKey &k) const noexcept {
        size_t h = k.fvf_code;
        h = h * 31 + k.vertex_shader_handle;
        h = h * 31 + k.vertex_layout.attribute_count;
        h = h * 31 + k.vertex_layout.binding_count;
        // Hash vertex layout attributes
        for (uint32_t i = 0; i < k.vertex_layout.attribute_count; i++) {
            const auto &a = k.vertex_layout.attributes[i];
            h = h * 31 + a.binding;
            h = h * 31 + a.location;
            h = h * 31 + a.format;
            h = h * 31 + a.offset;
        }
        for (uint32_t i = 0; i < k.vertex_layout.binding_count; i++) {
            const auto &b = k.vertex_layout.bindings[i];
            h = h * 31 + b.binding;
            h = h * 31 + b.stride;
            h = h * 31 + b.rate;
        }
        h = h * 31 + k.vertex_input_mask;
        h = h * 31 + k.pixel_shader_handle;
        h = h * 31 + k.tex_cube_mask;
        h = h * 31 + k.pipeline_flags;
        h = h * 31 + k.color_format;
        h = h * 31 + k.depth_format;
        h = h * 31 + k.sample_count;
        return h;
    }
};

// Pipeline cache entry
struct PipelineEntry {
    gfx::Pipeline pipeline = {};
    bool valid = false;
    uint64_t last_used_frame = 0;
};

// The pipeline cache
class PipelineCache {
public:
    PipelineCache() = default;
    ~PipelineCache() { clear(); }

    // Get or create a pipeline for the given key.
    // Returns nullptr with err set on failure.
    gfx::Pipeline *get_or_create(const PipelineKey &key, const char **err);

    // Invalidate all pipelines (e.g., on device lost / swapchain recreate)
    void clear();

    // Called each frame to track usage for potential LRU eviction
    void mark_frame(uint64_t frame) { current_frame_ = frame; }

    // Stats
    size_t size() const { return cache_.size(); }
    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }

private:
    struct CacheEntry {
        gfx::Pipeline pipeline;
        bool valid = false;
        uint64_t last_used_frame = 0;
    };

    // Simple linear-probe hash map for deterministic iteration and no allocator deps
    struct HashMap {
        static constexpr size_t INITIAL_BUCKETS = 256;
        struct Bucket {
            PipelineKey key;
            CacheEntry entry;
            bool occupied = false;
        };
        std::vector<Bucket> buckets;
        size_t count = 0;

        HashMap() : buckets(INITIAL_BUCKETS) {}

        Bucket *find(const PipelineKey &key) {
            size_t idx = PipelineKeyHash{}(key) % buckets.size();
            for (size_t i = 0; i < buckets.size(); i++) {
                size_t idx2 = (idx + i) % buckets.size();
                Bucket &b = buckets[idx2];
                if (!b.occupied) return nullptr;
                if (b.key == key) return &b;
            }
            return nullptr;
        }

        Bucket *insert(const PipelineKey &key) {
            if (count * 2 >= buckets.size()) grow();
            size_t idx = PipelineKeyHash{}(key) % buckets.size();
            for (size_t i = 0; i < buckets.size(); i++) {
                size_t idx2 = (idx + i) % buckets.size();
                Bucket &b = buckets[idx2];
                if (!b.occupied) {
                    b.occupied = true;
                    b.key = key;
                    count++;
                    return &b;
                }
            }
            return nullptr; // should not happen
        }

        void grow() {
            std::vector<Bucket> old = std::move(buckets);
            buckets.resize(buckets.size() * 2);
            count = 0;
            for (auto &b : old) {
                if (b.occupied) {
                    *insert(b.key) = b;
                }
            }
        }

        void clear() {
            for (auto &b : buckets) {
                if (b.occupied) {
                    if (b.entry.valid) {
                        destroy_pipeline(&b.entry.pipeline);
                    }
                    b.occupied = false;
                }
            }
            count = 0;
        }

        size_t size() const { return count; }
    };

    HashMap cache_;
    uint64_t current_frame_ = 0;
    size_t hits_ = 0;
    size_t misses_ = 0;
};

// Global cache instance
PipelineCache &pipeline_cache();

// Build PipelineKey from current device state + shaders.
// Used by draw path when a draw call is issued.
PipelineKey build_pipeline_key(
    uint32_t fvf_code,
    uint32_t vertex_shader_handle,
    const VertexLayout *vertex_layout,
    uint32_t vertex_input_mask,
    uint32_t pixel_shader_handle,
    uint32_t tex_cube_mask,
    uint32_t pipeline_flags,
    uint32_t color_format,
    uint32_t depth_format,
    uint32_t sample_count);

} // namespace xd3d