// Headless ASan harness for the vendored kimageformats wrappers.
// Drives each QImageIOHandler::read() directly on attacker bytes — the same
// call Qt's plugin system makes from App::readImage / QImageReader.
#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtGui/QImage>
#include <QtGui/QImageReader>
#include <cstdio>
#include <cstring>

#include "qoi_p.h"
#include "avif_p.h"
#include "heif_p.h"
#include "jxl_p.h"

static void tryOne(QImageIOHandler *h, QIODevice *dev) {
    h->setDevice(dev);
    if (h->canRead()) {
        QImage img;
        h->read(&img);           // decode; ignore result — we want crashes
        (void)img.width();
    }
    delete h;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    QByteArray bytes(reinterpret_cast<const char*>(data), int(size));
    // QOI
    { QBuffer b(&bytes); b.open(QIODevice::ReadOnly); tryOne(new QOIHandler, &b); }
    { QBuffer b(&bytes); b.open(QIODevice::ReadOnly); tryOne(new QAVIFHandler, &b); }
    { QBuffer b(&bytes); b.open(QIODevice::ReadOnly); tryOne(new HEIFHandler, &b); }
    { QBuffer b(&bytes); b.open(QIODevice::ReadOnly); tryOne(new QJpegXLHandler, &b); }
    return 0;
}
