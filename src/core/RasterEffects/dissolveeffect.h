/*
#
# Friction - https://friction.graphics
#
# Copyright (c) Ole-André Rodlie and contributors.
#
# This program is free software: you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
*/

#ifndef DISSOLVEEFFECT_H
#define DISSOLVEEFFECT_H

#include "rastereffect.h"

// kdenlive "dissolve" (MLT luma without resource) ported to a single
// clip: alpha crossfade anchored to the clip head/tail. Fading the
// clip in over whatever sits below on the timeline IS the dissolve -
// the transparency reveals the previous clip during the window.
class DissolveEffect : public RasterEffect {
public:
    DissolveEffect();

    stdsptr<RasterEffectCaller> getEffectCaller(
            const qreal relFrame, const qreal resolution,
            const qreal influence, BoxRenderData * const data) const;
private:
    qsptr<QrealAnimator> mFadeIn;
    qsptr<QrealAnimator> mFadeOut;
};

#endif // DISSOLVEEFFECT_H
