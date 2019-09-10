#include "task.hpp"
#include "queue.hpp"
#include "resources.hpp"
#include "formats.hpp"
#include <variant>

using D3D12TranslationLayer::ImmediateContext;
using UpdateSubresourcesScenario = ImmediateContext::UpdateSubresourcesScenario;
using CPrepareUpdateSubresourcesHelper = ImmediateContext::CPrepareUpdateSubresourcesHelper;

class MemWriteFillTask : public Task
{
public:
    struct FillData
    {
        char Pattern[16];
        cl_uint PatternSize;
    };
    struct WriteData
    {
        const void *pData;
        cl_uint RowPitch;
        cl_uint SlicePitch;
    };
    struct Args
    {
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_ushort FirstArraySlice;
        cl_ushort NumArraySlices;
        std::variant<WriteData, FillData> Data;
        cl_uint SrcX;
        cl_uint SrcY;
        cl_uint SrcZ;
        cl_uint DstBufferRowPitch;
        cl_uint DstBufferSlicePitch;
    };


    MemWriteFillTask(Context& Parent, Resource& Target, cl_command_type CommandType,
        cl_command_queue CommandQueue, Args const& args, bool DeferCopy);

private:
    Resource::ref_ptr_int m_Target;
    const Args m_Args;

    void CopyFromHostPtr(UpdateSubresourcesScenario);
    std::vector<CPrepareUpdateSubresourcesHelper> m_Helpers;

    void RecordImpl() final;
    void OnComplete() final
    {
        m_Target.Release();
    }
};

MemWriteFillTask::MemWriteFillTask(Context &Parent, Resource &Target,
    cl_command_type CommandType, cl_command_queue CommandQueue,
    Args const& args, bool DeferCopy)
    : Task(Parent, CommandType, CommandQueue)
    , m_Target(&Target)
    , m_Args(args)
{
    if (!DeferCopy)
    {
        CopyFromHostPtr(UpdateSubresourcesScenario::BatchedContext);
    }
}

void MemWriteFillTask::CopyFromHostPtr(UpdateSubresourcesScenario scenario)
{
    // For buffer rects, have to use row-by-row copies if the pitches don't align to
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
    // TODO: Add a path that uses CopyTextureRegion if it does align.
    
    const bool bIsRowByRowCopy = m_Target->m_Desc.image_type == CL_MEM_OBJECT_BUFFER;
    UINT NumRowCopies = bIsRowByRowCopy ? m_Args.Height : 1;
    UINT NumSliceCopies = bIsRowByRowCopy ? m_Args.Depth : 1;

    D3D12TranslationLayer::CSubresourceSubset subresources =
        m_Target->GetUnderlyingResource()->GetFullSubresourceSubset();
    for (UINT16 i = 0; i < m_Args.NumArraySlices; ++i)
    {
        subresources.m_BeginArray = (UINT16)(m_Args.FirstArraySlice + i);
        subresources.m_EndArray = (UINT16)(m_Args.FirstArraySlice + 1);

        for (UINT z = 0; z < NumSliceCopies; ++z)
        {
            for (UINT y = 0; y < NumRowCopies; ++y)
            {
                D3D11_SUBRESOURCE_DATA UploadData;
                auto pData = &UploadData;
                const void *pPattern = nullptr;
                UINT PatternSize = 0;

                if (m_Args.Data.index() == 0)
                {
                    WriteData const &WriteArgs = std::get<0>(m_Args.Data);

                    const char* pSubresourceData = reinterpret_cast<const char*>(WriteArgs.pData);
                    pSubresourceData += (i + z + m_Args.SrcZ) * WriteArgs.SlicePitch;
                    pSubresourceData += (y + m_Args.SrcY) * WriteArgs.RowPitch;
                    pSubresourceData += m_Args.SrcX;

                    UploadData.pSysMem = pSubresourceData;
                    UploadData.SysMemPitch = WriteArgs.RowPitch;
                    UploadData.SysMemSlicePitch = WriteArgs.SlicePitch;
                }
                else
                {
                    FillData const& FillArgs = std::get<1>(m_Args.Data);
                    pData = nullptr;
                    pPattern = FillArgs.Pattern;
                    PatternSize = FillArgs.PatternSize;
                }

                D3D12_BOX DstBox =
                {
                    m_Args.DstX, m_Args.DstY, m_Args.DstZ,
                    m_Args.DstX + m_Args.Width,
                    m_Args.DstY + m_Args.Height,
                    m_Args.DstZ + m_Args.Depth
                };
                if (bIsRowByRowCopy)
                {
                    DstBox = {};
                    DstBox.left = (UINT)(m_Target->m_Offset + // Buffer suballocation offset
                        ((z + m_Args.DstZ) * m_Args.DstBufferSlicePitch) + // Slice offset
                        ((y + m_Args.DstY) * m_Args.DstBufferRowPitch) + // Row offset
                        m_Args.DstX); // Offset within row
                    DstBox.right = DstBox.left + m_Args.Width;
                }
                m_Helpers.emplace_back(
                    *m_Target->GetUnderlyingResource(),
                    subresources,
                    pData,
                    &DstBox,
                    scenario,
                    pPattern,
                    PatternSize,
                    m_Parent->GetDevice().ImmCtx());
            }
        }

    }
}

void MemWriteFillTask::RecordImpl()
{
    if (m_Helpers.empty())
    {
        CopyFromHostPtr(UpdateSubresourcesScenario::ImmediateContext);
    }

    for (auto& Helper : m_Helpers)
    {
        if (Helper.FinalizeNeeded)
        {
            m_Parent->GetDevice().ImmCtx().FinalizeUpdateSubresources(
                &Helper.Dst, Helper.PreparedStorage.Base, Helper.PreparedStorage.LocalPlacementDescs);
        }
    }
}

cl_int clEnqueueWriteBufferRectImpl(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_write,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    const void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event,
    cl_command_type command_type
)
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!buffer)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(buffer);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("buffer must be a buffer object.", CL_INVALID_MEM_OBJECT);
    }

    if (buffer_offset[0] > resource.m_Desc.image_width ||
        region[0] > resource.m_Desc.image_width ||
        buffer_offset[0] + region[0] > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    if (buffer_row_pitch == 0)
    {
        buffer_row_pitch = region[0];
    }
    else if (buffer_row_pitch > resource.m_Desc.image_width ||
             buffer_row_pitch < region[0])
    {
        return ReportError("buffer_row_pitch must be 0 or between region[0] and the buffer size.", CL_INVALID_VALUE);
    }
    
    if (host_row_pitch == 0)
    {
        host_row_pitch = region[0];
    }
    else if (host_row_pitch > resource.m_Desc.image_width ||
             host_row_pitch < region[0])
    {
        return ReportError("host_row_pitch must be 0 or between region[0] and the buffer size.", CL_INVALID_VALUE);
    }

    size_t SliceSizeInBytes = (buffer_offset[1] + region[1] - 1) * buffer_row_pitch + buffer_offset[0] + region[0];
    if (SliceSizeInBytes > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    size_t ReqBufferSlicePitch = buffer_row_pitch * region[1];
    size_t ReqHostSlicePitch = host_row_pitch * region[1];
    if (buffer_slice_pitch == 0)
    {
        buffer_slice_pitch = ReqBufferSlicePitch;
    }
    else if (buffer_slice_pitch > resource.m_Desc.image_width ||
             buffer_slice_pitch < ReqBufferSlicePitch)
    {
        return ReportError("buffer_slice_pitch must be 0 or between (region[0] * buffer_row_pitch) and the buffer size.", CL_INVALID_VALUE);
    }

    if (host_slice_pitch == 0)
    {
        host_slice_pitch = ReqHostSlicePitch;
    }
    else if (host_slice_pitch > resource.m_Desc.image_width ||
             host_slice_pitch < ReqHostSlicePitch)
    {
        return ReportError("host_slice_pitch must be 0 or between (region[0] * buffer_row_pitch) and the buffer size.", CL_INVALID_VALUE);
    }

    size_t ResourceSizeInBytes = (buffer_offset[2] + region[2] - 1) * buffer_slice_pitch + SliceSizeInBytes;
    if (ResourceSizeInBytes > resource.m_Desc.image_width)
    {
        return ReportError("Offsets/regions too large.", CL_INVALID_VALUE);
    }

    if (resource.m_Flags & (CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Buffer is not writable from the host.", CL_INVALID_OPERATION);
    }

    if (!ptr)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    MemWriteFillTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)buffer_offset[0];
    CmdArgs.DstY = (cl_uint)buffer_offset[1];
    CmdArgs.DstZ = (cl_uint)buffer_offset[2];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = (cl_uint)region[1];
    CmdArgs.Depth = (cl_uint)region[2];
    CmdArgs.SrcX = (cl_uint)host_offset[0];
    CmdArgs.SrcY = (cl_uint)host_offset[1];
    CmdArgs.SrcZ = (cl_uint)host_offset[2];
    CmdArgs.NumArraySlices = 1;
    CmdArgs.DstBufferRowPitch = (cl_uint)buffer_row_pitch;
    CmdArgs.DstBufferSlicePitch = (cl_uint)buffer_slice_pitch;
    CmdArgs.Data = MemWriteFillTask::WriteData
    {
        ptr, (cl_uint)host_row_pitch, (cl_uint)host_slice_pitch
    };

    try
    {
        std::unique_ptr<Task> task(new MemWriteFillTask(context, resource, command_type, command_queue, CmdArgs, blocking_write == CL_FALSE));
        auto Lock = context.GetDevice().GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteBuffer(cl_command_queue   command_queue,
    cl_mem             buffer,
    cl_bool            blocking_write,
    size_t             offset,
    size_t             size,
    const void* ptr,
    cl_uint            num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    size_t buffer_offset[3] = { offset, 0, 0 };
    size_t host_offset[3] = { 0, 0, 0 };
    size_t region[3] = { size, 1, 1 };
    return clEnqueueWriteBufferRectImpl(
        command_queue,
        buffer,
        blocking_write,
        buffer_offset,
        host_offset,
        region,
        0,
        0,
        0,
        0,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
        CL_COMMAND_COPY_BUFFER
    );
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteBufferRect(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_write,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    const void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1
{
    return clEnqueueWriteBufferRectImpl(
        command_queue,
        buffer,
        blocking_write,
        buffer_offset,
        host_offset,
        region,
        buffer_row_pitch,
        buffer_slice_pitch,
        host_row_pitch,
        host_slice_pitch,
        ptr,
        num_events_in_wait_list,
        event_wait_list,
        event,
        CL_COMMAND_COPY_BUFFER_RECT
    );
}


extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueFillBuffer(cl_command_queue   command_queue,
    cl_mem             buffer,
    const void* pattern,
    size_t             pattern_size,
    size_t             offset,
    size_t             size,
    cl_uint            num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!buffer)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(buffer);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (resource.m_Desc.image_type != CL_MEM_OBJECT_BUFFER)
    {
        return ReportError("buffer must be a buffer object.", CL_INVALID_MEM_OBJECT);
    }

    if (offset > resource.m_Desc.image_width ||
        size > resource.m_Desc.image_width ||
        offset + size > resource.m_Desc.image_width)
    {
        return ReportError("offset/size too large.", CL_INVALID_VALUE);
    }

    switch (pattern_size)
    {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
        break;
    default:
        return ReportError("Invalid pattern_size. Valid values are {1, 2, 4, 8, 16} for this device.", CL_INVALID_VALUE);
    }

    if (!pattern)
    {
        return ReportError("pattern must not be null.", CL_INVALID_VALUE);
    }

    if (size % pattern_size != 0 || offset % pattern_size != 0)
    {
        return ReportError("offset and size must be a multiple of pattern_size.", CL_INVALID_VALUE);
    }

    if (resource.m_Flags & (CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Buffer is not writable from the host.", CL_INVALID_OPERATION);
    }

    MemWriteFillTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)offset;
    CmdArgs.Width = (cl_uint)size;
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;

    MemWriteFillTask::FillData FillData;
    memcpy(FillData.Pattern, pattern, pattern_size);
    CmdArgs.Data = FillData;

    try
    {
        std::unique_ptr<Task> task(new MemWriteFillTask(context, resource, CL_COMMAND_FILL_BUFFER, command_queue, CmdArgs, false));
        auto Lock = context.GetDevice().GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueWriteImage(cl_command_queue    command_queue,
    cl_mem              image,
    cl_bool             blocking_write,
    const size_t* origin,
    const size_t* region,
    size_t              input_row_pitch,
    size_t              input_slice_pitch,
    const void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!image)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(image);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (origin[0] > resource.m_Desc.image_width ||
        region[0] > resource.m_Desc.image_width ||
        origin[0] + region[0] > resource.m_Desc.image_width)
    {
        return ReportError("origin/region is too large.", CL_INVALID_VALUE);
    }

    size_t ReqRowPitch = CD3D11FormatHelper::GetByteAlignment(GetDXGIFormatForCLImageFormat(resource.m_Format)) * resource.m_Desc.image_width;
    if (input_row_pitch == 0)
    {
        input_row_pitch = ReqRowPitch;
    }
    else if (input_row_pitch < ReqRowPitch)
    {
        return ReportError("input_row_pitch must be 0 or at least large enough for a single row.", CL_INVALID_VALUE);
    }

    size_t ReqSlicePitch = input_row_pitch * max<size_t>(resource.m_Desc.image_height, 1);
    if (input_slice_pitch == 0)
    {
        input_slice_pitch = ReqSlicePitch;
    }
    else if (input_slice_pitch < ReqSlicePitch)
    {
        return ReportError("input_slice_pitch must be 0 or at least input_row_pitch * image_height.", CL_INVALID_VALUE);
    }

    if (resource.m_Flags & (CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Image is not writable from the host.", CL_INVALID_OPERATION);
    }

    if (!ptr)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    MemWriteFillTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;
    CmdArgs.Data = MemWriteFillTask::WriteData
    {
        ptr, (cl_uint)input_row_pitch, (cl_uint)input_slice_pitch
    };

    switch (resource.m_Desc.image_type)
    {
    default:
    case CL_MEM_OBJECT_BUFFER:
        return ReportError("image must be an image object.", CL_INVALID_MEM_OBJECT);

    case CL_MEM_OBJECT_IMAGE1D:
    case CL_MEM_OBJECT_IMAGE1D_BUFFER:
        if (origin[1] != 0 || origin[2] != 0 ||
            region[1] != 0 || region[2] != 0)
        {
            return ReportError("For 1D images, origin/region dimensions beyond the first must be 0.", CL_INVALID_VALUE);
        }
        break;

    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        if (origin[1] > resource.m_Desc.image_array_size ||
            region[1] > resource.m_Desc.image_array_size ||
            origin[1] + region[1] > resource.m_Desc.image_array_size)
        {
            return ReportError("For 1D image arrays, origin[1] and region[1] must be less than the image_array_size.", CL_INVALID_VALUE);
        }
        CmdArgs.FirstArraySlice = (cl_ushort)origin[1];
        CmdArgs.NumArraySlices = (cl_ushort)region[1];

        if (origin[2] != 0 || region[2] != 0)
        {
            return ReportError("For 1D image arrays, origin[2] and region[2] must be 0.", CL_INVALID_VALUE);
        }
        break;

    case CL_MEM_OBJECT_IMAGE2D:
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
    case CL_MEM_OBJECT_IMAGE3D:
        if (origin[1] > resource.m_Desc.image_height ||
            region[1] > resource.m_Desc.image_height ||
            origin[1] + region[1] > resource.m_Desc.image_height)
        {
            return ReportError("For 2D and 3D images, origin[1] and region[1] must be less than the image_height.", CL_INVALID_VALUE);
        }
        CmdArgs.DstY = (cl_uint)origin[1];
        CmdArgs.Height = (cl_uint)region[1];

        switch (resource.m_Desc.image_type)
        {
        case CL_MEM_OBJECT_IMAGE2D:
            if (origin[2] != 0 || region[2] != 0)
            {
                return ReportError("For 2D images, origin[2] and region[2] must be 0.", CL_INVALID_VALUE);
            }
            break;
        case CL_MEM_OBJECT_IMAGE2D_ARRAY:
            if (origin[2] > resource.m_Desc.image_array_size ||
                region[2] > resource.m_Desc.image_array_size ||
                origin[2] + region[2] > resource.m_Desc.image_array_size)
            {
                return ReportError("For 2D image arrays, origin[2] and region[2] must be less than the image_array_size.", CL_INVALID_VALUE);
            }
            CmdArgs.FirstArraySlice = (cl_ushort)origin[2];
            CmdArgs.NumArraySlices = (cl_ushort)region[2];
            break;
        case CL_MEM_OBJECT_IMAGE3D:
            if (origin[2] > resource.m_Desc.image_depth ||
                region[2] > resource.m_Desc.image_depth ||
                origin[2] + region[2] > resource.m_Desc.image_depth)
            {
                return ReportError("For 3D images, origin[2] and region[2] must be less than the image_depth.", CL_INVALID_VALUE);
            }
            CmdArgs.DstZ = (cl_uint)origin[2];
            CmdArgs.Depth = (cl_uint)region[2];
            break;
        }
        break;
    }

    try
    {
        std::unique_ptr<Task> task(new MemWriteFillTask(context, resource, CL_COMMAND_WRITE_IMAGE, command_queue, CmdArgs, blocking_write == CL_FALSE));
        auto Lock = context.GetDevice().GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}


class FillImageTask : public Task
{
public:
    struct Args
    {
        cl_uint DstX;
        cl_uint DstY;
        cl_uint DstZ;
        cl_uint Width;
        cl_uint Height;
        cl_uint Depth;
        cl_ushort FirstArraySlice;
        cl_ushort NumArraySlices;
        char Pattern[16];
    };

    FillImageTask(Context& Parent, Resource& Target, cl_command_queue CommandQueue, Args const& args);

private:
    Resource::ref_ptr_int m_Target;
    const Args m_Args;

    void RecordImpl() final;
    void OnComplete() final
    {
        m_Target.Release();
    }
};

FillImageTask::FillImageTask(Context &Parent, Resource &Target, cl_command_queue CommandQueue, Args const& args)
    : Task(Parent, CL_COMMAND_FILL_IMAGE, CommandQueue)
    , m_Target(&Target)
    , m_Args(args)
{
}

void FillImageTask::RecordImpl()
{
    auto& ImmCtx = m_Parent->GetDevice().ImmCtx();
    bool UseLocalUAV = true;
    if (m_Args.FirstArraySlice == 0 &&
        m_Args.NumArraySlices == m_Target->GetUnderlyingResource()->Parent()->ArraySize())
    {
        UseLocalUAV = false;
    }
    if (m_Args.DstZ != 0 && m_Args.Depth != m_Target->GetUnderlyingResource()->AppDesc()->Depth())
    {
        UseLocalUAV = false;
    }
    
    std::optional<D3D12TranslationLayer::UAV> LocalUAV;
    if (UseLocalUAV)
    {
        D3D12TranslationLayer::D3D12_UNORDERED_ACCESS_VIEW_DESC_WRAPPER UAVDescWrapper = {};
        auto &UAVDesc = UAVDescWrapper.m_Desc12;
        UAVDesc = m_Target->GetUAV().GetDesc12();
        switch (UAVDesc.ViewDimension)
        {
        case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
            UAVDesc.Texture1DArray.FirstArraySlice = m_Args.FirstArraySlice;
            UAVDesc.Texture1DArray.ArraySize = m_Args.NumArraySlices;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
            UAVDesc.Texture2DArray.FirstArraySlice = m_Args.FirstArraySlice;
            UAVDesc.Texture2DArray.ArraySize = m_Args.NumArraySlices;
            break;
        case D3D12_UAV_DIMENSION_TEXTURE3D:
            UAVDesc.Texture3D.FirstWSlice = m_Args.DstZ;
            UAVDesc.Texture3D.WSize = m_Args.Depth;
            break;
        }
        LocalUAV.emplace(&ImmCtx, UAVDescWrapper, *m_Target->GetUnderlyingResource());
    }
    auto pUAV = UseLocalUAV ? &LocalUAV.value() : &m_Target->GetUAV();
    D3D12_RECT Rect =
    {
        (LONG)m_Args.DstX,
        (LONG)m_Args.DstY,
        (LONG)(m_Args.DstX + m_Args.Width),
        (LONG)(m_Args.DstY + m_Args.Height)
    };
    switch (m_Target->m_Format.image_channel_data_type)
    {
    case CL_SNORM_INT8:
    case CL_SNORM_INT16:
    case CL_UNORM_INT8:
    case CL_UNORM_INT16:
    case CL_UNORM_INT24:
    case CL_FLOAT:
    case CL_HALF_FLOAT:
        ImmCtx.ClearUnorderedAccessViewFloat(pUAV, reinterpret_cast<const float*>(m_Args.Pattern), 1, &Rect);
        break;
    case CL_UNSIGNED_INT8:
    case CL_UNSIGNED_INT16:
    case CL_UNSIGNED_INT32:
        ImmCtx.ClearUnorderedAccessViewUint(pUAV, reinterpret_cast<const UINT*>(m_Args.Pattern), 1, &Rect);
        break;

    default:
        assert(false);
        break;
    }
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueFillImage(cl_command_queue   command_queue,
    cl_mem             image,
    const void* fill_color,
    const size_t* origin,
    const size_t* region,
    cl_uint            num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_2
{
    if (!command_queue)
    {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if (!image)
    {
        return CL_INVALID_MEM_OBJECT;
    }
    CommandQueue& queue = *static_cast<CommandQueue*>(command_queue);
    Resource& resource = *static_cast<Resource*>(image);
    Context& context = queue.GetContext();
    auto ReportError = context.GetErrorReporter();
    if (&context != &resource.m_Parent.get())
    {
        return ReportError("Context mismatch between command queue and buffer.", CL_INVALID_CONTEXT);
    }

    if (origin[0] > resource.m_Desc.image_width ||
        region[0] > resource.m_Desc.image_width ||
        origin[0] + region[0] > resource.m_Desc.image_width)
    {
        return ReportError("origin/region is too large.", CL_INVALID_VALUE);
    }

    if (resource.m_Flags & (CL_MEM_HOST_READ_ONLY | CL_MEM_HOST_NO_ACCESS))
    {
        return ReportError("Image is not writable from the host.", CL_INVALID_OPERATION);
    }

    if (!fill_color)
    {
        return ReportError("ptr must not be null.", CL_INVALID_VALUE);
    }

    FillImageTask::Args CmdArgs = {};
    CmdArgs.DstX = (cl_uint)origin[0];
    CmdArgs.Width = (cl_uint)region[0];
    CmdArgs.Height = 1;
    CmdArgs.Depth = 1;
    CmdArgs.NumArraySlices = 1;
    // fill_color is either 4 floats, 4 ints, or 4 uints
    memcpy(CmdArgs.Pattern, fill_color, sizeof(CmdArgs.Pattern));

    switch (resource.m_Desc.image_type)
    {
    default:
    case CL_MEM_OBJECT_BUFFER:
        return ReportError("image must be an image object.", CL_INVALID_MEM_OBJECT);

    case CL_MEM_OBJECT_IMAGE1D:
    case CL_MEM_OBJECT_IMAGE1D_BUFFER:
        if (origin[1] != 0 || origin[2] != 0 ||
            region[1] != 0 || region[2] != 0)
        {
            return ReportError("For 1D images, origin/region dimensions beyond the first must be 0.", CL_INVALID_VALUE);
        }
        break;

    case CL_MEM_OBJECT_IMAGE1D_ARRAY:
        if (origin[1] > resource.m_Desc.image_array_size ||
            region[1] > resource.m_Desc.image_array_size ||
            origin[1] + region[1] > resource.m_Desc.image_array_size)
        {
            return ReportError("For 1D image arrays, origin[1] and region[1] must be less than the image_array_size.", CL_INVALID_VALUE);
        }
        CmdArgs.FirstArraySlice = (cl_ushort)origin[1];
        CmdArgs.NumArraySlices = (cl_ushort)region[1];

        if (origin[2] != 0 || region[2] != 0)
        {
            return ReportError("For 1D image arrays, origin[2] and region[2] must be 0.", CL_INVALID_VALUE);
        }
        break;

    case CL_MEM_OBJECT_IMAGE2D:
    case CL_MEM_OBJECT_IMAGE2D_ARRAY:
    case CL_MEM_OBJECT_IMAGE3D:
        if (origin[1] > resource.m_Desc.image_height ||
            region[1] > resource.m_Desc.image_height ||
            origin[1] + region[1] > resource.m_Desc.image_height)
        {
            return ReportError("For 2D and 3D images, origin[1] and region[1] must be less than the image_height.", CL_INVALID_VALUE);
        }
        CmdArgs.DstY = (cl_uint)origin[1];
        CmdArgs.Height = (cl_uint)region[1];

        switch (resource.m_Desc.image_type)
        {
        case CL_MEM_OBJECT_IMAGE2D:
            if (origin[2] != 0 || region[2] != 0)
            {
                return ReportError("For 2D images, origin[2] and region[2] must be 0.", CL_INVALID_VALUE);
            }
            break;
        case CL_MEM_OBJECT_IMAGE2D_ARRAY:
            if (origin[2] > resource.m_Desc.image_array_size ||
                region[2] > resource.m_Desc.image_array_size ||
                origin[2] + region[2] > resource.m_Desc.image_array_size)
            {
                return ReportError("For 2D image arrays, origin[2] and region[2] must be less than the image_array_size.", CL_INVALID_VALUE);
            }
            CmdArgs.FirstArraySlice = (cl_ushort)origin[2];
            CmdArgs.NumArraySlices = (cl_ushort)region[2];
            break;
        case CL_MEM_OBJECT_IMAGE3D:
            if (origin[2] > resource.m_Desc.image_depth ||
                region[2] > resource.m_Desc.image_depth ||
                origin[2] + region[2] > resource.m_Desc.image_depth)
            {
                return ReportError("For 3D images, origin[2] and region[2] must be less than the image_depth.", CL_INVALID_VALUE);
            }
            CmdArgs.DstZ = (cl_uint)origin[2];
            CmdArgs.Depth = (cl_uint)region[2];
            break;
        }
        break;
    }

    try
    {
        std::unique_ptr<Task> task(new FillImageTask(context, resource, command_queue, CmdArgs));
        auto Lock = context.GetDevice().GetTaskPoolLock();
        task->AddDependencies(event_wait_list, num_events_in_wait_list, Lock);
        queue.QueueTask(task.get(), Lock);

        // No more exceptions
        if (event)
            *event = task.release();
        else
            task.release()->Release();
    }
    catch (std::bad_alloc &) { return ReportError(nullptr, CL_OUT_OF_HOST_MEMORY); }
    catch (std::exception &e) { return ReportError(e.what(), CL_OUT_OF_RESOURCES); }
    catch (_com_error&) { return ReportError(nullptr, CL_OUT_OF_RESOURCES); }
    return CL_SUCCESS;
}


#pragma warning(disable: 4100)

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadBuffer(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_read,
    size_t              offset,
    size_t              size,
    void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadBufferRect(cl_command_queue    command_queue,
    cl_mem              buffer,
    cl_bool             blocking_read,
    const size_t* buffer_offset,
    const size_t* host_offset,
    const size_t* region,
    size_t              buffer_row_pitch,
    size_t              buffer_slice_pitch,
    size_t              host_row_pitch,
    size_t              host_slice_pitch,
    void* ptr,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBuffer(cl_command_queue    command_queue,
    cl_mem              src_buffer,
    cl_mem              dst_buffer,
    size_t              src_offset,
    size_t              dst_offset,
    size_t              size,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBufferRect(cl_command_queue    command_queue,
    cl_mem              src_buffer,
    cl_mem              dst_buffer,
    const size_t* src_origin,
    const size_t* dst_origin,
    const size_t* region,
    size_t              src_row_pitch,
    size_t              src_slice_pitch,
    size_t              dst_row_pitch,
    size_t              dst_slice_pitch,
    cl_uint             num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_1
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueReadImage(cl_command_queue     command_queue,
    cl_mem               image,
    cl_bool              blocking_read,
    const size_t* origin,
    const size_t* region,
    size_t               row_pitch,
    size_t               slice_pitch,
    void* ptr,
    cl_uint              num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyImage(cl_command_queue     command_queue,
    cl_mem               src_image,
    cl_mem               dst_image,
    const size_t* src_origin,
    const size_t* dst_origin,
    const size_t* region,
    cl_uint              num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyImageToBuffer(cl_command_queue command_queue,
    cl_mem           src_image,
    cl_mem           dst_buffer,
    const size_t* src_origin,
    const size_t* region,
    size_t           dst_offset,
    cl_uint          num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueCopyBufferToImage(cl_command_queue command_queue,
    cl_mem           src_buffer,
    cl_mem           dst_image,
    size_t           src_offset,
    const size_t* dst_origin,
    const size_t* region,
    cl_uint          num_events_in_wait_list,
    const cl_event* event_wait_list,
    cl_event* event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}

extern CL_API_ENTRY void * CL_API_CALL
clEnqueueMapBuffer(cl_command_queue command_queue,
    cl_mem           buffer,
    cl_bool          blocking_map,
    cl_map_flags     map_flags,
    size_t           offset,
    size_t           size,
    cl_uint          num_events_in_wait_list,
    const cl_event * event_wait_list,
    cl_event *       event,
    cl_int *         errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    *errcode_ret = CL_INVALID_PLATFORM;
    return nullptr;
}

extern CL_API_ENTRY void * CL_API_CALL
clEnqueueMapImage(cl_command_queue  command_queue,
    cl_mem            image,
    cl_bool           blocking_map,
    cl_map_flags      map_flags,
    const size_t *    origin,
    const size_t *    region,
    size_t *          image_row_pitch,
    size_t *          image_slice_pitch,
    cl_uint           num_events_in_wait_list,
    const cl_event *  event_wait_list,
    cl_event *        event,
    cl_int *          errcode_ret) CL_API_SUFFIX__VERSION_1_0
{
    *errcode_ret = CL_INVALID_PLATFORM;
    return nullptr;
}

extern CL_API_ENTRY cl_int CL_API_CALL
clEnqueueUnmapMemObject(cl_command_queue command_queue,
    cl_mem           memobj,
    void *           mapped_ptr,
    cl_uint          num_events_in_wait_list,
    const cl_event * event_wait_list,
    cl_event *       event) CL_API_SUFFIX__VERSION_1_0
{
    return CL_INVALID_PLATFORM;
}
