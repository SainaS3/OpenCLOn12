// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#include "kernel.hpp"
#include "sampler.hpp"
#include "clc_compiler.h"

extern CL_API_ENTRY cl_kernel CL_API_CALL
clCreateKernel(cl_program      program_,
    const char* kernel_name,
    cl_int* errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        if (errcode_ret) *errcode_ret = CL_INVALID_PROGRAM;
        return nullptr;
    }

    Program& program = *static_cast<Program*>(program_);
    auto ReportError = program.GetContext().GetErrorReporter(errcode_ret);
    const clc_dxil_object* kernel = nullptr;

    {
        std::lock_guard Lock(program.m_Lock);
        cl_uint DeviceCountWithProgram = 0, DeviceCountWithKernel = 0;
        for (auto& Device : program.m_AssociatedDevices)
        {
            auto& BuildData = program.m_BuildData[Device.Get()];
            if (!BuildData ||
                BuildData->m_BuildStatus != CL_BUILD_SUCCESS ||
                BuildData->m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
            {
                continue;
            }

            ++DeviceCountWithProgram;
            auto iter = BuildData->m_Kernels.find(kernel_name);
            if (iter == BuildData->m_Kernels.end())
            {
                continue;
            }

            ++DeviceCountWithKernel;
            if (kernel)
            {
                if (kernel->kernel->num_args != iter->second.m_GenericDxil->kernel->num_args)
                {
                    return ReportError("Kernel argument count differs between devices.", CL_INVALID_KERNEL_DEFINITION);
                }
                for (unsigned i = 0; i < kernel->kernel->num_args; ++i)
                {
                    auto& a = kernel->kernel->args[i];
                    auto& b = iter->second.m_GenericDxil->kernel->args[i];
                    if (strcmp(a.type_name, b.type_name) != 0 ||
                        strcmp(a.name, b.name) != 0 ||
                        a.address_qualifier != b.address_qualifier ||
                        a.access_qualifier != b.access_qualifier ||
                        a.type_qualifier != b.type_qualifier)
                    {
                        return ReportError("Kernel argument differs between devices.", CL_INVALID_KERNEL_DEFINITION);
                    }
                }
            }
            kernel = iter->second.m_GenericDxil.get();
            if (!kernel)
            {
                return ReportError("Kernel failed to compile.", CL_OUT_OF_RESOURCES);
            }
        }
        if (!DeviceCountWithProgram)
        {
            return ReportError("No executable available for program.", CL_INVALID_PROGRAM_EXECUTABLE);
        }
        if (!DeviceCountWithKernel)
        {
            return ReportError("No kernel with that name found.", CL_INVALID_KERNEL_NAME);
        }
    }

    try
    {
        return new Kernel(program, kernel_name, kernel);
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clCreateKernelsInProgram(cl_program     program_,
    cl_uint        num_kernels,
    cl_kernel* kernels,
    cl_uint* num_kernels_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!program_)
    {
        return CL_INVALID_PROGRAM;
    }

    Program& program = *static_cast<Program*>(program_);
    auto ReportError = program.GetContext().GetErrorReporter();

    try
    {
        std::map<std::string, Kernel::ref_ptr> temp;

        {
            std::lock_guard Lock(program.m_Lock);
            for (auto& Device : program.m_AssociatedDevices)
            {
                auto& BuildData = program.m_BuildData[Device.Get()];
                if (!BuildData ||
                    BuildData->m_BuildStatus != CL_BUILD_SUCCESS ||
                    BuildData->m_BinaryType != CL_PROGRAM_BINARY_TYPE_EXECUTABLE)
                {
                    continue;
                }

                for (auto& pair : BuildData->m_Kernels)
                {
                    temp.emplace(pair.first, nullptr);
                }
            }
            if (temp.empty())
            {
                return ReportError("No executable available for program.", CL_INVALID_PROGRAM_EXECUTABLE);
            }
            if (num_kernels && num_kernels < temp.size())
            {
                return ReportError("num_kernels is too small.", CL_INVALID_VALUE);
            }
        }
        if (num_kernels_ret)
        {
            *num_kernels_ret = (cl_uint)temp.size();
        }

        if (num_kernels)
        {
            for (auto& pair : temp)
            {
                cl_int error = CL_SUCCESS;
                pair.second.Attach(static_cast<Kernel*>(clCreateKernel(program_, pair.first.c_str(), &error)));
                if (error != CL_SUCCESS)
                {
                    return error;
                }
            }
            for (auto& pair : temp)
            {
                *kernels = pair.second.Detach();
                ++kernels;
            }
        }
    }
    catch (std::bad_alloc&) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception & e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clRetainKernel(cl_kernel    kernel) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }
    static_cast<Kernel*>(kernel)->Retain();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clReleaseKernel(cl_kernel   kernel) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }
    static_cast<Kernel*>(kernel)->Release();
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clSetKernelArg(cl_kernel    kernel,
    cl_uint      arg_index,
    size_t       arg_size,
    const void* arg_value) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel)
    {
        return CL_INVALID_KERNEL;
    }
    return static_cast<Kernel*>(kernel)->SetArg(arg_index, arg_size, arg_value);
}

static cl_mem_object_type MemObjectTypeFromName(const char* name)
{
    if (strcmp(name, "image1d_buffer_t") == 0) return CL_MEM_OBJECT_IMAGE1D_BUFFER;
    if (strcmp(name, "image1d_t") == 0) return CL_MEM_OBJECT_IMAGE1D;
    if (strcmp(name, "image1d_array_t") == 0) return CL_MEM_OBJECT_IMAGE1D_ARRAY;
    if (strcmp(name, "image2d_t") == 0) return CL_MEM_OBJECT_IMAGE2D;
    if (strcmp(name, "image2d_array_t") == 0) return CL_MEM_OBJECT_IMAGE2D_ARRAY;
    if (strcmp(name, "image3d_t") == 0) return CL_MEM_OBJECT_IMAGE3D;
    return 0;
}

static D3D12TranslationLayer::RESOURCE_DIMENSION ResourceDimensionFromMemObjectType(cl_mem_object_type type)
{
    switch (type)
    {
    case CL_MEM_OBJECT_IMAGE1D: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE1D;
    case CL_MEM_OBJECT_IMAGE1D_ARRAY: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE1DARRAY;
    case CL_MEM_OBJECT_IMAGE1D_BUFFER: return D3D12TranslationLayer::RESOURCE_DIMENSION::BUFFER;
    case CL_MEM_OBJECT_IMAGE2D: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE2D;
    case CL_MEM_OBJECT_IMAGE2D_ARRAY: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE2DARRAY;
    case CL_MEM_OBJECT_IMAGE3D: return D3D12TranslationLayer::RESOURCE_DIMENSION::TEXTURE3D;
    }
    return D3D12TranslationLayer::RESOURCE_DIMENSION::UNKNOWN;
}

static D3D12TranslationLayer::SShaderDecls DeclsFromMetadata(clc_dxil_object const* pDxil)
{
    auto& metadata = pDxil->metadata;
    D3D12TranslationLayer::SShaderDecls decls = {};
    cl_uint KernelArgCBIndex = metadata.kernel_inputs_cbv_id;
    cl_uint WorkPropertiesCBIndex = metadata.work_properties_cbv_id;
    decls.m_NumCBs = max(KernelArgCBIndex + 1, WorkPropertiesCBIndex + 1);
    decls.m_NumSamplers = (UINT)metadata.num_samplers;
    decls.m_ResourceDecls.resize(metadata.num_srvs);
    decls.m_UAVDecls.resize(metadata.num_uavs);

    for (cl_uint i = 0; i < pDxil->kernel->num_args; ++i)
    {
        auto& arg = pDxil->kernel->args[i];
        if (arg.address_qualifier == CLC_KERNEL_ARG_ADDRESS_GLOBAL ||
            arg.address_qualifier == CLC_KERNEL_ARG_ADDRESS_CONSTANT)
        {
            cl_mem_object_type imageType = MemObjectTypeFromName(arg.type_name);
            if (imageType != 0)
            {
                auto dim = ResourceDimensionFromMemObjectType(imageType);
                bool uav = (arg.access_qualifier & CLC_KERNEL_ARG_ACCESS_WRITE) != 0;
                auto& declVector = uav ? decls.m_UAVDecls : decls.m_ResourceDecls;
                for (cl_uint j = 0; j < metadata.args[i].image.num_buf_ids; ++j)
                    declVector[metadata.args[i].image.buf_ids[j]] = dim;
            }
            else
            {
                decls.m_UAVDecls[metadata.args[i].globconstptr.buf_id] =
                    D3D12TranslationLayer::RESOURCE_DIMENSION::BUFFER;
            }
        }
    }
    return decls;
}

static cl_addressing_mode CLAddressingModeFromSpirv(unsigned addressing_mode)
{
    return addressing_mode + CL_ADDRESS_NONE;
}

static unsigned SpirvAddressingModeFromCL(cl_addressing_mode mode)
{
    return mode - CL_ADDRESS_NONE;
}

static cl_filter_mode CLFilterModeFromSpirv(unsigned filter_mode)
{
    return filter_mode + CL_FILTER_NEAREST;
}

Kernel::Kernel(Program& Parent, std::string const& name, clc_dxil_object const* pDxil)
    : CLChildBase(Parent)
    , m_pDxil(pDxil)
    , m_Name(name)
    , m_ShaderDecls(DeclsFromMetadata(pDxil))
{
    m_UAVs.resize(m_pDxil->metadata.num_uavs);
    m_SRVs.resize(m_pDxil->metadata.num_srvs);
    m_Samplers.resize(m_pDxil->metadata.num_samplers);
    m_ArgMetadataToCompiler.resize(m_pDxil->kernel->num_args);
    size_t KernelInputsCbSize = m_pDxil->metadata.kernel_inputs_buf_size;
    m_KernelArgsCbData.resize(KernelInputsCbSize);

    m_ConstSamplers.resize(m_pDxil->metadata.num_const_samplers);
    for (cl_uint i = 0; i < m_pDxil->metadata.num_const_samplers; ++i)
    {
        auto& samplerMeta = m_pDxil->metadata.const_samplers[i];
        Sampler::Desc desc =
        {
            samplerMeta.normalized_coords,
            CLAddressingModeFromSpirv(samplerMeta.addressing_mode),
            CLFilterModeFromSpirv(samplerMeta.filter_mode)
        };
        m_ConstSamplers[i] = new Sampler(m_Parent->GetContext(), desc);
        m_Samplers[samplerMeta.sampler_id] = m_ConstSamplers[i].Get();
    }

    for (cl_uint i = 0; i < m_pDxil->metadata.num_consts; ++i)
    {
        auto& constMeta = m_pDxil->metadata.consts[i];
        auto resource = static_cast<Resource*>(clCreateBuffer(&Parent.GetContext(),
                                                              CL_MEM_COPY_HOST_PTR | CL_MEM_READ_ONLY | CL_MEM_HOST_NO_ACCESS,
                                                              constMeta.size, constMeta.data,
                                                              nullptr));
        m_InlineConsts.emplace_back(resource, adopt_ref{});
        m_UAVs[constMeta.uav_id] = resource;
    }

    m_Parent->KernelCreated();
}

Kernel::~Kernel()
{
    m_Parent->KernelFreed();
}

cl_int Kernel::SetArg(cl_uint arg_index, size_t arg_size, const void* arg_value)
{
    auto ReportError = m_Parent->GetContext().GetErrorReporter();
    if (arg_index > m_pDxil->kernel->num_args)
    {
        return ReportError("Argument index out of bounds", CL_INVALID_ARG_INDEX);
    }

    auto& arg = m_pDxil->kernel->args[arg_index];
    switch (arg.address_qualifier)
    {
    case CLC_KERNEL_ARG_ADDRESS_GLOBAL:
    case CLC_KERNEL_ARG_ADDRESS_CONSTANT:
    {
        if (arg_size != sizeof(cl_mem))
        {
            return ReportError("Invalid argument size, must be sizeof(cl_mem) for global and constant arguments", CL_INVALID_ARG_SIZE);
        }

        cl_mem_object_type imageType = MemObjectTypeFromName(arg.type_name);
        cl_mem mem = arg_value ? *reinterpret_cast<cl_mem const*>(arg_value) : nullptr;
        Resource* resource = static_cast<Resource*>(mem);
        if (imageType != 0)
        {
            bool validImageType = true;
            if (resource)
            {
                validImageType = resource->m_Desc.image_type == imageType;
            }

            if (!validImageType)
            {
                return ReportError("Invalid image type.", CL_INVALID_ARG_VALUE);
            }

            if (arg.access_qualifier & CLC_KERNEL_ARG_ACCESS_WRITE)
            {
                if (resource && (resource->m_Flags & CL_MEM_READ_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding read-only image to writable image argument.", CL_INVALID_ARG_VALUE);
                }
                if ((arg.access_qualifier & CLC_KERNEL_ARG_ACCESS_READ) != 0 &&
                    resource && (resource->m_Flags & CL_MEM_WRITE_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding write-only image to read-write image argument.", CL_INVALID_ARG_VALUE);
                }
                for (cl_uint i = 0; i < m_pDxil->metadata.args[arg_index].image.num_buf_ids; ++i)
                {
                    m_UAVs[m_pDxil->metadata.args[arg_index].image.buf_ids[i]] = resource;
                }
            }
            else
            {
                if (resource && (resource->m_Flags & CL_MEM_WRITE_ONLY))
                {
                    return ReportError("Invalid mem object flags, binding write-only image to read-only image argument.", CL_INVALID_ARG_VALUE);
                }
                for (cl_uint i = 0; i < m_pDxil->metadata.args[arg_index].image.num_buf_ids; ++i)
                {
                    m_SRVs[m_pDxil->metadata.args[arg_index].image.buf_ids[i]] = resource;
                }
            }

            // Store image format in the kernel args
            cl_image_format* ImageFormatInKernelArgs = reinterpret_cast<cl_image_format*>(
                m_KernelArgsCbData.data() + m_pDxil->metadata.args[arg_index].offset);
            *ImageFormatInKernelArgs = {};
            if (resource)
            {
                *ImageFormatInKernelArgs = resource->m_Format;
                // The SPIR-V expects the values coming from the intrinsics to be 0-indexed, and implicitly
                // adds the necessary values to put it back into the CL constant range
                ImageFormatInKernelArgs->image_channel_data_type -= CL_SNORM_INT8;
                ImageFormatInKernelArgs->image_channel_order -= CL_R;
            }
        }
        else
        {
            if (resource && resource->m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
            {
                return ReportError("Invalid mem object type, must be buffer.", CL_INVALID_ARG_VALUE);
            }
            uint64_t *buffer_val = reinterpret_cast<uint64_t*>(m_KernelArgsCbData.data() + m_pDxil->metadata.args[arg_index].offset);
            auto buf_id = m_pDxil->metadata.args[arg_index].globconstptr.buf_id;
            m_UAVs[buf_id] = resource;
            if (resource)
            {
                *buffer_val = (uint64_t)buf_id << 32ull;
            }
            else
            {
                *buffer_val = ~0ull;
            }
        }

        break;
    }

    case CLC_KERNEL_ARG_ADDRESS_PRIVATE:
        if (strcmp(arg.type_name, "sampler_t") == 0)
        {
            if (arg_size != sizeof(cl_sampler))
            {
                return ReportError("Invalid argument size, must be sizeof(cl_mem) for global arguments", CL_INVALID_ARG_SIZE);
            }
            cl_sampler samp = arg_value ? *reinterpret_cast<cl_sampler const*>(arg_value) : nullptr;
            Sampler* sampler = static_cast<Sampler*>(samp);
            m_Samplers[m_pDxil->metadata.args[arg_index].sampler.sampler_id] = sampler;
            m_ArgMetadataToCompiler[arg_index].sampler.normalized_coords = sampler ? sampler->m_Desc.NormalizedCoords : 1u;
            m_ArgMetadataToCompiler[arg_index].sampler.addressing_mode = sampler ? SpirvAddressingModeFromCL(sampler->m_Desc.AddressingMode) : 0u;
            m_ArgMetadataToCompiler[arg_index].sampler.linear_filtering = sampler ? (sampler->m_Desc.FilterMode == CL_FILTER_LINEAR) : 0u;
        }
        else
        {
            if (arg_size != m_pDxil->metadata.args[arg_index].size)
            {
                return ReportError("Invalid argument size", CL_INVALID_ARG_SIZE);
            }
            memcpy(m_KernelArgsCbData.data() + m_pDxil->metadata.args[arg_index].offset, arg_value, arg_size);
        }
        break;

    case CLC_KERNEL_ARG_ADDRESS_LOCAL:
        if (arg_size == 0)
        {
            return ReportError("Argument size must be nonzero for local arguments", CL_INVALID_ARG_SIZE);
        }
        if (arg_value != nullptr)
        {
            return ReportError("Argument value must be null for local arguments", CL_INVALID_ARG_VALUE);
        }
        m_ArgMetadataToCompiler[arg_index].localptr.size = (cl_uint)arg_size;
        break;
    }

    return CL_SUCCESS;
}

uint16_t const* Kernel::GetRequiredLocalDims() const
{
    if (m_pDxil->metadata.local_size[0] != 0)
        return m_pDxil->metadata.local_size;
    return nullptr;
}

uint16_t const* Kernel::GetLocalDimsHint() const
{
    if (m_pDxil->metadata.local_size_hint[0] != 0)
        return m_pDxil->metadata.local_size_hint;
    return nullptr;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelInfo(cl_kernel       kernel_,
    cl_kernel_info  param_name,
    size_t          param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);
    switch (param_name)
    {
    case CL_KERNEL_FUNCTION_NAME: return RetValue(kernel.m_pDxil->kernel->name);
    case CL_KERNEL_NUM_ARGS: return RetValue((cl_uint)kernel.m_pDxil->kernel->num_args);
    case CL_KERNEL_REFERENCE_COUNT: return RetValue(kernel.GetRefCount());
    case CL_KERNEL_CONTEXT: return RetValue((cl_context)&kernel.m_Parent->m_Parent.get());
    case CL_KERNEL_PROGRAM: return RetValue((cl_program)&kernel.m_Parent.get());
    case CL_KERNEL_ATTRIBUTES: return RetValue("");
    }

    return kernel.m_Parent->GetContext().GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelArgInfo(cl_kernel       kernel_,
    cl_uint         arg_indx,
    cl_kernel_arg_info  param_name,
    size_t          param_value_size,
    void* param_value,
    size_t* param_value_size_ret) CL_API_SUFFIX__VERSION_1_2
{
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);
    
    if (arg_indx > kernel.m_pDxil->kernel->num_args)
    {
        return CL_INVALID_ARG_INDEX;
    }

    auto& arg = kernel.m_pDxil->kernel->args[arg_indx];
    switch (param_name)
    {
    case CL_KERNEL_ARG_ADDRESS_QUALIFIER:
        switch (arg.address_qualifier)
        {
        default:
        case CLC_KERNEL_ARG_ADDRESS_PRIVATE: return RetValue(CL_KERNEL_ARG_ADDRESS_PRIVATE);
        case CLC_KERNEL_ARG_ADDRESS_CONSTANT: return RetValue(CL_KERNEL_ARG_ADDRESS_CONSTANT);
        case CLC_KERNEL_ARG_ADDRESS_LOCAL: return RetValue(CL_KERNEL_ARG_ADDRESS_LOCAL);
        case CLC_KERNEL_ARG_ADDRESS_GLOBAL: return RetValue(CL_KERNEL_ARG_ADDRESS_GLOBAL);
        }
        break;
    case CL_KERNEL_ARG_ACCESS_QUALIFIER:
        switch (arg.access_qualifier)
        {
        default: return RetValue(CL_KERNEL_ARG_ACCESS_NONE);
        case CLC_KERNEL_ARG_ACCESS_READ: return RetValue(CL_KERNEL_ARG_ACCESS_READ_ONLY);
        case CLC_KERNEL_ARG_ACCESS_WRITE: return RetValue(CL_KERNEL_ARG_ACCESS_WRITE_ONLY);
        case CLC_KERNEL_ARG_ACCESS_READ | CLC_KERNEL_ARG_ACCESS_WRITE: return RetValue(CL_KERNEL_ARG_ACCESS_READ_WRITE);
        }
    case CL_KERNEL_ARG_TYPE_NAME: return RetValue(arg.type_name);
    case CL_KERNEL_ARG_TYPE_QUALIFIER:
    {
        cl_kernel_arg_type_qualifier qualifier = CL_KERNEL_ARG_TYPE_NONE;
        if ((arg.type_qualifier & CLC_KERNEL_ARG_TYPE_CONST) ||
            arg.address_qualifier == CLC_KERNEL_ARG_ADDRESS_CONSTANT) qualifier |= CL_KERNEL_ARG_TYPE_CONST;
        if (arg.type_qualifier & CLC_KERNEL_ARG_TYPE_RESTRICT) qualifier |= CL_KERNEL_ARG_TYPE_RESTRICT;
        if (arg.type_qualifier & CLC_KERNEL_ARG_TYPE_VOLATILE) qualifier |= CL_KERNEL_ARG_TYPE_VOLATILE;
        return RetValue(qualifier);
    }
    case CL_KERNEL_ARG_NAME:
        if (arg.name) return RetValue(arg.name);
        return CL_KERNEL_ARG_INFO_NOT_AVAILABLE;
    }

    return kernel.m_Parent->GetContext().GetErrorReporter()("Unknown param_name", CL_INVALID_VALUE);
}

extern CL_API_ENTRY cl_int CL_API_CALL
clGetKernelWorkGroupInfo(cl_kernel                  kernel_,
    cl_device_id               device,
    cl_kernel_work_group_info  param_name,
    size_t                     param_value_size,
    void *                     param_value,
    size_t *                   param_value_size_ret) CL_API_SUFFIX__VERSION_1_0
{
    if (!kernel_)
    {
        return CL_INVALID_KERNEL;
    }
    UNREFERENCED_PARAMETER(device);

    auto RetValue = [&](auto&& param)
    {
        return CopyOutParameter(param, param_value_size, param_value, param_value_size_ret);
    };
    auto& kernel = *static_cast<Kernel*>(kernel_);

    switch (param_name)
    {
    case CL_KERNEL_WORK_GROUP_SIZE: return RetValue((size_t)D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP);
    case CL_KERNEL_COMPILE_WORK_GROUP_SIZE:
    {
        size_t size[3] = {};
        auto ReqDims = kernel.GetRequiredLocalDims();
        if (ReqDims)
            std::copy(ReqDims, ReqDims + 3, size);
        return RetValue(size);
    }
    case CL_KERNEL_LOCAL_MEM_SIZE:
    {
        size_t size = kernel.m_pDxil->metadata.local_mem_size;
        for (cl_uint i = 0; i < kernel.m_pDxil->kernel->num_args; ++i)
        {
            if (kernel.m_pDxil->kernel->args[i].address_qualifier == CLC_KERNEL_ARG_ADDRESS_LOCAL)
            {
                size -= 4;
                size += kernel.m_ArgMetadataToCompiler[i].localptr.size;
            }
        }
        return RetValue(size);
    }
    case CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE: return RetValue((size_t)64);
    case CL_KERNEL_PRIVATE_MEM_SIZE: return RetValue(kernel.m_pDxil->metadata.priv_mem_size);
    }

    return CL_INVALID_VALUE;
}
