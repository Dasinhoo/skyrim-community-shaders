#include "FidelityFX.h"

#include "State.h"
#include <Util.h>

FfxErrorCode FidelityFX::Initialize(uint32_t a_maxContexts)
{
	auto state = State::GetSingleton();

	logger::info("[FidelityFX] Initialising");

	const auto fsrDevice = ffxGetDeviceDX11(state->device);
	const auto scratchSize = ffxGetScratchMemorySizeDX11(a_maxContexts);

	size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(a_maxContexts);
	void* scratchBuffer = malloc(scratchBufferSize);
	memset(scratchBuffer, 0, scratchBufferSize);

	auto errorCode = ffxGetInterfaceDX11(&ffxInterface, fsrDevice, scratchBuffer, scratchBufferSize, a_maxContexts);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised");
	} else {
		logger::error("[FidelityFX] Failed to initialise!");
	}

	dx11CommandList = ffxGetCommandListDX11(state->context);

	return errorCode;
}

typedef enum Fsr3BackendTypes : uint32_t
{
	FSR3_BACKEND_SHARED_RESOURCES,
	FSR3_BACKEND_UPSCALING,
	FSR3_BACKEND_FRAME_INTERPOLATION,
	FSR3_BACKEND_COUNT
} Fsr3BackendTypes;

FfxErrorCode FidelityFX::InitializeFSR3()
{
	auto state = State::GetSingleton();

	FfxErrorCode errorCode = 0;
	FfxInterface ffxFsr3Backends_[FSR3_BACKEND_COUNT] = {};
	const auto fsrDevice = ffxGetDeviceDX11(state->device);

	int effectCounts[] = { 1, 1, 2 };
	for (auto i = 0; i < FSR3_BACKEND_COUNT; i++) {
		const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(effectCounts[i]);
		void* scratchBuffer = calloc(scratchBufferSize, 1);
		memset(scratchBuffer, 0, scratchBufferSize);
		errorCode |= ffxGetInterfaceDX11(&ffxFsr3Backends_[i], fsrDevice, scratchBuffer, scratchBufferSize, effectCounts[i]);
	}

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised FSR3 backend interfaces");
	} else {
		logger::error("[FidelityFX] Failed to initialise FSR3 backend interfaces!");
		return errorCode;
	}

	FfxFsr3ContextDescription contextDescription;
	contextDescription.maxRenderSize.width = (uint)state->screenSize.x;
	contextDescription.maxRenderSize.height = (uint)state->screenSize.y;
	contextDescription.upscaleOutputSize.width = (uint)state->screenSize.x;
	contextDescription.upscaleOutputSize.height = (uint)state->screenSize.y;
	contextDescription.displaySize.width = (uint)state->screenSize.x;
	contextDescription.displaySize.height = (uint)state->screenSize.y;
	contextDescription.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE;
	contextDescription.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;

	contextDescription.backendInterfaceSharedResources = ffxFsr3Backends_[FSR3_BACKEND_SHARED_RESOURCES];
	contextDescription.backendInterfaceUpscaling = ffxFsr3Backends_[FSR3_BACKEND_UPSCALING];
	contextDescription.backendInterfaceFrameInterpolation = ffxFsr3Backends_[FSR3_BACKEND_FRAME_INTERPOLATION];

	errorCode = ffxFsr3ContextCreate(&fsrContext, &contextDescription);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised FSR3 context");
	} else {
		logger::error("[FidelityFX] Failed to initialise FSR3 context!");
		return errorCode;
	}

	auto manager = RE::BSGraphics::Renderer::GetSingleton();

	FfxSwapchain ffxSwapChain = reinterpret_cast<void*>(manager->GetRuntimeData().renderWindows->swapChain);

	FfxFrameGenerationConfig frameGenerationConfig;
	frameGenerationConfig.frameGenerationEnabled = true;
	frameGenerationConfig.frameGenerationCallback = ffxFsr3DispatchFrameGeneration;
	frameGenerationConfig.presentCallback = nullptr;
	frameGenerationConfig.swapChain = ffxSwapChain;
	frameGenerationConfig.HUDLessColor = FfxResource({});

	errorCode = ffxFsr3ConfigureFrameGeneration(&fsrContext, &frameGenerationConfig);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised frame generation");
	} else {
		logger::error("[FidelityFX] Failed to initialise frame generation!");
		return errorCode;
	}

	SetupFrameGenerationResources();

	return errorCode;
}

// register a DX11 resource to the backend
FfxResource ffxGetResource(ID3D11Resource* dx11Resource,
	wchar_t const* ffxResName,
	FfxResourceStates state /*=FFX_RESOURCE_STATE_COMPUTE_READ*/)
{
	FfxResource resource = {};
	resource.resource = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
	resource.state = state;
	resource.description = GetFfxResourceDescriptionDX11(dx11Resource);

#ifdef _DEBUG
	if (ffxResName) {
		wcscpy_s(resource.name, ffxResName);
	}
#endif

	return resource;
}

void FidelityFX::SetupFrameGenerationResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.RTV->GetDesc(&rtvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	swapChainPreviousTexture = new Texture2D(texDesc);
	swapChainPreviousTexture->CreateSRV(srvDesc);
	swapChainPreviousTexture->CreateRTV(rtvDesc);
	swapChainPreviousTexture->CreateUAV(uavDesc);

	swapChainPreviousTextureSwap = new Texture2D(texDesc);
	swapChainPreviousTextureSwap->CreateSRV(srvDesc);
	swapChainPreviousTextureSwap->CreateRTV(rtvDesc);
	swapChainPreviousTextureSwap->CreateUAV(uavDesc);

	swapChainTempTexture = new Texture2D(texDesc);
	swapChainTempTexture->CreateSRV(srvDesc);
	swapChainTempTexture->CreateRTV(rtvDesc);
	swapChainTempTexture->CreateUAV(uavDesc);
}

void FidelityFX::DispatchUpscaling()
{
	if (enableFrameGeneration) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& motionVectors = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

		FfxFsr3DispatchUpscaleDescription dispatchParameters{};

		dispatchParameters.commandList = dx11CommandList;
		dispatchParameters.color = ffxGetResource(nullptr, L"FSR3_Input_OutputColor", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.depth = ffxGetResource(depth.texture, L"FSR3_InputDepth", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.motionVectors = ffxGetResource(motionVectors.texture, L"FSR3_InputMotionVectors", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.exposure = ffxGetResource(nullptr, L"FSR3_InputExposure", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.upscaleOutput = dispatchParameters.color;
		dispatchParameters.reactive = ffxGetResource(nullptr, L"FSR3_InputReactiveMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.transparencyAndComposition = ffxGetResource(nullptr, L"FSR3_TransparencyAndCompositionMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

		dispatchParameters.jitterOffset.x = 0;
		dispatchParameters.jitterOffset.y = 0;

		auto state = State::GetSingleton();

		dispatchParameters.motionVectorScale.x = state->screenSize.x;
		dispatchParameters.motionVectorScale.y = state->screenSize.y;

		dispatchParameters.reset = false;

		dispatchParameters.enableSharpening = false;
		dispatchParameters.sharpness = 0.0;

		static float* g_deltaTime = (float*)RELOCATION_ID(523660, 410199).address();  // 2F6B948, 30064C8

		dispatchParameters.frameTimeDelta = *g_deltaTime * 1000.f;  // Milliseconds!

		dispatchParameters.preExposure = 1.0f;

		dispatchParameters.renderSize.width = (uint)state->screenSize.x;
		dispatchParameters.renderSize.height = (uint)state->screenSize.y;

		auto cameraData = Util::GetCameraData(0);
		dispatchParameters.cameraFovAngleVertical = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);
		dispatchParameters.cameraNear = cameraData.viewDepthRange.x;
		dispatchParameters.cameraFar = cameraData.viewDepthRange.y;

		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;

		FfxErrorCode errorCode = ffxFsr3ContextDispatchUpscale(&fsrContext, &dispatchParameters);
		if (errorCode != FFX_OK) {
			logger::error("[FidelityFX] Failed to dispatch upscaling!");
		}
	}
}

void FidelityFX::DispatchFrameGeneration()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];

	ID3D11Resource* swapChainResource;
	swapChain.SRV->GetResource(&swapChainResource);

	FfxResource backbuffer = ffxGetResource(swapChainResource, nullptr, FFX_RESOURCE_STATE_PRESENT);
	FfxResource backbufferFrameGeneration = ffxGetResource(swapChainTempTexture->resource.get(), nullptr, FFX_RESOURCE_STATE_PRESENT);

	FfxFrameGenerationDispatchDescription fgDesc{};
	fgDesc.commandList = dx11CommandList;
	fgDesc.outputs[0] = backbufferFrameGeneration;
	fgDesc.presentColor = backbuffer;
	fgDesc.reset = false;
	fgDesc.numInterpolatedFrames = 1;
	fgDesc.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
	fgDesc.minMaxLuminance[0] = 0.0;
	fgDesc.minMaxLuminance[1] = 200.0;

	FfxErrorCode errorCode = ffxFsr3DispatchFrameGeneration(&fgDesc);
	if (errorCode != FFX_OK) {
		logger::error("[FidelityFX] Failed to dispatch frame generation!");
	}
}

extern decltype(&IDXGISwapChain::Present) ptr_IDXGISwapChain_Present;

void FidelityFX::Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
	if (enableFrameGeneration) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& context = State::GetSingleton()->context;

		context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

		auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

		ID3D11Resource* swapChainResource;
		swapChain.SRV->GetResource(&swapChainResource);

		DispatchUpscaling();
		DispatchFrameGeneration();

		static bool swap = false;

		if (swap) {
			// Back up current frame
			context->CopyResource(swapChainPreviousTexture->resource.get(), swapChainResource);

			// Swap current frame with previous frame
			context->CopyResource(swapChainResource, swapChainPreviousTextureSwap->resource.get());

		} else {
			// Back up current frame
			context->CopyResource(swapChainPreviousTextureSwap->resource.get(), swapChainResource);

			// Swap current frame with previous frame
			context->CopyResource(swapChainResource, swapChainPreviousTexture->resource.get());
		}

		swap = !swap;

		(This->*ptr_IDXGISwapChain_Present)(SyncInterval, Flags);

		// Swap current frame with interpolated frame
		context->CopyResource(swapChainResource, swapChainTempTexture->resource.get());
	}
}