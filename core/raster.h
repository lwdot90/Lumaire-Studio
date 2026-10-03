#pragma once
#include "core/tiles.h"
#include <string>

namespace compositor::engine {
class Id {
public:
    static Id generate();
    explicit Id(std::string text);
    const std::string& text() const { return text_; }
    bool operator==(const Id&) const = default;
private:
    std::string text_;
};
struct SourceProfile {
    Id id;
    std::vector<std::uint8_t> bytes;
};

// RasterSnapshot.swift: immutable replacement tiles share untouched storage.
class RasterSnapshot {
public:
    RasterSnapshot(Id id, Extent extent, PackedPixel defaultValue={}, TileMap tiles={}, std::int64_t revision=0,
                   std::shared_ptr<const SourceProfile> sourceProfile={});
    const Id id;
    const Extent extent;
    const PackedPixel defaultValue;
    const TileMap tiles;
    const std::int64_t revision;
    const std::shared_ptr<const SourceProfile> sourceProfile;
    Pixel pixel(std::int64_t x, std::int64_t y) const;
};
}
