#include "imagebackground.h"
#include <QBitArray>
#include <QVector>
#include <algorithm>

namespace ik {
QImage removeWhiteBackground(const QImage &source, const WhiteBackgroundOptions &options,
                             const std::shared_ptr<std::atomic_bool> &cancelled) {
    const auto stopped = [&] { return cancelled && cancelled->load(std::memory_order_relaxed); };
    if (source.isNull() || stopped() || qint64(source.width()) * source.height() > 128LL * 1024 * 1024) return {};
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) return {};
    const int width = image.width(), height = image.height();
    const int tolerance = std::clamp(options.tolerance, 0, 100);
    const int softness = std::clamp(options.softness, 0, 100);
    const int limit = tolerance + softness;
    const auto distance = [](QRgb pixel) { return 255 - std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)}); };
    const auto candidate = [&](int x, int y) {
        const auto pixel = reinterpret_cast<const QRgb *>(image.constScanLine(y))[x];
        return !qAlpha(pixel) || distance(pixel) <= limit;
    };
    QBitArray connected;
    if (options.connectedOnly) {
        connected.resize(qsizetype(width) * height);
        struct Span { int left, right, y; };
        QVector<Span> pending;
        const auto visited = [&](int x, int y) { return connected.testBit(qsizetype(y) * width + x); };
        const auto seed = [&](int x, int y) {
            if (visited(x, y) || !candidate(x, y)) return x;
            int left = x, right = x;
            while (left > 0 && !visited(left - 1, y) && candidate(left - 1, y)) --left;
            while (right + 1 < width && !visited(right + 1, y) && candidate(right + 1, y)) ++right;
            connected.fill(true, qsizetype(y) * width + left, qsizetype(y) * width + right + 1);
            pending.append({left, right, y});
            return right;
        };
        for (int x = 0; x < width; ++x) { if (stopped()) return {}; x = seed(x, 0); }
        if (height > 1) for (int x = 0; x < width; ++x) { if (stopped()) return {}; x = seed(x, height - 1); }
        for (int y = 1; y + 1 < height; ++y) { if (stopped()) return {}; seed(0, y); if (width > 1) seed(width - 1, y); }
        // One visited bit per pixel and spans rather than a pixel queue.
        while (!pending.isEmpty()) {
            if (stopped()) return {};
            const auto span = pending.takeLast();
            for (const int y : {span.y - 1, span.y + 1}) {
                if (y < 0 || y >= height) continue;
                for (int x = span.left; x <= span.right; ++x) x = seed(x, y);
            }
        }
    }
    // Detach after the read-only flood fill; never modify the open document.
    if (!image.bits()) return {};
    for (int y = 0; y < height; ++y) {
        if (stopped()) return {};
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            if (options.connectedOnly && !connected.testBit(qsizetype(y) * width + x)) continue;
            const QRgb pixel = row[x]; const int delta = distance(pixel);
            if (!qAlpha(pixel) || delta <= tolerance) { row[x] = 0; continue; }
            if (!softness || delta >= limit) continue;
            const int coverage = (delta - tolerance) * 255 / softness;
            const int alpha = qAlpha(pixel) * coverage / 255;
            if (!alpha) { row[x] = 0; continue; }
            // Remove the white matte to avoid bright fringes on dark surfaces.
            // Existing alpha never increases.
            const auto unmatte = [coverage](int channel) {
                return std::clamp((channel * 255 - 255 * (255 - coverage) + coverage / 2) / coverage, 0, 255);
            };
            row[x] = qRgba(unmatte(qRed(pixel)), unmatte(qGreen(pixel)), unmatte(qBlue(pixel)), alpha);
        }
    }
    return stopped() ? QImage() : image;
}
}
