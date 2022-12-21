/*
 * GLRenderSystem.cpp
 *
 * This file is part of the "LLGL" project (Copyright (c) 2015-2019 by Lukas Hermanns)
 * See "LICENSE.txt" for license information.
 */

#include "GLRenderSystem.h"
#include "GLProfile.h"
#include "Texture/GLMipGenerator.h"
#include "Texture/GLTextureViewPool.h"
#include "Ext/GLExtensions.h"
#include "Ext/GLExtensionRegistry.h"
#include "RenderState/GLStatePool.h"
#include "../RenderSystemUtils.h"
#include "GLTypes.h"
#include "GLCore.h"
#include "Buffer/GLBufferWithVAO.h"
#include "Buffer/GLBufferArrayWithVAO.h"
#include "../CheckedCast.h"
#include "../BufferUtils.h"
#include "../TextureUtils.h"
#include "../../Core/Helper.h"
#include "../../Core/Assertion.h"
#include "GLRenderingCaps.h"
#include "Command/GLImmediateCommandBuffer.h"
#include "Command/GLDeferredCommandBuffer.h"
#include "RenderState/GLGraphicsPSO.h"
#include "RenderState/GLComputePSO.h"


namespace LLGL
{


#ifdef LLGL_OPENGLES3
#define LLGL_GLES3_NOT_IMPLEMENTED \
    throw std::runtime_error("not implemented for GLES3: " + std::string(__FUNCTION__))
#endif

/* ----- Common ----- */

static RendererConfigurationOpenGL GetGLProfileFromDesc(const RenderSystemDescriptor& renderSystemDesc)
{
    if (auto rendererConfigGL = GetRendererConfiguration<RendererConfigurationOpenGL>(renderSystemDesc))
        return *rendererConfigGL;
    else
        return RendererConfigurationOpenGL{};
}

GLRenderSystem::GLRenderSystem(const RenderSystemDescriptor& renderSystemDesc) :
    contextMngr_ { GetGLProfileFromDesc(renderSystemDesc) }
{
}

GLRenderSystem::~GLRenderSystem()
{
    /* Clear all render state containers first, the rest will be deleted automatically */
    GLTextureViewPool::Get().Clear();
    GLMipGenerator::Get().Clear();
    GLStatePool::Get().Clear();
}

/* ----- Swap-chain ----- */

SwapChain* GLRenderSystem::CreateSwapChain(const SwapChainDescriptor& desc, const std::shared_ptr<Surface>& surface)
{
    return AddSwapChain(MakeUnique<GLSwapChain>(desc, surface, contextMngr_));
}

void GLRenderSystem::Release(SwapChain& swapChain)
{
    RemoveFromUniqueSet(swapChains_, &swapChain);
}

/* ----- Command queues ----- */

CommandQueue* GLRenderSystem::GetCommandQueue()
{
    return commandQueue_.get();
}

/* ----- Command buffers ----- */

CommandBuffer* GLRenderSystem::CreateCommandBuffer(const CommandBufferDescriptor& desc)
{
    /* Get state manager from swap-chain with shared GL context */
    if (auto currentGLContext = contextMngr_.AllocContext())
    {
        if ((desc.flags & CommandBufferFlags::ImmediateSubmit) != 0)
        {
            /* Create immediate command buffer */
            return TakeOwnership(
                commandBuffers_,
                MakeUnique<GLImmediateCommandBuffer>(currentGLContext->GetStateManager())
            );
        }
        else
        {
            /* Create deferred command buffer */
            return TakeOwnership(
                commandBuffers_,
                MakeUnique<GLDeferredCommandBuffer>(desc.flags)
            );
        }
    }
    else
        throw std::runtime_error("cannot create OpenGL command buffer without active render context");
}

void GLRenderSystem::Release(CommandBuffer& commandBuffer)
{
    RemoveFromUniqueSet(commandBuffers_, &commandBuffer);
}

/* ----- Buffers ------ */

static GLbitfield GetGLBufferStorageFlags(long cpuAccessFlags)
{
    #ifdef GL_ARB_buffer_storage

    GLbitfield flagsGL = 0;

    /* Allways enable dynamic storage, to enable usage of 'glBufferSubData' */
    flagsGL |= GL_DYNAMIC_STORAGE_BIT;

    if ((cpuAccessFlags & CPUAccessFlags::Read) != 0)
        flagsGL |= GL_MAP_READ_BIT;
    if ((cpuAccessFlags & CPUAccessFlags::Write) != 0)
        flagsGL |= GL_MAP_WRITE_BIT;

    return flagsGL;

    #else

    return 0;

    #endif // /GL_ARB_buffer_storage
}

static GLenum GetGLBufferUsage(long miscFlags)
{
    return ((miscFlags & MiscFlags::DynamicUsage) != 0 ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
}

static void GLBufferStorage(GLBuffer& bufferGL, const BufferDescriptor& desc, const void* initialData)
{
    bufferGL.BufferStorage(
        static_cast<GLsizeiptr>(desc.size),
        initialData,
        GetGLBufferStorageFlags(desc.cpuAccessFlags),
        GetGLBufferUsage(desc.miscFlags)
    );
}

Buffer* GLRenderSystem::CreateBuffer(const BufferDescriptor& desc, const void* initialData)
{
    AssertCreateBuffer(desc, static_cast<std::uint64_t>(std::numeric_limits<GLsizeiptr>::max()));

    auto bufferGL = CreateGLBuffer(desc, initialData);

    /* Store meta data for certain types of buffers */
    if ((desc.bindFlags & BindFlags::IndexBuffer) != 0 && desc.format != Format::Undefined)
        bufferGL->SetIndexType(desc.format);

    return bufferGL;
}

// private
GLBuffer* GLRenderSystem::CreateGLBuffer(const BufferDescriptor& desc, const void* initialData)
{
    /* Create either base of sub-class GLBuffer object */
    if ((desc.bindFlags & BindFlags::VertexBuffer) != 0)
    {
        /* Create buffer with VAO and build vertex array */
        auto bufferGL = MakeUnique<GLBufferWithVAO>(desc.bindFlags);
        {
            GLBufferStorage(*bufferGL, desc, initialData);
            bufferGL->BuildVertexArray(desc.vertexAttribs.size(), desc.vertexAttribs.data());
        }
        return TakeOwnership(buffers_, std::move(bufferGL));
    }
    else
    {
        /* Create generic buffer */
        auto bufferGL = MakeUnique<GLBuffer>(desc.bindFlags);
        {
            GLBufferStorage(*bufferGL, desc, initialData);
        }
        return TakeOwnership(buffers_, std::move(bufferGL));
    }
}

// Returns true if at least one of the buffers in the specified array has a VertexBuffer binding flag.
static bool IsBufferArrayWithVertexBufferBinding(std::uint32_t numBuffers, Buffer* const * bufferArray)
{
    for (std::uint32_t i = 0; i < numBuffers; ++i)
    {
        if ((bufferArray[i]->GetBindFlags() & BindFlags::VertexBuffer) != 0)
            return true;
    }
    return false;
}

BufferArray* GLRenderSystem::CreateBufferArray(std::uint32_t numBuffers, Buffer* const * bufferArray)
{
    AssertCreateBufferArray(numBuffers, bufferArray);

    if (IsBufferArrayWithVertexBufferBinding(numBuffers, bufferArray))
    {
        /* Create vertex buffer array and build VAO */
        auto vertexBufferArray = MakeUnique<GLBufferArrayWithVAO>(numBuffers, bufferArray);
        return TakeOwnership(bufferArrays_, std::move(vertexBufferArray));
    }

    return TakeOwnership(bufferArrays_, MakeUnique<GLBufferArray>(numBuffers, bufferArray));
}

void GLRenderSystem::Release(Buffer& buffer)
{
    RemoveFromUniqueSet(buffers_, &buffer);
}

void GLRenderSystem::Release(BufferArray& bufferArray)
{
    RemoveFromUniqueSet(bufferArrays_, &bufferArray);
}

void GLRenderSystem::WriteBuffer(Buffer& dstBuffer, std::uint64_t dstOffset, const void* data, std::uint64_t dataSize)
{
    auto& dstBufferGL = LLGL_CAST(GLBuffer&, dstBuffer);
    dstBufferGL.BufferSubData(static_cast<GLintptr>(dstOffset), static_cast<GLsizeiptr>(dataSize), data);
}

void* GLRenderSystem::MapBuffer(Buffer& buffer, const CPUAccess access)
{
    auto& bufferGL = LLGL_CAST(GLBuffer&, buffer);
    return bufferGL.MapBuffer(GLTypes::Map(access));
}

static GLbitfield ToGLMapBufferAccess(CPUAccess access)
{
    switch (access)
    {
        case CPUAccess::ReadOnly:       return GL_MAP_READ_BIT;
        case CPUAccess::WriteOnly:      return GL_MAP_WRITE_BIT;
        case CPUAccess::WriteDiscard:   return GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT;
        case CPUAccess::ReadWrite:      return GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;
        default:                        return 0;
    }
}

void* GLRenderSystem::MapBuffer(Buffer& buffer, const CPUAccess access, std::uint64_t offset, std::uint64_t length)
{
    auto& bufferGL = LLGL_CAST(GLBuffer&, buffer);
    return bufferGL.MapBufferRange(static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(length), ToGLMapBufferAccess(access));
}

void GLRenderSystem::UnmapBuffer(Buffer& buffer)
{
    auto& bufferGL = LLGL_CAST(GLBuffer&, buffer);
    bufferGL.UnmapBuffer();
}

/* ----- Textures ----- */

//private
void GLRenderSystem::ValidateGLTextureType(const TextureType type)
{
    /* Validate texture type for this GL device */
    switch (type)
    {
        case TextureType::Texture1D:
        case TextureType::Texture2D:
            break;

        case TextureType::Texture3D:
            LLGL_ASSERT_FEATURE_SUPPORT(has3DTextures);
            break;

        case TextureType::TextureCube:
            LLGL_ASSERT_FEATURE_SUPPORT(hasCubeTextures);
            break;

        case TextureType::Texture1DArray:
        case TextureType::Texture2DArray:
            LLGL_ASSERT_FEATURE_SUPPORT(hasArrayTextures);
            break;

        case TextureType::TextureCubeArray:
            LLGL_ASSERT_FEATURE_SUPPORT(hasCubeArrayTextures);
            break;

        case TextureType::Texture2DMS:
        case TextureType::Texture2DMSArray:
            LLGL_ASSERT_FEATURE_SUPPORT(hasMultiSampleTextures);
            break;

        default:
            throw std::invalid_argument("failed to create texture with invalid texture type");
            break;
    }
}

Texture* GLRenderSystem::CreateTexture(const TextureDescriptor& textureDesc, const SrcImageDescriptor* imageDesc)
{
    ValidateGLTextureType(textureDesc.type);

    /* Create <GLTexture> object; will result in a GL renderbuffer or texture instance */
    auto texture = MakeUnique<GLTexture>(textureDesc);

    /* Initialize either renderbuffer or texture image storage */
    texture->BindAndAllocStorage(textureDesc, imageDesc);

    return TakeOwnership(textures_, std::move(texture));
}

void GLRenderSystem::Release(Texture& texture)
{
    RemoveFromUniqueSet(textures_, &texture);
}

void GLRenderSystem::WriteTexture(Texture& texture, const TextureRegion& textureRegion, const SrcImageDescriptor& imageDesc)
{
    /* Bind texture and write texture sub data */
    auto& textureGL = LLGL_CAST(GLTexture&, texture);
    textureGL.TextureSubImage(textureRegion, imageDesc, false);
}

void GLRenderSystem::ReadTexture(Texture& texture, const TextureRegion& textureRegion, const DstImageDescriptor& imageDesc)
{
    /* Bind texture and write texture sub data */
    LLGL_ASSERT_PTR(imageDesc.data);
    auto& textureGL = LLGL_CAST(GLTexture&, texture);
    textureGL.GetTextureSubImage(textureRegion, imageDesc, false);
}

/* ----- Sampler States ---- */

Sampler* GLRenderSystem::CreateSampler(const SamplerDescriptor& desc)
{
    #ifdef LLGL_GL_ENABLE_OPENGL2X
    /* If GL_ARB_sampler_objects is not supported, use emulated sampler states */
    if (!HasNativeSamplers())
    {
        auto samplerGL2X = MakeUnique<GL2XSampler>();
        samplerGL2X->SetDesc(desc);
        return TakeOwnership(samplersGL2X_, std::move(samplerGL2X));
    }
    #endif

    /* Create native GL sampler state */
    LLGL_ASSERT_FEATURE_SUPPORT(hasSamplers);
    auto sampler = MakeUnique<GLSampler>();
    sampler->SetDesc(desc);
    return TakeOwnership(samplers_, std::move(sampler));
}

void GLRenderSystem::Release(Sampler& sampler)
{
    #ifdef LLGL_GL_ENABLE_OPENGL2X
    /* If GL_ARB_sampler_objects is not supported, release emulated sampler states */
    if (!HasNativeSamplers())
        RemoveFromUniqueSet(samplersGL2X_, &sampler);
    else
        RemoveFromUniqueSet(samplers_, &sampler);
    #else
    RemoveFromUniqueSet(samplers_, &sampler);
    #endif
}

/* ----- Resource Heaps ----- */

ResourceHeap* GLRenderSystem::CreateResourceHeap(const ResourceHeapDescriptor& desc)
{
    return TakeOwnership(resourceHeaps_, MakeUnique<GLResourceHeap>(desc));
}

void GLRenderSystem::Release(ResourceHeap& resourceHeap)
{
    RemoveFromUniqueSet(resourceHeaps_, &resourceHeap);
}

/* ----- Render Passes ----- */

RenderPass* GLRenderSystem::CreateRenderPass(const RenderPassDescriptor& desc)
{
    AssertCreateRenderPass(desc);
    return TakeOwnership(renderPasses_, MakeUnique<GLRenderPass>(desc));
}

void GLRenderSystem::Release(RenderPass& renderPass)
{
    RemoveFromUniqueSet(renderPasses_, &renderPass);
}

/* ----- Render Targets ----- */

RenderTarget* GLRenderSystem::CreateRenderTarget(const RenderTargetDescriptor& desc)
{
    LLGL_ASSERT_FEATURE_SUPPORT(hasRenderTargets);
    AssertCreateRenderTarget(desc);
    return TakeOwnership(renderTargets_, MakeUnique<GLRenderTarget>(desc));
}

void GLRenderSystem::Release(RenderTarget& renderTarget)
{
    RemoveFromUniqueSet(renderTargets_, &renderTarget);
}

/* ----- Shader ----- */

Shader* GLRenderSystem::CreateShader(const ShaderDescriptor& desc)
{
    AssertCreateShader(desc);

    /* Validate rendering capabilities for required shader type */
    switch (desc.type)
    {
        case ShaderType::Geometry:
            LLGL_ASSERT_FEATURE_SUPPORT(hasGeometryShaders);
            break;
        case ShaderType::TessControl:
        case ShaderType::TessEvaluation:
            LLGL_ASSERT_FEATURE_SUPPORT(hasTessellationShaders);
            break;
        case ShaderType::Compute:
            LLGL_ASSERT_FEATURE_SUPPORT(hasComputeShaders);
            break;
        default:
            break;
    }

    /* Make and return shader object */
    return TakeOwnership(shaders_, MakeUnique<GLShader>(desc));
}

ShaderProgram* GLRenderSystem::CreateShaderProgram(const ShaderProgramDescriptor& desc)
{
    AssertCreateShaderProgram(desc);
    return TakeOwnership(shaderPrograms_, MakeUnique<GLShaderProgram>(desc));
}

void GLRenderSystem::Release(Shader& shader)
{
    RemoveFromUniqueSet(shaders_, &shader);
}

void GLRenderSystem::Release(ShaderProgram& shaderProgram)
{
    RemoveFromUniqueSet(shaderPrograms_, &shaderProgram);
}

/* ----- Pipeline Layouts ----- */

PipelineLayout* GLRenderSystem::CreatePipelineLayout(const PipelineLayoutDescriptor& desc)
{
    return TakeOwnership(pipelineLayouts_, MakeUnique<GLPipelineLayout>(desc));
}

void GLRenderSystem::Release(PipelineLayout& pipelineLayout)
{
    RemoveFromUniqueSet(pipelineLayouts_, &pipelineLayout);
}

/* ----- Pipeline States ----- */

PipelineState* GLRenderSystem::CreatePipelineState(const Blob& /*serializedCache*/)
{
    return nullptr;//TODO
}

PipelineState* GLRenderSystem::CreatePipelineState(const GraphicsPipelineDescriptor& desc, std::unique_ptr<Blob>* /*serializedCache*/)
{
    return TakeOwnership(pipelineStates_, MakeUnique<GLGraphicsPSO>(desc, GetRenderingCaps().limits));
}

PipelineState* GLRenderSystem::CreatePipelineState(const ComputePipelineDescriptor& desc, std::unique_ptr<Blob>* /*serializedCache*/)
{
    return TakeOwnership(pipelineStates_, MakeUnique<GLComputePSO>(desc));
}

void GLRenderSystem::Release(PipelineState& pipelineState)
{
    RemoveFromUniqueSet(pipelineStates_, &pipelineState);
}

/* ----- Queries ----- */

QueryHeap* GLRenderSystem::CreateQueryHeap(const QueryHeapDescriptor& desc)
{
    return TakeOwnership(queryHeaps_, MakeUnique<GLQueryHeap>(desc));
}

void GLRenderSystem::Release(QueryHeap& queryHeap)
{
    RemoveFromUniqueSet(queryHeaps_, &queryHeap);
}

/* ----- Fences ----- */

Fence* GLRenderSystem::CreateFence()
{
    return TakeOwnership(fences_, MakeUnique<GLFence>());
}

void GLRenderSystem::Release(Fence& fence)
{
    RemoveFromUniqueSet(fences_, &fence);
}


/*
 * ======= Protected: =======
 */

SwapChain* GLRenderSystem::AddSwapChain(std::unique_ptr<GLSwapChain>&& swapChain)
{
    /* Create devices that require an active GL context */
    if (swapChains_.empty())
        CreateGLContextDependentDevices(swapChain->GetStateManager());

    /* Take ownership and return raw pointer */
    return TakeOwnership(swapChains_, std::move(swapChain));
}


/*
 * ======= Private: =======
 */

void GLRenderSystem::CreateGLContextDependentDevices(GLStateManager& stateManager)
{
    /* Enable debug callback function */
    if (debugCallback_)
        SetDebugCallback(debugCallback_);

    /* Create command queue instance */
    commandQueue_ = MakeUnique<GLCommandQueue>(stateManager);

    /* Query renderer information and limits */
    QueryRendererInfo();
    QueryRenderingCaps();
}

#ifdef GL_KHR_debug

void APIENTRY GLDebugCallback(
    GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    /* Generate output stream */
    std::stringstream typeStr;

    typeStr
        << "OpenGL debug callback ("
        << GLDebugSourceToStr(source) << ", "
        << GLDebugTypeToStr(type) << ", "
        << GLDebugSeverityToStr(severity) << ")";

    /* Call debug callback */
    auto debugCallback = reinterpret_cast<const DebugCallback*>(userParam);
    (*debugCallback)(typeStr.str(), message);
}

#endif // /GL_KHR_debug

void GLRenderSystem::SetDebugCallback(const DebugCallback& debugCallback)
{
    #ifdef GL_KHR_debug
    if (HasExtension(GLExt::KHR_debug))
    {
        debugCallback_ = debugCallback;
        if (debugCallback_)
        {
            GLStateManager::Get().Enable(GLState::DEBUG_OUTPUT);
            GLStateManager::Get().Enable(GLState::DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(GLDebugCallback, &debugCallback_);
        }
        else
        {
            GLStateManager::Get().Disable(GLState::DEBUG_OUTPUT);
            GLStateManager::Get().Disable(GLState::DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(nullptr, nullptr);
        }
    }
    #endif // /GL_KHR_debug
}

static std::string GLGetString(GLenum name)
{
    auto bytes = glGetString(name);
    return (bytes != nullptr ? std::string(reinterpret_cast<const char*>(bytes)) : "");
}

void GLRenderSystem::QueryRendererInfo()
{
    RendererInfo info;

    info.rendererName           = GLProfile::GetAPIName() + std::string(" ") + GLGetString(GL_VERSION);
    info.deviceName             = GLGetString(GL_RENDERER);
    info.vendorName             = GLGetString(GL_VENDOR);
    info.shadingLanguageName    = GLProfile::GetShadingLanguageName() + std::string(" ") + GLGetString(GL_SHADING_LANGUAGE_VERSION);

    SetRendererInfo(info);
}

void GLRenderSystem::QueryRenderingCaps()
{
    RenderingCapabilities caps;
    GLQueryRenderingCaps(caps);
    SetRenderingCaps(caps);
}


} // /namespace LLGL



// ================================================================================
