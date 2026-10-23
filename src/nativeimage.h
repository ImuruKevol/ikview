#pragma once
#include <QByteArray>
#include <QImage>
#include <QSizeF>
#include <QStringList>
namespace ik {
struct NativeDecoded {
    QImage image;
    QString format;
    QStringList metadata;
    int pageCount = 1;
    bool pdf = false;
    QSizeF pageSizePoints;
    QString error;
};
// Uses the platform decoder only when Qt has no decoder for a source.
QStringList nativeImageExtensions();
// Detects PDF by both extension and its bounded header, including renamed PDFs.
// Each decode opens an independent CGPDFDocument, so workers never share pages.
bool isNativePdfSource(const QString &path, const QByteArray &encoded = {});
// maximumRasterSize limits PDF rasterization directly (e.g. list thumbnails),
// instead of allocating the full page and then scaling it down.
NativeDecoded decodeNativeImage(const QString &path, const QByteArray &encoded, int page,
                                QSize maximumRasterSize = {});
}
