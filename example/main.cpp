#include "pipelineset.h"

#include <d3d12.h>
#include <dxgi1_5.h>
#include <comdef.h>
#include <wrl/client.h>
#include "d3dx12.h"

#include "test.rs.hlsl"

#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cassert>
#include <string>
#include <memory>
#include <tuple>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#pragma region "stupid Win32 copy pasted boilerplate"
namespace detail
{
#define CHECKWIN32(bool_expr) ::detail::CheckWin32((bool_expr), #bool_expr, __FILE__, __FUNCSIG__, __LINE__)
#define CHECKHR(hr_expr)      ::detail::CheckHR  ((hr_expr),   #hr_expr,   __FILE__, __FUNCSIG__, __LINE__)

    bool CheckHR(HRESULT hr, const char* expr, const char* file, const char* function, int line);
    bool CheckWin32(bool okay, const char* expr, const char* file, const char* function, int line);

    void WideFromMultiByte(const char* s, std::wstring& dst)
    {
        int bufSize = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
        CHECKWIN32(bufSize != 0);

        dst.resize(bufSize, 0);
        CHECKWIN32(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, &dst[0], bufSize) != 0);
        dst.pop_back(); // remove null terminator
    }

    void WideFromMultiByte(const std::string& s, std::wstring& dst)
    {
        WideFromMultiByte(s.c_str(), dst);
    }

    void MultiByteFromWide(const wchar_t* ws, std::string& dst)
    {
        int bufSize = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws, -1, NULL, 0, NULL, NULL);
        CHECKWIN32(bufSize != 0);

        dst.resize(bufSize, 0);
        CHECKWIN32(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ws, -1, &dst[0], bufSize, NULL, NULL) != 0);
        dst.pop_back(); // remove null terminator
    }

    void MultiByteFromWide(const std::wstring& ws, std::string& dst)
    {
        MultiByteFromWide(ws.c_str(), dst);
    }

    bool WinAssert(bool okay, const char* error, const char* expr, const char* file, const char* function, int line)
    {
        if (okay)
        {
            return true;
        }

        std::wstring werror;
        WideFromMultiByte(error ? error : "Assertion failed", werror);
        std::wstring wexpr;
        WideFromMultiByte(expr, wexpr);
        std::wstring wfile;
        WideFromMultiByte(file, wfile);
        std::wstring wfunction;
        WideFromMultiByte(function, wfunction);

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
        }

        return false;
    }

    bool CheckHR(HRESULT hr, const char* expr, const char* file, const char* function, int line)
    {
        if (SUCCEEDED(hr))
        {
            return true;
        }

        _com_error err(hr);
        std::string mberr;
        MultiByteFromWide(err.ErrorMessage(), mberr);

        WinAssert(false, mberr.c_str(), expr, file, function, line);

        return false;
    }

    bool CheckWin32(bool okay, const char* expr, const char* file, const char* function, int line)
    {
        if (okay)
        {
            return true;
        }

        return CheckHR(HRESULT_FROM_WIN32(GetLastError()), expr, file, function, line);
    }
}
#pragma endregion

using Microsoft::WRL::ComPtr;

int main()
{
#ifdef _DEBUG
    ComPtr<ID3D12Debug> pDebug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug))))
    {
        pDebug->EnableDebugLayer();
    }
#endif

    ComPtr<IDXGIFactory5> pDXGIFactory;
    CHECKHR(CreateDXGIFactory1(IID_PPV_ARGS(&pDXGIFactory)));

    ComPtr<IDXGIAdapter> pDXGIAdapter;
    CHECKHR(pDXGIFactory->EnumAdapters(0, &pDXGIAdapter));
    ComPtr<IDXGIAdapter2> pDXGIAdapter2;
    CHECKHR(pDXGIAdapter.As<IDXGIAdapter2>(&pDXGIAdapter2));

    DXGI_ADAPTER_DESC2 adapterDesc2;
    CHECKHR(pDXGIAdapter2->GetDesc2(&adapterDesc2));

    wprintf(L"Adapter: %s\n", adapterDesc2.Description);

    ComPtr<ID3D12Device> pDevice;
    CHECKHR(D3D12CreateDevice(pDXGIAdapter.Get(), D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&pDevice)));

    ComPtr<ID3D12CommandQueue> pComputeCommandQueue;

    D3D12_COMMAND_QUEUE_DESC computeCommandQueueDesc = {};
    computeCommandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    CHECKHR(pDevice->CreateCommandQueue(&computeCommandQueueDesc, IID_PPV_ARGS(&pComputeCommandQueue)));

    ComPtr<ID3D12Fence> pFence;
    UINT64 currFenceValue = 0;
    CHECKHR(pDevice->CreateFence(currFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pFence)));

    std::shared_ptr<std::remove_pointer_t<HANDLE>> hFenceEvent(
        CreateEventW(NULL, FALSE, FALSE, NULL),
        CloseHandle);
    CHECKHR(hFenceEvent != NULL);

    ComPtr<ID3D12CommandAllocator> pComputeCmdAlloc;
    CHECKHR(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&pComputeCmdAlloc)));

    ComPtr<ID3D12GraphicsCommandList> pCmdList;
    CHECKHR(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, pComputeCmdAlloc.Get(), NULL, IID_PPV_ARGS(&pCmdList)));

    ComPtr<ID3D12Resource> pOutputBuffer;
    CHECKHR(pDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        NULL, IID_PPV_ARGS(&pOutputBuffer)));

    ComPtr<ID3D12Resource> pReadbackBuffer;
    CHECKHR(pDevice->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT)), D3D12_RESOURCE_STATE_COPY_DEST,
        NULL, IID_PPV_ARGS(&pReadbackBuffer)));

    struct GPUDescriptors
    {
        enum Enum
        {
            OutputBuffer,
            Count
        };
    };

    UINT kCBV_SRV_UAV_Increment = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeap_CBV_SRV_UAV_SV_Desc = {};
    descriptorHeap_CBV_SRV_UAV_SV_Desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeap_CBV_SRV_UAV_SV_Desc.NumDescriptors = GPUDescriptors::Count;
    descriptorHeap_CBV_SRV_UAV_SV_Desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    ComPtr<ID3D12DescriptorHeap> pDescriptorHeap_CBV_SRV_UAV_SV;
    CHECKHR(pDevice->CreateDescriptorHeap(&descriptorHeap_CBV_SRV_UAV_SV_Desc, IID_PPV_ARGS(&pDescriptorHeap_CBV_SRV_UAV_SV)));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_CBV_SRV_UAV_SV = pDescriptorHeap_CBV_SRV_UAV_SV->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_CBV_SRV_UAV_SV = pDescriptorHeap_CBV_SRV_UAV_SV->GetGPUDescriptorHandleForHeapStart();

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeap_CBV_SRV_UAV_Desc = {};
    descriptorHeap_CBV_SRV_UAV_Desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeap_CBV_SRV_UAV_Desc.NumDescriptors = 1;

    ComPtr<ID3D12DescriptorHeap> pDescriptorHeap_CBV_SRV_UAV;
    CHECKHR(pDevice->CreateDescriptorHeap(&descriptorHeap_CBV_SRV_UAV_Desc, IID_PPV_ARGS(&pDescriptorHeap_CBV_SRV_UAV)));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_CBV_SRV_UAV = pDescriptorHeap_CBV_SRV_UAV->GetCPUDescriptorHandleForHeapStart();

    D3D12_CPU_DESCRIPTOR_HANDLE cpuCurrIndexBufferUAVHandle_SV = { cpu_CBV_SRV_UAV_SV.ptr + GPUDescriptors::OutputBuffer * kCBV_SRV_UAV_Increment };
    D3D12_GPU_DESCRIPTOR_HANDLE gpuCurrIndexBufferUAVHandle_SV = { gpu_CBV_SRV_UAV_SV.ptr + GPUDescriptors::OutputBuffer * kCBV_SRV_UAV_Increment };
    D3D12_UNORDERED_ACCESS_VIEW_DESC currIndexBufferUAVDesc = {};
    currIndexBufferUAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    currIndexBufferUAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    currIndexBufferUAVDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    currIndexBufferUAVDesc.Buffer.NumElements = 1;
    pDevice->CreateUnorderedAccessView(pOutputBuffer.Get(), NULL, &currIndexBufferUAVDesc, cpuCurrIndexBufferUAVHandle_SV);

    // Initialize pipeline set
    const int kMaxFrameLatency = 3; // set this to the max number of frames you can buffer (or greater.)
    std::shared_ptr<IPipelineSet> pPipelines = IPipelineSet::Create(pDevice.Get(), kMaxFrameLatency, 0);

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    // I have nothing to set in the PSO desc for a compute shader, since they're so simple.
    // On the other hand, if this was a graphics pipeline set, I would set a bunch of stuff,
    // like the depth state, blend state, etc.
    
    ComputePipelineFiles csfiles;
    // RS is embedded in the CS binary.
    csfiles.RSFile = L"test.cs.cso";
    csfiles.CSFile = L"test.cs.cso";

    ID3D12RootSignature** ppRS;
    ID3D12PipelineState** ppPSO;
    std::tie(ppRS, ppPSO) = pPipelines->AddPipeline(psoDesc, csfiles);

    HANDLE hBuildHandle = pPipelines->BuildAllAsync();
    WaitForSingleObject(hBuildHandle, INFINITE);

    while (true)
    {
        pPipelines->UpdatePipelines();

        if (*ppRS && *ppPSO)
        {
            // Run this frame
            pCmdList->SetComputeRootSignature(*ppRS);
            pCmdList->SetPipelineState(*ppPSO);
            pCmdList->SetComputeRootUnorderedAccessView(TEST_RS_OUTPUT_BUFFER_PARAM_INDEX, pOutputBuffer->GetGPUVirtualAddress());
            pCmdList->Dispatch(1, 1, 1);
            pCmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(pOutputBuffer.Get()));

            pCmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pOutputBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
            pCmdList->CopyResource(pReadbackBuffer.Get(), pOutputBuffer.Get());
            pCmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(pOutputBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

            // Close and submit
            pCmdList->Close();
            ID3D12CommandList* pLists[] = { pCmdList.Get() };
            pComputeCommandQueue->ExecuteCommandLists(1, pLists);

            CHECKHR(pFence->SetEventOnCompletion(currFenceValue + 1, hFenceEvent.get()));
            pComputeCommandQueue->Signal(pFence.Get(), currFenceValue + 1);
            currFenceValue += 1;

            CHECKWIN32(WaitForSingleObject(hFenceEvent.get(), INFINITE) == WAIT_OBJECT_0);

            void* pMappedOutput;
            CHECKHR(pReadbackBuffer->Map(0, &CD3DX12_RANGE(0, sizeof(UINT)), &pMappedOutput));
            printf("Output: %u\n", *(UINT*)pMappedOutput);
            pReadbackBuffer->Unmap(0, &CD3DX12_RANGE(0, 0));

            CHECKHR(pComputeCmdAlloc->Reset());
            pCmdList->Reset(pComputeCmdAlloc.Get(), NULL);
        }
    }
}