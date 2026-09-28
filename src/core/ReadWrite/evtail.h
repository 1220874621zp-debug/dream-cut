#ifndef EVTAIL_H
#define EVTAIL_H

#include <QByteArray>
#include <QIODevice>
#include <QtEndian>

#include <cstdint>

#include "exceptions.h"

// 工程单文件封装：封面 PNG 以自描述尾部块追加在 .dreamcut 的
// FileFooter 之后，工程目录不再出现 sidecar 图片。
// 布局（自流末尾向文件末尾）：[magic "DCT1" 4B][PNG 数据 N 字节][N quint32 小端]
// 读取侧先探测剥离尾部块再按旧逻辑解析流；旧版无尾部块的文件 probe
// 返回文件大小，一切照旧。
namespace EvTail {
    extern CORE_EXPORT const char sMagic[4];

    // 返回 ev 流（含 FileFooter）的结束偏移；无有效尾部块时 = src->size()。
    // thumbPng 非空则把探测到的封面 PNG 原始字节收入其中。
    // 校验失败（长度越界/magic 不符/非 PNG 头）一律按"无尾部块"处理，不抛。
    CORE_EXPORT qint64 probe(QIODevice* src, QByteArray* thumbPng = nullptr);

    // 打开工程文件只读并 probe 的便捷封装；失败返回 -1。
    CORE_EXPORT qint64 probePath(const QString& path, QByteArray* thumbPng = nullptr);

    // 在流末尾（FileFooter 之后）追加尾部块；png 为空返回 false 不写。
    CORE_EXPORT bool append(QIODevice* dst, const QByteArray& thumbPng);
}

#endif // EVTAIL_H
