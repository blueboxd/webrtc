/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/desktop_capture/win/wgc_capture_session.h"

#include <DispatcherQueue.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directX.direct3d11.interop.h>
#include <windows.graphics.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "modules/desktop_capture/win/wgc_desktop_frame.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "rtc_base/win/create_direct3d_device.h"
#include "rtc_base/win/get_activation_factory.h"
#include "system_wrappers/include/metrics.h"
#include "system_wrappers/include/sleep.h"

using Microsoft::WRL::ComPtr;
namespace WGC = ABI::Windows::Graphics::Capture;

namespace webrtc {
namespace {

// We must use a BGRA pixel format that has 4 bytes per pixel, as required by
// the DesktopFrame interface.
constexpr auto kPixelFormat = ABI::Windows::Graphics::DirectX::
    DirectXPixelFormat::DirectXPixelFormat_B8G8R8A8UIntNormalized;

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class StartCaptureResult {
  kSuccess = 0,
  kSourceClosed = 1,
  kAddClosedFailed = 2,
  kDxgiDeviceCastFailed = 3,
  kD3dDelayLoadFailed = 4,
  kD3dDeviceCreationFailed = 5,
  kFramePoolActivationFailed = 6,
  // kFramePoolCastFailed = 7, (deprecated)
  // kGetItemSizeFailed = 8, (deprecated)
  kCreateFramePoolFailed = 9,
  kCreateCaptureSessionFailed = 10,
  kStartCaptureFailed = 11,
  kMaxValue = kStartCaptureFailed
};

// These values are persisted to logs. Entries should not be renumbered and
// numeric values should never be reused.
enum class GetFrameResult {
  kSuccess = 0,
  kItemClosed = 1,
  kTryGetNextFrameFailed = 2,
  kFrameDropped = 3,
  kGetSurfaceFailed = 4,
  kDxgiInterfaceAccessFailed = 5,
  kTexture2dCastFailed = 6,
  kCreateMappedTextureFailed = 7,
  kMapFrameFailed = 8,
  kGetContentSizeFailed = 9,
  kResizeMappedTextureFailed = 10,
  kRecreateFramePoolFailed = 11,
  kMaxValue = kRecreateFramePoolFailed
};

void RecordStartCaptureResult(StartCaptureResult error) {
  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.DesktopCapture.Win.WgcCaptureSessionStartResult",
      static_cast<int>(error), static_cast<int>(StartCaptureResult::kMaxValue));
}

void RecordGetFrameResult(GetFrameResult error) {
  RTC_HISTOGRAM_ENUMERATION(
      "WebRTC.DesktopCapture.Win.WgcCaptureSessionGetFrameResult",
      static_cast<int>(error), static_cast<int>(GetFrameResult::kMaxValue));
}

}  // namespace

WgcCaptureSession::WgcCaptureSession(ComPtr<ID3D11Device> d3d11_device,
                                     ComPtr<WGC::IGraphicsCaptureItem> item,
                                     ABI::Windows::Graphics::SizeInt32 size)
    : d3d11_device_(std::move(d3d11_device)),
      item_(std::move(item)),
      size_(size) {}

WgcCaptureSession::~WgcCaptureSession() {
  RemoveEventHandlers();
}

HRESULT WgcCaptureSession::StartCapture(const DesktopCaptureOptions& options) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  RTC_DCHECK(!is_capture_started_);

  if (item_closed_) {
    RTC_LOG(LS_ERROR) << "The target source has been closed.";
    RecordStartCaptureResult(StartCaptureResult::kSourceClosed);
    return E_ABORT;
  }

  RTC_DCHECK(d3d11_device_);
  RTC_DCHECK(item_);

  // Listen for the Closed event, to detect if the source we are capturing is
  // closed (e.g. application window is closed or monitor is disconnected). If
  // it is, we should abort the capture.
  item_closed_token_ = std::make_unique<EventRegistrationToken>();
  auto closed_handler =
      Microsoft::WRL::Callback<ABI::Windows::Foundation::ITypedEventHandler<
          WGC::GraphicsCaptureItem*, IInspectable*>>(
          this, &WgcCaptureSession::OnItemClosed);
  HRESULT hr =
      item_->add_Closed(closed_handler.Get(), item_closed_token_.get());
  if (FAILED(hr)) {
    RecordStartCaptureResult(StartCaptureResult::kAddClosedFailed);
    return hr;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  hr = d3d11_device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
  if (FAILED(hr)) {
    RecordStartCaptureResult(StartCaptureResult::kDxgiDeviceCastFailed);
    return hr;
  }

  if (!ResolveCoreWinRTDirect3DDelayload()) {
    RecordStartCaptureResult(StartCaptureResult::kD3dDelayLoadFailed);
    return E_FAIL;
  }

  hr = CreateDirect3DDeviceFromDXGIDevice(dxgi_device.Get(), &direct3d_device_);
  if (FAILED(hr)) {
    RecordStartCaptureResult(StartCaptureResult::kD3dDeviceCreationFailed);
    return hr;
  }

  ComPtr<WGC::IDirect3D11CaptureFramePoolStatics> frame_pool_statics;
  hr = GetActivationFactory<
      WGC::IDirect3D11CaptureFramePoolStatics,
      RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool>(
      &frame_pool_statics);
  if (FAILED(hr)) {
    RecordStartCaptureResult(StartCaptureResult::kFramePoolActivationFailed);
    return hr;
  }

  hr = frame_pool_statics->Create(direct3d_device_.Get(), kPixelFormat,
                                  kNumBuffers, size_, &frame_pool_);
  if (FAILED(hr)) {
    RecordStartCaptureResult(StartCaptureResult::kCreateFramePoolFailed);
    return hr;
  }

  // Because `WgcCapturerWin` created a `DispatcherQueue`, and we created
  // `frame_pool_` via `Create`, the `FrameArrived` event will be delivered on
  // the current thread.
  frame_arrived_token_ = std::make_unique<EventRegistrationToken>();
  auto frame_arrived_handler =
      Microsoft::WRL::Callback<ABI::Windows::Foundation::ITypedEventHandler<
          WGC::Direct3D11CaptureFramePool*, IInspectable*>>(
          this, &WgcCaptureSession::OnFrameArrived);
  hr = frame_pool_->add_FrameArrived(frame_arrived_handler.Get(),
                                     frame_arrived_token_.get());

  hr = frame_pool_->CreateCaptureSession(item_.Get(), &session_);
  if (FAILED(hr)) {
    RecordStartCaptureResult(StartCaptureResult::kCreateCaptureSessionFailed);
    return hr;
  }

  if (!options.prefer_cursor_embedded()) {
    ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(session_->QueryInterface(
            ABI::Windows::Graphics::Capture::IID_IGraphicsCaptureSession2,
            &session2))) {
      session2->put_IsCursorCaptureEnabled(false);
    }
  }

  hr = session_->StartCapture();
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "Failed to start CaptureSession: " << hr;
    RecordStartCaptureResult(StartCaptureResult::kStartCaptureFailed);
    return hr;
  }

  RecordStartCaptureResult(StartCaptureResult::kSuccess);

  is_capture_started_ = true;
  return hr;
}

bool WgcCaptureSession::GetFrame(std::unique_ptr<DesktopFrame>* output_frame) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  // When GetFrame() asks for the first frame it can happen that no frame has
  // arrived yet. We therefore try to get a new frame from the frame pool for a
  // maximum of 10 times after sleeping for 20ms. We choose 20ms as it's just a
  // bit longer than 17ms (for 60fps*) and hopefully avoids unlucky timing
  // causing us to wait two frames when we mostly seem to only need to wait for
  // one. This approach should ensure that GetFrame() always delivers a valid
  // frame with a max latency of 200ms and often after sleeping only once.
  // We also build up an `empty_frame_credit_count_` for each sleep call. As
  // long as this credit is above zero, error logs for "empty frame" are
  // avoided. The counter is reduced by one for each successful call to
  // ProcessFrame() until the number of credits is zero. This counter is only
  // expected to be above zero during a short startup phase. The scheme is
  // heuristic and based on manual testing.
  // (*) On a modern system, the FPS / monitor refresh rate is usually larger
  //     than or equal to 60.
  const int max_sleep_count = 10;
  const int sleep_time_ms = 20;

  int sleep_count = 0;
  while (!queue_.current_frame() && sleep_count < max_sleep_count) {
    sleep_count++;
    empty_frame_credit_count_ = sleep_count + 1;
    webrtc::SleepMs(sleep_time_ms);
    ProcessFrame();
  }

  // Return false if we still don't have a valid frame leading to a
  // DesktopCapturer::Result::ERROR_PERMANENT posted by the WGC capturer.
  if (!queue_.current_frame()) {
    RTC_LOG(LS_ERROR) << "GetFrame failed.";
    return false;
  }

  // Emit the current frame.
  std::unique_ptr<DesktopFrame> new_frame = queue_.current_frame()->Share();
  *output_frame = std::move(new_frame);

  return true;
}

HRESULT WgcCaptureSession::CreateMappedTexture(
    ComPtr<ID3D11Texture2D> src_texture,
    UINT width,
    UINT height) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  D3D11_TEXTURE2D_DESC src_desc;
  src_texture->GetDesc(&src_desc);
  D3D11_TEXTURE2D_DESC map_desc;
  map_desc.Width = width == 0 ? src_desc.Width : width;
  map_desc.Height = height == 0 ? src_desc.Height : height;
  map_desc.MipLevels = src_desc.MipLevels;
  map_desc.ArraySize = src_desc.ArraySize;
  map_desc.Format = src_desc.Format;
  map_desc.SampleDesc = src_desc.SampleDesc;
  map_desc.Usage = D3D11_USAGE_STAGING;
  map_desc.BindFlags = 0;
  map_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  map_desc.MiscFlags = 0;
  return d3d11_device_->CreateTexture2D(&map_desc, nullptr, &mapped_texture_);
}

HRESULT WgcCaptureSession::OnFrameArrived(
    WGC::IDirect3D11CaptureFramePool* sender,
    IInspectable* event_args) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);
  HRESULT hr = ProcessFrame();
  if (FAILED(hr)) {
    RTC_DLOG(LS_WARNING) << "ProcessFrame failed: " << hr;
  }
  return hr;
}

HRESULT WgcCaptureSession::ProcessFrame() {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  if (item_closed_) {
    RTC_LOG(LS_ERROR) << "The target source has been closed.";
    RecordGetFrameResult(GetFrameResult::kItemClosed);
    return E_ABORT;
  }

  RTC_DCHECK(is_capture_started_);

  queue_.MoveToNextFrame();
  if (queue_.current_frame() && queue_.current_frame()->IsShared()) {
    RTC_DLOG(LS_WARNING) << "Overwriting frame that is still shared.";
  }

  ComPtr<WGC::IDirect3D11CaptureFrame> capture_frame;
  HRESULT hr = frame_pool_->TryGetNextFrame(&capture_frame);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "TryGetNextFrame failed: " << hr;
    RecordGetFrameResult(GetFrameResult::kTryGetNextFrameFailed);
    return hr;
  }

  if (!capture_frame) {
    // Avoid logging errors while we still have credits (or allowance) to
    // consider this condition as expected and not as an error.
    if (empty_frame_credit_count_ == 0) {
      RTC_DLOG(LS_WARNING) << "Frame pool was empty => kFrameDropped.";
      RecordGetFrameResult(GetFrameResult::kFrameDropped);
    }
    return E_FAIL;
  }

  // We need to get `capture_frame` as an `ID3D11Texture2D` so that we can get
  // the raw image data in the format required by the `DesktopFrame` interface.
  ComPtr<ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>
      d3d_surface;
  hr = capture_frame->get_Surface(&d3d_surface);
  if (FAILED(hr)) {
    RecordGetFrameResult(GetFrameResult::kGetSurfaceFailed);
    return hr;
  }

  ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>
      direct3DDxgiInterfaceAccess;
  hr = d3d_surface->QueryInterface(IID_PPV_ARGS(&direct3DDxgiInterfaceAccess));
  if (FAILED(hr)) {
    RecordGetFrameResult(GetFrameResult::kDxgiInterfaceAccessFailed);
    return hr;
  }

  ComPtr<ID3D11Texture2D> texture_2D;
  hr = direct3DDxgiInterfaceAccess->GetInterface(IID_PPV_ARGS(&texture_2D));
  if (FAILED(hr)) {
    RecordGetFrameResult(GetFrameResult::kTexture2dCastFailed);
    return hr;
  }

  if (!mapped_texture_) {
    hr = CreateMappedTexture(texture_2D);
    if (FAILED(hr)) {
      RecordGetFrameResult(GetFrameResult::kCreateMappedTextureFailed);
      return hr;
    }
  }

  // We need to copy `texture_2D` into `mapped_texture_` as the latter has the
  // D3D11_CPU_ACCESS_READ flag set, which lets us access the image data.
  // Otherwise it would only be readable by the GPU.
  ComPtr<ID3D11DeviceContext> d3d_context;
  d3d11_device_->GetImmediateContext(&d3d_context);

  ABI::Windows::Graphics::SizeInt32 new_size;
  hr = capture_frame->get_ContentSize(&new_size);
  if (FAILED(hr)) {
    RecordGetFrameResult(GetFrameResult::kGetContentSizeFailed);
    return hr;
  }

  // If the size changed, we must resize `mapped_texture_` and `frame_pool_` to
  // fit the new size. This must be done before `CopySubresourceRegion` so that
  // the textures are the same size.
  if (size_.Height != new_size.Height || size_.Width != new_size.Width) {
    hr = CreateMappedTexture(texture_2D, new_size.Width, new_size.Height);
    if (FAILED(hr)) {
      RecordGetFrameResult(GetFrameResult::kResizeMappedTextureFailed);
      return hr;
    }

    hr = frame_pool_->Recreate(direct3d_device_.Get(), kPixelFormat,
                               kNumBuffers, new_size);
    if (FAILED(hr)) {
      RecordGetFrameResult(GetFrameResult::kRecreateFramePoolFailed);
      return hr;
    }
  }

  // If the size has changed since the last capture, we must be sure to use
  // the smaller dimensions. Otherwise we might overrun our buffer, or
  // read stale data from the last frame.
  int image_height = std::min(size_.Height, new_size.Height);
  int image_width = std::min(size_.Width, new_size.Width);

  D3D11_BOX copy_region;
  copy_region.left = 0;
  copy_region.top = 0;
  copy_region.right = image_width;
  copy_region.bottom = image_height;
  // Our textures are 2D so we just want one "slice" of the box.
  copy_region.front = 0;
  copy_region.back = 1;
  d3d_context->CopySubresourceRegion(mapped_texture_.Get(),
                                     /*dst_subresource_index=*/0, /*dst_x=*/0,
                                     /*dst_y=*/0, /*dst_z=*/0, texture_2D.Get(),
                                     /*src_subresource_index=*/0, &copy_region);

  D3D11_MAPPED_SUBRESOURCE map_info;
  hr = d3d_context->Map(mapped_texture_.Get(), /*subresource_index=*/0,
                        D3D11_MAP_READ, /*D3D11_MAP_FLAG_DO_NOT_WAIT=*/0,
                        &map_info);
  if (FAILED(hr)) {
    RecordGetFrameResult(GetFrameResult::kMapFrameFailed);
    return hr;
  }

  // Allocate the current frame buffer only if it is not already allocated or
  // if the size has changed. Note that we can't reallocate other buffers at
  // this point, since the caller may still be reading from them. The queue can
  // hold up tp two frames.
  DesktopSize image_size(image_width, image_height);
  if (!queue_.current_frame() ||
      !queue_.current_frame()->size().equals(image_size)) {
    std::unique_ptr<DesktopFrame> buffer =
        std::make_unique<BasicDesktopFrame>(image_size);
    queue_.ReplaceCurrentFrame(SharedDesktopFrame::Wrap(std::move(buffer)));
  }

  DesktopFrame* current_frame = queue_.current_frame();

  // Make a copy of the data pointed to by `map_info.pData` to the preallocated
  // `current_frame` so we are free to unmap our texture.
  uint8_t* src_data = static_cast<uint8_t*>(map_info.pData);
  uint8_t* dst_data = current_frame->data();
  for (int i = 0; i < image_height; i++) {
    memcpy(dst_data, src_data, current_frame->stride());
    dst_data += current_frame->stride();
    src_data += map_info.RowPitch;
  }

  d3d_context->Unmap(mapped_texture_.Get(), 0);

  if (empty_frame_credit_count_ > 0)
    --empty_frame_credit_count_;
  size_ = new_size;
  RecordGetFrameResult(GetFrameResult::kSuccess);
  return hr;
}

HRESULT WgcCaptureSession::OnItemClosed(WGC::IGraphicsCaptureItem* sender,
                                        IInspectable* event_args) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  RTC_LOG(LS_INFO) << "Capture target has been closed.";
  item_closed_ = true;

  RemoveEventHandlers();

  // Do not attempt to free resources in the OnItemClosed handler, as this
  // causes a race where we try to delete the item that is calling us. Removing
  // the event handlers and setting `item_closed_` above is sufficient to ensure
  // that the resources are no longer used, and the next time the capturer tries
  // to get a frame, we will report a permanent failure and be destroyed.
  return S_OK;
}

void WgcCaptureSession::RemoveEventHandlers() {
  HRESULT hr;
  if (frame_pool_ && frame_arrived_token_) {
    hr = frame_pool_->remove_FrameArrived(*frame_arrived_token_);
    frame_arrived_token_.reset();
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "Failed to remove FrameArrived event handler: "
                          << hr;
    }
  }
  if (item_ && item_closed_token_) {
    hr = item_->remove_Closed(*item_closed_token_);
    item_closed_token_.reset();
    if (FAILED(hr))
      RTC_LOG(LS_WARNING) << "Failed to remove Closed event handler: " << hr;
  }
}

}  // namespace webrtc
