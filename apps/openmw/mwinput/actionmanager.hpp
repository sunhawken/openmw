#ifndef MWINPUT_ACTIONMANAGER_H
#define MWINPUT_ACTIONMANAGER_H

#include <osg/ref_ptr>
#include <osg/Vec3f>
#include <osgViewer/ViewerEventHandlers>

namespace osgViewer
{
    class Viewer;
    class ScreenCaptureHandler;
}

namespace MWInput
{
    class BindingsManager;

    class ActionManager
    {
    public:
        ActionManager(BindingsManager* bindingsManager, osg::ref_ptr<osgViewer::Viewer> viewer,
            osg::ref_ptr<osgViewer::ScreenCaptureHandler> screenCaptureHandler);

        void update(float dt);

        void executeAction(int action);

        bool checkAllowedToUseItems() const;

        void toggleMainMenu();
        void toggleConsole();
        void screenshot();
        void activate();
        void rest();
        void quickLoad();
        void quickSave();

        void quickKey(int index);

        void resetIdleTime();
        float getIdleTime() const { return mTimeIdle; }

        bool isSneaking() const;

        // Object grab system (Oblivion/Skyrim style)
        void onActivatePressed();
        void onActivateReleased();
        void updateGrabbedObject(float dt);
        bool isHoldingActivate() const { return mActivateHeld; }

    private:
        void handleGuiArrowKey(int action);
        void tryGrabObject();

        BindingsManager* mBindingsManager;
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;

        float mTimeIdle;

        // Object grab state
        bool mActivateHeld = false;
        float mActivateHoldTime = 0.0f;
        static constexpr float sGrabHoldThreshold = 0.2f;  // Time in seconds to distinguish tap from hold
        bool mGrabAttempted = false;  // Whether we've tried to grab during this hold
        osg::Vec3f mLastGrabbedObjectVelocity;
    };
}
#endif
