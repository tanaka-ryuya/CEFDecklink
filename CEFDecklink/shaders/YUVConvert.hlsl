Texture2D<float4> InputTextureFrame1 : register(t0); // Current Frame (or T)
Texture2D<float4> InputTextureFrame2 : register(t1); // Previous Frame (or T-1)

RWTexture2D<uint> OutputBuffer : register(u0); // ARGB Packed (32-bit per pixel)

// Constant Buffer for Parameters
cbuffer Parameters : register(b0)
{
    float alphaThreshold; // Threshold to avoid division by zero
    float viewMode;       // 0=Interlace, 1=Diff, 2=Prog F1, 3=Prog F2
    float isLicensed;     // 1.0 = valid, 0.0 = invalid/expired
    float filterMode;     // 0=None, 1=3-tap, 2=5-tap vertical LPF
};

// ============================================================
// Vertical Low-Pass Filter (Anti-Flicker for Interlace)
// Applies a vertical blur kernel to reduce interline twitter
// when progressive content is converted to interlaced output.
// ============================================================

// Filter for InputTextureFrame1
float4 SampleFiltered1(uint x, uint y, uint height)
{
    float4 center = InputTextureFrame1.Load(int3(x, y, 0));
    if (filterMode < 0.5f) return center;

    int yM1 = max((int)y - 1, 0);
    int yP1 = min((int)y + 1, (int)height - 1);
    float4 above1 = InputTextureFrame1.Load(int3(x, yM1, 0));
    float4 below1 = InputTextureFrame1.Load(int3(x, yP1, 0));

    if (filterMode < 1.5f)
    {
        // 3-tap kernel: [0.25, 0.50, 0.25]
        return above1 * 0.25f + center * 0.50f + below1 * 0.25f;
    }

    // 5-tap kernel: [0.0625, 0.25, 0.375, 0.25, 0.0625]
    int yM2 = max((int)y - 2, 0);
    int yP2 = min((int)y + 2, (int)height - 1);
    float4 above2 = InputTextureFrame1.Load(int3(x, yM2, 0));
    float4 below2 = InputTextureFrame1.Load(int3(x, yP2, 0));

    return above2 * 0.0625f + above1 * 0.25f + center * 0.375f + below1 * 0.25f + below2 * 0.0625f;
}

// Filter for InputTextureFrame2
float4 SampleFiltered2(uint x, uint y, uint height)
{
    float4 center = InputTextureFrame2.Load(int3(x, y, 0));
    if (filterMode < 0.5f) return center;

    int yM1 = max((int)y - 1, 0);
    int yP1 = min((int)y + 1, (int)height - 1);
    float4 above1 = InputTextureFrame2.Load(int3(x, yM1, 0));
    float4 below1 = InputTextureFrame2.Load(int3(x, yP1, 0));

    if (filterMode < 1.5f)
    {
        // 3-tap kernel: [0.25, 0.50, 0.25]
        return above1 * 0.25f + center * 0.50f + below1 * 0.25f;
    }

    // 5-tap kernel: [0.0625, 0.25, 0.375, 0.25, 0.0625]
    int yM2 = max((int)y - 2, 0);
    int yP2 = min((int)y + 2, (int)height - 1);
    float4 above2 = InputTextureFrame2.Load(int3(x, yM2, 0));
    float4 below2 = InputTextureFrame2.Load(int3(x, yP2, 0));

    return above2 * 0.0625f + above1 * 0.25f + center * 0.375f + below1 * 0.25f + below2 * 0.0625f;
}

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
        float4 p1 = SampleFiltered1(x, y, height);
        float4 p2 = SampleFiltered2(x, y, height);
        float4 diffColor = abs(p1 - p2);
        pixel = float4(diffColor.rgb, 1.0f); 
    } 
    else if (viewMode > 1.5f && viewMode < 2.5f) {
        // --- Mode 2: Progressive Frame 1 (Top Field Source) ---
        pixel = SampleFiltered1(x, y, height);
    }
    else if (viewMode > 2.5f && viewMode < 3.5f) {
        // --- Mode 3: Progressive Frame 2 (Bottom Field Source) ---
        pixel = SampleFiltered2(x, y, height);
    }
    else if (viewMode > 3.5f) {
        // --- Mode 4: 30p Blend Mode ---
        float4 p1 = SampleFiltered1(x, y, height);
        float4 p2 = SampleFiltered2(x, y, height);
        pixel = p1 * 0.5f + p2 * 0.5f;
    }
    else {
        // --- Mode 0: Interlaced Sampling (Normal) ---
        if ((y % 2) == 0) {
            pixel = SampleFiltered1(x, y, height);
        } else {
            pixel = SampleFiltered2(x, y, height);
        }
    }

    // --- Unpremultiply: RGB /= Alpha ---
    if (pixel.a > alphaThreshold) {
        pixel.rgb /= pixel.a;
    }

    // --- Watermark for Unlicensed/Expired (Moved to CEF) ---
    // (JavaScript overlay replaces the old shader red line)

    // --- Output ARGB for Dual-Port External Key ---
    uint A = (uint)(saturate(pixel.a) * 255.0f);
    uint R = (uint)(saturate(pixel.r) * 255.0f);
    uint G = (uint)(saturate(pixel.g) * 255.0f);
    uint B = (uint)(saturate(pixel.b) * 255.0f);
    
    uint packed = (B << 24) | (G << 16) | (R << 8) | A;
    
    OutputBuffer[uint2(x, y)] = packed;
}
