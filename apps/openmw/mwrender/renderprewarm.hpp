#ifndef OPENMW_MWRENDER_RENDERPREWARM_H
#define OPENMW_MWRENDER_RENDERPREWARM_H

#include "vismask.hpp"

#include <osg/Geometry>
#include <osg/Group>
#include <osg/NodeVisitor>
#include <osg/RenderInfo>
#include <osg/State>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace MWRender
{
    // ICO compiles GL objects, but some drivers specialize a pipeline only on its
    // first draw. Warm a private copy through the real scene render state. The
    // draw callback changes only matrix values: every vertex is behind the eye,
    // so no color/depth pixel can change and no alternate blend/shader is used.
    class ClippedRenderPrewarm
    {
        struct Completion
        {
            std::atomic<bool> drawn{false};
        };

        class DrawCallback final : public osg::Drawable::DrawCallback
        {
        public:
            explicit DrawCallback(std::shared_ptr<Completion> completion)
                : mCompletion(std::move(completion))
            {
            }

            void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
            {
                auto* state = info.getState();
                if (!state)
                    return;
                const auto& projection = state->getProjectionMatrix();
                // Standard/reversed-Z perspective has clip.w=-eye.z. Do not
                // draw in an orthographic shadow/GUI pass or an unknown layout.
                if (std::abs(projection(3, 3)) > 1e-6 || projection(2, 3) >= -1e-6
                    || std::abs(projection(0, 3)) > 1e-6 || std::abs(projection(1, 3)) > 1e-6)
                    return;
                const auto& bounds = drawable->getBoundingBox();
                if (!bounds.valid())
                    return;
                const osg::Matrix saved = state->getModelViewMatrix();
                double minimumZ = std::numeric_limits<double>::infinity();
                for (unsigned i = 0; i < 8; ++i)
                    minimumZ = std::min(minimumZ, double((bounds.corner(i) * saved).z()));
                if (!std::isfinite(minimumZ))
                    return;
                osg::Matrix clipped = saved;
                clipped(3, 2) += 1.0 - minimumZ;
                struct Restore
                {
                    osg::State& state;
                    const osg::Matrix& matrix;
                    ~Restore()
                    {
                        state.applyModelViewMatrix(matrix);
                        state.applyModelViewAndProjectionUniformsIfRequired();
                    }
                } restore{*state, saved};
                state->applyModelViewMatrix(clipped);
                state->applyModelViewAndProjectionUniformsIfRequired();
                drawable->drawImplementation(info);
                mCompletion->drawn.store(true, std::memory_order_release);
            }

        private:
            std::shared_ptr<Completion> mCompletion;
        };

        struct PrepareVisitor final : osg::NodeVisitor
        {
            std::vector<std::shared_ptr<Completion>>& completions;
            explicit PrepareVisitor(std::vector<std::shared_ptr<Completion>>& value)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), completions(value)
            {
                setNodeMaskOverride(~0u);
            }
            void apply(osg::Node& node) override
            {
                node.setCullingActive(false);
                traverse(node);
            }
            void apply(osg::Drawable& drawable) override
            {
                drawable.setCullingActive(false);
                if (!drawable.asGeometry() || !drawable.getBoundingBox().valid())
                    return;
                auto completion = std::make_shared<Completion>();
                drawable.setUseDisplayList(false);
                drawable.setDrawCallback(new DrawCallback(completion));
                completions.push_back(std::move(completion));
            }
        };

    public:
        static std::unique_ptr<ClippedRenderPrewarm> attach(osg::ref_ptr<osg::Node> exactPreview, osg::Group* parent)
        {
            if (!exactPreview || !parent)
                return {};
            auto result = std::unique_ptr<ClippedRenderPrewarm>(new ClippedRenderPrewarm);
            PrepareVisitor visitor(result->mCompletions);
            exactPreview->accept(visitor);
            if (result->mCompletions.empty())
                return {};
            result->mRoot = new osg::Group;
            result->mRoot->setName("Brutal Invisible Pipeline Prewarm");
            result->mRoot->setNodeMask(Mask_Effect);
            result->mRoot->setCullingActive(false);
            result->mRoot->addChild(exactPreview);
            parent->addChild(result->mRoot);
            return result;
        }

        ~ClippedRenderPrewarm() { detach(); }

        bool drawn() const
        {
            return !mCompletions.empty() && std::all_of(mCompletions.begin(), mCompletions.end(), [](const auto& value) {
                return value->drawn.load(std::memory_order_acquire);
            });
        }
        std::size_t drawableCount() const { return mCompletions.size(); }
        bool timedOut() const { return mTimedOut; }
        // Call from the owner's ordinary scene update, never from a draw thread.
        bool update()
        {
            if (!mRoot)
                return true;
            if (drawn() || ++mUpdates >= 120)
            {
                mTimedOut = !drawn();
                detach();
                return true;
            }
            return false;
        }

    private:
        void detach()
        {
            if (!mRoot)
                return;
            mRoot->setNodeMask(0);
            while (mRoot->getNumParents())
                mRoot->getParent(0)->removeChild(mRoot);
            mRoot = nullptr;
        }
        osg::ref_ptr<osg::Group> mRoot;
        std::vector<std::shared_ptr<Completion>> mCompletions;
        unsigned mUpdates = 0;
        bool mTimedOut = false;
    };
}

#endif
