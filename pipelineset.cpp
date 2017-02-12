#include "pipelineset.h"

#include <d3d12.h>

// Used to concatenate paths
#include <Pathcch.h>

// for concurrency::parallel_for
#include <ppl.h>

// for _com_error
#include <comdef.h>

#include <deque>
#include <unordered_map>
#include <map>
#include <set>
#include <array>
#include <cassert>

#pragma comment(lib, "Pathcch.lib")

// boilerplate for error handling. Feel free to swap with your own if you don't like MB_ABORTRETRYIGNORE.
// These errors shouldn't happen during normal use anyways (in theory). Compile errors and stuff is reported through plain printf.
namespace
{
    // From https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
    //  
    // Usage: SetThreadName ((DWORD)-1, "MainThread");  
    //  
    const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push,8)  
    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.  
        LPCSTR szName; // Pointer to name (in user addr space).  
        DWORD dwThreadID; // Thread ID (-1=caller thread).  
        DWORD dwFlags; // Reserved for future use, must be zero.  
    } THREADNAME_INFO;
#pragma pack(pop)  
    void SetThreadName(DWORD dwThreadID, const char* threadName) {
        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = threadName;
        info.dwThreadID = dwThreadID;
        info.dwFlags = 0;
#pragma warning(push)  
#pragma warning(disable: 6320 6322)  
        __try {
            RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#pragma warning(pop)  
    }

    void SetThreadName(std::thread& th, const char* threadName)
    {
        SetThreadName(GetThreadId(static_cast<HANDLE>(th.native_handle())), threadName);
    }

    std::wstring WideFromMultiByte(const char* s)
    {
        int bufSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
        assert(bufSize != 0);

        std::wstring ws(bufSize, 0);
        bufSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, &ws[0], bufSize);
        assert(bufSize != 0);

        ws.pop_back(); // remove null terminator

        return ws;
    }

    bool detail_WinAssert(bool okay, const WCHAR* error, const char* expr, const char* file, const char* function, int line)
    {
        if (okay)
        {
            return true;
        }

        std::wstring werror = error ? error : L"Assertion failed";
        std::wstring wexpr = WideFromMultiByte(expr);
        std::wstring wfile = WideFromMultiByte(file);
        std::wstring wfunction = WideFromMultiByte(function);

        std::wstring msg = std::wstring() +
            L"Expr: " + wexpr + L"\n" +
            L"File: " + wfile + L"\n" +
            L"Function: " + wfunction + L"\n" +
            L"Line: " + std::to_wstring(line) + L"\n" +
            L"ErrorMessage: " + werror + L"\n";

        int result = MessageBoxW(NULL, msg.c_str(), L"Error", MB_ABORTRETRYIGNORE);
        if (result == IDABORT)
        {
            ExitProcess(-1);
        }
        else if (result == IDRETRY)
        {
            DebugBreak();
        }
        else if (result == IDIGNORE)
        {
            // do nothing
        }

        return false;
    }

    bool detail_CheckHR(HRESULT hr, const char* expr, const char* file, const char* function, int line)
    {
        if (SUCCEEDED(hr))
        {
            return true;
        }

        _com_error err(hr);

        detail_WinAssert(false, err.ErrorMessage(), expr, file, function, line);

        return false;
    }

    bool detail_CheckWin32(bool okay, const char* expr, const char* file, const char* function, int line)
    {
        if (okay)
        {
            return true;
        }

        return detail_CheckHR(HRESULT_FROM_WIN32(GetLastError()), expr, file, function, line);
    }
}

// If the HRESULT is an error, reports it.
#define CHECKHR(hr_expr) detail_CheckHR((hr_expr), #hr_expr, __FILE__, __FUNCSIG__, __LINE__)
// If the expression is false, reports the GetLastError().
#define CHECKWIN32(bool_expr) detail_CheckWin32((bool_expr), #bool_expr, __FILE__, __FUNCSIG__, __LINE__)

class PipelineSet : public IPipelineSet
{
    ID3D12Device* mpDevice;

    // I can't delete PSOs right away because they might still be used in current frames.
    // To counter this, I delete PSOs only after a number of updates corresponding to the maximum latency have passed.
    int mMaximumFrameLatency;

    // You're not allowed to add more pipelines after calling BuildAllAsync(),
    // and this flag is used to make sure of that.
    bool mClosedForAddition = false;

    // The user-supplied root signatures that aren't owned by the PipelineSet
    // The map is used to consolidate the ptrptrs to point to the same pointer.
    std::set<ID3D12RootSignature*> mUserRSes;

    struct ReloadablePipelineState
    {
        enum PipelineType
        {
            PT_Graphics,
            PT_Compute
        };

        PipelineType Type;

        // The public and internal pointers, similar to those in ReloadableRootSignature.
        ID3D12PipelineState* pPublicPtr;
        ID3D12PipelineState* pInternalPtr;

        // The prototype this PSO is based on.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC GfxPipelineStateDesc;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ComputePipelineStateDesc;

        // The files used to build this pipeline
        GraphicsPipelineFiles GfxFiles;
        ComputePipelineFiles ComputeFiles;

        // Storage for input layout (which is normally passed by reference)
        // If this array is not big enough and the assert doesn't catch it (at runtime in a release build), then a dynamic allocation is used.
        D3D12_INPUT_ELEMENT_DESC InputElementDescStorage[16];

        ReloadablePipelineState() = default;

        static ReloadablePipelineState Graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const GraphicsPipelineFiles& files)
        {
            ReloadablePipelineState ps{};
            ps.Type = PT_Graphics;
            ps.GfxPipelineStateDesc = desc;
            ps.GfxFiles = files;
            return ps;
        }

        static ReloadablePipelineState Compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, const ComputePipelineFiles& files)
        {
            ReloadablePipelineState ps{};
            ps.Type = PT_Compute;
            ps.ComputePipelineStateDesc = desc;
            ps.ComputeFiles = files;
            return ps;
        }
    };

    // All the pipelines that have been added to the PipelineSet.
    // Note the use of deque means pointers to objects in this container are never invalidated when push_back() is called.
    std::deque<ReloadablePipelineState> mPipelines;

    struct ReloadableRootSignature
    {
        // pPublicPtr is the pointer that public users of the interface see.
        // pPublicPtr is set to NULL when the compilation failed.
        // Otherwise, it has two possible values:
        // 1) It can be the same as the pInternalPtr, if the last compilation succeeded.
        // 2) It can be DIFFERENT from the pInternalPtr, which happens due to async reloads.
        //    That is, if a file's timestamp changes, the IPipelineSet will respond to that by reloading the RS.
        //    This reloaded RS will be stored in pInternalPtr, but not pPublicPtr.
        //    The reason why is that the pPublicPtr shouldn't change mid-frame from the user's perspective.
        //    Instead, the user needs to explicitly call UpdatePipelines() at the start of each frame.
        //    This update will set all the pPublicPtrs to the most recent pInternalPtr,
        //    which gives the user a consistent place to expect signature/pipeline pointers to change.
        //    Note this whole thing only matters for live-reloading, after the initial BuildAllAsync is done.
        //    When the BuildAllAsync is finishes, it'll set all the public ptrs right away.
        //    This shouldn't cause signatures/pipelines to change mid-frame,
        //    since they shouldn't be being used at all before the BuildAllAsync's event is set.
        ID3D12RootSignature* pPublicPtr;
        ID3D12RootSignature* pInternalPtr;

        // The pipelines that use this root signature
        std::vector<ReloadablePipelineState*> DependentPipelines;
    };

    // All the root signatures that have been created and owned by the PipelineSet
    // The mapping from filenames is used to identify and consolidate shared root signatures.
    // Note that std::map is used because map nodes are stable in memory. This is important to keep a RS ptrptr.
    // The RSes in this container are owned by the PipelineSet.
    std::map<std::wstring, ReloadableRootSignature> mFileToRS;

    struct ReloadableShader
    {
        // The pipelines that use this shader
        std::vector<ReloadablePipelineState*> DependentPipelines;
    };

    // All the shader files that are being watched by the PipelineSet.
    std::map<std::wstring, ReloadableShader> mFileToShader;

    // The last time each file was written.
    // This is useful because the directory watcher redundantly reports writes many times sometimes,
    // so this is used to filter the results.
    // Timestamps are initially 0 until the file gets actually opened for the first time.
    std::map<std::wstring, UINT64> mFileToLastWriteTimestamp;

    // When RS and PSOs are reloaded there has to be some fiddling between their public and internal pointers.
    // This lock grants exclusive access to ALL the public and internal ptrs in this class.
    std::mutex mReloadablePointerLock;

    // Windows doesn't let you watch files, but you can watch directories.
    // So I just have to collect all the directories I want to watch as the shader files are being added...
    // The HANDLE here is a handle to the directory.
    // This also lets me get the proper access rights so nobody can delete a watched directory out from under my feet.
    std::map<std::wstring, HANDLE> mDirectoriesToWatch;

    // Represents an IO task calling ReadDirectoryChanges in asynchronous mode.
    // Used by the DirectoryWatcherThread.
    struct RDCTask
    {
        // The directory for which this task is looking at changes.
        std::wstring DirectoryPath;
        HANDLE DirectoryHandle;

        // ReadDirectoryChanges docs say "The hEvent member of the OVERLAPPED structure is not used by the system, so you can use it yourself."
        // So the hEvent of this OVERLAPPED is used as a "userdata" pointer to this RDCTask.
        OVERLAPPED Overlapped;

        // Total number of active RDC tasks
        // When this is 0, that means there are no more RDC Tasks (ie. they've all been cancelled successfully).
        // Tasks should decrement this number when they shut themselves down.
        int* pOutstandingRDCTaskCount;

        // Buffer used to store the directory changes.
        // The choice of 64k is because that's the maximum allowed when you're monitoring a directory over a network (see ReadDirectoryChangesW documentation).
        // If the result needs more memory than this, the ReadDirectoryChangesW will fail, and the code has to fallback to manually traversing the directory.
        // It could be made bigger if need be, but be careful because this is non-paged kernel memory, so making this too big might be costly.
        // For now I assume that missing some changes is not the end of the world, and 64k is enough for most realistic purposes.
        static const size_t kDirectoryChangeBufferSize = 64 * 1024;
        std::unique_ptr<char[]> pDirectoryChangeBuffer;

        // Pointer back to the PipelineSet that spawned this task,
        // so we can call HandleFileChangeNotifications
        PipelineSet* pPipelineSet;
    };

    // Data for the directory watcher
    // Put into a class so its context can be passed through an APC...
    struct DirectoryWatcher
    {
        // Thread that asynchronously watches for any changes to shader files and reloads the RSs and PSOs.
        std::thread DirectoryWatcherThread;

        // The IO tasks currently asynchronously watching for directory changes.
        std::vector<std::unique_ptr<RDCTask>> RDCTasks;

        // Set to true when the terminate APC is received.
        bool ShouldTerminate = false;

        // Set to true after the watcher sent CancelIo()s to its IO tasks. It then waits for the cancels to finish before exiting the thread.
        bool StartedCancelling = false;
    };

    DirectoryWatcher mDirectoryWatcher;

    // When an object gets live-reloaded, it gets put in place of the public ptr at the next update.
    // However, the old public ptr can't be Release()'d right away, since it might still be in use.
    // For this reason, the deletion of the old public pointer is delayed by a certain number of frames, to guarantee is won't be in use anymore.
    // This delay consists of #mMaximumFrameLatency calls to UpdatePipelines()
    // The number associated to the objects here contain the countdown number, which is decremented with every update.
    // Note these aren't used for the initial creation of resources, as an optimization for the most common scenario of no reloading (see mDidInitialCommit).
    // When the commit is complete, the associated pair gets removed from the map.
    // These maps should only be accessed when mReloadablePointerLock is held.
    std::set<ReloadableRootSignature*> mRootSignaturesToCommit;
    std::set<ReloadablePipelineState*> mPipelinesToCommit;
    std::map<ID3D12RootSignature*, int> mRootSignatureGarbageCollectQueue;
    std::map<ID3D12PipelineState*, int> mPipelineGarbageCollectQueue;

    // Event used to signal that the initial build of the pipelines has finished.
    HANDLE mhBuildAsyncEvent = NULL;

    // The initial commit to public pointers inside UpdatePipelines() needs to write all pointers
    // Later, if only one shader is changed, then the cost can be amortized by only updating the pointers that actually changed.
    // This saves you from having to loop through all pointers ever at every update.
    bool mDidInitialCommit = false;

public:
    explicit PipelineSet(ID3D12Device* pDevice, int maximumFrameLatency)
        : mpDevice(pDevice)
        , mMaximumFrameLatency(maximumFrameLatency)
    { }

    ~PipelineSet()
    {
        // Wait for any Async build to finish
        if (mhBuildAsyncEvent != NULL)
        {
            CHECKWIN32(WaitForSingleObjectEx(mhBuildAsyncEvent, INFINITE, FALSE) == WAIT_OBJECT_0);
            CHECKWIN32(CloseHandle(mhBuildAsyncEvent) != FALSE);
        }

        // Stop looking for any change notifications (only if they've started already)
        if (mDirectoryWatcher.DirectoryWatcherThread.joinable())
        {
            // Send an APC that asks for the directory watcher to shut down
            CHECKWIN32(QueueUserAPC(DirectoryWatcherTerminateAPC, static_cast<HANDLE>(mDirectoryWatcher.DirectoryWatcherThread.native_handle()), reinterpret_cast<ULONG_PTR>(&mDirectoryWatcher)) != 0);
            // wait for the directory to receive and process the shutdown message
            mDirectoryWatcher.DirectoryWatcherThread.join();
        }

        // Close all opened directories
        for (std::pair<const std::wstring, HANDLE>& name2hdir : mDirectoriesToWatch)
        {
            CHECKWIN32(CloseHandle(name2hdir.second) != FALSE);
        }

        // Release all the ID3D12PipelineStates created and owned by the IPipelineSet.
        for (ReloadablePipelineState& pipeline : mPipelines)
        {
            if (pipeline.Type == ReloadablePipelineState::PT_Graphics)
            {
                // delete emergency input element storage if it existed
                if (pipeline.GfxPipelineStateDesc.InputLayout.pInputElementDescs != NULL &&
                    pipeline.GfxPipelineStateDesc.InputLayout.pInputElementDescs != pipeline.InputElementDescStorage)
                {
                    delete[] pipeline.GfxPipelineStateDesc.InputLayout.pInputElementDescs;
                }
            }

            if (pipeline.pPublicPtr != NULL && pipeline.pPublicPtr != pipeline.pInternalPtr)
            {
                // If the pPublicPtr has an uncommitted pInternalPtr, then both need to be Release()'d.
                pipeline.pPublicPtr->Release();
            }

            if (pipeline.pInternalPtr != NULL)
            {
                pipeline.pInternalPtr->Release();
            }
        }

        // Release all ID3D12RootSignatures created and owned by the IPipelineSet
        for (std::pair<const std::wstring, ReloadableRootSignature>& f2rs : mFileToRS)
        {
            if (f2rs.second.pPublicPtr != NULL && f2rs.second.pPublicPtr != f2rs.second.pInternalPtr)
            {
                // If the pPublicPtr has an uncommitted pInternalPtr, then both need to be Release()'d.
                f2rs.second.pPublicPtr->Release();
            }

            if (f2rs.second.pInternalPtr != NULL)
            {
                f2rs.second.pInternalPtr->Release();
            }
        }

        // Release everything in the RS GC queue
        for (std::pair<ID3D12RootSignature* const, int>& rsgc : mRootSignatureGarbageCollectQueue)
        {
            rsgc.first->Release();
        }

        // Release everything in the PSO GC queue
        for (std::pair<ID3D12PipelineState* const, int>& psogc : mPipelineGarbageCollectQueue)
        {
            psogc.first->Release();
        }
    }

    std::pair<ID3D12RootSignature**, ID3D12PipelineState**> AddPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const GraphicsPipelineFiles& files) override
    {
        return AddPipeline_Common(ReloadablePipelineState::Graphics(desc, files), desc.pRootSignature, files.RSFile);
    }

    std::pair<ID3D12RootSignature**, ID3D12PipelineState**> AddPipeline(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, const ComputePipelineFiles& files) override
    {
        return AddPipeline_Common(ReloadablePipelineState::Compute(desc, files), desc.pRootSignature, files.RSFile);
    }

    // Common code between AddPipeline for graphics and compute
    std::pair<ID3D12RootSignature**, ID3D12PipelineState**> AddPipeline_Common(
        const ReloadablePipelineState& reloadableState,
        ID3D12RootSignature* pRootSignature,
        const std::wstring& rsFile)
    {
        std::pair<ID3D12RootSignature**, ID3D12PipelineState**> retval{ nullptr, nullptr };

        assert(!mClosedForAddition);
        if (mClosedForAddition)
        {
            return retval;
        }

        // supply your own or let the set build it. Pick only one!
        assert((pRootSignature != NULL) != (!rsFile.empty()));

        if (pRootSignature == NULL && rsFile.empty())
        {
            // in case the assert isn't active, return the nulls.
            return retval;
        }

        mPipelines.push_back(reloadableState);

        if (pRootSignature == NULL)
        {
            // No user-supplied root signature, so need to use the PipelineSet's own.
            // This either inserts a new root sig or takes the old one if it was already present.

            // get the full filename of the root signature's file
            // this is used because the directory watch system needs to know which directory files are in
            // and I might as well disambiguate shaders by full path anyways...
            DWORD fullPathLengthPlusOne = GetFullPathNameW(rsFile.c_str(), 0, NULL, NULL);
            CHECKWIN32(fullPathLengthPlusOne != 0);

            std::unique_ptr<WCHAR[]> fullPathBuffer = std::make_unique<WCHAR[]>(fullPathLengthPlusOne);

            LPWSTR lpFilePart;
            DWORD fullPathResult = GetFullPathNameW(rsFile.c_str(), fullPathLengthPlusOne, fullPathBuffer.get(), &lpFilePart);
            CHECKWIN32(fullPathResult < fullPathLengthPlusOne); // the copy to the buffer should succeed

            bool inserted;
            std::map<std::wstring, ReloadableRootSignature>::iterator it;
            std::tie(it, inserted) = mFileToRS.emplace(fullPathBuffer.get(), ReloadableRootSignature{});
            retval.first = &it->second.pPublicPtr;

            // Initial timestamp
            mFileToLastWriteTimestamp.emplace(fullPathBuffer.get(), 0);

            // add the directory to the set of directories to watch
            // note I'm subslicing the string to keep only the directory part
            mDirectoriesToWatch.emplace(std::wstring(fullPathBuffer.get(), lpFilePart), INVALID_HANDLE_VALUE);

            // Mark the dependency between this root signature and the pipeline
            it->second.DependentPipelines.push_back(&mPipelines.back());

            // Replace the local path with full path
            if (mPipelines.back().Type == ReloadablePipelineState::PT_Graphics)
            {
                mPipelines.back().GfxFiles.RSFile = fullPathBuffer.get();
            }
            else if (mPipelines.back().Type == ReloadablePipelineState::PT_Compute)
            {
                mPipelines.back().ComputeFiles.RSFile = fullPathBuffer.get();
            }
        }
        else
        {
            // User-supplied root signature, so let's just steal it.
            // This either inserts a new root sig or takes the old one if it was already present.
            bool inserted;
            std::set<ID3D12RootSignature*>::iterator it;
            std::tie(it, inserted) = mUserRSes.insert(pRootSignature);
            retval.first = const_cast<ID3D12RootSignature**>(&*it);
        }

        // Copy out the input layout, since it's passed by reference
        if (reloadableState.Type == ReloadablePipelineState::PT_Graphics &&
            reloadableState.GfxPipelineStateDesc.InputLayout.pInputElementDescs != NULL)
        {
            // 16 is a typical maximum for input layout elements.
            if (reloadableState.GfxPipelineStateDesc.InputLayout.NumElements > 16)
            {
                // I still have an emergency fallback so things don't crash in the worst case.
                mPipelines.back().GfxPipelineStateDesc.InputLayout.pInputElementDescs = new D3D12_INPUT_ELEMENT_DESC[reloadableState.GfxPipelineStateDesc.InputLayout.NumElements];
            }
            else
            {
                mPipelines.back().GfxPipelineStateDesc.InputLayout.pInputElementDescs = &mPipelines.back().InputElementDescStorage[0];
            }
            std::copy_n(reloadableState.GfxPipelineStateDesc.InputLayout.pInputElementDescs, reloadableState.GfxPipelineStateDesc.InputLayout.NumElements, const_cast<D3D12_INPUT_ELEMENT_DESC*>(mPipelines.back().GfxPipelineStateDesc.InputLayout.pInputElementDescs));
        }

        retval.second = &mPipelines.back().pPublicPtr;

        // Hook up dependencies between all the shaders and the pipelines using them
        std::array<const std::wstring*, 5> shaderFilenames;
        std::array<std::wstring*, 5> pipelineShaderFilenames;
        int numShaders;
        if (reloadableState.Type == ReloadablePipelineState::PT_Graphics)
        {
            shaderFilenames[0] = &reloadableState.GfxFiles.VSFile;
            shaderFilenames[1] = &reloadableState.GfxFiles.PSFile;
            shaderFilenames[2] = &reloadableState.GfxFiles.DSFile;
            shaderFilenames[3] = &reloadableState.GfxFiles.HSFile;
            shaderFilenames[4] = &reloadableState.GfxFiles.GSFile;

            pipelineShaderFilenames[0] = &mPipelines.back().GfxFiles.VSFile;
            pipelineShaderFilenames[1] = &mPipelines.back().GfxFiles.PSFile;
            pipelineShaderFilenames[2] = &mPipelines.back().GfxFiles.DSFile;
            pipelineShaderFilenames[3] = &mPipelines.back().GfxFiles.HSFile;
            pipelineShaderFilenames[4] = &mPipelines.back().GfxFiles.GSFile;

            numShaders = 5;
        }
        else if (reloadableState.Type == ReloadablePipelineState::PT_Compute)
        {
            shaderFilenames[0] = &reloadableState.ComputeFiles.CSFile;

            pipelineShaderFilenames[0] = &mPipelines.back().ComputeFiles.CSFile;

            numShaders = 1;
        }

        for (int shaderIdx = 0; shaderIdx < numShaders; shaderIdx++)
        {
            const std::wstring* shaderFilename = shaderFilenames[shaderIdx];

            if (shaderFilename->empty())
            {
                // no file associated to this shader stage
                continue;
            }

            // get the full filename of the shader's file
            // this is used because the directory watch system needs to know which directory files are in
            // and I might as well disambiguate shaders by full path anyways...
            DWORD fullPathLengthPlusOne = GetFullPathNameW(shaderFilename->c_str(), 0, NULL, NULL);
            CHECKWIN32(fullPathLengthPlusOne != 0);

            std::unique_ptr<WCHAR[]> fullPathBuffer = std::make_unique<WCHAR[]>(fullPathLengthPlusOne);

            LPWSTR lpFilePart;
            DWORD fullPathResult = GetFullPathNameW(shaderFilename->c_str(), fullPathLengthPlusOne, fullPathBuffer.get(), &lpFilePart);
            CHECKWIN32(fullPathResult < fullPathLengthPlusOne); // the copy to the buffer should succeed

                                                                // add the shader file to the list of shader files
            bool inserted;
            std::map<std::wstring, ReloadableShader>::iterator it;
            std::tie(it, inserted) = mFileToShader.emplace(fullPathBuffer.get(), ReloadableShader{});

            // Initial timestamp
            mFileToLastWriteTimestamp.emplace(fullPathBuffer.get(), 0);

            // add the directory to the set of directories to watch
            // note I'm subslicing the string to keep only the directory part
            mDirectoriesToWatch.emplace(std::wstring(fullPathBuffer.get(), lpFilePart), INVALID_HANDLE_VALUE);

            // mark the dependency so we know what pipeline to reload if this shader changes
            it->second.DependentPipelines.push_back(&mPipelines.back());

            // Replace the local path with full path
            *pipelineShaderFilenames[shaderIdx] = fullPathBuffer.get();
        }

        return retval;
    }

    // Builds all the pipelines for the first time. Should only be called by BuildAllAsync().
    void DoInitialAsyncBuildAll()
    {
        struct MappedFile
        {
            HANDLE hFile;
            HANDLE hFileMapping;
            PVOID pData;
            LARGE_INTEGER DataSize;
        };

        // file mappings for all files referred to by pipelines
        std::unordered_map<std::wstring, MappedFile> filename2mapping;

        // map all the files referenced by pipelines and put them in the unordered_map
        // If the mapping fails, the file's MappedFile will be set to its null values.
        for (int i = 0; i < (int)mPipelines.size(); i++)
        {
            // group up all the filenames in an array to handle them all uniformly in the following loop.
            std::array<const std::wstring*, 6> filenames;
            int numFiles;
            if (mPipelines[i].Type == ReloadablePipelineState::PT_Graphics)
            {
                filenames[0] = &mPipelines[i].GfxFiles.RSFile;
                filenames[1] = &mPipelines[i].GfxFiles.VSFile;
                filenames[2] = &mPipelines[i].GfxFiles.PSFile;
                filenames[3] = &mPipelines[i].GfxFiles.DSFile;
                filenames[4] = &mPipelines[i].GfxFiles.HSFile;
                filenames[5] = &mPipelines[i].GfxFiles.GSFile;

                numFiles = 6;
            }
            else if (mPipelines[i].Type == ReloadablePipelineState::PT_Compute)
            {
                filenames[0] = &mPipelines[i].ComputeFiles.RSFile;
                filenames[1] = &mPipelines[i].ComputeFiles.CSFile;

                numFiles = 2;
            }

            // Try to map all the files referred to by this pipeline.
            for (int fileIdx = 0; fileIdx < numFiles; fileIdx++)
            {
                const std::wstring* filename = filenames[fileIdx];

                if (filename->empty())
                {
                    // no filename specified for this shader stage or root signature
                    continue;
                }

                // It's possible that this file was already mapped by another pipeline that referred to it.
                // This will be conveyed through the result of emplace, meaning we can skip mapping this file redundantly.
                auto found = filename2mapping.emplace(*filename, MappedFile{ INVALID_HANDLE_VALUE, NULL, NULL, 0 });
                if (!found.second)
                {
                    continue;
                }

                MappedFile mapping = { INVALID_HANDLE_VALUE, NULL, NULL, 0 };

                // Creating a file mapping involves three steps:
                // 1) Opening the file
                // 2) Creating a FileMapping object
                // 3) Mapping the file to a pointer
                // any of these steps can fail, most commonly due to not finding the file or due to a file read ownership issue (like the file is currently being written).
                // If any of the steps fails, then all the other previous steps are undone.
                mapping.hFile = CreateFileW(filename->c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (mapping.hFile == INVALID_HANDLE_VALUE)
                {
                    goto fail;
                }

                // Set the actual initial timestamp
                {
                    FILETIME lastWriteTime;
                    CHECKWIN32(GetFileTime(mapping.hFile, NULL, NULL, &lastWriteTime) != FALSE);
                    LARGE_INTEGER largeWriteTime;
                    largeWriteTime.HighPart = lastWriteTime.dwHighDateTime;
                    largeWriteTime.LowPart = lastWriteTime.dwLowDateTime;

                    mFileToLastWriteTimestamp.at(*filename) = largeWriteTime.QuadPart;
                }

                CHECKWIN32(GetFileSizeEx(mapping.hFile, &mapping.DataSize) != FALSE);

                mapping.hFileMapping = CreateFileMappingW(mapping.hFile, NULL, PAGE_READONLY, 0, 0, NULL);
                if (mapping.hFileMapping == NULL)
                {
                    goto fail;
                }

                mapping.pData = MapViewOfFile(mapping.hFileMapping, FILE_MAP_READ, 0, 0, 0);
                if (!mapping.pData)
                {
                    goto fail;
                }

                // If we got here, that means we successfully mapped the file without error.
                // In this case, the result of the mapping is stored in filename2mapping, and we move on to the next file.
                found.first->second = mapping;
                continue;

            fail:
                // report an error (assuming it was a Win32 error) and undo all the work.
                _com_error err(HRESULT_FROM_WIN32(GetLastError()));
                fwprintf(stderr, L"Error mapping file %s: %s\n", filename->c_str(), err.ErrorMessage());

                if (mapping.pData != NULL)
                {
                    CHECKWIN32(UnmapViewOfFile(mapping.pData) != FALSE);
                }
                if (mapping.hFileMapping != NULL)
                {
                    CHECKWIN32(CloseHandle(mapping.hFileMapping) != FALSE);
                }
                if (mapping.hFile != INVALID_HANDLE_VALUE)
                {
                    CHECKWIN32(CloseHandle(mapping.hFile) != FALSE);
                }
            }
        }

        // Build all the root signatures for pipelines that don't have one already specified
        for (std::pair<const std::wstring, ReloadableRootSignature>& rsf2rs : mFileToRS)
        {
            // Grab the file mapping of the root signature
            MappedFile mapping = filename2mapping.at(rsf2rs.first);
            if (mapping.pData == NULL)
            {
                // this file was failed to be mapped :(
                // The ReloadableRootSignature will remain NULL to indicate that it failed.
                continue;
            }

            // Store the timestamp for this modification
            {
                FILETIME writeTime;
                CHECKWIN32(GetFileTime(mapping.hFile, NULL, NULL, &writeTime) != FALSE);

                LARGE_INTEGER largeWriteTime;
                largeWriteTime.HighPart = writeTime.dwHighDateTime;
                largeWriteTime.LowPart = writeTime.dwLowDateTime;
            }

            // Attempt to create the root signature
            ID3D12RootSignature* pRootSignature;
            HRESULT hr = mpDevice->CreateRootSignature(0, mapping.pData, mapping.DataSize.QuadPart, IID_PPV_ARGS(&pRootSignature));
            if (SUCCEEDED(hr))
            {
                // It succeeded! Hold on to it.
                rsf2rs.second.pInternalPtr = pRootSignature;
            }
            else
            {
                // It failed, so report an error message and leave the ReloadableRootSignature as NULL.
                _com_error err(hr);
                fwprintf(stderr, L"Error building RootSignature %s: %s\n", rsf2rs.first.c_str(), err.ErrorMessage());
            }
        }

        // The pipelines with errors are all handled after the building, done in serial.
        // This is to avoid errors coming in scrambled due to multi-threading
        std::vector<HRESULT> pipelineBuildStatus(mPipelines.size(), S_OK);

        // Now that all the root signatures have been (in theory) created, it's time to build the PSOs.
        // This code is run in multiple threads. PPL hopefully auto-magically balance the work, otherwise this could be tweaked with proper partitioning.
        concurrency::parallel_for(0, (int)mPipelines.size(), [&](int i)
        {
            std::wstring rsfile;
            ID3D12RootSignature** ppRootSignature = NULL;
            D3D12_GRAPHICS_PIPELINE_STATE_DESC gfxPSODesc;
            D3D12_COMPUTE_PIPELINE_STATE_DESC computePSODesc;
            std::array<std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*>, 5> bytecodes;
            int numBytecodes;
            if (mPipelines[i].Type == ReloadablePipelineState::PT_Graphics)
            {
                rsfile = mPipelines[i].GfxFiles.RSFile;
                gfxPSODesc = mPipelines[i].GfxPipelineStateDesc;
                ppRootSignature = &gfxPSODesc.pRootSignature;

                bytecodes[0] = { &mPipelines[i].GfxFiles.VSFile, &gfxPSODesc.VS };
                bytecodes[1] = { &mPipelines[i].GfxFiles.PSFile, &gfxPSODesc.PS };
                bytecodes[2] = { &mPipelines[i].GfxFiles.DSFile, &gfxPSODesc.DS };
                bytecodes[3] = { &mPipelines[i].GfxFiles.HSFile, &gfxPSODesc.HS };
                bytecodes[4] = { &mPipelines[i].GfxFiles.GSFile, &gfxPSODesc.GS };

                numBytecodes = 5;
            }
            else if (mPipelines[i].Type == ReloadablePipelineState::PT_Compute)
            {
                rsfile = mPipelines[i].ComputeFiles.RSFile;
                computePSODesc = mPipelines[i].ComputePipelineStateDesc;
                ppRootSignature = &computePSODesc.pRootSignature;

                bytecodes[0] = { &mPipelines[i].ComputeFiles.CSFile, &computePSODesc.CS };

                numBytecodes = 1;
            }

            if (ppRootSignature && *ppRootSignature == NULL)
            {
                // If there was no user-supplied root signature, then it should come from the file.
                // This was reloaded previously in this function, or maybe it failed in which case this will be NULL still.
                *ppRootSignature = mFileToRS.at(rsfile).pInternalPtr;

                if (*ppRootSignature == NULL)
                {
                    // No root signature :/
                    // None user-supplied, and/or the one from the file failed to be created.
                    // Just skip building the PSO, and leave it null.
                    return; // note: return from the parallel_for
                }
            }

            // If any of the bytecodes failed to be found during the mapping phase, then we can't build the PSO.
            // This flag checks for this condition.
            bool bytecodeMissing = false;

            // Assign all the shader bytecodes in the PSO desc (or notice one is missing)
            for (int bytecodeIdx = 0; bytecodeIdx < numBytecodes; bytecodeIdx++)
            {
                std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*> bytecode = bytecodes[bytecodeIdx];

                if (bytecode.first->empty())
                {
                    // no filename was assigned to this bytecode.
                    // That means this is a user-supplied bytecode,
                    // which means the user already put the right bytecode in the .VS of the pipeline.
                    // We don't need to look for a file mapping, so we can happily skip this one.
                    continue;
                }

                // grab the mapping for this file
                MappedFile mapping = filename2mapping.at(*bytecode.first);

                if (mapping.pData == NULL)
                {
                    // Looks like the mapping of a shader failed, so we have to abandon building the PSO.
                    bytecodeMissing = true;
                    continue;
                }

                // The file mapping was found, so we can grab it and set it in the desc.
                bytecode.second->pShaderBytecode = mapping.pData;
                bytecode.second->BytecodeLength = mapping.DataSize.QuadPart;
            }

            if (bytecodeMissing)
            {
                // If one of the shader bytecodes was missing, we can't compile this PSO, so just skip it and leave it NULL.
                return; // note: return from the parallel_for
            }

            // At this point we've secured all the pieces of the PSO needed to build it, so let's build it already! (note: it may fail)
            if (mPipelines[i].Type == ReloadablePipelineState::PT_Graphics)
            {
                pipelineBuildStatus[i] = mpDevice->CreateGraphicsPipelineState(&gfxPSODesc, __uuidof(ID3D12PipelineState), (void**)&mPipelines[i].pInternalPtr);
            }
            else if (mPipelines[i].Type == ReloadablePipelineState::PT_Compute)
            {
                pipelineBuildStatus[i] = mpDevice->CreateComputePipelineState(&computePSODesc, __uuidof(ID3D12PipelineState), (void**)&mPipelines[i].pInternalPtr);
            }
        });

        // Report errors for all pipelines that failed to be built
        for (int i = 0; i < (int)mPipelines.size(); i++)
        {
            HRESULT hr = pipelineBuildStatus[i];

            if (SUCCEEDED(hr))
            {
                continue;
            }

            // Attempt to make a more descriptive error by sticking together all the files involved.
            std::array<const std::wstring*, 6> filenames;
            const wchar_t* filePrefixes[6];
            int numFilenames;
            if (mPipelines[i].Type == ReloadablePipelineState::PT_Graphics)
            {
                filenames[0] = &mPipelines[i].GfxFiles.RSFile;
                filenames[1] = &mPipelines[i].GfxFiles.VSFile;
                filenames[2] = &mPipelines[i].GfxFiles.PSFile;
                filenames[3] = &mPipelines[i].GfxFiles.DSFile;
                filenames[4] = &mPipelines[i].GfxFiles.HSFile;
                filenames[5] = &mPipelines[i].GfxFiles.GSFile;

                filePrefixes[0] = L"RS";
                filePrefixes[1] = L"VS";
                filePrefixes[2] = L"PS";
                filePrefixes[3] = L"DS";
                filePrefixes[4] = L"HS";
                filePrefixes[5] = L"GS";

                numFilenames = 6;
            }
            else if (mPipelines[i].Type == ReloadablePipelineState::PT_Compute)
            {
                filenames[0] = &mPipelines[i].ComputeFiles.RSFile;
                filenames[1] = &mPipelines[i].ComputeFiles.CSFile;

                filePrefixes[0] = L"RS";
                filePrefixes[1] = L"CS";

                numFilenames = 2;
            }

            std::wstring filenamesDisplay = L" (";
            bool firstName = true;
            for (int fileIdx = 0; fileIdx < numFilenames; fileIdx++)
            {
                const std::wstring* name = filenames[fileIdx];

                if (name->empty())
                {
                    continue;
                }

                if (!firstName)
                {
                    filenamesDisplay += L", ";
                }

                filenamesDisplay += filePrefixes[fileIdx];
                filenamesDisplay += L":";
                filenamesDisplay += *name;

                firstName = false;
            }

            filenamesDisplay += L")";

            const wchar_t* psoStateType = NULL;
            if (mPipelines[i].Type == ReloadablePipelineState::PT_Graphics)
            {
                psoStateType = L"GraphicsPipelineState";
            }
            else if (mPipelines[i].Type == ReloadablePipelineState::PT_Compute)
            {
                psoStateType = L"ComputePipelineState";
            }

            _com_error err(hr);
            fwprintf(stderr, L"Error building %s%s: %s\n", psoStateType, filenamesDisplay.c_str(), err.ErrorMessage());
        }

        // At this point, all possible root signatures and pipeline states have been created.
        // Only the pInternalPtrs have been set, meaning the user still has to call UpdatePipelines() to commit them to pPublicPtrs.

        // Unmap all the files that were mapped at the start of this function
        for (auto& f2m : filename2mapping)
        {
            MappedFile mapping = f2m.second;
            if (mapping.pData != NULL)
            {
                CHECKWIN32(UnmapViewOfFile(mapping.pData) != FALSE);
            }
            if (mapping.hFileMapping != NULL)
            {
                CHECKWIN32(CloseHandle(mapping.hFileMapping) != FALSE);
            }
            if (mapping.hFile != INVALID_HANDLE_VALUE)
            {
                CHECKWIN32(CloseHandle(mapping.hFile) != FALSE);
            }
        }

        // Start the directory change notifications
        for (std::pair<const std::wstring, HANDLE>& name2hdir : mDirectoriesToWatch)
        {
            // Open handle to the directory
            name2hdir.second = CreateFileW(name2hdir.first.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
            CHECKWIN32(name2hdir.second != INVALID_HANDLE_VALUE);
        }

        mDirectoryWatcher.DirectoryWatcherThread = std::thread([this] { DirectoryWatcherMain(); });
        SetThreadName(mDirectoryWatcher.DirectoryWatcherThread, "PSO Directory Watcher");
    }

    // This function gets called whenever a directory change notification happens.
    // Since these directory change notifications happen through the directory watcher thread's APC queue,
    // this function should only ever be called once at a time (ie. it's doesn't need to be reentrant.)
    // However, this function still has to play nice with API calls to PipelineSet happening in parallel to it, so do proper locking when necessary.
    void ProcessFileChangeNotifications(const WCHAR* pszDirectoryPath, char* pDirectoryChangeBuffer, DWORD dwDirectoryChangeBufferSizeInBytes)
    {
        // fwprintf(stdout, L"Directory %s had a change\n", pszDirectoryPath);

        // All the files that were modified according to this directory change notification
        std::set<std::wstring> modifiedFiles;

        // Read the results of the directory changes
        DWORD dwCurrOffset = 0;
        while (dwCurrOffset < dwDirectoryChangeBufferSizeInBytes)
        {
            FILE_NOTIFY_INFORMATION* pNotification = (FILE_NOTIFY_INFORMATION*)(pDirectoryChangeBuffer + dwCurrOffset);

            if (pNotification->Action == FILE_ACTION_MODIFIED)
            {
                // build a null terminated version of the filename (it isn't null terminated inside the FILE_NOTIFY_INFORMATION...)
                // Also FileNameLength is _in bytes_, not symbols. *shakes fist*
                std::wstring filename(pNotification->FileName, pNotification->FileName + pNotification->FileNameLength / sizeof(WCHAR));

                // The filename is relative to the directory it came from, so have to concatenate them to get the full path.
                PWSTR pCombined;
                CHECKHR(PathAllocCombine(pszDirectoryPath, filename.c_str(), PATHCCH_ALLOW_LONG_PATHS, &pCombined));

                // dumb copy, but probably need it as a wstring anyways to emplace it
                std::wstring combined = pCombined;

                // log the modified file only if it's actually a shader file we care about
                if (mFileToRS.find(combined) != end(mFileToRS) ||
                    mFileToShader.find(combined) != end(mFileToShader))
                {
                    modifiedFiles.insert(std::move(combined));
                }

                // free memory allocated by PathAllocCombine
                LocalFree(pCombined);
            }

            if (pNotification->NextEntryOffset == 0)
            {
                // NextEntryOffset == 0 indicates the last entry
                dwCurrOffset = dwDirectoryChangeBufferSizeInBytes;
                break;
            }
            else
            {
                // go to the next notification
                dwCurrOffset += pNotification->NextEntryOffset;
            }
        }

        // Now that we have all the modified files, we need to figure out what needs to be rebuilt.
        std::set<std::pair<std::wstring, ReloadableRootSignature*>> rootSigsToRebuild;
        std::set<ReloadablePipelineState*> pipelinesToRebuild;

        // build a list of all the files that need to be memory mapped to rebuild the required resources
        // This is not just the modified files. For example, modifying a VS means also re-loading the PS associated to it.
        std::set<std::wstring> filesThatNeedMapping;

        for (const std::wstring& modifiedFile : modifiedFiles)
        {
            // fwprintf(stdout, L"File %s was modified\n", modifiedFile.c_str());

            filesThatNeedMapping.insert(modifiedFile);

            // Add all pipelines that depend on this root signature (if it's used as a root signature)
            auto foundRS = mFileToRS.find(modifiedFile);
            if (foundRS != end(mFileToRS))
            {
                rootSigsToRebuild.emplace(modifiedFile, &foundRS->second);
                for (ReloadablePipelineState* pDependentPipeline : foundRS->second.DependentPipelines)
                {
                    pipelinesToRebuild.insert(pDependentPipeline);
                }
            }

            // Add all the pipelines that depend on this shader (if it's used as a shader)
            auto foundShader = mFileToShader.find(modifiedFile);
            if (foundShader != end(mFileToShader))
            {
                for (ReloadablePipelineState* pDependentPipeline : foundShader->second.DependentPipelines)
                {
                    pipelinesToRebuild.insert(pDependentPipeline);

                    // Also add the other files associated to this pipeline
                    std::array<const std::wstring*, 6> filenames;
                    int numFilenames;
                    if (pDependentPipeline->Type == ReloadablePipelineState::PT_Graphics)
                    {
                        filenames[0] = &pDependentPipeline->GfxFiles.RSFile;
                        filenames[1] = &pDependentPipeline->GfxFiles.VSFile;
                        filenames[2] = &pDependentPipeline->GfxFiles.PSFile;
                        filenames[3] = &pDependentPipeline->GfxFiles.DSFile;
                        filenames[4] = &pDependentPipeline->GfxFiles.HSFile;
                        filenames[5] = &pDependentPipeline->GfxFiles.GSFile;

                        numFilenames = 6;
                    }
                    else if (pDependentPipeline->Type == ReloadablePipelineState::PT_Compute)
                    {
                        filenames[0] = &pDependentPipeline->ComputeFiles.RSFile;
                        filenames[1] = &pDependentPipeline->ComputeFiles.CSFile;

                        numFilenames = 2;
                    }

                    for (int fileIdx = 0; fileIdx < numFilenames; fileIdx++)
                    {
                        const std::wstring* pFilename = filenames[fileIdx];

                        if (!pFilename->empty())
                        {
                            filesThatNeedMapping.insert(*pFilename);
                        }
                    }
                }
            }
        }

        struct MappedFile
        {
            HANDLE hFile;
            HANDLE hFileMapping;
            PVOID pData;
            LARGE_INTEGER DataSize;

            // true if the file's last write timestamp is different from last time
            // used to avoid building RS/PSOs twice if redundant directory change notifications come in
            bool ActuallyChanged;
        };

        // file mappings for all files referred to by the changes
        std::unordered_map<std::wstring, MappedFile> filename2mapping;

        // map all the files that have been modified
        for (const std::wstring& modifiedFile : filesThatNeedMapping)
        {
            auto found = filename2mapping.emplace(modifiedFile, MappedFile{ INVALID_HANDLE_VALUE, NULL, NULL, 0, false });
            assert(found.second);

            MappedFile mapping = { INVALID_HANDLE_VALUE, NULL, NULL, 0 };

            // wish I could use gotos here but sigh C++
            bool failed = false;

            // Creating a file mapping involves three steps:
            // 1) Opening the file
            // 2) Creating a FileMapping object
            // 3) Mapping the file to a pointer
            // any of these steps can fail, most commonly due to not finding the file or due to a file read ownership issue (like the file is currently being written).
            // If any of the steps fails, then all the other previous steps are undone.
            if (!failed)
            {
                // If we try to open the file immediately after getting the file change event,
                // it'll likely fail to open it because it's still currently being written to.
                // To handle this, just try with some sleeping in between.
                const int kMaxCreateFileAttempts = 100;
                const int kSleepMillisecondsBetweenAttempts = 10;

                for (int attempt = 0; attempt < kMaxCreateFileAttempts; attempt++)
                {
                    mapping.hFile = CreateFileW(modifiedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (mapping.hFile != INVALID_HANDLE_VALUE)
                    {
                        // We did it boys!
                        break;
                    }

                    Sleep(kSleepMillisecondsBetweenAttempts);
                }

                // Still couldn't open it? Alright, give up.
                if (mapping.hFile == INVALID_HANDLE_VALUE)
                {
                    failed = true;
                }
            }

            if (!failed)
            {
                CHECKWIN32(GetFileSizeEx(mapping.hFile, &mapping.DataSize) != FALSE);
            }

            if (!failed)
            {
                // Check if the file actually changed and update its timestamp if so
                FILETIME lastWriteTime;
                CHECKWIN32(GetFileTime(mapping.hFile, NULL, NULL, &lastWriteTime) != FALSE);
                LARGE_INTEGER largeWriteTime;
                largeWriteTime.HighPart = lastWriteTime.dwHighDateTime;
                largeWriteTime.LowPart = lastWriteTime.dwLowDateTime;

                auto pCurrTimestamp = &mFileToLastWriteTimestamp.at(modifiedFile);
                if ((UINT64)largeWriteTime.QuadPart > *pCurrTimestamp)
                {
                    mapping.ActuallyChanged = true;
                    *pCurrTimestamp = largeWriteTime.QuadPart;
                }
            }

            if (!failed)
            {
                mapping.hFileMapping = CreateFileMappingW(mapping.hFile, NULL, PAGE_READONLY, 0, 0, NULL);
                if (mapping.hFileMapping == NULL)
                {
                    failed = true;
                }
            }

            if (!failed)
            {
                mapping.pData = MapViewOfFile(mapping.hFileMapping, FILE_MAP_READ, 0, 0, 0);
                if (!mapping.pData)
                {
                    failed = true;
                }
            }

            if (!failed)
            {
                // If we got here, that means we successfully mapped the file without error.
                // In this case, the result of the mapping is stored in filename2mapping, and we move on to the next file.
                found.first->second = mapping;
                continue;
            }
            else
            {
                // report an error (assuming it was a Win32 error) and undo all the work.
                _com_error err(HRESULT_FROM_WIN32(GetLastError()));
                fwprintf(stderr, L"Error mapping file %s: %s\n", modifiedFile.c_str(), err.ErrorMessage());

                if (mapping.pData != NULL)
                {
                    CHECKWIN32(UnmapViewOfFile(mapping.pData) != FALSE);
                }
                if (mapping.hFileMapping != NULL)
                {
                    CHECKWIN32(CloseHandle(mapping.hFileMapping) != FALSE);
                }
                if (mapping.hFile != INVALID_HANDLE_VALUE)
                {
                    CHECKWIN32(CloseHandle(mapping.hFile) != FALSE);
                }
            }
        }

        // Lock a mutex to get access to the public and internal ptrs without another thread trying to read them.
        {
            std::lock_guard<std::mutex> ptrLock(mReloadablePointerLock);

            // Rebuild all the root signatures
            for (const std::pair<std::wstring, ReloadableRootSignature*>& rsf2rs : rootSigsToRebuild)
            {
                // wish I could use gotos here but sigh C++
                bool failed = false;

                // Grab the file mapping of the root signature
                MappedFile mapping;
                if (!failed)
                {
                    mapping = filename2mapping.at(rsf2rs.first);
                    if (mapping.pData == NULL)
                    {
                        // this file was failed to be mapped :(
                        // The ReloadableRootSignature will be set to NULL to indicate that it failed.
                        failed = true;
                    }
                    else if (!mapping.ActuallyChanged)
                    {
                        // The root sig file didn't actually change, so we don't need to redundantly rebuild this.
                        // Just bail out.
                        continue;
                    }
                }

                // Attempt to create the root signature
                ID3D12RootSignature* pRootSignature = NULL;
                if (!failed)
                {
                    HRESULT hr = mpDevice->CreateRootSignature(0, mapping.pData, mapping.DataSize.QuadPart, IID_PPV_ARGS(&pRootSignature));
                    if (FAILED(hr))
                    {
                        // It failed, so report an error message and leave the ReloadableRootSignature as NULL.
                        _com_error err(hr);
                        fwprintf(stderr, L"Error building RootSignature %s: %s\n", rsf2rs.first.c_str(), err.ErrorMessage());

                        failed = true;
                    }
                    else
                    {
                        fwprintf(stderr, L"Successfully rebuilt RootSignature %s\n", rsf2rs.first.c_str());
                    }
                }

                if (!failed)
                {
                    // If you got to this point, you didn't fail. Congrats!

                    // Release the old internal (and not published) pointer if it existed
                    // Note this old internal ptr could be the same ptr as the new pRootSignature if they had identical input
                    // but the second Create() will have incremented the refcount, so it should all be good.
                    if (rsf2rs.second->pInternalPtr != rsf2rs.second->pPublicPtr)
                    {
                        if (rsf2rs.second->pInternalPtr != NULL)
                        {
                            rsf2rs.second->pInternalPtr->Release();
                        }
                    }

                    // Hold on to the new value
                    rsf2rs.second->pInternalPtr = pRootSignature;

                    // It's possible for the public pointer to already be the same as what was just created,
                    // since D3D12 automatically recycles Root Signatures built with the same description.
                    // If we did indeed get the same pointer back, then there's no need to commit it, since it's already set.
                    if (rsf2rs.second->pPublicPtr != pRootSignature)
                    {
                        // insert this RS in the commit queue if it's not there yet
                        mRootSignaturesToCommit.emplace(rsf2rs.second);
                    }
                }
                else
                {
                    // Release the old internal (and not published) pointer if it existed
                    if (rsf2rs.second->pInternalPtr != rsf2rs.second->pPublicPtr)
                    {
                        if (rsf2rs.second->pInternalPtr != NULL)
                        {
                            rsf2rs.second->pInternalPtr->Release();
                        }
                    }
                    // Replace the new internal pointer with a NULL to indicate failure
                    rsf2rs.second->pInternalPtr = NULL;
                }
            }

            // Rebuild all the PSOs
            for (ReloadablePipelineState* pPipeline : pipelinesToRebuild)
            {
                std::wstring rsfile;
                ID3D12RootSignature** ppRootSignature = NULL;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC gfxPSODesc;
                D3D12_COMPUTE_PIPELINE_STATE_DESC computePSODesc;
                std::array<std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*>, 5> bytecodes;
                int numBytecodes;
                if (pPipeline->Type == ReloadablePipelineState::PT_Graphics)
                {
                    rsfile = pPipeline->GfxFiles.RSFile;
                    gfxPSODesc = pPipeline->GfxPipelineStateDesc;
                    ppRootSignature = &gfxPSODesc.pRootSignature;

                    bytecodes[0] = { &pPipeline->GfxFiles.VSFile, &gfxPSODesc.VS };
                    bytecodes[1] = { &pPipeline->GfxFiles.PSFile, &gfxPSODesc.PS };
                    bytecodes[2] = { &pPipeline->GfxFiles.DSFile, &gfxPSODesc.DS };
                    bytecodes[3] = { &pPipeline->GfxFiles.HSFile, &gfxPSODesc.HS };
                    bytecodes[4] = { &pPipeline->GfxFiles.GSFile, &gfxPSODesc.GS };

                    numBytecodes = 5;
                }
                else if (pPipeline->Type == ReloadablePipelineState::PT_Compute)
                {
                    rsfile = pPipeline->ComputeFiles.RSFile;
                    computePSODesc = pPipeline->ComputePipelineStateDesc;
                    ppRootSignature = &computePSODesc.pRootSignature;

                    bytecodes[0] = { &pPipeline->ComputeFiles.CSFile, &computePSODesc.CS };

                    numBytecodes = 1;
                }

                // wish I could use gotos here but sigh C++
                bool failed = false;

                // It's possible the root signature was redundantly reported to change twice by the directory watcher
                bool didRSActuallyChange = false;

                if (!failed)
                {
                    if (ppRootSignature && *ppRootSignature == NULL)
                    {
                        // If there was no user-supplied root signature, then it should come from the file.

                        // If the RS came from a file, then either it was reloaded earlier in this function or it didn't change at all.
                        // It's also possible that the RS is NULL if the last attempt to create it failed.

                        *ppRootSignature = mFileToRS.at(rsfile).pInternalPtr;

                        if (*ppRootSignature == NULL)
                        {
                            // No root signature :/
                            // None user-supplied, and/or the one from the file failed to be created.
                            failed = true;
                        }
                        else
                        {
                            auto foundMapping = filename2mapping.find(rsfile);
                            if (foundMapping != end(filename2mapping))
                            {
                                if (foundMapping->second.ActuallyChanged)
                                {
                                    didRSActuallyChange = true;
                                }
                            }
                        }
                    }
                }

                if (!failed)
                {
                    // If any of the bytecodes failed to be found during the mapping phase, then we can't build the PSO.
                    // This flag checks for this condition.
                    bool bytecodeMissing = false;

                    // It's possible that some bytecode was redundantly reported to change twice by the directory watcher
                    bool didBytecodeActuallyChange = false;

                    // Assign all the shader bytecodes in the PSO desc (or notice one is missing)
                    for (int bytecodeIdx = 0; bytecodeIdx < numBytecodes; bytecodeIdx++)
                    {
                        std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*> bytecode = bytecodes[bytecodeIdx];

                        if (bytecode.first->empty())
                        {
                            // no filename was assigned to this bytecode.
                            // That means this is a user-supplied bytecode,
                            // which means the user already put the right bytecode in the .VS of the pipeline.
                            // We don't need to look for a file mapping, so we can happily skip this one.
                            continue;
                        }

                        // grab the mapping for this file
                        MappedFile mapping = filename2mapping.at(*bytecode.first);

                        if (mapping.pData == NULL)
                        {
                            // Looks like the mapping of a shader failed, so we have to abandon building the PSO.
                            bytecodeMissing = true;
                            continue;
                        }

                        if (mapping.ActuallyChanged)
                        {
                            didBytecodeActuallyChange = true;
                        }

                        // The file mapping was found, so we can grab it and set it in the desc.
                        bytecode.second->pShaderBytecode = mapping.pData;
                        bytecode.second->BytecodeLength = mapping.DataSize.QuadPart;
                    }

                    if (bytecodeMissing)
                    {
                        // If one of the shader bytecodes was missing, we can't compile this PSO.
                        failed = true;
                    }

                    if (!didRSActuallyChange && !didBytecodeActuallyChange)
                    {
                        // No dependency of this PSO actually changed, it was just a redundant directory notification.
                        // Just bail out.
                        continue;
                    }
                }

                // At this point we've secured all the pieces of the PSO needed to build it, so let's build it already!
                ID3D12PipelineState* pPipelineState = NULL;
                HRESULT pso_hr;
                if (!failed)
                {
                    if (pPipeline->Type == ReloadablePipelineState::PT_Graphics)
                    {
                        pso_hr = mpDevice->CreateGraphicsPipelineState(&gfxPSODesc, __uuidof(ID3D12PipelineState), (void**)&pPipelineState);
                    }
                    else if (pPipeline->Type == ReloadablePipelineState::PT_Compute)
                    {
                        pso_hr = mpDevice->CreateComputePipelineState(&computePSODesc, __uuidof(ID3D12PipelineState), (void**)&pPipelineState);
                    }
                    
                    if (FAILED(pso_hr))
                    {
                        failed = true;
                    }
                }

                if (!failed)
                {
                    // If you got here, you succeeded. Congrats!

                    // Release the old internal (and not published) pointer if it existed
                    // Note this old internal ptr *might* (see comment below) be the same ptr as the new pPipelineState if they had identical input
                    // but the second Create() will have incremented the refcount, so it should all be good.
                    if (pPipeline->pInternalPtr != pPipeline->pPublicPtr)
                    {
                        if (pPipeline->pInternalPtr != NULL)
                        {
                            pPipeline->pInternalPtr->Release();
                        }
                    }

                    // Hold on to the new value
                    pPipeline->pInternalPtr = pPipelineState;

                    // It's possible for the public pointer to already be the same as what was just created,
                    // since D3D12 automatically recycles Root Signatures built with the same description.
                    // I haven't seen the RootSig recycling behavior happening on PSOs yet,
                    // but might as well handle it in case the behaviour is implementation-specified.

                    // If we did indeed get the same pointer back, then there's no need to commit it, since it's already set.
                    if (pPipeline->pPublicPtr != pPipelineState)
                    {
                        // insert this PSO into the commit queue if it's not there yet
                        mPipelinesToCommit.emplace(pPipeline);
                    }
                }
                else
                {
                    // Oops, it failed. Print an error and leave the PSO null.

                    // Release the old internal (and not published) pointer if it existed
                    if (pPipeline->pInternalPtr != pPipeline->pPublicPtr)
                    {
                        if (pPipeline->pInternalPtr != NULL)
                        {
                            pPipeline->pInternalPtr->Release();
                        }
                    }

                    // Replace the new internal pointer with a NULL to indicate failure
                    pPipeline->pInternalPtr = NULL;
                }

                // Attempt to make a more descriptive error by sticking together all the files involved.
                std::array<const std::wstring*, 6> filenames;
                const wchar_t* filePrefixes[6];
                int numFilenames;
                if (pPipeline->Type == ReloadablePipelineState::PT_Graphics)
                {
                    filenames[0] = &pPipeline->GfxFiles.RSFile;
                    filenames[1] = &pPipeline->GfxFiles.VSFile;
                    filenames[2] = &pPipeline->GfxFiles.PSFile;
                    filenames[3] = &pPipeline->GfxFiles.DSFile;
                    filenames[4] = &pPipeline->GfxFiles.HSFile;
                    filenames[5] = &pPipeline->GfxFiles.GSFile;

                    filePrefixes[0] = L"RS";
                    filePrefixes[1] = L"VS";
                    filePrefixes[2] = L"PS";
                    filePrefixes[3] = L"DS";
                    filePrefixes[4] = L"HS";
                    filePrefixes[5] = L"GS";

                    numFilenames = 6;
                }
                else if (pPipeline->Type == ReloadablePipelineState::PT_Compute)
                {
                    filenames[0] = &pPipeline->ComputeFiles.RSFile;
                    filenames[1] = &pPipeline->ComputeFiles.CSFile;

                    filePrefixes[0] = L"RS";
                    filePrefixes[1] = L"CS";

                    numFilenames = 2;
                }

                std::wstring filenamesDisplay = L" (";
                bool firstName = true;
                for (int fileIdx = 0; fileIdx < numFilenames; fileIdx++)
                {
                    const std::wstring* name = filenames[fileIdx];

                    if (name->empty())
                    {
                        continue;
                    }

                    if (!firstName)
                    {
                        filenamesDisplay += L", ";
                    }

                    filenamesDisplay += filePrefixes[fileIdx];
                    filenamesDisplay += L":";
                    filenamesDisplay += *name;

                    firstName = false;
                }

                filenamesDisplay += L")";

                const wchar_t* psoStateType = NULL;
                if (pPipeline->Type == ReloadablePipelineState::PT_Graphics)
                {
                    psoStateType = L"GraphicsPipelineState";
                }
                else if (pPipeline->Type == ReloadablePipelineState::PT_Compute)
                {
                    psoStateType = L"ComputePipelineState";
                }

                if (failed)
                {
                    _com_error err(pso_hr);
                    fwprintf(stderr, L"Error building %s%s: %s\n", psoStateType, filenamesDisplay.c_str(), err.ErrorMessage());
                }
                else
                {
                    fwprintf(stdout, L"Successfully rebuilt %s%s\n", psoStateType, filenamesDisplay.c_str());
                }
            }
        }

        // Unmap all the files that were mapped at the start of reloading
        for (auto& f2m : filename2mapping)
        {
            MappedFile mapping = f2m.second;
            if (mapping.pData != NULL)
            {
                CHECKWIN32(UnmapViewOfFile(mapping.pData) != FALSE);
            }
            if (mapping.hFileMapping != NULL)
            {
                CHECKWIN32(CloseHandle(mapping.hFileMapping) != FALSE);
            }
            if (mapping.hFile != INVALID_HANDLE_VALUE)
            {
                CHECKWIN32(CloseHandle(mapping.hFile) != FALSE);
            }
        }

        // printf("Finished handling directory change\n");
    }

    static void CALLBACK ReadDirectoryChangesIoCompletion(DWORD dwErrorCode, DWORD dwNumberOfBytesTransferred, LPOVERLAPPED lpOverlapped)
    {
        RDCTask* pTask = (RDCTask*)lpOverlapped->hEvent;

        if (dwErrorCode == ERROR_OPERATION_ABORTED)
        {
            // In this case, the IO task was canceled through CancelIo(), like when the PSO watcher is getting shut down.
            // That means this task is officially done its job.
            *pTask->pOutstandingRDCTaskCount -= 1;
            return;
        }

        // Handle all the file notifications that came from this IO completion
        pTask->pPipelineSet->ProcessFileChangeNotifications(pTask->DirectoryPath.c_str(), pTask->pDirectoryChangeBuffer.get(), dwNumberOfBytesTransferred);

        // Start a new RDC that will call this function again in the future
        BOOL rdcResult = ReadDirectoryChangesW(
            pTask->DirectoryHandle,
            pTask->pDirectoryChangeBuffer.get(),
            RDCTask::kDirectoryChangeBufferSize,
            FALSE, // don't watch subdirectories
            FILE_NOTIFY_CHANGE_LAST_WRITE,
            NULL, // asynchronous mode
            lpOverlapped,
            ReadDirectoryChangesIoCompletion);
        CHECKWIN32(rdcResult != FALSE);
    }

    static void CALLBACK DirectoryWatcherTerminateAPC(ULONG_PTR dwParam)
    {
        DirectoryWatcher* pDirectoryWatcher = reinterpret_cast<DirectoryWatcher*>(dwParam);
        pDirectoryWatcher->ShouldTerminate = true;
    }

    // This function represents the thread that responds to directory change events
    void DirectoryWatcherMain()
    {
        // Number of IO tasks calling ReadDirectoryChanges.
        // Used to know when all tasks have completed and deleted themselves.
        int outstandingRDCTaskCount = 0;

        // Start an async RDC call for each directory to watch
        // The IO Completion callback will get called when the directory changes.
        for (const std::pair<std::wstring, HANDLE>& dir : mDirectoriesToWatch)
        {
            outstandingRDCTaskCount++;

            mDirectoryWatcher.RDCTasks.push_back(std::make_unique<RDCTask>());
            RDCTask* pTask = mDirectoryWatcher.RDCTasks.back().get();

            pTask->DirectoryPath = dir.first;
            pTask->DirectoryHandle = dir.second;
            pTask->Overlapped.hEvent = pTask; // Use the hEvent as a userdata pointer to the task (see comments in RDCTask)
            pTask->pOutstandingRDCTaskCount = &outstandingRDCTaskCount;
            pTask->pDirectoryChangeBuffer = std::make_unique<char[]>(RDCTask::kDirectoryChangeBufferSize);
            pTask->pPipelineSet = this;

            // Initiate the first instance of this task
            BOOL rdcResult = ReadDirectoryChangesW(
                dir.second, 
                pTask->pDirectoryChangeBuffer.get(),
                RDCTask::kDirectoryChangeBufferSize,
                FALSE, // don't watch subdirectories
                FILE_NOTIFY_CHANGE_LAST_WRITE,
                NULL, // asynchronous mode
                &pTask->Overlapped,
                ReadDirectoryChangesIoCompletion);
            CHECKWIN32(rdcResult != FALSE);
        }

        // Main watcher loop
        // RDC IO completions are handled through APCs (this thread sleeps in an alertable state)
        // Requests to terminate the watcher are also handled through APCs.
        while (outstandingRDCTaskCount > 0)
        {
            // Check if a termination request has been received but not initiated yet.
            // If so, initiate the termination by cancelling the tasks, then wait for them all to finish (rdcTaskCount == 0)
            if (mDirectoryWatcher.ShouldTerminate && !mDirectoryWatcher.StartedCancelling)
            {
                for (std::unique_ptr<RDCTask>& pTask : mDirectoryWatcher.RDCTasks)
                {
                    // Cancel the task's current asynchronous RDC
                    // The watcher loop will keep running as long as the RDCs are still being cancelled, since the cancellation happens through its IO completion APC.
                    CHECKWIN32(CancelIoEx(pTask->DirectoryHandle, &pTask->Overlapped) != FALSE);
                }

                mDirectoryWatcher.StartedCancelling = true;
            }

            SleepEx(INFINITE, TRUE);
        }

        // Boy scout rules!
        mDirectoryWatcher.RDCTasks.clear();
    }

    // Creates a threadpool task that initially builds all the pipelines and stuff.
    // Returns a HANDLE that is signaled when the initial build is done.
    HANDLE BuildAllAsync() override
    {
        assert(!mClosedForAddition);
        if (mClosedForAddition)
        {
            return NULL;
        }

        mClosedForAddition = true;

        mhBuildAsyncEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        CHECKWIN32(mhBuildAsyncEvent != NULL);

        PTP_WORK pWork = CreateThreadpoolWork(
            [](
                PTP_CALLBACK_INSTANCE Instance,
                PVOID                 Context,
                PTP_WORK              Work)
        {
            CloseThreadpoolWork(Work);

            PipelineSet* pThis = (PipelineSet*)Context;
            SetEventWhenCallbackReturns(Instance, pThis->mhBuildAsyncEvent);
            pThis->DoInitialAsyncBuildAll();
        }, this, NULL);

        CHECKWIN32(pWork != NULL);

        SubmitThreadpoolWork(pWork);

        return mhBuildAsyncEvent;
    }

    void UpdatePipelines() override
    {
        // If the initial BuildAllAsync() hasn't finished yet, then just return straight away.
        bool buildAllAsyncDone = mhBuildAsyncEvent != NULL && WaitForSingleObjectEx(mhBuildAsyncEvent, 0, FALSE) == WAIT_OBJECT_0;
        if (!buildAllAsyncDone)
        {
            return;
        }

        // Lock a mutex so only this thread can be touching the public and internal pointers
        // Otherwise the async reloading thread might be doing stuff on them at the same time, and bad things will happen.
        std::lock_guard<std::mutex> ptrLock(mReloadablePointerLock);

        // If this is the first time we're updating the pipeline after the first build is done,
        // then we can just grab everything. This is a counter-pessimization (optimization?)
        //  for the case where we don't have to deal with all the complexity of delaying the initialization.
        if (!mDidInitialCommit)
        {
            // The queues aren't necessary in this case, since we're going to commit everything anyways.
            mRootSignaturesToCommit.clear();
            mPipelinesToCommit.clear();
            
            // There also can't be anything put up for garbage collection yet, since no references have escaped the IPipelineSet yet.
            assert(mRootSignatureGarbageCollectQueue.empty());
            assert(mPipelineGarbageCollectQueue.empty());

            // Just straight copy every internal pointer to public pointer
            // The public pointer might still be null if the internal pointer failed to be built
            for (auto& f2rs : mFileToRS)
            {
                f2rs.second.pPublicPtr = f2rs.second.pInternalPtr;
            }

            for (auto& pipeline : mPipelines)
            {
                pipeline.pPublicPtr = pipeline.pInternalPtr;
            }

            mDidInitialCommit = true;
            return;
        }

        // Every Root Siganture in the garbage collection queues is decremented, and if they reach 0 that means we can now safely delete them.
        for (std::map<ID3D12RootSignature*, int>::iterator it = begin(mRootSignatureGarbageCollectQueue); it != end(mRootSignatureGarbageCollectQueue); /* manual "it" update */)
        {
            // get the next one now, because we'll possibly delete the current node.
            std::map<ID3D12RootSignature*, int>::iterator it_next = next(it);

            it->second -= 1;
            if (it->second <= 0)
            {
                // We can now release the root signature since maximum latency frames have passed
                it->first->Release();

                // Remove it from the GC queue now that it's been deleted
                mRootSignatureGarbageCollectQueue.erase(it);
            }

            it = it_next;
        }

        // Every PSO in the garbage collection queues is decremented, and if they reach 0 that means we can now safely delete them.
        for (std::map<ID3D12PipelineState*, int>::iterator it = begin(mPipelineGarbageCollectQueue); it != end(mPipelineGarbageCollectQueue); /* manual "it" update */)
        {
            // get the next one now, because we'll possibly delete the current node.
            std::map<ID3D12PipelineState*, int>::iterator it_next = next(it);

            it->second -= 1;
            if (it->second <= 0)
            {
                // We can now release the PSO since maximum latency frames have passed
                it->first->Release();

                // Remove it from the GC queue now that it's been deleted
                mPipelineGarbageCollectQueue.erase(it);
            }

            it = it_next;
        }

        // Commit all root signatures that have changed since the last update
        for (ReloadableRootSignature* pRS : mRootSignaturesToCommit)
        {
            // assert for good measure. Should always be replacing the pointer with a new one (unless they're both NULL in which case it's a no-op)
            assert((pRS->pPublicPtr == NULL && pRS->pInternalPtr == NULL) != (pRS->pPublicPtr != pRS->pInternalPtr));

            if (pRS->pPublicPtr != NULL)
            {
                // enqueue the public ptr to replace into the garbage collection queue.
                mRootSignatureGarbageCollectQueue.emplace(pRS->pPublicPtr, mMaximumFrameLatency);
            }

            // Expose the new public ptr
            pRS->pPublicPtr = pRS->pInternalPtr;
        }

        // Commit all pipelines that have changed since the last update
        for (ReloadablePipelineState* pPipeline : mPipelinesToCommit)
        {
            // assert for good measure. Should always be replacing the pointer with a new one (unless they're both NULL in which case it's a no-op)
            assert((pPipeline->pPublicPtr == NULL && pPipeline->pInternalPtr == NULL) != (pPipeline->pPublicPtr != pPipeline->pInternalPtr));

            if (pPipeline->pPublicPtr != NULL)
            {
                // enqueue the public ptr to replace into the garbage collection queue.
                mPipelineGarbageCollectQueue.emplace(pPipeline->pPublicPtr, mMaximumFrameLatency);
            }

            // Expose the new public ptr
            pPipeline->pPublicPtr = pPipeline->pInternalPtr;
        }

        // can now ditch the commit queues, since they've all been processed.
        mRootSignaturesToCommit.clear();
        mPipelinesToCommit.clear();
    }
};

std::shared_ptr<IPipelineSet> IPipelineSet::Create(ID3D12Device* pDevice, int maximumFrameLatency)
{
    return std::make_shared<PipelineSet>(pDevice, maximumFrameLatency);
}
