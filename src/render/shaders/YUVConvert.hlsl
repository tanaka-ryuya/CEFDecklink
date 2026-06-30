Texture2D<float4> InputTextureFrame1 : register(t0); // Current Frame (or T)
Texture2D<float4> InputTextureFrame2 : register(t1); // Previous Frame (or T-1)

RWTexture2D<uint> OutputBuffer : register(u0); // ARGB Packed (32-bit per pixel)

// Constant Buffer for Parameters
cbuffer Parameters : register(b0)
{
    float alphaThreshold; // Threshold to avoid division by zero
    float viewMode;       // 0=Interlace, 1=Diff, 2=Prog F1, 3=Prog F2
    float isLicensed;     // 1.0 = valid, 0.0 = invalid/expired
    float padding;
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
        float4 p1 = InputTextureFrame1.Load(int3(x, y, 0));
        float4 p2 = InputTextureFrame2.Load(int3(x, y, 0));
        float4 diff = abs(p1 - p2);
        pixel = float4(diff.rgb, 1.0f); 
    } 
    else if (viewMode > 1.5f && viewMode < 2.5f) {
        // --- Mode 2: Progressive Frame 1 (Top Field Source) ---
        pixel = InputTextureFrame1.Load(int3(x, y, 0));
    }
    else if (viewMode > 2.5f && viewMode < 3.5f) {
        // --- Mode 3: Progressive Frame 2 (Bottom Field Source) ---
        pixel = InputTextureFrame2.Load(int3(x, y, 0));
    }
    else if (viewMode > 3.5f) {
        // --- Mode 4: 30p Blend Mode ---
        float4 p1 = InputTextureFrame1.Load(int3(x, y, 0));
        float4 p2 = InputTextureFrame2.Load(int3(x, y, 0));
        pixel = p1 * 0.5f + p2 * 0.5f;
    }
    else {
        // --- Mode 0: Interlaced Sampling (Normal) ---
        if ((y % 2) == 0) {
            pixel = InputTextureFrame1.Load(int3(x, y, 0));
        } else {
            pixel = InputTextureFrame2.Load(int3(x, y, 0));
        }
    }

    // --- Unpremultiply: RGB /= Alpha ---
    if (pixel.a > alphaThreshold) {
        pixel.rgb /= pixel.a;
    }

    // --- Watermark for Unlicensed/Expired ---
    if (isLicensed < 0.5f) {
        // Draw a single diagonal red line across the screen
        int diff = (int)x * (int)height - (int)y * (int)width;
        if (abs(diff) < (int)width * 5) {
            // Blend red color
            pixel.rgb = pixel.rgb * 0.5f + float3(0.5f, 0.0f, 0.0f);
            pixel.a = max(pixel.a, 0.5f); // Ensure it's opaque enough to be seen over background
        }
    }

    // --- Output ARGB for Dual-Port External Key ---
    uint A = (uint)(saturate(pixel.a) * 255.0f);
    uint R = (uint)(saturate(pixel.r) * 255.0f);
    uint G = (uint)(saturate(pixel.g) * 255.0f);
    uint B = (uint)(saturate(pixel.b) * 255.0f);
    
    uint packed = (B << 24) | (G << 16) | (R << 8) | A;
    
    OutputBuffer[uint2(x, y)] = packed;
}

