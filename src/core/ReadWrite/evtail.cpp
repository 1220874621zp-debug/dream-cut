#include "evtail.h"

#include <QFile>

#include <cstring>

namespace EvTail {

const char sMagic[4] = {'D', 'C', 'T', '1'};

qint64 probe(QIODevice* src, QByteArray* thumbPng)
{
    if (thumbPng) { thumbPng->clear(); }
    const qint64 size = src->size();
    // magic 4B + 长度 4B 是最小尾部块
    if (size < 8) { return size; }

    const qint64 savedPos = src->pos();

    quint32 nRaw = 0;
    if (!src->seek(size - 4) ||
        src->read(reinterpret_cast<char*>(&nRaw), 4) != 4) {
        src->seek(savedPos);
        return size;
    }
    const qint64 n = qFromLittleEndian<quint32>(&nRaw);
    if (n < 8 || n > size - 8) { src->seek(savedPos); return size; }

    const qint64 magicPos = size - 8 - n;
    if (!src->seek(magicPos)) { src->seek(savedPos); return size; }
    char magic[4];
    if (src->read(magic, 4) != 4 ||
        std::memcmp(magic, sMagic, 4) != 0) {
        src->seek(savedPos);
        return size;
    }

    if (thumbPng) {
        // PNG 魔数前三字节做内容 sanity，防随机数据凑巧通过长度校验
        *thumbPng = src->read(n);
        if (thumbPng->size() != static_cast<int>(n) ||
            static_cast<quint8>(thumbPng->at(0)) != 0x89 ||
            thumbPng->at(1) != 'P' || thumbPng->at(2) != 'N') {
            thumbPng->clear();
        }
    }

    src->seek(savedPos);
    return magicPos;
}

qint64 probePath(const QString& path, QByteArray* thumbPng)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { return -1; }
    const qint64 streamEnd = probe(&file, thumbPng);
    file.close();
    return streamEnd;
}

bool append(QIODevice* dst, const QByteArray& thumbPng)
{
    if (thumbPng.isEmpty()) { return false; }

    char lenRaw[4];
    qToLittleEndian(static_cast<quint32>(thumbPng.size()),
                    reinterpret_cast<uchar*>(lenRaw));
    if (dst->write(sMagic, 4) != 4) { return false; }
    if (dst->write(thumbPng.constData(), thumbPng.size())
            != thumbPng.size()) {
        return false;
    }
    return dst->write(lenRaw, 4) == 4;
}

} // namespace EvTail
