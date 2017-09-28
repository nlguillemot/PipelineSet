#include "test.rs.hlsl"

RWByteAddressBuffer OutputBuffer : register(u0);

[RootSignature(TEST_RS)]
[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    // magic number to output to the command line
    // with live reloading, you can switch this number without shutting down your program.
    OutputBuffer.Store(0, 1337);
}
