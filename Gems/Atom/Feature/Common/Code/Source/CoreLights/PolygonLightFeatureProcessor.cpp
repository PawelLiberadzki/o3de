/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <CoreLights/PolygonLightFeatureProcessor.h>
#include <CoreLights/LtcCommon.h>

#include <AzCore/Math/Vector3.h>
#include <AzCore/Math/Color.h>

#include <Atom/Feature/Mesh/MeshFeatureProcessor.h>
#include <Atom/Feature/CoreLights/CoreLightsConstants.h>

#include <Atom/RHI/Factory.h>

#include <Atom/RPI.Public/ColorManagement/TransformColor.h>
#include <Atom/RPI.Public/Image/StreamingImage.h>
#include <Atom/RPI.Public/RPISystemInterface.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>

namespace AZ::Render
{
    void PolygonLightFeatureProcessor::Reflect(ReflectContext* context)
    {
        if (auto* serializeContext = azrtti_cast<SerializeContext*>(context))
        {
            serializeContext
                ->Class<PolygonLightFeatureProcessor, FeatureProcessor>()
                ->Version(0);
        }
    }

    PolygonLightFeatureProcessor::PolygonLightFeatureProcessor()
        : PolygonLightFeatureProcessorInterface()
    {
    }

    void PolygonLightFeatureProcessor::Activate()
    {
        // Buffer for data about each light
        GpuBufferHandler::Descriptor desc;
        desc.m_bufferName = "PolygonLightBuffer";
        desc.m_bufferSrgName = "m_polygonLights";
        desc.m_elementCountSrgName = "m_polygonLightCount";
        desc.m_elementSize = sizeof(PolygonLightData);
        desc.m_srgLayout = RPI::RPISystemInterface::Get()->GetViewSrgLayout().get();

        m_lightBufferHandler = GpuBufferHandler(desc);

        // Buffer for all the polygon points for all the lights
        desc.m_bufferName = "PolygonLightPoints";
        desc.m_bufferSrgName = "m_polygonLightPoints";
        desc.m_elementCountSrgName = "";
        desc.m_elementSize = 16; // While only a 12 byte float3 is needed for positions, using 16 bytes since that's the minimal alignment.
        desc.m_srgLayout = RPI::RPISystemInterface::Get()->GetViewSrgLayout().get();

        m_lightPolygonPointBufferHandler = GpuBufferHandler(desc);

        Interface<ILtcCommon>::Get()->LoadMatricesForSrg(GetParentScene()->GetShaderResourceGroup());

        MeshFeatureProcessor* meshFeatureProcessor = GetParentScene()->GetFeatureProcessor<MeshFeatureProcessor>();
        if (meshFeatureProcessor)
        {
            m_lightMeshFlag = meshFeatureProcessor->GetShaderOptionFlagRegistry()->AcquireTag(AZ::Name("o_enablePolygonLights"));
        }
    }

    void PolygonLightFeatureProcessor::Deactivate()
    {
        m_lightData.Clear();
        m_lightBufferHandler.Release();
        m_lightPolygonPointBufferHandler.Release();
    }

    PolygonLightFeatureProcessor::LightHandle PolygonLightFeatureProcessor::AcquireLight()
    {
        uint16_t id = m_lightData.GetFreeSlotIndex();
        if (id == m_lightData.NoFreeSlot)
        {
            return LightHandle::Null;
        }
        else
        {
            // Set initial values for the start / end index of the light. Only the end needs to be recalculated as points are added / removed.
            PolygonLightData& lightData = m_lightData.GetData<0>(id);
            lightData.SetStartIndex(m_lightData.GetRawIndex(id) * MaxPolygonPoints);
            lightData.SetEndIndex(m_lightData.GetRawIndex(id) * MaxPolygonPoints + 1);
            return LightHandle(id);
        }
        // Intentionally don't set m_deviceBufferNeedsUpdate to true since the light doesn't yet have data.
    }

    bool PolygonLightFeatureProcessor::ReleaseLight(LightHandle& handle)
    {
        if (handle.IsValid())
        {
            auto movedIndex = m_lightData.RemoveIndex(handle.GetIndex());

            // fix up the start/end index for the light that moved into this deleted light's position since its
            // points were also moved.
            if (movedIndex != PolygonLightDataVector::NoFreeSlot)
            {
                EvaluateStartEndIndices(movedIndex);
            }

            m_deviceBufferNeedsUpdate = true;
            handle.Reset();
            return true;
        }
        return false;
    }

    PolygonLightFeatureProcessor::LightHandle PolygonLightFeatureProcessor::CloneLight(LightHandle sourceLightHandle)
    {
        AZ_Assert(sourceLightHandle.IsValid(), "Invalid LightHandle passed to PolygonLightFeatureProcessor::CloneLight().");

        LightHandle handle = AcquireLight();
        if (handle.IsValid())
        {
            // Duplicate the light data, update the start / end index fields to point to the new point buffer location.
            PolygonLightData& lightData = m_lightData.GetData<0>(handle.GetIndex());
            lightData = m_lightData.GetData<0>(sourceLightHandle.GetIndex());
            EvaluateStartEndIndices(handle.GetIndex());

            m_lightData.GetData<1>(handle.GetIndex()) = m_lightData.GetData<1>(sourceLightHandle.GetIndex());
            m_lightData.GetData<2>(handle.GetIndex()) = m_lightData.GetData<2>(sourceLightHandle.GetIndex());

            m_deviceBufferNeedsUpdate = true;
        }
        return handle;
    }

    void PolygonLightFeatureProcessor::Simulate(const FeatureProcessor::SimulatePacket& packet)
    {
        AZ_PROFILE_SCOPE(RPI, "PolygonLightFeatureProcessor: Simulate");
        AZ_UNUSED(packet);

        if (m_deviceBufferNeedsUpdate)
        {
            m_lightBufferHandler.UpdateBuffer(m_lightData.GetDataVector<0>());

            if (m_lightData.GetDataCount() > 0)
            {
                // A single array of MaxPolygonPoints points exists for each light, but we want to treat each
                // individual point as its own element instead of each array being its own element. Since all
                // the arrays are stored in a contiguous vector, we can treat it as one giant array.
                const LightPosition* firstPosition = m_lightData.GetDataVector<1>().at(0).data();
                m_lightPolygonPointBufferHandler.UpdateBuffer(firstPosition, static_cast<uint32_t>(m_lightData.GetDataCount() * MaxPolygonPoints));
            }
            m_deviceBufferNeedsUpdate = false;
        }

        if (r_enablePerMeshShaderOptionFlags)
        {
            MeshCommon::MarkMeshesWithFlag(GetParentScene(), AZStd::span(m_lightData.GetDataVector<2>()), m_lightMeshFlag.GetIndex());
        }
    }

    void PolygonLightFeatureProcessor::Render(const PolygonLightFeatureProcessor::RenderPacket& packet)
    {
        AZ_PROFILE_SCOPE(RPI, "PolygonLightFeatureProcessor: Render");

        for (const RPI::ViewPtr& view : packet.m_views)
        {
            m_lightBufferHandler.UpdateSrg(view->GetShaderResourceGroup().get());
            m_lightPolygonPointBufferHandler.UpdateSrg(view->GetShaderResourceGroup().get());
        }
    }

    void PolygonLightFeatureProcessor::SetRgbIntensity(LightHandle handle, const PhotometricColor<PhotometricUnitType>& lightRgbIntensity)
    {
        AZ_Assert(handle.IsValid(), "Invalid LightHandle passed to PolygonLightFeatureProcessor::SetRgbIntensity().");

        auto transformedColor = AZ::RPI::TransformColor(lightRgbIntensity, AZ::RPI::ColorSpaceId::LinearSRGB, AZ::RPI::ColorSpaceId::ACEScg);

        AZStd::array<float, 3>& rgbIntensity = m_lightData.GetData<0>(handle.GetIndex()).m_rgbIntensityNits;
        transformedColor.GetAsVector3().StoreToFloat3(rgbIntensity.data());

        m_deviceBufferNeedsUpdate = true;
    }

    void PolygonLightFeatureProcessor::SetPosition(LightHandle handle, const AZ::Vector3& position)
    {
        AZ_Assert(handle.IsValid(), "Invalid LightHandle passed to PolygonLightFeatureProcessor::SetTransform().");

        PolygonLightData& data = m_lightData.GetData<0>(handle.GetIndex());
        position.StoreToFloat3(data.m_position.data());

        UpdateBounds(handle);

        m_deviceBufferNeedsUpdate = true;
    }

    void PolygonLightFeatureProcessor::SetLightEmitsBothDirections(LightHandle handle, bool lightEmitsBothDirections)
    {
        AZ_Assert(handle.IsValid(), "Invalid LightHandle passed to PolygonLightFeatureProcessor::SetLightEmitsBothDirections().");

        float& invAttenuationRadiusSquared = m_lightData.GetData<0>(handle.GetIndex()).m_invAttenuationRadiusSquared;

        // Light emitting both directions is stored in the sign of the attenuation radius since that must always be positive.
        invAttenuationRadiusSquared = lightEmitsBothDirections ? -abs(invAttenuationRadiusSquared) : abs(invAttenuationRadiusSquared);

        UpdateBounds(handle);

        m_deviceBufferNeedsUpdate = true;
    }

    void PolygonLightFeatureProcessor::SetAttenuationRadius(LightHandle handle, float attenuationRadius)
    {
        AZ_Assert(handle.IsValid(), "Invalid LightHandle passed to PolygonLightFeatureProcessor::SetAttenuationRadius().");

        attenuationRadius = AZStd::max<float>(attenuationRadius, 0.001f); // prevent divide by zero.
        float& invAttenuationRadiusSquared = m_lightData.GetData<0>(handle.GetIndex()).m_invAttenuationRadiusSquared;
        float sign = invAttenuationRadiusSquared < 0.0f ? -1.0f : 1.0f; // preserve SetLightEmitsBothDirections data stored in the sign.
        invAttenuationRadiusSquared = 1.0f / (attenuationRadius * attenuationRadius) * sign;

        UpdateBounds(handle);

        m_deviceBufferNeedsUpdate = true;
    }

    void PolygonLightFeatureProcessor::SetPolygonPoints(LightHandle handle, const Vector3* vertices, const uint32_t vertexCount, const Vector3& direction)
    {
        AZ_Warning("PolygonLightFeatureProcessor", vertexCount <= MaxPolygonPoints, "Too many polygon points on polygon light. Only using the first %lu vertices.", MaxPolygonPoints);
        AZ_Warning("PolygonLightFeatureProcessor", vertexCount > 2, "Polygon light must have at least three points - ignoring points.");

        PolygonLightData& data = m_lightData.GetData<0>(handle.GetIndex());
        if (vertexCount < 3)
        {
            data.SetEndIndex(data.GetStartIndex()); // Set the number of points to 0.
        }
        else
        {
            PolygonPoints& pointArray = m_lightData.GetData<1>(handle.GetIndex());
            uint32_t clippedCount = AZ::GetMin<uint32_t>(vertexCount, MaxPolygonPoints);

            for (uint32_t i = 0; i < clippedCount; ++i)
            {
                vertices[i].StoreToFloat4(&pointArray.at(i).x); // StoreToFloat4 is faster and w is ignored by the shader.
            }
            data.SetEndIndex(data.GetStartIndex() + clippedCount);
            direction.GetNormalized().StoreToFloat3(data.m_direction.data());
        }

        UpdateBounds(handle);
        m_deviceBufferNeedsUpdate = true;
    }

    const Data::Instance<RPI::Buffer> PolygonLightFeatureProcessor::GetLightBuffer()const
    {
        return m_lightBufferHandler.GetBuffer();
    }

    const Data::Instance<RPI::Buffer> PolygonLightFeatureProcessor::GetLightPointBuffer() const
    {
        return m_lightPolygonPointBufferHandler.GetBuffer();
    }

    uint32_t PolygonLightFeatureProcessor::GetLightCount() const
    {
        return m_lightBufferHandler.GetElementCount();
    }

    void PolygonLightFeatureProcessor::EvaluateStartEndIndices(PolygonLightDataVector::IndexType index)
    {
        PolygonLightData& lightData = m_lightData.GetData<0>(index);
        uint32_t length = lightData.GetEndIndex() - lightData.GetStartIndex();
        lightData.SetStartIndex(m_lightData.GetRawIndex(index) * MaxPolygonPoints);
        lightData.SetEndIndex(lightData.GetStartIndex() + length);
    }

    void PolygonLightFeatureProcessor::UpdateBounds(LightHandle handle)
    {
        PolygonLightData data = m_lightData.GetData<0>(handle.GetIndex());
        MeshCommon::BoundsVariant bounds = m_lightData.GetData<2>(handle.GetIndex());

        AZ::Vector3 position = AZ::Vector3::CreateFromFloat3(data.m_position.data());
        float radius = LightCommon::GetRadiusFromInvRadiusSquared(abs(data.m_invAttenuationRadiusSquared));

        if (data.GetEndIndex() == data.GetStartIndex() || AZStd::isnan(radius))
        {
            // Invalid polygon light, just use an invalid variant.
            bounds.emplace<AZStd::monostate>();
        }
        if (data.m_invAttenuationRadiusSquared < 0.0f) // Emits both directions is stored in the sign of m_invAttenuationRadiusSquared.
        {
            bounds.emplace<Sphere>(AZ::Sphere(position, radius));
        }
        else
        {
            AZ::Vector3 normal = AZ::Vector3::CreateFromFloat3(data.m_direction.data());
            bounds.emplace<Hemisphere>(AZ::Hemisphere(position, radius, normal));
        }
    }

} // namespace AZ::Render
