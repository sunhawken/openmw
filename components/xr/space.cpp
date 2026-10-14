#include "debug.hpp"
#include "session.hpp"
#include "space.hpp"

#include <components/xr/instance.hpp>

#include <cassert>

namespace XR
{
    namespace
    {
        // The session (or the instance) destroyed with it already took the space with it.
        bool spaceStillOwned(XrSpace space)
        {
            return space != XR_NULL_HANDLE && !Session::xrSessionDestroyed() && !Instance::xrInstanceDestroyed();
        }
    }

    XrSpaceWrapper::~XrSpaceWrapper()
    {
        if (spaceStillOwned(mSpace))
            CHECK_XRCMD(xrDestroySpace(mSpace));
    }

    void XrSpaceWrapper::operator=(XrSpace space)
    {
        if (spaceStillOwned(mSpace))
            CHECK_XRCMD(xrDestroySpace(mSpace));
        mSpace = space;
    }

    Space::Space(XrSpace space)
        : mSpace(space)
    {

    }

    Space::~Space()
    {
    }

    VR::TrackingPose Space::locate(VR::Space& reference)
    {
        return XR::Session::instance().locateSpace(xrSpace(), dynamic_cast<XR::Space&>(reference).xrSpace());
    }

    VR::TrackingPose Space::locateInWorld()
    {
        return XR::Session::instance().locateSpaceInWorld(xrSpace());
    }

    ReferenceSpace::ReferenceSpace(VR::ReferenceSpace type, std::optional<VR::ReferenceSpace> recenterTo)
        : Space(XR_NULL_HANDLE)
        , mType(type)
        , mRecenterTo(recenterTo)
        , mRecenter(true)
    {
    }

    ReferenceSpace::~ReferenceSpace()
    {
    }

    XrSpace ReferenceSpace::xrSpace()
    {
        if (!mSpace.mSpace)
            recreate();
        return mSpace.mSpace;
    }

    void ReferenceSpace::recreate()
    {
        recenter(true, true, true);
    }

    void ReferenceSpace::recenter(bool recenterX, bool recenterY, bool recenterZ) 
    {
        auto& session = Session::instance();
        mSpace = session.createReferenceSpace(mType, {});
        if (mRecenterTo && mRecenterTo != mType)
        {
            auto other = session.getReferenceSpace(mRecenterTo.value());
            auto tp = other->locate(*this);
            if (!!tp.status)
            {
                if (recenterX)
                    mCenter.position.mX = tp.pose.position.mX;
                if (recenterY)
                    mCenter.position.mY = tp.pose.position.mY;
                if (recenterZ)
                    mCenter.position.mZ = tp.pose.position.mZ;
                mSpace = session.createReferenceSpace(mType, mCenter);
            }
        }
        mRecenter = false;
    }

}
