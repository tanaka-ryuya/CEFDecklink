Texture2D<float4> InputTextureFrame1 : register(t0); // Current Frame (or T)
Texture2D<float4> InputTextureFrame2 : register(t1); // Previous Frame (or T-1)

RWTexture2D<uint> OutputBuffer : register(u0); // ARGB Packed (32-bit per pixel)

// Constant Buffer for Parameters
cbuffer Parameters : register(b0)
{
    float alphaThreshold; // Threshold to avoid division by zero
    float3 padding;
};

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height;
    InputTextureFrame1.GetDimensions(width, height);

    uint x = DTid.x;
    uint y = DTid.y;

    if (x >= width || y >= height) return;

    // --- Interlaced Sampling ---
    float4 pixel;
    
    if ((y % 2) == 0) {
        // Top Field -> Frame 1
        pixel = InputTextureFrame1.Load(int3(x, y, 0));
    } else {
        // Bottom Field -> Frame 2
        pixel = InputTextureFrame2.Load(int3(x, y, 0));
    }

    // --- Unpremultiply: RGB /= Alpha ---
    if (pixel.a > alphaThreshold) {
        pixel.rgb /= pixel.a;
    }

    // --- Output ARGB for Dual-Port External Key ---
    // UltraStudio HD Mini will separate:
    //   SDI Out: Fill (RGB channels)
    //   SDI Out 2: Key (Alpha channel)
    
    // Clamp values
    uint A = (uint)(saturate(pixel.a) * 255.0f);
    uint R = (uint)(saturate(pixel.r) * 255.0f);
    uint G = (uint)(saturate(pixel.g) * 255.0f);
    uint B = (uint)(saturate(pixel.b) * 255.0f);
    
    // Pack as Big Endian ARGB: memory bytes [A][R][G][B]
    // Little Endian uint32: B in highest byte, A in lowest byte
    uint packed = (B << 24) | (G << 16) | (R << 8) | A;
    
    OutputBuffer[uint2(x, y)] = packed;
}

