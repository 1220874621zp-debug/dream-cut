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

#include "transitionanchor.h"
#include "Boxes/boxrenderdata.h"
#include "Boxes/boundingbox.h"
#include "Timeline/durationrectangle.h"

qreal nleTransitionClipRelFrame(const qreal relFrame,
                                const BoxRenderData * const data)
{
    if (data) {
        const auto box = data->fParentBox.data();
        if (box) {
            const auto dur = box->getDurationRectangle();
            if (dur) {
                return relFrame - dur->getMinRelFrame();
            }
        }
    }
    return relFrame;
}

qreal nleTransitionTotalFrames(const BoxRenderData * const data)
{
    if (data) {
        const auto box = data->fParentBox.data();
        if (box) {
            const auto dur = box->getDurationRectangle();
            if (dur) {
                const qreal total = dur->getMaxRelFrame() -
                                    dur->getMinRelFrame() + 1;
                if (total >= 1.) { return total; }
            }
        }
    }
    // preview fallback: the effect-preview loop plays transitions on
    // a 4-second cycle (gPreviewLoopSec * gPreviewFps * 2) so the
    // in/hold/out phases read clearly; the real pipeline never hits
    // this branch (data is always present there)
    return 96.;
}
