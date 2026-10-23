#pragma once

#include <QImage>
#include <atomic>
#include <memory>

namespace ik {
struct WhiteBackgroundOptions {
    int tolerance = 20; // Distance from white in an 8-bit RGB channel.
    int softness = 16;
    bool connectedOnly = true;
};

// Returns a separate, straight-alpha image. A null result indicates cancellation
// or an allocation/size failure; the shared source pixels are never modified.
QImage removeWhiteBackground(const QImage &source, const WhiteBackgroundOptions &options,
                             const std::shared_ptr<std::atomic_bool> &cancelled = {});
}
