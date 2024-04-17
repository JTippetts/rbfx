//
// Copyright (c) 2017-2020 the rbfx project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "../Precompiled.h"

#include "../RmlUI/RmlRenderer.h"

#include "../Core/Context.h"
#include "../Graphics/GraphicsEvents.h"
#include "../Graphics/IndexBuffer.h"
#include "../Graphics/Material.h"
#include "../Graphics/Texture2D.h"
#include "../Graphics/VertexBuffer.h"
#include "../IO/Log.h"
#include "../Math/Matrix4.h"
#include "../Resource/Image.h"
#include "../Resource/ResourceCache.h"
#include "Urho3D/RenderAPI/DrawCommandQueue.h"
#include "Urho3D/RenderAPI/RenderAPIUtils.h"
#include "Urho3D/RenderAPI/RenderContext.h"
#include "Urho3D/RenderAPI/RenderDevice.h"
#include "Urho3D/RenderAPI/RenderScope.h"
#include "Urho3D/RenderPipeline/ShaderConsts.h"

#include "../DebugNew.h"

namespace Urho3D
{

namespace Detail
{

namespace
{

/// Internal vertex type used to render RmlUI geometry.
struct RmlVertex
{
    Vector3 position_;
    unsigned color_;
    Vector2 texCoord_;
};

/// rbfx Vector3 to Rml Vector2f
Rml::Vector2f Vector3fToRmlVector2f(const Vector3 &vector) { return Rml::Vector2f(vector.x_, vector.y_); }

/// Internal compiled geometry holder.
struct CompiledRmlGeometry
{
    ea::vector<RmlVertex> vertices_;
    ea::vector<unsigned int> indices_;
};

/// Wrap CompiledrmlGeometry to RmlUI handle
Rml::CompiledGeometryHandle WrapGeometryHandle(CompiledRmlGeometry* geometry) { return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry); }

/// Unwrap RmlUI handle to CompiledRmlGeometry pointer
CompiledRmlGeometry* UnwrapGeometryHandle(Rml::CompiledGeometryHandle geometry) { return reinterpret_cast<CompiledRmlGeometry*>(geometry); }

/// Internal RmlUI texture holder.
struct CachedRmlTexture
{
    SharedPtr<Image> image_;
    SharedPtr<Texture2D> texture_;
};

/// Wrap CachedRmlTexture pointer to RmlUI handle.
Rml::TextureHandle WrapTextureHandle(CachedRmlTexture* texture) { return reinterpret_cast<Rml::TextureHandle>(texture); }

/// Unwrap RmlUI handle to CachedRmlTexture pointer.
CachedRmlTexture* UnwrapTextureHandle(Rml::TextureHandle texture) { return reinterpret_cast<CachedRmlTexture*>(texture); }

/// Roughly transform scissor rect.
IntRect TransformScissorRect(const IntRect& rect, const Matrix3x4& transform)
{
    const Vector3 corners[4] = {
        {static_cast<float>(rect.left_), static_cast<float>(rect.top_), 0.0f},
        {static_cast<float>(rect.right_), static_cast<float>(rect.top_), 0.0f},
        {static_cast<float>(rect.right_), static_cast<float>(rect.bottom_), 0.0f},
        {static_cast<float>(rect.left_), static_cast<float>(rect.bottom_), 0.0f}
    };

    IntVector2 minCorner{M_MAX_INT, M_MAX_INT}, maxCorner{M_MIN_INT, M_MIN_INT};
    for (const Vector3& corner : corners)
    {
        const IntVector2 transformedCorner = VectorRoundToInt((transform * corner).ToVector2());
        minCorner = VectorMin(minCorner, transformedCorner);
        maxCorner = VectorMax(maxCorner, transformedCorner);
    }
    return {minCorner, maxCorner};
}

}

RmlRenderer::RmlRenderer(Context* context)
    : Object(context)
{
    InitializeGraphics();
    SubscribeToEvent(E_SCREENMODE, &RmlRenderer::InitializeGraphics);
}

void RmlRenderer::BeginRendering()
{
    auto renderDevice = GetSubsystem<RenderDevice>();
    RenderContext* renderContext = renderDevice->GetRenderContext();

    drawQueue_ = renderDevice->GetDefaultQueue();
    vertexBuffer_->Discard();
    indexBuffer_->Discard();
    drawQueue_->Reset();
    textures_.clear();

    VertexBuffer* vertexBuffer = vertexBuffer_->GetVertexBuffer();
    IndexBuffer* indexBuffer = indexBuffer_->GetIndexBuffer();

    drawQueue_->SetVertexBuffers({vertexBuffer});
    drawQueue_->SetIndexBuffer(indexBuffer);

    batchStateCreateContext_.vertexBuffer_ = vertexBuffer;
    batchStateCreateContext_.indexBuffer_ = indexBuffer;

    const RenderBackend backend = renderDevice->GetBackend();
    const PipelineStateOutputDesc& outputDesc = renderContext->GetCurrentRenderTargetsDesc();
    const bool isSwapChain = renderContext->IsSwapChainRenderTarget();
    isRenderSurfaceSRGB_ = IsTextureFormatSRGB(outputDesc.renderTargetFormats_[0]);
    viewportSize_ = renderContext->GetCurrentViewport().Size();

    // On OpenGL, flip the projection if rendering to a texture so that the texture can be addressed in the
    // same way as a render texture produced on Direct3D.
    flipRect_ = !isSwapChain && backend == RenderBackend::OpenGL;

    const Vector2 invScreenSize = Vector2::ONE / viewportSize_.ToVector2();
    Vector2 scale(2.0f * invScreenSize.x_, -2.0f * invScreenSize.y_);
    Vector2 offset(-1.0f, 1.0f);
    if (flipRect_)
    {
        offset.y_ = -offset.y_;
        scale.y_ = -scale.y_;
    }

    const float farClip_ = 1000.0f;
    projection_ = Matrix4::IDENTITY;
    projection_.m00_ = scale.x_;
    projection_.m03_ = offset.x_;
    projection_.m11_ = scale.y_;
    projection_.m13_ = offset.y_;
    projection_.m22_ = 1.0f / farClip_;
    projection_.m23_ = 0.0f;
    projection_.m33_ = 1.0f;
}

void RmlRenderer::EndRendering()
{
    auto renderDevice = GetSubsystem<RenderDevice>();
    RenderContext* renderContext = renderDevice->GetRenderContext();
    const RenderScope renderScope(renderContext, "RmlRenderer::EndRendering");

    vertexBuffer_->Commit();
    indexBuffer_->Commit();
    renderContext->Execute(drawQueue_);
    drawQueue_ = nullptr;
}

void RmlRenderer::InitializeGraphics()
{
    auto renderDevice = GetSubsystem<RenderDevice>();
    if (!renderDevice)
        return;

    batchStateCache_ = MakeShared<DefaultUIBatchStateCache>(context_);

    vertexBuffer_ = MakeShared<DynamicVertexBuffer>(context_);
    vertexBuffer_->Initialize(1024, VertexBuffer::GetElements(MASK_POSITION | MASK_COLOR | MASK_TEXCOORD1));
    indexBuffer_ = MakeShared<DynamicIndexBuffer>(context_);
    indexBuffer_->Initialize(1024, true);

    const ea::string baseDefines = "VERTEXCOLOR ";
    const ea::string alphaMapDefines = baseDefines + "ALPHAMAP ";
    const ea::string diffMapDefines = baseDefines + "DIFFMAP ";

    noTextureMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", baseDefines, baseDefines);
    alphaMapMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", alphaMapDefines, alphaMapDefines);
    diffMapMaterial_ = Material::CreateBaseMaterial(context_, "v2/X_Basic", diffMapDefines, diffMapDefines);
}

Material* RmlRenderer::GetBatchMaterial(Texture2D* texture)
{
    if (!texture)
        return noTextureMaterial_;
    else if (texture->GetFormat() == TextureFormat::TEX_FORMAT_R8_UNORM)
        return alphaMapMaterial_;
    else
        return diffMapMaterial_;
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    auto compiledGeometry = new CompiledRmlGeometry{};
    compiledGeometry->vertices_.resize(vertices.size());
    compiledGeometry->indices_.resize(indices.size());
    
    for (unsigned i = 0; i < vertices.size(); ++i)
    {
        RmlVertex &vertex = compiledGeometry->vertices_[i];
        vertex.position_.x_ = vertices[i].position.x;
        vertex.position_.y_ = vertices[i].position.y;
        vertex.position_.z_ = 0.0f;
        Rml::Colourb color = vertices[i].colour.ToNonPremultiplied();
        vertex.color_ = (color.alpha << 24u) | (color.blue << 16u) | (color.green << 8u) | color.red;
        vertex.texCoord_.x_ = vertices[i].tex_coord.x;
        vertex.texCoord_.y_ = vertices[i].tex_coord.y;
    }

    for (unsigned i = 0; i < indices.size(); ++i)
        compiledGeometry->indices_[i] = indices[i];
        
    return WrapGeometryHandle(compiledGeometry);
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto renderDevice = GetSubsystem<RenderDevice>();
    RenderContext* renderContext = renderDevice->GetRenderContext();
    
    CompiledRmlGeometry *compiledGeometry = UnwrapGeometryHandle(geometry);
    
    const auto [firstVertex, vertexData] = vertexBuffer_->AddVertices(compiledGeometry->vertices_.size());
    const auto [firstIndex, indexData] = indexBuffer_->AddIndices(compiledGeometry->indices_.size());
    
    RmlVertex* destVertices = reinterpret_cast<RmlVertex*>(vertexData);
    for (unsigned i = 0; i < compiledGeometry->vertices_.size(); ++i)
    {
        destVertices[i].position_.x_ = compiledGeometry->vertices_[i].position_.x_ + translation.x;
        destVertices[i].position_.y_ = compiledGeometry->vertices_[i].position_.y_ + translation.y;
        destVertices[i].position_.z_ = 0.0f;
        destVertices[i].color_ = compiledGeometry->vertices_[i].color_;
        destVertices[i].texCoord_.x_ = compiledGeometry->vertices_[i].texCoord_.x_;
        destVertices[i].texCoord_.y_ = compiledGeometry->vertices_[i].texCoord_.y_;
    }

    unsigned* destIndices = reinterpret_cast<unsigned*>(indexData);
    for (unsigned i = 0; i < compiledGeometry->indices_.size(); ++i)
        destIndices[i] = compiledGeometry->indices_[i] + firstVertex;
        
    // Restore texture data if lost
    CachedRmlTexture* cachedTexture = UnwrapTextureHandle(texture);
    Texture2D* texture2D = cachedTexture ? cachedTexture->texture_ : nullptr;
    if (texture2D && texture2D->IsDataLost())
    {
        texture2D->SetData(cachedTexture->image_);
        texture2D->ClearDataLost();
    }

    Material* material = GetBatchMaterial(texture2D);
    Pass* pass = material->GetDefaultPass();

    const unsigned samplerStateHash = texture2D ? texture2D->GetSamplerStateDesc().ToHash() : 0;
    batchStateCreateContext_.defaultSampler_ = texture2D ? &texture2D->GetSamplerStateDesc() : nullptr;

    const UIBatchStateKey batchStateKey{isRenderSurfaceSRGB_, renderContext->GetCurrentRenderTargetsDesc(), material,
        pass, BLEND_ALPHA, samplerStateHash};
    PipelineState* pipelineState = batchStateCache_->GetOrCreatePipelineState(batchStateKey, batchStateCreateContext_);

    IntRect scissor;
    if (!scissorEnabled_)
        scissor = IntRect{IntVector2::ZERO, viewportSize_};
    else if (transformEnabled_)
        scissor = TransformScissorRect(scissor_, transform_);
    else
        scissor = scissor_;

    drawQueue_->SetScissorRect(scissor);
    drawQueue_->SetPipelineState(pipelineState);

    if (texture)
    {
        drawQueue_->AddShaderResource(ShaderResources::Albedo, texture2D);
        textures_.emplace_back(texture2D);
    }
    drawQueue_->CommitShaderResources();

    if (drawQueue_->BeginShaderParameterGroup(SP_CAMERA, false))
    {
        drawQueue_->AddShaderParameter(VSP_VIEWPROJ, projection_);
        drawQueue_->CommitShaderParameterGroup(SP_CAMERA);
    }

    if (drawQueue_->BeginShaderParameterGroup(SP_MATERIAL, false))
    {
        drawQueue_->AddShaderParameter(PSP_MATDIFFCOLOR, Color::WHITE.ToVector4());
        drawQueue_->CommitShaderParameterGroup(SP_MATERIAL);
    }

    if (drawQueue_->BeginShaderParameterGroup(SP_OBJECT, true))
    {
        drawQueue_->AddShaderParameter(VSP_MODEL, transform_);
        drawQueue_->CommitShaderParameterGroup(SP_OBJECT);
    }

    drawQueue_->DrawIndexed(firstIndex, compiledGeometry->indices_.size());
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    CompiledRmlGeometry *compiledGeometry = UnwrapGeometryHandle(geometry);
    delete compiledGeometry;
}

void RmlRenderer::EnableScissorRegion(bool enable)
{
    scissorEnabled_ = enable;
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region)
{
    scissor_.left_ = region.Left();
    scissor_.top_ = region.Top();
    scissor_.bottom_ = region.Bottom();
    scissor_.right_ = region.Right();

    if (flipRect_)
    {
        int top = scissor_.top_;
        int bottom = scissor_.bottom_;
        scissor_.top_ = viewportSize_.y_ - bottom;
        scissor_.bottom_ = viewportSize_.y_ - top;
    }

    // TODO: Support transformed scissors by doing scissor test on CPU
}

void RmlRenderer::EnableClipMask(bool enable)
{
    EnableScissorRegion(enable);
}

void RmlRenderer::RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation)
{
    switch (operation)
	{
	case Rml::ClipMaskOperation::Set:
	case Rml::ClipMaskOperation::Intersect:
		// Intersect is considered like Set. This typically occurs in nested clipping situations, which never worked
		// correctly in legacy.
		break;
	case Rml::ClipMaskOperation::SetInverse:
		// Using features not supported in legacy, bail out.
		return;
	}

	// New features can render more complex clip masks, while legacy only supported rectangle scissoring. Find the
	// geometry's rectangular coverage.
	const CompiledRmlGeometry* compiledGeometry = UnwrapGeometryHandle(geometry);
    

	Rml::Rectanglef rectangle = Rml::Rectanglef::FromPosition(Vector3fToRmlVector2f(compiledGeometry->vertices_[0].position_));
	for (const RmlVertex& vertex : compiledGeometry->vertices_)
		rectangle.Join(Vector3fToRmlVector2f(vertex.position_));
	rectangle.Translate(translation);

	const Rml::Rectanglei scissor = Rml::Rectanglei(rectangle);
    SetScissorRegion(scissor);
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& size, const Rml::String& source)
{
    ResourceCache* cache = context_->GetSubsystem<ResourceCache>();
    Texture2D* texture = cache->GetResource<Texture2D>(source.c_str());
    if (texture)
    {
        size.x = texture->GetWidth();
        size.y = texture->GetHeight();
        texture->AddRef();
    }
    auto cachedTexture = new CachedRmlTexture{ nullptr, SharedPtr(texture) };
    return WrapTextureHandle(cachedTexture);
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i size)
{
    auto image = MakeShared<Image>(context_);
    image->SetSize(size.x, size.y, 4);
    image->SetData(source.data());

    auto texture = MakeShared<Texture2D>(context_);
    texture->SetData(image);

    auto cachedTexture = new CachedRmlTexture{ image, texture };
    return WrapTextureHandle(cachedTexture);
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle textureHandle)
{
    CachedRmlTexture* cachedTexture = UnwrapTextureHandle(textureHandle);
    delete cachedTexture;
}

void RmlRenderer::SetTransform(const Rml::Matrix4f* transform)
{
    transformEnabled_ = transform != nullptr;
    transform_ = transform ? Matrix3x4(Matrix4(transform->data())) : Matrix3x4::IDENTITY;
}

}   // namespace Detail

}   // namespace Urho3D
