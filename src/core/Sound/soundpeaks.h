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

#ifndef SOUNDPEAKS_H
#define SOUNDPEAKS_H

#include <QVector>

#include "smartPointers/ememory.h"

class Samples;

// Shared absolute-peak extractor for timeline waveforms (and anything
// else that needs loudness columns). The per-second mixing math mirrors
// eIndependentSound::convertAudioToKeyframesAction; float samples only.
namespace SoundPeaks {
    // `buckets` amplitude columns (0..1) covering the whole second.
    // Returns an empty vector for non-float / empty input.
    CORE_EXPORT QVector<qreal> peaksForSecond(const stdsptr<Samples> &samples,
                                              const int buckets);
}

#endif // SOUNDPEAKS_H
