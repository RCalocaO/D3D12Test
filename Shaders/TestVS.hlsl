struct FVSOut
{
	float4 Pos : SV_POSITION;
	float2 UVs : TEXCOORD0;
	float4 Color : COLOR;
};

FVSOut Main(float3 Pos : POSITION, uint VertexID : SV_VertexID)
{
	//float3 P[3] =
	//{
	//	float3(0, 0.4444, 0),
	//	float3(0.25, -0.4444, 0),
	//	float3(-0.25, -0.4444, 0),
	//};

	float3 P[3] =
	{
		float3(-0.5, -0.1, 1),
		float3(0.1, -0.1, 1),
		float3(0, 0.5, 1),
		//float3(-0.464, -0.173, 1),
		//float3(0.0928, -0.173, 1),
		//float3(0, 0.866, 1),
	};
	
	FVSOut Out = (FVSOut)0;
	//Out.Pos = float4(P[VertexID % 3], 1);
	Out.Pos = float4(Pos, 1);

	#if 1
	float4x4 Proj =	
	{
		{0.928271, 0.0, 0.0, 0.0},
		{0.0, 1.73205090, 0.0, 0.0},
		{0.0, 0.0, 1.0001, 1},
		{0.0, 0.0, -0.10001, 0.0}
	};
	#else
	float4x4 Proj =	
	{
		{1, 0, 0 ,0},
		{0, 1, 0 ,0},
		{0, 0, 1, 0},
		{0.5, 0, 0, 1}
	};
	#endif
	Out.Pos = mul(Out.Pos, Proj);
	
	return Out;
}
