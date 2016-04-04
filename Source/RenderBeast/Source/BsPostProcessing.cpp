//********************************** Banshee Engine (www.banshee3d.com) **************************************************//
//**************** Copyright (c) 2016 Marko Pintera (marko.pintera@gmail.com). All rights reserved. **********************//
#include "BsPostProcessing.h"
#include "BsRenderTexture.h"
#include "BsRenderTexturePool.h"
#include "BsRendererUtility.h"
#include "BsTextureManager.h"

namespace BansheeEngine
{
	DownsampleMat::DownsampleMat()
	{
		mMaterial->setParamBlockBuffer("Input", mParams.getBuffer());

		mInputTexture = mMaterial->getParamTexture("gInputTex");
		mInvTexSize = mMaterial->getParamVec2("gInvTexSize");
	}

	void DownsampleMat::_initDefines(ShaderDefines& defines)
	{
		// Do nothing
	}

	void DownsampleMat::execute(const SPtr<RenderTextureCore>& target, PostProcessInfo& ppInfo)
	{
		// Set parameters
		SPtr<TextureCore> colorTexture = target->getBindableColorTexture();
		mInputTexture.set(colorTexture);

		const RenderTextureProperties& rtProps = target->getProperties();
		Vector2 invTextureSize(1.0f / rtProps.getWidth(), 1.0f / rtProps.getHeight());

		mParams.gInvTexSize.set(invTextureSize);

		// Set output
		const TextureProperties& colorProps = colorTexture->getProperties();

		UINT32 width = std::max(1, Math::ceilToInt(colorProps.getWidth() * 0.5f));
		UINT32 height = std::max(1, Math::ceilToInt(colorProps.getHeight() * 0.5f));

		mOutputDesc = POOLED_RENDER_TEXTURE_DESC::create2D(colorProps.getFormat(), width, height, TU_RENDERTARGET);

		// Render
		ppInfo.downsampledSceneTex = RenderTexturePool::instance().get(mOutputDesc);

		RenderAPICore& rapi = RenderAPICore::instance();
		rapi.setRenderTarget(ppInfo.downsampledSceneTex->renderTexture, true);

		gRendererUtility().setPass(mMaterial, 0);
		gRendererUtility().drawScreenQuad();

		mOutput = ppInfo.downsampledSceneTex->renderTexture;
	}

	void DownsampleMat::release(PostProcessInfo& ppInfo)
	{
		RenderTexturePool::instance().release(ppInfo.downsampledSceneTex);
		mOutput = nullptr;
	}

	EyeAdaptHistogramMat::EyeAdaptHistogramMat()
	{
		mMaterial->setParamBlockBuffer("Input", mParams.getBuffer());

		mSceneColor = mMaterial->getParamTexture("gSceneColorTex");
		mOutputTex = mMaterial->getParamLoadStoreTexture("gOutputTex");
	}

	void EyeAdaptHistogramMat::_initDefines(ShaderDefines& defines)
	{
		defines.set("THREADGROUP_SIZE_X", THREAD_GROUP_SIZE_X);
		defines.set("THREADGROUP_SIZE_Y", THREAD_GROUP_SIZE_Y);
		defines.set("LOOP_COUNT_X", LOOP_COUNT_X);
		defines.set("LOOP_COUNT_Y", LOOP_COUNT_Y);
	}

	void EyeAdaptHistogramMat::execute(PostProcessInfo& ppInfo)
	{
		// Set parameters
		SPtr<RenderTextureCore> target = ppInfo.downsampledSceneTex->renderTexture;
		mSceneColor.set(ppInfo.downsampledSceneTex->texture);

		const RenderTextureProperties& props = target->getProperties();
		int offsetAndSize[4] = { 0, 0, (INT32)props.getWidth(), (INT32)props.getHeight() };

		mParams.gHistogramParams.set(getHistogramScaleOffset(ppInfo));
		mParams.gPixelOffsetAndSize.set(Vector4I(offsetAndSize));

		Vector2I threadGroupCount = getThreadGroupCount(target);
		mParams.gThreadGroupCount.set(threadGroupCount);

		// Set output
		UINT32 numHistograms = threadGroupCount.x * threadGroupCount.y;

		mOutputDesc = POOLED_RENDER_TEXTURE_DESC::create2D(PF_FLOAT16_RGBA, HISTOGRAM_NUM_TEXELS, numHistograms,
			TU_LOADSTORE);

		// Dispatch
		ppInfo.histogramTex = RenderTexturePool::instance().get(mOutputDesc);

		mOutputTex.set(ppInfo.histogramTex->texture);

		// TODO - Clear downsampled scene texture as render target before dispatch?
		RenderAPICore& rapi = RenderAPICore::instance();
		rapi.dispatchCompute(threadGroupCount.x, threadGroupCount.y);

		mOutput = ppInfo.histogramTex->renderTexture;
	}

	void EyeAdaptHistogramMat::release(PostProcessInfo& ppInfo)
	{
		RenderTexturePool::instance().release(ppInfo.histogramTex);
		mOutput = nullptr;
	}

	Vector2I EyeAdaptHistogramMat::getThreadGroupCount(const SPtr<RenderTextureCore>& target)
	{
		const UINT32 texelsPerThreadGroupX = THREAD_GROUP_SIZE_X * LOOP_COUNT_X;
		const UINT32 texelsPerThreadGroupY = THREAD_GROUP_SIZE_Y * LOOP_COUNT_Y;

		const RenderTextureProperties& props = target->getProperties();
	
		Vector2I threadGroupCount;
		threadGroupCount.x = ((INT32)props.getWidth() + texelsPerThreadGroupX - 1) / texelsPerThreadGroupX;
		threadGroupCount.y = ((INT32)props.getHeight() + texelsPerThreadGroupY - 1) / texelsPerThreadGroupY;

		return threadGroupCount;
	}

	Vector2 EyeAdaptHistogramMat::getHistogramScaleOffset(const PostProcessInfo& ppInfo)
	{
		const PostProcessSettings& settings = ppInfo.settings;

		float diff = settings.histogramLog2Max - settings.histogramLog2Min;
		float scale = 1.0f / diff;
		float offset = -settings.histogramLog2Min * scale;

		return Vector2(scale, offset);
	}

	EyeAdaptHistogramReduceMat::EyeAdaptHistogramReduceMat()
	{
		mMaterial->setParamBlockBuffer("Input", mParams.getBuffer());

		mHistogramTex = mMaterial->getParamTexture("gHistogramTex");
		mEyeAdaptationTex = mMaterial->getParamTexture("gEyeAdaptationTex");
	}

	void EyeAdaptHistogramReduceMat::_initDefines(ShaderDefines& defines)
	{
		// Do nothing
	}

	void EyeAdaptHistogramReduceMat::execute(PostProcessInfo& ppInfo)
	{
		// Set parameters
		mHistogramTex.set(ppInfo.histogramTex->texture); // TODO - Unbind this from render target slot first?

		SPtr<PooledRenderTexture> eyeAdaptationRT = ppInfo.eyeAdaptationTex[ppInfo.lastEyeAdaptationTex];
		SPtr<TextureCore> eyeAdaptationTex;

		if (eyeAdaptationRT != nullptr) // Could be that this is the first run
			eyeAdaptationTex = eyeAdaptationRT->texture;
		else
			eyeAdaptationTex = TextureCore::WHITE;

		mEyeAdaptationTex.set(eyeAdaptationTex);

		Vector2I threadGroupCount = EyeAdaptHistogramMat::getThreadGroupCount(ppInfo.downsampledSceneTex->renderTexture);
		mParams.gThreadGroupCount.set(threadGroupCount);

		// Set output
		mOutputDesc = POOLED_RENDER_TEXTURE_DESC::create2D(PF_FLOAT16_RGBA, EyeAdaptHistogramMat::HISTOGRAM_NUM_TEXELS, 2,
			TU_LOADSTORE);

		// Render
		ppInfo.histogramReduceTex = RenderTexturePool::instance().get(mOutputDesc);

		RenderAPICore& rapi = RenderAPICore::instance();
		rapi.setRenderTarget(ppInfo.histogramReduceTex->renderTexture, true);

		gRendererUtility().setPass(mMaterial, 0);
		gRendererUtility().drawScreenQuad();

		mOutput = ppInfo.histogramReduceTex->renderTexture;
	}

	void EyeAdaptHistogramReduceMat::release(PostProcessInfo& ppInfo)
	{
		RenderTexturePool::instance().release(ppInfo.histogramReduceTex);
		mOutput = nullptr;
	}

	EyeAdaptationMat::EyeAdaptationMat()
	{
		mMaterial->setParamBlockBuffer("Input", mParams.getBuffer());

		mReducedHistogramTex = mMaterial->getParamTexture("gHistogramTex");
	}

	void EyeAdaptationMat::_initDefines(ShaderDefines& defines)
	{
		defines.set("THREADGROUP_SIZE_X", EyeAdaptHistogramMat::THREAD_GROUP_SIZE_X);
		defines.set("THREADGROUP_SIZE_Y", EyeAdaptHistogramMat::THREAD_GROUP_SIZE_Y);
	}

	void EyeAdaptationMat::execute(PostProcessInfo& ppInfo, float frameDelta)
	{
		bool texturesInitialized = ppInfo.eyeAdaptationTex[0] != nullptr && ppInfo.eyeAdaptationTex[1] != nullptr;
		if(!texturesInitialized)
		{
			POOLED_RENDER_TEXTURE_DESC outputDesc = POOLED_RENDER_TEXTURE_DESC::create2D(PF_FLOAT32_R, 1, 1, TU_RENDERTARGET);
			ppInfo.eyeAdaptationTex[0] = RenderTexturePool::instance().get(outputDesc);
			ppInfo.eyeAdaptationTex[1] = RenderTexturePool::instance().get(outputDesc);
		}

		ppInfo.lastEyeAdaptationTex = (ppInfo.lastEyeAdaptationTex + 1) % 2; // TODO - Do I really need two targets?

		// Set parameters
		mReducedHistogramTex.set(ppInfo.histogramReduceTex->texture); // TODO - Unbind this from render target slot first?

		Vector2 histogramScaleAndOffset = EyeAdaptHistogramMat::getHistogramScaleOffset(ppInfo);

		const PostProcessSettings& settings = ppInfo.settings;

		Vector4 eyeAdaptationParams[3];
		eyeAdaptationParams[0].x = histogramScaleAndOffset.x;
		eyeAdaptationParams[0].y = histogramScaleAndOffset.y;

		float histogramPctHigh = Math::clamp01(settings.histogramPctHigh);

		eyeAdaptationParams[0].z = std::min(Math::clamp01(settings.histogramPctLow), histogramPctHigh);
		eyeAdaptationParams[0].w = histogramPctHigh;

		eyeAdaptationParams[1].x = std::min(settings.minEyeAdaptation, settings.maxEyeAdaptation);
		eyeAdaptationParams[1].y = settings.maxEyeAdaptation;

		eyeAdaptationParams[1].z = settings.eyeAdaptationSpeedUp;
		eyeAdaptationParams[1].w = settings.eyeAdaptationSpeedDown;

		eyeAdaptationParams[2].x = Math::pow(2.0f, settings.exposureScale);
		eyeAdaptationParams[2].y = frameDelta;

		mParams.gEyeAdaptationParams.set(eyeAdaptationParams[0], 0);
		mParams.gEyeAdaptationParams.set(eyeAdaptationParams[1], 1);
		mParams.gEyeAdaptationParams.set(eyeAdaptationParams[2], 2);

		// Render
		SPtr<PooledRenderTexture> eyeAdaptationRT = ppInfo.eyeAdaptationTex[ppInfo.lastEyeAdaptationTex];

		RenderAPICore& rapi = RenderAPICore::instance();
		rapi.setRenderTarget(eyeAdaptationRT->renderTexture, true);

		gRendererUtility().setPass(mMaterial, 0);
		gRendererUtility().drawScreenQuad();
	}

	void PostProcessing::postProcess(const SPtr<RenderTextureCore>& target, PostProcessInfo& ppInfo, float frameDelta)
	{
		mDownsample.execute(target, ppInfo);
		mEyeAdaptHistogram.execute(ppInfo);
		mDownsample.release(ppInfo);

		mEyeAdaptHistogramReduce.execute(ppInfo);
		mEyeAdaptHistogram.release(ppInfo);

		mEyeAdaptation.execute(ppInfo, frameDelta);
		mEyeAdaptHistogramReduce.release(ppInfo);

		// TODO - Generate LUT, run tonemapping, output everything to scene
	}
}