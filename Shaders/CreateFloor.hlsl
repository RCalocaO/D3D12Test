#if 0
#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout (local_size_x = 1, local_size_y = 1) in;

//layout (binding = 0, r32f) uniform writeonly image1D RWImage;
layout (set = 0, binding = 0) buffer IB
{
	int OutIndices[];
};

struct FPosColorUVVertex
{
	float x, y, z;
	uint Color;
	float u, v;
};

layout (set = 0, binding = 1) buffer VB
{
	FPosColorUVVertex OutVertices[];
};

layout (set = 0, binding = 2) uniform UB
{
	float Y;
	float Extent;
	int NumQuadsX;
	int NumQuadsZ;
	float Elevation;
};

layout(set = 0, binding = 3) uniform sampler2D Heightmap;

void main()
{
	int QuadIndexX = int(gl_GlobalInvocationID.x);
	int QuadIndexZ = int(gl_GlobalInvocationID.z);
	int QuadIndex = QuadIndexX + QuadIndexZ * NumQuadsX;

	if (QuadIndexX < NumQuadsX && QuadIndexZ < NumQuadsZ)
	{
		float Width = 2.0 * Extent;
		float WidthPerQuadX = Width / float(NumQuadsX);
		float WidthPerQuadZ = Width / float(NumQuadsZ);
		float X = -Extent + WidthPerQuadX * float(QuadIndexX);
		float Z = -Extent + WidthPerQuadZ * float(QuadIndexZ);

		float UWidth = 1.0 / float(NumQuadsX);
		float U = UWidth * float(QuadIndexX);
		float VWidth = 1.0 / float(NumQuadsZ);
		float V = VWidth * float(QuadIndexZ);

		float Height = texture(Heightmap, vec2(U, V)).x;
		float Y = Y + Elevation * Height;

		OutVertices[QuadIndex].x = X;
		OutVertices[QuadIndex].y = Y;
		OutVertices[QuadIndex].z = Z;
		OutVertices[QuadIndex].Color = QuadIndexX * 65536 + QuadIndexZ;
		OutVertices[QuadIndex].u = U;
		OutVertices[QuadIndex].v = V;
		if (QuadIndexX != 0 && QuadIndexZ != 0)
		{
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 0] = NumQuadsX * QuadIndexZ + QuadIndexX;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 1] = NumQuadsX * QuadIndexZ + QuadIndexX - 1;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 2] = NumQuadsX * (QuadIndexZ - 1 ) + QuadIndexX - 1;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 3] = NumQuadsX * (QuadIndexZ - 1 ) + QuadIndexX - 1;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 4] = NumQuadsX * (QuadIndexZ - 1) + QuadIndexX;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 5] = NumQuadsX * QuadIndexZ + QuadIndexX;
		}
	}
}
#endif

struct FPosColorUVVertex
{
	float x, y, z;
	uint Color;
	float u, v;
};

RWBuffer<int> OutIndices : register(u0);
RWStructuredBuffer<FPosColorUVVertex> OutVertices : register(u1);

cbuffer UB
{
	float Y;
	float Extent;
	int NumQuadsX;
	int NumQuadsZ;
	float Elevation;
};

Texture2D Heightmap : register(t0);
SamplerState SS : register(s0);

[numthreads(1,1,1)]
void Main(uint3 gl_GlobalInvocationID : SV_DispatchThreadID)
{
	int QuadIndexX = int(gl_GlobalInvocationID.x);
	int QuadIndexZ = int(gl_GlobalInvocationID.z);
	int QuadIndex = QuadIndexX + QuadIndexZ * NumQuadsX;

	if (QuadIndexX < NumQuadsX && QuadIndexZ < NumQuadsZ)
	{
		float Width = 2.0 * Extent;
		float WidthPerQuadX = Width / float(NumQuadsX);
		float WidthPerQuadZ = Width / float(NumQuadsZ);
		float X = -Extent + WidthPerQuadX * float(QuadIndexX);
		float Z = -Extent + WidthPerQuadZ * float(QuadIndexZ);

		float UWidth = 1.0 / float(NumQuadsX);
		float U = UWidth * float(QuadIndexX);
		float VWidth = 1.0 / float(NumQuadsZ);
		float V = VWidth * float(QuadIndexZ);

		float Height = Heightmap.SampleLevel(SS, float2(U, V), 0).x;
		float CurrentY = Y + Elevation * Height;

		OutVertices[QuadIndex].x = X;
		OutVertices[QuadIndex].y = CurrentY;
		OutVertices[QuadIndex].z = Z;
		OutVertices[QuadIndex].Color = QuadIndexX * 65536 + QuadIndexZ;
		OutVertices[QuadIndex].u = U;
		OutVertices[QuadIndex].v = V;
		if (QuadIndexX != 0 && QuadIndexZ != 0)
		{
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 0] = NumQuadsX * QuadIndexZ + QuadIndexX;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 1] = NumQuadsX * QuadIndexZ + QuadIndexX - 1;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 2] = NumQuadsX * (QuadIndexZ - 1 ) + QuadIndexX - 1;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 3] = NumQuadsX * (QuadIndexZ - 1 ) + QuadIndexX - 1;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 4] = NumQuadsX * (QuadIndexZ - 1) + QuadIndexX;
			OutIndices[(QuadIndexX - 1 + NumQuadsX * (QuadIndexZ - 1)) * 6 + 5] = NumQuadsX * QuadIndexZ + QuadIndexX;
		}
	}
}
