#include "objectstate.hpp"

#include <array>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <typeinfo>

#include "esmreader.hpp"
#include "esmwriter.hpp"

namespace ESM
{
    namespace
    {
        template <std::size_t N>
        void loadReleasedArray(ESMReader& esm, NAME name, std::vector<std::array<float, N>>& target,
            std::size_t maxCount = ReleasedEquipmentState::MaxVertices)
        {
            esm.getSubNameIs(name);
            esm.getSubHeader();
            const std::size_t bytes = esm.getSubSize();
            if (bytes % sizeof(std::array<float, N>) != 0
                || bytes / sizeof(std::array<float, N>) > maxCount)
                esm.fail("Invalid released equipment array size");
            target.resize(bytes / sizeof(std::array<float, N>));
            if (bytes)
                esm.getExact(target.data(), bytes);
        }

        template <std::size_t N>
        void saveReleasedArray(ESMWriter& esm, NAME name, const std::vector<std::array<float, N>>& source)
        {
            esm.startSubRecord(name);
            if (!source.empty())
                esm.write(reinterpret_cast<const char*>(source.data()), source.size() * sizeof(source.front()));
            esm.endRecord(name);
        }

        template <std::size_t N>
        bool finiteReleasedArray(const std::vector<std::array<float, N>>& values)
        {
            for (const auto& value : values)
                for (const float component : value)
                    if (!std::isfinite(component) || std::abs(component) > 1000000.f)
                        return false;
            return true;
        }
    }

    bool ReleasedEquipmentState::valid() const
    {
        if (mModel.empty() || mModel.size() > 4096 || !std::isfinite(mMass) || mMass < .05f || mMass > 500.f
            || mDrawables.empty() || mDrawables.size() > MaxDrawables)
            return false;
        std::size_t vertices = 0;
        for (const auto& drawable : mDrawables)
        {
            const auto& a = drawable.mTransform;
            for (const float component : a)
                if (!std::isfinite(component) || std::abs(component) > 1000000.f)
                    return false;
            const double determinant = double(a[0]) * (double(a[5]) * a[10] - double(a[6]) * a[9])
                - double(a[1]) * (double(a[4]) * a[10] - double(a[6]) * a[8])
                + double(a[2]) * (double(a[4]) * a[9] - double(a[5]) * a[8]);
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12
                || std::abs(a[3]) > 1e-5f || std::abs(a[7]) > 1e-5f
                || std::abs(a[11]) > 1e-5f || std::abs(a[15] - 1.f) > 1e-5f)
                return false;
            vertices += drawable.mVertices.size();
            if (vertices > MaxVertices || drawable.mSourceName.size() > 4096 || drawable.mVertices.empty()
                || drawable.mSourceVertexCount != drawable.mVertices.size()
                || (!drawable.mNormals.empty() && drawable.mNormals.size() != 1
                    && drawable.mNormals.size() != drawable.mVertices.size())
                || (!drawable.mTangents.empty() && drawable.mTangents.size() != drawable.mVertices.size())
                || !finiteReleasedArray(drawable.mVertices) || !finiteReleasedArray(drawable.mNormals)
                || !finiteReleasedArray(drawable.mTangents))
                return false;
        }
        return true;
    }

    void ReleasedEquipmentState::load(ESMReader& esm)
    {
        uint32_t version = 0;
        esm.getHNT("RELS", version);
        if (version != 1)
            esm.fail("Unsupported released equipment snapshot version");
        mModel = esm.getHNString("RMDL");
        esm.getHNT("RMS_", mMass);
        uint32_t count = 0;
        esm.getHNT("RNUM", count);
        if (count == 0 || count > MaxDrawables)
            esm.fail("Invalid released equipment drawable count");
        mDrawables.clear();
        std::size_t total = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            Drawable drawable;
            drawable.mSourceName = esm.getHNString("RDGN");
            esm.getHNT("RORD", drawable.mSourceOrdinal);
            esm.getHNT("RRID", drawable.mSourceRecordIndex);
            esm.getHNT("RVCT", drawable.mSourceVertexCount);
            esm.getHNT("RTHS", drawable.mSourceTopologyHash);
            uint8_t clockwise = 0;
            esm.getHNT("RWND", clockwise);
            if (clockwise > 1)
                esm.fail("Invalid released equipment winding");
            drawable.mClockwise = clockwise != 0;
            esm.getHNT("RXFM", drawable.mTransform);
            loadReleasedArray(esm, "RVTX", drawable.mVertices);
            total += drawable.mVertices.size();
            if (total > MaxVertices)
                esm.fail("Released equipment snapshot exceeds vertex budget");
            loadReleasedArray(esm, "RNRM", drawable.mNormals, drawable.mVertices.size());
            loadReleasedArray(esm, "RTAN", drawable.mTangents, drawable.mVertices.size());
            mDrawables.push_back(std::move(drawable));
        }
        if (!valid())
            esm.fail("Invalid released equipment snapshot");
    }

    void ReleasedEquipmentState::save(ESMWriter& esm) const
    {
        if (!valid())
            throw std::runtime_error("Cannot save invalid released equipment snapshot");
        esm.writeHNT("RELS", uint32_t(1));
        esm.writeHNString("RMDL", mModel);
        esm.writeHNT("RMS_", mMass);
        esm.writeHNT("RNUM", static_cast<uint32_t>(mDrawables.size()));
        for (const auto& drawable : mDrawables)
        {
            esm.writeHNString("RDGN", drawable.mSourceName);
            esm.writeHNT("RORD", drawable.mSourceOrdinal);
            esm.writeHNT("RRID", drawable.mSourceRecordIndex);
            esm.writeHNT("RVCT", drawable.mSourceVertexCount);
            esm.writeHNT("RTHS", drawable.mSourceTopologyHash);
            esm.writeHNT("RWND", static_cast<uint8_t>(drawable.mClockwise));
            esm.writeHNT("RXFM", drawable.mTransform);
            saveReleasedArray(esm, "RVTX", drawable.mVertices);
            saveReleasedArray(esm, "RNRM", drawable.mNormals);
            saveReleasedArray(esm, "RTAN", drawable.mTangents);
        }
    }

    void ObjectState::load(ESMReader& esm)
    {
        mVersion = esm.getFormatVersion();
        mActorIdConverter = esm.mActorIdConverter;

        bool isDeleted;
        mRef.loadData(esm, isDeleted);

        mHasLocals = 0;
        esm.getHNOT(mHasLocals, "HLOC");

        if (mHasLocals)
            mLocals.load(esm);

        mLuaScripts.load(esm);

        mEnabled = 1;
        esm.getHNOT(mEnabled, "ENAB");

        if (mVersion <= MaxOldCountFormatVersion)
        {
            if (mVersion <= MaxOldGoldValueFormatVersion)
                mRef.mCount = std::max(1, mRef.mCount);
            esm.getHNOT("COUN", mRef.mCount);
        }

        mPosition = mRef.mPos;
        esm.getOptionalComposite("POS_", mPosition);

        mFlags = 0;
        esm.getHNOT(mFlags, "FLAG");

        mAnimationState.load(esm);

        mReleasedEquipment.reset();
        if (esm.peekNextSub("RELS"))
        {
            auto state = std::make_shared<ReleasedEquipmentState>();
            state->load(esm);
            mReleasedEquipment = std::move(state);
        }

        // FIXME: assuming "false" as default would make more sense, but also break compatibility with older save files
        mHasCustomState = true;
        esm.getHNOT(mHasCustomState, "HCUS");
    }

    void ObjectState::save(ESMWriter& esm, bool inInventory) const
    {
        mRef.save(esm, true, inInventory);

        if (mHasLocals)
        {
            esm.writeHNT("HLOC", mHasLocals);
            mLocals.save(esm);
        }

        mLuaScripts.save(esm);

        if (!mEnabled && !inInventory)
            esm.writeHNT("ENAB", mEnabled);

        if (!inInventory && mPosition != mRef.mPos)
        {
            esm.writeNamedComposite("POS_", mPosition);
        }

        if (mFlags != 0)
            esm.writeHNT("FLAG", mFlags);

        mAnimationState.save(esm);

        if (!inInventory && mReleasedEquipment)
            mReleasedEquipment->save(esm);

        if (!mHasCustomState)
            esm.writeHNT("HCUS", false);
    }

    void ObjectState::blank()
    {
        mRef.blank();
        mHasLocals = 0;
        mEnabled = false;
        for (int i = 0; i < 3; ++i)
        {
            mPosition.pos[i] = 0;
            mPosition.rot[i] = 0;
        }
        mFlags = 0;
        mHasCustomState = true;
        mReleasedEquipment.reset();
    }

    const NpcState& ObjectState::asNpcState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to NpcState";
        throw std::logic_error(error.str());
    }

    NpcState& ObjectState::asNpcState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to NpcState";
        throw std::logic_error(error.str());
    }

    const CreatureState& ObjectState::asCreatureState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureState";
        throw std::logic_error(error.str());
    }

    CreatureState& ObjectState::asCreatureState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureState";
        throw std::logic_error(error.str());
    }

    const ContainerState& ObjectState::asContainerState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to ContainerState";
        throw std::logic_error(error.str());
    }

    ContainerState& ObjectState::asContainerState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to ContainerState";
        throw std::logic_error(error.str());
    }

    const DoorState& ObjectState::asDoorState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to DoorState";
        throw std::logic_error(error.str());
    }

    DoorState& ObjectState::asDoorState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to DoorState";
        throw std::logic_error(error.str());
    }

    const CreatureLevListState& ObjectState::asCreatureLevListState() const
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureLevListState";
        throw std::logic_error(error.str());
    }

    CreatureLevListState& ObjectState::asCreatureLevListState()
    {
        std::stringstream error;
        error << "bad cast " << typeid(this).name() << " to CreatureLevListState";
        throw std::logic_error(error.str());
    }

    ObjectState::~ObjectState() = default;

}
