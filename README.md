# PipelineSet
D3D12 Root Signature/Pipeline State Object live-reloading

Explanation here: [https://nlguillemot.wordpress.com/2017/01/31/d3d12-shader-live-reloading/](https://nlguillemot.wordpress.com/2017/01/31/d3d12-shader-live-reloading/)

# Changelog

2017-02-17: Make AddPipeline thread-safe (by wrapping its body in a mutex), and add NodeMask for linked adapter use.
