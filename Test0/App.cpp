// App.cpp

#include "stdafx.h"
#include "App.h"
#include "D3D12Mem.h"
#include "D3D12Device.h"
#include "D3D12Resources.h"
#include "ObjLoader.h"

#if ENABLE_VULKAN

// 0 no multithreading
// 1 inside a render pass
// 2 run post
#define TRY_MULTITHREADED	0
#endif

FControl::FControl()
	: StepDirection{0, 0, 0}
	, CameraPos{0, 0, -10, 1}
	, ViewMode(EViewMode::Solid)
	, DoPost(true)
	, DoMSAA(false)
{
}

FControl GRequestControl;
FControl GControl;

FVector4 GCameraPos = {0, 0, 10, 1};

static FInstance GInstance;
static FDevice GDevice;
static FSwapchain GSwapchain;
static FCmdBufferMgr GCmdBufferMgr;
static FMemManager GMemMgr;
static FDescriptorPool GDescriptorPool;
static FStagingManager GStagingManager;

static FVertexBuffer GObjVB;
static Obj::FObj GObj;
static FRWVertexBuffer GFloorVB;
static FRWIndexBuffer GFloorIB;
struct FCreateFloorUB
{
	float Y;
	float Extent;
	uint32 NumQuadsX;
	uint32 NumQuadsZ;
	float Elevation;
};
static FUniformBuffer<FCreateFloorUB> GCreateFloorUB;
struct FViewUB
{
	FMatrix4x4 View;
	FMatrix4x4 Proj;
};
static FUniformBuffer<FViewUB> GViewUB;
struct FObjUB
{
	FMatrix4x4 Obj;
};
static FUniformBuffer<FObjUB> GObjUB;
static FUniformBuffer<FObjUB> GIdentityUB;

static FImage2DWithView GCheckerboardTexture;
static FImage2DWithView GHeightMap;
static FSampler GSampler;

struct FRenderTargetPool
{
	struct FEntry
	{
		bool bFree = true;
		FImage2DWithView Texture;
		//const char* Name = nullptr;
	};

	void Create()
	{
		InitializeCriticalSection(&CS);
	}

	void Destroy()
	{
		for (auto* Entry : Entries)
		{
			check(Entry->bFree);
			Entry->Texture.Destroy();
			delete Entry;
		}
	}

	void EmptyPool()
	{
		::EnterCriticalSection(&CS);
		for (auto* Entry : Entries)
		{
			if (!Entry->bFree)
			{
				Release(Entry);
			}
		}
	}

	FEntry* Acquire(FDevice* Device, const wchar_t* InName, uint32 Width, uint32 Height, DXGI_FORMAT Format, FMemManager& MemMgr)//, uint32 NumMips, VkSampleCountFlagBits Samples)
	{
		::EnterCriticalSection(&CS);
		for (auto* Entry : Entries)
		{
			if (Entry->bFree)
			{
				FImage& Image = Entry->Texture.Image;
				if (Image.Width == Width && 
					Image.Height == Height &&
					Image.Format == Format
#if ENABLE_VULKAN
					&&
					Image.NumMips == NumMips &&
					Image.Samples == Samples &&
#endif
					)
				{
					Entry->bFree = false;
					//Entry->Layout = VK_IMAGE_LAYOUT_UNDEFINED;
					Image.Alloc->Resource->SetName(InName);
					//Entry->Name = InName;
					return Entry;
				}
			}
		}

		FEntry* Entry = new FEntry;
		Entries.push_back(Entry);
		::LeaveCriticalSection(&CS);

		Entry->bFree = false;
#if ENABLE_VULKAN
		Entry->Usage = Usage;
		Entry->MemProperties = MemProperties;
#endif
		//Entry->Name = InName;

		Entry->Texture.Create(*Device, Width, Height, Format, GDescriptorPool, MemMgr,
			(IsDepthOrStencilFormat(Format) ? (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) : (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
#if ENABLE_VULKAN
			NumMips, Samples
#endif
		);
		Entry->Texture.Image.Alloc->Resource->SetName(InName);

		return Entry;
	}

	void Release(FEntry*& Entry)
	{
		::EnterCriticalSection(&CS);
		check(!Entry->bFree);
		Entry->bFree = true;
		::LeaveCriticalSection(&CS);
		Entry = nullptr;
	}

	std::vector<FEntry*> Entries;

	CRITICAL_SECTION CS;
};
static FRenderTargetPool GRenderTargetPool;

#if ENABLE_VULKAN
#if TRY_MULTITHREADED > 0
struct FThread
{
	volatile FCmdBuffer* ParentCmdBuffer = nullptr;
	volatile HANDLE StartEvent = INVALID_HANDLE_VALUE;
	volatile HANDLE DoneEvent = INVALID_HANDLE_VALUE;
	volatile bool bDoQuit = false;
	volatile int32 Width = 0;
	volatile int32 Height = 0;
	volatile FRenderPass* RenderPass = nullptr;
	volatile FFramebuffer* Framebuffer = nullptr;

	void Create()
	{
		ThreadHandle = ::CreateThread(nullptr, 0, ThreadFunction, this, 0, &ThreadId);
		StartEvent = ::CreateEventA(nullptr, true, false, "StartEvent");
		DoneEvent = ::CreateEventA(nullptr, true, false, "DoneEvent");
	}

	DWORD ThreadId = 0;
	HANDLE ThreadHandle = INVALID_HANDLE_VALUE;

	static DWORD __stdcall ThreadFunction(void*);
};
FThread GThread;

#endif
#endif



struct FPosColorUVVertex
{
	float x, y, z;
	uint32 Color;
	float u, v;
};
FVertexFormat GPosColorUVFormat;

bool GQuitting = false;

struct FTestPSO : public FGfxPSO
{
	virtual void SetupLayoutBindings(std::vector<D3D12_ROOT_PARAMETER>& OutRootParameters, std::vector<D3D12_DESCRIPTOR_RANGE>& OutRanges) override
	{
		uint32 SRVRange = AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
		uint32 SamplerRange = AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);

		AddRootCBVParam(OutRootParameters, 0, D3D12_SHADER_VISIBILITY_VERTEX);
		AddRootCBVParam(OutRootParameters, 1, D3D12_SHADER_VISIBILITY_VERTEX);
		AddRootTableParam(OutRootParameters, OutRanges, SRVRange, 1, D3D12_SHADER_VISIBILITY_PIXEL);
		AddRootTableParam(OutRootParameters, OutRanges, SamplerRange, 1, D3D12_SHADER_VISIBILITY_PIXEL);
	}
};
FTestPSO GTestPSO;

struct FOneRWImagePSO : public FComputePSO
{
	virtual void SetupLayoutBindings(std::vector<D3D12_ROOT_PARAMETER>& OutRootParameters, std::vector<D3D12_DESCRIPTOR_RANGE>& OutRanges) override
	{
		uint32 UAVRange = AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);

		AddRootTableParam(OutRootParameters, OutRanges, UAVRange, 1, D3D12_SHADER_VISIBILITY_ALL);
	}
};
FOneRWImagePSO GFillTexturePSO;

#if ENABLE_VULKAN
struct FTwoImagesPSO : public FComputePSO
{
	virtual void SetupLayoutBindings(std::vector<VkDescriptorSetLayoutBinding>& OutBindings) override
	{
		AddBinding(OutBindings, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		AddBinding(OutBindings, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	}
};
FTwoImagesPSO GTestComputePSO;
#endif
struct FTestPostComputePSO : public FComputePSO
{
	virtual void SetupLayoutBindings(std::vector<D3D12_ROOT_PARAMETER>& OutRootParameters, std::vector<D3D12_DESCRIPTOR_RANGE>& OutRanges) override
	{
		uint32 StartUAV = AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
		AddRange(OutRanges, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);

		AddRootTableParam(OutRootParameters, OutRanges, StartUAV, 2, D3D12_SHADER_VISIBILITY_ALL);
	}
};
FTestPostComputePSO GTestComputePostPSO;

struct FSetupFloorPSO : public FComputePSO
{
	virtual void SetupLayoutBindings(std::vector<D3D12_ROOT_PARAMETER>& OutRootParameters, std::vector<D3D12_DESCRIPTOR_RANGE>& OutRanges) override
	{
		uint32 StartUAV = AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
		AddRange(OutRanges, 1, D3D12_DESCRIPTOR_RANGE_TYPE_UAV);
		AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
		uint32 StartSampler = AddRange(OutRanges, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);

		AddRootCBVParam(OutRootParameters, 0, D3D12_SHADER_VISIBILITY_ALL);
		AddRootTableParam(OutRootParameters, OutRanges, StartUAV, 3, D3D12_SHADER_VISIBILITY_ALL);
		AddRootTableParam(OutRootParameters, OutRanges, StartSampler, 1, D3D12_SHADER_VISIBILITY_ALL);
	}
};
static FSetupFloorPSO GSetupFloorPSO;

void FInstance::CreateDevice(FDevice& OutDevice)
{
	std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> Adapters;
	uint32 NumAdapters = 0;
	{
		Microsoft::WRL::ComPtr<IDXGIAdapter1> CurrentAdapter;
		while (DXGIFactory->EnumAdapters1(NumAdapters, &CurrentAdapter) != DXGI_ERROR_NOT_FOUND)
		{
			Adapters.push_back(CurrentAdapter);
			++NumAdapters;
		}
	}

	check(NumAdapters > 0);
	for (uint32 Index = 0; Index < NumAdapters; ++Index)
	{
		DXGI_ADAPTER_DESC1 Desc;
		checkD3D12(Adapters[Index]->GetDesc1(&Desc));
		if (Desc.DedicatedVideoMemory > 0)
		{
			OutDevice.Adapter = Adapters[Index];
			goto Found;
		}
	}

	// Not found!
	check(0);
	return;

Found:
	OutDevice.Create();
}

struct FObjectCache
{
	FDevice* Device = nullptr;

#if ENABLE_VULKAN
	std::map<uint64, FRenderPass*> RenderPasses;
#endif
	std::map<FComputePSO*, FComputePipeline*> ComputePipelines;
	std::map<FGfxPSOLayout, FGfxPipeline*> GfxPipelines;

#if ENABLE_VULKAN
	struct FFrameBufferEntry
	{
		FFramebuffer* Framebuffer;

		FFrameBufferEntry(VkRenderPass InRenderPass, uint32 InWidth, uint32 InHeight, uint32 InNumColorTargets, VkImageView* InColorViews, VkImageView InDepthStencilView, VkImageView InResolveColor)
			: RenderPass(InRenderPass)
			, Width(InWidth)
			, Height(InHeight)
			, NumColorTargets(InNumColorTargets)
			, DepthStencilView(InDepthStencilView)
			, ResolveColor(InResolveColor)
		{
			for (uint32 Index = 0; Index < InNumColorTargets; ++Index)
			{
				ColorViews[Index] = InColorViews[Index];
			}
		}

		VkRenderPass RenderPass = VK_NULL_HANDLE;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 NumColorTargets = 0;
		VkImageView ColorViews[FRenderPassLayout::MAX_COLOR_ATTACHMENTS];
		VkImageView DepthStencilView = VK_NULL_HANDLE;
		VkImageView ResolveColor;
	};
	std::vector<FFrameBufferEntry> Framebuffers;
#endif
	void Create(FDevice* InDevice)
	{
		Device = InDevice;
	}
#if ENABLE_VULKAN
	FFramebuffer* GetOrCreateFramebuffer(VkRenderPass RenderPass, VkImageView Color, VkImageView DepthStencil, uint32 Width, uint32 Height, VkImageView ResolveColor = VK_NULL_HANDLE)
	{
		for (auto& Entry : Framebuffers)
		{
			if (Entry.RenderPass == RenderPass && Entry.NumColorTargets == 1 && Entry.ColorViews[0] == Color && Entry.DepthStencilView == DepthStencil && Entry.Width == Width && Entry.Height == Height && Entry.ResolveColor == ResolveColor)
			{
				return Entry.Framebuffer;
			}
		}

		FFrameBufferEntry Entry(RenderPass, Width, Height, 1, &Color, DepthStencil, ResolveColor);

		auto* NewFramebuffer = new FFramebuffer;
		NewFramebuffer->Create(Device->Device, RenderPass, Color, DepthStencil, Width, Height, ResolveColor);
		Entry.Framebuffer = NewFramebuffer;
		Framebuffers.push_back(Entry);
		return NewFramebuffer;
	}
#endif

	FGfxPipeline* GetOrCreateGfxPipeline(FGfxPSO* GfxPSO, FVertexFormat* VF, /*uint32 Width, uint32 Height, FRenderPass* RenderPass, */bool bWireframe = false)
	{
		FGfxPSOLayout Layout(GfxPSO, VF, /*Width, Height, RenderPass->RenderPass, */bWireframe);
		auto Found = GfxPipelines.find(Layout);
		if (Found != GfxPipelines.end())
		{
			return Found->second;
		}

		auto* NewPipeline = new FGfxPipeline;
		NewPipeline->Desc.RasterizerState.FillMode = bWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
		NewPipeline->Create(Device, GfxPSO, VF/*, Width, Height, RenderPass*/);
		GfxPipelines[Layout] = NewPipeline;
		return NewPipeline;
	}

	FComputePipeline* GetOrCreateComputePipeline(FComputePSO* ComputePSO)
	{
		auto Found = ComputePipelines.find(ComputePSO);
		if (Found != ComputePipelines.end())
		{
			return Found->second;
		}

		auto* NewPipeline = new FComputePipeline;
		NewPipeline->Create(Device, ComputePSO);
		ComputePipelines[ComputePSO] = NewPipeline;
		return NewPipeline;
	}

#if ENABLE_VULKAN
	FRenderPass* GetOrCreateRenderPass(uint32 Width, uint32 Height, uint32 NumColorTargets, VkFormat* ColorFormats, VkFormat DepthStencilFormat = VK_FORMAT_UNDEFINED, VkSampleCountFlagBits InNumSamples = VK_SAMPLE_COUNT_1_BIT, FImage2DWithView* ResolveColorBuffer = nullptr)
	{
		FRenderPassLayout Layout(Width, Height, NumColorTargets, ColorFormats, DepthStencilFormat, InNumSamples, ResolveColorBuffer ? ResolveColorBuffer->GetFormat() : VK_FORMAT_UNDEFINED);
		auto LayoutHash = Layout.GetHash();
		auto Found = RenderPasses.find(LayoutHash);
		if (Found != RenderPasses.end())
		{
			return Found->second;
		}

		auto* NewRenderPass = new FRenderPass;
		NewRenderPass->Create(Device->Device, Layout);
		RenderPasses[LayoutHash] = NewRenderPass;
		return NewRenderPass;
	}
#endif
	void Destroy()
	{
#if ENABLE_VULKAN
		for (auto& Pair : RenderPasses)
		{
			Pair.second->Destroy();
			delete Pair.second;
		}
		RenderPasses.swap(decltype(RenderPasses)());
#endif
		for (auto& Pair : ComputePipelines)
		{
			//Pair.second->Destroy(Device->Device);
			delete Pair.second;
		}
		ComputePipelines.swap(decltype(ComputePipelines)());

		for (auto& Pair : GfxPipelines)
		{
			//Pair.second->Destroy(Device->Device);
			delete Pair.second;
		}
		GfxPipelines.swap(decltype(GfxPipelines)());

#if ENABLE_VULKAN
		for (auto& Entry : Framebuffers)
		{
			Entry.Framebuffer->Destroy();
			delete Entry.Framebuffer;
		}
		Framebuffers.swap(decltype(Framebuffers)());
#endif
	}
};
FObjectCache GObjectCache;


template <typename TFillLambda>
void MapAndFillBufferSyncOneShotCmdBuffer(FDevice& Device, FBuffer* DestBuffer, TFillLambda Fill, uint32 Size)
{
	// HACK!
	void* Data = DestBuffer->GetMappedData();
#if ENABLE_VULKAN
	auto* CmdBuffer = GCmdBufferMgr.AllocateCmdBuffer(Device);
	CmdBuffer->Begin();
	FStagingBuffer* StagingBuffer = GStagingManager.RequestUploadBuffer(Size);
#endif
	Fill(Data);
#if ENABLE_VULKAN
	MapAndFillBufferSync(StagingBuffer, CmdBuffer, DestBuffer, Fill, Size);
	FlushMappedBuffer(GDevice.Device, StagingBuffer);
	CmdBuffer->End();
	GCmdBufferMgr.Submit(CmdBuffer, GDevice.PresentQueue, nullptr, nullptr);
	CmdBuffer->WaitForFence();
#endif
}

static bool LoadShadersAndGeometry()
{
	check(GTestPSO.CreateVSPS(GDevice, "../Shaders/TestVS.hlsl", "../Shaders/TestPS.hlsl"));
	check(GSetupFloorPSO.Create(GDevice, "../Shaders/CreateFloor.hlsl"));
	check(GFillTexturePSO.Create(GDevice, "../Shaders/FillTexture.hlsl"));
	check(GTestComputePostPSO.Create(GDevice, "../Shaders/TestPost.hlsl"));
#if ENABLE_VULKAN
	check(GTestComputePSO.Create(GDevice.Device, "../Shaders/Test0.comp.spv"));
#endif

	// Setup Vertex Format
	GPosColorUVFormat.AddVertexBuffer(0, sizeof(FPosColorUVVertex), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA);
	GPosColorUVFormat.AddVertexAttribute("POSITION", 0, 0, DXGI_FORMAT_R32G32B32_FLOAT, offsetof(FPosColorUVVertex, x));
	GPosColorUVFormat.AddVertexAttribute("COLOR", 0, 1, DXGI_FORMAT_R8G8B8A8_UNORM, offsetof(FPosColorUVVertex, Color));
	GPosColorUVFormat.AddVertexAttribute("TEXCOORD", 0, 2, DXGI_FORMAT_R32G32_FLOAT, offsetof(FPosColorUVVertex, u));

	// Load and fill geometry
	if (!Obj::Load("../Meshes/Cube/cube.obj", GObj))
	{
		return false;
	}

	GObjVB.Create(L"ObjVB", GDevice, sizeof(FPosColorUVVertex), sizeof(FPosColorUVVertex) * (uint32)GObj.Faces.size() * 3, GMemMgr, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true);
	//GObj.Faces.resize(1);

	auto FillObj = [](void* Data)
	{
		check(Data);
		auto* Vertex = (FPosColorUVVertex*)Data;
		for (uint32 Index = 0; Index < GObj.Faces.size(); ++Index)
		{
			auto& Face = GObj.Faces[Index];
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				Vertex->x = GObj.Vs[Face.Corners[Corner].Pos].x;
				Vertex->y = GObj.Vs[Face.Corners[Corner].Pos].y;
				Vertex->z = GObj.Vs[Face.Corners[Corner].Pos].z;
				Vertex->Color = PackNormalToU32(GObj.VNs[Face.Corners[Corner].Normal]);
				Vertex->u = GObj.VTs[Face.Corners[Corner].UV].u;
				Vertex->v = GObj.VTs[Face.Corners[Corner].UV].v;
				++Vertex;
			}
		}
	};
	MapAndFillBufferSyncOneShotCmdBuffer(GDevice, &GObjVB.Buffer, FillObj, sizeof(FPosColorUVVertex) * (uint32)GObj.Faces.size() * 3);
	return true;
}

void CreateAndFillTexture()
{
	srand(0);
	GCheckerboardTexture.Create(GDevice, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, GDescriptorPool, GMemMgr, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	GHeightMap.Create(GDevice, 64, 64, DXGI_FORMAT_R32_FLOAT, GDescriptorPool, GMemMgr);

	auto* CmdBuffer = GCmdBufferMgr.AllocateCmdBuffer(GDevice);
	CmdBuffer->Begin();

	{
		FComputePipeline* Pipeline = GObjectCache.GetOrCreateComputePipeline(&GFillTexturePSO);

		// Starts as a UAV so no transition needed
		ResourceBarrier(CmdBuffer, &GCheckerboardTexture.Image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		CmdBuffer->CommandList->SetPipelineState(Pipeline->PipelineState.Get());
		CmdBuffer->CommandList->SetComputeRootSignature(GFillTexturePSO.RootSignature.Get());
		ID3D12DescriptorHeap* ppHeaps[] ={GDescriptorPool.CSUHeap.Get()};
		CmdBuffer->CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
		FDescriptorHandle UAVHandle = GDescriptorPool.AllocateCSU();
		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = MakeTexture2DUAVDesc(GCheckerboardTexture.GetFormat());
		GDevice.Device->CreateUnorderedAccessView(GCheckerboardTexture.Image.Alloc->Resource.Get(), nullptr, &UAVDesc, UAVHandle.CPU);
		CmdBuffer->CommandList->SetComputeRootDescriptorTable(0, UAVHandle.GPU);

		CmdBuffer->CommandList->Dispatch(GCheckerboardTexture.GetWidth() / 8, GCheckerboardTexture.GetHeight() / 8, 1);
		ResourceBarrier(CmdBuffer, &GCheckerboardTexture.Image, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}

	{
		ResourceBarrier(CmdBuffer, &GHeightMap.Image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		auto FillHeightMap = [](void* Data, uint32 Width, uint32 Height)
		{
			float* Out = (float*)Data;
			while (Height--)
			{
				uint32 InnerWidth = Width;
				while (InnerWidth--)
				{
					*Out++ = (float)rand() / (float)RAND_MAX;
				}
			}
		};
		auto* StagingBuffer = GStagingManager.RequestUploadBufferForImage(GDevice, &GHeightMap.Image, GMemMgr);
		MapAndFillImageSync(StagingBuffer, CmdBuffer, &GHeightMap.Image, FillHeightMap);
		ResourceBarrier(CmdBuffer, &GHeightMap.Image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	}
	CmdBuffer->End();
	GCmdBufferMgr.Submit(GDevice, CmdBuffer);
	CmdBuffer->WaitForFence();
}


static void FillFloor(FCmdBuffer* CmdBuffer)
{
	ResourceBarrier(CmdBuffer, GFloorVB.VB.Buffer.Alloc->Resource.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ResourceBarrier(CmdBuffer, GFloorIB.IB.Buffer.Alloc->Resource.Get(), D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	auto* ComputePipeline = GObjectCache.GetOrCreateComputePipeline(&GSetupFloorPSO);
	CmdBuffer->CommandList->SetPipelineState(ComputePipeline->PipelineState.Get());
	FCreateFloorUB& CreateFloorUB = *GCreateFloorUB.GetMappedData();
	CmdBuffer->CommandList->SetComputeRootSignature(GSetupFloorPSO.RootSignature.Get());

	ID3D12DescriptorHeap* ppHeaps[] = {GDescriptorPool.CSUHeap.Get(), GDescriptorPool.SamplerHeap.Get()};
	CmdBuffer->CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	CmdBuffer->CommandList->SetComputeRootConstantBufferView(0, GCreateFloorUB.View.BufferLocation);
	FDescriptorHandle IBHandle = GDescriptorPool.AllocateCSU();
	FDescriptorHandle VBHandle = GDescriptorPool.AllocateCSU();
	FDescriptorHandle HeightmapHandle = GDescriptorPool.AllocateCSU();
	GDevice.Device->CreateUnorderedAccessView(GFloorIB.IB.Buffer.Alloc->Resource.Get(), nullptr, &GFloorIB.View, IBHandle.CPU);
	GDevice.Device->CreateUnorderedAccessView(GFloorVB.VB.Buffer.Alloc->Resource.Get(), nullptr, &GFloorVB.View, VBHandle.CPU);
	GDevice.Device->CreateShaderResourceView(GHeightMap.Image.Alloc->Resource.Get(), &GHeightMap.SRVView, HeightmapHandle.CPU);
	CmdBuffer->CommandList->SetComputeRootDescriptorTable(1, IBHandle.GPU);	// Setting IB and then VB as they are contiguous
	CmdBuffer->CommandList->SetComputeRootDescriptorTable(2, GSampler.Handle.GPU);

	CmdBuffer->CommandList->Dispatch(CreateFloorUB.NumQuadsX, 1, CreateFloorUB.NumQuadsZ);

	{
		D3D12_RESOURCE_BARRIER Barriers[2];
		MemZero(Barriers);
		Barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		Barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		Barriers[0].UAV.pResource = GFloorVB.VB.Buffer.Alloc->Resource.Get();
		Barriers[1] = Barriers[0];
		Barriers[1].UAV.pResource = GFloorIB.IB.Buffer.Alloc->Resource.Get();
		CmdBuffer->CommandList->ResourceBarrier(2, Barriers);
	}

	ResourceBarrier(CmdBuffer, GFloorVB.VB.Buffer.Alloc->Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
	ResourceBarrier(CmdBuffer, GFloorIB.IB.Buffer.Alloc->Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDEX_BUFFER);
}

static void SetupFloor()
{
/*
	auto FillVertices = [](void* Data)
	{
		check(Data);
		auto* Vertex = (FPosColorUVVertex*)Data;
		float Y = 10;
		float Extent = 250;
		Vertex[0].x = -Extent; Vertex[0].y = Y; Vertex[0].z = -Extent; Vertex[0].Color = 0xffff0000; Vertex[0].u = 0; Vertex[0].v = 0;
		Vertex[1].x = Extent; Vertex[1].y = Y; Vertex[1].z = -Extent; Vertex[1].Color = 0xff00ff00; Vertex[1].u = 1; Vertex[1].v = 0;
		Vertex[2].x = Extent; Vertex[2].y = Y; Vertex[2].z = Extent; Vertex[2].Color = 0xff0000ff; Vertex[2].u = 1; Vertex[2].v = 1;
		Vertex[3].x = -Extent; Vertex[3].y = Y; Vertex[3].z = Extent; Vertex[3].Color = 0xffff00ff; Vertex[3].u = 0; Vertex[3].v = 1;
	};
	MapAndFillBufferSyncOneShotCmdBuffer(&GFloorVB.Buffer, FillVertices, sizeof(FPosColorUVVertex) * 4);
*/
	uint32 NumQuadsX = 128;
	uint32 NumQuadsZ = 128;
	float Elevation = 40;
	GCreateFloorUB.Create(GDevice, GDescriptorPool, GMemMgr, true);
	{
		FCreateFloorUB& CreateFloorUB = *GCreateFloorUB.GetMappedData();
		CreateFloorUB.Y = 10;
		CreateFloorUB.Extent = 250;
		CreateFloorUB.NumQuadsX = NumQuadsX;
		CreateFloorUB.NumQuadsZ = NumQuadsZ;
		CreateFloorUB.Elevation = Elevation;
	}
	GFloorVB.Create(GDevice, GDescriptorPool, sizeof(FPosColorUVVertex), sizeof(FPosColorUVVertex) * 4 * NumQuadsX * NumQuadsZ, GMemMgr, false);
	GFloorIB.Create(GDevice, GDescriptorPool, true, 3 * 2 * (NumQuadsX - 1) * (NumQuadsZ - 1), GMemMgr, false);
	{
		auto* CmdBuffer = GCmdBufferMgr.AllocateCmdBuffer(GDevice);
		CmdBuffer->Begin();
		FillFloor(CmdBuffer);
		CmdBuffer->End();
		GCmdBufferMgr.Submit(GDevice, CmdBuffer);
		CmdBuffer->WaitForFence();
	}
/*
	auto FillIndices = [](void* Data)
	{
		check(Data);
		auto* Index = (uint32*)Data;
		Index[0] = 0;
		Index[1] = 1;
		Index[2] = 2;
		Index[3] = 3;
	};
	MapAndFillBufferSyncOneShotCmdBuffer(&GFloorIB.Buffer, FillIndices, sizeof(uint32) * 4);*/
}

bool DoInit(HINSTANCE hInstance, HWND hWnd, uint32& Width, uint32& Height)
{
	LPSTR CmdLine = ::GetCommandLineA();
	const char* Token = CmdLine;
	while (Token = strchr(Token, ' '))
	{
		++Token;
		if (!_strcmpi(Token, "-debugger"))
		{
			while (!::IsDebuggerPresent())
			{
				Sleep(0);
			}
		}
	}

	GInstance.Create(hInstance, hWnd);
	GInstance.CreateDevice(GDevice);
	GCmdBufferMgr.Create(GDevice/*.Device, GDevice.PresentQueueFamilyIndex*/);
	GMemMgr.Create(GDevice);
	GDescriptorPool.Create(GDevice);
	GSwapchain.Create(GInstance.DXGIFactory.Get(), hWnd, GDevice, Width, Height, GDescriptorPool);

	GObjectCache.Create(&GDevice);
	if (!LoadShadersAndGeometry())
	{
		return false;
	}
	GViewUB.Create(GDevice, GDescriptorPool, GMemMgr, true);

	GObjUB.Create(GDevice, GDescriptorPool, GMemMgr, true);
	GIdentityUB.Create(GDevice, GDescriptorPool, GMemMgr, true);

	{
		FObjUB& ObjUB = *GObjUB.GetMappedData();
		ObjUB.Obj = FMatrix4x4::GetIdentity();
	}

	{
		FObjUB& ObjUB = *GIdentityUB.GetMappedData();
		ObjUB.Obj = FMatrix4x4::GetIdentity();
	}

	GRenderTargetPool.Create();//GDevice.Device, &GMemMgr);

	CreateAndFillTexture();

	GSampler.Create(GDevice, GDescriptorPool);
	SetupFloor();

#if TRY_MULTITHREADED
	GThread.Create();
#endif
	return true;
}

static void DrawCube(/*FGfxPipeline* GfxPipeline, */FDevice* Device, FCmdBuffer* CmdBuffer)
{
	{
		FObjUB& ObjUB = *GObjUB.GetMappedData();
		static float AngleDegrees = 0;
		{
			AngleDegrees += 360.0f / 10.0f / 60.0f;
			AngleDegrees = fmod(AngleDegrees, 360.0f);
		}
		ObjUB.Obj = FMatrix4x4::GetRotationY(ToRadians(AngleDegrees));
	}
	ID3D12DescriptorHeap* ppHeaps[] = {GDescriptorPool.CSUHeap.Get(), GDescriptorPool.SamplerHeap.Get()};
	CmdBuffer->CommandList->SetGraphicsRootSignature(GTestPSO.RootSignature.Get());
	CmdBuffer->CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	CmdBuffer->CommandList->SetGraphicsRootConstantBufferView(0, GViewUB.View.BufferLocation);
	CmdBuffer->CommandList->SetGraphicsRootConstantBufferView(1, GObjUB.View.BufferLocation);
	CmdBuffer->CommandList->SetGraphicsRootDescriptorTable(2, GHeightMap.ImageView.Handle.GPU);
	CmdBuffer->CommandList->SetGraphicsRootDescriptorTable(3, GSampler.Handle.GPU);

	CmdBuffer->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CmdBind(CmdBuffer, &GObjVB);
	CmdBuffer->CommandList->DrawInstanced((uint32)GObj.Faces.size() * 3, 1, 0, 0);
}

static void DrawFloor(/*FGfxPipeline* GfxPipeline, */FDevice* Device, FCmdBuffer* CmdBuffer)
{
	ID3D12DescriptorHeap* ppHeaps[] = {GDescriptorPool.CSUHeap.Get(), GDescriptorPool.SamplerHeap.Get()};
	CmdBuffer->CommandList->SetGraphicsRootSignature(GTestPSO.RootSignature.Get());
	CmdBuffer->CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	CmdBuffer->CommandList->SetGraphicsRootConstantBufferView(0, GViewUB.View.BufferLocation);
	CmdBuffer->CommandList->SetGraphicsRootConstantBufferView(1, GIdentityUB.View.BufferLocation);
	CmdBuffer->CommandList->SetGraphicsRootDescriptorTable(2, GCheckerboardTexture.ImageView.Handle.GPU);
	CmdBuffer->CommandList->SetGraphicsRootDescriptorTable(3, GSampler.Handle.GPU);

	CmdBuffer->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CmdBind(CmdBuffer, &GFloorVB.VB);
	CmdBind(CmdBuffer, &GFloorIB.IB);
	CmdBuffer->CommandList->DrawIndexedInstanced(GFloorIB.IB.NumIndices, 1, 0, 0, 0);
}

static void SetDynamicStates(FCmdBuffer* CmdBuffer, uint32 Width, uint32 Height)
{
	D3D12_VIEWPORT Viewport;
	MemZero(Viewport);
	Viewport.Width = (float)Width;
	Viewport.Height = (float)Height;
	Viewport.MinDepth = 0;
	Viewport.MaxDepth = 1;
	CmdBuffer->CommandList->RSSetViewports(1, &Viewport);
	D3D12_RECT Scissor;
	MemZero(Scissor);
	Scissor.right = Width;
	Scissor.bottom = Height;
	CmdBuffer->CommandList->RSSetScissorRects(1, &Scissor);
}

static void UpdateCamera()
{
	FViewUB& ViewUB = *GViewUB.GetMappedData();
	ViewUB.View = FMatrix4x4::GetIdentity();
	//ViewUB.View.Values[3 * 4 + 2] = -10;
	GCameraPos = GCameraPos.Add(GControl.StepDirection.Mul3({0.01f, 0.01f, -0.01f}));
	GRequestControl.StepDirection = {0, 0, 0};
	GControl.StepDirection ={0, 0, 0};
	ViewUB.View.Rows[3] = GCameraPos;
	ViewUB.Proj = CalculateProjectionMatrix(ToRadians(60), (float)GSwapchain.GetWidth() / (float)GSwapchain.GetHeight(), 0.1f, 1000.0f);
}

static void InternalRenderFrame(FDevice* Device, /*FRenderPass* RenderPass, */FCmdBuffer* CmdBuffer, uint32 Width, uint32 Height)
{
	auto* GfxPipeline = GObjectCache.GetOrCreateGfxPipeline(&GTestPSO, &GPosColorUVFormat, /*Width, Height, RenderPass, */GControl.ViewMode == EViewMode::Wireframe);

	CmdBind(CmdBuffer, GfxPipeline);

	SetDynamicStates(CmdBuffer, Width, Height);
	DrawFloor(/*GfxPipeline, */Device, CmdBuffer);
	DrawCube(/*GfxPipeline, */Device, CmdBuffer);
}

static void RenderFrame(FDevice* Device, FCmdBuffer* CmdBuffer, FImage2DWithView* ColorBuffer, FImage2DWithView* DepthBuffer/*, FImage2DWithView* ResolveColorBuffer*/)
{
	UpdateCamera();

	FillFloor(CmdBuffer);
#if ENABLE_VULKAN
	VkFormat ColorFormat = ColorBuffer->GetFormat();
	auto* RenderPass = GObjectCache.GetOrCreateRenderPass(ColorBuffer->GetWidth(), ColorBuffer->GetHeight(), 1, &ColorFormat, DepthBuffer->GetFormat(), ColorBuffer->Image.Samples, ResolveColorBuffer);
	auto* Framebuffer = GObjectCache.GetOrCreateFramebuffer(RenderPass->RenderPass, ColorBuffer->GetImageView(), DepthBuffer->GetImageView(), ColorBuffer->GetWidth(), ColorBuffer->GetHeight(), ResolveColorBuffer ? ResolveColorBuffer->GetImageView() : VK_NULL_HANDLE);

	CmdBuffer->BeginRenderPass(RenderPass->RenderPass, *Framebuffer, TRY_MULTITHREADED == 1);
#endif
	FDescriptorHandle Handle = GDescriptorPool.AllocateRTV();
	D3D12_RENDER_TARGET_VIEW_DESC RTVDesc;
	MemZero(RTVDesc);
	RTVDesc.Format = ColorBuffer->GetFormat();
	RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	Device->Device->CreateRenderTargetView(ColorBuffer->Image.Alloc->Resource.Get(), &RTVDesc, Handle.CPU);
	CmdBuffer->CommandList->OMSetRenderTargets(1, &Handle.CPU, false, &DepthBuffer->ImageView.Handle.CPU);
	float ClearColor[4];
	MemZero(ClearColor);
	CmdBuffer->CommandList->ClearRenderTargetView(Handle.CPU, ClearColor, 0, nullptr);
#if TRY_MULTITHREADED == 1
	{
		GThread.ParentCmdBuffer = CmdBuffer;
		GThread.Width = ColorBuffer->GetWidth();
		GThread.Height = ColorBuffer->GetHeight();
		GThread.RenderPass = RenderPass;
		GThread.Framebuffer = Framebuffer;
		ResetEvent(GThread.DoneEvent);
		SetEvent(GThread.StartEvent);
		WaitForSingleObject(GThread.DoneEvent, INFINITE);
		CmdBuffer->ExecuteSecondary();
	}
#else
	InternalRenderFrame(Device, /*RenderPass, */CmdBuffer, ColorBuffer->GetWidth(), ColorBuffer->GetHeight());
#endif

#if ENABLE_VULKAN
	CmdBuffer->EndRenderPass();
#endif
}

void RenderPost(FDevice& Device, FCmdBuffer* CmdBuffer, FRenderTargetPool::FEntry* SceneColorEntry, FRenderTargetPool::FEntry* SceneColorAfterPostEntry)
{
#if ENABLE_VULKAN
	SceneColorEntry->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_GENERAL);
	SceneColorAfterPostEntry->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_GENERAL);
#endif
	auto* ComputePipeline = GObjectCache.GetOrCreateComputePipeline(&GTestComputePostPSO);
	CmdBuffer->CommandList->SetPipelineState(ComputePipeline->PipelineState.Get());
	CmdBuffer->CommandList->SetComputeRootSignature(GTestComputePostPSO.RootSignature.Get());

	{
		FDescriptorHandle InHandle = GDescriptorPool.AllocateCSU();
		FDescriptorHandle OutHandle = GDescriptorPool.AllocateCSU();
		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = MakeTexture2DUAVDesc(SceneColorEntry->Texture.GetFormat());
		GDevice.Device->CreateUnorderedAccessView(SceneColorEntry->Texture.Image.Alloc->Resource.Get(), nullptr, &UAVDesc, InHandle.CPU);
		GDevice.Device->CreateUnorderedAccessView(SceneColorAfterPostEntry->Texture.Image.Alloc->Resource.Get(), nullptr, &UAVDesc, OutHandle.CPU);
		CmdBuffer->CommandList->SetComputeRootDescriptorTable(0, InHandle.GPU);	// Setting IB and then VB as they are contiguous
	}

	CmdBuffer->CommandList->Dispatch(SceneColorAfterPostEntry->Texture.Image.Width / 8, SceneColorAfterPostEntry->Texture.Image.Height / 8, 1);
}

#if ENABLE_VULKAN
#if TRY_MULTITHREADED
DWORD __stdcall FThread::ThreadFunction(void* Param)
{
	auto* This = (FThread*)Param;

	FCmdBufferMgr ThreadMgr;
	ThreadMgr.Create(GDevice.Device, GDevice.PresentQueueFamilyIndex);
	while (!This->bDoQuit)
	{
		WaitForSingleObject(This->StartEvent, INFINITE);
		if (This->bDoQuit)
		{
			break;
		}

		VkFormat ColorFormat = (VkFormat)GSwapchain.BACKBUFFER_VIEW_FORMAT;
		FCmdBuffer* ParentCmdBuffer = (FCmdBuffer*)This->ParentCmdBuffer;
		auto* CmdBuffer = ThreadMgr.AllocateSecondaryCmdBuffer(ParentCmdBuffer->Fence);
		CmdBuffer->BeginSecondary(ParentCmdBuffer, This->RenderPass ? This->RenderPass->RenderPass : VK_NULL_HANDLE, This->Framebuffer ? This->Framebuffer->Framebuffer : VK_NULL_HANDLE);

#if TRY_MULTITHREADED == 1
		InternalRenderFrame(GDevice.Device, (FRenderPass*)This->RenderPass, CmdBuffer, This->Width, This->Height);
#elif TRY_MULTITHREADED == 2
		RenderPost(GDevice.Device, CmdBuffer, &GSceneColor, &GSceneColorAfterPost);
#endif

		CmdBuffer->End();

		//::Sleep(0);
		//This->bWorkDone = true;
		ResetEvent(This->StartEvent);
		SetEvent(This->DoneEvent);
	}

	ThreadMgr.Destroy();

	return 0;
}
#endif
#endif

void DoRender()
{
	if (GQuitting)
	{
		return;
	}

	GRenderTargetPool.EmptyPool();

	GControl = GRequestControl;

	auto* CmdBuffer = GCmdBufferMgr.GetActiveCmdBuffer(GDevice);
	CmdBuffer->Begin();
	ResourceBarrier(CmdBuffer, GSwapchain.GetAcquiredImage(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
	{
		static int N = 0;
		++N;
		const float ClearColor[] ={(float)N / 256.0f, 0.2f, 0.4f, 1.0f};
		CmdBuffer->CommandList->ClearRenderTargetView(GSwapchain.GetAcquiredImageView(), ClearColor, 0, nullptr);
	}

	auto* SceneColor = GRenderTargetPool.Acquire(&GDevice, GControl.DoMSAA ? L"SceneColorMSAA" : L"SceneColor", GSwapchain.GetWidth(), GSwapchain.GetHeight(), DXGI_FORMAT_R8G8B8A8_UNORM, GMemMgr
#if ENABLE_VULKAN
		1, GControl.DoMSAA ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT
#endif
	);
#if ENABLE_VULKAN
	SceneColor->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	//TestCompute(CmdBuffer);

	VkFormat ColorFormat = (VkFormat)GSwapchain.BACKBUFFER_VIEW_FORMAT;
#endif
	auto* DepthBuffer = GRenderTargetPool.Acquire(&GDevice, L"DepthBuffer", GSwapchain.GetWidth(), GSwapchain.GetHeight(), DXGI_FORMAT_D32_FLOAT, GMemMgr);// VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, SceneColor->Texture.Image.Samples);
	CmdBuffer->CommandList->ClearDepthStencilView(DepthBuffer->Texture.ImageView.Handle.CPU, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
#if ENABLE_VULKAN
	DepthBuffer->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	if (GControl.DoMSAA)
	{
/*
		VkImageResolve Region;
		MemZero(Region);
		Region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Region.srcSubresource.layerCount = 1;
		Region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		Region.dstSubresource.layerCount = 1;
		Region.extent.width = SceneColor->Texture.GetWidth();
		Region.extent.height = SceneColor->Texture.GetHeight();
		Region.extent.depth = 1;

		auto* MSAA = SceneColor;
		SceneColor = GRenderTargetPool.Acquire("SceneColor", GSwapchain.GetWidth(), GSwapchain.GetHeight(), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, VK_SAMPLE_COUNT_1_BIT);
		vkCmdResolveImage(CmdBuffer->CmdBuffer, MSAA->Texture.GetImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SceneColor->Texture.GetImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, &Region);
*/
		auto* ResolvedSceneColor = GRenderTargetPool.Acquire("SceneColor", GSwapchain.GetWidth(), GSwapchain.GetHeight(), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, VK_SAMPLE_COUNT_1_BIT);
		ResolvedSceneColor->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		RenderFrame(GDevice.Device, CmdBuffer, &SceneColor->Texture, &DepthBuffer->Texture, &ResolvedSceneColor->Texture);

		auto* MSAA = SceneColor;
		GRenderTargetPool.Release(MSAA);
		SceneColor = ResolvedSceneColor;
	}
	else
#endif
	{
		RenderFrame(&GDevice, CmdBuffer, &SceneColor->Texture, &DepthBuffer->Texture/*, nullptr*/);
	}

	GRenderTargetPool.Release(DepthBuffer);

	if (GControl.DoPost)
	{
#if TRY_MULTITHREADED == 2
		check(0);
		GThread.ParentCmdBuffer = CmdBuffer;
		GThread.Width = 0;
		GThread.Height = 0;
		GThread.RenderPass = nullptr;
		GThread.Framebuffer = nullptr;
		ResetEvent(GThread.DoneEvent);
		SetEvent(GThread.StartEvent);
		WaitForSingleObject(GThread.DoneEvent, INFINITE);
		CmdBuffer->ExecuteSecondary();
#else
		auto* PrePost = SceneColor;
		SceneColor = GRenderTargetPool.Acquire(&GDevice, L"SceneColor", GSwapchain.GetWidth(), GSwapchain.GetHeight(), DXGI_FORMAT_R8G8B8A8_UNORM, GMemMgr);//, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, VK_SAMPLE_COUNT_1_BIT);
		//SceneColor->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		RenderPost(GDevice, CmdBuffer, PrePost, SceneColor);
#endif
	}

	ResourceBarrier(CmdBuffer, GSwapchain.GetAcquiredImage(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
	// Blit post into scene color
	{
		uint32 Width = min(GSwapchain.GetWidth(), SceneColor->Texture.GetWidth());
		uint32 Height = min(GSwapchain.GetHeight(), SceneColor->Texture.GetHeight());
		//SceneColor->DoTransition(CmdBuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		ResourceBarrier(CmdBuffer, &SceneColor->Texture.Image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
		BlitColorImage(CmdBuffer, Width, Height, SceneColor->Texture.Image.Alloc->Resource.Get(), /*VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, */GSwapchain.GetAcquiredImage()/*, VK_IMAGE_LAYOUT_UNDEFINED*/);
		ResourceBarrier(CmdBuffer, &SceneColor->Texture.Image, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
		GRenderTargetPool.Release(SceneColor);
	}
	ResourceBarrier(CmdBuffer, GSwapchain.GetAcquiredImage(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
	CmdBuffer->End();

	// First submit needs to wait for present semaphore
	GCmdBufferMgr.Submit(GDevice, CmdBuffer);//, GDevice.PresentQueue, &GSwapchain.PresentCompleteSemaphores[GSwapchain.PresentCompleteSemaphoreIndex], &GSwapchain.RenderingSemaphores[GSwapchain.AcquiredImageIndex]);

	GSwapchain.Present(GDevice.Queue.Get());
}

void DoResize(uint32 Width, uint32 Height)
{
#if ENABLE_VULKAN
	if (Width != GSwapchain.GetWidth() && Height != GSwapchain.GetHeight())
	{
		vkDeviceWaitIdle(GDevice.Device);
		GSwapchain.Destroy();
		GObjectCache.Destroy();
		GSwapchain.Create(GInstance.Surface, GDevice.PhysicalDevice, GDevice.Device, GInstance.Surface, Width, Height);
		GObjectCache.Create(&GDevice);
	}
#endif
}

void DoDeinit()
{
	GCmdBufferMgr.Destroy();
	GRenderTargetPool.EmptyPool();
#if ENABLE_VULKAN
	checkVk(vkDeviceWaitIdle(GDevice.Device));
#if TRY_MULTITHREADED
	GThread.bDoQuit = true;
	SetEvent(GThread.StartEvent);
	WaitForMultipleObjects(1, &GThread.ThreadHandle, TRUE, INFINITE);
#endif
#endif
	GQuitting = true;
	GFloorIB.Destroy();
	GFloorVB.Destroy();
	GViewUB.Destroy();
	GCreateFloorUB.Destroy();
	GObjUB.Destroy();
	GObjVB.Destroy();
	GIdentityUB.Destroy();
	GSampler.Destroy();
	GCheckerboardTexture.Destroy();
	GHeightMap.Destroy();
	GDescriptorPool.Destroy();
	GTestComputePostPSO.Destroy();
#if ENABLE_VULKAN
	GTestComputePSO.Destroy(GDevice.Device);
#endif
	GTestPSO.Destroy();
	GSetupFloorPSO.Destroy();
	GFillTexturePSO.Destroy();

	GStagingManager.Destroy();
	GRenderTargetPool.Destroy();
	GObjectCache.Destroy();
	GMemMgr.Destroy();
	GSwapchain.Destroy();
	GDevice.Destroy();
	GInstance.Destroy();
}
