#ifndef OPENMW_SDLUTIL_WINDOWCOORDINATES_H
#define OPENMW_SDLUTIL_WINDOWCOORDINATES_H

#include <algorithm>
#include <cmath>

namespace SDLUtil
{
    // SDL mouse/window coordinates are logical; OSG and MyGUI consume drawable pixels.
    inline float drawableScale(int windowSize, int drawableSize)
    {
        return windowSize > 0 && drawableSize > 0 ? static_cast<float>(drawableSize) / windowSize : 1.f;
    }

    inline int drawableToWindowSize(int pixels, int windowSize, int drawableSize)
    {
        return std::max(1, static_cast<int>(std::lround(pixels / drawableScale(windowSize, drawableSize))));
    }
}
#endif
