#include "nativeimage.h"
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <limits>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreServices/CoreServices.h>
#include <ImageIO/ImageIO.h>
#endif

namespace ik {
#ifdef Q_OS_MACOS
namespace {
template<class T> struct CFReleaseGuard {
    T value = nullptr;
    explicit CFReleaseGuard(T pointer) : value(pointer) {}
    ~CFReleaseGuard() { if (value) CFRelease(value); }
    CFReleaseGuard(const CFReleaseGuard &) = delete;
    CFReleaseGuard &operator=(const CFReleaseGuard &) = delete;
    operator T() const { return value; }
};
QString stringValue(CFStringRef value) {
    if (!value) return {};
    const CFIndex length = CFStringGetLength(value);
    QString result(length, Qt::Uninitialized);
    CFStringGetCharacters(value, CFRangeMake(0, length), reinterpret_cast<UniChar *>(result.data()));
    return result;
}
double number(CFDictionaryRef dictionary, CFStringRef key, double fallback = 0) {
    if (!dictionary) return fallback;
    auto value = CFDictionaryGetValue(dictionary, key);
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return fallback;
    double result = fallback;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &result);
    return result;
}
QString text(CFDictionaryRef dictionary, CFStringRef key) {
    if (!dictionary) return {};
    auto value = CFDictionaryGetValue(dictionary, key);
    return value && CFGetTypeID(value) == CFStringGetTypeID() ? stringValue(static_cast<CFStringRef>(value)) : QString();
}
CFDictionaryRef dictionary(CFDictionaryRef properties, CFStringRef key) {
    auto value = CFDictionaryGetValue(properties, key);
    return value && CFGetTypeID(value) == CFDictionaryGetTypeID() ? static_cast<CFDictionaryRef>(value) : nullptr;
}
NativeDecoded decodePdf(const QString &path, const QByteArray &encoded, int page, QSize maximumRasterSize) {
    NativeDecoded result;
    result.pdf = true;
    result.format = QStringLiteral("PDF");
    CGPDFDocumentRef raw = nullptr;
    if (!encoded.isEmpty()) {
        // encoded outlives both the document and its provider. Avoid another
        // potentially 256 MiB allocation for an archived PDF.
        CFReleaseGuard<CFDataRef> data(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
            reinterpret_cast<const UInt8 *>(encoded.constData()), encoded.size(), kCFAllocatorNull));
        CFReleaseGuard<CGDataProviderRef> provider(data.value ? CGDataProviderCreateWithCFData(data) : nullptr);
        if (provider.value) raw = CGPDFDocumentCreateWithProvider(provider);
    } else {
        constexpr qint64 maximumFileBytes = 4LL * 1024 * 1024 * 1024;
        if (QFileInfo(path).size() > maximumFileBytes) {
            result.error = QStringLiteral("PDF 파일이 입력 한도 4 GiB를 초과합니다."); return result;
        }
        const auto name = QFile::encodeName(path);
        CFReleaseGuard<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
            reinterpret_cast<const UInt8 *>(name.constData()), name.size(), false));
        if (url.value) raw = CGPDFDocumentCreateWithURL(url);
    }
    CFReleaseGuard<CGPDFDocumentRef> document(raw);
    if (!document.value) {
        result.error = QStringLiteral("PDF 문서를 열 수 없습니다. 파일이 손상되었거나 접근할 수 없습니다."); return result;
    }
    if (CGPDFDocumentIsEncrypted(document) && !CGPDFDocumentIsUnlocked(document)
        && !CGPDFDocumentUnlockWithPassword(document, "")) {
        result.error = QStringLiteral("암호로 보호된 PDF입니다. 암호를 해제한 문서를 열어 주세요."); return result;
    }
    const auto count = CGPDFDocumentGetNumberOfPages(document);
    if (!count || count > size_t(std::numeric_limits<int>::max())) {
        result.error = QStringLiteral("PDF에 표시할 수 있는 페이지가 없습니다."); return result;
    }
    result.pageCount = int(count);
    if (page < 0 || page >= result.pageCount) {
        result.error = QStringLiteral("요청한 페이지 번호가 PDF의 페이지 범위를 벗어났습니다."); return result;
    }
    CGPDFPageRef pdfPage = CGPDFDocumentGetPage(document, size_t(page + 1));
    if (!pdfPage) { result.error = QStringLiteral("PDF 페이지를 읽을 수 없습니다."); return result; }
    const CGRect crop = CGPDFPageGetBoxRect(pdfPage, kCGPDFCropBox);
    double width = crop.size.width, height = crop.size.height;
    const auto rotation = ((CGPDFPageGetRotationAngle(pdfPage) % 360) + 360) % 360;
    if (rotation == 90 || rotation == 270) std::swap(width, height);
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
        result.error = QStringLiteral("PDF 페이지 크기가 올바르지 않습니다."); return result;
    }
    result.pageSizePoints = QSizeF(width, height);
    // Render one page at 144 DPI. Bound unusual CAD/poster pages by both area
    // and edge length before allocating; normal Retina pages remain detailed.
    constexpr double maximumPixels = 16.0 * 1024 * 1024;
    constexpr double maximumEdge = 8192;
    double scale = std::min({2.0, std::sqrt(maximumPixels / width / height), maximumEdge / width, maximumEdge / height});
    if (maximumRasterSize.width() > 0 && maximumRasterSize.height() > 0)
        scale = std::min({scale, maximumRasterSize.width() / width, maximumRasterSize.height() / height});
    const int rasterWidth = std::max(1, int(std::floor(width * scale)));
    const int rasterHeight = std::max(1, int(std::floor(height * scale)));
    QImage pixels(rasterWidth, rasterHeight, QImage::Format_RGBA8888_Premultiplied);
    if (pixels.isNull()) { result.error = QStringLiteral("PDF 페이지를 표시할 메모리가 부족합니다."); return result; }
    CFReleaseGuard<CGColorSpaceRef> colorSpace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    CFReleaseGuard<CGContextRef> context(CGBitmapContextCreate(pixels.bits(), rasterWidth, rasterHeight, 8, pixels.bytesPerLine(),
        colorSpace, CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big)));
    if (!context.value) { result.error = QStringLiteral("PDF 페이지 렌더러를 만들 수 없습니다."); return result; }
    const CGRect target = CGRectMake(0, 0, rasterWidth, rasterHeight);
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, target);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextSetShouldAntialias(context, true);
    // Quartz's bitmap device transform already maps its bottom-left page
    // coordinates to top-first scanlines. A second vertical flip would invert
    // PDF contents (including text) when QImage presents those scanlines.
    // GetDrawingTransform fits large pages down but does not enlarge a small
    // page. Scale points to pixels explicitly to get a true 144 DPI raster.
    CGContextScaleCTM(context, rasterWidth / width, rasterHeight / height);
    CGContextConcatCTM(context, CGPDFPageGetDrawingTransform(pdfPage, kCGPDFCropBox, CGRectMake(0, 0, width, height), 0, true));
    CGContextDrawPDFPage(context, pdfPage);
    pixels.setColorSpace(QColorSpace::SRgb);
    pixels.setDotsPerMeterX(int(std::round((rasterWidth / width) * 72 / 0.0254)));
    pixels.setDotsPerMeterY(int(std::round((rasterHeight / height) * 72 / 0.0254)));
    result.image = pixels.convertToFormat(QImage::Format_RGBA8888);
    result.metadata.append(QStringLiteral("PDF.PageSize: %1 × %2 pt").arg(width, 0, 'f', 2).arg(height, 0, 'f', 2));
    result.metadata.append(QStringLiteral("PDF.RenderDPI: %1").arg(scale * 72, 0, 'f', 1));
    int major = 0, minor = 0; CGPDFDocumentGetVersion(document, &major, &minor);
    result.metadata.append(QStringLiteral("PDF.Version: %1.%2").arg(major).arg(minor));
    if (const auto info = CGPDFDocumentGetInfo(document)) {
        for (const char *key : {"Title", "Author", "Subject", "Creator", "Producer", "CreationDate"}) {
            CGPDFStringRef value = nullptr;
            if (!CGPDFDictionaryGetString(info, key, &value)) continue;
            CFReleaseGuard<CFStringRef> string(CGPDFStringCopyTextString(value));
            const auto valueText = stringValue(string).left(4096);
            if (!valueText.isEmpty()) result.metadata.append(QStringLiteral("PDF.") + QString::fromLatin1(key) + ": " + valueText);
        }
    }
    return result;
}
}
#endif
bool isNativePdfSource(const QString &path, const QByteArray &encoded) {
#ifdef Q_OS_MACOS
    if (QFileInfo(path).suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) return true;
    if (!encoded.isEmpty()) return encoded.first(1024).trimmed().startsWith("%PDF-");
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(1024).trimmed().startsWith("%PDF-");
#else
    Q_UNUSED(path)
    Q_UNUSED(encoded)
    return false;
#endif
}
QStringList nativeImageExtensions() {
    QStringList result;
#ifdef Q_OS_MACOS
    result.append(QStringLiteral("pdf"));
    CFReleaseGuard<CFArrayRef> types(CGImageSourceCopyTypeIdentifiers());
    if (!types.value) return result;
    for (CFIndex i = 0; i < CFArrayGetCount(types); ++i) {
        auto type = static_cast<CFStringRef>(CFArrayGetValueAtIndex(types, i));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CFReleaseGuard<CFArrayRef> tags(UTTypeCopyAllTagsWithClass(type, kUTTagClassFilenameExtension));
#pragma clang diagnostic pop
        if (!tags.value) continue;
        for (CFIndex j = 0; j < CFArrayGetCount(tags); ++j)
            result.append(stringValue(static_cast<CFStringRef>(CFArrayGetValueAtIndex(tags, j))).toLower());
    }
    result.removeDuplicates();
#endif
    return result;
}
NativeDecoded decodeNativeImage(const QString &path, const QByteArray &encoded, int page, QSize maximumRasterSize) {
    NativeDecoded result;
#ifdef Q_OS_MACOS
    if (isNativePdfSource(path, encoded)) return decodePdf(path, encoded, page, maximumRasterSize);
    CGImageSourceRef rawSource = nullptr;
    if (!encoded.isEmpty()) {
        CFReleaseGuard<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(encoded.constData()), encoded.size()));
        if (data.value) rawSource = CGImageSourceCreateWithData(data, nullptr);
    } else {
        const QByteArray filename = QFile::encodeName(path);
        CFReleaseGuard<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
            reinterpret_cast<const UInt8 *>(filename.constData()), filename.size(), false));
        if (url.value) rawSource = CGImageSourceCreateWithURL(url, nullptr);
    }
    CFReleaseGuard<CGImageSourceRef> source(rawSource);
    if (!source.value) { result.error = QStringLiteral("지원하지 않는 이미지 형식이거나 파일이 손상되었습니다."); return result; }
    result.pageCount = int(CGImageSourceGetCount(source));
    if (result.pageCount < 1) { result.error = QStringLiteral("지원하지 않는 이미지 형식이거나 파일이 손상되었습니다."); return result; }
    if (page < 0 || page >= result.pageCount) { result.error = QStringLiteral("요청한 페이지 번호가 이미지의 페이지 범위를 벗어났습니다."); return result; }
    CFReleaseGuard<CFDictionaryRef> properties(CGImageSourceCopyPropertiesAtIndex(source, page, nullptr));
    if (properties.value) {
        const double width = number(properties, kCGImagePropertyPixelWidth);
        const double height = number(properties, kCGImagePropertyPixelHeight);
        if (width <= 0 || height <= 0 || width * height > 128.0 * 1024 * 1024) {
            result.error = QStringLiteral("이미지가 할당 한도 128 메가픽셀을 초과합니다."); return result;
        }
    }
    CFReleaseGuard<CGImageRef> image(CGImageSourceCreateImageAtIndex(source, page, nullptr));
    if (!image.value) { result.error = QStringLiteral("이미지를 디코딩할 수 없습니다. 파일 손상 여부를 확인하세요."); return result; }
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0 || width * height > 128ULL * 1024 * 1024) {
        result.error = QStringLiteral("이미지가 할당 한도 128 메가픽셀을 초과합니다."); return result;
    }
    QImage pixels(int(width), int(height), QImage::Format_RGBA8888_Premultiplied);
    if (pixels.isNull()) { result.error = QStringLiteral("이미지를 디코딩할 메모리가 부족합니다."); return result; }
    CFReleaseGuard<CGColorSpaceRef> colorSpace(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    CFReleaseGuard<CGContextRef> context(CGBitmapContextCreate(pixels.bits(), width, height, 8, pixels.bytesPerLine(),
        colorSpace, CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big)));
    if (!context.value) return result;
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    pixels.setColorSpace(QColorSpace::SRgb);
    const int orientation = int(number(properties, kCGImagePropertyOrientation, 1));
    QTransform transform;
    switch (orientation) {
        case 2: transform.scale(-1, 1); break;
        case 3: transform.rotate(180); break;
        case 4: transform.scale(1, -1); break;
        case 5: transform = QTransform(0, 1, 1, 0, 0, 0); break;
        case 6: transform.rotate(90); break;
        case 7: transform = QTransform(0, -1, -1, 0, 0, 0); break;
        case 8: transform.rotate(270); break;
        default: break;
    }
    if (!transform.isIdentity()) pixels = pixels.transformed(transform);
    result.image = pixels.convertToFormat(QImage::Format_RGBA8888);
    result.format = stringValue(CGImageSourceGetType(source));
    if (properties.value) {
        const CFDictionaryRef tiff = dictionary(properties, kCGImagePropertyTIFFDictionary);
        const QString make = text(tiff, kCGImagePropertyTIFFMake);
        const QString model = text(tiff, kCGImagePropertyTIFFModel);
        if (!make.isEmpty()) result.metadata.append(QStringLiteral("Camera.Make: ") + make);
        if (!model.isEmpty()) result.metadata.append(QStringLiteral("Camera.Model: ") + model);
        const CFDictionaryRef exif = dictionary(properties, kCGImagePropertyExifDictionary);
        const QString timestamp = text(exif, kCGImagePropertyExifDateTimeOriginal);
        if (!timestamp.isEmpty()) result.metadata.append(QStringLiteral("DateTimeOriginal: ") + timestamp);
        const CFDictionaryRef gps = dictionary(properties, kCGImagePropertyGPSDictionary);
        if (gps) {
            double latitude = number(gps, kCGImagePropertyGPSLatitude, 999);
            double longitude = number(gps, kCGImagePropertyGPSLongitude, 999);
            if (text(gps, kCGImagePropertyGPSLatitudeRef) == QStringLiteral("S")) latitude = -latitude;
            if (text(gps, kCGImagePropertyGPSLongitudeRef) == QStringLiteral("W")) longitude = -longitude;
            if (latitude >= -90 && latitude <= 90 && longitude >= -180 && longitude <= 180) {
                result.metadata.append(QStringLiteral("GPS.Latitude: ") + QString::number(latitude, 'f', 8));
                result.metadata.append(QStringLiteral("GPS.Longitude: ") + QString::number(longitude, 'f', 8));
            }
        }
    }
#else
    Q_UNUSED(path)
    Q_UNUSED(encoded)
    Q_UNUSED(page)
    Q_UNUSED(maximumRasterSize)
#endif
    return result;
}
}
