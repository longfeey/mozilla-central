/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CompositorD3D11.h"

#include "TextureD3D11.h"
#include "CompositorD3D11Shaders.h"

#include "gfxWindowsPlatform.h"
#include "nsIWidget.h"
#include "mozilla/layers/ImageHost.h"
#include "mozilla/layers/ContentHost.h"
#include "mozilla/layers/Effects.h"

namespace mozilla {

using namespace gfx;

namespace layers {

struct Vertex
{
    float position[2];
};

// {1E4D7BEB-D8EC-4A0B-BF0A-63E6DE129425}
static const GUID sDeviceAttachmentsD3D11 = 
{ 0x1e4d7beb, 0xd8ec, 0x4a0b, { 0xbf, 0xa, 0x63, 0xe6, 0xde, 0x12, 0x94, 0x25 } };
// {88041664-C835-4AA8-ACB8-7EC832357ED8}
static const GUID sLayerManagerCount = 
{ 0x88041664, 0xc835, 0x4aa8, { 0xac, 0xb8, 0x7e, 0xc8, 0x32, 0x35, 0x7e, 0xd8 } };

struct DeviceAttachmentsD3D11
{
  RefPtr<ID3D11InputLayout> mInputLayout;
  RefPtr<ID3D11Buffer> mVertexBuffer;
  RefPtr<ID3D11VertexShader> mVSQuadShader;
  RefPtr<ID3D11PixelShader> mSolidColorShader;
  RefPtr<ID3D11PixelShader> mRGBAShader;
  RefPtr<ID3D11PixelShader> mRGBShader;
  RefPtr<ID3D11PixelShader> mYCbCrShader;
  RefPtr<ID3D11Buffer> mPSConstantBuffer;
  RefPtr<ID3D11Buffer> mVSConstantBuffer;
  RefPtr<ID3D11RasterizerState> mRasterizerState;
  RefPtr<ID3D11SamplerState> mLinearSamplerState;
  RefPtr<ID3D11SamplerState> mPointSamplerState;
  RefPtr<ID3D11BlendState> mPremulBlendState;
};

CompositorD3D11::CompositorD3D11(nsIWidget *aWidget)
  : mWidget(aWidget)
  , mAttachments(nullptr)
{
}

CompositorD3D11::~CompositorD3D11()
{
  if (mDevice) {
    int referenceCount = 0;
    UINT size = sizeof(referenceCount);
    HRESULT hr = mDevice->GetPrivateData(sLayerManagerCount, &size, &referenceCount);
    NS_ASSERTION(SUCCEEDED(hr), "Reference count not found on device.");
    referenceCount--;
    mDevice->SetPrivateData(sLayerManagerCount, sizeof(referenceCount), &referenceCount);

    if (!referenceCount) {
      DeviceAttachmentsD3D11 *attachments;
      size = sizeof(attachments);
      mDevice->GetPrivateData(sDeviceAttachmentsD3D11, &size, &attachments);
      // No LayerManagers left for this device. Clear out interfaces stored which
      // hold a reference to the device.
      mDevice->SetPrivateData(sDeviceAttachmentsD3D11, 0, NULL);

      delete attachments;
    }
  }
}

bool
CompositorD3D11::Initialize()
{
  HRESULT hr;

  mDevice = gfxWindowsPlatform::GetPlatform()->GetD3D11Device();

  if (!mDevice) {
    return false;
  }

  mDevice->GetImmediateContext(byRef(mContext));

  if (!mContext) {
    return false;
  }

  int referenceCount = 0;
  UINT size = sizeof(referenceCount);
  // If this isn't there yet it'll fail, count will remain 0, which is correct.
  mDevice->GetPrivateData(sLayerManagerCount, &size, &referenceCount);
  referenceCount++;
  mDevice->SetPrivateData(sLayerManagerCount, sizeof(referenceCount), &referenceCount);

  size = sizeof(DeviceAttachmentsD3D11*);
  if (FAILED(mDevice->GetPrivateData(sDeviceAttachmentsD3D11, &size, &mAttachments))) {
    mAttachments = new DeviceAttachmentsD3D11;
    mDevice->SetPrivateData(sDeviceAttachmentsD3D11, sizeof(mAttachments), &mAttachments);

    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
      { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = mDevice->CreateInputLayout(layout,
                                    sizeof(layout) / sizeof(D3D11_INPUT_ELEMENT_DESC),
                                    LayerQuadVS,
                                    sizeof(LayerQuadVS),
                                    byRef(mAttachments->mInputLayout));
    
    if (FAILED(hr)) {
      return false;
    }

    Vertex vertices[] = { {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0} };
    CD3D11_BUFFER_DESC bufferDesc(sizeof(vertices), D3D11_BIND_VERTEX_BUFFER);
    D3D11_SUBRESOURCE_DATA data;
    data.pSysMem = (void*)vertices;

    hr = mDevice->CreateBuffer(&bufferDesc, &data, byRef(mAttachments->mVertexBuffer));

    if (FAILED(hr)) {
      return false;
    }

    if (!CreateShaders()) {
      return false;
    }

    CD3D11_BUFFER_DESC cBufferDesc(sizeof(VertexShaderConstants), D3D11_BIND_CONSTANT_BUFFER,
                                   D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);

    hr = mDevice->CreateBuffer(&cBufferDesc, nullptr, byRef(mAttachments->mVSConstantBuffer));
    if (FAILED(hr)) {
      return false;
    }

    cBufferDesc.ByteWidth = sizeof(PixelShaderConstants);
    hr = mDevice->CreateBuffer(&cBufferDesc, nullptr, byRef(mAttachments->mPSConstantBuffer));
    if (FAILED(hr)) {
      return false;
    }

    CD3D11_RASTERIZER_DESC rastDesc(D3D11_DEFAULT);
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.ScissorEnable = TRUE;

    hr = mDevice->CreateRasterizerState(&rastDesc, byRef(mAttachments->mRasterizerState));
    if (FAILED(hr)) {
      return false;
    }

    CD3D11_SAMPLER_DESC samplerDesc(D3D11_DEFAULT);
    samplerDesc.AddressU = samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    hr = mDevice->CreateSamplerState(&samplerDesc, byRef(mAttachments->mLinearSamplerState));
    if (FAILED(hr)) {
      return false;
    }

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = mDevice->CreateSamplerState(&samplerDesc, byRef(mAttachments->mPointSamplerState));
    if (FAILED(hr)) {
      return false;
    }
  }

  nsRefPtr<IDXGIDevice> dxgiDevice;
  nsRefPtr<IDXGIAdapter> dxgiAdapter;

  mDevice->QueryInterface(dxgiDevice.StartAssignment());
  dxgiDevice->GetAdapter(getter_AddRefs(dxgiAdapter));

#ifdef MOZ_METRO
  if (gfxWindowsPlatform::IsRunningInWindows8Metro()) {
    nsRefPtr<IDXGIFactory2> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.StartAssignment()));

    nsIntRect rect;
    mWidget->GetClientBounds(rect);

    DXGI_SWAP_CHAIN_DESC1 swapDesc = { 0 };
    // Automatically detect the width and the height from the winrt CoreWindow
    swapDesc.Width = rect.width;
    swapDesc.Height = rect.height;
    // This is the most common swapchain format
    swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.Stereo = false; 
    // Don't use multi-sampling
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SampleDesc.Quality = 0;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Use double buffering to enable flip
    swapDesc.BufferCount = 2;
    swapDesc.Scaling = DXGI_SCALING_STRETCH;
    // All Metro style apps must use this SwapEffect
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapDesc.Flags = 0;

    /**
     * Create a swap chain, this swap chain will contain the backbuffer for
     * the window we draw to. The front buffer is the full screen front
     * buffer.
    */
    nsRefPtr<IDXGISwapChain1> swapChain1;
    hr = dxgiFactory->CreateSwapChainForCoreWindow(
           dxgiDevice, (IUnknown *)mWidget->GetNativeData(NS_NATIVE_ICOREWINDOW),
           &swapDesc, nullptr, getter_AddRefs(swapChain1));
    if (FAILED(hr)) {
        return false;
    }
    mSwapChain = swapChain1;
  } else
#endif
  {
    nsRefPtr<IDXGIFactory> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.StartAssignment()));

    DXGI_SWAP_CHAIN_DESC swapDesc;
    ::ZeroMemory(&swapDesc, sizeof(swapDesc));
    swapDesc.BufferDesc.Width = 0;
    swapDesc.BufferDesc.Height = 0;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.SampleDesc.Quality = 0;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 1;
    swapDesc.OutputWindow = (HWND)mWidget->GetNativeData(NS_NATIVE_WINDOW);
    swapDesc.Windowed = TRUE;
    // We don't really need this flag, however it seems on some NVidia hardware
    // smaller area windows do not present properly without this flag. This flag
    // should have no negative consequences by itself. See bug 613790. This flag
    // is broken on optimus devices. As a temporary solution we don't set it
    // there, the only way of reliably detecting we're on optimus is looking for
    // the DLL. See Bug 623807.
    if (gfxWindowsPlatform::IsOptimus()) {
      swapDesc.Flags = 0;
    } else {
      swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    }

    /**
     * Create a swap chain, this swap chain will contain the backbuffer for
     * the window we draw to. The front buffer is the full screen front
     * buffer.
    */
    hr = dxgiFactory->CreateSwapChain(dxgiDevice, &swapDesc, byRef(mSwapChain));
    if (FAILED(hr)) {
     return false;
    }

    // We need this because we don't want DXGI to respond to Alt+Enter.
    dxgiFactory->MakeWindowAssociation(swapDesc.OutputWindow, DXGI_MWA_NO_WINDOW_CHANGES);
  }

  return true;
}

TextureFactoryIdentifier
CompositorD3D11::GetTextureFactoryIdentifier()
{
  TextureFactoryIdentifier ident;
  ident.mMaxTextureSize = GetMaxTextureSize();
  ident.mParentBackend = LAYERS_D3D11;
  return ident;
}

bool
CompositorD3D11::CanUseCanvasLayerForSize(const gfxIntSize &aSize)
{
  int32_t maxTextureSize = GetMaxTextureSize();

  if (aSize.width > maxTextureSize || aSize.height > maxTextureSize) {
    return false;
  }

  return true;
}

int32_t
CompositorD3D11::GetMaxTextureSize() const
{
  int32_t maxTextureSize;
  switch (mFeatureLevel) {
  case D3D_FEATURE_LEVEL_11_1:
  case D3D_FEATURE_LEVEL_11_0:
    maxTextureSize = 16384;
    break;
  case D3D_FEATURE_LEVEL_10_1:
  case D3D_FEATURE_LEVEL_10_0:
    maxTextureSize = 8192;
    break;
  case D3D_FEATURE_LEVEL_9_3:
    maxTextureSize = 4096;
    break;
  default:
    maxTextureSize = 2048;
  }
  return maxTextureSize;
}

TemporaryRef<TextureHost>
CompositorD3D11::CreateTextureHost(TextureHostType aTextureType,
                                   uint32_t aTextureFlags,
                                   SurfaceDescriptorType aDescriptorType,
                                   ISurfaceDeallocator* aDeAllocator)
{
  BufferMode bufferMode = aTextureType & TEXTURE_BUFFERED ? BUFFER_BUFFERED
                                                          : BUFFER_NONE;
  RefPtr<TextureHost> result;
  if (aDescriptorType == SurfaceDescriptor::TYCbCrImage) {
    result = new TextureHostYCbCrD3D11(bufferMode, aDeAllocator, mDevice);
  } else {
    result = new TextureHostD3D11(bufferMode, aDeAllocator, mDevice);
  }

  result->SetFlags(aTextureFlags);

  return result.forget();
}

TemporaryRef<CompositingRenderTarget>
CompositorD3D11::CreateRenderTarget(const gfx::IntRect &aRect,
                                    SurfaceInitMode aInit)
{
  CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, aRect.width, aRect.height, 1, 1,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

  RefPtr<ID3D11Texture2D> texture;
  mDevice->CreateTexture2D(&desc, NULL, byRef(texture));

  RefPtr<CompositingRenderTargetD3D11> rt = new CompositingRenderTargetD3D11(texture);

  if (aInit == INIT_MODE_CLEAR) {
    FLOAT clear[] = { 0, 0, 0, 0 };
    mContext->ClearRenderTargetView(rt->mRTView, clear);
  }

  return rt;
}

TemporaryRef<CompositingRenderTarget>
CompositorD3D11::CreateRenderTargetFromSource(const gfx::IntRect &aRect,
                                              const CompositingRenderTarget *aSource)
{
  CD3D11_TEXTURE2D_DESC desc(DXGI_FORMAT_B8G8R8A8_UNORM, aRect.width, aRect.height, 1, 1,
                             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

  RefPtr<ID3D11Texture2D> texture;
  mDevice->CreateTexture2D(&desc, NULL, byRef(texture));

  RefPtr<CompositingRenderTargetD3D11> rt = new CompositingRenderTargetD3D11(texture);

  return rt;
}

void
CompositorD3D11::DrawQuad(const gfx::Rect &aRect, const gfx::Rect *aClipRect,
                          const EffectChain &aEffectChain,
                          gfx::Float aOpacity, const gfx::Matrix4x4 &aTransform,
                          const gfx::Point &aOffset)
{
  memcpy(&mVSConstants.layerTransform, &aTransform._11, 64);
  mVSConstants.renderTargetOffset[0] = aOffset.x;
  mVSConstants.renderTargetOffset[1] = aOffset.y;
  mVSConstants.layerQuad = aRect;

  mPSConstants.layerOpacity[0] = aOpacity;

  D3D11_RECT scissor;
  if (aClipRect) {
    scissor.left = aClipRect->x;
    scissor.right = aClipRect->XMost();
    scissor.top = aClipRect->y;
    scissor.bottom = aClipRect->YMost();
  } else {
    scissor.left = scissor.top = INT32_MIN;
    scissor.right = scissor.bottom = INT32_MAX;
  }
  mContext->RSSetScissorRects(1, &scissor);
  mContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

  if (aEffectChain.mPrimaryEffect->mType == EFFECT_SOLID_COLOR) {
    Color color =
      static_cast<EffectSolidColor*>(aEffectChain.mPrimaryEffect.get())->mColor;
    mPSConstants.layerColor[0] = color.r * aOpacity;
    mPSConstants.layerColor[1] = color.g * aOpacity;
    mPSConstants.layerColor[2] = color.b * aOpacity;
    mPSConstants.layerColor[3] = color.a * aOpacity;

    mContext->VSSetShader(mAttachments->mVSQuadShader, nullptr, 0);
    mContext->PSSetShader(mAttachments->mSolidColorShader, nullptr, 0);
  } else if (aEffectChain.mPrimaryEffect->mType == EFFECT_BGRX) {
    EffectBGRX *rgbEffect = static_cast<EffectBGRX*>(aEffectChain.mPrimaryEffect.get());

    mVSConstants.textureCoords = rgbEffect->mTextureCoords;

    TextureSourceD3D11 *source = rgbEffect->mBGRXTexture->AsSourceD3D11();

    RefPtr<ID3D11ShaderResourceView> view;
    mDevice->CreateShaderResourceView(source->GetD3D11Texture(), nullptr, byRef(view));

    ID3D11ShaderResourceView *srView = view;
    mContext->PSSetShaderResources(0, 1, &srView);

    SetSamplerForFilter(rgbEffect->mFilter);

    mContext->VSSetShader(mAttachments->mVSQuadShader, nullptr, 0);
    mContext->PSSetShader(mAttachments->mRGBShader, nullptr, 0);
  } else if (aEffectChain.mPrimaryEffect->mType == EFFECT_BGRA) {
    EffectBGRA *rgbEffect = static_cast<EffectBGRA*>(aEffectChain.mPrimaryEffect.get());

    mVSConstants.textureCoords = rgbEffect->mTextureCoords;

    TextureSourceD3D11 *source = rgbEffect->mBGRATexture->AsSourceD3D11();

    RefPtr<ID3D11ShaderResourceView> view;
    mDevice->CreateShaderResourceView(source->GetD3D11Texture(), nullptr, byRef(view));

    ID3D11ShaderResourceView *srView = view;
    mContext->PSSetShaderResources(0, 1, &srView);

    SetSamplerForFilter(rgbEffect->mFilter);

    mContext->VSSetShader(mAttachments->mVSQuadShader, nullptr, 0);
    mContext->PSSetShader(mAttachments->mRGBAShader, nullptr, 0);
  } else if (aEffectChain.mPrimaryEffect->mType == EFFECT_YCBCR) {
    EffectYCbCr *ycbcrEffect = static_cast<EffectYCbCr*>(aEffectChain.mPrimaryEffectget());

    SetSamplerForFilter(FILTER_LINEAR);

    mVSConstants.textureCoords = ycbcrEffect->mTextureCoords;

    TextureSourceD3D11 *source = ycbcrEffect->mYCbCrTexture->AsSourceD3D11();
    TextureSourceD3D11::YCbCrTextures textures = source->GetYCbCrTextures();

    RefPtr<ID3D11ShaderResourceView> views[3];
    mDevice->CreateShaderResourceView(textures.mY, nullptr, byRef(views[0]));
    mDevice->CreateShaderResourceView(textures.mCb, nullptr, byRef(views[1]));
    mDevice->CreateShaderResourceView(textures.mCr, nullptr, byRef(views[2]));

    ID3D11ShaderResourceView *srViews[3] = { views[0], views[1], views[2] };
    mContext->PSSetShaderResources(0, 3, srViews);

    mContext->VSSetShader(mAttachments->mVSQuadShader, nullptr, 0);
    mContext->PSSetShader(mAttachments->mYCbCrShader, nullptr, 0);
  } else {
    return;
  }

  UpdateConstantBuffers();

  mContext->Draw(4, 0);
}

void
CompositorD3D11::BeginFrame(const gfx::Rect *aClipRectIn, const gfxMatrix& aTransform,
                            const gfx::Rect& aRenderBounds, gfx::Rect *aClipRectOut)
{
  VerifyBufferSize();
  UpdateRenderTarget();

  nsIntRect rect;
  mWidget->GetClientBounds(rect);
  PrepareViewport(rect.width, rect.height, gfxMatrix());

  mContext->IASetInputLayout(mAttachments->mInputLayout);

  ID3D11Buffer *buffer = mAttachments->mVertexBuffer;
  UINT size = sizeof(Vertex);
  UINT offset = 0;
  mContext->IASetVertexBuffers(0, 1, &buffer, &size, &offset);
  ID3D11RenderTargetView *view = mDefaultRT;
  mContext->OMSetRenderTargets(1, &view, nullptr);

  FLOAT green[] = { 0, 1.0f, 0, 1.0f };
  mContext->ClearRenderTargetView(mDefaultRT, green);

  if (aClipRectOut) {
    nsIntRect rect;
    mWidget->GetClientBounds(rect);
    *aClipRectOut = Rect(0, 0, rect.width, rect.height);
  }
}

void
CompositorD3D11::EndFrame(const gfxMatrix &aTransform)
{
  mContext->Flush();
  mSwapChain->Present(0, 0);
}

void
CompositorD3D11::PrepareViewport(int aWidth, int aHeight, const gfxMatrix &aWorldTransform)
{
  D3D11_VIEWPORT viewport;
  viewport.MaxDepth = 1.0f;
  viewport.MinDepth = 0;
  viewport.Width = aWidth;
  viewport.Height = aHeight;
  viewport.TopLeftX = 0;
  viewport.TopLeftY = 0;

  mContext->RSSetViewports(1, &viewport);

  gfxMatrix viewMatrix;
  viewMatrix.Translate(-gfxPoint(1.0, -1.0));
  viewMatrix.Scale(2.0f / float(aWidth), 2.0f / float(aHeight));
  viewMatrix.Scale(1.0f, -1.0f);

  viewMatrix = aWorldTransform * viewMatrix;

  gfx3DMatrix projection = gfx3DMatrix::From2D(viewMatrix);
  projection._33 = 0.0f;

  memcpy(&mVSConstants.projection, &projection, 64);
}

void
CompositorD3D11::SaveViewport()
{
  D3D11_VIEWPORT viewport;
  UINT viewports = 1;
  mContext->RSGetViewports(&viewports, &viewport);

  mViewportStack.push(IntRect(viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height));
}

IntRect
CompositorD3D11::RestoreViewport()
{
  IntRect oldViewport = mViewportStack.top();
  mViewportStack.pop();

  D3D11_VIEWPORT viewport;
  viewport.MaxDepth = 1.0f;
  viewport.MinDepth = 0;
  viewport.Width = oldViewport.width;
  viewport.Height = oldViewport.height;
  viewport.TopLeftX = oldViewport.x;
  viewport.TopLeftY = oldViewport.y;

  mContext->RSSetViewports(1, &viewport);

  return oldViewport;
}

nsIntSize*
CompositorD3D11::GetWidgetSize()
{
  nsIntRect rect;
  mWidget->GetClientBounds(rect);

  mSize = rect.Size();

  return &mSize;
}

void
CompositorD3D11::VerifyBufferSize()
{
  nsIntRect rect;
  mWidget->GetClientBounds(rect);

  DXGI_SWAP_CHAIN_DESC swapDesc;
  mSwapChain->GetDesc(&swapDesc);

  if (swapDesc.BufferDesc.Width == rect.width &&
      swapDesc.BufferDesc.Height == rect.height) {
    return;
  }

  mDefaultRT = nullptr;
  if (gfxWindowsPlatform::IsOptimus()) { 
    mSwapChain->ResizeBuffers(1, rect.width, rect.height,
                              DXGI_FORMAT_B8G8R8A8_UNORM,
                              0);
  } else if (gfxWindowsPlatform::IsRunningInWindows8Metro()) {
    mSwapChain->ResizeBuffers(2, rect.width, rect.height,
                              DXGI_FORMAT_B8G8R8A8_UNORM,
                              0);
  } else {
    mSwapChain->ResizeBuffers(1, rect.width, rect.height,
                              DXGI_FORMAT_B8G8R8A8_UNORM,
                              DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
  }
}

void
CompositorD3D11::UpdateRenderTarget()
{
  if (mDefaultRT) {
    return;
  }

  HRESULT hr;

  nsRefPtr<ID3D11Texture2D> backBuf;
  
  hr = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)backBuf.StartAssignment());
  if (FAILED(hr)) {
    return;
  }

  mDevice->CreateRenderTargetView(backBuf, NULL, byRef(mDefaultRT));
}

bool
CompositorD3D11::CreateShaders()
{
  HRESULT hr;


  hr = mDevice->CreateVertexShader(LayerQuadVS, sizeof(LayerQuadVS), nullptr, byRef(mAttachments->mVSQuadShader));

  if (FAILED(hr)) {
    return false;
  }

#define LOAD_PIXEL_SHADER(x) hr = mDevice->CreatePixelShader(x, sizeof(x), nullptr, byRef(mAttachments->m##x)); \
  if (FAILED(hr)) { \
    return false; \
  }

  LOAD_PIXEL_SHADER(SolidColorShader);
  LOAD_PIXEL_SHADER(RGBShader);
  LOAD_PIXEL_SHADER(RGBAShader);
  LOAD_PIXEL_SHADER(YCbCrShader);

#undef LOAD_PIXEL_SHADER
}

void
CompositorD3D11::UpdateConstantBuffers()
{
  D3D11_MAPPED_SUBRESOURCE resource;
  mContext->Map(mAttachments->mVSConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource);
  *(VertexShaderConstants*)resource.pData = mVSConstants;
  mContext->Unmap(mAttachments->mVSConstantBuffer, 0);
  mContext->Map(mAttachments->mPSConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &resource);
  *(PixelShaderConstants*)resource.pData = mPSConstants;
  mContext->Unmap(mAttachments->mPSConstantBuffer, 0);

  ID3D11Buffer *buffer = mAttachments->mVSConstantBuffer;

  mContext->VSSetConstantBuffers(0, 1, &buffer);

  buffer = mAttachments->mPSConstantBuffer;
  mContext->PSSetConstantBuffers(0, 1, &buffer);
}

void
CompositorD3D11::SetSamplerForFilter(Filter aFilter)
{
  ID3D11SamplerState *sampler;
  switch (aFilter) {
  case FILTER_LINEAR:
    sampler = mAttachments->mLinearSamplerState;
    break;
  case FILTER_POINT:
    sampler = mAttachments->mPointSamplerState;
    break;
  default:
    sampler = mAttachments->mLinearSamplerState;
  }

  mContext->PSSetSamplers(0, 1, &sampler);
}

}
}
