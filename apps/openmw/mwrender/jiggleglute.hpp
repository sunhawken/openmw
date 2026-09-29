#ifndef OPENMW_MWRENDER_JIGGLEGLUTE_H
#define OPENMW_MWRENDER_JIGGLEGLUTE_H

#include <algorithm>
#include <cmath>

namespace MWRender::JiggleGlute
{
    // Glute ("booty") physics ported from everlaster's Naturalis v74 VaM plugin (BootyMagic module:
    // GluteJointPhysicsHandler, GluteForcePhysicsHandler, GluteGravityPhysicsHandler). Naturalis
    // drives a Unity ConfigurableJoint per glute; OpenMW's jiggle bone is a single spring-damper, so
    // the joint curves are evaluated here and returned as scale factors relative to Naturalis's
    // default glute (lightest mass, 70% softness, zero quickness offset). At the default slider
    // values every factor is therefore 1.0 and the glutes keep the feel of the shared Jiggle tab
    // tuning; softness / quickness / mass then bend the response along Naturalis's own curves.

    // Naturalis GLUTE_JOINT_MASS range, in kg ("Glute Weight").
    constexpr float sMassMin = 1.f;
    constexpr float sMassMax = 3.f;
    // Naturalis "Glute Softness (Joint Physics)" default.
    constexpr float sDefaultSoftnessPercent = 70.f;

    inline float lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    inline float inverseLerp(float a, float b, float v)
    {
        if (a == b)
            return 0.f;
        return std::clamp((v - a) / (b - a), 0.f, 1.f);
    }

    // Calc.Exponential1
    inline float exponential1(float x, float b, float p, float q, float a = 1.f, float s = 0.f)
    {
        const float v = std::max(0.f, a * x + s);
        return (1.f - b) * std::pow(v, p) + b * std::pow(v, q);
    }

    // Calc.SoftnessBaseCurve - BootyMagic runs the joint softness slider through this curve.
    inline float softnessBaseCurve(float x)
    {
        return exponential1(x, 6.44f, 1.27f, 1.15f);
    }

    // Calc.InverseSmoothStep
    inline float inverseSmoothStep(float value, float curvature, float midPoint)
    {
        if (value <= 0.f)
            return 0.f;
        if (value > 1.f)
            return 1.f;
        const float c = 2.f / (1.f - curvature) - midPoint;
        const auto f1 = [c](float v, float mid) { return std::pow(v, c) / std::pow(mid, c - 1.f); };
        const float result = value < midPoint ? f1(value, midPoint) : 1.f - f1(1.f - value, 1.f - midPoint);
        return std::isnan(result) ? 0.f : result;
    }

    // Base value + Naturalis quickness/slowness offset (PhysicsParam.UpdateBaseValue).
    inline float withQuickness(float value, float quickness, float quickOffset, float slowOffset)
    {
        if (quickness > 0.f)
            value += quickOffset * std::min(quickness, 1.f);
        else if (quickness < 0.f)
            value += slowOffset * std::min(-quickness, 1.f);
        return value;
    }

    // Raw Naturalis joint values. `mass` is the normalized glute mass (0..1), `softness` is the
    // curved joint softness (0..1), `quickness` is the Glute Quickness Offset (-1..1).
    inline float rotationSpring(float mass, float softness, float quickness)
    {
        const float massCurve = inverseSmoothStep(mass, 0.23f, 0.30f);
        const float base = 66.f * (1.f + 0.80f * massCurve - 0.46f * inverseSmoothStep(softness, 0.10f, 0.13f));
        const float v = withQuickness(
            base, quickness, 6.f * (1.f + 0.67f * massCurve), -9.f * (1.f + 0.67f * massCurve));
        return std::clamp(v, 10.f, 200.f);
    }

    inline float rotationDamper(float mass, float softness, float quickness)
    {
        const float massCurve = inverseSmoothStep(mass, 0.31f, 0.80f);
        const float base = 0.58f * (1.f + 1.00f * massCurve - 0.55f * inverseSmoothStep(softness, 0.24f, 0.61f));
        const float v = withQuickness(
            base, quickness, -0.03f * (1.f + 8.375f * massCurve), 0.015f * (1.f + 8.375f * massCurve));
        return std::max(0.01f, v);
    }

    inline float inOutSpring(float mass, float softness, float quickness)
    {
        const float light = 1.f - mass;
        const float base = 900.f * (1.f - 0.15f * light - 0.30f * softness);
        const float v
            = withQuickness(base, quickness, 35.f * (1.f - 0.28f * light), -52.f * (1.f - 0.28f * light));
        return std::clamp(v, 1.f, 5000.f);
    }

    inline float inOutDamper(float mass, float softness, float quickness)
    {
        const float base = 9.50f * (1.f + 0.67f * mass - 0.40f * softness);
        const float v = withQuickness(base, quickness, -2.f * (1.f + 1.33f * mass), 3.f * (1.f + 1.33f * mass));
        return std::clamp(v, 0.01f, 50.f);
    }

    // GluteJointPhysicsHandler.UpdateMassValueAndAmounts: part of the mass goes to the colliders.
    inline float jointMass(float mass)
    {
        const float kg = lerp(sMassMin, sMassMax, mass);
        return lerp(0.92f, 0.78f, mass) * kg;
    }

    // GluteForcePhysicsHandler.SetDepthInMultiplier / SetDepthOutMultiplier.
    inline float forceSoftness(float softness)
    {
        return inverseSmoothStep(softness, 0.80f, 0.03f);
    }

    inline float depthInForce(float mass, float softness)
    {
        return lerp(0.50f, 1.00f, forceSoftness(softness)) * 4.40f * lerp(1.f, 0.50f, mass);
    }

    inline float depthOutForce(float mass, float softness)
    {
        return lerp(0.65f, 1.125f, forceSoftness(softness)) * 2.20f * lerp(1.f, 1.50f, mass);
    }

    struct Response
    {
        float mTangentialStiffness = 1.f; // swing (rotation spring)
        float mTangentialDamping = 1.f; // swing (rotation damper)
        float mDepthStiffness = 1.f; // in/out along the glute's depth axis (in/out spring)
        float mDepthDamping = 1.f; // in/out damper
        float mMass = 1.f;
        float mDepthIn = 1.f; // how far motion can push the glute into the body
        float mDepthOut = 1.f; // how far motion can pull the glute out from the body
        float mMaxDisplacement = 1.f; // depth limit grows with glute mass
    };

    /// @param normalizedMass 0..1 (Naturalis NormalizedMass: inverse-lerp of 1..3 kg)
    /// @param softnessPercent 0..100 (Glute Softness)
    /// @param quickness -1..1 (Glute Quickness Offset)
    inline Response computeResponse(float normalizedMass, float softnessPercent, float quickness)
    {
        const float m = std::clamp(normalizedMass, 0.f, 1.f);
        const float s = softnessBaseCurve(std::clamp(softnessPercent, 0.f, 100.f) / 100.f);
        const float q = std::clamp(quickness, -1.f, 1.f);

        const float m0 = 0.f;
        const float s0 = softnessBaseCurve(sDefaultSoftnessPercent / 100.f);

        Response r;
        r.mTangentialStiffness = rotationSpring(m, s, q) / rotationSpring(m0, s0, 0.f);
        r.mTangentialDamping = rotationDamper(m, s, q) / rotationDamper(m0, s0, 0.f);
        r.mDepthStiffness = inOutSpring(m, s, q) / inOutSpring(m0, s0, 0.f);
        r.mDepthDamping = inOutDamper(m, s, q) / inOutDamper(m0, s0, 0.f);
        r.mMass = jointMass(m) / jointMass(m0);
        r.mDepthIn = depthInForce(m, s) / depthInForce(m0, s0);
        r.mDepthOut = depthOutForce(m, s) / depthOutForce(m0, s0);
        r.mMaxDisplacement = lerp(1.f, 3.f, m);
        return r;
    }
}

#endif
