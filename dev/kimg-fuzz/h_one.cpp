#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtGui/QImage>
#include HANDLER_HEADER
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    QByteArray bytes(reinterpret_cast<const char*>(data), int(size));
    QBuffer b(&bytes); b.open(QIODevice::ReadOnly);
    HANDLER_CLASS *h = new HANDLER_CLASS;
    h->setDevice(&b);
    if (h->canRead()) { QImage img; h->read(&img); (void)img.width(); }
    delete h;
    return 0;
}
