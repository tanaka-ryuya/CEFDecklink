Texture2D<float4> InputTextureFrame1 : register(t0); // Current Frame (or T)
Texture2D<float4> InputTextureFrame2 : register(t1); // Previous Frame (or T-1)

RWTexture2D<uint> OutputBuffer : register(u0); // ARGB Packed (32-bit per pixel)

// Constant Buffer for Parameters
cbuffer Parameters : register(b0)
{
    float alphaThreshold; // Threshold to avoid division by zero
    float viewMode;       // 0=Interlace, 1=Diff, 2=Prog F1, 3=Prog F2
    float2 padding;
};

[numthreads(16, 16, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint width, height;
    InputTextureFrame1.GetDimensions(width, height);

    uint x = DTid.x;
    uint y = DTid.y;

    if (x >= width || y >= height) return;

    float4 pixel;

    // --- Mode Selection ---
    if (viewMode > 0.5f && viewMode < 1.5f) {
        // --- Mode 1: Diff Mode ---
        // Sample both frames at the same location
        float4 p1 = InputTextureFrame1.Load(int3(x, y, 0));
        float4 p2 = InputTextureFrame2.Load(int3(x, y, 0));
        
        // Calculate absolute difference
        float4 diff = abs(p1 - p2);
        
        // boost alpha to 1.0 so it's visible even if diff is small (or just show RGB diff)
        pixel = float4(diff.rgb, 1.0f); 
    } 
    else if (viewMode > 1.5f && viewMode < 2.5f) {
        // --- Mode 2: Progressive Frame 1 (Top Field Source) ---
        pixel = InputTextureFrame1.Load(int3(x, y, 0));
    }
    else if (viewMode > 2.5f) {
        // --- Mode 3: Progressive Frame 2 (Bottom Field Source) ---
        pixel = InputTextureFrame2.Load(int3(x, y, 0));
    }
    else {
        // --- Mode 0: Interlaced Sampling (Normal) ---
        // [Field Order: Top Field First (TFF)]
        // Even Lines (0, 2, 4...) -> Top Field    -> Frame T   (InputTextureFrame1)
        // Odd Lines  (1, 3, 5...) -> Bottom Field -> Frame T+1 (InputTextureFrame2)
        
        if ((y % 2) == 0) {
            // Top Field -> Current Frame (T)
            pixel = InputTextureFrame1.Load(int3(x, y, 0));
        } else {
            // Bottom Field -> Next Frame (T+1)
            pixel = InputTextureFrame2.Load(int3(x, y, 0));
        }
        
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

