/*
#
# Dream Cut - based on Friction
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
*/

#include "soundpeaks.h"

#include "CacheHandlers/samples.h"

#include <QtMath>

QVector<qreal> SoundPeaks::peaksForSecond(const stdsptr<Samples> &samples,
                                          const int buckets)
{
    if (!samples || buckets <= 0) { return {}; }
    if (samples->fSampleSize != 4) { return {}; } // float only
    const int sr = int(samples->fSampleRate);
    if (sr <= 0) { return {}; }
    const int nSamples = int(samples->fSampleRange.span());
    if (nSamples <= 0) { return {}; }
    QVector<qreal> peaks(buckets, 0.);
    const qreal samplesPerBucket = qreal(nSamples) / buckets;
    for (int b = 0; b < buckets; ++b) {
        const int i0 = qBound(0, qFloor(b * samplesPerBucket), nSamples);
        const int i1 = qBound(i0 + 1, qFloor((b + 1) * samplesPerBucket), nSamples);
        qreal peak = 0.;
        for (int i = i0; i < i1; ++i) {
            for (uint ch = 0; ch < samples->fNChannels; ch++) {
                float v;
                if (samples->fPlanar) {
                    v = reinterpret_cast<const float*>(
                                samples->fData[ch])[i];
                } else {
                    v = reinterpret_cast<const float*>(
                                samples->fData[0])[i*samples->fNChannels + ch];
                }
                const qreal a = qAbs(qreal(v));
                if (a > peak) { peak = a; }
            }
        }
        peaks[b] = qBound(0., peak, 1.);
    }
    return peaks;
}
