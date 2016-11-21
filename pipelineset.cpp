#include "pipelineset.h"

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
        // The public and internal pointers, similar to those in ReloadableRootSignature.
        ID3D12PipelineState* pPublicPtr;
        ID3D12PipelineState* pInternalPtr;

        // The prototype this PSO is based on.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC PipelineStateDesc;

        // The files used to build this pipeline
        GraphicsPipelineFiles Files;

        // Storage for input layout (which is normally passed by reference)
        // If this array is not big enough and the assert doesn't catch it (at runtime in a release build), then a dynamic allocation is used.
        D3D12_INPUT_ELEMENT_DESC InputElementDescStorage[16];
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

    // When RS and PSOs are reloaded there has to be some fiddling between their public and internal pointers.
    // This lock grants exclusive access to ALL the public and internal ptrs in this class.
    std::mutex mReloadablePointerLock;

    // Windows doesn't let you watch files, but you can watch directories.
    // So I just have to collect all the directories I want to watch as the shader files are being added...
    // The HANDLE here is a handle to the directory.
    // This also lets me get the proper access rights so nobody can delete a watched directory out from under my feet.
    std::map<std::wstring, HANDLE> mDirectoriesToWatch;

    // When a change notification handle gets signaled, we can know which directory it was using this map.
    // The HANDLE here is the change notification handle.
    std::map<HANDLE, std::wstring> mChangeNotificationToDirectory;

    // Event to send to the change notification watcher thread to say that it should shut itself down
    HANDLE hEndChangeNotificationsEvent = NULL;

    // Thread that asynchronously watches for any changes to shader files and reloads the RSs and PSOs
    std::thread mChangeNotifWatcherThread;

    // When an object gets live-reloaded, a delay is added before it gets exposed through the public ptr.
    // This delay consists of #mMaximumFrameLatency calls to UpdatePipelines()
    // The number associated to the objects here contain the countdown number, which is decremented with every update.
    // Note these aren't used for the initial creation of resources, as an optimization for the most common scenario of no reloading (see mDidInitialCommit).
    // When the commit is complete, the associated pair gets removed from the map.
    // These maps should only be accessed when mReloadablePointerLock is held.
    std::map<ReloadableRootSignature*, int> mRootSignatureCommitQueue;
    std::map<ReloadablePipelineState*, int> mPipelineCommitQueue;

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
        if (hEndChangeNotificationsEvent != NULL)
        {
            CHECKWIN32(SetEvent(hEndChangeNotificationsEvent) != FALSE);
            mChangeNotifWatcherThread.join();
            CHECKWIN32(CloseHandle(hEndChangeNotificationsEvent) != FALSE);
        }

        // Close all file notification objects
        for (std::pair<const HANDLE, std::wstring>& notif2dir : mChangeNotificationToDirectory)
        {
            CHECKWIN32(FindCloseChangeNotification(notif2dir.first) != FALSE);
        }

        // Close all opened directories
        for (std::pair<const std::wstring, HANDLE>& name2hdir : mDirectoriesToWatch)
        {
            CHECKWIN32(CloseHandle(name2hdir.second) != FALSE);
        }

        // Release all the ID3D12PipelineStates created and owned by the IPipelineSet.
        for (ReloadablePipelineState& pipeline : mPipelines)
        {
            // delete emergency input element storage if it existed
            if (pipeline.PipelineStateDesc.InputLayout.pInputElementDescs != NULL &&
                pipeline.PipelineStateDesc.InputLayout.pInputElementDescs != pipeline.InputElementDescStorage)
            {
                delete[] pipeline.PipelineStateDesc.InputLayout.pInputElementDescs;
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
    }

    std::pair<ID3D12RootSignature**, ID3D12PipelineState**> AddPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const GraphicsPipelineFiles& files) override
    {
        std::pair<ID3D12RootSignature**, ID3D12PipelineState**> retval{ nullptr, nullptr };

        assert(!mClosedForAddition);
        if (mClosedForAddition)
        {
            return retval;
        }

        // supply your own or let the set build it. Pick only one!
        assert((desc.pRootSignature != NULL) != (!files.RSFile.empty()));

        if (desc.pRootSignature == NULL && files.RSFile.empty())
        {
            // in case the assert isn't active, return the nulls.
            return retval;
        }

        mPipelines.push_back(ReloadablePipelineState{ nullptr, nullptr, desc, files });

        if (desc.pRootSignature == NULL)
        {
            // No user-supplied root signature, so need to use the PipelineSet's own.
            // This either inserts a new root sig or takes the old one if it was already present.

            // get the full filename of the root signature's file
            // this is used because the directory watch system needs to know which directory files are in
            // and I might as well disambiguate shaders by full path anyways...
            DWORD fullPathLengthPlusOne = GetFullPathNameW(files.RSFile.c_str(), 0, NULL, NULL);
            CHECKWIN32(fullPathLengthPlusOne != 0);

            std::unique_ptr<WCHAR[]> fullPathBuffer = std::make_unique<WCHAR[]>(fullPathLengthPlusOne);

            LPWSTR lpFilePart;
            DWORD fullPathResult = GetFullPathNameW(files.RSFile.c_str(), fullPathLengthPlusOne, fullPathBuffer.get(), &lpFilePart);
            CHECKWIN32(fullPathResult < fullPathLengthPlusOne); // the copy to the buffer should succeed

            bool inserted;
            std::map<std::wstring, ReloadableRootSignature>::iterator it;
            std::tie(it, inserted) = mFileToRS.emplace(fullPathBuffer.get(), ReloadableRootSignature{});
            retval.first = &it->second.pPublicPtr;

            // add the directory to the set of directories to watch
            // note I'm subslicing the string to keep only the directory part
            mDirectoriesToWatch.emplace(std::wstring(fullPathBuffer.get(), lpFilePart), INVALID_HANDLE_VALUE);

            // Mark the dependency between this root signature and the pipeline
            it->second.DependentPipelines.push_back(&mPipelines.back());

            // Replace the local path with full path
            mPipelines.back().Files.RSFile = fullPathBuffer.get();
        }
        else
        {
            // User-supplied root signature, so let's just steal it.
            // This either inserts a new root sig or takes the old one if it was already present.
            bool inserted;
            std::set<ID3D12RootSignature*>::iterator it;
            std::tie(it, inserted) = mUserRSes.insert(desc.pRootSignature);
            retval.first = const_cast<ID3D12RootSignature**>(&*it);
        }

        // Copy out the input layout, since it's passed by reference

        // 16 is a typical maximum for input layout elements.
        assert(desc.InputLayout.NumElements <= 16);
        if (desc.InputLayout.NumElements > 16)
        {
            // I still have an emergency fallback so things don't crash in the worst case.
            mPipelines.back().PipelineStateDesc.InputLayout.pInputElementDescs = new D3D12_INPUT_ELEMENT_DESC[desc.InputLayout.NumElements];
        }
        else
        {
            mPipelines.back().PipelineStateDesc.InputLayout.pInputElementDescs = &mPipelines.back().InputElementDescStorage[0];
        }
        std::copy_n(desc.InputLayout.pInputElementDescs, desc.InputLayout.NumElements, const_cast<D3D12_INPUT_ELEMENT_DESC*>(mPipelines.back().PipelineStateDesc.InputLayout.pInputElementDescs));

        retval.second = &mPipelines.back().pPublicPtr;

        // Hook up dependencies between all the shaders and the pipelines using them
        std::array<const std::wstring*, 5> shaderFilenames{ {
                &files.VSFile,
                &files.PSFile,
                &files.DSFile,
                &files.HSFile,
                &files.GSFile,
            } };
        std::array<std::wstring*, 5> pipelineShaderFilenames{ {
                &mPipelines.back().Files.VSFile,
                &mPipelines.back().Files.PSFile,
                &mPipelines.back().Files.DSFile,
                &mPipelines.back().Files.HSFile,
                &mPipelines.back().Files.GSFile,
            } };
        for (int shaderIdx = 0; shaderIdx < 5; shaderIdx++)
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
            const GraphicsPipelineFiles& files = mPipelines[i].Files;

            // group up all the filenames in an array to handle them all uniformly in the following loop.
            std::array<const std::wstring*, 6> filenames{ {
                    &files.RSFile,
                    &files.VSFile,
                    &files.PSFile,
                    &files.DSFile,
                    &files.HSFile,
                    &files.GSFile,
                } };

            // Try to map all the files referred to by this pipeline.
            for (const std::wstring* filename : filenames)
            {
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
            const GraphicsPipelineFiles& files = mPipelines[i].Files;

            // build the description of this PSO from the pipeline, in order to create it.
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = mPipelines[i].PipelineStateDesc;

            if (desc.pRootSignature == NULL)
            {
                // If there was no user-supplied root signature, then it should come from the file.
                // This was reloaded previously in this function, or maybe it failed in which case this will be NULL still.
                desc.pRootSignature = mFileToRS.at(files.RSFile).pInternalPtr;

                if (desc.pRootSignature == NULL)
                {
                    // No root signature :/
                    // None user-supplied, and/or the one from the file failed to be created.
                    // Just skip building the PSO, and leave it null.
                    return; // note: return from the parallel_for
                }
            }

            // Have to assign the bytecodes in the PSO Desc based on the files specified in the pipeline
            // They're all put together in one array here to make it easier to handle them all in the same way.
            std::array<std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*>, 5> bytecodes{ {
                { &files.VSFile, &desc.VS },
                { &files.PSFile, &desc.PS },
                { &files.DSFile, &desc.DS },
                { &files.HSFile, &desc.HS },
                { &files.GSFile, &desc.GS },
                } };

            // If any of the bytecodes failed to be found during the mapping phase, then we can't build the PSO.
            // This flag checks for this condition.
            bool bytecodeMissing = false;

            // Assign all the shader bytecodes in the PSO desc (or notice one is missing)
            for (std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*> bytecode : bytecodes)
            {
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
            pipelineBuildStatus[i] = mpDevice->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), (void**)&mPipelines[i].pInternalPtr);
        });

        // Report errors for all pipelines that failed to be built
        for (int i = 0; i < (int)mPipelines.size(); i++)
        {
            HRESULT hr = pipelineBuildStatus[i];

            if (SUCCEEDED(hr))
            {
                continue;
            }

            // Oops, it failed. Print an error and leave the PSO null.
            const GraphicsPipelineFiles& files = mPipelines[i].Files;

            // Attempt to make a more descriptive error by sticking together all the files involved.
            std::array<const std::wstring*, 6> filenames{ {
                    &files.RSFile,
                    &files.VSFile,
                    &files.PSFile,
                    &files.DSFile,
                    &files.HSFile,
                    &files.GSFile,
                } };

            const wchar_t* filePrefixes[] = {
                L"RS",
                L"VS",
                L"PS",
                L"DS",
                L"HS",
                L"GS",
            };

            std::wstring filenamesDisplay = L" (";
            bool firstName = true;
            for (int fileIdx = 0; fileIdx < (int)filenames.size(); fileIdx++)
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

            _com_error err(hr);
            fwprintf(stderr, L"Error building GraphicsPipelineState%s: %s\n", filenamesDisplay.c_str(), err.ErrorMessage());
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
            name2hdir.second = CreateFileW(name2hdir.first.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            CHECKWIN32(name2hdir.second != INVALID_HANDLE_VALUE);

            HANDLE hChangeNotif = FindFirstChangeNotificationW(name2hdir.first.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
            CHECKWIN32(hChangeNotif != INVALID_HANDLE_VALUE);

            mChangeNotificationToDirectory.emplace(hChangeNotif, name2hdir.first);
        }

        hEndChangeNotificationsEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        CHECKWIN32(hEndChangeNotificationsEvent != NULL);

        mChangeNotifWatcherThread = std::thread([this] { ChangeNotificationWatcherMain(); });
        SetThreadName(mChangeNotifWatcherThread, "PSO File Watcher");
    }

    // This function represents the thread that responds to change notification events
    void ChangeNotificationWatcherMain()
    {
        // Buffer to store the results of ReadDirectoryChangesW
        // The choice of 64k is because that's the maximum allowed when you're monitoring a directory over a network (see ReadDirectoryChangesW documentation).
        // If the result needs more memory than this, the ReadDirectoryChangesW will fail, and the code has to fallback to manually traversing the directory.
        size_t kDirectoryChangeBufferSize = 64 * 1024;
        std::unique_ptr<char[]> pDirectoryChangeBuffer = std::make_unique<char[]>(kDirectoryChangeBufferSize);

        // grab a list of all the events to wait for
        std::vector<HANDLE> eventsToWaitFor;
        for (const std::pair<const HANDLE, std::wstring>& notif2dir : mChangeNotificationToDirectory)
        {
            eventsToWaitFor.push_back(notif2dir.first);
        }
        eventsToWaitFor.push_back(hEndChangeNotificationsEvent);

        // The API for reading directory changes has this weird behavior where more than one event happens from one file change
        // Background info: https://blogs.msdn.microsoft.com/oldnewthing/20140507-00/?p=1053/
        //                  http://stackoverflow.com/questions/1764809/filesystemwatcher-changed-event-is-raised-twice
        // Getting 2 events for one change is problematic:
        // The second call to ReadDirectoryChangesW will block, since likely nothing changed in between the 2 events.
        //
        // It's a hack, but since this code is designed to work only with fxc, I'll just assume that the 2 event behaviour is not going to change.
        // Just have a flag per event for this purpose.
        std::vector<bool> eventHappenedOnce(eventsToWaitFor.size() - 1, false);

        for (;;)
        {
            // wait for any of the directories to be changed, or for an event saying this thread should end.
            DWORD waitResult = WaitForMultipleObjectsEx((DWORD)eventsToWaitFor.size(), eventsToWaitFor.data(), FALSE, INFINITE, FALSE);
            CHECKWIN32(waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0 + eventsToWaitFor.size());

            if (waitResult == WAIT_OBJECT_0 + eventsToWaitFor.size() - 1)
            {
                // The last event in the list is the event that says we close shop.
                break;
            }

            if (eventHappenedOnce[waitResult - WAIT_OBJECT_0])
            {
                // this event was a repetition of an event that was already handled
                // reset the flag so we can handle the next (non-repetition) event.
                eventHappenedOnce[waitResult - WAIT_OBJECT_0] = false;

                // Start looking for the next change
                CHECKWIN32(FindNextChangeNotification(eventsToWaitFor[waitResult - WAIT_OBJECT_0]) != FALSE);

                continue;
            }

            // set the flag now that we're actually handling it
            // this'll make it so we don't handle this same change a second time.
            eventHappenedOnce[waitResult - WAIT_OBJECT_0] = true;

            // get the name of the directory that was changed
            const std::wstring& dir = mChangeNotificationToDirectory.at(eventsToWaitFor[waitResult - WAIT_OBJECT_0]);
            // fwprintf(stdout, L"Directory %s had a change\n", dir.c_str());

            // All the files that were modified according to this directory change notification
            std::set<std::wstring> modifiedFiles;

            DWORD lBytesReturned;
            BOOL rdcResult = ReadDirectoryChangesW(mDirectoriesToWatch.at(dir), pDirectoryChangeBuffer.get(), (DWORD)kDirectoryChangeBufferSize, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE, &lBytesReturned, NULL, NULL);
            if (rdcResult != FALSE)
            {
                // Read the results of the directory changes
                DWORD lCurrOffset = 0;
                while (lCurrOffset < lBytesReturned)
                {
                    FILE_NOTIFY_INFORMATION* pNotification = (FILE_NOTIFY_INFORMATION*)(pDirectoryChangeBuffer.get() + lCurrOffset);

                    if (pNotification->Action == FILE_ACTION_MODIFIED)
                    {
                        // build a null terminated version of the filename (it isn't null terminated inside the FILE_NOTIFY_INFORMATION...)
                        std::wstring filename(pNotification->FileName, pNotification->FileName + pNotification->FileNameLength);

                        // The filename is relative to the directory it came from, so have to concatenate them to get the full path.
                        PWSTR pCombined;
                        CHECKHR(PathAllocCombine(dir.c_str(), filename.c_str(), PATHCCH_ALLOW_LONG_PATHS, &pCombined));

                        // log the modified file
                        modifiedFiles.insert(std::wstring(pCombined));

                        // free memory allocated by PathAllocCombine
                        LocalFree(pCombined);
                    }

                    if (pNotification->NextEntryOffset == 0)
                    {
                        // NextEntryOffset == 0 indicates the last entry
                        lCurrOffset = lBytesReturned;
                        break;
                    }
                    else
                    {
                        // go to the next notification
                        lCurrOffset += pNotification->NextEntryOffset;
                    }
                }
            }
            else
            {
                // In this case, rdcResult == FALSE
                // That might mean the operation failed for whatever reason,
                // but if it says ERROR_NOTIFY_ENUM_DIR that means the directory change buffer wasn't big enough.
                // If the directory change buffer isn't big enough, its contents are entirely dropped,
                // and you have to manually check for changes yourself as fallback.
                if (GetLastError() == ERROR_NOTIFY_ENUM_DIR)
                {
                    // For now I'm too lazy to actually do this.
                    // If you somehow filled up that buffer (how?!), then just rebuild your shaders to get the notification.
                    fwprintf(stderr, L"Warning: Failed to ReadDirectoryChangesW (ERROR_NOTIFY_ENUM_DIR)\n");
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
                        std::array<const std::wstring*, 6> filenames{ {
                                &pDependentPipeline->Files.RSFile,
                                &pDependentPipeline->Files.VSFile,
                                &pDependentPipeline->Files.PSFile,
                                &pDependentPipeline->Files.DSFile,
                                &pDependentPipeline->Files.HSFile,
                                &pDependentPipeline->Files.GSFile,
                            } };

                        for (const std::wstring* pFilename : filenames)
                        {
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
            };

            // file mappings for all files referred to by the changes
            std::unordered_map<std::wstring, MappedFile> filename2mapping;

            // map all the files that have been modified
            for (const std::wstring& modifiedFile : filesThatNeedMapping)
            {
                auto found = filename2mapping.emplace(modifiedFile, MappedFile{ INVALID_HANDLE_VALUE, NULL, NULL, 0 });
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
                        if (rsf2rs.second->pInternalPtr != rsf2rs.second->pPublicPtr)
                        {
                            if (rsf2rs.second->pInternalPtr != NULL)
                            {
                                rsf2rs.second->pInternalPtr->Release();
                            }
                        }

                        // Hold on to the new value
                        rsf2rs.second->pInternalPtr = pRootSignature;

                        // insert this RS in the commit queue if it's not there yet
                        // The latency is +1 to account for the last frame where the public pointer might be used
                        mRootSignatureCommitQueue.emplace(rsf2rs.second, mMaximumFrameLatency + 1);
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
                    const GraphicsPipelineFiles& files = pPipeline->Files;

                    // build the description of this PSO from the desc, in order to create it.
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = pPipeline->PipelineStateDesc;

                    // wish I could use gotos here but sigh C++
                    bool failed = false;

                    if (!failed)
                    {
                        if (desc.pRootSignature == NULL)
                        {
                            // If there was no user-supplied root signature, then it should come from the file.

                            // If the RS came from a file, then either it was reloaded earlier in this function or it didn't change at all.
                            // It's also possible that the RS is NULL if the last attempt to create it failed.

                            desc.pRootSignature = mFileToRS.at(files.RSFile).pInternalPtr;

                            if (desc.pRootSignature == NULL)
                            {
                                // No root signature :/
                                // None user-supplied, and/or the one from the file failed to be created.
                                failed = true;
                            }
                        }
                    }

                    if (!failed)
                    {
                        // Have to assign the bytecodes in the PSO Desc based on the files specified in the pipeline
                        // They're all put together in one array here to make it easier to handle them all in the same way.
                        std::array<std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*>, 5> bytecodes{ {
                            { &files.VSFile, &desc.VS },
                            { &files.PSFile, &desc.PS },
                            { &files.DSFile, &desc.DS },
                            { &files.HSFile, &desc.HS },
                            { &files.GSFile, &desc.GS },
                            } };

                        // If any of the bytecodes failed to be found during the mapping phase, then we can't build the PSO.
                        // This flag checks for this condition.
                        bool bytecodeMissing = false;

                        // Assign all the shader bytecodes in the PSO desc (or notice one is missing)
                        for (std::pair<const std::wstring*, D3D12_SHADER_BYTECODE*> bytecode : bytecodes)
                        {
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
                            // If one of the shader bytecodes was missing, we can't compile this PSO.
                            failed = true;
                        }
                    }

                    // At this point we've secured all the pieces of the PSO needed to build it, so let's build it already!
                    ID3D12PipelineState* pPipelineState = NULL;
                    HRESULT pso_hr;
                    if (!failed)
                    {
                        pso_hr = mpDevice->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), (void**)&pPipelineState);
                        if (FAILED(pso_hr))
                        {
                            failed = true;
                        }
                    }

                    if (!failed)
                    {
                        // If you got here, you succeeded. Congrats!

                        // Release the old internal (and not published) pointer if it existed
                        if (pPipeline->pInternalPtr != pPipeline->pPublicPtr)
                        {
                            if (pPipeline->pInternalPtr != NULL)
                            {
                                pPipeline->pInternalPtr->Release();
                            }
                        }

                        // Hold on to the new value
                        pPipeline->pInternalPtr = pPipelineState;

                        // insert this PSO in the commit queue if it's not there yet
                        // The latency is +1 to account for the last frame where the public pointer might be used
                        mPipelineCommitQueue.emplace(pPipeline, mMaximumFrameLatency + 1);
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
                    std::array<const std::wstring*, 6> filenames{ {
                            &files.RSFile,
                            &files.VSFile,
                            &files.PSFile,
                            &files.DSFile,
                            &files.HSFile,
                            &files.GSFile,
                        } };

                    const wchar_t* filePrefixes[] = {
                        L"RS",
                        L"VS",
                        L"PS",
                        L"DS",
                        L"HS",
                        L"GS",
                    };

                    std::wstring filenamesDisplay = L" (";
                    bool firstName = true;
                    for (int fileIdx = 0; fileIdx < (int)filenames.size(); fileIdx++)
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

                    if (failed)
                    {
                        _com_error err(pso_hr);
                        fwprintf(stderr, L"Error building GraphicsPipelineState%s: %s\n", filenamesDisplay.c_str(), err.ErrorMessage());
                    }
                    else
                    {
                        fwprintf(stdout, L"Successfully rebuilt GraphicsPipelineState%s\n", filenamesDisplay.c_str());
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

            // Start looking for the next change
            CHECKWIN32(FindNextChangeNotification(eventsToWaitFor[waitResult - WAIT_OBJECT_0]) != FALSE);
        }
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
        bool buildAllAsyncDone = WaitForSingleObjectEx(mhBuildAsyncEvent, 0, FALSE) == WAIT_OBJECT_0;
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
            // The queues aren't necessary in this case, since there's nothing to invalidate.
            // There might only be something in the queue if you somehow change a file in between BuildAllAsync() finishing and calling UpdatePipelines()
            // It's unlikely, but handled here anyways to be sure.
            mRootSignatureCommitQueue.clear();
            mPipelineCommitQueue.clear();

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

        // Everything in the commit queues is decremented, and if they reach 0 that means we can now safely swap out their pointers.
        for (std::map<ReloadableRootSignature*, int>::iterator it = begin(mRootSignatureCommitQueue); it != end(mRootSignatureCommitQueue); /* manual "it" update */)
        {
            // get the next one now, because we'll possibly delete the current node.
            std::map<ReloadableRootSignature*, int>::iterator it_next = next(it);

            it->second -= 1;
            if (it->second <= 0)
            {
                if (it->first->pPublicPtr != NULL)
                {
                    // We can now release the last public ptr since maximum latency frames have passed
                    it->first->pPublicPtr->Release();
                }

                // Swap out the pointer and remove it from the commit queue
                it->first->pPublicPtr = it->first->pInternalPtr;
                mRootSignatureCommitQueue.erase(it);
            }

            it = it_next;
        }

        for (std::map<ReloadablePipelineState*, int>::iterator it = begin(mPipelineCommitQueue); it != end(mPipelineCommitQueue); /* manual "it" update */)
        {
            // get the next one now, because we'll possibly delete the current node.
            std::map<ReloadablePipelineState*, int>::iterator it_next = next(it);

            it->second -= 1;
            if (it->second <= 0)
            {
                if (it->first->pPublicPtr != NULL)
                {
                    // We can now release the last public ptr since maximum latency frames have passed
                    it->first->pPublicPtr->Release();
                }

                // Swap out the pointer and remove it from the commit queue
                it->first->pPublicPtr = it->first->pInternalPtr;
                mPipelineCommitQueue.erase(it);
            }

            it = it_next;
        }
    }
};

std::shared_ptr<IPipelineSet> IPipelineSet::Create(ID3D12Device* pDevice, int maximumFrameLatency)
{
    return std::make_shared<PipelineSet>(pDevice, maximumFrameLatency);
}
