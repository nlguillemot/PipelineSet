#pragma once

#include <d3d12.h>

#include <string>
#include <memory>

// An extension of the PSO desc that lets you specify a filename from which to read the root signature and shader bytecodes.
// The IPipelineSet will use these filenames in order to build the ID3D12RootSignatures and ID3D12PipelineStates.
// If you want to supply your own root signature or your own shader bytecode, you can set the pRootSignature or VS/PS/HS/DS/GS bytecode pointers manually.
// In the case of user-supplied pRootSignature or bytecodes, the filename should be empty.
//   -> The two methods are currently mututally exclusive.
struct GraphicsPipelineFiles
{
    std::wstring RSFile;
    std::wstring VSFile;
    std::wstring PSFile;
    std::wstring DSFile;
    std::wstring HSFile;
    std::wstring GSFile;
};

// FEATURES OVERVIEW
// -----------------
// IPipelineSet manages a set of pipeline state objects. You may ask, "Why?":
// * IPipelineSet allows you to build a bunch of PSOs asynchronously in parallel. This is important, building PSOs is very expensive (in CPU time).
// * IPipelineSet encapsulates all the tedious file I/O you have to do to read shaders and root signatures from the filesystem.
// * IPipelineSet detects modifications to shader files and live-reloads the PSOs and root signatures that changed. Huge productivity win.
//
// WHY DOUBLE INDIRECTION?
// -----------------------
// Once you've added a pipeline to the set, you'll get back a pointer-pointer to the RootSignature and PipelineState.
// You can pass these ptrptrs to your code that will later consume the built PSO. The reason for the double indirection is that it allows live reloading.
// In other words, when a RS/PSO is live-reloaded, it'll switch out the pointer that your pointer-pointer is pointing to.
// This pointer-pointer update is only done when you call UpdatePipelines(), so your live-reloading doesn't happen mid-frame.
// This also allows you to detect if a RS/PSO is currently invalid (like if it failed to compile). Just check if the *pptr is NULL.
// The *pptr will be set to NULL if the compilation fails, so watch the command line for compiler errors if your scene stops rendering.
//
// USER-SUPPLIED ROOT SIGNATURES AND SHADER BYTECODES
// --------------------------------------------------
// You may want to hook a PSO into this system that uses a pRootSignature or some shader bytecode from an external source.
// For example, you might pre-compile your HLSL bytecode into a C++ array, then pass that array directly to your PSO description.
// To do these things, you can set the pRootSignature or the VS/PS/HS/DS/GS pointers inside the PSO desc.
// This is explained in more detail in the documentation for GraphicsPipelineFiles above.
//
// SHARING LIFETIME
// ----------------
// When you pass in your own pRootSignature, the IPipelineSet will NOT take ownership of it by calling AddRef() or Release() on it.
// That means YOU have to guarantee that your pRootSignature stays alive as long as the IPipelineSet is using it.
// Similarly, the pointers you pass as shader bytecodes will not be taken ownership, since it's not possible to do that with raw pointers.
// Actually, ownership of pRootSignature is not taken in order to be consistent in behaviour with the shader bytecodes.
// The IPipelineSet takes ownership of all ID3D12RootSignatures and ID3D12PipelineStates that it creates,
// therefore you don't have to free those yourself.
// You MAY "steal" a ID3D12RootSignature or ID3D12PipelineState that the IPipelineSet created,
// meaning that you can AddRef() what it gave you and destroy the IPipelineSet.
// This is just tricky to accomplish because:
// 1) The pointers it returns may be null because they haven't been built yet, or their compilation failed.
// 2) The object might get recreated through live-reloading, so you have to swap out your reference too.
// 3) You have to be aware that some root signatures are those you supplied and some are IPipelineSet created.
// So yeah... be careful out there.
//
// GPU LIFETIME
// ------------
// Since this API needs to reload RSs and PSOs, it also needs to delete stale RSs and PSOs.
// This is kinda hard since we can't easily know for sure if/when the GPU is done using them.
// As a simple solution, you specify a maximum latency with the IPipelineSet.
// PSOs will only be released by the IPipelineSet after that many calls to UpdatePipelines() have passed.
// You should set this number to the maximum number of simultaneous frames your D3D code allows.
// Or you can just be lazy and set it to some arbitrary high-ish number.
//
// SHARING ROOT SIGNATURES
// -----------------------
// D3D12 invalidates all your root parameters every time you switch root signature,
// but there's a trick to it: You're allowed to switch PSOs without switching root signature,
// as long as those PSOs were built using the same root signature.
// In other words, your code can do something like: foreach (rs) { set(rs, params); foreach (pso) { draw(); } }
// If the IPipelineSet built a new RootSignature for each PSO, this sharing wouldn't be possible.
// To share PSOs, you have two options:
// 1. Explicitly pass a user-supplied root signature to PSOs that need to share the same root signature.
// 2. Create your PSOs with the same RSFile name.
//    The IPipelineSet will keep only one copy of each RS that came from the same RSFile,
//    so you can be sure all those PSOs will be compatible with each other.
class IPipelineSet
{
public:
    // Create the PipelineSet which will use the passed-in device to create its pipelines and root signatures.
    static std::shared_ptr<IPipelineSet> Create(ID3D12Device* pDevice, int maximumFrameLatency);

    // Adds a pipeline to the set and returns ptrptrs that will eventually be filled with meaningful pointers.
    // They'll only get filled-in at the next call to UpdatePipelines() after the pipelines have been created (or reloaded).
    // Even though this returns mutable ptrptrs, you should NOT write to that pointer.
    // If you do, you're a bad person, and you deserve whatever happens!
    //
    // BTW: If you dislike std::pair, just use std::tie, as follows:
    //     ID3D12RootRootSignature** ppRS;
    //     ID3D12PipelineState** ppPSO;
    //     std::tie(ppRS, ppPSO) = pPipelineSet->AddPipeline(pipelineDesc, pipelineFiles);
    virtual std::pair<ID3D12RootSignature**, ID3D12PipelineState**> AddPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const GraphicsPipelineFiles& files) = 0;

    // Call this after having called AddPipeline() for all the pipelines in the set.
    // This will start building the root signature and PSOs necessary for the PipelineSet.
    // When the build is done, the returned event HANDLE will be set.
    // After calling BuildAllAsync(), you are no longer allowed to call AddPipeline to add more pipelines. It's closed.
    virtual HANDLE BuildAllAsync() = 0;

    // The IPipelineSet will automatically get file change notifications for the root signature/shader files it depends on,
    // and once it sees them change it'll rebuild the affected root signatures and pipeline states.
    // In order to prevent RS/PSOs to switch out from under your feet mid-frame,
    // the changes are only committed when you call UpdatePipelines().
    // That means you should call UpdatePipelines() perhaps once per frame, at the start of the frame.
    virtual void UpdatePipelines() = 0;
};
